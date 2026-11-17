#pragma once

#include "CoreMinimal.h"
#include "Geometry/UbstreamCSGKernel.h"

DECLARE_DELEGATE_OneParam(FUbstreamCSGPreviewReady, const FUbstreamCSGResult&);

/** One brush in the live stack: a cutter mesh, where it sits, what it does. */
struct FUbstreamCSGSessionBrush
{
	UE::Geometry::FDynamicMesh3 Mesh;
	FTransform Transform = FTransform::Identity;
	EUbstreamCSGOp Operation = EUbstreamCSGOp::Subtract;
};

/** One evaluation step: a single brush, or ALL intersect brushes merged
    into one crop region (applied last, so several intersect shapes each
    crop the same accumulated base: base n (A u B) == (base n A) u (base n B)). */
struct FUbstreamEvalStep
{
	TArray<int32> BrushIndexes;
	TArray<FTransform> Transforms;
	EUbstreamCSGOp Operation = EUbstreamCSGOp::Subtract;

	bool MatchesSignature(const FUbstreamEvalStep& Other) const
	{
		if (Operation != Other.Operation
			|| BrushIndexes != Other.BrushIndexes
			|| Transforms.Num() != Other.Transforms.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Transforms.Num(); ++Index)
		{
			if (!Transforms[Index].Equals(Other.Transforms[Index]))
			{
				return false;
			}
		}
		return true;
	}
};

/**
 * Owns one in-progress CSG editing session between the stage subject and an
 * ordered STACK of brushes - every cutter participates in the live result at
 * once, each with its own operation, applied in order. Holds shared mesh
 * snapshots, recomputes on the thread pool whenever any brush moves or
 * changes operation, and delivers results back on the game thread.
 * Latest-wins: at most one compute is in flight; edits during a compute just
 * mark the session dirty and the completion relaunches with the freshest
 * state, so compute time itself paces the preview.
 *
 * UI-free by design - the viewport drives Tick() and renders whatever
 * OnPreviewReady delivers. All methods are game-thread only.
 */
class UBSTREAMMODELEREDITOR_API FUbstreamCSGSession : public TSharedFromThis<FUbstreamCSGSession, ESPMode::ThreadSafe>
{
public:
	void Begin(
		UE::Geometry::FDynamicMesh3 SubjectMesh,
		const FTransform& SubjectTransform,
		TArray<FUbstreamCSGSessionBrush> InBrushes,
		bool bFillCreatedHoles = true);
	void End();

	bool IsActive() const { return bActive; }
	/** True while an edit has not yet been reflected in LatestResult. */
	bool HasPendingWork() const { return bActive && (bDirty || bComputeInFlight); }

	int32 GetBrushCount() const { return BrushMeshes.Num(); }
	/** Swap in a new subject mesh mid-session (live sculpting feeds the sculpted
	    base each stroke). Invalidates the whole prefix cache and re-evaluates. */
	void UpdateSubjectMesh(UE::Geometry::FDynamicMesh3 NewSubjectMesh);
	void SetBrushTransform(int32 BrushIndex, const FTransform& InBrushTransform);
	void SetBrushOperation(int32 BrushIndex, EUbstreamCSGOp InOperation);
	EUbstreamCSGOp GetBrushOperation(int32 BrushIndex) const;

	/** Drive from a ticker; launches a recompute when dirty and idle. */
	void Tick();

	/** Latest computed result, if any compute has finished since Begin. */
	const FUbstreamCSGResult* GetLatestResult() const;

	/**
	 * Hands out the latest result mesh for committing to the subject and ends
	 * the session. Returns unset if no successful result exists.
	 */
	TOptional<UE::Geometry::FDynamicMesh3> TakeResultMeshAndEnd();

	/** Fired on the game thread each time a fresh preview mesh is available. */
	FUbstreamCSGPreviewReady OnPreviewReady;

	/** Brush index the last completed compute actually started from (chain
	    prefix caching); INDEX_NONE before the first completion. Test hook. */
	int32 GetLastComputeStartIndexForTesting() const { return LastComputeStartIndex; }

private:
	void LaunchCompute();
	void HandleComputeFinished(struct FUbstreamChainComputeOutput&& Output, uint64 InGeneration);

	// Immutable per session; shared with worker threads without copying.
	TSharedPtr<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> SubjectMesh;
	TArray<TSharedPtr<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>> BrushMeshes;
	FTransform SubjectTransform = FTransform::Identity;

	// Mutable per brush; copied into each compute launch.
	TArray<FTransform> BrushTransforms;
	TArray<EUbstreamCSGOp> BrushOperations;

	/** Cap cut holes only when the subject started watertight. */
	bool bFillCreatedHoles = true;

	TOptional<FUbstreamCSGResult> LatestResult;

	/**
	 * Chain-prefix cache: intermediates[i] is the chain result after brush i,
	 * with the transform/op inputs that produced it. Editing brush K only
	 * recomputes from K onward - the difference between fluid and pacing once
	 * several cutters are live.
	 */
	TArray<TSharedPtr<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>> CachedIntermediates;
	TArray<FUbstreamEvalStep> CachedSteps;
	int32 LastComputeStartIndex = INDEX_NONE;

	/* Invalidates in-flight completions from a previous Begin/End. */
	uint64 Generation = 0;
	uint64 SubjectRevision = 0;
	bool bActive = false;
	bool bDirty = false;
	bool bComputeInFlight = false;
};
