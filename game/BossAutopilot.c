#include <common.h>

// Full CPU autopilot for playable Story bosses.
//
// L3 toggles the mode. Enabling converts the current human driver into the
// retail BOTS driving pipeline without respawning it, then relocalizes the AI
// to the closest forward nav segment so it continues from the activation point.
// Disabling removes the driver from the AI nav list and restores normal player
// physics at the position/speed reached by the CPU.

enum
{
	BOSS_AUTOPILOT_MAX_DRIVERS = 8,
	BOSS_AUTOPILOT_NAV_AHEAD = 1,
	BOSS_AUTOPILOT_ANGLE_MASK = 0xfff,
	BOSS_AUTOPILOT_ANGLE_HALF = 0x800,
};

struct BossAutopilotState
{
	s16 characterID;
	s16 levelID;
	u8 initialized;
	u8 enabled;
	u32 lastToggleFrame;
	void (*savedThTick)(struct Thread *);
};

static struct BossAutopilotState s_bossAutopilotState[BOSS_AUTOPILOT_MAX_DRIVERS];

static int BossAutopilot_GetCharacterID(struct Driver *driver)
{
	if (driver == NULL || (u32)driver->driverID >= BOSS_AUTOPILOT_MAX_DRIVERS)
	{
		return -1;
	}

	return data.characterIDs[driver->driverID];
}

static int BossAutopilot_IsBossCharacter(int characterID)
{
	switch (characterID)
	{
	case RIPPER_ROO:
	case PAPU_PAPU:
	case KOMODO_JOE:
	case PINSTRIPE:
	case NITROS_OXIDE:
		return 1;
	default:
		return 0;
	}
}

static struct BossAutopilotState *BossAutopilot_GetState(struct Driver *driver)
{
	if (driver == NULL || (u32)driver->driverID >= BOSS_AUTOPILOT_MAX_DRIVERS)
	{
		return NULL;
	}

	return &s_bossAutopilotState[driver->driverID];
}

static int BossAutopilot_AngleAbsDelta(int a, int b)
{
	int delta = CTR_MipsSubLo(a, b) & BOSS_AUTOPILOT_ANGLE_MASK;
	if (delta >= BOSS_AUTOPILOT_ANGLE_HALF)
	{
		delta = CTR_MipsSubLo(delta, BOSS_AUTOPILOT_ANGLE_MASK + 1);
	}
	if (delta < 0)
	{
		delta = CTR_MipsNegLo(delta);
	}
	return delta;
}

static s64 BossAutopilot_NavScore(struct Driver *driver, const struct NavFrame *frame)
{
	int driverX = CTR_MipsSra(driver->posCurr.x, FRACTIONAL_BITS_8);
	int driverZ = CTR_MipsSra(driver->posCurr.z, FRACTIONAL_BITS_8);
	int dx = CTR_MipsSubLo(driverX, frame->pos.x);
	int dz = CTR_MipsSubLo(driverZ, frame->pos.z);
	int navYaw = CTR_MipsSll(frame->rot[1], 4) & BOSS_AUTOPILOT_ANGLE_MASK;
	int headingError = BossAutopilot_AngleAbsDelta(navYaw, driver->angle);

	// Distance dominates. Heading is a secondary discriminator that keeps the
	// conversion on the correct branch at crossings and parallel track pieces.
	return ((s64)dx * dx + (s64)dz * dz) + ((s64)headingError * headingError << 2);
}

static int BossAutopilot_FindClosestNav(struct Driver *driver, int *pathOut, int *indexOut)
{
	int bestPath = -1;
	int bestIndex = -1;
	s64 bestScore = (s64)1 << 62;

	for (int path = 0; path < BOTS_NAV_PATH_COUNT; path++)
	{
		struct NavHeader *header = sdata->NavPath_ptrHeader[path];
		struct NavFrame *frames = sdata->NavPath_ptrNavFrameArray[path];
		if (header == NULL || frames == NULL || header->numPoints <= 1)
		{
			continue;
		}

		for (int index = 0; index < header->numPoints; index++)
		{
			s64 score = BossAutopilot_NavScore(driver, &frames[index]);
			if (score < bestScore)
			{
				bestScore = score;
				bestPath = path;
				bestIndex = index;
			}
		}
	}

	if (bestPath < 0)
	{
		return 0;
	}

	*pathOut = bestPath;
	*indexOut = bestIndex;
	return 1;
}

static void BossAutopilot_ResetState(struct Driver *driver, struct BossAutopilotState *state)
{
	memset(state, 0, sizeof(*state));
	state->characterID = (s16)BossAutopilot_GetCharacterID(driver);
	state->levelID = (s16)sdata->gGT->levelID;
	state->lastToggleFrame = (u32)-1;
	state->initialized = 1;
}

static void BossAutopilot_Disable(struct Thread *thread, struct Driver *driver, struct BossAutopilotState *state)
{
	if (state == NULL || !state->enabled)
	{
		return;
	}

	if ((driver->actionsFlagSet & ACTION_BOT) != 0)
	{
		int path = driver->botData.botPath;
		if ((u32)path < BOTS_NAV_PATH_COUNT)
		{
			LIST_RemoveMember(&sdata->navBotList[path], &driver->botData.item);
		}
	}

	driver->actionsFlagSet &= ~(ACTION_BOT | ACTION_ENGINE_ECHO | ACTION_BACK_SKID | ACTION_FRONT_SKID | ACTION_DROPPING_MINE);
	thread->funcThTick = state->savedThTick;

	// Keep the position, orientation and linear speed produced by BOTS, but
	// discard the AI-only navigation state before returning to human physics.
	memset(&driver->botData, 0, sizeof(struct BotData));
	driver->turnAngleCurr = 0;
	driver->multDrift = 0;
	driver->ampTurnState = 0;
	driver->rotationSpinRate = 0;
	driver->funcPtrs[DRIVER_FUNC_INIT] = VehPhysProc_Driving_Init;
	VehPhysProc_Driving_Init(thread, driver);

	state->enabled = 0;
}

