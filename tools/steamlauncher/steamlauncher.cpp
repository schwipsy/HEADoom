// Headcutter Steam launcher
// Copyright (c) 2026 schwipsy
// SPDX-License-Identifier: MIT (see LICENSE in this folder)
//
// A separate process that owns the Steamworks API so the GPLv3 engine never
// links steam_api64.dll. It starts the game, hands it the name of a named
// pipe through the HEADCUTTER_STEAM_PIPE environment variable, and serves
// simple text commands on that pipe until the game exits.
//
// Protocol (one command per line, '\n' terminated, ASCII):
//   game -> launcher
//     HELLO <version>          handshake; launcher answers with READY lines
//     ACH <api_name>           unlock achievement
//     CLRACH <api_name>        clear achievement (dev builds only)
//     STAT <api_name> <int>    set int stat
//     ADDSTAT <api_name> <int> add to int stat
//     GETSTAT <api_name>       launcher answers "STAT <api_name> <int>"
//     STORE                    push stats/achievements to Steam
//     QUIT                     game is shutting down
//   launcher -> game
//     READY <0|1> <persona>    1 = Steam is up, 0 = offline (commands ignored)
//     OVERLAY <0|1>            Steam overlay opened / closed (pause the game)
//     STAT <api_name> <int>    reply to GETSTAT
//     ERR <text>               bad command

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <share.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "steam/steam_api.h"

#ifndef HC_STEAM_APPID
#define HC_STEAM_APPID 0 // 0 = don't force a restart through Steam (dev)
#endif

static const char *kVersion = "1";
static FILE *gLog = nullptr;

static void Log(const char *fmt, ...)
{
	if (!gLog) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(gLog, fmt, ap);
	va_end(ap);
	fputc('\n', gLog);
	fflush(gLog);
}

static std::wstring ExeDir()
{
	wchar_t buf[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
	std::wstring p(buf, n);
	size_t s = p.find_last_of(L"\\/");
	return s == std::wstring::npos ? L"." : p.substr(0, s);
}

static std::wstring Quote(const std::wstring &s)
{
	if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
	std::wstring r = L"\"";
	for (wchar_t c : s) { if (c == L'"') r += L'\\'; r += c; }
	return r + L"\"";
}

// ---------------------------------------------------------------------------

class Pipe
{
public:
	HANDLE h = INVALID_HANDLE_VALUE;
	OVERLAPPED ov = {};
	bool connected = false;
	std::string inbuf;

	bool Create(const std::wstring &name)
	{
		h = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
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

	void PollConnect()
	{
		if (connected) return;
		DWORD dummy;
		if (GetOverlappedResult(h, &ov, &dummy, FALSE)) connected = true;
	}

	// Non-blocking: returns false once the client is gone.
	bool ReadLines(std::vector<std::string> &out)
	{
		if (!connected) return true;
		for (;;)
		{
			DWORD avail = 0;
			if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;
			if (avail == 0) break;
			char buf[1024];
			DWORD got = 0;
			OVERLAPPED rov = {};
			rov.hEvent = ov.hEvent;
			ResetEvent(rov.hEvent);
			DWORD want = avail < sizeof(buf) ? avail : sizeof(buf);
			if (!ReadFile(h, buf, want, &got, &rov))
			{
				if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(h, &rov, &got, TRUE)) return false;
			}
			inbuf.append(buf, got);
		}
		size_t nl;
		while ((nl = inbuf.find('\n')) != std::string::npos)
		{
			std::string line = inbuf.substr(0, nl);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			inbuf.erase(0, nl + 1);
			if (!line.empty()) out.push_back(line);
		}
		return true;
	}

	void Send(const std::string &line)
	{
		if (!connected) return;
		std::string s = line + "\n";
		DWORD w = 0;
		OVERLAPPED wov = {};
		wov.hEvent = ov.hEvent;
		ResetEvent(wov.hEvent);
		if (!WriteFile(h, s.data(), (DWORD)s.size(), &w, &wov) && GetLastError() == ERROR_IO_PENDING)
			GetOverlappedResult(h, &wov, &w, TRUE);
	}
};

static Pipe gPipe;
static bool gSteam = false;

// Steam callbacks need an object; this one only relays overlay state.
class SteamEvents
{
public:
	STEAM_CALLBACK(SteamEvents, OnOverlay, GameOverlayActivated_t);
};

void SteamEvents::OnOverlay(GameOverlayActivated_t *p)
{
	gPipe.Send(std::string("OVERLAY ") + (p->m_bActive ? "1" : "0"));
}

static bool ValidName(const std::string &s)
{
	if (s.empty() || s.size() > 128) return false;
	for (char c : s)
		if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) return false;
	return true;
}

