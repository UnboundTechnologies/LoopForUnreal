#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshTransforms.h"
#include "Generators/GridBoxMeshGenerator.h"
#include "Generators/SphereGenerator.h"
#include "Generators/MinimalBoxMeshGenerator.h"
#include "Geometry/UbstreamCSGKernel.h"
#include "Geometry/UbstreamMeshOrientation.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace UE::Geometry;

namespace
{
	FDynamicMesh3 MakeBox(const FVector3d& Center, const FVector3d& HalfExtents, const int32 EdgeVertices = 8)
	{
		FGridBoxMeshGenerator Generator;
		Generator.Box = FOrientedBox3d(Center, HalfExtents);
		Generator.EdgeVertices = FIndex3i(EdgeVertices, EdgeVertices, EdgeVertices);
		Generator.Generate();
		return FDynamicMesh3(&Generator);
	}

	FDynamicMesh3 MakeSphere(const double Radius, const int32 Steps = 16)
	{
		FSphereGenerator Generator;
		Generator.Radius = Radius;
		Generator.NumPhi = Steps;
		Generator.NumTheta = Steps;
		Generator.Generate();
		return FDynamicMesh3(&Generator);
	}

	int32 CountDegenerateTriangles(const FDynamicMesh3& Mesh, const double AreaTolerance = 1e-6)
	{
		int32 Count = 0;
		for (const int32 TriangleId : Mesh.TriangleIndicesItr())
		{
			if (Mesh.GetTriArea(TriangleId) < AreaTolerance)
			{
				++Count;
			}
		}
		return Count;
	}

	struct FCSGCaseExpectations
	{
		/** The op must report success and produce a watertight mesh. */
		bool bExpectWatertight = true;
		/** The result must contain geometry. */
		bool bExpectNonEmpty = true;
	};

