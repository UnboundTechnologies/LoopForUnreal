#include "Geometry/UbstreamMeshOrientation.h"

#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/MeshNormals.h"

using namespace UE::Geometry;

namespace UbstreamMeshOrientation
{
	double ComputeOutwardOrientationScore(const FDynamicMesh3& Mesh)
	{
		if (Mesh.TriangleCount() == 0)
		{
			return 0.0;
		}

		const FAxisAlignedBox3d Bounds = Mesh.GetBounds(true);
		if (Bounds.MaxDim() <= KINDA_SMALL_NUMBER)
		{
			return 0.0;
		}

		const FVector3d Center = Bounds.Center();
		double Score = 0.0;
		double TotalArea = 0.0;

		for (const int32 TriangleId : Mesh.TriangleIndicesItr())
		{
			const FIndex3i Triangle = Mesh.GetTriangle(TriangleId);
			const FVector3d A = Mesh.GetVertex(Triangle.A);
			const FVector3d B = Mesh.GetVertex(Triangle.B);
			const FVector3d C = Mesh.GetVertex(Triangle.C);
			// RENDERED front-face normal. UE draws left-handed: the visible
			// front of (A,B,C) is (C-A)x(B-A), the NEGATION of the textbook
			// right-handed cross. Engine generators prove it (FSphereGenerator
			// spheres render front-out, and their right-handed cross points
			// INWARD). Scoring with the right-handed cross inverts every
			// verdict and makes the "fixer" flip correct meshes inside-out.
			const FVector3d Cross = (C - A).Cross(B - A);
			const double TwiceArea = Cross.Length();
			if (TwiceArea <= KINDA_SMALL_NUMBER)
			{
				continue;
			}

			const double Area = 0.5 * TwiceArea;
			const FVector3d FaceNormal = Cross / TwiceArea;
			const FVector3d FaceCentroid = (A + B + C) / 3.0;
			Score += Area * FaceNormal.Dot(FaceCentroid - Center);
			TotalArea += Area;
		}

		return TotalArea > KINDA_SMALL_NUMBER ? (Score / TotalArea) : 0.0;
	}

	void RecomputeNormals(FDynamicMesh3& Mesh)
	{
		if (Mesh.TriangleCount() == 0)
		{
			return;
		}

		if (!Mesh.HasAttributes())
		{
			Mesh.EnableAttributes();
		}

		// Prefer overlay normals (what DynamicMeshComponent actually renders).
		if (FDynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals())
		{
			// Normal splits encode hard edges. Recompute values without welding
			// their overlay topology; initialize only meshes that have no normals.
			if (NormalOverlay->ElementCount() == 0)
			{
				FMeshNormals::InitializeOverlayToPerVertexNormals(NormalOverlay, false);
			}
			else
			{
				for (const int32 Tid : Mesh.TriangleIndicesItr())
				{
					if (!NormalOverlay->IsSetTriangle(Tid))
					{
						const int32 A = NormalOverlay->AppendElement(FVector3f::Zero());
						const int32 B = NormalOverlay->AppendElement(FVector3f::Zero());
						const int32 C = NormalOverlay->AppendElement(FVector3f::Zero());
						NormalOverlay->SetTriangle(Tid, FIndex3i(A, B, C));
					}
				}
			}
			FMeshNormals::QuickRecomputeOverlayNormals(Mesh, /*bInvert*/ false);
		}

		if (Mesh.HasVertexNormals())
		{
			FMeshNormals::QuickComputeVertexNormals(Mesh, /*bInvert*/ false);
		}
		else
		{
			Mesh.EnableVertexNormals(FVector3f::UnitZ());
			FMeshNormals::QuickComputeVertexNormals(Mesh, /*bInvert*/ false);
		}
	}

	double ComputeBackfaceFirstFraction(const FDynamicMesh3& Mesh)
	{
		if (Mesh.TriangleCount() == 0)
		{
			return 0.5;
		}
		const FAxisAlignedBox3d Bounds = Mesh.GetBounds(true);
		if (Bounds.MaxDim() <= KINDA_SMALL_NUMBER)
		{
			return 0.5;
		}

		FDynamicMeshAABBTree3 Tree(&Mesh, /*bAutoBuild*/ true);
		const FVector3d Center = Bounds.Center();
		const double OrbitRadius = FMath::Max(Bounds.DiagonalLength(), double(KINDA_SMALL_NUMBER)) * 1.5;

		// Deterministic uniform directions (Fibonacci sphere): every probe is
		// an outside observer looking at the mesh; no triangle-picking bias.
		constexpr int32 SampleCount = 128;
		const double GoldenAngle = UE_DOUBLE_PI * (3.0 - FMath::Sqrt(5.0));
		int32 BackFirstVotes = 0;
		int32 HitCount = 0;
		for (int32 Index = 0; Index < SampleCount; ++Index)
		{
			const double Z = 1.0 - 2.0 * (Index + 0.5) / SampleCount;
			const double RingRadius = FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));
			const double Phi = GoldenAngle * Index;
			const FVector3d Direction(RingRadius * FMath::Cos(Phi), RingRadius * FMath::Sin(Phi), Z);
			const FRay3d Ray(Center + Direction * OrbitRadius, -Direction);

			double RayParameter = TNumericLimits<double>::Max();
			int32 HitTriangleId = IndexConstants::InvalidID;
			if (!Tree.FindNearestHitTriangle(Ray, RayParameter, HitTriangleId))
			{
				continue;
			}
			FVector3d A, B, C;
			Mesh.GetTriVertices(HitTriangleId, A, B, C);
			// Rendered front-face normal (left-handed; see score above).
			const FVector3d FaceNormal = (C - A).Cross(B - A);
			if (FaceNormal.SquaredLength() <= KINDA_SMALL_NUMBER)
			{
				continue;
			}
			++HitCount;
			if (FaceNormal.Dot(Ray.Direction) > 0.0)
			{
				++BackFirstVotes;
			}
		}
		return HitCount > 0 ? static_cast<double>(BackFirstVotes) / static_cast<double>(HitCount) : 0.5;
	}

	bool EnsureOutwardOrientation(FDynamicMesh3& Mesh, bool bRayProbe)
	{
		if (Mesh.TriangleCount() == 0)
		{
			return false;
		}

		bool bReverse = false;
		bool bDecided = false;
		if (bRayProbe)
		{
			// The probe measures the user-facing symptom directly ("how much
			// of the visible outside is backfaces"), so when it is decisive it
			// outranks the centroid score - open shells (tubs, plates) cancel
			// the centroid sum but vote clearly here.
			const double BackFraction = ComputeBackfaceFirstFraction(Mesh);
			if (BackFraction >= 0.6)
			{
				bReverse = true;
				bDecided = true;
			}
			else if (BackFraction <= 0.4)
			{
				bReverse = false;
				bDecided = true;
			}
		}

		if (!bDecided)
		{
			const double Score = ComputeOutwardOrientationScore(Mesh);
			// Only flip when clearly inverted. Near-zero scores (open/noisy shells)
			// are left alone so we do not thrash legitimate open geometry.
			const FAxisAlignedBox3d Bounds = Mesh.GetBounds(true);
			const double Scale = FMath::Max(Bounds.MaxDim(), KINDA_SMALL_NUMBER);
			bReverse = Score < -1.0e-4 * Scale;
		}

		if (!bReverse)
		{
			// Still recompute normals so lighting matches winding after import/boolean.
			RecomputeNormals(Mesh);
			return false;
		}

		Mesh.ReverseOrientation(/*bFlipNormals*/ true);
		RecomputeNormals(Mesh);
		return true;
	}
}
