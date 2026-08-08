#include <common.h>

// Optional steering/drift assist for playable Story bosses.
//
// L3 toggles the mode. The assist never chooses a turn on its own: the player
// must first steer or drift in the desired direction. Once a matching curve is
// detected, the closest authored nav path becomes a temporary safe trajectory
// through that single corner. Steering and the drift button are injected only
// while the corner is active; throttle, brake and weapons remain untouched.

enum
{
	BOSS_CORNER_ASSIST_MAX_DRIVERS = 8,
	BOSS_CORNER_ASSIST_STICK_CENTER = 0x80,
	BOSS_CORNER_ASSIST_INTENT_STICK_THRESHOLD = 0x18,
	BOSS_CORNER_ASSIST_CURVATURE_LOOKAHEAD = 14,
	BOSS_CORNER_ASSIST_ENGAGE_CURVATURE_MIN = 0x30,
	BOSS_CORNER_ASSIST_EXIT_CURVATURE_MAX = 0x18,
	BOSS_CORNER_ASSIST_EXIT_HEADING_MAX = 0x28,
	BOSS_CORNER_ASSIST_EXIT_STRAIGHT_FRAMES = 8,
	BOSS_CORNER_ASSIST_ENGAGE_DISTANCE = 0x700,
	BOSS_CORNER_ASSIST_LOST_DISTANCE = 0xa00,
	BOSS_CORNER_ASSIST_TARGET_LOOKAHEAD_MIN = 5,
	BOSS_CORNER_ASSIST_TARGET_LOOKAHEAD_MAX = 14,
	BOSS_CORNER_ASSIST_STEER_OFFSET_MIN = 0x18,
	BOSS_CORNER_ASSIST_STEER_OFFSET_MAX = 0x70,
	BOSS_CORNER_ASSIST_STEER_FULL_ERROR = 0x180,
	BOSS_CORNER_ASSIST_DRIFT_RETRY_FRAMES = 8,
	BOSS_CORNER_ASSIST_DIR_NONE = 0,
	BOSS_CORNER_ASSIST_DIR_LEFT = 1,
	BOSS_CORNER_ASSIST_DIR_RIGHT = -1,
};

struct BossCornerAssistState
{
	s16 characterID;
	s16 levelID;
	s16 navPath;
	s16 navIndex;
	u8 initialized;
	u8 enabled;
	u8 engaged;
	u8 driftTapPending;
	u8 driftRetryFrames;
	u8 straightFrames;
	u8 steerByte;
	s8 intentDirection;
};

struct BossCornerAssistPadBackup
{
	s16 stickLX;
	int buttonsHeld;
	int buttonsTapped;
};

static struct BossCornerAssistState s_bossCornerAssistState[BOSS_CORNER_ASSIST_MAX_DRIVERS];

static void BossCornerAssist_DrivingUpdate(struct Thread *thread, struct Driver *driver);
static void BossCornerAssist_DrivingPhysLinear(struct Thread *thread, struct Driver *driver);
static void BossCornerAssist_PowerSlideUpdate(struct Thread *thread, struct Driver *driver);
static void BossCornerAssist_PowerSlidePhysLinear(struct Thread *thread, struct Driver *driver);
static void BossCornerAssist_InstallHooks(struct Driver *driver);

static int BossCornerAssist_Abs(int value)
{
	return value < 0 ? CTR_MipsNegLo(value) : value;
}

static int BossCornerAssist_AngleDelta(int target, int current)
{
	int delta = CTR_MipsSubLo(target, current) & 0xfff;
	if (delta > 0x7ff)
	{
		delta = CTR_MipsSubLo(delta, 0x1000);
	}
	return delta;
}

static int BossCornerAssist_WrapIndex(int index, int count)
{
	while (index >= count)
	{
		index -= count;
	}
	while (index < 0)
	{
		index += count;
	}
	return index;
}

static struct BossCornerAssistState *BossCornerAssist_GetState(struct Driver *driver)
{
	if (driver == NULL || (u32)driver->driverID >= BOSS_CORNER_ASSIST_MAX_DRIVERS)
	{
		return NULL;
	}
	return &s_bossCornerAssistState[driver->driverID];
}

