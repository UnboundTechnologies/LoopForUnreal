#include "Geometry/UbstreamCSGSession.h"

#include "Async/Async.h"

using namespace UE::Geometry;

void FUbstreamCSGSession::Begin(
	FDynamicMesh3 InSubjectMesh,
	const FTransform& InSubjectTransform,
	TArray<FUbstreamCSGSessionBrush> InBrushes,
	const bool bInFillCreatedHoles)
{
	check(IsInGameThread());
	check(!InBrushes.IsEmpty());

	++Generation;
	bFillCreatedHoles = bInFillCreatedHoles;
	SubjectMesh = MakeShared<const FDynamicMesh3, ESPMode::ThreadSafe>(MoveTemp(InSubjectMesh));
	SubjectTransform = InSubjectTransform;

	BrushMeshes.Reset(InBrushes.Num());
	BrushTransforms.Reset(InBrushes.Num());
	BrushOperations.Reset(InBrushes.Num());
	for (FUbstreamCSGSessionBrush& Brush : InBrushes)
	{
		BrushMeshes.Add(MakeShared<const FDynamicMesh3, ESPMode::ThreadSafe>(MoveTemp(Brush.Mesh)));
		BrushTransforms.Add(Brush.Transform);
		BrushOperations.Add(Brush.Operation);
	}

	LatestResult.Reset();
	CachedIntermediates.Reset();
	CachedSteps.Reset();
	LastComputeStartIndex = INDEX_NONE;
	bActive = true;
	bDirty = true;
	bComputeInFlight = false;
}

void FUbstreamCSGSession::End()
{
	check(IsInGameThread());

	++Generation;
	bActive = false;
	bDirty = false;
	bComputeInFlight = false;
	LatestResult.Reset();
	SubjectMesh.Reset();
	BrushMeshes.Reset();
	BrushTransforms.Reset();
	BrushOperations.Reset();
	CachedIntermediates.Reset();
	CachedSteps.Reset();
	LastComputeStartIndex = INDEX_NONE;
}

void FUbstreamCSGSession::UpdateSubjectMesh(FDynamicMesh3 NewSubjectMesh)
{
	check(IsInGameThread());
	if (!bActive)
	{
		return;
	}
	SubjectMesh = MakeShared<const FDynamicMesh3, ESPMode::ThreadSafe>(MoveTemp(NewSubjectMesh));
	++SubjectRevision;
	// The subject is the root of every chain step, so its change invalidates
	// the entire prefix cache; recompute from scratch.
	CachedIntermediates.Reset();
	CachedSteps.Reset();
	LastComputeStartIndex = INDEX_NONE;
	bDirty = true;
}

void FUbstreamCSGSession::SetBrushTransform(const int32 BrushIndex, const FTransform& InBrushTransform)
{
	check(IsInGameThread());
	if (!bActive || !BrushTransforms.IsValidIndex(BrushIndex)
		|| BrushTransforms[BrushIndex].Equals(InBrushTransform))
	{
		return;
	}
	BrushTransforms[BrushIndex] = InBrushTransform;
	bDirty = true;
}

void FUbstreamCSGSession::SetBrushOperation(const int32 BrushIndex, const EUbstreamCSGOp InOperation)
{
	check(IsInGameThread());
	if (!bActive || !BrushOperations.IsValidIndex(BrushIndex)
		|| BrushOperations[BrushIndex] == InOperation)
	{
		return;
	}
	BrushOperations[BrushIndex] = InOperation;
	bDirty = true;
}

EUbstreamCSGOp FUbstreamCSGSession::GetBrushOperation(const int32 BrushIndex) const
{
	return BrushOperations.IsValidIndex(BrushIndex)
		? BrushOperations[BrushIndex]
		: EUbstreamCSGOp::Subtract;
}

void FUbstreamCSGSession::Tick()
{
	check(IsInGameThread());
	if (bActive && bDirty && !bComputeInFlight)
	{
		LaunchCompute();
	}
}

