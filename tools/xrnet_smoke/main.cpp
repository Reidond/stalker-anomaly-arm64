// xrnet_smoke: headless check of the single-player message path through xrNetServer on Apple platforms.
//
//   xrnet_smoke
//
// Anomaly single player runs an xrServer (an IPureServer) and a CLevel (an IPureClient) in one process; the DirectPlay
// transport is never used. Both classes need a game-side subclass, so the tool carries minimal stand-ins that do what the
// game does at the three points where it touches the transport:
//   - xrServer::SendTo_LL   hands server -> client traffic to CLevel::OnMessage            (TestServer::SendTo_LL)
//   - CLevel::Send          hands client -> server traffic to xrServer::OnMessage           (TestClient::Send)
//   - CLevel::ClientReceive drains the IPureClient queue between Start/EndProcessQueue      (drain())
//
// What it checks:
//   1. IPureServer::Connect("…/single…") selects the in-process path, IPureClient::Connect succeeds without a network.
//   2. A few thousand packets from 15 bytes up to the largest NET_Packet go client -> server and server -> client (direct,
//      broadcast with and without the sender excluded): ordering, size and a payload checksum on every packet, and the
//      INetQueue pool is recycled while it is drained in bursts.
//   3. The same server -> client path with a producer thread and the consumer draining concurrently.
//   4. The multipacket framing the real transport carries (MultipacketSender / MultipacketReciever): packets are merged,
//      a stream larger than one buffer is split over several frames, a change of send flags forces a flush, and every
//      packet comes out the other end in order and byte-identical.
//   5. Without "/single" the Apple stub refuses to host or to connect, logs why, and nothing crashes (the Windows
//      DirectPlay path would be taken there).
//
// Exit status 0 means every check passed.
#include "xrCore/xrCore.h"
#include "xrNetServer/NET_Client.h"
#include "xrNetServer/NET_Server.h"
#include "xrNetServer/NET_Common.h"
#include "xrNetServer/NET_Messages.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace
{
int g_failures = 0;
std::vector<std::string> g_log;

void check(bool ok, const char* what)
{
	printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
	if (!ok)
		++g_failures;
}

void engine_log(LPCSTR line)
{
	g_log.emplace_back(line);
	printf("    | %s\n", line);
}

bool logged(const char* needle)
{
	for (const std::string& l : g_log)
		if (l.find(needle) != std::string::npos)
			return true;
	return false;
}

// --- deterministic payloads --------------------------------------------------------------------------------------------

struct Rng
{
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
	uint64_t next()
	{
		s ^= s >> 12;
		s ^= s << 25;
		s ^= s >> 27;
		return s * 0x2545F4914F6CDD1Dull;
	}
	uint32_t below(uint32_t n) { return uint32_t(next() % n); }
};

uint32_t fnv1a(const uint8_t* p, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; ++i)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

// type u16 | seq u32 | total length u32 | checksum of the filler u32 | filler
const u32 kHeader = 2 + 4 + 4 + 4;
const u16 kTypeData = 0x7001;
const u16 kTypeEcho = 0x7002;

// The largest packet NET_Packet takes (it asserts B.count < NET_PacketSizeLimit), and the largest one that
// MultipacketSender can merge: it flushes when count + size + 2 >= limit and then writes size + 2 into the empty
// buffer, which NET_Packet::w asserts stays below the limit (a debug build); 16382 would still fit, 16383 overflows.
const u32 kMaxPacket = NET_PacketSizeLimit - 1;
const u32 kMaxMultipacketPacket = NET_PacketSizeLimit - 3;

void build(NET_Packet& P, u16 type, u32 seq, u32 total, Rng& rng)
{
	std::vector<uint8_t> filler(total - kHeader);
	for (uint8_t& b : filler)
		b = uint8_t(rng.next() >> 24);
	P.w_begin(type);
	P.w_u32(seq);
	P.w_u32(total);
	P.w_u32(fnv1a(filler.data(), filler.size()));
	P.w(filler.data(), u32(filler.size()));
}

struct Parsed
{
	u16 type = 0;
	u32 seq = 0, total = 0;
	bool intact = false;
};

Parsed parse(const void* data, u32 size)
{
	Parsed r;
	if (size < kHeader)
		return r;
	NET_Packet P;
	P.construct(data, size);
	u32 sum = 0;
	P.r_begin(r.type);
	P.r_u32(r.seq);
	P.r_u32(r.total);
	P.r_u32(sum);
	r.intact = r.total == size && fnv1a(P.B.data + kHeader, size - kHeader) == sum;
	return r;
}

// Packet sizes: boundaries first, then random ones biased to small (like real traffic) with a heavy tail.
u32 pick_size(u32 i, Rng& rng, u32 max_total)
{
	static const u32 fixed[] = {kHeader, kHeader + 1, 31, 36, 37, 100, 255, 256, 257, 1023, 1024, 4095, 4096, 8191, 8192,
	                            16000, NET_PacketSizeLimit - 3, NET_PacketSizeLimit - 2, NET_PacketSizeLimit - 1};
	if (i < sizeof(fixed) / sizeof(fixed[0]))
		return fixed[i] > max_total ? max_total : fixed[i];
	const u32 r = rng.below(100);
	u32 n;
	if (r < 70) n = kHeader + rng.below(200);
	else if (r < 95) n = kHeader + rng.below(2000);
	else n = kHeader + rng.below(max_total - kHeader + 1);
	return n > max_total ? max_total : n;
}

// --- minimal game-side stand-ins ---------------------------------------------------------------------------------------

class TestClient;

class TestServer : public IPureServer
{
public:
	TestClient* client = nullptr;
	u32 expect_seq = 0;      // next kTypeData sequence number from the client
	u32 received = 0, bad = 0, out_of_order = 0;
	u32 echoed = 0;

	explicit TestServer(CTimer* t) : IPureServer(t) {}
	~TestServer() override
	{
		// IPureServer does not own its clients; xrServer::client_Destroy deletes them
		while (IClient* c = net_players.GetFoundClient([](IClient*) { return true; }))
		{
			net_players.FindAndEraseClient([c](IClient* x) { return x == c; });
			xr_delete(c);
		}
	}

	IClient* client_Create() override { return xr_new<IClient>(device_timer); }
	void client_Replicate() override {}
	void client_Destroy(IClient* C) override
	{
		net_players.FindAndEraseClient([C](IClient* x) { return x == C; });
		xr_delete(C);
	}

	// xrServer::client_Find_Get + AttachNewClient for a direct connection
	IClient* new_client(SClientConnectData* cl_data) override
	{
		IClient* CL = client_Create();
		CL->ID = cl_data->clientID;
		CL->server = this;
		CL->name = cl_data->name;
		CL->process_id = cl_data->process_id;
		net_players.AddNewClient(CL);

		SV_Client = CL;
		CL->flags.bLocal = 1;
		CL->flags.bConnected = TRUE;
		MSYS_CONFIG msgConfig;
		msgConfig.sign1 = 0x12071980;
		msgConfig.sign2 = 0x26111975;
		SendTo_LL(CL->ID, &msgConfig, sizeof(msgConfig), net_flags(TRUE, TRUE, TRUE, TRUE));
		return CL;
	}

	// xrServer::create_direct_client
	void create_direct_client()
	{
		SClientConnectData cl_data;
		cl_data.clientID.set(1);
		xr_strcpy(cl_data.name, "single_player");
		cl_data.process_id = GetCurrentProcessId();
		new_client(&cl_data);
	}

	u32 OnMessage(NET_Packet& P, ClientID sender) override;
	void SendTo_LL(ClientID ID, void* data, u32 size, u32 dwFlags = DPNSEND_GUARANTEED, u32 dwTimeout = 0) override;
};

class TestClient : public IPureClient
{
public:
	TestServer* server = nullptr;
	u32 invalid_host_calls = 0;

	explicit TestClient(CTimer* t) : IPureClient(t) {}

	// CLevel::Send
	void Send(NET_Packet& P, u32 dwFlags = DPNSEND_GUARANTEED, u32 dwTimeout = 0) override
	{
		if (psNET_direct_connect)
		{
			ClientID clid;
			clid.set(1);
			server->OnMessage(P, clid);
		}
		else
			IPureClient::Send(P, dwFlags, dwTimeout);
	}

	void OnInvalidHost() override { ++invalid_host_calls; }
};

u32 TestServer::OnMessage(NET_Packet& P, ClientID sender)
{
	const Parsed p = parse(P.B.data, P.B.count);
	if (!p.intact)
		++bad;
	if (p.type == kTypeData)
	{
		++received;
		if (p.seq != expect_seq)
			++out_of_order;
		expect_seq = p.seq + 1;
	}
	else if (p.type == kTypeEcho)
	{
		++echoed;
		SendTo(sender, P); // answer on the same connection, like xrServer does for most requests
	}
	return 0;
}

// xrServer::SendTo_LL: "optimize local traffic" -- the in-process client gets the bytes directly
void TestServer::SendTo_LL(ClientID ID, void* data, u32 size, u32 dwFlags, u32 dwTimeout)
{
	if ((SV_Client && SV_Client->ID == ID) || psNET_direct_connect)
		client->OnMessage(data, size);
	else
		IPureServer::SendTo_LL(ID, data, size, dwFlags, dwTimeout);
}

// CLevel::ClientReceive's queue loop. Returns packets drained; checks ordering / integrity of kTypeData by sequence.
struct Drain
{
	u32 data = 0, system = 0, bad = 0, out_of_order = 0, other = 0;
	u32 next_seq = 0;
	u32 total_bytes = 0;
};

u32 drain(TestClient& c, Drain& d, u32 max_packets = ~0u)
{
	u32 n = 0;
	c.StartProcessQueue();
	for (NET_Packet* P = c.net_msg_Retreive(); P && n < max_packets; P = c.net_msg_Retreive())
	{
		++n;
		d.total_bytes += P->B.count;
		if (P->B.count == sizeof(MSYS_CONFIG))
		{
			const MSYS_CONFIG* m = (const MSYS_CONFIG*)P->B.data;
			if (m->sign1 == 0x12071980 && m->sign2 == 0x26111975)
				++d.system;
			else
				++d.other;
		}
		else
		{
			const Parsed p = parse(P->B.data, P->B.count);
			if (!p.intact)
				++d.bad;
			if (p.type == kTypeData)
			{
				++d.data;
				if (p.seq != d.next_seq)
					++d.out_of_order;
				d.next_seq = p.seq + 1;
			}
			else
				++d.other;
		}
		c.net_msg_Release();
	}
	c.EndProcessQueue();
	return n;
}

// --- multipacket framing -------------------------------------------------------------------------------------------------

struct WireOut : public MultipacketSender
{
	std::vector<std::vector<uint8_t>> frames;
	std::vector<u32> flags;

protected:
	void _SendTo_LL(const void* data, u32 size, u32 f, u32) override
	{
		frames.emplace_back((const uint8_t*)data, (const uint8_t*)data + size);
		flags.push_back(f);
	}
};

struct WireIn : public MultipacketReciever
{
	std::vector<std::vector<uint8_t>> packets;
	u32 last_param = 0;

protected:
	void _Recieve(const void* data, u32 size, u32 param) override
	{
		packets.emplace_back((const uint8_t*)data, (const uint8_t*)data + size);
		last_param = param;
	}
};

// xrCore insists on a file system for its log. This one is empty: no archives, only $app_data_root$ and $logs$ under
// the run directory, so the tool needs no game data.
bool write_fsgame(const std::string& run_dir, const std::string& out_path)
{
	std::ofstream out(out_path, std::ios::trunc);
	out << "; generated by xrnet_smoke\n";
	out << "$fs_root$ = false | false | " << run_dir << "\n";
	out << "$app_data_root$ = true | false | $fs_root$ | appdata\\\n";
	out << "$logs$ = true | false | $app_data_root$ | logs\\\n";
	return out.good();
}

void make_dirs(const std::string& path)
{
	for (size_t i = 1; i <= path.size(); ++i)
		if (i == path.size() || path[i] == '/')
			mkdir(path.substr(0, i).c_str(), 0755);
}

} // namespace

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0); // keep output ordered if an assert ends the process
	std::string run_dir = XRNET_SMOKE_RUN_DIR;
	for (int i = 1; i + 1 < argc; ++i)
		if (!strcmp(argv[i], "--run-dir"))
			run_dir = argv[++i];
	if (run_dir.empty() || run_dir.back() != '/')
		run_dir += '/';
	make_dirs(run_dir + "appdata");
	const std::string fsgame = run_dir + "fsgame.ltx";
	if (!write_fsgame(run_dir, fsgame))
	{
		printf("cannot write %s\n", fsgame.c_str());
		return 2;
	}

	printf("xrnet_smoke\n  run dir: %s\n", run_dir.c_str());
	Debug._initialize(false);
	Core._initialize("xrnet_smoke", engine_log, TRUE, fsgame.c_str());

	CTimer timer;
	timer.Start();
	GameDescriptionData game_descr;
	ZeroMemory(&game_descr, sizeof(game_descr));

	// ---------------------------------------------------------------------------------------------------------------
	printf("\n[1] single-player connect (\"/single\"), in-process\n");
	TestServer* server = xr_new<TestServer>(&timer);
	TestClient* client = xr_new<TestClient>(&timer);
	server->client = client;
	client->server = server;
	{
		check(!psNET_direct_connect, "direct connect is off before the server connects");
		const IPureServer::EConnect r = server->Connect("mp_testmap/single/alife/new", game_descr);
		check(r == IPureServer::ErrNoError, "IPureServer::Connect accepts \"/single\"");
		check(psNET_direct_connect, "psNET_direct_connect set by \"/single\"");
		check(server->GetConnectOptions() == "mp_testmap/single/alife/new", "connect options are kept");
		check(client->Connect("localhost/name=player/port=1235"), "IPureClient::Connect succeeds in-process");
		check(!client->net_isDisconnected(), "client is not disconnected");
		check(!client->net_isFails_Connect(), "client does not report a failed connection");

		server->create_direct_client();
		check(server->GetClientsCount() == 1, "one in-process client on the server");
		check(server->GetServerClient() && server->GetServerClient()->ID.value() == 1, "server client has ID 1");
		check(server->GetServerClient() && server->GetServerClient()->flags.bLocal, "server client is local");
		check(server->HasBandwidth(server->GetServerClient()), "IPureServer::HasBandwidth (direct) is always true");

		Drain d;
		drain(*client, d);
		check(d.system == 1 && d.other == 0, "the MSYS_CONFIG handshake reaches the client queue");
		check(server->GetPort() == 0, "GetPort() is defined in single player");
	}

	// ---------------------------------------------------------------------------------------------------------------
	printf("\n[2] packets in both directions, in-process\n");
	{
		Rng rng(12345);
		const u32 count = 3000;

		// client -> server (CLevel::Send), consecutive sequence numbers
		u64 bytes_up = 0;
		for (u32 i = 0; i < count; ++i)
		{
			NET_Packet P;
			const u32 size = pick_size(i, rng, kMaxPacket);
			build(P, kTypeData, i, size, rng);
			bytes_up += P.B.count;
			client->Send(P, net_flags(TRUE, TRUE));
		}
		check(server->received == count, "all client -> server packets arrived");
		check(server->bad == 0, "client -> server: size and payload checksum intact");
		check(server->out_of_order == 0, "client -> server: order preserved");
		printf("    (%u packets, %llu bytes up)\n", count, (unsigned long long)bytes_up);

		// server -> client (xrServer::SendTo -> CLevel::OnMessage), drained in random-sized bursts so the queue pool is reused
		Drain d;
		u32 sent = 0;
		while (sent < count)
		{
			const u32 burst = 1 + rng.below(40);
			for (u32 i = 0; i < burst && sent < count; ++i, ++sent)
			{
				NET_Packet P;
				build(P, kTypeData, sent, pick_size(sent, rng, kMaxPacket), rng);
				server->SendTo(ClientID(1), P, net_flags(TRUE, TRUE));
			}
			drain(*client, d);
		}
		check(d.data == count, "all server -> client packets arrived");
		check(d.bad == 0, "server -> client: size and payload checksum intact");
		check(d.out_of_order == 0, "server -> client: order preserved");
		check(d.other == 0, "no stray packets in the client queue");
		client->StartProcessQueue();
		check(client->net_msg_Retreive() == nullptr, "the client queue is empty after draining");
		client->EndProcessQueue();

		// a request answered on the same connection
		u32 echo_ok = 0;
		for (u32 i = 0; i < 200; ++i)
		{
			NET_Packet P;
			build(P, kTypeEcho, i, pick_size(i, rng, kMaxPacket), rng);
			NET_Packet copy = P;
			client->Send(P, net_flags(TRUE, TRUE));
			client->StartProcessQueue();
			NET_Packet* R = client->net_msg_Retreive();
			if (R && R->B.count == copy.B.count && !memcmp(R->B.data, copy.B.data, copy.B.count))
				++echo_ok;
			if (R)
				client->net_msg_Release();
			client->EndProcessQueue();
		}
		check(server->echoed == 200 && echo_ok == 200, "request/answer round trip: 200 echoes byte-identical");

		// broadcast: the sender is excluded or not
		{
			NET_Packet P;
			build(P, kTypeData, 0, 64, rng);
			server->SendBroadcast(ClientID(1), P, net_flags(TRUE, TRUE));
			Drain none;
			check(drain(*client, none) == 0, "SendBroadcast excludes the excluded client");
			server->SendBroadcast(ClientID(0), P, net_flags(TRUE, TRUE));
			Drain one;
			check(drain(*client, one) == 1 && one.data == 1, "SendBroadcast reaches the other clients");
		}

		// statistics and bandwidth gates used by the game loop
		client->net_HasBandwidth(); // rate gate used by CLevel::ClientSend; only checked for not touching the missing transport
		client->ClearStatistic();
		server->ClearStatistic();
		check(server->GetStatistic() != nullptr, "server statistics available");
	}

	// ---------------------------------------------------------------------------------------------------------------
	printf("\n[3] concurrent producer (server) and consumer (client)\n");
	{
		const u32 count = 20000;
		std::atomic<u32> produced{0};
		std::thread producer(
			[&]()
			{
				Rng prng(4242);
				for (u32 i = 0; i < count; ++i)
				{
					NET_Packet P;
					build(P, kTypeData, i, pick_size(i, prng, 3000), prng);
					server->SendTo(ClientID(1), P, net_flags(TRUE, TRUE));
					produced.store(i + 1);
				}
			});
		Drain d;
		const u32 start = GetTickCount();
		while (d.data < count && GetTickCount() - start < 30000)
		{
			if (!drain(*client, d, 64))
				Sleep(1);
		}
		producer.join();
		drain(*client, d);
		check(d.data == count, "all packets arrived");
		check(d.bad == 0 && d.out_of_order == 0 && d.other == 0, "order and payloads intact across threads");
	}

	// ---------------------------------------------------------------------------------------------------------------
	printf("\n[4] multipacket framing (what a transport would carry)\n");
	{
		// the compressor only runs when psNET_direct_connect is off; its own gate (g_net_compressor_enabled) is off by default
		psNET_direct_connect = FALSE;

		{
			WireOut out;
			WireIn in;
			Rng rng(99);
			std::vector<std::vector<uint8_t>> sent;
			const u32 count = 600;
			const u32 reliable = net_flags(TRUE, TRUE);
			for (u32 i = 0; i < count; ++i)
			{
				NET_Packet P;
				build(P, kTypeData, i, pick_size(i, rng, kMaxMultipacketPacket), rng);
				sent.emplace_back(P.B.data, P.B.data + P.B.count);
				out.SendPacket(P.B.data, P.B.count, reliable, 0);
			}
			out.FlushSendBuffer(0);
			check(out.frames.size() > 1, "a long stream is split over several frames");
			bool frames_ok = true;
			size_t wire_bytes = 0;
			for (const auto& f : out.frames)
			{
				wire_bytes += f.size();
				frames_ok = frames_ok && f.size() >= 3 && f[0] == NET_TAG_MERGED && f.size() <= NET_PacketSizeLimit + 64;
			}
			check(frames_ok, "every frame is tagged as merged and fits the transport");
			size_t payload = 0;
			for (const auto& s : sent) payload += s.size();
			printf("    (%zu packets, %zu payload bytes -> %zu frames, %zu bytes on the wire)\n", sent.size(), payload,
			       out.frames.size(), wire_bytes);

			for (const auto& f : out.frames)
				in.RecievePacket(f.data(), u32(f.size()), 7);
			bool identical = in.packets.size() == sent.size();
			for (size_t i = 0; identical && i < sent.size(); ++i)
				identical = in.packets[i] == sent[i];
			check(in.packets.size() == sent.size(), "every packet comes out of the frames");
			check(identical, "packets are byte-identical and in order");
			check(in.last_param == 7, "the receive parameter (sender id) is passed through");
		}

		{
			// a single packet of the largest mergeable size, and several of them back to back
			WireOut out;
			WireIn in;
			Rng rng(5);
			NET_Packet big;
			build(big, kTypeData, 0, kMaxMultipacketPacket, rng);
			for (int i = 0; i < 3; ++i)
				out.SendPacket(big.B.data, big.B.count, net_flags(TRUE, TRUE), 0);
			out.FlushSendBuffer(0);
			check(out.frames.size() == 3, "maximum-size packets get one frame each");
			for (const auto& f : out.frames)
				in.RecievePacket(f.data(), u32(f.size()), 0);
			bool same = in.packets.size() == 3;
			for (size_t i = 0; same && i < 3; ++i)
				same = in.packets[i].size() == big.B.count && !memcmp(in.packets[i].data(), big.B.data, big.B.count);
			check(same, "maximum-size packets survive the round trip");
		}

		{
			// reliable and unreliable packets are never merged into one frame; DPNSEND_IMMEDIATELLY sends at once
			WireOut out;
			WireIn in;
			Rng rng(8);
			NET_Packet a, b, c;
			build(a, kTypeData, 0, 40, rng);
			build(b, kTypeData, 1, 40, rng);
			build(c, kTypeData, 2, 40, rng);
			const u32 f_rel = net_flags(TRUE, TRUE), f_unrel = net_flags(FALSE, TRUE), f_now = net_flags(TRUE, TRUE, TRUE, TRUE);
			out.SendPacket(a.B.data, a.B.count, f_rel, 0);
			out.SendPacket(b.B.data, b.B.count, f_unrel, 0);
			check(out.frames.size() == 1 && out.flags[0] == f_rel, "changing the flags flushes the buffer with the old flags");
			out.SendPacket(c.B.data, c.B.count, f_now, 0);
			check(out.frames.size() == 3, "DPNSEND_IMMEDIATELLY flushes at once");
			out.FlushSendBuffer(0);
			check(out.frames.size() == 3, "flushing an empty buffer sends nothing");
			for (const auto& f : out.frames)
				in.RecievePacket(f.data(), u32(f.size()), 0);
			check(in.packets.size() == 3 && in.packets[0].size() == a.B.count && in.packets[1].size() == b.B.count
			          && in.packets[2].size() == c.B.count,
			      "all three packets arrive");
		}
	}

	// ---------------------------------------------------------------------------------------------------------------
	printf("\n[5] shutdown, then network sessions are refused (no DirectPlay)\n");
	{
		client->Disconnect();
		server->Disconnect();
		check(client->net_isCompleted_Connect() == FALSE, "client Disconnect resets the connection state");
		xr_delete(client);
		xr_delete(server);
		check(!psNET_direct_connect, "destroying the pair clears psNET_direct_connect");

		server = xr_new<TestServer>(&timer);
		client = xr_new<TestClient>(&timer);
		server->client = client;
		client->server = server;
		g_log.clear();

		const IPureServer::EConnect r = server->Connect("mp_testmap/dm/maxplayers=4/portsv=1237", game_descr);
		check(r == IPureServer::ErrConnect, "hosting a network session returns ErrConnect");
		check(!psNET_direct_connect, "direct connect stays off");
		check(logged("not supported on this platform"), "the server refusal is logged");

		g_log.clear();
		check(!client->Connect("192.168.1.10/name=player/port=1237"), "connecting to a host returns FALSE");
		check(client->net_isFails_Connect(), "the client reports a failed connection");
		check(client->invalid_host_calls == 1, "OnInvalidHost is called");
		check(logged("not supported on this platform"), "the client refusal is logged");

		ip_address addr;
		DWORD port = 7;
		check(!client->GetServerAddress(addr, &port) && port == 0, "GetServerAddress has nothing to report");
		client->net_Syncronize();
		check(client->net_isDisconnected(), "net_Syncronize fails instead of waiting for a server");

		ip_address sv_addr;
		check(!server->GetClientAddress(ClientID(1), sv_addr), "the server has no client addresses");
		check(!server->DisconnectClient(nullptr, "x"), "DisconnectClient(NULL) is refused");

		// a send with no transport is dropped (and logged), not a crash
		g_log.clear();
		NET_Packet P;
		Rng rng(1);
		build(P, kTypeData, 0, 64, rng);
		server->IPureServer::SendTo_LL(ClientID(5), P.B.data, P.B.count);
		check(logged("no network transport"), "IPureServer::SendTo_LL without a transport is dropped and logged");

		client->Disconnect();
		server->Disconnect();
		xr_delete(client);
		xr_delete(server);
	}

	printf("\n%s (%d check%s failed)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
	Core._destroy();
	return g_failures ? 1 : 0;
}
