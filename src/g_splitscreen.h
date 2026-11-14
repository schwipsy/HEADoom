#pragma once
//
// Local splitscreen co-op: what the renderer drew this frame, so the HUD can
// be drawn once per view.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

struct sector_t;

enum { SPLITSCREEN_MAXVIEWS = 4 };

struct FSplitFrame
{
	int count = 0;	// views drawn this frame; 0 or 1 means no split
	int player[SPLITSCREEN_MAXVIEWS] = {};
	sector_t* sector[SPLITSCREEN_MAXVIEWS] = {};
	int x[SPLITSCREEN_MAXVIEWS] = {}, y[SPLITSCREEN_MAXVIEWS] = {};	// top-down screen coordinates
	int w[SPLITSCREEN_MAXVIEWS] = {}, h[SPLITSCREEN_MAXVIEWS] = {};
};

extern FSplitFrame gSplitFrame;
