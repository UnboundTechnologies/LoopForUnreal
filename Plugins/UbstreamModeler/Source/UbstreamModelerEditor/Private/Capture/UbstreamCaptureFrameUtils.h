#pragma once

#include "CoreMinimal.h"

struct FUbstreamEditorWorldCaptureResult;

/** Shared frame metadata; capture services retain their own filename prefixes. */
namespace UbstreamCaptureFrameUtils
{
	FString GetCaptureOutputDir();
	FString BuildFrameHeaderJson(const FUbstreamEditorWorldCaptureResult& Result);
}
