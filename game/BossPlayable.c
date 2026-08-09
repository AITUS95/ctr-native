#include <common.h>

// Playable Story-boss mechanics for the native port.
//
// The goal is to preserve human control while borrowing the parts of the
// Adventure boss implementation that are properties of the boss encounter:
// dynamic longitudinal speed pressure, scripted boss weapons, the confirmed
// plant immunity, and Oxide's reduced active-damage speed penalty.
//
// Steering, nav paths, bot acceleration ordering, and adaptive difficulty are
// deliberately not copied here.

enum
{
	BOSS_PLAYABLE_MAX_DRIVERS = 8,
	BOSS_PLAYABLE_DIFFICULTY_BASE = 0xe1,
	BOSS_PLAYABLE_DIFFICULTY_STEP = 5,
	BOSS_PLAYABLE_DIFFICULTY_DENOMINATOR = 0xf0,
	BOSS_PLAYABLE_RUBBERBAND_DISTANCE_SCALE = 0xa00,
	BOSS_PLAYABLE_RUBBERBAND_ROUNDING = 0x80,
	BOSS_PLAYABLE_RANK_ZERO_PERCENT_BONUS = 7,
	BOSS_PLAYABLE_PERCENT_SCALE = 100,
	BOSS_PLAYABLE_PERCENT_SHIFT = 3,
	BOSS_PLAYABLE_SPEED_CAP = 0x6900,
	BOSS_PLAYABLE_WEAPON_SPEED_MIN = 0x1f41,
	BOSS_PLAYABLE_WEAPON_COOLDOWN_RANDOM_MASK = 0x10,
	BOSS_PLAYABLE_WEAPON_COOLDOWN_BASE = 0xc,
};

struct BossPlayableProfile
{
	s8 difficultyID;
	s8 weaponTableID;
	u8 weaponMetaCount;
};

struct BossPlayableWeaponState
{
	s16 characterID;
	s16 levelID;
	s16 cooldown;
	u8 juiceCounter;
	u8 initialized;
};

static struct BossPlayableWeaponState s_bossPlayableWeaponState[BOSS_PLAYABLE_MAX_DRIVERS];

static const struct BossPlayableProfile s_bossPlayableProfiles[] =
{
	// Ripper Roo
	{0, 1, 6},
	// Papu Papu
	{1, 2, 6},
	// Komodo Joe
	{2, 3, 6},
	// Pinstripe
	{3, 4, 6},
	// Nitros Oxide (first Adventure encounter)
	{4, 0, 14},
};

static const struct BossPlayableProfile *BossPlayable_GetProfileFromCharacter(int characterID)
{
	switch (characterID)
	{
	case RIPPER_ROO:
		return &s_bossPlayableProfiles[0];
	case PAPU_PAPU:
		return &s_bossPlayableProfiles[1];
	case KOMODO_JOE:
		return &s_bossPlayableProfiles[2];
	case PINSTRIPE:
		return &s_bossPlayableProfiles[3];
	case NITROS_OXIDE:
		return &s_bossPlayableProfiles[4];
	default:
		return NULL;
	}
}

static const struct BossPlayableProfile *BossPlayable_GetProfile(struct Driver *driver)
{
	if (driver == NULL)
	{
		return NULL;
	}

	if ((driver->actionsFlagSet & ACTION_BOT) != 0)
	{
		return NULL;
	}

	if ((u32)driver->driverID >= BOSS_PLAYABLE_MAX_DRIVERS)
	{
		return NULL;
	}

	return BossPlayable_GetProfileFromCharacter(data.characterIDs[driver->driverID]);
}

static int BossPlayable_GetCharacterID(struct Driver *driver)
{
	if (driver == NULL || (u32)driver->driverID >= BOSS_PLAYABLE_MAX_DRIVERS)
	{
		return -1;
	}

	return data.characterIDs[driver->driverID];
}

