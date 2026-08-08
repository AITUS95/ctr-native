#include <common.h>

// Hybrid curve assist for playable Story bosses.
//
// L3 toggles assist mode, not full autopilot. On straights the kart stays under
// normal human control. The assist watches authored BOTS navigation ahead; when
// a meaningful bend is approaching it temporarily hands the existing driver to
// the retail BOTS pipeline, caps speed according to turn severity, then returns
// to player physics after the navigation has been straight for several frames.

enum
{
	BOSS_CURVE_ASSIST_MAX_DRIVERS = 8,
	BOSS_CURVE_ASSIST_NAV_AHEAD = 1,
	BOSS_CURVE_ASSIST_ANGLE_MASK = 0xfff,
	BOSS_CURVE_ASSIST_ANGLE_HALF = 0x800,

	// Look far enough ahead to brake before the turn instead of reacting only
	// after the kart is already committed to it.
	BOSS_CURVE_ASSIST_ENTRY_LOOKAHEAD = 12,
	BOSS_CURVE_ASSIST_EXIT_LOOKAHEAD = 6,
	BOSS_CURVE_ASSIST_ENTRY_ANGLE = 0x100,
	BOSS_CURVE_ASSIST_EXIT_ANGLE = 0x60,
	BOSS_CURVE_ASSIST_STRAIGHT_FRAMES_TO_RELEASE = 5,

	// BOTS normally tops out around 0x6400. Tight authored turns are deliberately
	// clamped much lower so a fast player approach is converted into decisive
	// braking before the AI steers through the corner.
	BOSS_CURVE_ASSIST_SPEED_GENTLE = 0x5600,
	BOSS_CURVE_ASSIST_SPEED_MEDIUM = 0x4c00,
	BOSS_CURVE_ASSIST_SPEED_TIGHT = 0x4200,
	BOSS_CURVE_ASSIST_SPEED_HAIRPIN = 0x3600,
	BOSS_CURVE_ASSIST_SEVERITY_MEDIUM = 0x200,
	BOSS_CURVE_ASSIST_SEVERITY_TIGHT = 0x320,
	BOSS_CURVE_ASSIST_SEVERITY_HAIRPIN = 0x480,
};

struct BossCurveAssistState
{
	s16 characterID;
	s16 levelID;
	u8 initialized;
	u8 assistEnabled;
	u8 botActive;
	u8 pendingDisable;
	u8 straightFrames;
	u8 _pad[3];
	u32 lastToggleFrame;
	void (*savedThTick)(struct Thread *);
};

struct BossCurveInfo
{
	int severity;
	int hasAuthoredDrift;
};

static struct BossCurveAssistState s_bossCurveAssistState[BOSS_CURVE_ASSIST_MAX_DRIVERS];

static void BossCurveAssist_ThTick_Drive(struct Thread *thread);

static int BossCurveAssist_GetCharacterID(struct Driver *driver)
{
	if (driver == NULL || (u32)driver->driverID >= BOSS_CURVE_ASSIST_MAX_DRIVERS)
	{
		return -1;
	}

	return data.characterIDs[driver->driverID];
}

static int BossCurveAssist_IsBossCharacter(int characterID)
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

static struct BossCurveAssistState *BossCurveAssist_GetState(struct Driver *driver)
{
	if (driver == NULL || (u32)driver->driverID >= BOSS_CURVE_ASSIST_MAX_DRIVERS)
	{
		return NULL;
	}

	return &s_bossCurveAssistState[driver->driverID];
}

static int BossCurveAssist_AngleAbsDelta(int a, int b)
{
	int delta = CTR_MipsSubLo(a, b) & BOSS_CURVE_ASSIST_ANGLE_MASK;
	if (delta >= BOSS_CURVE_ASSIST_ANGLE_HALF)
	{
		delta = CTR_MipsSubLo(delta, BOSS_CURVE_ASSIST_ANGLE_MASK + 1);
	}
	if (delta < 0)
	{
		delta = CTR_MipsNegLo(delta);
	}
	return delta;
}

