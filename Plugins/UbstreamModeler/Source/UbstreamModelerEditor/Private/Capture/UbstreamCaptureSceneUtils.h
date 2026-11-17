#pragma once

#include "CoreMinimal.h"
#include "PixelFormat.h"

class UPrimitiveComponent;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
struct FUbstreamCaptureCamera;

/** Game-thread scene/resource setup. Capture ownership remains with callers. */
namespace UbstreamCaptureSceneUtils
{
	UTextureRenderTarget2D* CreateRenderTarget(int32 Width, int32 Height, EPixelFormat PixelFormat);
	void ConfigureCamera(USceneCaptureComponent2D& CaptureComponent, const FUbstreamCaptureCamera& Camera);
	void ConfigureCleanCapture(USceneCaptureComponent2D& CaptureComponent);
	void ConfigureFlatColorFallbackCapture(USceneCaptureComponent2D& CaptureComponent);
	int32 ApplyShowOnlyComponents(USceneCaptureComponent2D& CaptureComponent, const TArray<UPrimitiveComponent*>& ShowOnlyComponents);
	void ClearShowOnlyComponents(USceneCaptureComponent2D& CaptureComponent);
	void FinishMeshCompilationForCapture(const TArray<UPrimitiveComponent*>& ShowOnlyComponents);
}
