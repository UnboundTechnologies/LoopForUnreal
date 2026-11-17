#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"

enum class EUbstreamCSGOp : uint8
{
	Subtract,
	Union,
	Intersect
};

struct FUbstreamCSGResult
{
	UE::Geometry::FDynamicMesh3 Mesh;
	bool bSuccess = false;
	/** Boundary edges the boolean failed to weld, before repair. */
	int32 BoundaryEdgesBeforeRepair = 0;
	/** Boundary edges still open after repair; 0 means the result is watertight. */
	int32 BoundaryEdgesAfterRepair = 0;
	FString ErrorMessage;

	bool IsWatertight() const { return bSuccess && BoundaryEdgesAfterRepair == 0; }
};

/**
 * UI-free CSG between a subject mesh and a brush mesh, with the repair
 * post-pass the interactive tools apply (hole fill with projected UVs and
 * normals, residual edge weld). Pure function over plain geometry: no
 * UObjects, callable from worker threads.
 *
 * The result is returned in the subject's local space so it can be written
 * straight back to the subject component without touching its transform.
 */
class UBSTREAMMODELEREDITOR_API FUbstreamCSGKernel
{
public:
	/**
	 * bFillCreatedHoles: cap boundary edges the boolean introduces. Correct
	 * for watertight subjects (keeps results solid); WRONG for open shells,
	 * where it seals the very hole a subtract cuts - pass false for those.
	 */
	static FUbstreamCSGResult Apply(
		const UE::Geometry::FDynamicMesh3& Subject,
		const FTransform& SubjectTransform,
		const UE::Geometry::FDynamicMesh3& Brush,
		const FTransform& BrushTransform,
		EUbstreamCSGOp Operation,
		bool bFillCreatedHoles = true);
};