static s16 BossPlayable_GetDifficultyParam(const struct BossPlayableProfile *profile, int index)
{
	struct Difficulty *difficulty = &data.BossDifficulty[profile->difficultyID];
	int lo = difficulty->params1[index];
	int hi = difficulty->params2[index];
	int factor = BOSS_PLAYABLE_DIFFICULTY_BASE + profile->difficultyID * BOSS_PLAYABLE_DIFFICULTY_STEP;

	return (s16)(lo + (factor * (hi - lo)) / BOSS_PLAYABLE_DIFFICULTY_DENOMINATOR);
}

static struct Driver *BossPlayable_FindReferenceDriver(struct Driver *driver)
{
	struct GameTracker *gGT = sdata->gGT;

	// Prefer race order so the playable boss reacts to the strongest current
	// opponent, just as the Story boss reacts to the human it is racing.
	for (int i = 0; i < BOSS_PLAYABLE_MAX_DRIVERS; i++)
	{
		struct Driver *candidate = gGT->driversInRaceOrder[i];
		if (candidate != NULL && candidate != driver)
		{
			return candidate;
		}
	}

	// Race order can be temporarily empty during setup; fall back to slots.
	for (int i = 0; i < BOSS_PLAYABLE_MAX_DRIVERS; i++)
	{
		struct Driver *candidate = gGT->drivers[i];
		if (candidate != NULL && candidate != driver)
		{
			return candidate;
		}
	}

	return NULL;
}

static int BossPlayable_GetTrackDistance(void)
{
	struct GameTracker *gGT = sdata->gGT;

	if (gGT == NULL || gGT->level1 == NULL || gGT->level1->ptr_restart_points == NULL)
	{
		return 0;
	}

	return CTR_MipsSll(gGT->level1->ptr_restart_points->distToFinish, 3);
}

static int BossPlayable_GetRaceProgress(struct Driver *driver, int trackDistance)
{
	int lapIndex = driver->lapIndex;

	if ((driver->actionsFlagSet & ACTION_BEHIND_START_LINE) != 0)
	{
		lapIndex--;
	}

	return CTR_MipsAddLo(CTR_MipsSubLo(trackDistance, driver->distanceToFinish_curr), CTR_MipsMulLo(lapIndex, trackDistance));
}

