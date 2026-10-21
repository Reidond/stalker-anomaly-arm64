# xrNetServer on Apple platforms

`src/xrNetServer` builds as a static library (`Anomaly::xrNetServer`, `cmake/targets/xrNetServer.cmake`) for macOS and
iOS/iPadOS, arm64 only. It is off by default: `-DANOMALY_BUILD_XRNETSERVER=ON` (or `ANOMALY_BUILD_ALL_MODULES=ON`).

Anomaly is single player only here. The Windows DirectPlay transport is replaced by a stub that refuses network
sessions; single player never used the transport anyway. The Windows build is unchanged: every port change sits behind
`XR_NET_STUB_TRANSPORT`, which `NET_Shared.h` defines when `XR_PLATFORM_APPLE` is.

```bash
cmake --preset macos-arm64 -DANOMALY_BUILD_XRNETSERVER=ON
cmake --build --preset macos-arm64 --target xrNetServer xrnet_smoke
build/macos-arm64/tools/xrnet_smoke/xrnet_smoke

cmake --preset ios-arm64 -DANOMALY_BUILD_XRNETSERVER=ON
xcodebuild -project build/ios-arm64/AnomalyPort.xcodeproj -scheme xrNetServer \
  -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO build
```

## How a single-player session uses xrNetServer

A single-player game runs an `xrServer` (derives `IPureServer`) and a `CLevel` (derives `IPureClient`) in one process. They
still talk through the xrNetServer interfaces, but the DirectPlay session is never created.

| Step | Where | What happens |
|---|---|---|
| Server start | `Level_start.cpp` `net_start1/2`, `xrServer::Connect` (`xrServer_Connect.cpp:40`) | `IPureServer::Connect(options)` sees `/single` in the options (`console_commands.cpp` builds `start server(<save>/single/alife/load)`) and sets `psNET_direct_connect = TRUE` (`NET_Server.cpp:250`). The DirectPlay `Host()` block, the ban list and the IP filter are skipped. |
| Local client | `CLevel::net_start_client2` -> `xrServer::create_direct_client` (`xrServer.cpp:1040`) | `new_client()` with `ClientID` 1 and the name `single_player`; `AttachNewClient` makes it `SV_Client`, flags it local and sends it a `MSYS_CONFIG` through `SendTo_LL`. |
| Client connect | `CLevel::Connect2Server` -> `IPureClient::Connect` | With `psNET_direct_connect` it does nothing but reset the time delta and return TRUE. `m_bConnectResultReceived` is set by the caller and `net_Syncronised` is set TRUE, so there is no ping-based time sync. |
| Client -> server | `CLevel::Send` (`Level_network.cpp:294`) | `Server->OnMessage(P, ClientID(1))`: a direct call on the `NET_Packet`. |
| Server -> client | `xrServer::SendTo_LL` (`xrServer.cpp:797`) | "optimize local traffic": `Level().OnMessage(data, size)` -> `IPureClient::OnMessage` copies the bytes into a `NET_Packet` from the `INetQueue` pool. |
| Receive | `CLevel::ClientReceive` | `StartProcessQueue()`, `net_msg_Retreive()` / `net_msg_Release()` per packet, `EndProcessQueue()`. The queue lock is a recursive `xrCriticalSection`, so a network thread (`mtNetwork`) can enqueue while the game thread drains. |
| Rate gates | `IPureClient::net_HasBandwidth`, `IPureServer::HasBandwidth` | The `psNET_direct_connect` branches only throttle by time; they never look at a send queue. |
| Stop | `CLevel::net_Stop` | `IPureClient::Disconnect`, `IPureServer::Disconnect`: with no DirectPlay objects (`NET == NULL`) they only reset state. |

What single player does **not** use: `MultipacketSender` / `MultipacketReciever` framing, `NET_Compressor` (its compress step is
also gated by `!psNET_direct_connect`), the ping/time-sync thread, ban list, IP filter, `NET_AuthCheck`, and every
`IDirectPlay8*` call. DirectPlay is therefore only a compile-time dependency in single player:

