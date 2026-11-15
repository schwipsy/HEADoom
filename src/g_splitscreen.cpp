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
#include "g_splitscreen.h"
#include "d_eventbase.h"
#include "vm.h"
#include "printf.h"
#include "d_protocol.h"
#include "d_event.h"
#include "g_game.h"
#include "actor.h"
#include "p_local.h"
#include "p_checkposition.h"
#include "sbar.h"

uint32_t I_SplitscreenReservedPads = 0;
FSplitFrame gSplitFrame;

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

	// Pitch limits normally arrive over the network for the local player only;
	// without them this player's pitch is clamped to 0.
	player->MinPitch = players[consoleplayer].MinPitch;
	player->MaxPitch = players[consoleplayer].MaxPitch;

	const uint16_t pressed = s.buttons & ~PrevButtons[pad];
	PrevButtons[pad] = s.buttons;

	// In a menu: the controller drives the menu instead (see g_splitmenu.cpp).
	if (G_SplitMenuOpen(int(player - players)))
		return;

	const float lx = StickAxis(s.lx), ly = StickAxis(s.ly);
	const float rx = StickAxis(s.rx), ry = StickAxis(s.ry);

	cmd->buttons |= BT_RUN;
	// G_BuildTiccmd shifts its movement left by 8 before sending, so do the same.
	cmd->forwardmove = (int16_t)clamp(ly * forwardmove[1] * 256.f, -32767.f, 32767.f);
	cmd->sidemove = (int16_t)clamp(lx * sidemove[1] * 256.f, -32767.f, 32767.f);

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
// Moves a freshly joined player next to player 1, so late joiners do not
// appear at the map start (maps usually have only a player 1 start).
// Leaves the player where it spawned if no free visible spot is found.
//
//==========================================================================

static void PlaceBesidePlayer(AActor* mo, AActor* anchor)
{
	if (mo == nullptr || anchor == nullptr || mo == anchor)
		return;

	const double dist = anchor->radius + mo->radius + 16.;
	// Left, right, behind, then the diagonals and front.
	static const double offsets[] = { 90., -90., 180., 135., -135., 45., -45., 0. };
	const DVector3 oldpos = mo->Pos();

	for (double off : offsets)
	{
		const DAngle ang = anchor->Angles.Yaw + DAngle::fromDeg(off);
		const DVector2 pos = anchor->Pos().XY() + ang.ToVector(dist);

		FCheckPosition tm;
		if (!P_CheckPosition(mo, pos, tm))
			continue;
		if (tm.ceilingz - tm.floorz < mo->Height)
			continue;
		if (fabs(tm.floorz - anchor->Z()) > mo->MaxStepHeight)
			continue;

		if (!P_TeleportMove(mo, DVector3(pos.X, pos.Y, tm.floorz), false))
			continue;
		if (!P_CheckSight(anchor, mo, SF_IGNOREVISIBILITY))
		{
			P_TeleportMove(mo, oldpos, false);
			continue;
		}
		mo->Angles.Yaw = anchor->Angles.Yaw;
		mo->ClearInterpolation();
		return;
	}
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
	// Start from player 1's userinfo so per-player mod settings (user cvars) match
	// what player 1 configured, then give this player their own name and color.
	FString info = D_GetUserInfoStrings(consoleplayer, false);
	info.AppendFormat("\\name\\Player %d\\color\\%s", count + 1, colors[count & 3]);

	botskill_t skill = {};
	TArray<uint8_t> buf;
	buf.Resize((unsigned)info.Len() + 1);
	memcpy(buf.Data(), info.GetChars(), info.Len() + 1);
	if (!primaryLevel->BotInfo.DoAddBot(primaryLevel, TArrayView<uint8_t>(buf.Data(), buf.Size()), skill))
		return false;
	if (players[slot].Bot == nullptr)
		return false;

	players[slot].Bot->LocalPad = pad;
	PlaceBesidePlayer(players[slot].mo, players[consoleplayer].mo);
	I_SplitscreenReservedPads |= 1u << pad;
	return true;
}

void G_RemoveSplitscreenPlayer(int pad)
{
	const int pnum = FindSplitscreenPlayer(pad);
	if (pnum < 0 || primaryLevel == nullptr)
		return;
	// Leave like a disconnecting network player. Unlike FCajunMaster::ClearPlayer
	// this keeps the slot's userinfo alive: mods may hold per-player CVar
	// references to it, and destroying them would leave those dangling.
	G_DoPlayerPop(pnum);
	ST_ReleaseSplitStatusBar(pnum);
	if (players[pnum].Bot != nullptr)
	{
		players[pnum].Bot->Destroy();
		players[pnum].Bot = nullptr;
	}
	I_SplitscreenReservedPads &= ~(1u << pad);
}

