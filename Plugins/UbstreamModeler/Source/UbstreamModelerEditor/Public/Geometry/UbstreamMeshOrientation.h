#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"

namespace UbstreamMeshOrientation
{
	/**
	 * Area-weighted "outwardness" of triangle winding vs the mesh bounds center.
	 * Positive => overall front faces point away from the center (expected).
	 * Strongly negative => winding is inside-out (only backfaces visible from outside).
	 */
	double ComputeOutwardOrientationScore(const UE::Geometry::FDynamicMesh3& Mesh);

	/** Recompute vertex + overlay normals from triangle winding. */
	void RecomputeNormals(UE::Geometry::FDynamicMesh3& Mesh);

	/**
	 * Fraction of outside-in probe rays whose FIRST surface hit is back-facing.
	 * Rays start on a sphere around the mesh bounds and aim at the center, so
	 * this measures literally "how much of the visible outside is backfaces".
	 * ~0 = outward, ~1 = inside-out, ~0.5 = undecidable (mixed winding).
	 * Robust for OPEN shells (tubs, plates, scans) where the centroid score
	 * nearly cancels; costs an AABB-tree build, so not for per-preview use.
	 */
	double ComputeBackfaceFirstFraction(const UE::Geometry::FDynamicMesh3& Mesh);

	/**
	 * If the mesh is inside-out, reverse triangle winding (and normals) then
	 * recompute normals. Returns true when a reverse was applied.
	 * Default (fast) mode uses the centroid score only: safe for typical
	 * closed/nearly-closed meshes, leaves ambiguous open shells alone - cheap
	 * enough for per-preview kernel results.
	 * bRayProbe mode lets the ray-parity vote decide first (import and stage
	 * load): open shells that the centroid score cannot classify get fixed.
	 */
	bool EnsureOutwardOrientation(UE::Geometry::FDynamicMesh3& Mesh, bool bRayProbe = false);
}