static s64 BossCurveAssist_NavScore(struct Driver *driver, const struct NavFrame *frame)
{
	int driverX = CTR_MipsSra(driver->posCurr.x, FRACTIONAL_BITS_8);
	int driverZ = CTR_MipsSra(driver->posCurr.z, FRACTIONAL_BITS_8);
	int dx = CTR_MipsSubLo(driverX, frame->pos.x);
	int dz = CTR_MipsSubLo(driverZ, frame->pos.z);
	int navYaw = CTR_MipsSll(frame->rot[1], 4) & BOSS_CURVE_ASSIST_ANGLE_MASK;
	int headingError = BossCurveAssist_AngleAbsDelta(navYaw, driver->angle);

	// Distance dominates. Heading keeps crossings and parallel track sections
	// from selecting a nearby nav point that faces the wrong direction.
	return ((s64)dx * dx + (s64)dz * dz) + ((s64)headingError * headingError << 2);
}

static int BossCurveAssist_FindClosestNav(struct Driver *driver, int *pathOut, int *indexOut)
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
			s64 score = BossCurveAssist_NavScore(driver, &frames[index]);
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

static struct BossCurveInfo BossCurveAssist_AnalyzeNav(int path, int startIndex, int lookAhead)
{
	struct BossCurveInfo info = {0};
	if ((u32)path >= BOTS_NAV_PATH_COUNT)
	{
		return info;
	}

	struct NavHeader *header = sdata->NavPath_ptrHeader[path];
	struct NavFrame *frames = sdata->NavPath_ptrNavFrameArray[path];
	if (header == NULL || frames == NULL || header->numPoints <= 1 || startIndex < 0 || startIndex >= header->numPoints)
	{
		return info;
	}

	int index = startIndex;
	int prevYaw = CTR_MipsSll(frames[index].rot[1], 4) & BOSS_CURVE_ASSIST_ANGLE_MASK;

	for (int step = 0; step < lookAhead; step++)
	{
		if ((frames[index].flags & BOTS_NAV_FLAG_DRIFT_MASK) != 0)
		{
			info.hasAuthoredDrift = 1;
		}

		int nextIndex = index + 1;
		if (nextIndex >= header->numPoints)
		{
			nextIndex = 0;
		}

		int nextYaw = CTR_MipsSll(frames[nextIndex].rot[1], 4) & BOSS_CURVE_ASSIST_ANGLE_MASK;
		info.severity = CTR_MipsAddLo(info.severity, BossCurveAssist_AngleAbsDelta(nextYaw, prevYaw));
		if (info.severity > BOSS_CURVE_ASSIST_ANGLE_MASK)
		{
			info.severity = BOSS_CURVE_ASSIST_ANGLE_MASK;
		}

		prevYaw = nextYaw;
		index = nextIndex;
	}

	return info;
}

static int BossCurveAssist_GetSpeedCap(struct BossCurveInfo info)
{
	if (info.severity >= BOSS_CURVE_ASSIST_SEVERITY_HAIRPIN)
	{
		return BOSS_CURVE_ASSIST_SPEED_HAIRPIN;
	}
	if (info.severity >= BOSS_CURVE_ASSIST_SEVERITY_TIGHT)
	{
		return BOSS_CURVE_ASSIST_SPEED_TIGHT;
	}
	if (info.severity >= BOSS_CURVE_ASSIST_SEVERITY_MEDIUM)
	{
		return BOSS_CURVE_ASSIST_SPEED_MEDIUM;
	}

	// Authored drift sections deserve a meaningful slowdown even when their
	// local nav yaw changes gradually across many points.
	if (info.hasAuthoredDrift)
	{
		return BOSS_CURVE_ASSIST_SPEED_MEDIUM;
	}

	return BOSS_CURVE_ASSIST_SPEED_GENTLE;
}

static void BossCurveAssist_ClampBotSpeed(struct Driver *driver, struct BossCurveInfo info)
{
	if (driver == NULL)
	{
		return;
	}

	int speedCap = BossCurveAssist_GetSpeedCap(info);
	if (driver->botData.aiPhysics.speedLinear > speedCap)
	{
		driver->botData.aiPhysics.speedLinear = speedCap;
	}
	if (driver->speedApprox > speedCap)
	{
		driver->speedApprox = speedCap;
	}
	if (driver->speed > speedCap)
	{
		driver->speed = speedCap;
	}
}