//==========================================================================
//
// Joining and leaving changes the player list, so it must happen inside the
// game tic. Console commands typed by the user run outside of it, between
// client prediction and its rollback, which would throw the new player away.
// The commands only queue a request; G_Ticker applies it.
//
//==========================================================================

struct FSplitscreenRequest
{
	int pad;
	bool leave;
	bool force;
};
static TArray<FSplitscreenRequest> SplitscreenRequests;

void G_RunSplitscreenRequests()
{
	for (auto& req : SplitscreenRequests)
	{
		if (req.leave)
		{
			if (FindSplitscreenPlayer(req.pad) >= 0)
			{
				G_RemoveSplitscreenPlayer(req.pad);
				Printf("Controller %d left.\n", req.pad + 1);
			}
		}
		else if (G_AddSplitscreenPlayer(req.pad, req.force))
		{
			Printf("Controller %d joined.\n", req.pad + 1);
		}
	}
	SplitscreenRequests.Clear();
}

static void QueueSplitscreenRequest(int pad, bool leave, bool force)
{
	if (pad < 0 || pad >= SPLITPAD_MAX)
	{
		Printf("Controller number must be 1-%d.\n", SPLITPAD_MAX);
		return;
	}
	SplitscreenRequests.Push({ pad, leave, force });
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
		if (pad < 0)
		{
			Printf("No free controller found. Usage: splitscreen_join [controller 1-4]\n");
			return;
		}
	}
	QueueSplitscreenRequest(pad, false, false);
}

CCMD(splitscreen_leave)
{
	if (argv.argc() < 2)
	{
		Printf("Usage: splitscreen_leave <controller 1-4>\n");
		return;
	}
	QueueSplitscreenRequest(atoi(argv[1]) - 1, true, false);
}

// Testing aid: adds a splitscreen player on the given controller slot even if
// nothing is connected there (that player just stands still).
CCMD(splitscreen_debugadd)
{
	QueueSplitscreenRequest(argv.argc() > 1 ? atoi(argv[1]) - 1 : 3, false, true);
}

//==========================================================================
//
// Whether a player is currently playing with a controller: splitscreen
// players always are, player 1 is when their last key press came from one.
// This is local input state, so it must not drive game logic in netgames.
//
//==========================================================================

bool G_PlayerUsesGamepad(int pnum)
{
	if (pnum < 0 || pnum >= MAXPLAYERS || !playeringame[pnum])
		return false;
	if (players[pnum].Bot != nullptr)
		return players[pnum].Bot->LocalPad >= 0;
	return pnum == consoleplayer && I_LastInputWasGamepad;
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, UsingGamepad)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_BOOL(G_PlayerUsesGamepad(int(self - players)));
}

//==========================================================================
//
// Ticks the own status bar of every extra local player, as that player.
//
//==========================================================================

// Number of local splitscreen players (0 when not in splitscreen), for mods and SBARINFO
// (for example: IfCVarInt not splitscreen_players, 2 { ...singleplayer only... }).
CVAR(Int, splitscreen_players, 0, CVAR_NOSET | CVAR_NOSAVE)

void G_TickSplitStatusBars()
{
	int locals[MAXPLAYERS];
	const int count = G_GetLocalPlayers(locals);
	const int splitcount = count > 1 ? count : 0;
	if (splitscreen_players != splitcount)
	{
		UCVarValue val;
		val.Int = splitcount;
		splitscreen_players->ForceSet(val, CVAR_Int);
	}
	if (StatusBar == nullptr)
		return;
	if (count <= 1)
		return;

	DBaseStatusBar* const mainbar = StatusBar;
	const int cp = consoleplayer;
	for (int i = 0; i < count; i++)
	{
		if (locals[i] == cp)
			continue;
		DBaseStatusBar* sb = ST_GetSplitStatusBar(locals[i], mainbar);
		if (sb == nullptr || sb == mainbar)
			continue;
		StatusBar = sb;
		consoleplayer = locals[i];
		sb->CPlayer = &players[locals[i]];
		sb->CallTick();
		consoleplayer = cp;
		StatusBar = mainbar;
	}
}
