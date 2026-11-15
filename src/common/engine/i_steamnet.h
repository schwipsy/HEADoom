// Optional packet transport through the Steam launcher (tools/steamlauncher).
// When the launcher provides a packet pipe, network packets go to it instead
// of the UDP socket; the launcher carries them over Steam's relay network.
// Addresses stay plain IPv4:port (Steam FakeIPs), so the netcode is unchanged.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

struct sockaddr_in;

bool SteamNet_Active();
void SteamNet_Send(const sockaddr_in &to, const void *data, int len);
// Returns the packet size, or -1 when nothing is waiting.
int SteamNet_Recv(sockaddr_in *from, void *buf, int maxlen);
