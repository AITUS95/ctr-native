#include <common.h>

enum
{
	NATIVE_STEER_TURN_GAIN_NUM = 3,
	NATIVE_STEER_TURN_GAIN_DEN = 2,
	NATIVE_STEER_TURN_GAIN_MAX = 0x70,
};

static s8 NativeSteer_BoostTurnState(s8 turnState)
{
	int boosted = CTR_MipsDiv(CTR_MipsMulLo((int)turnState, NATIVE_STEER_TURN_GAIN_NUM), NATIVE_STEER_TURN_GAIN_DEN);

	if (boosted > NATIVE_STEER_TURN_GAIN_MAX)
	{
		boosted = NATIVE_STEER_TURN_GAIN_MAX;
	}
	else if (boosted < -NATIVE_STEER_TURN_GAIN_MAX)
	{
		boosted = -NATIVE_STEER_TURN_GAIN_MAX;
	}

	return (s8)boosted;
}

// Keep normal player input and gameplay thresholds intact, but temporarily
// amplify the steering command seen by angular physics. This increases the
// kart's actual yaw/turn rate instead of making the analog stick more sensitive.
// CPU/BOTS drivers keep retail behavior.
void VehPhysGeneral_PhysAngular(struct Thread *thread, struct Driver *driver)
{
	s8 originalTurnState = 0;
	int boosted = 0;

#ifdef CTR_NATIVE
	if (driver != NULL)
	{
		originalTurnState = (s8)driver->simpTurnState;
		if (originalTurnState != 0 && (driver->actionsFlagSet & ACTION_BOT) == 0)
		{
			driver->simpTurnState = NativeSteer_BoostTurnState(originalTurnState);
			boosted = 1;
		}
	}
#endif

	VehPhysGeneral_PhysAngular_Original(thread, driver);

#ifdef CTR_NATIVE
	if (boosted)
	{
		driver->simpTurnState = originalTurnState;
	}
#endif
}
