//
// Co-op enemy scaling.
//
// When a map starts with more than one player in a co-op game, extra copies of
// the map's monsters are placed next to the originals (sv_coopmonsters percent
// more per extra player), and monsters marked +BOSS get more health instead
// (sv_coopbosshealth percent more per extra player). Runs once when the map is
// first entered; savegames and revisited hub maps keep what they had.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "doomstat.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "actor.h"
#include "actorinlines.h"
#include "p_local.h"
#include "p_checkposition.h"
#include "c_cvars.h"

CVAR(Int, sv_coopmonsters, 50, CVAR_SERVERINFO | CVAR_ARCHIVE)		// % more monsters per extra player, 0 = off
CVAR(Int, sv_coopbosshealth, 50, CVAR_SERVERINFO | CVAR_ARCHIVE)	// % more boss health per extra player, 0 = off

// A free spot next to orig (same floor), or false. Also used to bring downed players to a checkpoint.
bool G_FindSpotNear(AActor* orig, DVector3& out)
{
	static const double turns[8] = { 90, 270, 180, 0, 135, 225, 45, 315 };	// sides and back first
	const bool flyer = (orig->flags & MF_NOGRAVITY) != 0;
	for (int ring = 1; ring <= 3; ring++)
	{
		const double dist = (orig->radius * 2 + 8) * ring;
		for (double t : turns)
		{
			const DAngle ang = orig->Angles.Yaw + DAngle::fromDeg(t);
			const DVector2 p = orig->Vec2Offset(ang.Cos() * dist, ang.Sin() * dist);
			FCheckPosition tm;
			const bool ok = P_CheckPosition(orig, p, tm);
			orig->BlockingMobj = nullptr;
			orig->BlockingLine = nullptr;
			if (!ok)
				continue;
			const double z = flyer ? orig->Z() : tm.floorz;
			if (!flyer && fabs(tm.floorz - orig->floorz) > orig->MaxStepHeight)
				continue;
			if (tm.ceilingz - z < orig->Height || tm.floorz - tm.dropoffz > orig->MaxDropOffHeight)
				continue;
			out = DVector3(p, z);
			return true;
		}
	}
	return false;
}

static void SpawnCopy(AActor* orig)
{
	DVector3 pos;
	if (!G_FindSpotNear(orig, pos))
		return;
	AActor* copy = Spawn(orig->Level, orig->GetClass(), pos, NO_REPLACE);
	if (copy == nullptr)
		return;
	if (!P_TestMobjLocation(copy) || !P_CheckSight(orig, copy, SF_IGNOREVISIBILITY | SF_IGNOREWATERBOUNDARY))
	{
		copy->ClearCounters();
		copy->Destroy();
		return;
	}
	copy->Angles.Yaw = orig->Angles.Yaw;
	copy->SpawnPoint = pos;
	copy->SpawnAngle = orig->SpawnAngle;
	copy->SpawnFlags = orig->SpawnFlags & ~MTF_SECRET;
	copy->HandleSpawnFlags();
	for (int i = 0; i < 5; i++)
		copy->args[i] = orig->args[i];
	copy->Translation = orig->Translation;
	copy->Scale = orig->Scale;
	copy->health = orig->health;
	copy->StartHealth = orig->StartHealth;
	copy->SetTID(orig->tid);	// scripts that wait for or wake a group of monsters include the copies
}

void G_CoopScaleMonsters(FLevelLocals* Level)
{
	if (!multiplayer || deathmatch)
		return;
	int count = 0;
	for (int i = 0; i < MAXPLAYERS; i++)
		if (playeringame[i]) count++;
	if (count < 2)
		return;

	const double extra = (count - 1) * clamp<int>(sv_coopmonsters, 0, 1000) / 100.;
	const double bossmul = 1 + (count - 1) * clamp<int>(sv_coopbosshealth, 0, 1000) / 100.;

	TArray<AActor*> monsters;
	auto it = Level->GetThinkerIterator<AActor>();
	AActor* mo;
	while ((mo = it.Next()) != nullptr)
	{
		if ((mo->flags & MF_COUNTKILL) && !(mo->flags & MF_FRIENDLY) && mo->health > 0 && mo->player == nullptr)
			monsters.Push(mo);
	}

	double acc = 0;
	int added = 0;
	for (AActor* m : monsters)
	{
		if (m->flags2 & MF2_BOSS)
		{
			m->health = int(m->health * bossmul);
			// SpawnHealth() (health bars, resurrection) follows the new value
			m->StartHealth = int((m->StartHealth ? m->StartHealth : m->GetDefault()->health) * bossmul);
			continue;
		}
		acc += extra;
		while (acc >= 1)
		{
			acc -= 1;
			const int before = Level->total_monsters;
			SpawnCopy(m);
			if (Level->total_monsters > before) added++;
		}
	}
	if (added > 0)
		Printf("Co-op:%d extra monsters for %d players\n", added, count);
}
