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
** Settings that belong to a mod (blood decals, destruction quality) are only
** set when that mod is loaded.
**
*/

#include "c_cvars.h"
#include "c_dispatch.h"
#include "gamestate.h"
#include "printf.h"

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

	{ "gl_texture_filter_anisotropic",	{ "2", "4", "8", "16", "16" } },

	// Decals and particles
	{ "cl_maxdecals",					{ "400", "1000", "2000", "4000", "9999" } },
	{ "r_maxparticles",					{ "1000", "2000", "4000", "6000", "10000" } },
	{ "ug_decal_max",					{ "100", "300", "1000", "1800", "3000" } },

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
	FBaseCVar *backend = FindCVar("vid_preferbackend", nullptr);
	if (backend != nullptr)
	{
		const int current = backend->GetGenericRep(CVAR_Int).Int;
		const int wanted = preset == 1 ? 2 : (current == 2 ? 1 : current);
		if (wanted != current)
		{
			UCVarValue value;
			value.Int = wanted;
			backend->SetGenericRep(value, CVAR_Int);
			Printf("The renderer change takes effect after restarting the game.\n");
		}
	}
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
