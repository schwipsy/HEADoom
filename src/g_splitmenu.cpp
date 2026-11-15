//
// Local splitscreen co-op: menus for the extra local players.
//
// The engine has one menu stack (CurrentMenu), driven by keyboard / mouse and drawn over the
// whole screen. Gameplay menus (anything that is not a system menu such as the options or the
// main menu) can also be opened for an extra local player: each of those players gets an own
// menu instance that is drawn inside their view and driven by their controller. While such a
// menu runs, CurrentMenu, menuactive and consoleplayer are swapped to that player, so menu code
// that reads players[consoleplayer] sees its own player, and the network events it sends are
// applied to that player.
//
// In splitscreen a gameplay menu only pauses the game when every local player has one open.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "menu.h"
#include "menustate.h"
#include "d_player.h"
#include "doomstat.h"
#include "b_bot.h"
#include "i_splitpad.h"
#include "g_splitscreen.h"
#include "g_levellocals.h"
#include "events.h"
#include "vm.h"

int G_GetLocalPlayers(int out[MAXPLAYERS]);

static DMenu* SplitMenus[MAXPLAYERS];
static uint16_t SplitMenuButtons[MAXPLAYERS];
static int SplitMenuRepeatKey[MAXPLAYERS];
static int SplitMenuRepeatTics[MAXPLAYERS];
int G_SplitMenuSender = -1;	// player whose splitscreen menu code is running, -1 if none

struct FSplitNetEvent
{
	int player;
	FString name;
	int args[3];
	bool manual;
};
static TArray<FSplitNetEvent> SplitNetEvents;

//==========================================================================
//
// Runs code with the menu state swapped to player pnum's splitscreen menu.
//
//==========================================================================

struct FSplitMenuContext
{
	int pnum;
	DMenu* savedMenu;
	EMenuState savedActive;
	int savedConsole;
	int savedSender;

	FSplitMenuContext(int p) : pnum(p), savedMenu(CurrentMenu), savedActive(menuactive), savedConsole(consoleplayer), savedSender(G_SplitMenuSender)
	{
		CurrentMenu = SplitMenus[pnum];
		menuactive = CurrentMenu != nullptr ? MENU_On : MENU_Off;
		consoleplayer = pnum;
		G_SplitMenuSender = pnum;
	}
	~FSplitMenuContext()
	{
		SplitMenus[pnum] = menuactive != MENU_Off ? CurrentMenu : nullptr;
		if (SplitMenus[pnum] != nullptr)
			GC::WriteBarrier(SplitMenus[pnum]);
		CurrentMenu = savedMenu;
		menuactive = savedActive;
		consoleplayer = savedConsole;
		G_SplitMenuSender = savedSender;
	}
};

static void MarkSplitMenus()
{
	for (auto& m : SplitMenus)
		GC::Mark(m);
}

static bool IsExtraLocalPlayer(int pnum)
{
	if (pnum < 0 || pnum >= MAXPLAYERS || pnum == consoleplayer || !playeringame[pnum])
		return false;
	return players[pnum].Bot != nullptr && players[pnum].Bot->LocalPad >= 0;
}

// System menus (options, main menu, save / load, message boxes ...) keep their usual behaviour.
bool G_IsGameplayMenu(DMenu* menu)
{
	if (menu == nullptr)
		return false;
	return !menu->IsKindOf("OptionMenu") && !menu->IsKindOf("ListMenu")
		&& !menu->IsKindOf("MessageBoxMenu") && !menu->IsKindOf("ImageScrollerMenu")
		&& !menu->IsKindOf("TextEnterMenu");
}

bool G_SplitMenuOpen(int pnum)
{
	return pnum >= 0 && pnum < MAXPLAYERS && SplitMenus[pnum] != nullptr;
}

//==========================================================================
//
// Opens a gameplay menu for a local player: the normal menu for the
// console player, an own instance for an extra splitscreen player.
//
//==========================================================================

void G_OpenMenuForPlayer(FName name, int pnum)
{
	static bool marker = false;
	if (!marker)
	{
		marker = true;
		GC::AddMarkerFunc(MarkSplitMenus);
	}

	if (pnum == consoleplayer)
	{
		if (CurrentMenu == nullptr)
			M_SetMenu(name, 0);
		return;
	}
	if (!IsExtraLocalPlayer(pnum) || SplitMenus[pnum] != nullptr)
		return;

	// Buttons already held when it opens must not count as presses.
	FSplitPadState s;
	SplitMenuButtons[pnum] = (I_GetSplitPadState(players[pnum].Bot->LocalPad, s) && s.connected) ? s.buttons : 0;
	SplitMenuRepeatKey[pnum] = -1;

	FSplitMenuContext ctx(pnum);
	M_SetMenu(name, 0);
}

DEFINE_ACTION_FUNCTION(DMenu, SetMenuForPlayer)
{
	PARAM_PROLOGUE;
	PARAM_NAME(name);
	PARAM_INT(pnum);
	G_OpenMenuForPlayer(name, pnum);
	return 0;
}

//==========================================================================
//
// Per tic: controller input and Ticker() of every splitscreen menu.
//
//==========================================================================

enum { SPLITMENU_REPEAT_DELAY = 12, SPLITMENU_REPEAT_RATE = 4 };

