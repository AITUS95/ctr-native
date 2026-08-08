#include <common.h>

// Native SDL packets expose the physical L3 click in the first PSX button
// byte. Retail CTR's button remap table predates our dedicated BTN_L3 use and
// can reinterpret that raw bit as another gameplay command after its byte
// shuffle. Mask the physical bit while retail mapping runs, then synthesize
// only BTN_L3 in the game-facing buffer. This keeps the retail mapper intact
// while giving native-only features a clean L3 edge/hold signal.
int GAMEPAD_ProcessHold(struct GamepadSystem *gGamepads)
{
#if defined(CTR_NATIVE)
	u8 l3Pressed[8] = {0};
	u8 packetMasked[8] = {0};
	u8 savedInput1[8] = {0};

	for (int i = 0; i < 8; i++)
	{
		struct ControllerPacket *packet = gGamepads->gamepad[i].ptrControllerPacket;
		if (packet == NULL || packet->plugged != PLUGGED)
		{
			continue;
		}

		savedInput1[i] = packet->controllerInput1;
		packetMasked[i] = 1;

		// PSX pad button bits are active-low in the packet.
		if ((packet->controllerInput1 & RAW_BTN_L3) == 0)
		{
			l3Pressed[i] = 1;
			packet->controllerInput1 |= RAW_BTN_L3;
		}
	}

	int heldAny = GAMEPAD_ProcessHold_Original(gGamepads);

	for (int i = 0; i < 8; i++)
	{
		struct GamepadBuffer *pad = &gGamepads->gamepad[i];
		struct ControllerPacket *packet = pad->ptrControllerPacket;

		if (packetMasked[i] && packet != NULL)
		{
			packet->controllerInput1 = savedInput1[i];
		}

		if (l3Pressed[i])
		{
			pad->buttonsHeldCurrFrame |= BTN_L3;
			heldAny |= BTN_L3;
		}
	}

	return heldAny;
#else
	return GAMEPAD_ProcessHold_Original(gGamepads);
#endif
}
