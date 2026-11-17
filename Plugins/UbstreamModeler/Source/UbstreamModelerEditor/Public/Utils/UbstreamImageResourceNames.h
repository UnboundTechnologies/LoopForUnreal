#pragma once

#include "CoreMinimal.h"

namespace UbstreamImageResourceNames
{
	/** Game-thread only. Keep uniqueness in FName's numeric field rather than
	    interning a new timestamp-bearing string for every video frame. */
	inline FName Next(const TCHAR* Prefix)
	{
		check(IsInGameThread());
		static uint32 Sequence = 0;
		const FName BaseName(*FString::Printf(TEXT("UnboundLoop%s"), Prefix));
		return FName(BaseName, static_cast<int32>(++Sequence));
	}
}
