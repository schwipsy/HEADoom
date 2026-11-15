# Headcutter Steam launcher

MIT-licensed helper process. It owns the Steamworks API so the GPLv3 engine
never links `steam_api64.dll` (same model as SelacoLauncherPlugin / steamshim).
Not part of the engine build; never add it to the top-level CMake.

## How it runs

1. Steam starts `HeadcutterSteam.exe` (this launcher; not `Headcutter.exe`, which Windows treats as the same file as `HEADCUTTER.exe`). Set it as the launch option.
2. It initialises Steam, creates `\\.\pipe\headcutter_steam_<pid>` and starts
   `HEADCUTTER.exe` with `HEADCUTTER_STEAM_PIPE` set to that name. All other
   command-line arguments pass through; `--game <exe>` overrides the exe.
3. It pumps Steam callbacks and serves pipe commands until the game exits,
   then stores stats and shuts Steam down.
4. If Steam is not available, the game still runs (`READY 0`); writes are
   dropped and queries answer `ERR offline`.

The protocol is documented at the top of `steamlauncher.cpp`. Every query
gets exactly one reply; writes reply only with `ERR` on bad input.

## Build

```
cmake -S tools/steamlauncher -B build-steam -G Ninja -DCMAKE_BUILD_TYPE=Release -DHC_STEAM_APPID=<appid>
cmake --build build-steam
```

`STEAMWORKS_SDK` defaults to `S:/Z-GameDev/GzdoomGame/STEAM/steamworks/sdk`.
The SDK is proprietary: never commit it. Ship `HeadcutterSteam.exe` and
`steam_api64.dll` next to `HEADCUTTER.exe`.

`HC_STEAM_APPID=0` (default) skips `SteamAPI_RestartAppIfNecessary`; for local
tests put `steam_appid.txt` next to the exe (480 = Valve's Spacewar test app).

## Test

```
HeadcutterSteam.exe --game pipetest.exe ACH_WIN_ONE_GAME
```

Writes `pipetest.log` and `steamlauncher.log`.