static void Handle(const std::string &line, bool &quit)
{
	char cmd[16] = {}, name[160] = {};
	long long val = 0;
	int n = sscanf_s(line.c_str(), "%15s %159s %lld", cmd, (unsigned)sizeof(cmd), name, (unsigned)sizeof(name), &val);
	if (n < 1) return;
	std::string c = cmd;
	Log("<- %s", line.c_str());

	if (c == "HELLO")
	{
		std::string persona = gSteam ? SteamFriends()->GetPersonaName() : "";
		for (char &ch : persona) if (ch == '\n' || ch == '\r') ch = ' ';
		gPipe.Send(std::string("READY ") + (gSteam ? "1 " : "0 ") + persona);
		return;
	}
	if (c == "QUIT") { quit = true; return; }
	if (c == "STORE") { if (gSteam) SteamUserStats()->StoreStats(); return; }
	if (n < 2 || !ValidName(name)) { gPipe.Send("ERR bad name"); return; }
	if (!gSteam)
	{
		// Offline: drop writes, but never leave a query unanswered.
		if (c == "GETSTAT") gPipe.Send("ERR offline");
		return;
	}

	if (c == "ACH") SteamUserStats()->SetAchievement(name);
	else if (c == "CLRACH")
	{
#ifdef HC_ALLOW_CLEAR
		SteamUserStats()->ClearAchievement(name);
#else
		gPipe.Send("ERR clear disabled");
#endif
	}
	else if (c == "STAT" && n == 3) SteamUserStats()->SetStat(name, (int32)val);
	else if (c == "ADDSTAT" && n == 3)
	{
		int32 cur = 0;
		SteamUserStats()->GetStat(name, &cur);
		SteamUserStats()->SetStat(name, cur + (int32)val);
	}
	else if (c == "GETSTAT")
	{
		int32 cur = 0;
		if (SteamUserStats()->GetStat(name, &cur)) gPipe.Send("STAT " + std::string(name) + " " + std::to_string(cur));
		else gPipe.Send("ERR no stat " + std::string(name));
	}
	else gPipe.Send("ERR unknown " + c);
}

// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
	std::wstring dir = ExeDir();
	gLog = _wfsopen((dir + L"\\steamlauncher.log").c_str(), L"w", _SH_DENYNO);

#if HC_STEAM_APPID != 0
	if (SteamAPI_RestartAppIfNecessary(HC_STEAM_APPID)) return 0; // Steam relaunches us
#endif

	SteamErrMsg err = {};
	gSteam = SteamAPI_InitEx(&err) == k_ESteamAPIInitResult_OK;
	Log("steam: %s %s", gSteam ? "ok" : "offline", gSteam ? "" : err);
	SteamEvents *events = gSteam ? new SteamEvents : nullptr;

	// Game exe and its arguments: --game <exe> overrides the default, the
	// rest of our command line is passed through unchanged.
	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::wstring game = dir + L"\\HEADCUTTER.exe";
	std::wstring args;
	for (int i = 1; i < argc; i++)
	{
		if (!wcscmp(argv[i], L"--game") && i + 1 < argc) { game = argv[++i]; continue; }
		args += L" " + Quote(argv[i]);
	}
	LocalFree(argv);

	wchar_t pipeName[96];
	swprintf_s(pipeName, L"\\\\.\\pipe\\headcutter_steam_%lu", GetCurrentProcessId());
	if (!gPipe.Create(pipeName)) Log("pipe: create failed %lu", GetLastError());
	SetEnvironmentVariableW(L"HEADCUTTER_STEAM_PIPE", pipeName);

	std::wstring cmdline = Quote(game) + args;
	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(game.c_str(), &cmdline[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
	{
		MessageBoxW(nullptr, (L"Could not start " + game).c_str(), L"Headcutter", MB_ICONERROR);
		if (gSteam) SteamAPI_Shutdown();
		return 1;
	}
	CloseHandle(pi.hThread);
	Log("game started, pid %lu", pi.dwProcessId);

	bool quit = false;
	while (!quit)
	{
		if (WaitForSingleObject(pi.hProcess, 33) == WAIT_OBJECT_0) break;
		if (gSteam) SteamAPI_RunCallbacks();
		gPipe.PollConnect();
		std::vector<std::string> lines;
		if (!gPipe.ReadLines(lines)) { Log("pipe closed"); WaitForSingleObject(pi.hProcess, INFINITE); break; }
		for (auto &l : lines) Handle(l, quit);
	}

	DWORD code = 0;
	WaitForSingleObject(pi.hProcess, 5000);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	if (gSteam)
	{
		SteamUserStats()->StoreStats();
		SteamAPI_RunCallbacks();
		delete events;
		SteamAPI_Shutdown();
	}
	Log("exit %lu", code);
	if (gLog) fclose(gLog);
	return (int)code;
}
