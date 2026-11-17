#include "Editing/UbstreamStageEditorMode.h"

#include "BaseGizmos/TransformGizmoUtil.h"
#include "CSGMeshesTool.h"
#include "DisplaceMeshTool.h"
#include "EditMeshPolygonsTool.h"
#include "HoleFillTool.h"
#include "InteractiveToolManager.h"
#include "LatticeDeformerTool.h"
#include "Editing/UbstreamVertexSculptTool.h"
#include "OffsetMeshTool.h"
#include "RemeshMeshTool.h"
#include "SmoothMeshTool.h"
#include "WeldMeshEdgesTool.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "ToolTargetManager.h"
#include "ToolTargets/DynamicMeshComponentToolTarget.h"
#include "ToolTargets/StaticMeshComponentToolTarget.h"

#define LOCTEXT_NAMESPACE "UbstreamStageEditorMode"

const FEditorModeID UUbstreamStageEditorMode::EM_UbstreamStageEditorModeId = TEXT("EM_UbstreamStageEditorMode");
const FString UUbstreamStageEditorMode::PolyEditToolIdentifier = TEXT("UbstreamPolyEditTool");
const FString UUbstreamStageEditorMode::VertexSculptToolIdentifier = TEXT("UbstreamVertexSculptTool");
const FString UUbstreamStageEditorMode::SmoothToolIdentifier = TEXT("UbstreamSmoothTool");
const FString UUbstreamStageEditorMode::OffsetToolIdentifier = TEXT("UbstreamOffsetTool");
const FString UUbstreamStageEditorMode::DisplaceToolIdentifier = TEXT("UbstreamDisplaceTool");
const FString UUbstreamStageEditorMode::LatticeToolIdentifier = TEXT("UbstreamLatticeTool");
const FString UUbstreamStageEditorMode::RemeshToolIdentifier = TEXT("UbstreamRemeshTool");
const FString UUbstreamStageEditorMode::CSGMeshesToolIdentifier = TEXT("UbstreamCSGMeshesTool");
const FString UUbstreamStageEditorMode::TrimMeshesToolIdentifier = TEXT("UbstreamTrimMeshesTool");
const FString UUbstreamStageEditorMode::WeldToolIdentifier = TEXT("UbstreamWeldTool");
const FString UUbstreamStageEditorMode::HoleFillToolIdentifier = TEXT("UbstreamHoleFillTool");

UUbstreamStageEditorMode::UUbstreamStageEditorMode()
{
	Info = FEditorModeInfo(
		EM_UbstreamStageEditorModeId,
		LOCTEXT("UbstreamStageEditorModeName", "Unbound Loop Stage Editing"),
		FSlateIcon(),
		/*bVisible*/ false);
}

void UUbstreamStageEditorMode::Enter()
{
	UEdMode::Enter();

	UEditorInteractiveToolsContext* ToolsContext = GetInteractiveToolsContext();
	if (!ToolsContext)
	{
		return;
	}

	ToolsContext->TargetManager->AddTargetFactory(NewObject<UStaticMeshComponentToolTargetFactory>(GetToolManager()));
	ToolsContext->TargetManager->AddTargetFactory(NewObject<UDynamicMeshComponentToolTargetFactory>(GetToolManager()));
	UE::TransformGizmoUtil::RegisterTransformGizmoContextObject(ToolsContext);

	GetToolManager()->RegisterToolType(PolyEditToolIdentifier, NewObject<UEditMeshPolygonsToolBuilder>(GetToolManager()));
	GetToolManager()->RegisterToolType(VertexSculptToolIdentifier, NewObject<UUbstreamVertexSculptToolBuilder>(GetToolManager()));
	GetToolManager()->RegisterToolType(SmoothToolIdentifier, NewObject<USmoothMeshToolBuilder>(GetToolManager()));
	GetToolManager()->RegisterToolType(OffsetToolIdentifier, NewObject<UOffsetMeshToolBuilder>(GetToolManager()));
	GetToolManager()->RegisterToolType(DisplaceToolIdentifier, NewObject<UDisplaceMeshToolBuilder>(GetToolManager()));
	GetToolManager()->RegisterToolType(LatticeToolIdentifier, NewObject<ULatticeDeformerToolBuilder>(GetToolManager()));
	GetToolManager()->RegisterToolType(RemeshToolIdentifier, NewObject<URemeshMeshToolBuilder>(GetToolManager()));
	GetToolManager()->RegisterToolType(CSGMeshesToolIdentifier, NewObject<UCSGMeshesToolBuilder>(GetToolManager()));
	UCSGMeshesToolBuilder* TrimMeshesToolBuilder = NewObject<UCSGMeshesToolBuilder>(GetToolManager());
	TrimMeshesToolBuilder->bTrimMode = true;
	GetToolManager()->RegisterToolType(TrimMeshesToolIdentifier, TrimMeshesToolBuilder);
	GetToolManager()->RegisterToolType(WeldToolIdentifier, NewObject<UWeldMeshEdgesToolBuilder>(GetToolManager()));
	GetToolManager()->RegisterToolType(HoleFillToolIdentifier, NewObject<UHoleFillToolBuilder>(GetToolManager()));
}

void UUbstreamStageEditorMode::Exit()
{
	EndActiveToolImmediate(false);
	if (UEditorInteractiveToolsContext* ToolsContext = GetInteractiveToolsContext())
	{
		UE::TransformGizmoUtil::DeregisterTransformGizmoContextObject(ToolsContext);
	}
	UEdMode::Exit();
}