void G_SplitMenuTicker()
{
	for (int pnum = 0; pnum < MAXPLAYERS; pnum++)
	{
		if (SplitMenus[pnum] == nullptr)
			continue;

		if (!IsExtraLocalPlayer(pnum))
		{
			FSplitMenuContext ctx(pnum);
			M_ClearMenus();
			continue;
		}

		FSplitPadState s;
		uint16_t buttons = 0;
		if (I_GetSplitPadState(players[pnum].Bot->LocalPad, s) && s.connected)
		{
			buttons = s.buttons;
			// left stick works like the d-pad
			if (s.ly > 16000) buttons |= SPLITPAD_DPAD_UP;
			if (s.ly < -16000) buttons |= SPLITPAD_DPAD_DOWN;
			if (s.lx < -16000) buttons |= SPLITPAD_DPAD_LEFT;
			if (s.lx > 16000) buttons |= SPLITPAD_DPAD_RIGHT;
		}
		const uint16_t pressed = buttons & ~SplitMenuButtons[pnum];
		SplitMenuButtons[pnum] = buttons;

		static const struct { uint16_t bit; int mkey; bool repeat; } map[] =
		{
			{ SPLITPAD_DPAD_UP, MKEY_Up, true },
			{ SPLITPAD_DPAD_DOWN, MKEY_Down, true },
			{ SPLITPAD_DPAD_LEFT, MKEY_Left, true },
			{ SPLITPAD_DPAD_RIGHT, MKEY_Right, true },
			{ SPLITPAD_LB, MKEY_PageUp, false },
			{ SPLITPAD_RB, MKEY_PageDown, false },
			{ SPLITPAD_A, MKEY_Enter, false },
			{ SPLITPAD_B, MKEY_Back, false },
			{ SPLITPAD_BACK, MKEY_Back, false },
			{ SPLITPAD_START, MKEY_Back, false },
		};

		TArray<int> events;
		for (auto& m : map)
		{
			if (pressed & m.bit)
			{
				events.Push(m.mkey);
				if (m.repeat)
				{
					SplitMenuRepeatKey[pnum] = m.mkey;
					SplitMenuRepeatTics[pnum] = SPLITMENU_REPEAT_DELAY;
				}
			}
		}
		// held direction: auto repeat
		const int rk = SplitMenuRepeatKey[pnum];
		if (rk >= 0)
		{
			bool held = false;
			for (auto& m : map)
				if (m.mkey == rk && m.repeat && (buttons & m.bit)) held = true;
			if (!held)
				SplitMenuRepeatKey[pnum] = -1;
			else if (--SplitMenuRepeatTics[pnum] <= 0)
			{
				SplitMenuRepeatTics[pnum] = SPLITMENU_REPEAT_RATE;
				events.Push(rk);
			}
		}

		FSplitMenuContext ctx(pnum);
		if (CurrentMenu != nullptr && menuactive != MENU_Off)
			CurrentMenu->CallTicker();
		for (int mkey : events)
		{
			if (CurrentMenu == nullptr || menuactive == MENU_Off)
				break;
			CurrentMenu->CallMenuEvent(mkey, true);
		}
	}
}

//==========================================================================
//
// Draws player pnum's splitscreen menu into the current 2D view.
//
//==========================================================================

void G_DrawSplitMenu(int pnum)
{
	if (!G_SplitMenuOpen(pnum))
		return;
	FSplitMenuContext ctx(pnum);
	if (CurrentMenu != nullptr && menuactive != MENU_Off)
		CurrentMenu->CallDrawer();
}

//==========================================================================
//
// Network events sent by a splitscreen menu are applied to its player
// (local only: splitscreen does not run in a network game).
//
//==========================================================================

bool G_QueueSplitMenuNetEvent(const FString& name, int arg1, int arg2, int arg3, bool manual)
{
	if (G_SplitMenuSender < 0)
		return false;
	SplitNetEvents.Push({ G_SplitMenuSender, name, { arg1, arg2, arg3 }, manual });
	return true;
}

void G_RunSplitMenuNetEvents()
{
	if (SplitNetEvents.Size() == 0 || primaryLevel == nullptr)
		return;
	TArray<FSplitNetEvent> events = std::move(SplitNetEvents);
	SplitNetEvents.Clear();
	for (auto& e : events)
	{
		if (playeringame[e.player])
			primaryLevel->localEventManager->Console(e.player, e.name, e.args[0], e.args[1], e.args[2], e.manual, false);
	}
}

void G_ClearSplitMenus()
{
	for (int pnum = 0; pnum < MAXPLAYERS; pnum++)
	{
		if (SplitMenus[pnum] != nullptr)
		{
			FSplitMenuContext ctx(pnum);
			M_ClearMenus();
		}
	}
	SplitNetEvents.Clear();
}

//==========================================================================
//
// Pause rule: in splitscreen a gameplay menu pauses only when every local
// player has a menu open.
//
//==========================================================================

bool G_SplitMenuAllowsPause()
{
	int locals[MAXPLAYERS];
	const int count = G_GetLocalPlayers(locals);
	if (count <= 1 || !G_IsGameplayMenu(CurrentMenu))
		return true;
	for (int i = 0; i < count; i++)
	{
		if (locals[i] != consoleplayer && !G_SplitMenuOpen(locals[i]))
			return false;
	}
	return true;
}

// Debug / testing: splitscreen_openmenu <menu name> <player number 1-4>
#include "c_dispatch.h"
CCMD(splitscreen_openmenu)
{
	if (argv.argc() < 3)
	{
		Printf("Usage: splitscreen_openmenu <menu> <player 1-4>\n");
		return;
	}
	G_OpenMenuForPlayer(FName(argv[1]), atoi(argv[2]) - 1);
}