static int BossCurveAssist_GetBotNavIndex(struct Driver *driver, int *pathOut, int *indexOut)
{
	if (driver == NULL)
	{
		return 0;
	}

	int path = driver->botData.botPath;
	if ((u32)path >= BOTS_NAV_PATH_COUNT)
	{
		return 0;
	}

	struct NavHeader *header = sdata->NavPath_ptrHeader[path];
	struct NavFrame *frames = sdata->NavPath_ptrNavFrameArray[path];
	if (header == NULL || frames == NULL || driver->botData.botNavFrame == NULL)
	{
		return 0;
	}

	int index = (int)(driver->botData.botNavFrame - frames);
	if (index < 0 || index >= header->numPoints)
	{
		return 0;
	}

	*pathOut = path;
	*indexOut = index;
	return 1;
}

static int BossCurveAssist_IsApproachingCurve(struct Driver *driver, int *pathOut, int *indexOut, struct BossCurveInfo *infoOut)
{
	int path;
	int index;
	if (!BossCurveAssist_FindClosestNav(driver, &path, &index))
	{
		return 0;
	}

	struct BossCurveInfo info = BossCurveAssist_AnalyzeNav(path, index, BOSS_CURVE_ASSIST_ENTRY_LOOKAHEAD);
	*pathOut = path;
	*indexOut = index;
	*infoOut = info;

	return info.hasAuthoredDrift || info.severity >= BOSS_CURVE_ASSIST_ENTRY_ANGLE;
}

static void BossCurveAssist_ResetState(struct Driver *driver, struct BossCurveAssistState *state)
{
	memset(state, 0, sizeof(*state));
	state->characterID = (s16)BossCurveAssist_GetCharacterID(driver);
	state->levelID = (s16)sdata->gGT->levelID;
	state->lastToggleFrame = (u32)-1;
	state->initialized = 1;
}

static void BossCurveAssist_ResetPlayerCamera(struct Driver *driver)
{
	struct GameTracker *gGT = sdata->gGT;
	if (gGT == NULL || driver == NULL || (u32)driver->driverID >= 4)
	{
		return;
	}

	struct CameraDC *cDC = &gGT->cameraDC[driver->driverID];
	cDC->driverToFollow = driver;
	cDC->cameraMode = 0;
	cDC->cameraModePrev = 0;
	cDC->currEOR = NULL;
	cDC->trackPathNode = NULL;
	cDC->trackPathProgress = 0;
	cDC->transitionBlend = 0x1000;
	cDC->transitionFrame = 0;
	cDC->transitionFrameCount = 0;
	cDC->spin360Angle = 0;
	cDC->botFlagsPrevFrame = 0;
	cDC->flags &= ~(CAMERA_FLAG_BATTLE_END_OF_RACE |
	                 CAMERA_FLAG_ARCADE_END_OF_RACE_REQUESTED |
	                 CAMERA_FLAG_TRACK_PATH_FACE_DRIVER |
	                 CAMERA_FLAG_TRACK_PATH_ALT_BRANCH |
	                 CAMERA_FLAG_TRANSITION_AWAY |
	                 CAMERA_FLAG_TRANSITION_BACK |
	                 CAMERA_FLAG_TRANSITION_HOLD |
	                 CAMERA_FLAG_ARCADE_END_OF_RACE_ACTIVE |
	                 CAMERA_FLAG_REVERSE);
	cDC->flags |= CAMERA_FLAG_RESET_RAIN_POS | CAMERA_FLAG_DIRECTION_CHANGED;
}

