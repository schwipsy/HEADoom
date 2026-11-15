//
// Client for the Steam launcher (tools/steamlauncher).
//
// The engine never links the Steamworks SDK. When the game is started by the
// launcher, HEADCUTTER_STEAM_PIPE names a local pipe; achievements and stats
// are sent over it as plain text lines. Without the launcher (or off Windows)
// every call here is a no-op, so the engine runs exactly as before.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <stdlib.h>

#include "c_dispatch.h"
#include "c_cvars.h"
#include "doomstat.h"
#include "gamestate.h"
#include "menu.h"
#include "menustate.h"
#include "vm.h"
#include "printf.h"
#include "g_steampipe.h"

CVAR(Bool, steam_overlaypause, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

static bool SteamOnline;
static FString SteamPersona;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HANDLE Pipe = INVALID_HANDLE_VALUE;
static bool Tried;
static FString InBuf;

static void Connect();

static void ClosePipe()
{
	if (Pipe != INVALID_HANDLE_VALUE) CloseHandle(Pipe);
	Pipe = INVALID_HANDLE_VALUE;
	SteamOnline = false;
}

static void SendLine(const FString &line)
{
	if (!Tried) Connect(); // commands can arrive before the first frame
	if (Pipe == INVALID_HANDLE_VALUE) return;
	FString s = line + "\n";
	DWORD w = 0;
	if (!WriteFile(Pipe, s.GetChars(), (DWORD)s.Len(), &w, nullptr)) ClosePipe();
}

static void OnLine(const FString &line)
{
	if (line.IndexOf("READY ") == 0)
	{
		SteamOnline = line.Len() > 6 && line[6] == '1';
		SteamPersona = line.Len() > 8 ? line.Mid(8) : FString();
		DPrintf(DMSG_NOTIFY, "Steam: %s %s\n", SteamOnline ? "online" : "offline", SteamPersona.GetChars());
	}
	else if (line.IndexOf("OVERLAY 1") == 0)
	{
		if (steam_overlaypause && gamestate == GS_LEVEL && !netgame && menuactive == MENU_Off)
		{
			M_StartControlPanel(false);
			M_SetMenu(NAME_MainMenu);
		}
	}
	else if (line.IndexOf("ERR ") == 0)
	{
		DPrintf(DMSG_WARNING, "Steam: %s\n", line.GetChars() + 4);
	}
}

static void AtExitQuit()
{
	SendLine("QUIT");
	ClosePipe();
}

static void Connect()
{
	Tried = true;
	const char *name = getenv("HEADCUTTER_STEAM_PIPE");
	if (name == nullptr || strncmp(name, "\\\\.\\pipe\\", 9) != 0) return;
	Pipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (Pipe == INVALID_HANDLE_VALUE) return;
	atexit(AtExitQuit);
	SendLine("HELLO 1");
}

void SteamPipe_Poll()
{
	if (!Tried) Connect();
	if (Pipe == INVALID_HANDLE_VALUE) return;
	for (;;)
	{
		DWORD avail = 0;
		if (!PeekNamedPipe(Pipe, nullptr, 0, nullptr, &avail, nullptr)) { ClosePipe(); return; }
		if (avail == 0) break;
		char buf[512];
		DWORD got = 0;
		if (!ReadFile(Pipe, buf, avail < sizeof(buf) ? avail : sizeof(buf), &got, nullptr)) { ClosePipe(); return; }
		InBuf.AppendCStrPart(buf, got);
	}
	long nl;
	while ((nl = InBuf.IndexOf('\n')) >= 0)
	{
		FString line = InBuf.Left(nl);
		InBuf = InBuf.Mid(nl + 1);
		line.StripRight();
		if (line.Len() > 0) OnLine(line);
	}
}

#else

void SteamPipe_Poll() {}
static void SendLine(const FString &) {}

#endif

// Steam API names are letters, digits, '_', '-' and '.'; anything else would
// break the line protocol.
static bool ValidName(const FString &name)
{
	if (name.Len() == 0 || name.Len() > 128) return false;
	for (unsigned i = 0; i < name.Len(); i++)
	{
		char c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
			return false;
	}
	return true;
}

void SteamPipe_Achievement(const FString &name)
{
	if (ValidName(name)) { SendLine("ACH " + name); SendLine("STORE"); }
}

void SteamPipe_SetStat(const FString &name, int value)
{
	if (ValidName(name)) SendLine(FStringf("STAT %s %d", name.GetChars(), value));
}

void SteamPipe_AddStat(const FString &name, int value)
{
	if (ValidName(name)) SendLine(FStringf("ADDSTAT %s %d", name.GetChars(), value));
}

void SteamPipe_Store()
{
	SendLine("STORE");
}

bool SteamPipe_Online() { return SteamOnline; }

//==========================================================================
//
// Console commands
//
//==========================================================================

CCMD(steam_achievement)
{
	if (argv.argc() < 2) { Printf("usage: steam_achievement <api_name>\n"); return; }
	SteamPipe_Achievement(argv[1]);
}

CCMD(steam_stat)
{
	if (argv.argc() < 3) { Printf("usage: steam_stat <api_name> <value>\n"); return; }
	SteamPipe_SetStat(argv[1], atoi(argv[2]));
}

CCMD(steam_addstat)
{
	if (argv.argc() < 3) { Printf("usage: steam_addstat <api_name> <amount>\n"); return; }
	SteamPipe_AddStat(argv[1], atoi(argv[2]));
}

CCMD(steam_store)
{
	SteamPipe_Store();
}

CCMD(steam_status)
{
	Printf("Steam: %s%s%s\n", SteamOnline ? "online as " : "offline", SteamPersona.GetChars(), "");
}

//==========================================================================
//
// ZScript: struct Steam (engine/base.zs)
//
//==========================================================================

DEFINE_ACTION_FUNCTION(_Steam, Achievement)
{
	PARAM_PROLOGUE;
	PARAM_STRING(name);
	SteamPipe_Achievement(name);
	return 0;
}

DEFINE_ACTION_FUNCTION(_Steam, SetStat)
{
	PARAM_PROLOGUE;
	PARAM_STRING(name);
	PARAM_INT(value);
	SteamPipe_SetStat(name, value);
	return 0;
}

DEFINE_ACTION_FUNCTION(_Steam, AddStat)
{
	PARAM_PROLOGUE;
	PARAM_STRING(name);
	PARAM_INT(value);
	SteamPipe_AddStat(name, value);
	return 0;
}

DEFINE_ACTION_FUNCTION(_Steam, Store)
{
	PARAM_PROLOGUE;
	SteamPipe_Store();
	return 0;
}

DEFINE_ACTION_FUNCTION(_Steam, IsOnline)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_BOOL(SteamOnline);
}

DEFINE_ACTION_FUNCTION(_Steam, PersonaName)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_STRING(SteamPersona);
}
