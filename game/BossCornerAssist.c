#include <common.h>

// L3 now toggles only the manual curve-assist state. The assist itself lives in
// BossCornerAssistPhysics.c and runs inside normal player angular physics; this
// frame wrapper never converts the player to BOTS or changes the thread tick.
void VehFrameProc_Driving(struct Thread *thread, struct Driver *driver)
{
	VehFrameProc_Driving_BossPlayable(thread, driver);
	BossCornerAssist_UpdateToggle(driver);
}