static int BossPlayable_GetRubberbandCorrection(struct Driver *driver, const struct BossPlayableProfile *profile)
{
	struct GameTracker *gGT = sdata->gGT;
	struct Driver *reference = BossPlayable_FindReferenceDriver(driver);
	int trackDistance = BossPlayable_GetTrackDistance();

	if (reference == NULL || trackDistance <= 0 || gGT->numLaps <= 0 || gGT->trafficLightsTimer > 0)
	{
		return 0;
	}

	int difficultyStat;
	if (reference->lapIndex == 0)
	{
		difficultyStat = BossPlayable_GetDifficultyParam(profile, 11);
	}
	else if (reference->lapIndex == gGT->numLaps - 1)
	{
		difficultyStat = BossPlayable_GetDifficultyParam(profile, 13);
	}
	else
	{
		difficultyStat = BossPlayable_GetDifficultyParam(profile, 12);
	}

	int referenceProgress = BossPlayable_GetRaceProgress(reference, trackDistance);
	int bossProgress = BossPlayable_GetRaceProgress(driver, trackDistance);

	// Story BOTS.c uses rank slot zero in a one-on-one boss race. Preserve that
	// exact boss offset instead of importing normal multi-bot rank ordering.
	int complexDifficultyStat = CTR_MipsSubLo(
	    CTR_MipsSubLo(referenceProgress, bossProgress),
	    CTR_MipsAddLo(BossPlayable_GetDifficultyParam(profile, 0), difficultyStat));

	int targetStat;
	if (complexDifficultyStat < 1)
	{
		targetStat = BossPlayable_GetDifficultyParam(profile, 9);
	}
	else if (driver->lapIndex >= gGT->numLaps - 1)
	{
		targetStat = CTR_MipsAddLo(BossPlayable_GetDifficultyParam(profile, 8), BossPlayable_GetDifficultyParam(profile, 10));
	}
	else
	{
		targetStat = BossPlayable_GetDifficultyParam(profile, 8);
	}

	int distanceMagnitude = complexDifficultyStat;
	if (distanceMagnitude < 0)
	{
		distanceMagnitude = CTR_MipsNegLo(distanceMagnitude);
	}

	int targetMagnitude = targetStat;
	if (targetMagnitude < 0)
	{
		targetMagnitude = CTR_MipsNegLo(targetMagnitude);
	}

	// Algebraic equivalent of the boss AI's distance ramp, followed by its
	// rank-zero +56% ramp-up and final cap at the difficulty stat.
	int correction = (int)(((s64)targetMagnitude * (distanceMagnitude + BOSS_PLAYABLE_RUBBERBAND_ROUNDING)) /
	                       BOSS_PLAYABLE_RUBBERBAND_DISTANCE_SCALE);
	correction = CTR_MipsAddLo(
	    correction,
	    CTR_MipsMulLo(CTR_MipsDiv(CTR_MipsSll(correction, BOSS_PLAYABLE_PERCENT_SHIFT), BOSS_PLAYABLE_PERCENT_SCALE),
	                  BOSS_PLAYABLE_RANK_ZERO_PERCENT_BONUS));

	if (targetMagnitude < correction)
	{
		correction = targetMagnitude;
	}

	if (targetStat < 0)
	{
		correction = CTR_MipsNegLo(correction);
	}

	return correction;
}

static int BossPlayable_ApplyOxideDamageResistance(struct Driver *driver, int speed)
{
	if (BossPlayable_GetCharacterID(driver) != NITROS_OXIDE)
	{
		return speed;
	}

	// The Story Oxide bot loses only one quarter of const_DamagedSpeed while
	// active damage is applied. GetBaseSpeed already subtracted the full human
	// penalty, so add back the other three quarters. Do not rewrite clock/TNT
	// handling, which follows different retail branches.
	if (driver->clockReceive == 0 &&
	    ((driver->burnTimer != 0) || (driver->squishTimer != 0) || (driver->rainCloudEffect == RAIN_CLOUD_EFFECT_SLOW)))
	{
		int fullPenalty = driver->const_DamagedSpeed;
		int oxidePenalty = CTR_MipsSra(fullPenalty, 2);
		speed = CTR_MipsAddLo(speed, CTR_MipsSubLo(fullPenalty, oxidePenalty));
	}

	return speed;
}

int VehPhysGeneral_GetBaseSpeed(struct Driver *driver)
{
	int speed = VehPhysGeneral_GetBaseSpeed_Original(driver);
	const struct BossPlayableProfile *profile = BossPlayable_GetProfile(driver);

	if (profile == NULL)
	{
		return speed;
	}

	speed = BossPlayable_ApplyOxideDamageResistance(driver, speed);

	// Human-control floor: Story AI can deliberately slow a boss while it is
	// ahead, but player drift initiation still compares speedApprox against the
	// normal class threshold. Let rubber-band add catch-up speed, never remove
	// the driver's normal (or Oxide damage-resistant) base speed.
	int humanControlFloor = speed;
	speed = CTR_MipsAddLo(speed, BossPlayable_GetRubberbandCorrection(driver, profile));
	if (speed < humanControlFloor)
	{
		speed = humanControlFloor;
	}

	if (speed < 0)
	{
		speed = 0;
	}
	else if (BOSS_PLAYABLE_SPEED_CAP < speed)
	{
		speed = BOSS_PLAYABLE_SPEED_CAP;
	}

	return speed;
}

