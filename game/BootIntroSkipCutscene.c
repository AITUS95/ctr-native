#include <common.h>

// Native boot-skip extension for the already-loaded Naughty Dog box scene.
// Retail's CS_Thread_UseOpcode already has the correct teardown/load path, but
// it accepts only Start and refuses the skip until a minimum scene time. Keep
// every other cutscene on retail behavior and make only the boot box respond to
// any freshly-tapped digital button immediately.
int CS_Thread_UseOpcode(struct Instance *instance, struct CutsceneObj *cs)
{
#ifdef CTR_NATIVE
	struct GameTracker *gGT = sdata->gGT;
	struct GamepadSystem *gGS = sdata->gGamepads;

	if (instance == NULL && gGT != NULL && gGS != NULL &&
	    gGT->levelID == NAUGHTY_DOG_CRATE &&
	    gGS->gamepad[0].buttonsTapped != 0)
	{
		// Mirror the retail ND-box Start-skip teardown, but do it immediately
		// for any button. This requests a real level transition; no timer or
		// cutscene playback position is advanced artificially.
		gGT->clockEffectEnabled &= ~CAM_PATH_FLAG_CLOCK_EFFECT;
		RaceFlag_SetCanDraw(1);
		if (!RaceFlag_IsTransitioning() && !RaceFlag_IsFullyOnScreen())
		{
			RaceFlag_SetFullyOffScreen();
		}

		CseqMusic_StopAll();
		CDSYS_XAPauseRequest();
		RaceFlag_SetDrawOrder(0);
		MainRaceTrack_RequestLoad(MAIN_MENU_LEVEL);
		D233.isCutsceneOver = 1;
		gGT->gameMode2 &= ~VEH_FREEZE_PODIUM;
		return 1;
	}
#endif

	return CS_Thread_UseOpcode_Original(instance, cs);
}