- the interface pointers and `HOST_NODE` in `NET_Client.h` / `NET_Server.h` (`IDirectPlay8Client*`, `IDirectPlay8Address*`,
  `DPN_APPLICATION_DESC`), and `DPN_CONNECTION_INFO` inside `IClientStatistic`;
- the `DPNSEND_*` flag constants, which xrGame names as default arguments (`Level.h`, `xrServer.h`, `game_sv_base.h`,
  `GameObject.h`, `game_sv_mp.cpp`) and `net_flags()` combines;
- `psNET_direct_connect` itself, read by `xrServer*.cpp`, `Level*.cpp`, `game_sv_base.cpp` (list: `grep -rn psNET_direct_connect src/xrGame`).

All of these are covered by the shim's `Common/compat/DPlay/dplay8.h` (names only; the interfaces are incomplete types).
`DPNSEND_IMMEDIATELLY` is defined by `NET_Messages.h`. No new shim header was needed and no shim change was requested.

## What was stubbed

`NET_Client.cpp` and `NET_Server.cpp` keep one source for both platforms. Each DirectPlay body is wrapped in
`#ifdef XR_NET_STUB_TRANSPORT ... #else <original> #endif`; the original code stays byte-for-byte, so the Windows build
compiles exactly what it did before (preprocessing the files with the macro undefined reproduces the old text apart from the
changes listed under "Other changes").

| Function | Windows | Apple stub |
|---|---|---|
| `IPureServer::Connect`, non-`/single` | `CoCreateInstance` + `Host()` on a UDP port | logs `hosting a network session is not supported on this platform`, returns `ErrConnect` (the level start then reports `Failed to start server`) |
| `IPureClient::Connect`, non-direct | enumerate hosts, `Connect()` | logs `network connections are not supported on this platform`, sets `net_isFails_Connect()`, calls `OnInvalidHost()`, returns FALSE |
| `net_Syncronize` | spawns the ping thread | logs and marks the client disconnected, so waiters in `Connect2Server` leave their loop; `Sync_Thread` is empty |
| `SendTo_LL` (client and server) | `IDirectPlay8*::Send/SendTo` | counts the bytes, logs the first 8 drops, returns. Never reached in single player (see above). |
| `net_Handler` (both) | DirectPlay message switch | `return S_OK` |
| `HasBandwidth`, `net_HasBandwidth` (non-direct) | `GetSendQueueInfo` | FALSE |
| `UpdateStatistic`, `UpdateClientStatistic` (non-direct) | `GetConnectionInfo` | no-op |
| `Disconnect` (both) | `Close()`, `Release()` of the COM objects | only resets state |
| `DisconnectClient`, `GetClientAddress`, `GetServerAddress` | DirectPlay calls, `gethostbyname` | log and return false / empty |

Left alone because they need no transport: `INetQueue`, `MultipacketSender` / `MultipacketReciever`, `NET_Compressor`,
`INetLog`, `ip_filter`, ban list code, `NET_AuthCheck`, `IClient`, the `ip_address` helpers. The GUID tables in `NET_Client.cpp`
are plain constants and compile as they are; `typedef REFIID DP8REFIID` (unused, and `REFIID` is COM) is excluded on Apple.

### Other changes (both platforms)

- `psNET_Port` was never initialised in `IPureServer` and single player reads it (`CLevel::net_start3`, `GetPort()`); it starts at 0.
- `IPureClient` left `net_Connected`, `net_Syncronised`, `net_Disconnected` uninitialised until `Connect()`; they start at
  wait / FALSE / FALSE.
- `#include <dplay/dplay8.h>` in `NET_Shared.h` is spelled `<DPlay/dplay8.h>`, matching the shim directory on a case-sensitive volume.

## Link-time needs

