#pragma once

#include "MeshVertexSculptTool.h"
#include "UbstreamVertexSculptTool.generated.h"

/** Epic's sculpt tool with a revision for completed strokes and undo/redo. */
UCLASS()
class UUbstreamVertexSculptTool : public UMeshVertexSculptTool
{
	GENERATED_BODY()
public:
	virtual void Setup() override;
	virtual void Shutdown(EToolShutdownType ShutdownType) override;
	virtual void UpdateToolMeshes(TFunctionRef<TUniquePtr<FMeshRegionChangeBase>(UE::Geometry::FDynamicMesh3&, int32)> UpdateMesh) override;
	/** Idle ticks are O(1); copy only once per changed, completed stroke. */
	bool CopyMeshIfChanged(uint64& LastRevision, UE::Geometry::FDynamicMesh3& OutMesh) const;
	/** Only our idle working mesh has a reliable revision. Active strokes use
	    the normal per-capture copy so their intermediate results stay live. */
	bool TryGetStableCaptureRevision(const UBaseDynamicMeshComponent* Source, uint64& OutRevision) const;

protected:
	virtual void OnEndStroke() override;
	virtual void OnCancelStroke() override;

private:
	friend class FUbstreamSculptRevisionTest;
	friend class FUbstreamCrashStressTest;
	void BindMeshChanges();
	uint64 MeshRevision = 1;
	FDelegateHandle MeshRegionChangedHandle;
	FDelegateHandle MeshChangedHandle;
};

UCLASS()
class UUbstreamVertexSculptToolBuilder : public UMeshVertexSculptToolBuilder
{
	GENERATED_BODY()
public:
	virtual UMeshSurfacePointTool* CreateNewTool(const FToolBuilderState& SceneState) const override;
};
