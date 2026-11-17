#include "Geometry/UbstreamRemeshJob.h"

#include "Async/Async.h"
#include "CleaningOps/RemeshMeshOp.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Util/ProgressCancel.h"

void FUbstreamRemeshJob::Start(double TargetEdgeLength, TFunction<void()> OnDone)
{
	const auto Self = AsShared();
	Async(EAsyncExecution::ThreadPool, [Self, TargetEdgeLength, OnDone = MoveTemp(OnDone)]() mutable
	{
		if (!Self->IsCancelled())
		{
			FProgressCancel Progress;
			Progress.CancelF = [Self]() { return Self->IsCancelled(); };
			UE::Geometry::FRemeshMeshOp Op;
			Op.OriginalMesh = Self->Input;
			Op.OriginalMeshSpatial = MakeShared<UE::Geometry::FDynamicMeshAABBTree3, ESPMode::ThreadSafe>(Self->Input.Get(), true);
			Op.RemeshType = ERemeshType::Standard;
			Op.TargetEdgeLength = TargetEdgeLength;
			Op.RemeshIterations = 20;
			Op.MaxRemeshIterations = 20;
			Op.SmoothingStrength = 0.0f;
			// UE 5.7 leaves these enum fields uninitialized.
			Op.SmoothingType = ERemeshSmoothingType::Uniform;
			Op.MeshBoundaryConstraint = UE::Geometry::EEdgeRefineFlags::NoConstraint;
			Op.GroupBoundaryConstraint = UE::Geometry::EEdgeRefineFlags::NoConstraint;
			Op.MaterialBoundaryConstraint = UE::Geometry::EEdgeRefineFlags::NoConstraint;
			Op.bReproject = true;
			Op.bPreventNormalFlips = true;
			Op.bDiscardAttributes = false;
			Op.SetTransform(UE::Geometry::FTransformSRT3d::Identity());
			Op.CalculateResult(&Progress);
			if (!Self->IsCancelled())
			{
				Self->Result = Op.ExtractResult();
				if (!Self->Result || Self->Result->TriangleCount() == 0)
				{
					Self->Error = TEXT("Remesh produced no geometry. The original mesh was kept.");
				}
			}
		}
		// The task owns its input until compute ends; the queued completion owns
		// its result. Neither depends on the viewport or any UObject staying alive.
		AsyncTask(ENamedThreads::GameThread, [Self, OnDone = MoveTemp(OnDone)]() { OnDone(); });
	});
}