static void BossCornerAssist_RestoreHooks(struct Driver *driver)
{
	if (driver == NULL)
	{
		return;
	}

	if (driver->funcPtrs[DRIVER_FUNC_UPDATE] == BossCornerAssist_DrivingUpdate)
	{
		driver->funcPtrs[DRIVER_FUNC_UPDATE] = VehPhysProc_Driving_Update;
	}
	else if (driver->funcPtrs[DRIVER_FUNC_UPDATE] == BossCornerAssist_PowerSlideUpdate)
	{
		driver->funcPtrs[DRIVER_FUNC_UPDATE] = VehPhysProc_PowerSlide_Update;
	}

	if (driver->funcPtrs[DRIVER_FUNC_PHYS_LINEAR] == BossCornerAssist_DrivingPhysLinear)
	{
		driver->funcPtrs[DRIVER_FUNC_PHYS_LINEAR] = VehPhysProc_Driving_PhysLinear;
	}
	else if (driver->funcPtrs[DRIVER_FUNC_PHYS_LINEAR] == BossCornerAssist_PowerSlidePhysLinear)
	{
		driver->funcPtrs[DRIVER_FUNC_PHYS_LINEAR] = VehPhysProc_PowerSlide_PhysLinear;
	}
}

static void BossCornerAssist_ResetState(struct Driver *driver, struct BossCornerAssistState *state, int characterID, int levelID)
{
	BossCornerAssist_RestoreHooks(driver);
	memset(state, 0, sizeof(*state));
	state->characterID = (s16)characterID;
	state->levelID = (s16)levelID;
	state->navPath = -1;
	state->navIndex = -1;
	state->steerByte = BOSS_CORNER_ASSIST_STICK_CENTER;
	state->initialized = 1;
}

static void BossCornerAssist_Disengage(struct Driver *driver, struct BossCornerAssistState *state)
{
	state->engaged = 0;
	state->driftTapPending = 0;
	state->driftRetryFrames = 0;
	state->straightFrames = 0;
	state->steerByte = BOSS_CORNER_ASSIST_STICK_CENTER;
	state->intentDirection = BOSS_CORNER_ASSIST_DIR_NONE;
	state->navPath = -1;
	state->navIndex = -1;
	BossCornerAssist_RestoreHooks(driver);
}

static s64 BossCornerAssist_NavDistanceSq(struct Driver *driver, const struct NavFrame *frame)
{
	int driverX = CTR_MipsSra(driver->posCurr.x, FRACTIONAL_BITS_8);
	int driverZ = CTR_MipsSra(driver->posCurr.z, FRACTIONAL_BITS_8);
	int dx = CTR_MipsSubLo(driverX, frame->pos.x);
	int dz = CTR_MipsSubLo(driverZ, frame->pos.z);
	return (s64)dx * dx + (s64)dz * dz;
}

static int BossCornerAssist_FindNearestOnPath(struct Driver *driver, int path, s64 *distanceOut)
{
	if ((u32)path >= BOTS_NAV_PATH_COUNT)
	{
		return -1;
	}

	struct NavHeader *header = sdata->NavPath_ptrHeader[path];
	struct NavFrame *frames = sdata->NavPath_ptrNavFrameArray[path];
	if (header == NULL || frames == NULL || header->numPoints <= 0)
	{
		return -1;
	}

	int bestIndex = -1;
	s64 bestDistance = (s64)1 << 62;
	for (int i = 0; i < header->numPoints; i++)
	{
		s64 distance = BossCornerAssist_NavDistanceSq(driver, &frames[i]);
		if (distance < bestDistance)
		{
			bestDistance = distance;
			bestIndex = i;
		}
	}

	if (distanceOut != NULL)
	{
		*distanceOut = bestDistance;
	}
	return bestIndex;
}

