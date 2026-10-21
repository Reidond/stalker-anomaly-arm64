// Stand-in for the DirectPlay 8 header <DPlay/dplay8.h>. DirectPlay does not exist on Apple platforms: the engine's
// own message layer keeps working over the in-process transport (xrNetServer's `empty` backend). Only the names that
// xrGame, xrEngine and xrNetServer's headers use are declared: DPNID, the DPNSEND_* flags, the application and
// connection description structs, and the interface names (as incomplete types, only ever held by pointer).
#pragma once

#include "../../Platform.h"

typedef DWORD DPNID, *PDPNID;
typedef DWORD DPNHANDLE, *PDPNHANDLE;
typedef DWORD DPNMSGID;

#define DPNID_ALL_PLAYERS_GROUP 0

#define DPNOP_SYNC 0x80000000
#define DPNSEND_SYNC DPNOP_SYNC
#define DPNSEND_NOCOPY 0x0001
#define DPNSEND_NOCOMPLETE 0x0002
#define DPNSEND_COMPLETEONPROCESS 0x0004
#define DPNSEND_GUARANTEED 0x0008
#define DPNSEND_NONSEQUENTIAL 0x0010
#define DPNSEND_NOLOOPBACK 0x0020
#define DPNSEND_PRIORITY_LOW 0x0040
#define DPNSEND_PRIORITY_HIGH 0x0080
#define DPNSEND_COALESCE 0x0100

#define DPNSESSION_CLIENT_SERVER 0x0001
#define DPNSESSION_MIGRATE_HOST 0x0004
#define DPNSESSION_NODPNSVR 0x0040
#define DPNSESSION_REQUIREPASSWORD 0x0080
#define DPNSESSION_NOENUMS 0x0100
#define DPNSESSION_FAST_SIGNED 0x0200
#define DPNSESSION_FULL_SIGNED 0x0400

#ifdef __cplusplus
struct IDirectPlay8Client;
struct IDirectPlay8Server;
struct IDirectPlay8Peer;
struct IDirectPlay8Address;
struct IDirectPlay8ThreadPool;
struct IDirectPlay8NATResolver;

typedef struct _DPN_APPLICATION_DESC
{
	DWORD dwSize;
	DWORD dwFlags;
	GUID guidInstance;
	GUID guidApplication;
	DWORD dwMaxPlayers;
	DWORD dwCurrentPlayers;
	WCHAR* pwszSessionName;
	WCHAR* pwszPassword;
	PVOID pvReservedData;
	DWORD dwReservedDataSize;
	PVOID pvApplicationReservedData;
	DWORD dwApplicationReservedDataSize;
} DPN_APPLICATION_DESC, *PDPN_APPLICATION_DESC;

typedef struct _DPN_CONNECTION_INFO
{
	DWORD dwSize;
	DWORD dwRoundTripLatencyMS;
	DWORD dwThroughputBPS;
	DWORD dwPeakThroughputBPS;

	DWORD dwBytesSentGuaranteed;
	DWORD dwPacketsSentGuaranteed;
	DWORD dwBytesSentNonGuaranteed;
	DWORD dwPacketsSentNonGuaranteed;

	DWORD dwBytesRetried;
	DWORD dwPacketsRetried;
	DWORD dwBytesDropped;
	DWORD dwPacketsDropped;

	DWORD dwMessagesTransmittedHighPriority;
	DWORD dwMessagesTimedOutHighPriority;
	DWORD dwMessagesTransmittedNormalPriority;
	DWORD dwMessagesTimedOutNormalPriority;
	DWORD dwMessagesTransmittedLowPriority;
	DWORD dwMessagesTimedOutLowPriority;

	DWORD dwBytesReceivedGuaranteed;
	DWORD dwPacketsReceivedGuaranteed;
	DWORD dwBytesReceivedNonGuaranteed;
	DWORD dwPacketsReceivedNonGuaranteed;
	DWORD dwMessagesReceived;
} DPN_CONNECTION_INFO, *PDPN_CONNECTION_INFO;
#endif // __cplusplus
