#pragma once

#include "CoreMinimal.h"
#include "ShowFlags.h"

namespace UbstreamCleanViewportFlags
{
	inline void Apply(FEngineShowFlags& ShowFlags)
	{
		ShowFlags.SetGrid(false);
		ShowFlags.SetSelectionOutline(false);
		ShowFlags.SetCompositeEditorPrimitives(false);
		ShowFlags.SetCompositeDebugPrimitives(false);
		ShowFlags.SetModeWidgets(false);
		ShowFlags.SetOnScreenDebug(false);
		ShowFlags.SetWidgetComponents(false);
		ShowFlags.SetBones(false);
		ShowFlags.SetCameras(false);
		ShowFlags.SetCloud(false);
		ShowFlags.SetAtmosphere(false);
		ShowFlags.SetFog(false);
		ShowFlags.SetVolumetricFog(false);
		ShowFlags.SetTemporalAA(false);
		ShowFlags.SetMotionBlur(false);
		ShowFlags.SetEyeAdaptation(false);
		ShowFlags.SetLocalExposure(false);
		ShowFlags.SetBloom(false);
		ShowFlags.SetScreenSpaceReflections(false);
	}
}
