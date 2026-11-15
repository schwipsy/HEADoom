// Client for the Steam launcher (tools/steamlauncher). No-op without it.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "zstring.h"

void SteamPipe_Poll();
void SteamPipe_Achievement(const FString &name);
void SteamPipe_SetStat(const FString &name, int value);
void SteamPipe_AddStat(const FString &name, int value);
void SteamPipe_Store();
bool SteamPipe_Online();
