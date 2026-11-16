/*
** g_graphicspreset.cpp
**
** Graphics presets for the video menus (vid_graphicspreset)
**
**---------------------------------------------------------------------------
**
** Copyright 2026 HEADoom Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Each preset only touches settings that cost real frame time or memory and
** leaves the game's look alone: the dither, color grading, texture filtering
** mode, dynamic lights and the HUD are never changed. "Max" is the reference
** configuration the game was tuned with.
**
** The render scale steps are 1, 1/2 and 1/3 so every rendered pixel becomes an
** exact block of screen pixels; the dither runs after the upscale, so its
** pattern keeps its size at every scale.
**
** Settings that belong to a mod (destruction quality) are only set when that
** mod is loaded. The blood decal limit follows cl_maxdecals (see g_cvars.cpp).
**
*/

#include "c_cvars.h"
#include "c_dispatch.h"
#include "gamestate.h"
#include "printf.h"
#include "doomstat.h"
#include "d_player.h"
#include "g_game.h"
#include "menu.h"
#include "savegamemanager.h"
#include "v_video.h"
#include "engineerrors.h"
#include "cmdlib.h"
#include "m_misc.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

// From v_video.cpp: called when vid_preferbackend changes while running
extern void (*vid_backendChangedHook)(int backend);
void G_DoSaveGame(bool okForQuicksave, bool forceQuicksave, FString filename, const char *description);
DMenu *CreateMessageBoxMenu(DMenu *parent, const char *message, int messagemode, bool playsound, FName action, void (*handler)());

//==========================================================================
//
// Renderer restart
//
// The renderer can't be switched while the game runs, so changing it offers
// to restart: the game saves to a "rendererswitch" slot, starts a new copy of
// itself with the same command line plus -loadgame, and quits. The player is
// back where they were after a few seconds.
//
//==========================================================================

static bool SuppressBackendPrompt;

// BACKEND_* value of the renderer that is actually running
static int RunningBackend()
{
	if (screen == nullptr) return vid_preferbackend;
	switch (screen->Backend())
	{
	case 0: return BACKEND_OPENGLES;
	case 1: return BACKEND_VULKAN;
	default: return BACKEND_OPENGL;
	}
}

#ifdef _WIN32
static void QuoteArg(std::wstring &out, const wchar_t *arg)
{
	out += L'"';
	for (const wchar_t *p = arg; *p; p++)
	{
		if (*p == L'"') out += L'\\';
		out += *p;
	}
	out += L'"';
}
#endif

