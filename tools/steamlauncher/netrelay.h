// Headcutter Steam launcher: network packet relay
// Copyright (c) 2026 schwipsy
// SPDX-License-Identifier: MIT (see LICENSE in this folder)
//
// The game keeps its normal UDP netcode, but when started for online play it
// hands every packet to us through a message-mode pipe instead of a socket.
// Each pipe message is: 4 bytes IPv4 + 2 bytes port (network order) + data.
//
// We carry the packets with a Transport:
//   SteamTransport - Steam FakeIP: every player gets a global fake IPv4:port
//                    from Steam, and packets travel over Steam's relay
//                    network (no port forwarding, NAT handled by Steam).
//   UdpTransport   - plain UDP on 127.0.0.1, only for testing several game
//                    instances on one PC without Steam accounts.
// Because addresses stay ordinary IPv4:port values, the game's peer-to-peer
// netcode works unchanged for any number of players.

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

#include "steam/steam_api.h"
#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"
#include "steam/steamnetworkingfakeip.h"
#include "steam/isteamnetworkingmessages.h"

struct Transport
{
	uint32_t myIP = 0;	// host byte order
	uint16_t myPort = 0;
	virtual ~Transport() {}
	virtual bool Ready() = 0;
	virtual void Send(uint32_t ip, uint16_t port, const uint8_t *data, int len) = 0;
	// Returns the packet length, or -1 if nothing is waiting.
	virtual int Recv(uint32_t &ip, uint16_t &port, uint8_t *buf, int maxlen) = 0;
	virtual std::string Error() { return ""; }

	std::string Address() const
	{
		char b[32];
		snprintf(b, sizeof(b), "%u.%u.%u.%u:%u", myIP >> 24, (myIP >> 16) & 255, (myIP >> 8) & 255, myIP & 255, myPort);
		return b;
	}
};

//--------------------------------------------------------------------------

class SteamTransport : public Transport
{
	ISteamNetworkingFakeUDPPort *port = nullptr;
	bool requested = false, failed = false;
	std::string err;
	STEAM_CALLBACK(SteamTransport, OnFakeIP, SteamNetworkingFakeIPResult_t);

public:
	SteamTransport()
	{
		SteamNetworkingUtils()->InitRelayNetworkAccess();
	}

	// Call every ~10 ms until Ready() or Failed(). Waits for the relay network
	// (asking for a FakeIP before it is up fails with result 2), then requests
	// the FakeIP, retrying a few times.
	int tries = 0, wait = 0;
	void Tick()
	{
		if (port || failed) return;
		if (wait > 0) { wait--; return; }
		if (requested) return; // callback pending
		SteamRelayNetworkStatus_t st;
		ESteamNetworkingAvailability a = SteamNetworkingUtils()->GetRelayNetworkStatus(&st);
		if (a != k_ESteamNetworkingAvailability_Current && tries == 0 && waitedRelay++ < 1000) return;
		tries++;
		requested = SteamNetworkingSockets()->BeginAsyncRequestFakeIP(1);
		if (!requested) Retry("FakeIP request refused");
	}
	int waitedRelay = 0;
	void Retry(const std::string &why)
	{
		requested = false;
		err = why;
		if (tries >= 5) failed = true;
		else wait = 200; // 2 s
	}
	~SteamTransport() override
	{
		if (port) port->DestroyFakeUDPPort();
	}
	bool Ready() override { return port != nullptr; }
	bool Failed() const { return failed; }
	std::string Error() override { return err; }

	void Send(uint32_t ip, uint16_t p, const uint8_t *data, int len) override
	{
		if (!port) return;
		SteamNetworkingIPAddr addr;
		addr.Clear();
		addr.SetIPv4(ip, p);
		port->SendMessageToFakeIP(addr, data, (uint32)len, k_nSteamNetworkingSend_UnreliableNoNagle);
	}

