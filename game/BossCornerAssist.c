// Keep the curve-assist implementation in BossAutopilot.c, but wrap its public
// frameproc so L3 only arms the assist. The retail BOTS takeover is allowed only
// after the player actually starts a NEW steering gesture into an approaching
// authored curve after arming the assist.
#define VehFrameProc_Driving VehFrameProc_Driving_CurveAssistInternal
#include "BossAutopilot.c"
#undef VehFrameProc_Driving

enum
{
	BOSS_CURVE_ASSIST_HUMAN_STEER_THRESHOLD = 8,
};

// Arming must never fall through into BOTS, even if VehFrameProc_Driving runs
// more than once during the same game frame. Requiring a neutral sample after
// L3 also prevents stale steering state from being mistaken for a new corner.
static u32 s_bossCurveAssistArmFrame[BOSS_CURVE_ASSIST_MAX_DRIVERS];
static u8 s_bossCurveAssistNeedNeutral[BOSS_CURVE_ASSIST_MAX_DRIVERS];

static int BossCurveAssist_HumanIsSteering(struct Driver *driver)
{
	int steer = driver != NULL ? (int)driver->simpTurnState : 0;
	if (steer < 0)
	{
		steer = CTR_MipsNegLo(steer);
	}

	return steer >= BOSS_CURVE_ASSIST_HUMAN_STEER_THRESHOLD;
}

static void BossCurveAssist_RunManualFrame(struct Thread *thread, struct Driver *driver, struct BossCurveAssistState *state)
{
	// Hide the armed flag from the older internal update routine so it cannot
	// call BOTS_Driver_Convert while this wrapper intentionally keeps control
	// with the player.
	int savedAssistEnabled = state->assistEnabled;
	state->assistEnabled = 0;
	VehFrameProc_Driving_CurveAssistInternal(thread, driver);
	state->assistEnabled = savedAssistEnabled;
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
	// is reduced decisively before a fresh steering gesture hands the corner to
	// BOTS.
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
		s_bossCurveAssistArmFrame[driver->driverID] = (u32)-1;
		s_bossCurveAssistNeedNeutral[driver->driverID] = 0;
	}

	// While BOTS already owns an assisted corner, keep the existing implementation
	// in charge of curve completion, recovery states, L3-off, and clean handoff.
	if (state->botActive || !BossCurveAssist_IsBossCharacter(characterID))
	{
		VehFrameProc_Driving_CurveAssistInternal(thread, driver);
		return;
	}

	// L3 is strictly an arm/disarm gesture. On arm, remember this frame and force
	// a fresh neutral -> steering sequence before BOTS can ever be entered.
	if (BossCurveAssist_IsL3Tapped(driver) && state->lastToggleFrame != gGT->timer)
	{
		state->lastToggleFrame = gGT->timer;
		state->assistEnabled ^= 1;

		if (state->assistEnabled)
		{
			s_bossCurveAssistArmFrame[driver->driverID] = gGT->timer;
			s_bossCurveAssistNeedNeutral[driver->driverID] = 1;
		}
		else
		{
			s_bossCurveAssistArmFrame[driver->driverID] = (u32)-1;
			s_bossCurveAssistNeedNeutral[driver->driverID] = 0;
		}

		VehFrameProc_Driving_BossPlayable(thread, driver);
		return;
	}

	if (!state->assistEnabled)
	{
		BossCurveAssist_RunManualFrame(thread, driver, state);
		return;
	}

	// Critical same-frame guard: VehFrameProc_Driving may run again after the L3
	// handler above. Never let that second pass see an armed assist as permission
	// to convert the driver into BOTS.
	if (s_bossCurveAssistArmFrame[driver->driverID] == gGT->timer)
	{
		BossCurveAssist_RunManualFrame(thread, driver, state);
		return;
	}

	// Require the steering state to become neutral at least once after L3 ON.
	// If L3 was pressed while turning, that existing/stale turn cannot trigger
	// takeover; the player must release/center and then start a new turn.
	if (s_bossCurveAssistNeedNeutral[driver->driverID])
	{
		if (!BossCurveAssist_HumanIsSteering(driver))
		{
			s_bossCurveAssistNeedNeutral[driver->driverID] = 0;
		}

		BossCurveAssist_RunManualFrame(thread, driver, state);
		return;
	}

	// Damage/warp states remain entirely under the normal player pipeline until
	// ordinary driving resumes.
	if ((driver->actionsFlagSet & (ACTION_BOT | ACTION_RACE_FINISHED)) != 0 ||
	    gGT->trafficLightsTimer > 0 ||
	    (driver->kartState != KS_NORMAL && driver->kartState != KS_DRIFTING && driver->kartState != KS_ANTIVSHIFT))
	{
		BossCurveAssist_RunManualFrame(thread, driver, state);
		return;
	}

	int path;
	int index;
	struct BossCurveInfo info;
	int approachingCurve = BossCurveAssist_IsApproachingCurve(driver, &path, &index, &info);

	if (!approachingCurve)
	{
		BossCurveAssist_RunManualFrame(thread, driver, state);
		return;
	}

	// A bend ahead may brake the player, but steering remains manual until a NEW
	// steering gesture after arming exceeds the threshold.
	BossCurveAssist_PreBrakePlayer(driver, info);

	if (!BossCurveAssist_HumanIsSteering(driver))
	{
		BossCurveAssist_RunManualFrame(thread, driver, state);
		return;
	}

	// Only this path may enter BOTS: assist was armed on an earlier frame, the
	// player has centered the steering since then, a curve is ahead, and the
	// player has now deliberately started a new turn.
	VehFrameProc_Driving_CurveAssistInternal(thread, driver);
}