static void RestartGame()
{
#ifdef _WIN32
	FString saveFile;
	if (gamestate == GS_LEVEL && !netgame && !demoplayback && usergame &&
		players[consoleplayer].mo != nullptr && players[consoleplayer].health > 0)
	{
		saveFile = G_BuildSaveName("rendererswitch");
		G_DoSaveGame(false, false, saveFile, "Renderer switch");
		if (!FileExists(saveFile)) saveFile = "";
	}
	M_SaveDefaults(nullptr);

	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);

	// Same arguments as this run, minus an old -loadgame
	std::wstring cmd;
	QuoteArg(cmd, exePath);
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 1; argv != nullptr && i < argc; i++)
	{
		if (_wcsicmp(argv[i], L"-loadgame") == 0)
		{
			i++;
			continue;
		}
		cmd += L' ';
		QuoteArg(cmd, argv[i]);
	}
	if (argv != nullptr) LocalFree(argv);
	if (saveFile.IsNotEmpty())
	{
		cmd += L" -loadgame ";
		QuoteArg(cmd, L"rendererswitch");	// -loadgame looks in the save folder
	}

	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(exePath, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
	{
		Printf(TEXTCOLOR_RED "Could not restart the game. Please restart it by hand.\n");
		return;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	throw CExitEvent(0);
#else
	Printf("Please restart the game to use the new renderer.\n");
#endif
}

static const char *PotatoText =
	"POTATO MODE\n\n"
	"The GLES renderer is the game's potato mode. "
	"It runs on almost anything, but some shaders and effects "
	"are lost and the game will look simpler than intended.";

// Shows the restart prompt (and the potato mode warning when GLES is picked)
static void PromptRendererChange(int backend)
{
	if (gamestate == GS_STARTUP)
		return;
	if (menuactive == MENU_Off || CurrentMenu == nullptr)
	{
		if (backend != RunningBackend()) Printf("Changing the renderer requires a restart.\n");
		return;
	}
	const bool needsRestart = backend != RunningBackend();
	const bool potato = backend == BACKEND_OPENGLES;
	if (!needsRestart && !potato)
		return;

	FString text;
	if (potato) text = PotatoText;
	if (needsRestart)
	{
		if (text.IsNotEmpty()) text += "\n\n";
		text += "The renderer change needs a restart. ";
		if (netgame) text += "Restart the game to use it.";
		else text += "Restart now? You will be back where you are.";
	}
	const bool ask = needsRestart && !netgame;
	auto restart = []()
	{
		M_ClearMenus();
		RestartGame();
	};
	DMenu *box = CreateMessageBoxMenu(CurrentMenu, text.GetChars(), ask ? 0 : 1, false, NAME_None, ask ? +restart : nullptr);
	M_ActivateMenu(box);
}

static void OnBackendChanged(int backend)
{
	if (!SuppressBackendPrompt)
		PromptRendererChange(backend);
}

static struct FBackendHookInstaller
{
	FBackendHookInstaller() { vid_backendChangedHook = OnBackendChanged; }
} BackendHookInstaller;

struct FPresetSetting
{
	const char *CVar;
	const char *Values[5];	// Very Low, Low, Medium, High, Max
};

static const FPresetSetting PresetSettings[] =
{
	// 3D render resolution (the HUD stays at full resolution)
	{ "vid_scalefactor",				{ "0.334", "0.5", "0.5", "1", "1" } },
	{ "vid_scale_linear",				{ "0", "0", "0", "0", "0" } },

	// Ambient occlusion: the most expensive effect, but it also sets how dark the
	// game looks, so every preset keeps it (Very Low uses the GLES version)
	{ "gl_ssao",						{ "1", "1", "1", "2", "3" } },
	{ "gl_ssao_portals",				{ "1", "1", "2", "3", "4" } },

	// Dynamic light shadows
	{ "gl_light_shadowmap",				{ "0", "0", "1", "1", "1" } },
	{ "gl_shadowmap_quality",			{ "256", "256", "512", "512", "1024" } },

	// Glow and lens
	{ "gl_bloom",						{ "0", "0", "1", "1", "1" } },
	{ "gl_lens",						{ "0", "0", "1", "1", "1" } },

	// Which things dynamic lights also light
	{ "gl_light_particles",				{ "0", "0", "1", "1", "1" } },
	{ "gl_light_sprites",				{ "0", "1", "1", "1", "1" } },

	// Sprite drop shadows: 0 off, 1 default, 2 always
	{ "r_actorspriteshadow",			{ "0", "1", "2", "2", "2" } },

	// Mirrors seen through another portal (stacked sectors, line portals)
	{ "gl_mirror_nested",				{ "0", "0", "0", "0", "1" } },

	{ "gl_texture_filter_anisotropic",	{ "2", "4", "8", "16", "16" } },

	// Decals and particles
	{ "cl_maxdecals",					{ "1000", "2500", "5000", "7500", "10000" } },
	{ "r_maxparticles",					{ "1250", "2500", "5000", "7500", "10000" } },

	// Destruction effect quality
	{ "dissolve_canvas_quality",		{ "10", "10", "20", "20", "20" } },
	{ "dissolve_patch_density",			{ "10", "10", "20", "20", "20" } },

	// Off at every level; listed so a preset undoes them if they were turned on
	{ "gl_multisample",					{ "1", "1", "1", "1", "1" } },
	{ "gl_fxaa",						{ "0", "0", "0", "0", "0" } },
};

static void ApplyGraphicsPreset(int preset)
{
	if (preset < 1 || preset > 5)
		return;
	for (const auto &setting : PresetSettings)
	{
		FBaseCVar *cvar = FindCVar(setting.CVar, nullptr);
		if (cvar == nullptr)
			continue;
		UCVarValue value;
		value.String = setting.Values[preset - 1];
		cvar->SetGenericRep(value, CVAR_String);
	}

	// Very Low runs on the GLES renderer (the lightest one); the other presets
	// move back to Vulkan if GLES was picked by Very Low. Takes effect on restart.
	const int current = vid_preferbackend;
	const int wanted = preset == 1 ? BACKEND_OPENGLES : (current == BACKEND_OPENGLES ? BACKEND_VULKAN : current);
	if (wanted != current)
	{
		SuppressBackendPrompt = true;
		vid_preferbackend = wanted;
		SuppressBackendPrompt = false;
	}
	// Very Low always shows the potato mode warning, even when already on GLES
	if (wanted != current || preset == 1)
		PromptRendererChange(wanted);
}

// 0 = Custom (nothing applied), 1 = Very Low ... 5 = Max. The defaults of the
// settings it controls are the Medium values.
CUSTOM_CVAR(Int, vid_graphicspreset, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	if (self < 0 || self > 5)
	{
		self = 0;
		return;
	}
	// Loading the config must not overwrite the settings saved with it
	if (gamestate == GS_STARTUP)
		return;
	ApplyGraphicsPreset(self);
}

CCMD(vid_listpresetsettings)
{
	static const char *names[] = { "Very Low", "Low", "Medium", "High", "Max" };
	for (int i = 0; i < 5; i++)
	{
		Printf("%s:\n", names[i]);
		for (const auto &setting : PresetSettings)
			Printf("  %s %s%s\n", setting.CVar, setting.Values[i], FindCVar(setting.CVar, nullptr) ? "" : " (not loaded)");
	}
}