	int Recv(uint32_t &ip, uint16_t &p, uint8_t *buf, int maxlen) override
	{
		if (!port) return -1;
		SteamNetworkingMessage_t *msg = nullptr;
		if (port->ReceiveMessages(&msg, 1) != 1) return -1;
		int len = -1;
		const SteamNetworkingIPAddr *from = msg->m_identityPeer.GetIPAddr();
		if (from && msg->m_cbSize <= maxlen)
		{
			ip = from->GetIPv4();
			p = from->m_port;
			memcpy(buf, msg->m_pData, msg->m_cbSize);
			len = msg->m_cbSize;
		}
		msg->Release();
		return len < 0 ? 0 : len; // 0 = dropped, try again
	}
};

inline void SteamTransport::OnFakeIP(SteamNetworkingFakeIPResult_t *r)
{
	if (r->m_eResult != k_EResultOK)
	{
		Retry("FakeIP request failed (" + std::to_string((int)r->m_eResult) + ")");
		return;
	}
	myIP = r->m_unIP;
	myPort = r->m_unPorts[0];
	port = SteamNetworkingSockets()->CreateFakeUDPPort(0);
	if (!port) { failed = true; err = "CreateFakeUDPPort failed"; }
}

//--------------------------------------------------------------------------
// Steam account transport: packets go to players by Steam account through
// ISteamNetworkingMessages (relayed, no port forwarding, nothing to request
// up front). The game still sees IPv4:port addresses: each player's address
// is their 32-bit Steam account ID as the IP, with a fixed port. Everyone
// derives the same address for the same player, so the host can hand guest
// addresses to the other guests as usual.

static const uint16_t kSteamIdPort = 5029;

inline uint32_t SteamIdToIP(uint64_t id) { return CSteamID(id).GetAccountID(); }
inline CSteamID IPToSteamId(uint32_t ip) { return CSteamID(ip, k_EUniversePublic, k_EAccountTypeIndividual); }

class SteamIdTransport : public Transport
{
	STEAM_CALLBACK(SteamIdTransport, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t);
	STEAM_CALLBACK(SteamIdTransport, OnSessionFailed, SteamNetworkingMessagesSessionFailed_t);

public:
	SteamIdTransport()
	{
		SteamNetworkingUtils()->InitRelayNetworkAccess();
		myIP = SteamIdToIP(SteamUser()->GetSteamID().ConvertToUint64());
		myPort = kSteamIdPort;
	}
	bool Ready() override { return SteamNetworkingMessages() != nullptr; }
	std::string Error() override { return "Steam networking is not available"; }

	void Send(uint32_t ip, uint16_t, const uint8_t *data, int len) override
	{
		SteamNetworkingIdentity to;
		to.SetSteamID(IPToSteamId(ip));
		SteamNetworkingMessages()->SendMessageToUser(to, data, (uint32)len,
			k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_AutoRestartBrokenSession, 0);
	}

	int Recv(uint32_t &ip, uint16_t &p, uint8_t *buf, int maxlen) override
	{
		SteamNetworkingMessage_t *msg = nullptr;
		if (SteamNetworkingMessages()->ReceiveMessagesOnChannel(0, &msg, 1) != 1) return -1;
		int len = 0;
		CSteamID from = msg->m_identityPeer.GetSteamID();
		if (from.IsValid() && msg->m_cbSize <= maxlen)
		{
			ip = from.GetAccountID();
			p = kSteamIdPort;
			memcpy(buf, msg->m_pData, msg->m_cbSize);
			len = msg->m_cbSize;
		}
		msg->Release();
		return len; // 0 = dropped, try again
	}

	void (*log)(const char *, ...) = nullptr;
};

inline void SteamIdTransport::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t *r)
{
	SteamNetworkingMessages()->AcceptSessionWithUser(r->m_identityRemote);
	if (log) log("net: session with %llu", r->m_identityRemote.GetSteamID64());
}

inline void SteamIdTransport::OnSessionFailed(SteamNetworkingMessagesSessionFailed_t *r)
{
	if (log) log("net: session with %llu failed: %s", r->m_info.m_identityRemote.GetSteamID64(), r->m_info.m_szEndDebug);
}

