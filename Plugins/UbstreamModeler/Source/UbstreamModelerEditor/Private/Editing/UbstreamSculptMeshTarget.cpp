#include "Editing/UbstreamSculptMeshTarget.h"

#include "Components/DynamicMeshComponent.h"

void UUbstreamSculptMeshTarget::Initialize(UDynamicMeshComponentToolTarget* InSource)
{
	Source = InSource;
	InitializeComponent(Source->GetDynamicMeshComponent());
}

bool UUbstreamSculptMeshTarget::IsValid() const
{
	return Source && Source->IsValid();
}

UE::Geometry::FDynamicMesh3 UUbstreamSculptMeshTarget::GetDynamicMesh()
{
	UE::Geometry::FDynamicMesh3 Mesh = Source->GetDynamicMesh();
	// UE 5.7's vertex sculpt stamp uses TriangleCount as an ID-array bound.
	// Boolean/remesh results can have larger IDs, causing out-of-bounds writes.
	bCompacted = !Mesh.IsCompact();
	if (bCompacted) Mesh.CompactInPlace();
	return Mesh;
}

void UUbstreamSculptMeshTarget::CommitDynamicMesh(
	const UE::Geometry::FDynamicMesh3& Mesh, const FDynamicMeshCommitInfo& Info)
{
	FDynamicMeshCommitInfo CommitInfo = Info;
	// Reindexing requires a full replacement, including its undo record;
	// a vertex-only update would write compact IDs into the sparse source.
	CommitInfo.bTopologyChanged |= bCompacted;
	Source->CommitDynamicMesh(Mesh, CommitInfo);
}

int32 UUbstreamSculptMeshTarget::GetNumMaterials() const { return Source->GetNumMaterials(); }
UMaterialInterface* UUbstreamSculptMeshTarget::GetMaterial(int32 Index) const { return Source->GetMaterial(Index); }
void UUbstreamSculptMeshTarget::GetMaterialSet(FComponentMaterialSet& Out, bool bPreferAssetMaterials) const
{
	Source->GetMaterialSet(Out, bPreferAssetMaterials);
}
bool UUbstreamSculptMeshTarget::CommitMaterialSetUpdate(const FComponentMaterialSet& Materials, bool bApplyToAsset)
{
	return Source->CommitMaterialSetUpdate(Materials, bApplyToAsset);
}
