#include <common.h>

// Curve assist is now fully contextual: BossCornerAssistPhysics.c activates it
// only while the player is steering or powersliding. No L3 toggle/state remains.
void VehFrameProc_Driving(struct Thread *thread, struct Driver *driver)
{
	VehFrameProc_Driving_BossPlayable(thread, driver);
}
