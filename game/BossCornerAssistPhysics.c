#include <common.h>

// Compatibility pass-through kept only because game_unity.h currently wraps
// VehPhysGeneral_PhysAngular under a private symbol. All curve-assist behavior
// has been removed: no nav lookup, no speed clamp, no steering/yaw correction.
void VehPhysGeneral_PhysAngular(struct Thread *thread, struct Driver *driver)
{
	VehPhysGeneral_PhysAngular_Original(thread, driver);
}