static int BossCornerAssist_GetPathCurvature(int path, int index)
{
	if ((u32)path >= BOTS_NAV_PATH_COUNT)
	{
		return 0;
	}

	struct NavHeader *header = sdata->NavPath_ptrHeader[path];
	struct NavFrame *frames = sdata->NavPath_ptrNavFrameArray[path];
	if (header == NULL || frames == NULL || header->numPoints <= 0 || index < 0)
	{
		return 0;
	}

	int currentIndex = BossCornerAssist_WrapIndex(index, header->numPoints);
	int futureIndex = BossCornerAssist_WrapIndex(index + BOSS_CORNER_ASSIST_CURVATURE_LOOKAHEAD, header->numPoints);
	int currentYaw = CTR_MipsSll(frames[currentIndex].rot[1], 4);
	int futureYaw = CTR_MipsSll(frames[futureIndex].rot[1], 4);
	return BossCornerAssist_AngleDelta(futureYaw, currentYaw);
}

static int BossCornerAssist_CurvatureDirection(int curvature)
{
	if (curvature > 0)
	{
		return BOSS_CORNER_ASSIST_DIR_LEFT;
	}
	if (curvature < 0)
	{
		return BOSS_CORNER_ASSIST_DIR_RIGHT;
	}
	return BOSS_CORNER_ASSIST_DIR_NONE;
}

