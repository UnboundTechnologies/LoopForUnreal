#pragma once

#include "CoreMinimal.h"

struct FUbstreamObjectState
{
	FString Id;
	FString Type;
	FTransform Transform = FTransform::Identity;
};