const FUbstreamCSGResult* FUbstreamCSGSession::GetLatestResult() const
{
	return LatestResult.IsSet() ? &LatestResult.GetValue() : nullptr;
}

TOptional<FDynamicMesh3> FUbstreamCSGSession::TakeResultMeshAndEnd()
{
	check(IsInGameThread());

	TOptional<FDynamicMesh3> ResultMesh;
	if (LatestResult.IsSet() && LatestResult->bSuccess)
	{
		ResultMesh.Emplace(MoveTemp(LatestResult->Mesh));
	}
	End();
	return ResultMesh;
}

/** Everything one chained compute produces: the final result plus the
    intermediates needed to warm the prefix cache. */
struct FUbstreamChainComputeOutput
{
	uint64 SubjectRevision = 0;
	FUbstreamCSGResult Result;
	TArray<TSharedPtr<const FDynamicMesh3, ESPMode::ThreadSafe>> Intermediates;
	TArray<FUbstreamEvalStep> Steps;
	int32 StartIndex = 0;
};

void FUbstreamCSGSession::LaunchCompute()
{
	bDirty = false;
	bComputeInFlight = true;

	// Build the evaluation plan: subtract/union steps keep their relative
	// order; ALL intersect brushes merge into ONE crop step applied last, so
	// several intersect shapes each crop the same accumulated base.
	TArray<FUbstreamEvalStep> Steps;
	FUbstreamEvalStep IntersectStep;
	IntersectStep.Operation = EUbstreamCSGOp::Intersect;
	for (int32 BrushIndex = 0; BrushIndex < BrushMeshes.Num(); ++BrushIndex)
	{
		if (BrushOperations[BrushIndex] == EUbstreamCSGOp::Intersect)
		{
			IntersectStep.BrushIndexes.Add(BrushIndex);
			IntersectStep.Transforms.Add(BrushTransforms[BrushIndex]);
		}
		else
		{
			FUbstreamEvalStep& Step = Steps.AddDefaulted_GetRef();
			Step.BrushIndexes.Add(BrushIndex);
			Step.Transforms.Add(BrushTransforms[BrushIndex]);
			Step.Operation = BrushOperations[BrushIndex];
		}
	}
	if (!IntersectStep.BrushIndexes.IsEmpty())
	{
		Steps.Add(MoveTemp(IntersectStep));
	}

	// Chain-prefix reuse over the evaluation steps.
	int32 StartIndex = 0;
	if (CachedIntermediates.Num() == Steps.Num() && CachedSteps.Num() == Steps.Num())
	{
		while (StartIndex < Steps.Num()
			&& CachedIntermediates[StartIndex].IsValid()
			&& CachedSteps[StartIndex].MatchesSignature(Steps[StartIndex]))
		{
			++StartIndex;
		}
	}
	TSharedPtr<const FDynamicMesh3, ESPMode::ThreadSafe> StartMesh =
		StartIndex > 0 ? CachedIntermediates[StartIndex - 1] : SubjectMesh;
	if (StartIndex >= Steps.Num())
	{
		StartIndex = Steps.Num() - 1;
		StartMesh = StartIndex > 0 ? CachedIntermediates[StartIndex - 1] : SubjectMesh;
	}

	const uint64 LaunchGeneration = Generation;
	const uint64 LaunchSubjectRevision = SubjectRevision;
	TArray<TSharedPtr<const FDynamicMesh3, ESPMode::ThreadSafe>> Brushes = BrushMeshes;
	const FTransform LocalSubjectTransform = SubjectTransform;
	const bool bLocalFillCreatedHoles = bFillCreatedHoles;

	TWeakPtr<FUbstreamCSGSession, ESPMode::ThreadSafe> WeakThis = AsShared();
	Async(EAsyncExecution::ThreadPool,
		[WeakThis, StartMesh, StartIndex, Brushes = MoveTemp(Brushes), LocalSubjectTransform,
		Steps = MoveTemp(Steps), LaunchGeneration, LaunchSubjectRevision, bLocalFillCreatedHoles]()
	{
		FUbstreamChainComputeOutput Output;
		Output.SubjectRevision = LaunchSubjectRevision;
		Output.StartIndex = StartIndex;
		Output.Steps = Steps;
		Output.Intermediates.SetNum(Steps.Num());

		TSharedPtr<const FDynamicMesh3, ESPMode::ThreadSafe> Current = StartMesh;
		for (int32 StepIndex = StartIndex; StepIndex < Steps.Num(); ++StepIndex)
		{
			const FUbstreamEvalStep& Step = Steps[StepIndex];

			// Merge multi-brush steps (the intersect group) into one region:
			// brush0 u brush1 u ... in brush0-local space.
			FDynamicMesh3 StepBrush = *Brushes[Step.BrushIndexes[0]];
			FTransform StepBrushTransform = Step.Transforms[0];
			bool bMergeOk = true;
			for (int32 Extra = 1; Extra < Step.BrushIndexes.Num(); ++Extra)
			{
				FUbstreamCSGResult Merged = FUbstreamCSGKernel::Apply(
					StepBrush, StepBrushTransform,
					*Brushes[Step.BrushIndexes[Extra]], Step.Transforms[Extra],
					EUbstreamCSGOp::Union,
					/*bFillCreatedHoles*/ true);
				if (!Merged.bSuccess)
				{
					bMergeOk = false;
					break;
				}
				StepBrush = MoveTemp(Merged.Mesh);
			}
			if (!bMergeOk)
			{
				Output.Result.bSuccess = false;
				Output.Result.ErrorMessage = TEXT("Could not merge intersect shapes");
				break;
			}

			Output.Result = FUbstreamCSGKernel::Apply(
				*Current, LocalSubjectTransform,
				StepBrush, StepBrushTransform, Step.Operation,
				bLocalFillCreatedHoles);
			if (!Output.Result.bSuccess)
			{
				break;
			}
			Output.Intermediates[StepIndex] =
				MakeShared<const FDynamicMesh3, ESPMode::ThreadSafe>(Output.Result.Mesh);
			Current = Output.Intermediates[StepIndex];
		}

		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, Output = MoveTemp(Output), LaunchGeneration]() mutable
		{
			if (const TSharedPtr<FUbstreamCSGSession, ESPMode::ThreadSafe> Pinned = WeakThis.Pin())
			{
				Pinned->HandleComputeFinished(MoveTemp(Output), LaunchGeneration);
			}
		});
	});
}

