// Direct per-controller gamepad access for local splitscreen players.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdint.h>

// Raw state of one gamepad, in XInput layout.
struct FSplitPadState
{
	bool connected = false;
	uint16_t buttons = 0;    // XINPUT_GAMEPAD_* bits
	uint8_t lt = 0, rt = 0;  // triggers, 0..255
	int16_t lx = 0, ly = 0;  // left stick
	int16_t rx = 0, ry = 0;  // right stick
};

enum
{
	SPLITPAD_DPAD_UP = 0x0001,
	SPLITPAD_DPAD_DOWN = 0x0002,
	SPLITPAD_DPAD_LEFT = 0x0004,
	SPLITPAD_DPAD_RIGHT = 0x0008,
	SPLITPAD_START = 0x0010,
	SPLITPAD_BACK = 0x0020,
	SPLITPAD_LTHUMB = 0x0040,
	SPLITPAD_RTHUMB = 0x0080,
	SPLITPAD_LB = 0x0100,
	SPLITPAD_RB = 0x0200,
	SPLITPAD_A = 0x1000,
	SPLITPAD_B = 0x2000,
	SPLITPAD_X = 0x4000,
	SPLITPAD_Y = 0x8000,
	SPLITPAD_MAX = 4,
};

// Reads controller 'pad' (0-3). Returns false if it cannot be read.
bool I_GetSplitPadState(int pad, FSplitPadState& out);

// Bit i set: controller i belongs to a splitscreen player and is ignored by
// the normal input system, so it does not also move player 1.
extern uint32_t I_SplitscreenReservedPads;