//--------------------------------------------------------------------------

class UdpTransport : public Transport
{
	SOCKET s = INVALID_SOCKET;

public:
	explicit UdpTransport(uint16_t localPort)
	{
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
		s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = htons(localPort);
		if (bind(s, (sockaddr *)&a, sizeof(a)) != 0) { closesocket(s); s = INVALID_SOCKET; return; }
		u_long nb = 1;
		ioctlsocket(s, FIONBIO, &nb);
		myIP = INADDR_LOOPBACK;
		myPort = localPort;
	}
	~UdpTransport() override { if (s != INVALID_SOCKET) closesocket(s); }
	bool Ready() override { return s != INVALID_SOCKET; }

	void Send(uint32_t ip, uint16_t p, const uint8_t *data, int len) override
	{
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(ip);
		a.sin_port = htons(p);
		sendto(s, (const char *)data, len, 0, (sockaddr *)&a, sizeof(a));
	}

	int Recv(uint32_t &ip, uint16_t &p, uint8_t *buf, int maxlen) override
	{
		sockaddr_in a = {};
		int al = sizeof(a);
		int n = recvfrom(s, (char *)buf, maxlen, 0, (sockaddr *)&a, &al);
		if (n == SOCKET_ERROR) return WSAGetLastError() == WSAEMSGSIZE ? 0 : -1;
		ip = ntohl(a.sin_addr.s_addr);
		p = ntohs(a.sin_port);
		return n;
	}
};

//--------------------------------------------------------------------------
// Message-mode pipe carrying packets to and from the game.

class PacketPipe
{
	HANDLE h = INVALID_HANDLE_VALUE;
	OVERLAPPED ov = {};
	bool connected = false;
	std::vector<uint8_t> buf = std::vector<uint8_t>(6 + 65536);

public:
	bool Create(const std::wstring &name)
	{
		h = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 256 * 1024, 256 * 1024, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;
		ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!ConnectNamedPipe(h, &ov))
		{
			DWORD e = GetLastError();
			if (e == ERROR_PIPE_CONNECTED) connected = true;
			else if (e != ERROR_IO_PENDING) return false;
		}
		return true;
	}

	bool Connected()
	{
		DWORD d;
		if (!connected && GetOverlappedResult(h, &ov, &d, FALSE)) connected = true;
		return connected;
	}

	// Game -> launcher. Returns payload length, -1 if none.
	int Read(uint32_t &ip, uint16_t &port, const uint8_t *&data)
	{
		if (!Connected()) return -1;
		DWORD avail = 0;
		if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return -1;
		OVERLAPPED r = {};
		r.hEvent = ov.hEvent;
		ResetEvent(r.hEvent);
		DWORD got = 0;
		if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, &r))
		{
			if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(h, &r, &got, TRUE)) return -1;
		}
		if (got < 6) return 0;
		ip = (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) | (uint32_t(buf[2]) << 8) | buf[3];
		port = uint16_t((buf[4] << 8) | buf[5]);
		data = buf.data() + 6;
		return int(got) - 6;
	}

	// Launcher -> game.
	void Write(uint32_t ip, uint16_t port, const uint8_t *data, int len)
	{
		if (!Connected() || len < 0 || len > 65536) return;
		std::vector<uint8_t> m(6 + len);
		m[0] = uint8_t(ip >> 24); m[1] = uint8_t(ip >> 16); m[2] = uint8_t(ip >> 8); m[3] = uint8_t(ip);
		m[4] = uint8_t(port >> 8); m[5] = uint8_t(port);
		memcpy(m.data() + 6, data, len);
		OVERLAPPED w = {};
		w.hEvent = ov.hEvent;
		ResetEvent(w.hEvent);
		DWORD n = 0;
		if (!WriteFile(h, m.data(), (DWORD)m.size(), &n, &w) && GetLastError() == ERROR_IO_PENDING)
			GetOverlappedResult(h, &w, &n, TRUE);
	}
};