void FUbstreamCSGSession::HandleComputeFinished(FUbstreamChainComputeOutput&& Output, const uint64 InGeneration)
{
	// A stale completion from before End()/Begin() must not clear the
	// in-flight guard or surface an outdated preview.
	if (InGeneration != Generation)
	{
		return;
	}

	bComputeInFlight = false;
	// The subject can change while this worker is running. Its intermediates
	// belong to the old base and must never warm the new subject's cache.
	if (Output.SubjectRevision != SubjectRevision)
	{
		if (bActive && bDirty)
		{
			LaunchCompute();
		}
		return;
	}
	LastComputeStartIndex = Output.StartIndex;

	if (Output.Result.bSuccess)
	{
		// Merge: prefix entries stay from the previous cache (they were the
		// start mesh's ancestry), recomputed suffix replaces the rest.
		if (CachedIntermediates.Num() != Output.Steps.Num())
		{
			CachedIntermediates.SetNum(Output.Steps.Num());
			CachedSteps.SetNum(Output.Steps.Num());
		}
		for (int32 Index = Output.StartIndex; Index < Output.Steps.Num(); ++Index)
		{
			CachedIntermediates[Index] = Output.Intermediates[Index];
			CachedSteps[Index] = Output.Steps[Index];
		}
	}

	LatestResult.Emplace(MoveTemp(Output.Result));

	OnPreviewReady.ExecuteIfBound(LatestResult.GetValue());

	// An edit arrived mid-compute; chase it immediately rather than waiting
	// for the next tick.
	if (bActive && bDirty)
	{
		LaunchCompute();
	}
}