static struct MetaDataBOSS *BossPlayable_SelectWeaponMeta(const struct BossPlayableProfile *profile, u8 checkpoint)
{
	struct MetaDataBOSS *base = data.bossWeaponMetaPtr[profile->weaponTableID];
	struct MetaDataBOSS *selected = base;

	for (u8 i = 0; i < profile->weaponMetaCount; i++)
	{
		struct MetaDataBOSS *candidate = &base[i];

		if (candidate->throwFlag == 0)
		{
			break;
		}

		if (candidate->trackCheckpoint > checkpoint)
		{
			break;
		}

		selected = candidate;
	}

	return selected;
}

static void BossPlayable_ResetWeaponState(struct BossPlayableWeaponState *state, int characterID, int levelID)
{
	state->characterID = (s16)characterID;
	state->levelID = (s16)levelID;
	state->cooldown = 0;
	state->juiceCounter = 0;
	state->initialized = 1;
}

static void BossPlayable_SetWeaponCooldown(struct BossPlayableWeaponState *state, struct MetaDataBOSS *meta)
{
	state->cooldown = (s16)((RngDeadCoed(&sdata->advRng) & BOSS_PLAYABLE_WEAPON_COOLDOWN_RANDOM_MASK) +
	                        meta->weaponCooldown + BOSS_PLAYABLE_WEAPON_COOLDOWN_BASE);
}

static void BossPlayable_DiscardCollectedWeapon(struct Driver *driver)
{
	if (BossPlayable_GetProfile(driver) == NULL)
	{
		return;
	}

	// Playable Story bosses use their scripted arsenal only. Weapon crates may
	// still break normally, but roulette/held-item state is discarded before
	// the next physics tick can turn it into a normal player weapon.
	if (driver->heldItemID != HELD_ITEM_NONE || driver->itemRollTimer != 0 || driver->numHeldItems != 0)
	{
		driver->heldItemID = HELD_ITEM_NONE;
		driver->itemRollTimer = 0;
		driver->numHeldItems = 0;
		driver->noItemTimer = 0;
		driver->actionsFlagSet &= ~ACTION_WEAPON_FIRE_REQUEST;
		driver->PickupTimeboxHUD.cooldown = 0;
	}
}

