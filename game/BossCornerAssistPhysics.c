#include <common.h>

// Manual-only curve assist for playable Story bosses.
//
// There is no toggle and no BOTS takeover. The assist is context-sensitive:
// it can run only while the player is actively steering or powersliding. The
// authored BOTS nav path is used only as a racing-line reference while normal
// player physics remain in full control of gas, brake, jump, drift and weapons.

enum
{
	BOSS_MANUAL_CURVE_NAV_PATHS = 3,
	BOSS_MANUAL_CURVE_ANGLE_MASK = 0xfff,
	BOSS_MANUAL_CURVE_ANGLE_HALF = 0x800,

	// A long window can anticipate braking, while steering requires the bend to
	// be local to the kart so a distant corner never pulls the player sideways.
	BOSS_MANUAL_CURVE_BRAKE_LOOKAHEAD = 12,
	BOSS_MANUAL_CURVE_STEER_LOOKAHEAD = 4,
	BOSS_MANUAL_CURVE_TARGET_AHEAD = 2,
	BOSS_MANUAL_CURVE_BRAKE_ENTRY_ANGLE = 0x100,
	BOSS_MANUAL_CURVE_STEER_ENTRY_ANGLE = 0x40,
	BOSS_MANUAL_CURVE_STEER_TARGET_DELTA = 0x30,

	BOSS_MANUAL_CURVE_MEDIUM = 0x200,
	BOSS_MANUAL_CURVE_TIGHT = 0x320,
	BOSS_MANUAL_CURVE_HAIRPIN = 0x480,
	BOSS_MANUAL_CURVE_SPEED_GENTLE = 0x5600,
	BOSS_MANUAL_CURVE_SPEED_MEDIUM = 0x4c00,
	BOSS_MANUAL_CURVE_SPEED_TIGHT = 0x4200,
	BOSS_MANUAL_CURVE_SPEED_HAIRPIN = 0x3600,

	BOSS_MANUAL_CURVE_MIN_STEER = 10,
	BOSS_MANUAL_CURVE_STEER_DIVISOR = 8,
	BOSS_MANUAL_CURVE_STEER_MAX = 0x58,
	BOSS_MANUAL_CURVE_YAW_CORRECTION_MAX = 0x18,
	BOSS_MANUAL_CURVE_YAW_CORRECTION_DIV = 6,
};

struct BossManualCurveInfo
{
	int severityAhead;
	int severityLocal;
	int driftAhead;
	int path;
	int nearestIndex;
	int nearestYaw;
	int targetYaw;
};

static int BossManualCurve_GetCharacterID(struct Driver *driver)
{
	if (driver == NULL || (u32)driver->driverID >= 8)
	{
		return -1;
	}

	return data.characterIDs[driver->driverID];
}

static int BossManualCurve_IsBossCharacter(int characterID)
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

static int BossManualCurve_SignedAngleDelta(int target, int current)
{
	int delta = CTR_MipsSubLo(target, current) & BOSS_MANUAL_CURVE_ANGLE_MASK;
	if (delta >= BOSS_MANUAL_CURVE_ANGLE_HALF)
	{
		delta = CTR_MipsSubLo(delta, BOSS_MANUAL_CURVE_ANGLE_MASK + 1);
	}
	return delta;
}

static int BossManualCurve_Abs(int value)
{
	return value < 0 ? CTR_MipsNegLo(value) : value;
}

static int BossManualCurve_SameDirection(int a, int b)
{
	return (a < 0 && b < 0) || (a > 0 && b > 0);
}

static int BossManualCurve_PlayerRequestsAssist(struct Driver *driver)
{
	if (driver == NULL)
	{
		return 0;
	}

	// simpTurnState has already passed through CTR's normal stick dead zone by
	// the time angular physics runs, so non-zero means genuine player steering.
	// Powerslide remains eligible even if the stick is momentarily centered.
	return ((s8)driver->simpTurnState != 0) || driver->kartState == KS_DRIFTING;
}

static int BossManualCurve_CanAssist(struct Driver *driver)
{
	struct GameTracker *gGT = sdata->gGT;
	if (driver == NULL || gGT == NULL)
	{
		return 0;
	}

	if (!BossManualCurve_IsBossCharacter(BossManualCurve_GetCharacterID(driver)))
	{
		return 0;
	}

	if (!BossManualCurve_PlayerRequestsAssist(driver))
	{
		return 0;
	}

	if ((driver->actionsFlagSet & (ACTION_BOT | ACTION_RACE_FINISHED)) != 0 || gGT->trafficLightsTimer > 0)
	{
		return 0;
	}

	if (driver->kartState != KS_NORMAL && driver->kartState != KS_DRIFTING && driver->kartState != KS_ANTIVSHIFT)
	{
		return 0;
	}

	return 1;
}

