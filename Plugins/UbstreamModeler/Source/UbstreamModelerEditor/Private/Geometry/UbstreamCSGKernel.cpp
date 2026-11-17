#include "Geometry/UbstreamCSGKernel.h"

#include "CompGeom/PolygonTriangulation.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMesh/MeshTransforms.h"
#include "DynamicMesh/Operations/MergeCoincidentMeshEdges.h"
#include "DynamicMeshEditor.h"
#include "Geometry/UbstreamMeshOrientation.h"
#include "MeshBoundaryLoops.h"
#include "Operations/MeshBoolean.h"
#include "Operations/MinimalHoleFiller.h"

using namespace UE::Geometry;

namespace
{
	FMeshBoolean::EBooleanOp ToBooleanOp(const EUbstreamCSGOp Operation)
	{
		switch (Operation)
		{
		case EUbstreamCSGOp::Union:
			return FMeshBoolean::EBooleanOp::Union;
		case EUbstreamCSGOp::Intersect:
			return FMeshBoolean::EBooleanOp::Intersect;
		case EUbstreamCSGOp::Subtract:
		default:
			return FMeshBoolean::EBooleanOp::Difference;
		}
	}

	int32 CountBoundaryEdges(const FDynamicMesh3& Mesh)
	{
		int32 Count = 0;
		for (const int32 EdgeId : Mesh.BoundaryEdgeIndicesItr())
		{
			(void)EdgeId;
			++Count;
		}
		return Count;
	}

	/** Fill the open loops the boolean left behind, following the repair pass
	    the engine's Boolean tool applies: minimal fill per loop, then flat
	    normals and planar-projected UVs on the patch. */
	void FillCreatedHoles(FDynamicMesh3& Mesh, const TArray<int32>& CreatedBoundaryEdges)
	{
		FMeshBoundaryLoops OpenBoundary(&Mesh, false);
		TSet<int32> ConsiderEdges(CreatedBoundaryEdges);
		OpenBoundary.EdgeFilterFunc = [&ConsiderEdges](int EdgeId)
		{
			return ConsiderEdges.Contains(EdgeId);
		};
		OpenBoundary.Compute();

		const bool bNeedsNormalsAndUVs = Mesh.HasAttributes();
		const double MeshUVScaleFactor = 1.0 / FMath::Max(Mesh.GetBounds().MaxDim(), KINDA_SMALL_NUMBER);
		for (FEdgeLoop& Loop : OpenBoundary.Loops)
		{
			const int32 NewGroupId = Mesh.AllocateTriangleGroup();
			FMinimalHoleFiller Filler(&Mesh, Loop);
			if (!Filler.Fill(NewGroupId))
			{
				continue;
			}

			if (bNeedsNormalsAndUVs)
			{
				TArray<FVector3d> VertexPositions;
				Loop.GetVertices(VertexPositions);
				FVector3d PlaneOrigin, PlaneNormal;
				PolygonTriangulation::ComputePolygonPlane<double>(VertexPositions, PlaneNormal, PlaneOrigin);
				PlaneNormal *= -1.0;

				FDynamicMeshEditor Editor(&Mesh);
				const FFrame3d ProjectionFrame(PlaneOrigin, PlaneNormal);
				Editor.SetTriangleNormals(Filler.NewTriangles, static_cast<FVector3f>(PlaneNormal));
				Editor.SetTriangleUVsFromProjection(Filler.NewTriangles, ProjectionFrame, MeshUVScaleFactor);
			}
		}
	}
}

FUbstreamCSGResult FUbstreamCSGKernel::Apply(
	const FDynamicMesh3& Subject,
	const FTransform& SubjectTransform,
	const FDynamicMesh3& Brush,
	const FTransform& BrushTransform,
	const EUbstreamCSGOp Operation,
	const bool bFillCreatedHoles)
{
	FUbstreamCSGResult Result;

	if (Subject.TriangleCount() == 0 || Brush.TriangleCount() == 0)
	{
		Result.ErrorMessage = TEXT("CSG inputs must both have triangles");
		return Result;
	}

	const FVector SubjectScale = SubjectTransform.GetScale3D().GetAbs();
	if (SubjectScale.GetMin() <= UE_SMALL_NUMBER)
	{
		Result.ErrorMessage = TEXT("The subject has a zero scale axis; restore its scale before cutting.");
		return Result;
	}

	// Work in subject-local space: leave the subject at identity and bring the
	// brush across, so the output can replace the subject mesh in place.
	FTransform BrushToSubject = BrushTransform * SubjectTransform.Inverse();
	const FDynamicMesh3* BooleanBrush = &Brush;
	TOptional<FDynamicMesh3> ExactBrush;
	if (!SubjectTransform.GetScale3D().AllComponentsEqual(KINDA_SMALL_NUMBER))
	{
		// A rotated non-uniform inverse cannot be represented by one SRT.
		// Apply the two transforms sequentially, including inverse-transpose
		// normals and reflected winding. Keep the common uniform path cheap.
		ExactBrush.Emplace(Brush);
		MeshTransforms::ApplyTransform(*ExactBrush, FTransformSRT3d(BrushTransform), true);
		MeshTransforms::ApplyTransformInverse(*ExactBrush, FTransformSRT3d(SubjectTransform), true);
		BooleanBrush = &ExactBrush.GetValue();
		BrushToSubject = FTransform::Identity;
	}

	FMeshBoolean MeshBoolean(
		&Subject,
		FTransformSRT3d::Identity(),
		BooleanBrush,
		FTransformSRT3d(BrushToSubject),
		&Result.Mesh,
		ToBooleanOp(Operation));
	MeshBoolean.bPutResultInInputSpace = true;
	// Collapse the sliver triangles the cut generates; this is the main
	// output-quality knob and the reason results look modeled, not shattered.
	MeshBoolean.bSimplifyAlongNewEdges = true;
	// The subject is user-authored, the brush procedural: only the subject's
	// UVs are worth preserving through simplification.
	MeshBoolean.PreserveUVsOnlyForMesh = 0;
	const bool bComputeSucceeded = MeshBoolean.Compute();

	Result.BoundaryEdgesBeforeRepair = MeshBoolean.CreatedBoundaryEdges.Num();

	if (!bComputeSucceeded && MeshBoolean.CreatedBoundaryEdges.IsEmpty())
	{
		Result.ErrorMessage = TEXT("Mesh boolean failed to compute");
		return Result;
	}

	if (!MeshBoolean.CreatedBoundaryEdges.IsEmpty() && bFillCreatedHoles)
	{
		FillCreatedHoles(Result.Mesh, MeshBoolean.CreatedBoundaryEdges);

		// Fills can leave coincident-but-unwelded seams; a weld pass closes
		// those without disturbing welded geometry.
		if (CountBoundaryEdges(Result.Mesh) > 0)
		{
			FMergeCoincidentMeshEdges Weld(&Result.Mesh);
			Weld.Apply();
		}
	}

	Result.BoundaryEdgesAfterRepair = CountBoundaryEdges(Result.Mesh);
	// Booleans + hole fills can leave attribute normals pointing the wrong way
	// relative to final winding. Recompute so the stage doesn't render as
	// "only backfaces" after a cut.
	UbstreamMeshOrientation::EnsureOutwardOrientation(Result.Mesh);
	Result.bSuccess = true;
	return Result;
}