- Library: `Anomaly::xrNetServer` (static). It links `Anomaly::EngineConfig` PUBLIC, so consumers also get xrCore, the include
  roots and the dialect flags. It exports `XR_NETSERVER_EXPORTS` (empty `XRNETSERVER_API`).
- Undefined symbols resolve against xrCore only (`Memory`, `Msg`, `Debug`, `Core`, `FS`, `xrCriticalSection`, `crc32`, `rtc_*`
  compression, `thread_spawn`, ...) plus libc++. No xrEngine, xrGame, Windows or DirectPlay library is needed;
  `xrnet_smoke` links `Anomaly::xrNetServer` alone.
- Globals defined here that the game reads or sets: `psNET_Flags`, `psNET_ClientUpdate/ClientPending/ServerUpdate/ServerPending`,
  `psNET_Name`, `psNET_direct_connect`, `psNET_GuaranteedPacketMode`, `g_net_compressor_enabled`,
  `g_net_compressor_gather_stats`, `BroadcastCID`, `nameTraffic`. `get_psNET_ClientUpdate()` / `get_psNET_ServerUpdate()`
  are declared in `NET_Shared.h` but defined nowhere (not referenced).
- xrGame needs nothing from this module that the Windows build did not: `Level.h` and `xrServer.h` include
  `xrNetServer/NET_Client.h` / `NET_Server.h`, and the DirectPlay names they use come from the shim.

## `tools/xrnet_smoke`

macOS only, headless, no game data (it writes a minimal `fsgame.ltx`, its log and `appdata/` under
`build/macos-arm64/tools/xrnet_smoke/run/`; `--run-dir <dir>` overrides). `xrServer` and `CLevel` live in xrGame, so it
carries minimal `IPureServer` / `IPureClient` subclasses that do what the game does at the three transport touchpoints
(`TestServer::SendTo_LL` = `xrServer::SendTo_LL`, `TestClient::Send` = `CLevel::Send`, `drain()` = `CLevel::ClientReceive`).

1. `/single` connect: direct mode is selected, client connects, the local client is attached, the `MSYS_CONFIG` handshake arrives.
2. 3000 packets each way from 15 bytes to the largest `NET_Packet` (16383), drained in random bursts: order, size and an FNV
   checksum of every packet; request/answer echo (200); broadcast with and without the sender excluded.
3. 20 000 packets with the server in a producer thread and the client draining concurrently.
4. The multipacket framing a transport would carry: 600 mixed-size packets merge into 41 frames and come back byte-identical
   and in order; maximum-size packets get a frame each; a change of flags or `DPNSEND_IMMEDIATELLY` flushes.
5. Without `/single`: hosting and connecting are refused, logged, and `net_Syncronize`, `GetServerAddress`, `GetClientAddress`,
   `DisconnectClient` and a transport-less `SendTo_LL` do not crash.

## Things noticed (not changed)

- `MultipacketSender::SendPacket` overflows its buffer by a byte for a packet of 16383 bytes (`NET_PacketSizeLimit - 1`, which
  `NET_Packet` itself still allows): it flushes when `count + size + 2 >= 16384` and then writes `size + 2` bytes into the empty
  buffer. 16382 fits exactly but trips the `VERIFY` in `NET_Packet::w` in a debug build, so the smoke test uses 16381.
  Irrelevant to single player (no multipacket framing).
- With `g_net_compressor_enabled` on, `NET_Compressor::Compress` / `Decompress` compute the CRC over `payload + 5` bytes, reading
  past the end of the data on both sides, so the check cannot match. The flag is off by default and only the network path
  compresses (`!psNET_direct_connect`), so the smoke test leaves it off.
- `IClient::_SendTo_LL` calls `IPureServer::SendTo_LL` non-virtually. On Windows that dereferences `NET == NULL` in single
  player if a client's multipacket buffer is ever flushed with data; the single-player path avoids it by never buffering.
  On Apple it drops the bytes and logs.
- `NET_Compressor.cpp` has `#if NET_USE_COMPRESSSION` (three S) around an `__asm { int 3 }`: dead code, left as is.
