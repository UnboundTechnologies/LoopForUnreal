#pragma once

#include "ToolTargets/DynamicMeshComponentToolTarget.h"
#include "UbstreamSculptMeshTarget.generated.h"

/** Gives sculpt a dense working copy without changing the staged mesh on Cancel. */
UCLASS(Transient)
class UUbstreamSculptMeshTarget : public UPrimitiveComponentToolTarget,
	public IDynamicMeshProvider, public IDynamicMeshCommitter, public IMaterialProvider
{
	GENERATED_BODY()
public:
	void Initialize(UDynamicMeshComponentToolTarget* InSource);
	virtual bool IsValid() const override;
	virtual UE::Geometry::FDynamicMesh3 GetDynamicMesh() override;
	virtual void CommitDynamicMesh(const UE::Geometry::FDynamicMesh3& Mesh, const FDynamicMeshCommitInfo& Info) override;
	virtual int32 GetNumMaterials() const override;
	virtual UMaterialInterface* GetMaterial(int32 Index) const override;
	virtual void GetMaterialSet(FComponentMaterialSet& Out, bool bPreferAssetMaterials) const override;
	virtual bool CommitMaterialSetUpdate(const FComponentMaterialSet& Materials, bool bApplyToAsset) override;

private:
	UPROPERTY()
	TObjectPtr<UDynamicMeshComponentToolTarget> Source;
	bool bCompacted = false;
};