static int BossCornerAssist_FindIntentPath(struct Driver *driver, int intentDirection, int *pathOut, int *indexOut)
{
	s64 engageDistanceSq = (s64)BOSS_CORNER_ASSIST_ENGAGE_DISTANCE * BOSS_CORNER_ASSIST_ENGAGE_DISTANCE;
	s64 bestDistance = (s64)1 << 62;
	int bestPath = -1;
	int bestIndex = -1;

	for (int path = 0; path < BOTS_NAV_PATH_COUNT; path++)
	{
		s64 distance;
		int index = BossCornerAssist_FindNearestOnPath(driver, path, &distance);
		if (index < 0 || distance > engageDistanceSq)
		{
			continue;
		}

		int curvature = BossCornerAssist_GetPathCurvature(path, index);
		if (BossCornerAssist_Abs(curvature) < BOSS_CORNER_ASSIST_ENGAGE_CURVATURE_MIN ||
		    BossCornerAssist_CurvatureDirection(curvature) != intentDirection)
		{
			continue;
		}

		if (distance < bestDistance)
		{
			bestDistance = distance;
			bestPath = path;
			bestIndex = index;
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

static int BossCornerAssist_GetPlayerIntent(struct Driver *driver)
{
	struct GamepadBuffer *pad = &sdata->gGamepads->gamepad[driver->driverID];
	int stickLX = pad->stickLX;

	if (stickLX <= BOSS_CORNER_ASSIST_STICK_CENTER - BOSS_CORNER_ASSIST_INTENT_STICK_THRESHOLD)
	{
		return BOSS_CORNER_ASSIST_DIR_LEFT;
	}
	if (stickLX >= BOSS_CORNER_ASSIST_STICK_CENTER + BOSS_CORNER_ASSIST_INTENT_STICK_THRESHOLD)
	{
		return BOSS_CORNER_ASSIST_DIR_RIGHT;
	}

	if (driver->kartState == KS_DRIFTING)
	{
		return driver->multDrift < 0 ? BOSS_CORNER_ASSIST_DIR_RIGHT : BOSS_CORNER_ASSIST_DIR_LEFT;
	}

	int simpTurnState = (s8)driver->simpTurnState;
	if (simpTurnState > 4)
	{
		return BOSS_CORNER_ASSIST_DIR_LEFT;
	}
	if (simpTurnState < -4)
	{
		return BOSS_CORNER_ASSIST_DIR_RIGHT;
	}

	return BOSS_CORNER_ASSIST_DIR_NONE;
}

static int BossCornerAssist_ComputeSteerByte(struct Driver *driver, struct BossCornerAssistState *state, int *headingErrorOut)
{
	if ((u32)state->navPath >= BOTS_NAV_PATH_COUNT || state->navIndex < 0)
	{
		if (headingErrorOut != NULL)
		{
			*headingErrorOut = 0;
		}
		return BOSS_CORNER_ASSIST_STICK_CENTER;
	}

	struct NavHeader *header = sdata->NavPath_ptrHeader[state->navPath];
	struct NavFrame *frames = sdata->NavPath_ptrNavFrameArray[state->navPath];
	if (header == NULL || frames == NULL || header->numPoints <= 0)
	{
		if (headingErrorOut != NULL)
		{
			*headingErrorOut = 0;
		}
		return BOSS_CORNER_ASSIST_STICK_CENTER;
	}

	int speed = BossCornerAssist_Abs(driver->speedApprox);
	int lookahead = BOSS_CORNER_ASSIST_TARGET_LOOKAHEAD_MIN + CTR_MipsSra(speed, 12);
	if (lookahead > BOSS_CORNER_ASSIST_TARGET_LOOKAHEAD_MAX)
	{
		lookahead = BOSS_CORNER_ASSIST_TARGET_LOOKAHEAD_MAX;
	}

	int curvature = BossCornerAssist_Abs(BossCornerAssist_GetPathCurvature(state->navPath, state->navIndex));
	if (curvature > 0x100 && lookahead > BOSS_CORNER_ASSIST_TARGET_LOOKAHEAD_MIN + 2)
	{
		lookahead -= 2;
	}

	int targetIndex = BossCornerAssist_WrapIndex(state->navIndex + lookahead, header->numPoints);
	int driverX = CTR_MipsSra(driver->posCurr.x, FRACTIONAL_BITS_8);
	int driverZ = CTR_MipsSra(driver->posCurr.z, FRACTIONAL_BITS_8);
	int dx = CTR_MipsSubLo(frames[targetIndex].pos.x, driverX);
	int dz = CTR_MipsSubLo(frames[targetIndex].pos.z, driverZ);
	int pointYaw = ratan2(dx, dz) & 0xfff;
	int pathYaw = CTR_MipsSll(frames[targetIndex].rot[1], 4) & 0xfff;
	int pointError = BossCornerAssist_AngleDelta(pointYaw, driver->angle);
	int pathError = BossCornerAssist_AngleDelta(pathYaw, driver->angle);
	int headingError = CTR_MipsSra(CTR_MipsAddLo(CTR_MipsMulLo(pointError, 3), pathError), 2);

	if (headingErrorOut != NULL)
	{
		*headingErrorOut = headingError;
	}

	int errorAbs = BossCornerAssist_Abs(headingError);
	if (errorAbs <= 8)
	{
		return BOSS_CORNER_ASSIST_STICK_CENTER;
	}

	int offset = BOSS_CORNER_ASSIST_STEER_OFFSET_MIN +
	             CTR_MipsDiv(CTR_MipsMulLo(errorAbs, BOSS_CORNER_ASSIST_STEER_OFFSET_MAX - BOSS_CORNER_ASSIST_STEER_OFFSET_MIN),
	                         BOSS_CORNER_ASSIST_STEER_FULL_ERROR);
	if (offset > BOSS_CORNER_ASSIST_STEER_OFFSET_MAX)
	{
		offset = BOSS_CORNER_ASSIST_STEER_OFFSET_MAX;
	}

	int stick = BOSS_CORNER_ASSIST_STICK_CENTER;
	if (headingError > 0)
	{
		stick = CTR_MipsSubLo(stick, offset);
	}
	else
	{
		stick = CTR_MipsAddLo(stick, offset);
	}

	if (stick < 0)
	{
		stick = 0;
	}
	else if (stick > 0xff)
	{
		stick = 0xff;
	}
	return stick;
}

static int BossCornerAssist_IsPadInjectionAllowed(struct Driver *driver)
{
	return driver->kartState == KS_NORMAL || driver->kartState == KS_DRIFTING;
}

static int BossCornerAssist_PreparePad(struct Driver *driver, struct BossCornerAssistPadBackup *backup, int includeTap)
{
	struct BossCornerAssistState *state = BossCornerAssist_GetState(driver);
	if (state == NULL || !state->initialized || !state->enabled || !state->engaged || !BossCornerAssist_IsPadInjectionAllowed(driver))
	{
		return 0;
	}

	struct GamepadBuffer *pad = &sdata->gGamepads->gamepad[driver->driverID];
	backup->stickLX = pad->stickLX;
	backup->buttonsHeld = pad->buttonsHeldCurrFrame;
	backup->buttonsTapped = pad->buttonsTapped;

	pad->stickLX = state->steerByte;

	u32 driftButton = driver->buttonUsedToStartDrift & VEH_PHYS_PROC_JUMP_BUTTON_MASK;
	if (driftButton == 0 || state->driftTapPending)
	{
		driftButton = VEH_PHYS_PROC_DEFAULT_DRIFT_BUTTON;
	}
	pad->buttonsHeldCurrFrame |= driftButton;

	if (includeTap && state->driftTapPending && driver->kartState == KS_NORMAL)
	{
		pad->buttonsTapped |= VEH_PHYS_PROC_DEFAULT_DRIFT_BUTTON;
	}

	return 1;
}

static void BossCornerAssist_RestorePad(struct Driver *driver, const struct BossCornerAssistPadBackup *backup)
{
	struct GamepadBuffer *pad = &sdata->gGamepads->gamepad[driver->driverID];
	pad->stickLX = backup->stickLX;
	pad->buttonsHeldCurrFrame = backup->buttonsHeld;
	pad->buttonsTapped = backup->buttonsTapped;
}

static void BossCornerAssist_DrivingUpdate(struct Thread *thread, struct Driver *driver)
{
	struct BossCornerAssistPadBackup backup;
	int injected = BossCornerAssist_PreparePad(driver, &backup, 0);
	VehPhysProc_Driving_Update(thread, driver);
	if (injected)
	{
		BossCornerAssist_RestorePad(driver, &backup);
	}
	BossCornerAssist_InstallHooks(driver);
}

static void BossCornerAssist_DrivingPhysLinear(struct Thread *thread, struct Driver *driver)
{
	struct BossCornerAssistPadBackup backup;
	struct BossCornerAssistState *state = BossCornerAssist_GetState(driver);
	int consumeTap = state != NULL && state->driftTapPending && driver->kartState == KS_NORMAL;
	int injected = BossCornerAssist_PreparePad(driver, &backup, 1);
	VehPhysProc_Driving_PhysLinear(thread, driver);
	if (injected)
	{
		BossCornerAssist_RestorePad(driver, &backup);
		if (consumeTap)
		{
			state->driftTapPending = 0;
			state->driftRetryFrames = BOSS_CORNER_ASSIST_DRIFT_RETRY_FRAMES;
		}
	}
}

static void BossCornerAssist_PowerSlideUpdate(struct Thread *thread, struct Driver *driver)
{
	struct BossCornerAssistPadBackup backup;
	int injected = BossCornerAssist_PreparePad(driver, &backup, 0);
	VehPhysProc_PowerSlide_Update(thread, driver);
	if (injected)
	{
		BossCornerAssist_RestorePad(driver, &backup);
	}
	BossCornerAssist_InstallHooks(driver);
}

static void BossCornerAssist_PowerSlidePhysLinear(struct Thread *thread, struct Driver *driver)
{
	struct BossCornerAssistPadBackup backup;
	int injected = BossCornerAssist_PreparePad(driver, &backup, 0);
	VehPhysProc_PowerSlide_PhysLinear(thread, driver);
	if (injected)
	{
		BossCornerAssist_RestorePad(driver, &backup);
	}
}

static void BossCornerAssist_InstallHooks(struct Driver *driver)
{
	struct BossCornerAssistState *state = BossCornerAssist_GetState(driver);
	if (state == NULL || !state->initialized || !state->enabled || !state->engaged)
	{
		BossCornerAssist_RestoreHooks(driver);
		return;
	}

	if (driver->kartState == KS_NORMAL)
	{
		driver->funcPtrs[DRIVER_FUNC_UPDATE] = BossCornerAssist_DrivingUpdate;
		driver->funcPtrs[DRIVER_FUNC_PHYS_LINEAR] = BossCornerAssist_DrivingPhysLinear;
	}
	else if (driver->kartState == KS_DRIFTING)
	{
		driver->funcPtrs[DRIVER_FUNC_UPDATE] = BossCornerAssist_PowerSlideUpdate;
		driver->funcPtrs[DRIVER_FUNC_PHYS_LINEAR] = BossCornerAssist_PowerSlidePhysLinear;
	}
	else
	{
		BossCornerAssist_RestoreHooks(driver);
	}
}

static void BossCornerAssist_Update(struct Driver *driver)
{
	struct BossCornerAssistState *state = BossCornerAssist_GetState(driver);
	struct GameTracker *gGT = sdata->gGT;
	if (state == NULL || gGT == NULL)
	{
		return;
	}

	const struct BossPlayableProfile *profile = BossPlayable_GetProfile(driver);
	if (profile == NULL)
	{
		if (state->initialized)
		{
			BossCornerAssist_RestoreHooks(driver);
			memset(state, 0, sizeof(*state));
		}
		return;
	}

	int characterID = BossPlayable_GetCharacterID(driver);
	if (!state->initialized || state->characterID != characterID || state->levelID != gGT->levelID)
	{
		BossCornerAssist_ResetState(driver, state, characterID, gGT->levelID);
	}

	struct GamepadBuffer *pad = &sdata->gGamepads->gamepad[driver->driverID];
	if ((pad->buttonsTapped & BTN_L3) != 0)
	{
		state->enabled ^= 1;
		BossCornerAssist_Disengage(driver, state);
	}

	if (!state->enabled || gGT->trafficLightsTimer > 0 || (driver->actionsFlagSet & ACTION_RACE_FINISHED) != 0 ||
	    (gGT->gameMode1 & END_OF_RACE) != 0)
	{
		BossCornerAssist_RestoreHooks(driver);
		return;
	}

	if (!state->engaged)
	{
		int intentDirection = BossCornerAssist_GetPlayerIntent(driver);
		int path;
		int index;
		if (intentDirection == BOSS_CORNER_ASSIST_DIR_NONE || !BossCornerAssist_FindIntentPath(driver, intentDirection, &path, &index))
		{
			BossCornerAssist_RestoreHooks(driver);
			return;
		}

		state->engaged = 1;
		state->intentDirection = (s8)intentDirection;
		state->navPath = (s16)path;
		state->navIndex = (s16)index;
		state->straightFrames = 0;
		state->driftRetryFrames = 0;
		state->driftTapPending = driver->kartState == KS_DRIFTING ? 0 : 1;
	}

	s64 pathDistance;
	int nearestIndex = BossCornerAssist_FindNearestOnPath(driver, state->navPath, &pathDistance);
	s64 lostDistanceSq = (s64)BOSS_CORNER_ASSIST_LOST_DISTANCE * BOSS_CORNER_ASSIST_LOST_DISTANCE;
	if (nearestIndex < 0 || pathDistance > lostDistanceSq)
	{
		BossCornerAssist_Disengage(driver, state);
		return;
	}
	state->navIndex = (s16)nearestIndex;

	int curvature = BossCornerAssist_GetPathCurvature(state->navPath, state->navIndex);
	int curvatureAbs = BossCornerAssist_Abs(curvature);
	int curvatureDirection = BossCornerAssist_CurvatureDirection(curvature);
	if (curvatureAbs >= BOSS_CORNER_ASSIST_ENGAGE_CURVATURE_MIN && curvatureDirection != state->intentDirection)
	{
		BossCornerAssist_Disengage(driver, state);
		return;
	}

	int headingError;
	state->steerByte = (u8)BossCornerAssist_ComputeSteerByte(driver, state, &headingError);

	if (curvatureAbs <= BOSS_CORNER_ASSIST_EXIT_CURVATURE_MAX && BossCornerAssist_Abs(headingError) <= BOSS_CORNER_ASSIST_EXIT_HEADING_MAX)
	{
		if (state->straightFrames < 0xff)
		{
			state->straightFrames++;
		}
	}
	else
	{
		state->straightFrames = 0;
	}

	if (state->straightFrames >= BOSS_CORNER_ASSIST_EXIT_STRAIGHT_FRAMES)
	{
		BossCornerAssist_Disengage(driver, state);
		return;
	}

	if (driver->kartState == KS_DRIFTING)
	{
		state->driftTapPending = 0;
		state->driftRetryFrames = 0;
	}
	else if (!state->driftTapPending)
	{
		if (state->driftRetryFrames > 0)
		{
			state->driftRetryFrames--;
		}
		else if (driver->kartState == KS_NORMAL)
		{
			state->driftTapPending = 1;
		}
	}

	BossCornerAssist_InstallHooks(driver);
}

void VehFrameProc_Driving(struct Thread *thread, struct Driver *driver)
{
	VehFrameProc_Driving_BossPlayable(thread, driver);
	BossCornerAssist_Update(driver);
}
