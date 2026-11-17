#pragma once

#include "CoreMinimal.h"
#include "ImageCore.h"

class FUbstreamCaptureEncodingCache;

struct FUbstreamCaptureCamera
{
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	float FovDegrees = 45.0f;
	float AspectRatio = 1.0f;
	/** <= 0 uses the engine default (GNearClippingPlane, 10 units). Viewports that
	    frame small subjects override their near plane; captures must match or the
	    whole subject clips to black. */
	float NearClipPlane = 0.0f;
};

struct FUbstreamCaptureSettings
{
	int32 Width = 512;
	int32 Height = 512;
	int32 JpegQuality = 88;
	bool bCaptureDepth = true;
	bool bSaveDebugImage = false;
	FString SessionId = TEXT("editor_modeling_session");
};

struct FUbstreamCaptureResult
{
	bool bSuccess = false;
	FString ErrorMessage;
	FIntPoint Size = FIntPoint::ZeroValue;
	FUbstreamCaptureCamera Camera;
	FString ColorDebugPath;
	TArray64<uint8> ColorPngBytes;
};

/** Intermediate result of the game-thread capture phase: raw, un-encoded pixels
    plus the metadata the encode phase needs. Produced by
    FUbstreamCaptureService::CaptureFramePixels on the game thread (render +
    readback + black-frame fallback), then handed to EncodeCapturedFrame, which
    JPEG-encodes and builds the frame bundle and is safe to run on a worker
    thread. Movable so the pixel buffers can cross threads without a copy. */
struct FUbstreamCapturePixels
{
	bool bSuccess = false;
	FString ErrorMessage;
	uint64 FrameId = 0;
	FIntPoint Size = FIntPoint::ZeroValue;
	FUbstreamCaptureCamera Camera;
	FString SessionId;
	int32 JpegQuality = 88;
	bool bCaptureDepth = true;
	bool bSaveDebugImage = false;
	bool bUsedSceneFallback = false;
	bool bUsedFlatColorFallback = false;
	int32 RequestedShowOnlyComponentCount = 0;
	int32 AppliedShowOnlyComponentCount = 0;
	/** Final color pixels (BGRA8/sRGB after the nearly-black test reformats it). */
	FImage ColorImage;
	TSharedPtr<FUbstreamCaptureEncodingCache, ESPMode::ThreadSafe> EncodingCache;
	/** Raw scene-depth readback; empty when depth capture is disabled. */
	TArray<FLinearColor> DepthPixels;
};