static void BossCurveAssist_ResetPlayerAngularState(struct Driver *driver)
{
	// BOTS uses rotCurr.w as a camera yaw offset. Human camera follow consumes it
	// directly, so clear it and all BOTS/drift interpolation at every handoff.
	driver->rotCurr.w = 0;
	driver->rotPrev.w = 0;
	driver->simpTurnState = 0;
	driver->wheelRotation = 0;
	driver->turnAngleCurr = 0;
	driver->turnAnglePrev = 0;
	driver->turnAngleLerpTarget = 0;
	driver->turnAngleLerpVel = 0;
	driver->multDrift = 0;
	driver->previousFrameMultDrift = 0;
	driver->timeUntilDriftSpinout = 0;
	driver->ampTurnState = 0;
	driver->rotationSpinRate = 0;
	driver->turnWobbleAngle = 0;
	driver->turnWobbleTimer = 0;
	driver->turnWobbleVelocity = 0;
}

static void BossCurveAssist_DisableBot(struct Thread *thread, struct Driver *driver, struct BossCurveAssistState *state)
{
	if (state == NULL || !state->botActive)
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

	// Keep position, forward yaw and linear speed reached by the CPU, while
	// discarding navigation and camera-facing state before human physics resumes.
	memset(&driver->botData, 0, sizeof(struct BotData));
	BossCurveAssist_ResetPlayerAngularState(driver);
	driver->funcPtrs[DRIVER_FUNC_INIT] = VehPhysProc_Driving_Init;
	VehPhysProc_Driving_Init(thread, driver);
	BossCurveAssist_ResetPlayerCamera(driver);

	state->pendingDisable = 0;
	state->straightFrames = 0;
	state->botActive = 0;
}

static int BossCurveAssist_EnableBot(struct Thread *thread, struct Driver *driver, struct BossCurveAssistState *state,
                                     int path, int nearestIndex, struct BossCurveInfo info)
{
	struct NavHeader *header = sdata->NavPath_ptrHeader[path];
	if (header == NULL || header->numPoints <= 1)
	{
		return 0;
	}

	// Retail conversion is normally an end-of-race path, so preserve the race
	// statistics/UI state that UI_RaceEnd_GetDriverClock mutates internally.
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

	int targetIndex = nearestIndex + BOSS_CURVE_ASSIST_NAV_AHEAD;
	if (targetIndex >= header->numPoints)
	{
		targetIndex -= header->numPoints;
	}

	driver->botData.botNavFrame = &sdata->NavPath_ptrNavFrameArray[path][targetIndex];
	BOTS_SetRotation(driver, 0);
	driver->botData.botFlags |= BOT_FLAG_STARTLINE_INIT_DONE;
	driver->botData.botAccel = 0;

	// Immediate cap is deliberate: if the player approaches a hairpin at full
	// speed, the assist brakes at takeover rather than waiting for BOTS friction.
	BossCurveAssist_ClampBotSpeed(driver, info);

	thread->funcThTick = BossCurveAssist_ThTick_Drive;
	state->pendingDisable = 0;
	state->straightFrames = 0;
	state->botActive = 1;
	return 1;
}

static int BossCurveAssist_IsL3Tapped(struct Driver *driver)
{
	return (sdata->gGamepads->gamepad[driver->driverID].buttonsTapped & BTN_L3) != 0;
}

