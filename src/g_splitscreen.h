#pragma once
//
// Local splitscreen co-op: what the renderer drew this frame, so the HUD can
// be drawn once per view.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

struct sector_t;
struct FRenderViewpoint;

enum { SPLITSCREEN_MAXVIEWS = 4 };

struct FSplitFrame
{
	int count = 0;	// views drawn this frame; 0 or 1 means no split
	int player[SPLITSCREEN_MAXVIEWS] = {};
	sector_t* sector[SPLITSCREEN_MAXVIEWS] = {};
	int x[SPLITSCREEN_MAXVIEWS] = {}, y[SPLITSCREEN_MAXVIEWS] = {};	// top-down screen coordinates
	int w[SPLITSCREEN_MAXVIEWS] = {}, h[SPLITSCREEN_MAXVIEWS] = {};
	const FRenderViewpoint* viewpoint[SPLITSCREEN_MAXVIEWS] = {};
	int current = -1;	// view whose HUD is being drawn, -1 outside of that
};

extern FSplitFrame gSplitFrame;

void G_TickSplitStatusBars();

class DMenu;
class FName;
class FString;
bool G_IsGameplayMenu(DMenu* menu);
bool G_SplitMenuOpen(int pnum);
void G_OpenMenuForPlayer(FName name, int pnum);
void G_SplitMenuTicker();
void G_DrawSplitMenu(int pnum);
bool G_QueueSplitMenuNetEvent(const FString& name, int arg1, int arg2, int arg3, bool manual);
void G_RunSplitMenuNetEvents();
void G_QueueSplitNetEvent(int player, const FString& name, int arg1, int arg2, int arg3, bool manual);
void G_ClearSplitMenus();
bool G_SplitMenuAllowsPause();
