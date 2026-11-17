#include "Capture/UbstreamCaptureTargetPool.h"
#include "Capture/UbstreamCaptureSceneUtils.h"

#include "Engine/TextureRenderTarget2D.h"
#include "RHIGPUReadback.h"
#include "Capture/UbstreamCaptureEncodingCache.h"

UTextureRenderTarget2D* FUbstreamCaptureTargetPool::AcquireColorTarget(int32 Width, int32 Height)
{
	return AcquireTarget(ColorTarget, Width, Height, PF_B8G8R8A8);
}

UTextureRenderTarget2D* FUbstreamCaptureTargetPool::AcquireDepthTarget(int32 Width, int32 Height)
{
	return AcquireTarget(DepthTarget, Width, Height, PF_FloatRGBA);
}

TSharedPtr<FRHIGPUTextureReadback, ESPMode::ThreadSafe> FUbstreamCaptureTargetPool::AcquireReadback(
	const FIntPoint Size, const EPixelFormat Format)
{
	check(IsInGameThread());
	for (const FReadbackSlot& Slot : Readbacks)
	{
		// A frame retains its lease until copying/unlocking/encoding finishes.
		// Cancelled frames can release their lease before the GPU finishes, so
		// uniqueness alone is insufficient. Never resize a staging texture: the
		// engine's readback reuse path assumes an unchanged extent and format.
		if (Slot.Size == Size && Slot.Format == Format
			&& Slot.Readback.IsUnique() && Slot.Readback->IsReady())
		{
			return Slot.Readback;
		}
	}

	Readbacks.RemoveAll([Size, Format](const FReadbackSlot& Slot)
	{
		return (Slot.Size != Size && Slot.Format == Format)
			&& Slot.Readback.IsUnique() && Slot.Readback->IsReady();
	});
	const auto Readback = MakeShared<FRHIGPUTextureReadback, ESPMode::ThreadSafe>(TEXT("UbstreamPooledReadback"));
	// Bound retention even if captures are repeatedly cancelled or a GPU stalls.
	// Overflow still captures normally with a frame-owned temporary readback.
	if (Readbacks.Num() < 4)
	{
		Readbacks.Add({Size, Format, Readback});
	}
	return Readback;
}

TSharedPtr<FUbstreamCaptureEncodingCache, ESPMode::ThreadSafe> FUbstreamCaptureTargetPool::GetEncodingCache()
{
	check(IsInGameThread());
	if (!EncodingCache.IsValid())
	{
		EncodingCache = MakeShared<FUbstreamCaptureEncodingCache, ESPMode::ThreadSafe>();
	}
	return EncodingCache;
}

void FUbstreamCaptureTargetPool::Reset()
{
	Readbacks.Reset();
	EncodingCache.Reset();
	ColorTarget.Reset();
	DepthTarget.Reset();
}

UTextureRenderTarget2D* FUbstreamCaptureTargetPool::AcquireTarget(
	TStrongObjectPtr<UTextureRenderTarget2D>& Slot,
	int32 Width,
	int32 Height,
	EPixelFormat PixelFormat)
{
	if (UTextureRenderTarget2D* Existing = Slot.Get();
		IsValid(Existing)
		&& Existing->SizeX == Width
		&& Existing->SizeY == Height
		&& Existing->GetFormat() == PixelFormat)
	{
		return Existing;
	}

	UTextureRenderTarget2D* RenderTarget = UbstreamCaptureSceneUtils::CreateRenderTarget(Width, Height, PixelFormat);
	Slot.Reset(RenderTarget);
	return RenderTarget;
}
