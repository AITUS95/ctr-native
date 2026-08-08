// Keep the curve-assist implementation in BossAutopilot.c, but wrap its public
// frameproc so L3 only arms the assist. The retail BOTS takeover is allowed only
// after the player actually starts steering into an approaching authored curve.
#define VehFrameProc_Driving VehFrameProc_Driving_CurveAssistInternal
#include "BossAutopilot.c"
#undef VehFrameProc_Driving

enum
{
	BOSS_CURVE_ASSIST_HUMAN_STEER_THRESHOLD = 8,
};

static int BossCurveAssist_HumanIsSteering(struct Driver *driver)
{
	int steer = driver != NULL ? (int)driver->simpTurnState : 0;
	if (steer < 0)
	{
		steer = CTR_MipsNegLo(steer);
	}

	return steer >= BOSS_CURVE_ASSIST_HUMAN_STEER_THRESHOLD;
}

static void BossCurveAssist_PreBrakePlayer(struct Driver *driver, struct BossCurveInfo info)
{
	if (driver == NULL)
	{
		return;
	}

	int speedCap = BossCurveAssist_GetSpeedCap(info);
	int speedApprox = driver->speedApprox;
	int absSpeed = speedApprox;
	if (absSpeed < 0)
	{
		absSpeed = CTR_MipsNegLo(absSpeed);
	}

	if (absSpeed <= speedCap || absSpeed == 0)
	{
		return;
	}

	// Brake the actual horizontal motion while the player still owns the kart.
	// This runs every frame while a sharp bend is ahead, so a full-speed approach
	// is reduced decisively before the player begins the steering gesture that
	// hands the corner itself to BOTS.
	driver->xSpeed = CTR_MipsDiv(CTR_MipsMulLo(driver->xSpeed, speedCap), absSpeed);
	driver->zSpeed = CTR_MipsDiv(CTR_MipsMulLo(driver->zSpeed, speedCap), absSpeed);
	driver->speedApprox = (s16)((speedApprox < 0) ? CTR_MipsNegLo(speedCap) : speedCap);

	if (driver->speed > speedCap)
	{
		driver->speed = speedCap;
	}
	else if (driver->speed < -speedCap)
	{
		driver->speed = (s16)CTR_MipsNegLo(speedCap);
	}
}

void VehFrameProc_Driving(struct Thread *thread, struct Driver *driver)
{
	struct GameTracker *gGT = sdata->gGT;
	if (thread == NULL || driver == NULL || gGT == NULL || (u32)driver->driverID >= BOSS_CURVE_ASSIST_MAX_DRIVERS)
	{
		VehFrameProc_Driving_CurveAssistInternal(thread, driver);
		return;
	}

	struct BossCurveAssistState *state = BossCurveAssist_GetState(driver);
	int characterID = BossCurveAssist_GetCharacterID(driver);
	if (!state->initialized || state->characterID != characterID || state->levelID != gGT->levelID)
	{
		BossCurveAssist_ResetState(driver, state);
	}

	// While BOTS owns an assisted corner, keep the existing implementation in
	// charge of curve completion, recovery states, L3-off, and the clean camera
	// handoff back to human physics.
	if (state->botActive || !BossCurveAssist_IsBossCharacter(characterID))
	{
		VehFrameProc_Driving_CurveAssistInternal(thread, driver);
		return;
	}

	// L3 is now strictly an arm/disarm gesture. Do not evaluate or enter BOTS in
	// the same frame as the click, even if the kart happens to be near a bend.
	if (BossCurveAssist_IsL3Tapped(driver) && state->lastToggleFrame != gGT->timer)
	{
		state->lastToggleFrame = gGT->timer;
		state->assistEnabled ^= 1;
		VehFrameProc_Driving_BossPlayable(thread, driver);
		return;
	}

	if (!state->assistEnabled)
	{
		VehFrameProc_Driving_CurveAssistInternal(thread, driver);
		return;
	}

	// Damage/warp states remain entirely under the normal player pipeline until
	// ordinary driving resumes.
	if ((driver->actionsFlagSet & (ACTION_BOT | ACTION_RACE_FINISHED)) != 0 ||
	    gGT->trafficLightsTimer > 0 ||
	    (driver->kartState != KS_NORMAL && driver->kartState != KS_DRIFTING && driver->kartState != KS_ANTIVSHIFT))
	{
		int savedAssistEnabled = state->assistEnabled;
		state->assistEnabled = 0;
		VehFrameProc_Driving_CurveAssistInternal(thread, driver);
		state->assistEnabled = savedAssistEnabled;
		return;
	}

	int path;
	int index;
	struct BossCurveInfo info;
	int approachingCurve = BossCurveAssist_IsApproachingCurve(driver, &path, &index, &info);

	if (!approachingCurve)
	{
		// Keep full manual ownership on straights. Temporarily hide the armed flag
		// from the old update routine so it cannot start BOTS on its own.
		int savedAssistEnabled = state->assistEnabled;
		state->assistEnabled = 0;
		VehFrameProc_Driving_CurveAssistInternal(thread, driver);
		state->assistEnabled = savedAssistEnabled;
		return;
	}

	// A bend ahead may brake the player, but steering remains manual until the
	// player makes a real turn input. This is the key distinction from autopilot.
	BossCurveAssist_PreBrakePlayer(driver, info);

	if (!BossCurveAssist_HumanIsSteering(driver))
	{
		int savedAssistEnabled = state->assistEnabled;
		state->assistEnabled = 0;
		VehFrameProc_Driving_CurveAssistInternal(thread, driver);
		state->assistEnabled = savedAssistEnabled;
		return;
	}

	// The player has started the corner: let the existing implementation convert
	// to BOTS, execute the authored line, and release on the straight exit.
	VehFrameProc_Driving_CurveAssistInternal(thread, driver);
}
