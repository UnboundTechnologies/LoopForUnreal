#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"

/** Owns plain geometry only. Cancellation never waits on the editor thread. */
class FUbstreamRemeshJob : public TSharedFromThis<FUbstreamRemeshJob, ESPMode::ThreadSafe>
{
public:
	TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> Input;
	TUniquePtr<UE::Geometry::FDynamicMesh3> Result;
	FString Error;
	void Start(double TargetEdgeLength, TFunction<void()> OnDone);
	void Cancel() { bCancelled.Store(true); }
	bool IsCancelled() const { return bCancelled.Load(); }

private:
	TAtomic<bool> bCancelled { false };
};
