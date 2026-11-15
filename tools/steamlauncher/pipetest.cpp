// Stand-in for HEADCUTTER.exe to test the launcher's pipe.
// SPDX-License-Identifier: MIT
// Run: HeadcutterSteam.exe --game pipetest.exe ACH_TEST
// Prints every reply to pipetest.log next to the exe.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <share.h>
#include <string>

int main(int argc, char **argv)
{
	FILE *log = _fsopen("pipetest.log", "w", _SH_DENYNO);
	setvbuf(log, nullptr, _IONBF, 0);
	char name[128];
	if (!GetEnvironmentVariableA("HEADCUTTER_STEAM_PIPE", name, sizeof(name))) { fprintf(log, "no pipe env\n"); return 2; }
	HANDLE h = INVALID_HANDLE_VALUE;
	for (int i = 0; i < 50 && h == INVALID_HANDLE_VALUE; i++, Sleep(100))
		h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) { fprintf(log, "connect failed %lu\n", GetLastError()); return 3; }

	auto send = [&](const std::string &s) { DWORD w; std::string l = s + "\n"; WriteFile(h, l.data(), (DWORD)l.size(), &w, nullptr); fprintf(log, "-> %s\n", s.c_str()); };
	auto recv = [&]() { char b[512]; DWORD r = 0; if (ReadFile(h, b, sizeof(b) - 1, &r, nullptr)) { b[r] = 0; fprintf(log, "<- %s", b); } };

	send("HELLO 1"); recv();
	send("GETSTAT NUM_KILLS"); recv();
	if (argc > 1) { send(std::string("ACH ") + argv[1]); send("STORE"); }
	send("ACH b@d"); recv();
	send("QUIT");
	fclose(log);
	CloseHandle(h);
	return 0;
}
