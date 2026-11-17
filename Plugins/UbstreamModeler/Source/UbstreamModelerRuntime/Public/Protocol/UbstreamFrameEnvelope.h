#pragma once

#include "CoreMinimal.h"

class UBSTREAMMODELERRUNTIME_API FUbstreamFrameEnvelope
{
public:
	static bool BuildFrameBundle(
		uint64 FrameId,
		const FString& HeaderJson,
		const TArray<uint8>& ColorBytes,
		const TArray<uint8>& DepthBytes,
		TArray<uint8>& OutBytes);

private:
	static void WriteUInt16LE(TArray<uint8>& OutBytes, uint16 Value);
	static void WriteUInt32LE(TArray<uint8>& OutBytes, uint32 Value);
	static void WriteUInt64LE(TArray<uint8>& OutBytes, uint64 Value);
};
