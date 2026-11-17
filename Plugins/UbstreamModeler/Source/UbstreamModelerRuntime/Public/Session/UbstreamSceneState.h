#pragma once

#include "CoreMinimal.h"
#include "Session/UbstreamObjectState.h"

struct FUbstreamSceneState
{
	FString SessionId;
	TArray<FUbstreamObjectState> Objects;
};
