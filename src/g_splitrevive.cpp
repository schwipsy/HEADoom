//
// Local splitscreen co-op: reviving downed partners.
//
// While another local player is alive, a dead player cannot respawn on their own
// (PlayerPawn.DeathThink checks splitscreen_players). A living player revives them
// by holding Use within reach of the body for SPLITREVIVE_TICS; they get up where
// they fell with SPLITREVIVE_HEALTH. When every local player is down, Use loads the
// last save like in singleplayer.
//
// Online co-op works the same way (sv_coopreviving, on by default): a dead player
// waits for a partner, and when everyone is down Use loads the last save on every
// PC (or restarts the map if there is none). The revive settings are server cvars
// there, so the host's values apply.
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

#include "g_game.h"
#include "cmdlib.h"
#include "resourcefile.h"
#include "serializer.h"
#include <memory>

extern FString BackupSaveName;

// True if the save is a co-op save made with exactly the players in this game.
static bool IsSaveForThisCoopGame(const FString& file)
{
	if (file.Len() == 0 || !FileExists(file))
		return false;
	std::unique_ptr<FResourceFile> res(FResourceFile::OpenResourceFile(file.GetChars(), true));
	if (res == nullptr)
		return false;
	const auto entry = res->FindEntry("info.json");
	if (entry < 0)
		return false;
	auto data = res->Read(entry);
	FSerializer arc;
	if (!arc.OpenReader(data.string(), data.size()))
		return false;
	int saved = 0;
	arc("CoopPlayers", saved);
	int now = 0;
	for (int i = 0; i < MAXPLAYERS; i++) if (playeringame[i]) now |= 1 << i;
	return saved != 0 && saved == now;
}
extern FString savename;

CVAR(Int, splitscreen_revive_seconds, 6, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_SERVERINFO)
CVAR(Int, splitscreen_revive_health, 25, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_SERVERINFO)
CVAR(Float, splitscreen_revive_range, 96.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_SERVERINFO)
CVAR(Bool, sv_coopreviving, true, CVAR_ARCHIVE | CVAR_SERVERINFO)	// online co-op: revive partners, reload when everyone is down
// HUD: 1 while this PC's player is down and a partner can still revive them (SBARINFO hides the death screen)
CVAR(Int, hud_coopwaitrevive, 0, CVAR_NOSET | CVAR_NOSAVE)

static int ReviveProgress[MAXPLAYERS];	// tics of reviving done on this (dead) player
static int ReviveBy[MAXPLAYERS];		// player reviving them, -1 if none
static int ReviveTarget[MAXPLAYERS];	// for living players: the downed partner in reach, -1 if none

// Everyone who takes part: all players online, the local players in splitscreen.
static int GetCoopPlayers(int out[MAXPLAYERS])
{
	if (!netgame)
		return G_GetLocalPlayers(out);
	int count = 0;
	for (int i = 0; i < MAXPLAYERS; i++)
		if (playeringame[i]) out[count++] = i;
	return count;
}

bool G_CoopReviveActive()
{
	if (deathmatch || (netgame && !sv_coopreviving))
		return false;
	int list[MAXPLAYERS];
	return GetCoopPlayers(list) > 1;
}

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
	const int count = GetCoopPlayers(locals);
	for (int i = 0; i < MAXPLAYERS; i++)
		ReviveTarget[i] = -1;
	if (!G_CoopReviveActive())
	{
		for (int i = 0; i < MAXPLAYERS; i++) { ReviveProgress[i] = 0; ReviveBy[i] = -1; }
		if (hud_coopwaitrevive != 0) { UCVarValue v; v.Int = 0; hud_coopwaitrevive->ForceSet(v, CVAR_Int); }
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

	// local HUD state only (not part of the game)
	bool partnerUp = false;
	for (int a = 0; a < count; a++)
		if (locals[a] != consoleplayer && IsUp(locals[a])) partnerUp = true;
	const int wait = IsDown(consoleplayer) && partnerUp ? 1 : 0;
	if (hud_coopwaitrevive != wait)
	{
		UCVarValue v; v.Int = wait;
		hud_coopwaitrevive->ForceSet(v, CVAR_Int);
	}
}

