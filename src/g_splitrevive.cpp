//
// Local splitscreen co-op: reviving downed partners.
//
// While another local player is alive, a dead player cannot respawn on their own
// (PlayerPawn.DeathThink checks splitscreen_players). A living player revives them
// by holding Use within reach of the body for SPLITREVIVE_TICS; they get up where
// they fell with SPLITREVIVE_HEALTH. When every local player is down, Use loads the
// last save like in singleplayer.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "d_player.h"
#include "d_event.h"
#include "doomstat.h"
#include "g_levellocals.h"
#include "g_splitscreen.h"
#include "actor.h"
#include "actorinlines.h"
#include "c_cvars.h"
#include "v_draw.h"
#include "v_font.h"
#include "v_text.h"
#include "printf.h"
#include "vm.h"

int G_GetLocalPlayers(int out[MAXPLAYERS]);

CVAR(Int, splitscreen_revive_seconds, 6, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, splitscreen_revive_health, 25, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, splitscreen_revive_range, 96.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

static int ReviveProgress[MAXPLAYERS];	// tics of reviving done on this (dead) player
static int ReviveBy[MAXPLAYERS];		// player reviving them, -1 if none
static int ReviveTarget[MAXPLAYERS];	// for living players: the downed partner in reach, -1 if none

static int ReviveTics()
{
	return clamp<int>(splitscreen_revive_seconds, 1, 30) * TICRATE;
}

static bool IsDown(int pnum)
{
	return playeringame[pnum] && players[pnum].mo != nullptr && players[pnum].playerstate == PST_DEAD;
}

static bool IsUp(int pnum)
{
	return playeringame[pnum] && players[pnum].mo != nullptr && players[pnum].playerstate == PST_LIVE && players[pnum].health > 0;
}

//==========================================================================
//
// Per tic (only while the game runs).
//
//==========================================================================

void G_TickSplitRevive()
{
	int locals[MAXPLAYERS];
	const int count = G_GetLocalPlayers(locals);
	for (int i = 0; i < MAXPLAYERS; i++)
		ReviveTarget[i] = -1;
	if (count <= 1)
	{
		for (int i = 0; i < MAXPLAYERS; i++) { ReviveProgress[i] = 0; ReviveBy[i] = -1; }
		return;
	}

	const double range = splitscreen_revive_range;
	for (int d = 0; d < count; d++)
	{
		const int down = locals[d];
		if (!IsDown(down))
		{
			ReviveProgress[down] = 0;
			ReviveBy[down] = -1;
			continue;
		}

		// closest living partner in reach
		int best = -1;
		double bestdist = range;
		for (int a = 0; a < count; a++)
		{
			const int up = locals[a];
			if (!IsUp(up) || players[up].mo->Level != players[down].mo->Level)
				continue;
			const double dist = players[up].mo->Distance3D(players[down].mo);
			if (dist <= bestdist)
			{
				bestdist = dist;
				best = up;
			}
		}

		if (best >= 0)
			ReviveTarget[best] = down;
		if (best >= 0 && (players[best].cmd.buttons & BT_USE))
		{
			ReviveBy[down] = best;
			if (++ReviveProgress[down] >= ReviveTics())
			{
				player_t* p = &players[down];
				if (p->Resurrect())
				{
					const int hp = clamp<int>(splitscreen_revive_health, 1, 1000);
					p->health = p->mo->health = hp;
					Printf("%s was revived by %s.\n", p->userinfo.GetName(), players[best].userinfo.GetName());
				}
				ReviveProgress[down] = 0;
				ReviveBy[down] = -1;
			}
		}
		else
		{
			ReviveBy[down] = -1;
			if (ReviveProgress[down] > 0) ReviveProgress[down]--;	// drains back slowly when let go
		}
	}
}

//==========================================================================
//
// Prompts and progress, drawn inside player pnum's view (twod is sized to it).
//
//==========================================================================

static void DrawCentered(const char* text, double y, double scale, int color)
{
	const double w = SmallFont->StringWidth(text) * scale;
	DrawText(twod, SmallFont, color, (twod->GetWidth() - w) / 2, y, text, DTA_ScaleX, scale, DTA_ScaleY, scale, TAG_DONE);
}

static void DrawBar(double y, double scale, double frac)
{
	const int w = int(twod->GetWidth() * 0.3);
	const int h = max(2, int(4 * scale));
	const int x = (twod->GetWidth() - w) / 2;
	Dim(twod, 0, 0.6f, x - 1, int(y) - 1, w + 2, h + 2);
	Dim(twod, PalEntry(255, 14, 146, 238), 1.f, x, int(y), int(w * clamp(frac, 0., 1.)), h);
}

void G_DrawSplitReviveHud(int pnum)
{
	if (gSplitFrame.count <= 1 || pnum < 0 || pnum >= MAXPLAYERS || !playeringame[pnum])
		return;
	const double scale = max(1., twod->GetHeight() / 270.);
	const double y = twod->GetHeight() * 0.62;
	const bool usingPad = players[pnum].Bot != nullptr;	// extra splitscreen players always use a controller
	const char* useKey = usingPad ? "Y" : "Use";

	if (IsDown(pnum))
	{
		// (consoleplayer is swapped while the HUD is drawn, so use the drawn views as the player list)
		bool partnerUp = false;
		for (int i = 0; i < gSplitFrame.count; i++)
			if (gSplitFrame.player[i] != pnum && IsUp(gSplitFrame.player[i])) partnerUp = true;

		if (!partnerUp)
		{
			FString msg;
			msg.Format("Everyone is down. Press %s to load the last save", useKey);
			DrawCentered(msg.GetChars(), y, scale, CR_RED);
		}
		else if (ReviveBy[pnum] >= 0 || ReviveProgress[pnum] > 0)
		{
			DrawCentered("Being revived...", y, scale, CR_WHITE);
			DrawBar(y + SmallFont->GetHeight() * scale + 4, scale, ReviveProgress[pnum] / double(ReviveTics()));
		}
		else
			DrawCentered("You are down. Wait for a partner to revive you", y, scale, CR_WHITE);
		return;
	}

	const int target = ReviveTarget[pnum];
	if (target >= 0)
	{
		FString msg;
		if (ReviveBy[target] == pnum)
			msg.Format("Reviving %s...", players[target].userinfo.GetName());
		else
			msg.Format("Hold %s to revive %s", useKey, players[target].userinfo.GetName());
		DrawCentered(msg.GetChars(), y, scale, CR_WHITE);
		if (ReviveProgress[target] > 0)
			DrawBar(y + SmallFont->GetHeight() * scale + 4, scale, ReviveProgress[target] / double(ReviveTics()));
	}
}

// Testing: splitscreen_debugkill <player 1-4>
#include "c_dispatch.h"
#include "p_local.h"
CCMD(splitscreen_debugkill)
{
	const int pnum = argv.argc() > 1 ? atoi(argv[1]) - 1 : -1;
	if (pnum < 0 || pnum >= MAXPLAYERS || !IsUp(pnum))
		return;
	P_DamageMobj(players[pnum].mo, nullptr, nullptr, TELEFRAG_DAMAGE, NAME_None);
}

// Testing: splitscreen_give <player 1-4> <item> [amount]
CCMD(splitscreen_give)
{
	if (argv.argc() < 3)
	{
		Printf("Usage: splitscreen_give <player 1-4> <item> [amount]\n");
		return;
	}
	const int pnum = atoi(argv[1]) - 1;
	if (pnum < 0 || pnum >= MAXPLAYERS || !playeringame[pnum] || players[pnum].mo == nullptr)
		return;
	PClassActor* cls = PClass::FindActor(argv[2]);
	if (cls == nullptr)
	{
		Printf("Unknown item '%s'\n", argv[2]);
		return;
	}
	const int amount = argv.argc() > 3 ? atoi(argv[3]) : 1;
	IFVM(Actor, GiveInventory)
	{
		VMValue params[] = { players[pnum].mo, cls, amount, false };
		VMCall(func, params, 4, nullptr, 0);
	}
}
