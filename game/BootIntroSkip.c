#include <common.h>

#ifdef CTR_NATIVE

enum
{
	BOOT_INTRO_SKIP_SONG_SYNC_TIME = 0x11c0,
};

static int BootIntroSkip_IsStartHeld(void)
{
	struct GamepadSystem *gGS = sdata->gGamepads;
	if (gGS == NULL)
	{
		return 0;
	}

	struct ControllerPacket *packet = gGS->gamepad[0].ptrControllerPacket;
	if (packet == NULL || packet->plugged != PLUGGED)
	{
		return 0;
	}

	// Controller packets are active-low. StateZero has already initialized the
	// controller before it enters the blocking SCEA/XA wait, so raw Start is the
	// only reliable input path this early in boot.
	return (packet->controllerInput1 & RAW_BTN_START) == 0;
}

static void BootIntroSkip_RequestMainMenu(void)
{
	struct GameTracker *gGT = sdata->gGT;
	if (gGT == NULL)
	{
		return;
	}

	// This wrapper is also used by the normal per-frame XA polling, so restrict
	// the shortcut to the one-time first-boot sequence only.
	if (sdata->boolFirstBoot == 0 || gGT->levelID != NAUGHTY_DOG_CRATE || sdata->XA_State == XA_IDLE)
	{
		return;
	}

	if (!BootIntroSkip_IsStartHeld())
	{
		return;
	}

	// Stop the spoken SCEA intro immediately. The first LOAD_TenStages pass would
	// normally wait for the intro CSEQ to reach 0x11c0 while showing Copyright;
	// advance that synchronization point too, so one Start press skips the whole
	// boot presentation instead of requiring another input later.
	CDSYS_XAPauseForce();
	if (sdata->songPool[0].timeSpentPlaying < BOOT_INTRO_SKIP_SONG_SYNC_TIME)
	{
		sdata->songPool[0].timeSpentPlaying = BOOT_INTRO_SKIP_SONG_SYNC_TIME;
	}

	// The first loader has not started yet, so redirect its initial level from
	// the Naughty Dog crate cutscene straight to the ordinary title/main-menu
	// level. LOAD_TenStages will derive MAIN_MENU mode from this level normally.
	gGT->levelID = MAIN_MENU_LEVEL;
	sdata->mainMenuState = MAIN_MENU_TITLE;
}

#endif

void CDSYS_XAPauseAtEnd(void)
{
#ifdef CTR_NATIVE
	BootIntroSkip_RequestMainMenu();
#endif
	CDSYS_XAPauseAtEnd_Original();
}
