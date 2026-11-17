#pragma once

#include "CoreMinimal.h"
#include "Capture/UbstreamCaptureTypes.h"
#include "Capture/UbstreamEditorWorldCaptureService.h"

class FAdvancedPreviewScene;
class FUbstreamCaptureTargetPool;
class UPrimitiveComponent;
class USceneCaptureComponent2D;

/**
 * Owns one streamed frame through GPU readback and worker encoding.
 * Begin() submits capture and readback work on the game thread. After IsReady(),
 * CompleteAndEncodeAsync() copies staging pixels on the render thread, encodes
 * on a worker, and delivers the result on the game thread.
 * The black-frame fallback chain cannot run pre-encode here (pixels arrive
 * late), so a black show-only frame is reported via bNearlyBlackWithShowOnly
 * for the caller to redo synchronously.
 */
class FUbstreamAsyncFrameReadback : public TSharedFromThis<FUbstreamAsyncFrameReadback, ESPMode::ThreadSafe>
{
public:
	static TSharedPtr<FUbstreamAsyncFrameReadback, ESPMode::ThreadSafe> Begin(
		FAdvancedPreviewScene& PreviewScene,
		const FUbstreamCaptureCamera& Camera,
		uint64 FrameId,
		const FUbstreamCaptureSettings& Settings,
		const TArray<UPrimitiveComponent*>& ShowOnlyComponents,
		FUbstreamCaptureTargetPool* TargetPool,
		USceneCaptureComponent2D* ReusedComponent,
		USceneCaptureComponent2D* ReusedDepthComponent);

	bool IsReady() const;
	bool HasTimedOut() const;
	void CompleteAndEncodeAsync(TUniqueFunction<void(FUbstreamEditorWorldCaptureResult&&)> OnDone);

	FUbstreamCapturePixels Pixels;
	TSharedPtr<class FRHIGPUTextureReadback, ESPMode::ThreadSafe> ColorReadback;
	TSharedPtr<class FRHIGPUTextureReadback, ESPMode::ThreadSafe> DepthReadback;
	double BeginTimeSeconds = 0.0;

private:
	// A reused fence may still signal the previous frame until EnqueueCopy runs.
	TAtomic<bool> bCopyCommandsEnqueued{false};
};