static s64 BossManualCurve_NavScore(struct Driver *driver, const struct NavFrame *frame)
{
	int driverX = CTR_MipsSra(driver->posCurr.x, FRACTIONAL_BITS_8);
	int driverZ = CTR_MipsSra(driver->posCurr.z, FRACTIONAL_BITS_8);
	int dx = CTR_MipsSubLo(driverX, frame->pos.x);
	int dz = CTR_MipsSubLo(driverZ, frame->pos.z);
	int navYaw = CTR_MipsSll(frame->rot[1], 4) & BOSS_MANUAL_CURVE_ANGLE_MASK;
	int headingError = BossManualCurve_Abs(BossManualCurve_SignedAngleDelta(navYaw, driver->angle));

	// Distance chooses the local racing line; heading rejects nearby branches at
	// crossings and parallel pieces that face the wrong way.
	return ((s64)dx * dx + (s64)dz * dz) + ((s64)headingError * headingError << 2);
}

static int BossManualCurve_FindClosestNav(struct Driver *driver, int *pathOut, int *indexOut)
{
	int bestPath = -1;
	int bestIndex = -1;
	s64 bestScore = (s64)1 << 62;

	for (int path = 0; path < BOSS_MANUAL_CURVE_NAV_PATHS; path++)
	{
		struct NavHeader *header = sdata->NavPath_ptrHeader[path];
		struct NavFrame *frames = sdata->NavPath_ptrNavFrameArray[path];
		if (header == NULL || frames == NULL || header->numPoints <= 1)
		{
			continue;
		}

		for (int index = 0; index < header->numPoints; index++)
		{
			s64 score = BossManualCurve_NavScore(driver, &frames[index]);
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

static int BossManualCurve_WrapIndex(struct NavHeader *header, int index)
{
	while (index >= header->numPoints)
	{
		index -= header->numPoints;
	}
	while (index < 0)
	{
		index += header->numPoints;
	}
	return index;
}

static int BossManualCurve_Analyze(struct Driver *driver, struct BossManualCurveInfo *info)
{
	int path;
	int nearestIndex;
	if (!BossManualCurve_FindClosestNav(driver, &path, &nearestIndex))
	{
		return 0;
	}

	struct NavHeader *header = sdata->NavPath_ptrHeader[path];
	struct NavFrame *frames = sdata->NavPath_ptrNavFrameArray[path];
	if (header == NULL || frames == NULL || header->numPoints <= 1)
	{
		return 0;
	}

	int severityAhead = 0;
	int severityLocal = 0;
	int driftAhead = 0;
	int index = nearestIndex;
	int prevYaw = CTR_MipsSll(frames[index].rot[1], 4) & BOSS_MANUAL_CURVE_ANGLE_MASK;

	for (int step = 0; step < BOSS_MANUAL_CURVE_BRAKE_LOOKAHEAD; step++)
	{
		if ((frames[index].flags & BOTS_NAV_FLAG_DRIFT_MASK) != 0)
		{
			driftAhead = 1;
		}

		int nextIndex = BossManualCurve_WrapIndex(header, index + 1);
		int nextYaw = CTR_MipsSll(frames[nextIndex].rot[1], 4) & BOSS_MANUAL_CURVE_ANGLE_MASK;
		int delta = BossManualCurve_Abs(BossManualCurve_SignedAngleDelta(nextYaw, prevYaw));

		severityAhead = CTR_MipsAddLo(severityAhead, delta);
		if (step < BOSS_MANUAL_CURVE_STEER_LOOKAHEAD)
		{
			severityLocal = CTR_MipsAddLo(severityLocal, delta);
		}

		if (severityAhead > BOSS_MANUAL_CURVE_ANGLE_MASK)
		{
			severityAhead = BOSS_MANUAL_CURVE_ANGLE_MASK;
		}
		if (severityLocal > BOSS_MANUAL_CURVE_ANGLE_MASK)
		{
			severityLocal = BOSS_MANUAL_CURVE_ANGLE_MASK;
		}

		prevYaw = nextYaw;
		index = nextIndex;
	}

	int targetIndex = BossManualCurve_WrapIndex(header, nearestIndex + BOSS_MANUAL_CURVE_TARGET_AHEAD);
	info->severityAhead = severityAhead;
	info->severityLocal = severityLocal;
	info->driftAhead = driftAhead;
	info->path = path;
	info->nearestIndex = nearestIndex;
	info->nearestYaw = CTR_MipsSll(frames[nearestIndex].rot[1], 4) & BOSS_MANUAL_CURVE_ANGLE_MASK;
	info->targetYaw = CTR_MipsSll(frames[targetIndex].rot[1], 4) & BOSS_MANUAL_CURVE_ANGLE_MASK;

	return driftAhead || severityAhead >= BOSS_MANUAL_CURVE_BRAKE_ENTRY_ANGLE;
}

static int BossManualCurve_ShouldSteer(const struct BossManualCurveInfo *info)
{
	int targetDelta = BossManualCurve_Abs(BossManualCurve_SignedAngleDelta(info->targetYaw, info->nearestYaw));

	if (info->severityLocal < BOSS_MANUAL_CURVE_STEER_ENTRY_ANGLE)
	{
		return 0;
	}
	if (targetDelta < BOSS_MANUAL_CURVE_STEER_TARGET_DELTA)
	{
		return 0;
	}

	return 1;
}

static int BossManualCurve_GetSpeedCap(const struct BossManualCurveInfo *info)
{
	if (info->severityAhead >= BOSS_MANUAL_CURVE_HAIRPIN)
	{
		return BOSS_MANUAL_CURVE_SPEED_HAIRPIN;
	}
	if (info->severityAhead >= BOSS_MANUAL_CURVE_TIGHT)
	{
		return BOSS_MANUAL_CURVE_SPEED_TIGHT;
	}
	if (info->severityAhead >= BOSS_MANUAL_CURVE_MEDIUM || info->driftAhead)
	{
		return BOSS_MANUAL_CURVE_SPEED_MEDIUM;
	}
	return BOSS_MANUAL_CURVE_SPEED_GENTLE;
}

static void BossManualCurve_ClampSpeed(struct Driver *driver, const struct BossManualCurveInfo *info)
{
	int cap = BossManualCurve_GetSpeedCap(info);
	int speedApprox = driver->speedApprox;
	int absSpeed = BossManualCurve_Abs(speedApprox);

	if (absSpeed > cap && absSpeed != 0)
	{
		driver->xSpeed = CTR_MipsDiv(CTR_MipsMulLo(driver->xSpeed, cap), absSpeed);
		driver->zSpeed = CTR_MipsDiv(CTR_MipsMulLo(driver->zSpeed, cap), absSpeed);
		driver->speedApprox = (s16)(speedApprox < 0 ? CTR_MipsNegLo(cap) : cap);
	}

	if (driver->speed > cap)
	{
		driver->speed = (s16)cap;
	}
	else if (driver->speed < -cap)
	{
		driver->speed = (s16)CTR_MipsNegLo(cap);
	}

	if (driver->baseSpeed > cap)
	{
		driver->baseSpeed = (s16)cap;
	}
	if (driver->terrainScaledBaseSpeed > cap)
	{
		driver->terrainScaledBaseSpeed = (s16)cap;
	}
}

static int BossManualCurve_GetIdealSteer(struct Driver *driver, const struct BossManualCurveInfo *info)
{
	int delta = BossManualCurve_SignedAngleDelta(info->targetYaw, driver->angle);
	int absDelta = BossManualCurve_Abs(delta);
	if (absDelta < BOSS_MANUAL_CURVE_STEER_TARGET_DELTA)
	{
		return 0;
	}

	// Positive target yaw requires negative simpTurnState in CTR's player
	// steering convention, and vice versa.
	int steer = CTR_MipsDiv(CTR_MipsNegLo(delta), BOSS_MANUAL_CURVE_STEER_DIVISOR);
	if (steer > BOSS_MANUAL_CURVE_STEER_MAX)
	{
		steer = BOSS_MANUAL_CURVE_STEER_MAX;
	}
	else if (steer < -BOSS_MANUAL_CURVE_STEER_MAX)
	{
		steer = -BOSS_MANUAL_CURVE_STEER_MAX;
	}

	if (steer > 0 && steer < BOSS_MANUAL_CURVE_MIN_STEER)
	{
		steer = BOSS_MANUAL_CURVE_MIN_STEER;
	}
	else if (steer < 0 && steer > -BOSS_MANUAL_CURVE_MIN_STEER)
	{
		steer = -BOSS_MANUAL_CURVE_MIN_STEER;
	}

	return steer;
}

static int BossManualCurve_GetAssistedSteer(struct Driver *driver, int idealSteer)
{
	int playerSteer = (s8)driver->simpTurnState;

	// During a powerslide the nav correction may take the stronger role even if
	// the stick crosses center for a moment.
	if (driver->kartState == KS_DRIFTING)
	{
		return idealSteer;
	}

	if (playerSteer == 0 || idealSteer == 0)
	{
		return playerSteer;
	}

	// Never fight a deliberate opposite steering input. Assistance strengthens
	// only the direction the player has already chosen.
	if (!BossManualCurve_SameDirection(playerSteer, idealSteer))
	{
		return playerSteer;
	}

	int playerMagnitude = BossManualCurve_Abs(playerSteer);
	int idealMagnitude = BossManualCurve_Abs(idealSteer);
	if (idealMagnitude <= playerMagnitude)
	{
		return playerSteer;
	}

	// Blend toward the ideal line without replacing the player's steering feel.
	return CTR_MipsDiv(CTR_MipsAddLo(CTR_MipsMulLo(playerSteer, 2), idealSteer), 3);
}

// Normal human angular physics always owns the kart. Assistance exists only
// while the player is steering or powersliding; with centered steering in normal
// driving this function is exactly the retail angular path.
void VehPhysGeneral_PhysAngular(struct Thread *thread, struct Driver *driver)
{
	if (!BossManualCurve_CanAssist(driver))
	{
		VehPhysGeneral_PhysAngular_Original(thread, driver);
		return;
	}

	struct BossManualCurveInfo info;
	int curveAhead = BossManualCurve_Analyze(driver, &info);
	if (!curveAhead)
	{
		VehPhysGeneral_PhysAngular_Original(thread, driver);
		return;
	}

	// Since the player is already steering/drifting, braking is allowed to use
	// the longer lookahead to make tight entries safer.
	BossManualCurve_ClampSpeed(driver, &info);

	int steerAssist = BossManualCurve_ShouldSteer(&info);
	int assistedSteer = (s8)driver->simpTurnState;
	if (steerAssist)
	{
		int idealSteer = BossManualCurve_GetIdealSteer(driver, &info);
		assistedSteer = BossManualCurve_GetAssistedSteer(driver, idealSteer);
		driver->simpTurnState = (s8)assistedSteer;

		if (assistedSteer < 0)
		{
			driver->actionsFlagSet |= ACTION_STEER_LEFT;
		}
		else if (assistedSteer > 0)
		{
			driver->actionsFlagSet &= ~ACTION_STEER_LEFT;
		}
	}

	VehPhysGeneral_PhysAngular_Original(thread, driver);

	if (steerAssist)
	{
		int idealSteer = BossManualCurve_GetIdealSteer(driver, &info);
		int playerSteer = assistedSteer;

		// In normal steering, apply yaw correction only when the assisted direction
		// agrees with the ideal curve. A deliberate opposite input remains manual.
		if (driver->kartState == KS_DRIFTING ||
		    (playerSteer != 0 && idealSteer != 0 && BossManualCurve_SameDirection(playerSteer, idealSteer)))
		{
			int delta = BossManualCurve_SignedAngleDelta(info.targetYaw, driver->angle);
			int correction = CTR_MipsDiv(delta, BOSS_MANUAL_CURVE_YAW_CORRECTION_DIV);
			if (correction > BOSS_MANUAL_CURVE_YAW_CORRECTION_MAX)
			{
				correction = BOSS_MANUAL_CURVE_YAW_CORRECTION_MAX;
			}
			else if (correction < -BOSS_MANUAL_CURVE_YAW_CORRECTION_MAX)
			{
				correction = -BOSS_MANUAL_CURVE_YAW_CORRECTION_MAX;
			}

			driver->angle = (s16)(CTR_MipsAddLo(driver->angle, correction) & BOSS_MANUAL_CURVE_ANGLE_MASK);
			driver->rotCurr.y = (s16)CTR_MipsAddLo(driver->rotCurr.y, correction);
		}
	}
}