	bool RunCSGCase(
		FAutomationTestBase& Test,
		const FString& CaseName,
		const FDynamicMesh3& Subject,
		const FTransform& SubjectTransform,
		const FDynamicMesh3& Brush,
		const FTransform& BrushTransform,
		const EUbstreamCSGOp Operation,
		const FCSGCaseExpectations& Expectations = FCSGCaseExpectations())
	{
		const FUbstreamCSGResult Result = FUbstreamCSGKernel::Apply(
			Subject, SubjectTransform, Brush, BrushTransform, Operation);

		Test.TestTrue(FString::Printf(TEXT("%s: kernel reported success"), *CaseName), Result.bSuccess);
		if (!Result.bSuccess)
		{
			Test.AddError(FString::Printf(TEXT("%s: %s"), *CaseName, *Result.ErrorMessage));
			return false;
		}

		Test.TestTrue(
			FString::Printf(TEXT("%s: result mesh passes CheckValidity"), *CaseName),
			Result.Mesh.CheckValidity(FDynamicMesh3::FValidityOptions(), EValidityCheckFailMode::ReturnOnly));

		if (Expectations.bExpectNonEmpty)
		{
			Test.TestTrue(
				FString::Printf(TEXT("%s: result has triangles"), *CaseName),
				Result.Mesh.TriangleCount() > 0);
		}

		if (Expectations.bExpectWatertight)
		{
			Test.TestEqual(
				FString::Printf(TEXT("%s: result is watertight (open boundary edges)"), *CaseName),
				Result.BoundaryEdgesAfterRepair, 0);
		}

		const int32 DegenerateCount = CountDegenerateTriangles(Result.Mesh);
		Test.TestEqual(
			FString::Printf(TEXT("%s: no degenerate triangles"), *CaseName),
			DegenerateCount, 0);

		Test.AddInfo(FString::Printf(
			TEXT("%s: tris=%d verts=%d boundaryEdges before/after repair=%d/%d degenerate=%d"),
			*CaseName,
			Result.Mesh.TriangleCount(),
			Result.Mesh.VertexCount(),
			Result.BoundaryEdgesBeforeRepair,
			Result.BoundaryEdgesAfterRepair,
			DegenerateCount));

		Test.TestTrue(
			FString::Printf(TEXT("%s: result has attributes (normals/UVs)"), *CaseName),
			Result.Mesh.HasAttributes());

		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUbstreamCSGKernelQualityTest,
	"Ubstream.Modeler.CSGKernel.Quality",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamCSGKernelQualityTest::RunTest(const FString& Parameters)
{
	// Orientation repair must preserve authored hard-edge overlay topology.
	{
		FDynamicMesh3 HardBox = MakeBox(FVector3d::Zero(), FVector3d(50));
		const auto BeforeOverlay = *HardBox.Attributes()->PrimaryNormals();
		UbstreamMeshOrientation::EnsureOutwardOrientation(HardBox);
		const auto* AfterOverlay = HardBox.Attributes()->PrimaryNormals();
		TestEqual(TEXT("Normal seams survive orientation repair"), AfterOverlay->ElementCount(), BeforeOverlay.ElementCount());
		bool bCornersPreserved = true;
		for (int32 Tid : HardBox.TriangleIndicesItr())
		{
			bCornersPreserved &= AfterOverlay->GetTriangle(Tid) == BeforeOverlay.GetTriangle(Tid);
		}
		TestTrue(TEXT("Normal corner assignments are preserved"), bCornersPreserved);
	}
	// A containing intersection must recover the brush's exact local vertices.
	// This exercises rotation plus non-uniform subject scale, not just translation.
	{
		const FTransform SubjectTransform(FRotator(20, 35, 10), FVector(80, -30, 15), FVector(2, 0.7, 1.3));
		const FTransform BrushTransform(FRotator(-15, 60, 25), FVector(82, -28, 16), FVector(1.2, 0.8, 1.1));
		FDynamicMesh3 Brush = MakeBox(FVector3d::Zero(), FVector3d(5), 2);
		TArray<FVector3d> ExpectedVertices;
		for (int32 Vid : Brush.VertexIndicesItr())
		{
			ExpectedVertices.Add(SubjectTransform.InverseTransformPosition(BrushTransform.TransformPosition(Brush.GetVertex(Vid))));
		}
		const auto Result = FUbstreamCSGKernel::Apply(MakeBox(FVector3d::Zero(), FVector3d(100), 2), SubjectTransform,
			Brush, BrushTransform, EUbstreamCSGOp::Intersect);
		TestTrue(TEXT("Scaled/rotated intersection succeeds"), Result.bSuccess);
		for (const FVector3d& Expected : ExpectedVertices)
		{
			double Nearest = TNumericLimits<double>::Max();
			for (int32 Vid : Result.Mesh.VertexIndicesItr())
			{
				Nearest = FMath::Min(Nearest, (Expected - Result.Mesh.GetVertex(Vid)).Length());
			}
			TestTrue(TEXT("Boolean respects exact world-to-subject transform"), Nearest < 1.e-3);
		}
	}

	// Open-shell subtract: with hole filling OFF the cut must stay open (the
	// user sees through it), not get capped by the repair pass.
	{
		UE::Geometry::FDynamicMesh3 ShellMesh;
		ShellMesh.AppendVertex(FVector3d(-100, -100, 0));
		ShellMesh.AppendVertex(FVector3d(100, -100, 0));
		ShellMesh.AppendVertex(FVector3d(100, 100, 0));
		ShellMesh.AppendVertex(FVector3d(-100, 100, 0));
		ShellMesh.AppendTriangle(0, 1, 2);
		ShellMesh.AppendTriangle(0, 2, 3);

		UE::Geometry::FMinimalBoxMeshGenerator ShellCutterGenerator;
		ShellCutterGenerator.Box = UE::Geometry::FOrientedBox3d(FVector3d::Zero(), FVector3d(20.0, 20.0, 20.0));
		ShellCutterGenerator.Generate();

		const FUbstreamCSGResult OpenResult = FUbstreamCSGKernel::Apply(
			ShellMesh, FTransform::Identity,
			UE::Geometry::FDynamicMesh3(&ShellCutterGenerator), FTransform::Identity,
			EUbstreamCSGOp::Subtract,
			/*bFillCreatedHoles*/ false);
		TestTrue(TEXT("Open-shell subtract succeeds"), OpenResult.bSuccess);
		TestTrue(TEXT("Open-shell subtract keeps geometry"), OpenResult.Mesh.TriangleCount() > 0);
		TestTrue(
			TEXT("Open-shell subtract leaves the cut hole open (not capped)"),
			OpenResult.BoundaryEdgesAfterRepair > 0);
	}

	const FDynamicMesh3 SubjectBox = MakeBox(FVector3d::Zero(), FVector3d(50.0, 50.0, 50.0));
	const FDynamicMesh3 BrushSphere = MakeSphere(35.0);
	const FDynamicMesh3 BrushBox = MakeBox(FVector3d::Zero(), FVector3d(30.0, 30.0, 80.0));

	// The bread-and-butter op: carve a sphere bite out of a box corner.
	RunCSGCase(
		*this, TEXT("Subtract sphere from box corner"),
		SubjectBox, FTransform::Identity,
		BrushSphere, FTransform(FVector(50.0, 50.0, 50.0)),
		EUbstreamCSGOp::Subtract);

	// Torture: brush face exactly coplanar with the subject's +X face (both at
	// x=50). This is the classic float-kernel failure case; the repair pass
	// must close whatever the cut leaves open.
	RunCSGCase(
		*this, TEXT("Subtract coplanar box"),
		SubjectBox, FTransform::Identity,
		BrushBox, FTransform(FVector(20.0, 0.0, 0.0)),
		EUbstreamCSGOp::Subtract);

	// Punch a through-hole: the brush spans the subject completely along Z
	// (half-extent 80 vs subject 50), leaving a genus-1 result.
	RunCSGCase(
		*this, TEXT("Subtract through-hole"),
		SubjectBox, FTransform::Identity,
		BrushBox, FTransform(FVector(0.0, 0.0, 0.0)),
		EUbstreamCSGOp::Subtract);

	// Union of overlapping shapes must weld into a single closed surface.
	RunCSGCase(
		*this, TEXT("Union box and sphere"),
		SubjectBox, FTransform::Identity,
		BrushSphere, FTransform(FVector(40.0, 0.0, 0.0)),
		EUbstreamCSGOp::Union);

	// Intersection of two boxes at an angle.
	RunCSGCase(
		*this, TEXT("Intersect rotated boxes"),
		SubjectBox, FTransform::Identity,
		BrushBox, FTransform(FRotator(0.0, 30.0, 0.0), FVector(20.0, 10.0, 0.0)),
		EUbstreamCSGOp::Intersect);

	// Non-uniform subject transform: result must come back in subject-local
	// space and still be closed.
	RunCSGCase(
		*this, TEXT("Subtract with transformed subject"),
		SubjectBox, FTransform(FRotator(0.0, 45.0, 0.0), FVector(100.0, 0.0, 0.0), FVector(1.0, 2.0, 1.0)),
		BrushSphere, FTransform(FVector(100.0, 0.0, 60.0)),
		EUbstreamCSGOp::Subtract);

	// Brush fully inside the subject: subtract must produce an internal
	// cavity while staying closed (two shells, zero boundary edges).
	RunCSGCase(
		*this, TEXT("Subtract fully-enclosed brush"),
		SubjectBox, FTransform::Identity,
		MakeSphere(20.0), FTransform::Identity,
		EUbstreamCSGOp::Subtract);

	// Orientation repair. Closed inverted meshes are the easy case; the ray
	// probe exists for OPEN shells (tub/bowl-like imports), whose centroid
	// score nearly cancels - an inverted one then renders backfaces-only and
	// every cut inherits the flip into its cavity walls.
	{
		// Open bowl: sphere with its top cap removed.
		const auto MakeOpenBowl = []()
		{
			FDynamicMesh3 Bowl = MakeSphere(50.0);
			TArray<int32> TrianglesToRemove;
			for (const int32 TriangleId : Bowl.TriangleIndicesItr())
			{
				FVector3d A, B, C;
				Bowl.GetTriVertices(TriangleId, A, B, C);
				if (((A + B + C) / 3.0).Z > 30.0)
				{
					TrianglesToRemove.Add(TriangleId);
				}
			}
			for (const int32 TriangleId : TrianglesToRemove)
			{
				Bowl.RemoveTriangle(TriangleId);
			}
			return Bowl;
		};

		FDynamicMesh3 ClosedInverted = MakeSphere(50.0);
		ClosedInverted.ReverseOrientation(true);
		TestTrue(
			TEXT("Orientation: inverted closed sphere is flipped back (fast path)"),
			UbstreamMeshOrientation::EnsureOutwardOrientation(ClosedInverted));

		FDynamicMesh3 CorrectBowl = MakeOpenBowl();
		TestTrue(
			TEXT("Orientation probe: correct open bowl reads mostly front-facing"),
			UbstreamMeshOrientation::ComputeBackfaceFirstFraction(CorrectBowl) <= 0.4);
		TestFalse(
			TEXT("Orientation: correct open bowl is left alone (ray probe)"),
			UbstreamMeshOrientation::EnsureOutwardOrientation(CorrectBowl, /*bRayProbe*/ true));

		FDynamicMesh3 InvertedBowl = MakeOpenBowl();
		InvertedBowl.ReverseOrientation(true);
		TestTrue(
			TEXT("Orientation probe: inverted open bowl reads mostly back-facing"),
			UbstreamMeshOrientation::ComputeBackfaceFirstFraction(InvertedBowl) >= 0.6);
		TestTrue(
			TEXT("Orientation: inverted open bowl is flipped back (ray probe)"),
			UbstreamMeshOrientation::EnsureOutwardOrientation(InvertedBowl, /*bRayProbe*/ true));
		TestTrue(
			TEXT("Orientation: repaired bowl now reads front-facing"),
			UbstreamMeshOrientation::ComputeBackfaceFirstFraction(InvertedBowl) <= 0.4);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