static void BossPlayable_UpdateWeapons(struct Driver *driver)
{
	const struct BossPlayableProfile *profile = BossPlayable_GetProfile(driver);
	struct GameTracker *gGT = sdata->gGT;

	if (profile == NULL || gGT == NULL || gGT->numLaps <= 0 || BossPlayable_FindReferenceDriver(driver) == NULL)
	{
		return;
	}

	int driverID = driver->driverID;
	int characterID = BossPlayable_GetCharacterID(driver);
	struct BossPlayableWeaponState *state = &s_bossPlayableWeaponState[driverID];

	if (!state->initialized || state->characterID != characterID || state->levelID != gGT->levelID || gGT->trafficLightsTimer > 0)
	{
		BossPlayable_ResetWeaponState(state, characterID, gGT->levelID);

		if (gGT->trafficLightsTimer > 0)
		{
			return;
		}
	}

	if ((driver->actionsFlagSet & ACTION_RACE_FINISHED) != 0)
	{
		return;
	}

	if (state->cooldown > 0)
	{
		state->cooldown--;
		return;
	}

	struct MetaDataBOSS *meta = BossPlayable_SelectWeaponMeta(profile, driver->checkpoint.currentIndex);
	BossPlayable_SetWeaponCooldown(state, meta);

	int speed = driver->speedApprox;
	if (speed < 0)
	{
		speed = CTR_MipsNegLo(speed);
	}

	// Scripted boss attacks require an empty player item slot. Crate weapons are
	// discarded by BossPlayable_DiscardCollectedWeapon before this update.
	if (driver->heldItemID != PICKUPBOTS_ITEM_NONE || driver->instTntRecv != NULL || driver->clockReceive != 0 ||
	    driver->pendingDamageType != 0 || speed < BOSS_PLAYABLE_WEAPON_SPEED_MIN)
	{
		return;
	}

	struct MetaDataBOSS localMeta = *meta;
	int savedGlobalJuiceCounter = sdata->bossJuiceCounter;
	sdata->bossJuiceCounter = state->juiceCounter;

	int weaponID = PickupBots_GetBossWeaponID(&localMeta);
	weaponID = PickupBots_UpdateBossJuice(&localMeta, weaponID);
	state->juiceCounter = (u8)sdata->bossJuiceCounter;
	sdata->bossJuiceCounter = savedGlobalJuiceCounter;

	if (weaponID < 0 || weaponID == PICKUPBOTS_ITEM_NONE)
	{
		return;
	}

	int throwFlag = localMeta.throwFlag;
	int weaponFlags = (throwFlag == BOSS_WEAPON_THROW) ? PICKUPBOTS_SHOOT_FLAG_RANDOM : 0;
	u8 oldWumpa = driver->numWumpas;
	u8 oldItem = driver->heldItemID;

	driver->numWumpas = ((localMeta.juiceFlag & BOSS_WEAPON_JUICED) != 0) ? DRIVER_WUMPA_JUICED_COUNT : 0;
	driver->heldItemID = (u8)weaponID;

	// Do not request boss attack XA here. On the native backend, starting a new
	// XA track currently prepares it synchronously (file open/sector scan/copy)
	// on the gameplay thread. The scripted weapon behavior is unchanged, while
	// avoiding intermittent frame-time spikes when an automatic attack fires.

	if (weaponID == PICKUPBOTS_ITEM_BOMB)
	{
		VehPickupItem_ShootNow(driver, PICKUPBOTS_SHOOT_ID_BOMB_MISSILE, (s16)weaponFlags);
	}
	else if (BossPlayable_GetCharacterID(driver) == NITROS_OXIDE && weaponID == PICKUPBOTS_ITEM_POTION &&
	         weaponFlags == PICKUPBOTS_SHOOT_FLAG_RANDOM && gGT->levelID == OXIDE_STATION)
	{
		// Retail Oxide Station boss exception: throw the potion twice.
		VehPickupItem_ShootNow(driver, weaponID, PICKUPBOTS_SHOOT_FLAG_RANDOM);
		VehPickupItem_ShootNow(driver, weaponID, PICKUPBOTS_SHOOT_FLAG_RANDOM);
	}
	else
	{
		VehPickupItem_ShootNow(driver, weaponID, (s16)weaponFlags);

		if (weaponID == PICKUPBOTS_ITEM_TNT && localMeta.throwFlag == BOSS_WEAPON_NORMAL &&
		    state->juiceCounter != PICKUPBOTS_BOSS_JUICE_COUNTER_MAX)
		{
			state->juiceCounter = PICKUPBOTS_BOSS_JUICE_COUNTER_MAX;
		}
	}

	driver->heldItemID = oldItem;
	driver->numWumpas = oldWumpa;
}

void VehFrameProc_Driving(struct Thread *thread, struct Driver *driver)
{
	VehFrameProc_Driving_Original(thread, driver);
	BossPlayable_DiscardCollectedWeapon(driver);
	BossPlayable_UpdateWeapons(driver);
}

// RB_Hazard.c is included later in game_unity.h under a temporary rename so
// this wrapper can preserve the confirmed Story-boss plant immunity.
int RB_Hazard_HurtDriver_Original(struct Driver *driverVictim, int damageType, struct Driver *driverAttacker, int reason);

int RB_Hazard_HurtDriver(struct Driver *driverVictim, int damageType, struct Driver *driverAttacker, int reason)
{
	if (BossPlayable_GetProfile(driverVictim) != NULL && damageType == 5 && driverAttacker == NULL && reason == 0)
	{
		return 0;
	}

	return RB_Hazard_HurtDriver_Original(driverVictim, damageType, driverAttacker, reason);
}
