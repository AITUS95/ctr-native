#include <common.h>

// Native convenience: keep retail's existing title-intro skip behavior, but
// also accept Start so the opening Crash/C-T-R animation can be dismissed with
// the conventional menu/start button.
void MM_Title_ThTick(struct Thread *title)
{
	if (D230.titleMenuState == TITLE_MENU_STATE_INTRO &&
	    (sdata->gGamepads->gamepad[0].buttonsTapped & BTN_START) != 0)
	{
		// Feed the original routine one of its accepted skip bits. It will clear
		// menu input and jump the title animation to its ready state itself.
		sdata->buttonTapPerPlayer[0] |= TITLE_INTRO_SKIP_INPUT;
	}

	MM_Title_ThTick_Original(title);
}