//==========================================================================
//
// Called from DoReborn for a player who pressed Use while dead. Online, when
// nobody is up any more, loads the last save on every PC (all PCs save the same
// games, so every PC has it) or restarts the map. Returns true if it handled it.
//
//==========================================================================

bool G_CoopAllDownReload(FLevelLocals* Level, int playernum)
{
	if (!netgame || !G_CoopReviveActive() || players[playernum].playerstate != PST_REBORN)
		return false;
	int list[MAXPLAYERS];
	const int count = GetCoopPlayers(list);
	for (int i = 0; i < count; i++)
		if (IsUp(list[i])) return false;

	players[playernum].playerstate = PST_DEAD;	// stays down until the load / restart happens
	if (gameaction != ga_nothing)
		return true;
	// Only a co-op save with these same players (from today or another day); otherwise restart the map.
	if (IsSaveForThisCoopGame(BackupSaveName))
	{
		savename = BackupSaveName;
		gameaction = ga_autoloadgame;
	}
	else
	{
		Level->ChangeLevel(Level->MapName.GetChars(), 0, CHANGELEVEL_NOINTERMISSION | CHANGELEVEL_RESETINVENTORY | CHANGELEVEL_RESETHEALTH, -1);
	}
	return true;
}

bool G_AnyCoopPlayerAlive()
{
	for (int i = 0; i < MAXPLAYERS; i++)
		if (IsUp(i)) return true;
	return false;
}

//==========================================================================
//
// Checkpoints: when a living player triggers an autosave (ACS Autosave() or
// the Autosave line special), the bodies of downed partners are brought next
// to them, so they can be revived there (and the save has them there too).
//
//==========================================================================

bool G_FindSpotNear(AActor* orig, DVector3& out);

void G_CoopCheckpoint(AActor* activator)
{
	if (activator == nullptr || activator->player == nullptr || !G_CoopReviveActive())
		return;
	const int who = int(activator->player - players);
	if (!IsUp(who))
		return;
	int list[MAXPLAYERS];
	const int count = GetCoopPlayers(list);
	for (int i = 0; i < count; i++)
	{
		const int down = list[i];
		if (!IsDown(down) || players[down].mo->Level != activator->Level)
			continue;
		AActor* body = players[down].mo;
		DVector3 pos;
		if (!G_FindSpotNear(activator, pos))
			pos = activator->Pos();
		body->Vel.Zero();
		body->SetOrigin(pos, false);
		body->floorz = activator->floorz;	// refined by the next movement tick
		body->ClearInterpolation();
		ReviveProgress[down] = 0;
		ReviveBy[down] = -1;
		Printf("%s was brought to the checkpoint.\n", players[down].userinfo.GetName());
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
	if (!G_CoopReviveActive() || pnum < 0 || pnum >= MAXPLAYERS || !playeringame[pnum])
		return;
	const double scale = max(1., twod->GetHeight() / 270.);
	const double y = twod->GetHeight() * 0.62;
	const bool usingPad = players[pnum].Bot != nullptr;	// extra splitscreen players always use a controller
	const char* useKey = usingPad ? "Y" : "Use";

	if (IsDown(pnum))
	{
		bool partnerUp = false;
		for (int i = 0; i < MAXPLAYERS; i++)
			if (i != pnum && IsUp(i)) partnerUp = true;

		if (!partnerUp)
		{
			if (netgame && gSplitFrame.count <= 1)
				return;	// the status bar shows the death screen (Use loads the last save)
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

// Debug / testing: splitscreen_printinv <player 1-4>
CCMD(splitscreen_printinv)
{
	const int pnum = argv.argc() > 1 ? atoi(argv[1]) - 1 : consoleplayer;
	if (pnum < 0 || pnum >= MAXPLAYERS || !playeringame[pnum] || players[pnum].mo == nullptr)
		return;
	auto mo = players[pnum].mo;
	Printf("Player %d: health %d\n", pnum + 1, mo->health);
	for (AActor* item = mo->Inventory; item != nullptr; item = item->Inventory)
		Printf("  %s %d/%d\n", item->GetClass()->TypeName.GetChars(), item->IntVar(NAME_Amount), item->IntVar(NAME_MaxAmount));
}
