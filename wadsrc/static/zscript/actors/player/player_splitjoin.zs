//
// Local splitscreen co-op: a player who joins a game in progress gets a copy of
// another player's inventory (weapons, reserve ammo, keys, upgrades ...), with full
// health and full magazines. Items the joining player already has (its start items)
// are not given again.
//
// Mods decide what else is copied by overriding Inventory.CopiesToJoiningPlayer()
// and Inventory.CopyToJoiningPlayer().
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

extend class PlayerPawn
{
	private Inventory SplitJoinGive(class<Inventory> cls)
	{
		let item = Inventory(Spawn(cls));
		if (!item)
			return null;
		item.ClearCounters();
		item.bDropped = item.bNeverLocal = true;	// never leaves a "weapon stay" copy behind
		item.bQuiet = true;
		let w = Weapon(item);
		if (w)
			w.AmmoGive1 = w.AmmoGive2 = 0;
		if (!item.CallTryPickup(self))
		{
			item.Destroy();
			return null;
		}
		return FindInventory(cls);
	}

	void SplitCopyInventoryFrom(PlayerPawn src)
	{
		if (!src || src == self || !player)
			return;

		// What this player already carries: its start items.
		Array<class<Inventory> > own;
		for (let it = Inv; it; it = it.Inv)
			own.Push(it.GetClass());

		// Ammo used by the source's weapons. Any other ammo is a reload / state counter.
		Array<class<Inventory> > weaponAmmo;
		for (let it = src.Inv; it; it = it.Inv)
		{
			let w = Weapon(it);
			if (!w)
				continue;
			if (w.AmmoType1) weaponAmmo.Push(w.AmmoType1);
			if (w.AmmoType2) weaponAmmo.Push(w.AmmoType2);
		}

		let ready = player.ReadyWeapon;

		for (let it = src.Inv; it; it = it.Inv)
		{
			let cls = it.GetClass();
			bool started = own.Find(cls) != own.Size();

			if (it is 'Weapon')
			{
				if (!started)
					SplitJoinGive(cls);
				continue;
			}

			if (it is 'Ammo')
			{
				bool used = weaponAmmo.Find(cls) != weaponAmmo.Size();
				if (!started && !used)
					continue;
				let def = GetDefaultByType(cls);
				// Magazines start full (Amount == MaxAmount); reserve ammo does not.
				bool mag = def.Amount > 0 && def.Amount == def.MaxAmount;
				let mine = started ? FindInventory(cls) : SplitJoinGive(cls);
				if (!mine)
					continue;
				mine.MaxAmount = max(mine.MaxAmount, it.MaxAmount);
				mine.Amount = mag ? mine.MaxAmount : max(mine.Amount, it.Amount);
				continue;
			}

			if (it is 'BasicArmor')
			{
				let a = BasicArmor(it);
				let mine = BasicArmor(FindInventory(cls));
				if (mine && a.Amount > mine.Amount)
				{
					mine.Amount = a.Amount;
					mine.MaxAmount = a.MaxAmount;
					mine.SavePercent = a.SavePercent;
					mine.MaxAbsorb = a.MaxAbsorb;
					mine.MaxFullAbsorb = a.MaxFullAbsorb;
					mine.BonusCount = a.BonusCount;
					mine.ArmorType = a.ArmorType;
					mine.ActualSaveAmount = a.ActualSaveAmount;
				}
				continue;
			}

			if (it is 'Powerup' || it is 'Health' || !it.CopiesToJoiningPlayer())
				continue;

			let mine = started ? FindInventory(cls) : SplitJoinGive(cls);
			if (!mine)
				continue;
			mine.MaxAmount = max(mine.MaxAmount, it.MaxAmount);
			mine.Amount = clamp(it.Amount, 0, mine.MaxAmount);
			mine.CopyToJoiningPlayer(it);
		}

		// Keep the weapon the player spawned with selected.
		if (ready)
			player.PendingWeapon = WP_NOCHANGE;

		// Full health.
		health = player.health = GetMaxHealth(true);
	}
}
