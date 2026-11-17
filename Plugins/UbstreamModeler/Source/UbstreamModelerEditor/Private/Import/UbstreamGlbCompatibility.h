#pragma once

#include "CoreMinimal.h"

namespace UbstreamGlbCompatibility
{
	/** Reconcile Draco accessor counts with the decoded geometry before Interchange reads it. */
	bool PrepareForImport(const TArray<uint8>& Source, TArray<uint8>& OutGlb, FString& OutError);
}
