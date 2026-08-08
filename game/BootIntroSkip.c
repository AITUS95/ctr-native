#include <common.h>

#ifdef CTR_NATIVE

static int s_bootIntroSkipRequested;

static int BootIntroSkip_IsAnyButtonHeld(void)
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

	// PSX controller button bits are active-low. Reconstruct the same 16-bit
	// digital field used by GAMEPAD_ProcessHold and accept any digital button,
	// rather than reserving the shortcut for Start alone.
	u32 rawInput = CTR_MipsSll((u32)packet->controllerInput1, 8) | (u32)packet->controllerInput2;
	return (rawInput ^ 0xffff) != 0;
}

int BootIntroSkip_IsRequested(void)
{
	return s_bootIntroSkipRequested;
}

int BootIntroSkip_PollRequest(void)
{
	if (s_bootIntroSkipRequested)
	{
		return 1;
	}

	if (!BootIntroSkip_IsAnyButtonHeld())
	{
		return 0;
	}

	s_bootIntroSkipRequested = 1;
	return 1;
}

void BootIntroSkip_ApplyMainMenuRedirect(struct GameTracker *gGT)
{
	if (gGT == NULL)
	{
		return;
	}

	// This is a real skip, not a fast-forward. Stop both pieces of the boot
	// presentation and redirect the loader to the ordinary title/menu level.
	// Never fake songPool[0].timeSpentPlaying to make the intro advance faster.
	CDSYS_XAPauseForce();
	CseqMusic_StopAll();
	gGT->levelID = MAIN_MENU_LEVEL;
	sdata->mainMenuState = MAIN_MENU_TITLE;
}

static void BootIntroSkip_RequestMainMenu(void)
{
	struct GameTracker *gGT = sdata->gGT;
	if (gGT == NULL)
	{
		return;
	}

	// This wrapper is also used by normal per-frame XA polling, so restrict the
	// shortcut to the one-time SCEA / Copyright / Naughty Dog boot presentation.
	if (sdata->boolFirstBoot == 0 || gGT->levelID != NAUGHTY_DOG_CRATE || sdata->XA_State == XA_IDLE)
	{
		return;
	}

	if (!BootIntroSkip_PollRequest())
	{
		return;
	}

	BootIntroSkip_ApplyMainMenuRedirect(gGT);
}

#endif

void CDSYS_XAPauseAtEnd(void)
{
#ifdef CTR_NATIVE
	BootIntroSkip_RequestMainMenu();
#endif
	CDSYS_XAPauseAtEnd_Original();
}
