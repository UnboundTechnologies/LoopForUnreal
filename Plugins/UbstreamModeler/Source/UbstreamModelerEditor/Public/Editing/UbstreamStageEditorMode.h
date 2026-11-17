#pragma once

#include "CoreMinimal.h"
#include "Tools/UEdMode.h"

#include "UbstreamStageEditorMode.generated.h"

class UEditorInteractiveToolsContext;
class UInteractiveTool;

/**
 * Hosts Unreal's Interactive Tools Framework mesh-editing tools inside the
 * Unbound Loop isolated stage viewport. Runs on the stage's own
 * FAssetEditorModeManager, fully independent of the Level Editor.
 *
 * The stage panel acts as the tool palette, so this mode does not create a
 * toolkit; tools are started and finished through the wrapper methods below.
 */
UCLASS(Transient)
class UUbstreamStageEditorMode : public UEdMode
{
	GENERATED_BODY()

public:
	static const FEditorModeID EM_UbstreamStageEditorModeId;
	static const FString PolyEditToolIdentifier;
	static const FString VertexSculptToolIdentifier;
	static const FString SmoothToolIdentifier;
	static const FString OffsetToolIdentifier;
	static const FString DisplaceToolIdentifier;
	static const FString LatticeToolIdentifier;
	static const FString RemeshToolIdentifier;
	static const FString CSGMeshesToolIdentifier;
	static const FString TrimMeshesToolIdentifier;
	static const FString WeldToolIdentifier;
	static const FString HoleFillToolIdentifier;

	UUbstreamStageEditorMode();

	virtual void Enter() override;
	virtual void Exit() override;
	virtual void ModeTick(float DeltaTime) override;
	virtual bool UsesToolkits() const override { return false; }
	virtual void OnToolStarted(UInteractiveToolManager* Manager, UInteractiveTool* Tool) override;
	virtual void OnToolEnded(UInteractiveToolManager* Manager, UInteractiveTool* Tool) override;

	bool CanStartTool(const FString& ToolIdentifier);
	bool StartTool(const FString& ToolIdentifier);
	bool HasActiveTool();
	FString GetActiveToolIdentifier();
	void EndActiveTool(bool bAccept);
	/** Ends the active tool synchronously, bypassing the deferred shutdown path. */
	void EndActiveToolImmediate(bool bAccept);
	bool CanAcceptActiveTool();
	UInteractiveTool* GetActiveToolInstance();
	UEditorInteractiveToolsContext* GetStageToolsContext();

	/** Fires when the active tool actually starts, ends, or a pending start expires. */
	FSimpleMulticastDelegate OnToolStateChanged;

private:
	void ShutdownCurrentTool(bool bAccept);
	// Keep deferred starts cancellable until the next mode tick.
	FString ActiveToolStartIdentifier;
	bool bToolStartPending = false;
	bool bToolEndPending = false;
	bool bAcceptPendingToolEnd = false;
};