void UUbstreamStageEditorMode::ModeTick(float DeltaTime)
{
	UEdMode::ModeTick(DeltaTime);
	if (bToolEndPending)
	{
		bToolEndPending = false;
		ShutdownCurrentTool(bAcceptPendingToolEnd);
	}
	if (!bToolStartPending)
	{
		return;
	}

	// Own the deferred request until this safe mode tick. A cancel before the
	// tick removes it completely; no orphan request remains in the editor context.
	UEditorInteractiveToolsContext* ToolsContext = GetInteractiveToolsContext();
	ShutdownCurrentTool(true);
	if (!bToolStartPending) // A shutdown callback may have cancelled the swap.
	{
		return;
	}
	const FString ToolIdentifier = ActiveToolStartIdentifier;
	if (!ToolsContext || !ToolsContext->CanStartTool(ToolIdentifier)
		|| !ToolsContext->UInteractiveToolsContext::StartTool(EToolSide::Mouse, ToolIdentifier))
	{
		bToolStartPending = false;
		ActiveToolStartIdentifier.Reset();
		OnToolStateChanged.Broadcast();
	}
}

void UUbstreamStageEditorMode::OnToolStarted(UInteractiveToolManager* Manager, UInteractiveTool* Tool)
{
	UEdMode::OnToolStarted(Manager, Tool);
	bToolStartPending = false;
	OnToolStateChanged.Broadcast();
}

void UUbstreamStageEditorMode::OnToolEnded(UInteractiveToolManager* Manager, UInteractiveTool* Tool)
{
	UEdMode::OnToolEnded(Manager, Tool);
	if (!bToolStartPending)
	{
		ActiveToolStartIdentifier.Reset();
	}
	OnToolStateChanged.Broadcast();
}

bool UUbstreamStageEditorMode::CanStartTool(const FString& ToolIdentifier)
{
	UEditorInteractiveToolsContext* ToolsContext = GetInteractiveToolsContext();
	return ToolsContext && ToolsContext->CanStartTool(ToolIdentifier);
}

bool UUbstreamStageEditorMode::StartTool(const FString& ToolIdentifier)
{
	UEditorInteractiveToolsContext* ToolsContext = GetInteractiveToolsContext();
	if (!ToolsContext || !ToolsContext->CanStartTool(ToolIdentifier))
	{
		return false;
	}

	// The next mode tick performs the swap through ITF, after input dispatch.
	// Keeping this request here lets EndActiveTool cancel it before construction.
	ActiveToolStartIdentifier = ToolIdentifier;
	bToolStartPending = true;
	bToolEndPending = false;
	return true;
}

bool UUbstreamStageEditorMode::HasActiveTool()
{
	UEditorInteractiveToolsContext* ToolsContext = GetInteractiveToolsContext();
	return (ToolsContext && ToolsContext->HasActiveTool()) || bToolStartPending;
}

FString UUbstreamStageEditorMode::GetActiveToolIdentifier()
{
	return HasActiveTool() ? ActiveToolStartIdentifier : FString();
}

void UUbstreamStageEditorMode::EndActiveTool(bool bAccept)
{
	bToolStartPending = false;
	bAcceptPendingToolEnd = bAccept;
	UEditorInteractiveToolsContext* ToolsContext = GetInteractiveToolsContext();
	bToolEndPending = ToolsContext && ToolsContext->HasActiveTool();
	if (!bToolEndPending)
	{
		ActiveToolStartIdentifier.Reset();
	}
}

void UUbstreamStageEditorMode::ShutdownCurrentTool(bool bAccept)
{
	UEditorInteractiveToolsContext* ToolsContext = GetInteractiveToolsContext();
	if (ToolsContext && ToolsContext->UInteractiveToolsContext::HasActiveTool(EToolSide::Mouse))
	{
		const EToolShutdownType ShutdownType = bAccept && ToolsContext->CanAcceptActiveTool()
			? EToolShutdownType::Accept
			: bAccept ? EToolShutdownType::Completed : EToolShutdownType::Cancel;
		ToolsContext->UInteractiveToolsContext::EndTool(EToolSide::Mouse, ShutdownType);
	}
}

void UUbstreamStageEditorMode::EndActiveToolImmediate(bool bAccept)
{
	bToolStartPending = false;
	bToolEndPending = false;
	ShutdownCurrentTool(bAccept);
	ActiveToolStartIdentifier.Reset();
}

bool UUbstreamStageEditorMode::CanAcceptActiveTool()
{
	UEditorInteractiveToolsContext* ToolsContext = GetInteractiveToolsContext();
	return ToolsContext && ToolsContext->CanAcceptActiveTool();
}

UInteractiveTool* UUbstreamStageEditorMode::GetActiveToolInstance()
{
	UEditorInteractiveToolsContext* ToolsContext = GetInteractiveToolsContext();
	return ToolsContext && ToolsContext->ToolManager
		? ToolsContext->ToolManager->GetActiveTool(EToolSide::Mouse)
		: nullptr;
}

UEditorInteractiveToolsContext* UUbstreamStageEditorMode::GetStageToolsContext()
{
	return GetInteractiveToolsContext();
}

#undef LOCTEXT_NAMESPACE
