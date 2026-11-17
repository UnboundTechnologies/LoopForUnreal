#pragma once

#include "CoreMinimal.h"

class AActor;

struct FUbstreamSelectedModelingObject
{
	FString ActorName;
	FString ActorPath;
	FString ClassName;
	FString Type;
	FString AssetPath;
	TArray<FString> Tags;
	FVector BoundsOrigin = FVector::ZeroVector;
	FVector BoundsExtent = FVector::ZeroVector;
	FTransform Transform = FTransform::Identity;
};

class FUbstreamModelingModeBridge
{
public:
	static bool IsModelingModeAvailable();
	static bool IsModelingModeActive();
	static bool ActivateModelingMode();
	static TArray<FUbstreamSelectedModelingObject> GetSelectedModelingActors();
	static int32 TagSelectedModelingActors();
	static FString BuildSelectedModelingStateJson(const FString& SessionId);
	static FString BuildSelectionSummary(const TArray<FUbstreamSelectedModelingObject>& Objects);

	static const FName SandboxTag;
	static const FName DoNotCommitTag;
};