static int BossAutopilot_Enable(struct Thread *thread, struct Driver *driver, struct BossAutopilotState *state)
{
	int path;
	int nearestIndex;
	if (!BossAutopilot_FindClosestNav(driver, &path, &nearestIndex))
	{
		return 0;
	}

	// BOTS_Driver_Convert is retail's existing "player becomes CPU" path. It is
	// normally called after a human finishes, so preserve and restore the race
	// statistics that UI_RaceEnd_GetDriverClock would otherwise finalize here.
	int savedDistanceDriven = driver->distanceDriven;
	int savedMissileRatio = driver->NumMissilesComparedToNumAttacks;
	int savedNumTimesAttacked = driver->numTimesAttacked;
	int savedWinningLastPlaceTime = driver->TimeWinningDriverSpentLastPlace;
	u32 savedRaceTimerFrozen = driver->actionsFlagSet & ACTION_RACE_TIMER_FROZEN;

	state->savedThTick = thread->funcThTick;
	BOTS_Driver_Convert(driver);

	driver->distanceDriven = savedDistanceDriven;
	driver->NumMissilesComparedToNumAttacks = savedMissileRatio;
	driver->numTimesAttacked = savedNumTimesAttacked;
	driver->TimeWinningDriverSpentLastPlace = savedWinningLastPlaceTime;
	if (savedRaceTimerFrozen == 0)
	{
		driver->actionsFlagSet &= ~ACTION_RACE_TIMER_FROZEN;
	}
	else
	{
		driver->actionsFlagSet |= ACTION_RACE_TIMER_FROZEN;
	}

	if ((driver->actionsFlagSet & ACTION_BOT) == 0)
	{
		thread->funcThTick = state->savedThTick;
		return 0;
	}

	int oldPath = driver->botData.botPath;
	if (oldPath != path)
	{
		if ((u32)oldPath < BOTS_NAV_PATH_COUNT)
		{
			LIST_RemoveMember(&sdata->navBotList[oldPath], &driver->botData.item);
		}
		driver->botData.botPath = (s16)path;
		LIST_AddFront(&sdata->navBotList[path], &driver->botData.item);
	}

	struct NavHeader *header = sdata->NavPath_ptrHeader[path];
	int targetIndex = nearestIndex + BOSS_AUTOPILOT_NAV_AHEAD;
	if (targetIndex >= header->numPoints)
	{
		targetIndex -= header->numPoints;
	}

	// Re-anchor the retail conversion from the activation point instead of its
	// usual finish-line/first-nav-frame assumption.
	driver->botData.botNavFrame = &sdata->NavPath_ptrNavFrameArray[path][targetIndex];
	BOTS_SetRotation(driver, 0);
	driver->botData.botFlags |= BOT_FLAG_STARTLINE_INIT_DONE;
	driver->botData.botAccel = 0;

	// Full CPU control starts on the next thread tick: throttle, braking,
	// steering, drifting, jumps, collision recovery and nav-path progression.
	thread->funcThTick = BOTS_ThTick_Drive;
	state->enabled = 1;
	return 1;
}

static void BossAutopilot_UpdateToggle(struct Thread *thread, struct Driver *driver)
{
	struct GameTracker *gGT = sdata->gGT;
	if (thread == NULL || driver == NULL || gGT == NULL || (u32)driver->driverID >= BOSS_AUTOPILOT_MAX_DRIVERS)
	{
		return;
	}

	struct BossAutopilotState *state = BossAutopilot_GetState(driver);
	int characterID = BossAutopilot_GetCharacterID(driver);
	if (!state->initialized || state->characterID != characterID || state->levelID != gGT->levelID)
	{
		BossAutopilot_ResetState(driver, state);
	}

	if (!BossAutopilot_IsBossCharacter(characterID))
	{
		if (state->enabled)
		{
			BossAutopilot_Disable(thread, driver, state);
		}
		return;
	}

	if ((sdata->gGamepads->gamepad[driver->driverID].buttonsTapped & BTN_L3) == 0)
	{
		return;
	}

	// A newly-enabled BOTS thread can execute later in this same game frame and
	// calls VehFrameProc_Driving again. buttonsTapped still contains the original
	// L3 edge at that point, so without this guard the very same click enables
	// and then immediately disables autopilot. Consume a toggle at most once per
	// gGT frame; a later physical L3 click on a new frame can toggle normally.
	if (state->lastToggleFrame == gGT->timer)
	{
		return;
	}
	state->lastToggleFrame = gGT->timer;

	if (state->enabled)
	{
		BossAutopilot_Disable(thread, driver, state);
		return;
	}

	if ((driver->actionsFlagSet & (ACTION_BOT | ACTION_RACE_FINISHED)) != 0 || gGT->trafficLightsTimer > 0)
	{
		return;
	}

	BossAutopilot_Enable(thread, driver, state);
}

void VehFrameProc_Driving(struct Thread *thread, struct Driver *driver)
{
	VehFrameProc_Driving_BossPlayable(thread, driver);
	BossAutopilot_UpdateToggle(thread, driver);
}
