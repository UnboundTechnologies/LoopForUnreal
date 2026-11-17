#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureRenderTarget2D.h"
#include "PixelFormat.h"
#include "UObject/StrongObjectPtr.h"

class FRHIGPUTextureReadback;
class FUbstreamCaptureEncodingCache;

/**
 * Owns reusable capture render targets so streaming does not allocate and
 * initialize new GPU resources for every captured frame. Targets are
 * recreated only when the requested size or format changes.
 */
class UBSTREAMMODELEREDITOR_API FUbstreamCaptureTargetPool
{
public:
	UTextureRenderTarget2D* AcquireColorTarget(int32 Width, int32 Height);
	UTextureRenderTarget2D* AcquireDepthTarget(int32 Width, int32 Height);
	/** Only reuses unowned, GPU-complete readbacks of exactly this size/format. */
	TSharedPtr<FRHIGPUTextureReadback, ESPMode::ThreadSafe> AcquireReadback(FIntPoint Size, EPixelFormat Format);
	TSharedPtr<FUbstreamCaptureEncodingCache, ESPMode::ThreadSafe> GetEncodingCache();
	void Reset();

private:
	UTextureRenderTarget2D* AcquireTarget(
		TStrongObjectPtr<UTextureRenderTarget2D>& Slot,
		int32 Width,
		int32 Height,
		EPixelFormat PixelFormat);

	struct FReadbackSlot
	{
		FIntPoint Size;
		EPixelFormat Format;
		TSharedPtr<FRHIGPUTextureReadback, ESPMode::ThreadSafe> Readback;
	};
	TArray<FReadbackSlot> Readbacks;
	TSharedPtr<FUbstreamCaptureEncodingCache, ESPMode::ThreadSafe> EncodingCache;

	TStrongObjectPtr<UTextureRenderTarget2D> ColorTarget;
	TStrongObjectPtr<UTextureRenderTarget2D> DepthTarget;
};
