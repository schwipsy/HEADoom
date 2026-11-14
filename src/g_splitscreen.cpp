//
// Local splitscreen co-op.
//
// Extra local players are regular player slots that use the bot plumbing to
// get their per-tic commands, but instead of the bot AI their commands are
// built from one specific gamepad that is read directly (i_splitpad.h).
// Player 1 keeps the normal keyboard/mouse/gamepad input; controllers that
// belong to the other players are ignored by the normal input system.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <math.h>

#include "c_dispatch.h"
#include "d_player.h"
#include "d_netinf.h"
#include "doomstat.h"
#include "g_levellocals.h"
#include "b_bot.h"
#include "i_splitpad.h"
#include "printf.h"
#include "d_protocol.h"
#include "d_event.h"
#include "g_game.h"

uint32_t I_SplitscreenReservedPads = 0;

#ifndef _WIN32
bool I_GetSplitPadState(int pad, FSplitPadState& out)
{
	out = {};
	return false; // TODO: SDL game controller path for Linux/macOS
}
#endif

extern int forwardmove[2], sidemove[2];
void SelectWeapon(int player, int slot);

CVAR(Float, splitscreen_deadzone, 0.24f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, splitscreen_lookspeed, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, splitscreen_invertlook, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

static uint16_t PrevButtons[SPLITPAD_MAX];

static float StickAxis(int16_t raw)
{
	const float dz = clamp((float)splitscreen_deadzone, 0.f, 0.9f);
	float v = raw / 32767.f;
	const float mag = fabsf(v);
	if (mag <= dz)
		return 0.f;
	v = copysignf((mag - dz) / (1.f - dz), v);
	return clamp(v, -1.f, 1.f);
}

//==========================================================================
//
// Builds one tic of input for a splitscreen player from its controller.
// Default layout (Xbox names):
//   left stick move, right stick look, RT fire, LT alt-fire, A jump,
//   B crouch, X use, Y reload, RB / LB next / previous weapon,
//   right stick click zoom.
//
//==========================================================================

void G_BuildSplitscreenCmd(int pad, player_t* player, usercmd_t* cmd)
{
	if (pad < 0 || pad >= SPLITPAD_MAX)
		return;

	FSplitPadState s;
	if (!I_GetSplitPadState(pad, s) || !s.connected)
	{
		PrevButtons[pad] = 0;
		return;
	}

	const uint16_t pressed = s.buttons & ~PrevButtons[pad];
	PrevButtons[pad] = s.buttons;

	const float lx = StickAxis(s.lx), ly = StickAxis(s.ly);
	const float rx = StickAxis(s.rx), ry = StickAxis(s.ry);

	cmd->buttons |= BT_RUN;
	cmd->forwardmove = (int16_t)(ly * forwardmove[1]);
	cmd->sidemove = (int16_t)(lx * sidemove[1]);

	// Same scale as the engine's analog look (ANALOG_LOOK_BASE = 1280 per tic at full tilt).
	const float look = 1280.f * splitscreen_lookspeed;
	cmd->yaw = (int16_t)clamp(-rx * fabsf(rx) * look, -32767.f, 32767.f);
	cmd->pitch = (int16_t)clamp((splitscreen_invertlook ? -ry : ry) * fabsf(ry) * look * 0.6f, -32767.f, 32767.f);

	if (s.rt > 64) cmd->buttons |= BT_ATTACK;
	if (s.lt > 64) cmd->buttons |= BT_ALTATTACK;
	if (s.buttons & SPLITPAD_A) cmd->buttons |= BT_JUMP;
	if (s.buttons & SPLITPAD_B) cmd->buttons |= BT_CROUCH;
	if (s.buttons & SPLITPAD_X) cmd->buttons |= BT_USE;
	if (s.buttons & SPLITPAD_Y) cmd->buttons |= BT_RELOAD;
	if (s.buttons & SPLITPAD_RTHUMB) cmd->buttons |= BT_ZOOM;

	const int pnum = int(player - players);
	if (pressed & SPLITPAD_RB) SelectWeapon(pnum, WST_NEXT);
	if (pressed & SPLITPAD_LB) SelectWeapon(pnum, WST_PREV);

	// Dead: fire, jump or use respawns.
	if (player->playerstate == PST_DEAD && (s.rt > 64 || (s.buttons & (SPLITPAD_A | SPLITPAD_X))))
		cmd->buttons |= BT_USE;
}

//==========================================================================
//
// Local players: player 1 (consoleplayer) plus every splitscreen player.
//
//==========================================================================

int G_GetLocalPlayers(int out[MAXPLAYERS])
{
	int count = 0;
	out[count++] = consoleplayer;
	for (int i = 0; i < MAXPLAYERS; i++)
	{
		if (i != consoleplayer && playeringame[i] && players[i].Bot != nullptr && players[i].Bot->LocalPad >= 0)
			out[count++] = i;
	}
	return count;
}

static int FindSplitscreenPlayer(int pad)
{
	for (int i = 0; i < MAXPLAYERS; i++)
	{
		if (playeringame[i] && players[i].Bot != nullptr && players[i].Bot->LocalPad == pad)
			return i;
	}
	return -1;
}

//==========================================================================
//
// Adds a splitscreen player driven by controller 'pad'.
//
//==========================================================================

bool G_AddSplitscreenPlayer(int pad, bool force = false)
{
	if (gamestate != GS_LEVEL || netgame || primaryLevel == nullptr)
	{
		Printf("Splitscreen players can only join a local game in progress.\n");
		return false;
	}
	if (pad < 0 || pad >= SPLITPAD_MAX)
		return false;
	if (FindSplitscreenPlayer(pad) >= 0)
	{
		Printf("Controller %d is already playing.\n", pad + 1);
		return false;
	}
	FSplitPadState s;
	if (!force && (!I_GetSplitPadState(pad, s) || !s.connected))
	{
		Printf("Controller %d is not connected.\n", pad + 1);
		return false;
	}

	int locals[MAXPLAYERS];
	const int count = G_GetLocalPlayers(locals);
	if (count >= 4)
	{
		Printf("Splitscreen supports up to 4 players.\n");
		return false;
	}

	int slot = -1;
	for (int i = 0; i < MAXPLAYERS; i++)
	{
		if (!playeringame[i])
		{
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return false;

	static const char* const colors[] = { "40 cf 00", "bf 00 00", "00 00 ff", "ff ff 00" };
	FString info;
	info.Format("\\name\\Player %d\\color\\%s", count + 1, colors[count & 3]);

	botskill_t skill = {};
	TArray<uint8_t> buf;
	buf.Resize((unsigned)info.Len() + 1);
	memcpy(buf.Data(), info.GetChars(), info.Len() + 1);
	if (!primaryLevel->BotInfo.DoAddBot(primaryLevel, TArrayView<uint8_t>(buf.Data(), buf.Size()), skill))
		return false;
	if (players[slot].Bot == nullptr)
		return false;

	players[slot].Bot->LocalPad = pad;
	I_SplitscreenReservedPads |= 1u << pad;
	return true;
}

void G_RemoveSplitscreenPlayer(int pad)
{
	const int pnum = FindSplitscreenPlayer(pad);
	if (pnum < 0 || primaryLevel == nullptr)
		return;
	primaryLevel->BotInfo.ClearPlayer(pnum, false);
	I_SplitscreenReservedPads &= ~(1u << pad);
}

CCMD(splitscreen_join)
{
	int pad = -1;
	if (argv.argc() > 1)
	{
		pad = atoi(argv[1]) - 1;
	}
	else
	{
		// First connected controller that is not taken yet.
		for (int i = 0; i < SPLITPAD_MAX && pad < 0; i++)
		{
			FSplitPadState s;
			if (FindSplitscreenPlayer(i) < 0 && I_GetSplitPadState(i, s) && s.connected)
				pad = i;
		}
	}
	if (pad < 0)
	{
		Printf("No free controller found. Usage: splitscreen_join [controller 1-4]\n");
		return;
	}
	if (G_AddSplitscreenPlayer(pad))
		Printf("Controller %d joined.\n", pad + 1);
}

CCMD(splitscreen_leave)
{
	if (argv.argc() < 2)
	{
		Printf("Usage: splitscreen_leave <controller 1-4>\n");
		return;
	}
	G_RemoveSplitscreenPlayer(atoi(argv[1]) - 1);
}

// Testing aid: adds a splitscreen player on the given controller slot even if
// nothing is connected there (that player just stands still).
CCMD(splitscreen_debugadd)
{
	const int pad = argv.argc() > 1 ? atoi(argv[1]) - 1 : 3;
	if (G_AddSplitscreenPlayer(pad, true))
		Printf("Debug splitscreen player added on controller slot %d.\n", pad + 1);
}
