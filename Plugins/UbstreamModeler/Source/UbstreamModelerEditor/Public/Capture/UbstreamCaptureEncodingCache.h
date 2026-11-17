#pragma once

#include "CoreMinimal.h"
#include "ImageCore.h"
#include "HAL/CriticalSection.h"

/** Last-image JPEG cache. Exact bytes and image metadata are compared; no
 * sampled signature can hide animation or a small mesh/material change.
 * Owns at most one color and one normalized-depth image per capture workspace.
 * May be shared by synchronous fallback and asynchronous encoder workers.
 */
class FUbstreamCaptureEncodingCache
{
public:
	bool Compress(FImage&& Image, int32 Quality, bool bDepth, TArray64<uint8>& OutBytes);

private:
	friend class FUbstreamEncodingReuseTest;
	struct FEntry
	{
		FImage Image;
		TArray64<uint8> Bytes;
		int32 Quality = 0;
	};
	FCriticalSection Mutex;
	FEntry Color;
	FEntry Depth;
#if WITH_DEV_AUTOMATION_TESTS
	uint64 CompressionCount = 0;
#endif
};