static void BossCurveAssist_ThTick_Drive(struct Thread *thread)
{
	struct Driver *driver = thread != NULL ? (struct Driver *)thread->object : NULL;
	struct BossCurveAssistState *state = BossCurveAssist_GetState(driver);
	struct GameTracker *gGT = sdata->gGT;

	if (thread == NULL || driver == NULL || state == NULL || gGT == NULL || !state->botActive)
	{
		BOTS_ThTick_Drive(thread);
		return;
	}

	// L3 while the CPU owns a curve disables the assist and returns control before
	// another BOTS step can move or rotate the kart.
	if (state->pendingDisable ||
	    (BossCurveAssist_IsL3Tapped(driver) && state->lastToggleFrame != gGT->timer))
	{
		if (BossCurveAssist_IsL3Tapped(driver))
		{
			state->lastToggleFrame = gGT->timer;
			state->assistEnabled = 0;
		}
		BossCurveAssist_DisableBot(thread, driver, state);
		return;
	}

	int path;
	int index;
	struct BossCurveInfo info = {0};
	if (BossCurveAssist_GetBotNavIndex(driver, &path, &index))
	{
		info = BossCurveAssist_AnalyzeNav(path, index, BOSS_CURVE_ASSIST_EXIT_LOOKAHEAD);

		if (info.hasAuthoredDrift || info.severity >= BOSS_CURVE_ASSIST_EXIT_ANGLE)
		{
			state->straightFrames = 0;
		}
		else
		{
			if (state->straightFrames < BOSS_CURVE_ASSIST_STRAIGHT_FRAMES_TO_RELEASE)
			{
				state->straightFrames++;
			}
			if (state->straightFrames >= BOSS_CURVE_ASSIST_STRAIGHT_FRAMES_TO_RELEASE)
			{
				BossCurveAssist_DisableBot(thread, driver, state);
				return;
			}
		}
	}

	// Clamp before BOTS so this frame's nav integration already sees the reduced
	// speed, then clamp again after BOTS so acceleration/turbo cannot carry an
	// excessive corner-entry speed into the next frame.
	BossCurveAssist_ClampBotSpeed(driver, info);
	BOTS_ThTick_Drive(thread);
	BossCurveAssist_ClampBotSpeed(driver, info);

	// Recovery paths sometimes restore the retail pointer. Keep wrapping it so
	// automatic release and L3-off remain available after jumps/damage states.
	if (state->botActive && thread->funcThTick == BOTS_ThTick_Drive)
	{
		thread->funcThTick = BossCurveAssist_ThTick_Drive;
	}
}

static void BossCurveAssist_Update(struct Thread *thread, struct Driver *driver)
{
	struct GameTracker *gGT = sdata->gGT;
	if (thread == NULL || driver == NULL || gGT == NULL || (u32)driver->driverID >= BOSS_CURVE_ASSIST_MAX_DRIVERS)
	{
		return;
	}

	struct BossCurveAssistState *state = BossCurveAssist_GetState(driver);
	int characterID = BossCurveAssist_GetCharacterID(driver);
	if (!state->initialized || state->characterID != characterID || state->levelID != gGT->levelID)
	{
		BossCurveAssist_ResetState(driver, state);
	}

	if (!BossCurveAssist_IsBossCharacter(characterID))
	{
		state->assistEnabled = 0;
		if (state->botActive)
		{
			state->pendingDisable = 1;
			thread->funcThTick = BossCurveAssist_ThTick_Drive;
		}
		return;
	}

	if (state->botActive)
	{
		// VehFrameProc_Driving also runs from inside BOTS. Record L3 here but let
		// the wrapper perform the actual handoff at the start of the next CPU step.
		if (BossCurveAssist_IsL3Tapped(driver) && state->lastToggleFrame != gGT->timer)
		{
			state->lastToggleFrame = gGT->timer;
			state->assistEnabled = 0;
			state->pendingDisable = 1;
		}

		if (thread->funcThTick == BOTS_ThTick_Drive)
		{
			thread->funcThTick = BossCurveAssist_ThTick_Drive;
		}
		return;
	}

	if (BossCurveAssist_IsL3Tapped(driver) && state->lastToggleFrame != gGT->timer)
	{
		state->lastToggleFrame = gGT->timer;
		state->assistEnabled ^= 1;
	}

	if (!state->assistEnabled)
	{
		return;
	}

	if ((driver->actionsFlagSet & (ACTION_BOT | ACTION_RACE_FINISHED)) != 0 || gGT->trafficLightsTimer > 0)
	{
		return;
	}

	// Do not seize control during a damage/warp state. Once normal driving is
	// restored, the next frame will evaluate the upcoming curve again.
	if (driver->kartState != KS_NORMAL && driver->kartState != KS_DRIFTING && driver->kartState != KS_ANTIVSHIFT)
	{
		return;
	}

	int path;
	int index;
	struct BossCurveInfo info;
	if (BossCurveAssist_IsApproachingCurve(driver, &path, &index, &info))
	{
		BossCurveAssist_EnableBot(thread, driver, state, path, index, info);
	}
}

void VehFrameProc_Driving(struct Thread *thread, struct Driver *driver)
{
	VehFrameProc_Driving_BossPlayable(thread, driver);
	BossCurveAssist_Update(thread, driver);
}
