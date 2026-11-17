#pragma once

#include "CoreMinimal.h"
#include "Capture/UbstreamCaptureTypes.h"

class FUbstreamCaptureTargetPool;

struct FUbstreamEditorWorldCaptureResult
{
	bool bSuccess = false;
	FString ErrorMessage;
	FString SessionId;
	uint64 FrameId = 0;
	FIntPoint Size = FIntPoint::ZeroValue;
	FUbstreamCaptureCamera Camera;
	FString HeaderJson;
	uint64 ColorSampleHash = 0;
	int32 RequestedShowOnlyComponentCount = 0;
	int32 AppliedShowOnlyComponentCount = 0;
	bool bUsedSceneFallback = false;
	bool bUsedFlatColorFallback = false;
	/** Async path only: the show-only capture came back black; the caller
	    should re-capture once through the synchronous fallback chain. */
	bool bNearlyBlackWithShowOnly = false;
	TArray<uint8> ColorJpegBytes;
	TArray<uint8> DepthJpegBytes;
	TArray<uint8> FrameBundleBytes;
	FString HeaderDebugPath;
	FString ColorDebugPath;
	FString DepthDebugPath;
};

class FUbstreamEditorWorldCaptureService
{
public:
	static FUbstreamEditorWorldCaptureResult CaptureFrameBundle(
		uint64 FrameId,
		const FUbstreamCaptureSettings& Settings = FUbstreamCaptureSettings(),
		FUbstreamCaptureTargetPool* TargetPool = nullptr);
};
