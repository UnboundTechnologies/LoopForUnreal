#pragma once

#include "CoreMinimal.h"
#include "Capture/UbstreamAsyncFrameReadback.h"
#include "Capture/UbstreamCaptureTypes.h"
#include "Capture/UbstreamEditorWorldCaptureService.h"

class FAdvancedPreviewScene;
class FUbstreamCaptureTargetPool;
class UPrimitiveComponent;
class USceneCaptureComponent2D;

class FUbstreamCaptureService
{
public:
	static FUbstreamCaptureResult CaptureRgb(
		FAdvancedPreviewScene& PreviewScene,
		const FUbstreamCaptureCamera& Camera,
		const FUbstreamCaptureSettings& Settings = FUbstreamCaptureSettings());

	/** Game-thread capture phase: renders the scene, reads back raw color (and
	    optional depth) pixels, and runs the black-frame fallback chain. Does NOT
	    encode. Pass a persistent ReusedComponent (already registered in the
	    scene) to avoid per-frame component churn; pass nullptr to have a transient
	    component created and destroyed for this call. */
	static FUbstreamCapturePixels CaptureFramePixels(
		FAdvancedPreviewScene& PreviewScene,
		const FUbstreamCaptureCamera& Camera,
		uint64 FrameId,
		const FUbstreamCaptureSettings& Settings,
		const TArray<UPrimitiveComponent*>& ShowOnlyComponents,
		FUbstreamCaptureTargetPool* TargetPool = nullptr,
		USceneCaptureComponent2D* ReusedComponent = nullptr);

	/** Encode phase: JPEG-encodes the captured pixels and builds the UBS1 frame
	    bundle. Touches no UObjects or RHI resources, so it is safe to call on a
	    worker thread. */
	static FUbstreamEditorWorldCaptureResult EncodeCapturedFrame(FUbstreamCapturePixels Pixels);

	static FUbstreamEditorWorldCaptureResult CaptureFrameBundle(
		FAdvancedPreviewScene& PreviewScene,
		const FUbstreamCaptureCamera& Camera,
		uint64 FrameId,
		const FUbstreamCaptureSettings& Settings = FUbstreamCaptureSettings());

	static FUbstreamEditorWorldCaptureResult CaptureFrameBundle(
		FAdvancedPreviewScene& PreviewScene,
		const FUbstreamCaptureCamera& Camera,
		uint64 FrameId,
		const FUbstreamCaptureSettings& Settings,
		const TArray<UPrimitiveComponent*>& ShowOnlyComponents,
		FUbstreamCaptureTargetPool* TargetPool = nullptr);
};
