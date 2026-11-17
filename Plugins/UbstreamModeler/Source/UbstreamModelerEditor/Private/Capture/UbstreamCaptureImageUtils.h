#pragma once

#include "CoreMinimal.h"
#include "ImageCore.h"

class FFloat16Color;

/** CPU image operations shared by preview and editor-world capture. */
namespace UbstreamCaptureImageUtils
{
	/** PF_FloatRGBA scene depth saturates empty/far pixels to the largest
	 *  half-float on some RHIs (including Metal). They are background, not
	 *  samples for the foreground normalization range. */
	inline bool IsUsableSceneDepth(float Depth)
	{
		return FMath::IsFinite(Depth) && Depth > 0.0f && Depth < 65504.0f;
	}

	bool CopyCompressedBytes(const TArray64<uint8>& Source, TArray<uint8>& Target);
	/** Transfers ownership to a 32-bit image buffer, copying only if the source
	 *  capacity cannot fit. Oversized payloads leave both buffers unchanged. */
	bool TakeImageBytes(TArray64<uint8>& Source, TArray<uint8>& Target);
	/** Converts one packed depth row; callers handle GPU row padding. */
	void ConvertDepthRow(const FFloat16Color* Source, FLinearColor* Destination, int32 Width);
	/** Normalizes to BGRA8/sRGB and samples pixels for capture diagnostics.
	 *  This is not an exact-image identity check; do not use it for caching. */
	uint64 BuildDeterministicColorSampleHash(FImage& ColorImage);
	/** Normalizes to BGRA8/sRGB before applying the visibility fallback test. */
	bool IsNearlyBlackImage(FImage& ColorImage);
}
