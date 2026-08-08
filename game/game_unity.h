#ifndef GAME_UNITY_H
#define GAME_UNITY_H

#include <common.h>

#include "226/R226.c"
#include "227/R227.c"
#include "228/R228.c"
#include "229/R229.c"
#include "226/226_00_DrawLevelOvr1P.c"
#include "227/227_00_DrawLevelOvr2P.c"
#include "228/228_00_DrawLevelOvr3P.c"
#include "229/229_00_DrawLevelOvr4P.c"
#include "DrawConfetti.c"
#include "DrawSky.c"

#include "RenderBucket/RenderBucket_08_InitDepthGTE.c"
#include "RenderBucket/RenderBucket_QueueExecute.c"
#include "RenderLevel/AnimateWater.c"
#include "RenderLevel/RenderLists.c"
#include "DrawTires.c"
#include "RenderStars.c"
#include "Torch.c"
#include "RenderWeather/RedBeaker_RenderRain.c"
#include "RenderWeather/RenderWeather.c"

#include "CAM.c"

#include "BOTS.c"
#include "CDSYS.c"

#include "COLL.c"

#include "CTR/CTR_Box.c"
#include "CTR/CTR_CycleTex.c"
#include "CTR/CTR_RenderLists.c"
#include "CTR/CTR_Error.c"
#include "CTR/CTR_Visibility.c"
#include "CTR/CTR_Matrix.c"
#include "CTR/CTR_Ghost.c"

#include "DebugFont.c"

#include "DecalFont.c"

#include "DecalGlobal.c"

#include "DecalHUD.c"

#include "DecalMP.c"

#include "Display.c"

#include "DotLights.c"

#include "DropRain.c"

#include "ElimBG.c"

#include "FLARE.c"

// Preserve CTR's retail button mapper privately, then expose a native wrapper
// that reserves the physical L3 click exclusively as BTN_L3.
#define GAMEPAD_ProcessHold GAMEPAD_ProcessHold_Original
#include "GAMEPAD.c"
#undef GAMEPAD_ProcessHold
#include "GamepadL3Fix.c"

#include "GAMEPROG.c"

#include "GhostReplay.c"

#include "GhostTape.c"

#include "HOWL/HOWL_OtherFX.c"
#include "HOWL/HOWL_AudioLR.c"
#include "HOWL/HOWL_OtherFXRecycle.c"
#include "HOWL/HOWL_LevelAudio.c"
#include "HOWL/HOWL_Engine.c"
#include "HOWL/HOWL_Reverb.c"
#include "HOWL/HOWL_CseqMusic.c"
#include "HOWL/HOWL_Bank.c"
#include "HOWL/HOWL_Load.c"
#include "HOWL/HOWL_Overlays.c"
