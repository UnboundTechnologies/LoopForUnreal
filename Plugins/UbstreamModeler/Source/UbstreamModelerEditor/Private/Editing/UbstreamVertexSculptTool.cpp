#include "Editing/UbstreamVertexSculptTool.h"

#include "Components/DynamicMeshComponent.h"
#include "Editing/UbstreamSculptMeshTarget.h"
#include "InteractiveToolManager.h"

void UUbstreamVertexSculptTool::Setup()
{
	if (UDynamicMeshComponentToolTarget* SourceTarget = Cast<UDynamicMeshComponentToolTarget>(Target))
	{
		UUbstreamSculptMeshTarget* SculptTarget = NewObject<UUbstreamSculptMeshTarget>(this);
		SculptTarget->Initialize(SourceTarget);
		Target = SculptTarget;
	}
	Super::Setup();
	MeshRevision = 1;
	BindMeshChanges();
}

void UUbstreamVertexSculptTool::BindMeshChanges()
{
	// SetMesh/EditMesh also cover attribute-only changes that a position
	// fingerprint cannot detect. Region notifications cover undo/redo.
	MeshChangedHandle = DynamicMeshComponent->OnMeshChanged.AddWeakLambda(this,
		[this]() { ++MeshRevision; });
	MeshRegionChangedHandle = DynamicMeshComponent->OnMeshRegionChanged.AddWeakLambda(this,
		[this](UDynamicMeshComponent*, const FMeshRegionChangeBase*, bool)
		{
			++MeshRevision;
		});
}

void UUbstreamVertexSculptTool::Shutdown(EToolShutdownType ShutdownType)
{
	// Epic's stamp and undo workers capture this tool and its working mesh.
	// Join them before base Shutdown destroys the component or GC can collect
	// the tool, including when closing immediately after a stroke or undo.
	WaitForPendingStampUpdateConst();
	if (UndoUpdateFuture.IsValid())
	{
		WaitForPendingUndoRedoUpdate();
	}
	if (DynamicMeshComponent)
	{
		DynamicMeshComponent->OnMeshRegionChanged.Remove(MeshRegionChangedHandle);
		DynamicMeshComponent->OnMeshChanged.Remove(MeshChangedHandle);
	}
	Super::Shutdown(ShutdownType);
}

void UUbstreamVertexSculptTool::OnEndStroke()
{
	Super::OnEndStroke();
	++MeshRevision;
}

void UUbstreamVertexSculptTool::OnCancelStroke()
{
	Super::OnCancelStroke();
	++MeshRevision;
}

void UUbstreamVertexSculptTool::UpdateToolMeshes(
	TFunctionRef<TUniquePtr<FMeshRegionChangeBase>(UE::Geometry::FDynamicMesh3&, int32)> UpdateMesh)
{
	if (!AllowToolMeshUpdates())
	{
		return;
	}
	Super::UpdateToolMeshes(UpdateMesh);
	++MeshRevision;
}

bool UUbstreamVertexSculptTool::CopyMeshIfChanged(uint64& LastRevision, UE::Geometry::FDynamicMesh3& OutMesh) const
{
	if (LastRevision == MeshRevision || InStroke() || IsStampPending() || !AllowToolMeshUpdates())
	{
		return false;
	}
	WaitForPendingStampUpdateConst();
	WaitForPendingUndoRedoUpdate();
	OutMesh = *GetSculptMesh();
	LastRevision = MeshRevision;
	return true;
}

bool UUbstreamVertexSculptTool::TryGetStableCaptureRevision(
	const UBaseDynamicMeshComponent* Source, uint64& OutRevision) const
{
	if (Source != DynamicMeshComponent || InStroke() || IsStampPending() || !AllowToolMeshUpdates())
	{
		return false;
	}
	WaitForPendingStampUpdateConst();
	WaitForPendingUndoRedoUpdate();
	OutRevision = MeshRevision;
	return true;
}

UMeshSurfacePointTool* UUbstreamVertexSculptToolBuilder::CreateNewTool(const FToolBuilderState& SceneState) const
{
	UUbstreamVertexSculptTool* Tool = NewObject<UUbstreamVertexSculptTool>(SceneState.ToolManager);
	Tool->SetWorld(SceneState.World);
	Tool->SetDefaultPrimaryBrushID(DefaultPrimaryBrushID);
	return Tool;
}
