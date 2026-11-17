#pragma once

#include "CoreMinimal.h"
#include "Capture/UbstreamCaptureTargetPool.h"
#include "Capture/UbstreamEditorWorldCaptureService.h"
#include "Components/SceneCaptureComponent2D.h"
#include "SEditorViewport.h"
#include "UObject/StrongObjectPtr.h"

class AActor;
class FAdvancedPreviewScene;
class FAssetEditorModeManager;
class FUbstreamCSGSession;
class FUbstreamRemeshJob;
class FUbstreamStageToolkitHost;
class FUbstreamViewportClient;
class UActorComponent;
class UCombinedTransformGizmo;
class UEditorInteractiveToolsContext;
class UDynamicMeshComponent;
class UInteractiveTool;
class UInteractiveToolManager;
class UMeshComponent;
class UMeshVertexSculptTool;
class UMeshSculptBrushOpProps;
class UPrimitiveComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UTransformProxy;
class UUbstreamStageEditorMode;
class UUbstreamVertexSculptTool;
class FDragDropEvent;
struct FUbstreamCSGResult;
namespace UE::Geometry { class FDynamicMesh3; }

/** User lighting preferences, independent of mesh edits and asset-viewer profiles. */
struct FUbstreamStageLighting
{
	// Inspect is the default: an offset camera light with generous ambient fill.
	float AmbientFill = 1.3f;
	float MainStrength = 0.7f;
	float Rotation = 25.0f;
	float Elevation = -20.0f;
	bool bFollowCamera = true;
	// Linear tints. White keeps the neutral look the stage has always had.
	FLinearColor MainColor = FLinearColor::White;
	FLinearColor AmbientColor = FLinearColor::White;
};

enum class EUbstreamStageCameraView : uint8
{
	Iso,
	Front,
	Right,
	Top
};

enum class EUbstreamCSGPrimitiveType : uint8
{
	Box,
	Cylinder,
	Sphere
};

enum class EUbstreamCSGTransformMode : uint8
{
	Move,
	Rotate,
	Scale
};

enum class EUbstreamCSGOperation : uint8
{
	Subtract,
	Union,
	Intersect
};

/**
 * Surface-sculpting brushes we surface in our own panel. Each maps to one of
 * Epic's UMeshVertexSculptTool brush modes; we deliberately expose only the
 * intuitive core set (the tool ships many more) to keep the controls focused.
 */
enum class EUbstreamSculptBrush : uint8
{
	/** Move vertices parallel to the view plane (grab). */
	Move,
	/** Push/pull along the surface normal (Ctrl inverts). */
	Draw,
	/** Relax the surface. */
	Smooth,
	/** Expand along vertex normals (Ctrl deflates). */
	Inflate,
	/** Pull toward the average plane of the brush region. */
	Flatten,
	/** Pull vertices toward the brush centre (Ctrl pushes out). */
	Pinch,
	/** Build up to a bounded height along the normal - classic clay. */
	Clay,
	/** Shave high spots toward a plane at the stroke start (Ctrl inverts). */
	Scrape,
	/** Soft volumetric grab with a Kelvinlet falloff. */
	Grab
};

DECLARE_DELEGATE_RetVal_OneParam(bool, FUbstreamConfirmDiscardStageEdits, const FText&);

class SUbstreamModelerViewport final : public SEditorViewport
{
public:
	SLATE_BEGIN_ARGS(SUbstreamModelerViewport) {}
		SLATE_EVENT(FSimpleDelegate, OnSoloMeshChanged)
		SLATE_EVENT(FSimpleDelegate, OnEditingStateChanged)
		SLATE_EVENT(FUbstreamConfirmDiscardStageEdits, OnConfirmDiscardStageEdits)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	~SUbstreamModelerViewport() override;
	// SEditorViewport otherwise treats a never-ticked viewport as visible.
	virtual bool IsVisible() const override { return GetVisibility().IsVisible() && SEditorViewport::IsVisible(); }

	const FUbstreamStageLighting& GetStageLighting() const { return StageLighting; }
	void SetStageLighting(const FUbstreamStageLighting& Settings);
	void SetLightFollowsCamera(bool bFollow);
	void SaveStageLighting() const;
	/** Called after camera movement and before capture; unchanged lights do no render work. */
	void UpdateStageLighting();

	bool EnterEditMode();
	void ExitEditMode();
	bool IsEditModeActive() const { return bEditModeActive; }
	bool HasActiveEditTool() const;
	/** True when a tool is running in EITHER the custom fallback mode or the
	    hosted built-in Modeling Mode; mesh deformation only happens via tools. */
	bool HasAnyActiveStageTool() const;
	/** Keeps the visible stage from drawing both the base component and a tool's
	    temporary working mesh at the same time. */
	void RefreshToolPreviewVisibility();
	bool CanStartEditTool(const FString& ToolIdentifier) const;
	bool StartEditTool(const FString& ToolIdentifier);
	/**
	 * Live CSG between the single source mesh and the resolved cutter, computed
	 * off-thread by FUbstreamCSGSession (Unreal's boolean kernel + repair pass).
	 * The subject hides behind a transient preview component that follows every
	 * cutter drag; Commit writes the result back under one transaction, Cancel
	 * just discards the preview. Starting a different operation on the same
	 * cutter retargets the running session instead of restarting it.
	 */
	bool CanStartCSGOperation() const;
	bool StartCSGOperation(EUbstreamCSGOperation Operation);
	/** The selected cutter's operation (Subtract when nothing is selected). */
	EUbstreamCSGOperation GetSelectedCSGOperation() const;
	EUbstreamCSGOperation GetCSGPrimitiveOperation(const UMeshComponent* Component) const;
	void SetCSGPrimitiveOperation(UMeshComponent* Component, EUbstreamCSGOperation Operation);
	/** Selected cutter's transform decomposed for numeric editing; size is
	    the largest world-space dimension. */
	bool GetSelectedCSGPrimitiveTransform(FVector& OutLocation, FRotator& OutRotation, double& OutSize) const;
	bool SetSelectedCSGPrimitiveLocation(const FVector& NewLocation);
	bool SetSelectedCSGPrimitiveRotation(const FRotator& NewRotation);
	bool SetSelectedCSGPrimitiveSize(double NewSize);
	/** Uniform component scale (max of XYZ). Used by the properties Scale slider. */
	bool GetSelectedCSGPrimitiveUniformScale(double& OutScale) const;
	bool SetSelectedCSGPrimitiveUniformScale(double NewScale);
	/** Oriented per-axis dimensions (the cutter's own width/depth/height). */
	bool GetSelectedCSGPrimitiveDimensions(FVector& OutDimensions) const;
	bool SetSelectedCSGPrimitiveDimension(int32 Axis, double NewSize);
	/** Cutters in stack order with short display names ("Box 1"). */
	void GetCSGPrimitiveDisplayList(TArray<TPair<UMeshComponent*, FString>>& OutCutters) const;
	/** Gizmo snapping: 0 disables; rotation snaps to 15 degrees when enabled. */
	void SetCSGSnapIncrement(double Increment);
	double GetCSGSnapIncrement() const { return CSGSnapIncrement; }
	bool HasActiveCSGSession() const;
	bool IsCSGSessionComputing() const;
	bool CanCommitCSGSession() const;
	void CommitCSGSession();
	void CancelCSGSession();
	/** Drives the async session recompute; called from the viewport client tick. */
	void TickCSGSession();
	/** Stage ITF context access for interactivity automation. */
	UEditorInteractiveToolsContext* GetStageToolsContextForTesting() const;
	/** How many gizmo-driven transform changes have landed (automation probe). */
	int32 GetGizmoTransformChangeCountForTesting() const { return GizmoTransformChangeCount; }
	/** Live session access for automation (chain-cache assertions). */
	FUbstreamCSGSession* GetCSGSessionForTesting() const { return CSGSession.Get(); }
	/** User-facing session state: kernel errors or open-boundary warnings. */
	const FString& GetCSGSessionMessage() const { return CSGSessionMessage; }
	bool CanAcceptActiveStageTool() const;
	void EndActiveEditTool(bool bAccept);
	FString GetActiveEditToolIdentifier() const;
	UInteractiveTool* GetActiveEditToolInstance() const;
	bool HasUncommittedStageEdits() const { return bHasUncommittedStageEdits || GetCSGPrimitiveCount() > 0; }
	bool ApplyEditsToSource(FString& OutErrorMessage);
	/** UI boundary: only prompt when replacing unapplied stage work. */
	bool ConfirmDiscardStageEdits(const FText& Prompt) const;
	bool ConfirmReplaceSubject() const;

	/**
	 * Surface sculpting, layered on the same single base mesh the CSG cutters
	 * operate on. Wraps Epic's UMeshVertexSculptTool but is driven entirely
	 * through these methods (our panel is the palette - no ITF toolkit UI).
	 * Starting a brush first bakes any committable live CSG cut so sculpt and
	 * the cutter stack never fight over the subject.
	 */
	bool CanSculpt() const;
	bool IsSculptActive() const;
	/** Begin (or, if already sculpting, switch to) the given brush. */
	bool StartSculpt(EUbstreamSculptBrush Brush);
	/** Finish sculpting, baking the deformation into the base mesh (accept) or
	    discarding the stroke session (cancel). */
	void StopSculpt(bool bAccept);
	/** While sculpting these read the LIVE tool (so the tool's own hotkeys -
	    Shift smooth, [ ]/S/D size, W/E strength - keep the panel in sync);
	    otherwise they return the cached value the next session will start from. */
	EUbstreamSculptBrush GetSculptBrush() const;
	void SetSculptBrushSize(float Normalized);
	float GetSculptBrushSize() const;
	void SetSculptBrushStrength(float Normalized);
	float GetSculptBrushStrength() const;
	void SetSculptBrushFalloff(float Normalized);
	float GetSculptBrushFalloff() const;
	/** Symmetry is only offerable once the tool detects a symmetric base mesh. */
	bool CanEnableSculptSymmetry() const;
	void SetSculptSymmetryEnabled(bool bEnabled);
	bool IsSculptSymmetryEnabled() const { return bSculptSymmetryEnabled; }

	/**
	 * Surface detail: remesh the single base mesh to a target triangle budget so
	 * the sculptor has enough vertices to carve fine detail (the vertex sculptor
	 * only moves existing verts - resolution is the ceiling on detail). Uses the
	 * geometry toolset's remesher, reprojecting onto the original to hold the
	 * shape. Accepts active sculpting, preserves and reapplies live cutters,
	 * and records an undoable mesh replacement.
	 */
	bool CanRemeshSubject() const;
	int32 GetSubjectTriangleCount() const;
	int32 GetSculptTargetTriangleCount() const { return SculptTargetTriangleCount; }
	void SetSculptTargetTriangleCount(int32 TargetTriangleCount);
	static int32 GetSculptTargetTriangleMin() { return 500; }
	static int32 GetSculptTargetTriangleMax() { return 200000; }
	/** Starts background work; true means accepted, not completed. */
	bool RemeshSubjectToTargetDetail(FString& OutErrorMessage);
	bool IsRemeshing() const { return RemeshJob.IsValid(); }
	void CancelRemesh();
	const FString& GetRemeshStatus() const { return RemeshStatus; }
	FSimpleMulticastDelegate OnRemeshStateChanged;

	TSharedRef<FAdvancedPreviewScene> GetPreviewScene() const;
	TSharedPtr<FUbstreamViewportClient> GetUbstreamViewportClient() const;
	bool SetStaticMesh(UStaticMesh* StaticMesh, bool bFocusCamera = true);
	bool SetMeshComponents(
		const TArray<UMeshComponent*>& Components,
		FString SubjectLabel,
		FString SubjectPath,
		TArray<TWeakObjectPtr<AActor>> InSourceActors = TArray<TWeakObjectPtr<AActor>>(),
		bool bFocusCamera = true);
	bool SetActors(const TArray<TWeakObjectPtr<AActor>>& Actors, bool bFocusCamera = true);
	bool FocusSoloMesh();
	bool SetCameraView(EUbstreamStageCameraView CameraView);
	bool ResyncFromSource(bool bPreserveCamera = false);
	bool CanResyncFromSource() const;
	bool HasEditableSource() const;
	bool HasSoloMesh() const;
	bool IsSourceDirty() const;
	UStaticMesh* GetPrimaryStaticMesh() const;
	TArray<TWeakObjectPtr<AActor>> GetSourceActors() const;
	TArray<TWeakObjectPtr<UMeshComponent>> GetSourceComponents() const;
	const TArray<TObjectPtr<UMeshComponent>>& GetPreviewComponents() const { return PreviewComponents; }
	FString GetSoloMeshLabel() const;
	FString GetSoloMeshPath() const;
	FString GetSourceModeLabel() const;
	int32 GetPreviewComponentCount() const;
	int32 GetSourcePreviewComponentCount() const;
	int32 GetSourceActorCount() const;
	int32 GetSourceComponentCount() const;
	uint64 BuildSceneSignature() const;
	uint64 BuildSourceSignature() const;
	void AddBox();
	bool CanAddCSGPrimitive() const;
	bool AddCSGPrimitive(
		EUbstreamCSGPrimitiveType PrimitiveType,
		const TOptional<FVector>& WorldLocation = TOptional<FVector>(),
		const TOptional<double>& HalfSizeOverride = TOptional<double>());
	/** Arm click-to-place: the next click on the model spawns this cutter there. */
	void ArmCSGPlacement(EUbstreamCSGPrimitiveType PrimitiveType);
	void CancelCSGPlacement();
	bool HasPendingCSGPlacement() const;
	/** Places the armed cutter at the ray's hit point on the model and starts a live subtract. */
	bool PlacePendingCSGPrimitiveAtRay(const FVector& RayOrigin, const FVector& RayDirection);
	/** Moves the live session's cutter to the ray's hit point on the model. */
	bool RepositionSessionCutterAtRay(const FVector& RayOrigin, const FVector& RayDirection);
	bool RaycastSourcePreview(const FVector& RayOrigin, const FVector& RayDirection, FVector& OutHitPoint) const;
	/** Nearest cutter whose mesh the ray hits, or null. Analytic (mesh ray
	    test), NOT hit-proxy based: session cutter bodies render invisible and
	    translucent surfaces drop out of the hit-proxy pass, so picking must
	    not depend on rendered pixels. OutRayParameter is world units. */
	UMeshComponent* FindCSGPrimitiveUnderRay(const FVector& RayOrigin, const FVector& RayDirection, double& OutRayParameter) const;
	/** Clones the selected cutter with a sideways offset and starts a live subtract. */
	bool DuplicateSelectedCSGPrimitive();
	/** Re-arms placement with the last cutter type/size used. */
	bool RepeatLastCSGPrimitive();
	/** Selected single cutter is a Subtract or Intersect: acceptable alone. */
	bool CanAcceptSelectedCutter() const;
	/** Bakes ONLY the selected cutter's cut (subtract/intersect) into the
	    base mesh and consumes that cutter; other cutters stay live on the
	    new base. Undoable. */
	bool AcceptSelectedCutter();
	void SetHoveredCSGPrimitiveComponent(UMeshComponent* Component);
	/** Clears the stage selection and drops the gizmo (outliner model row,
	    empty-viewport clicks). */
	void DeselectAllStageComponentsForUI();
	/** Destroys any transform gizmo actor in the stage world the stage did
	    not create (editor subsystems can spawn dead duplicates). */
	void DestroyForeignGizmoActors();
	/** Show/hide the stage ITF CombinedTransformGizmo (used while legacy scale widget is active). */
	void SetStageTransformGizmoVisible(bool bVisible);
	bool ClearCSGPrimitives();
	bool DeleteSelectedCSGPrimitives();
	int32 GetCSGPrimitiveCount() const;
	bool HasSelectedCSGPrimitive() const;
	int32 GetSelectedCSGPrimitiveIndex() const;
	void GetCSGPrimitiveComponents(TArray<UMeshComponent*>& OutComponents) const;
	void GetSelectedCSGPrimitiveComponents(TArray<UMeshComponent*>& OutComponents) const;
	bool SelectCSGPrimitiveForEditing(const UPrimitiveComponent* PrimitiveComponent, bool bAddToSelection = false);
	bool SelectLatestCSGPrimitiveForEditing();
	bool SelectNextCSGPrimitiveForEditing(int32 Direction = 1);
	bool SetCSGTransformMode(EUbstreamCSGTransformMode TransformMode);
	EUbstreamCSGTransformMode GetCSGTransformMode() const { return CSGTransformMode; }
	bool GetCSGTransformWidgetLocation(FVector& OutLocation) const;
	bool ApplyCSGTransformDelta(const FVector& Drag, const FRotator& Rot, const FVector& Scale);
	bool SelectStageComponentForEditing(const UPrimitiveComponent* PrimitiveComponent, bool bAddToSelection = false);
	void ClearObjects();
	FString BuildSceneStateJson(const FString& SessionId) const;
	FString CaptureRgbFrame();
	FUbstreamEditorWorldCaptureResult CaptureFrameBundle(uint64 FrameId, const FUbstreamCaptureSettings& Settings);
	/** Game-thread capture phase only (render + readback, no JPEG encode), using
	    the persistent reused capture component. The caller encodes off-thread via
	    FUbstreamCaptureService::EncodeCapturedFrame. */
	FUbstreamCapturePixels CaptureFramePixels(uint64 FrameId, const FUbstreamCaptureSettings& Settings);
	/** Async variant: renders + enqueues GPU readback without stalling the game
	    thread. Returns null when the async path is unavailable (no persistent
	    capture component/pool yet) - fall back to CaptureFramePixels. */
	TSharedPtr<class FUbstreamAsyncFrameReadback, ESPMode::ThreadSafe> BeginAsyncCaptureFramePixels(
		uint64 FrameId, const FUbstreamCaptureSettings& Settings);
	bool IsCaptureCameraSettling(double SettleSeconds) const;

	virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual void OnFocusViewportToSelection() override;

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;

private:
	friend class FUbstreamStageLightingTest;
	friend class FUbstreamStageEditingAutomationTest;
	friend class FUbstreamSculptToolsAutomationTest;
	FUbstreamConfirmDiscardStageEdits OnConfirmDiscardStageEdits;
	UStaticMesh* ResolveDraggedStaticMesh(const FDragDropEvent& DragDropEvent) const;
	TArray<TWeakObjectPtr<AActor>> ResolveDraggedActors(const FDragDropEvent& DragDropEvent) const;
	TArray<UMeshComponent*> ResolveDraggedMeshComponents(const FDragDropEvent& DragDropEvent) const;
	TArray<FString> ResolveDraggedExternalMeshFiles(const FDragDropEvent& DragDropEvent) const;
	bool ImportExternalMeshFiles(const TArray<FString>& Files);
	void SetAcceptedDropFeedback(const FDragDropEvent& DragDropEvent) const;
	void ClearObjectsInternal(bool bNotify);
	/** Realtime while a subject is loaded (or edit mode is active); off when empty. */
	void UpdateStageRealtime();
	void FocusCameraOnComponent(const UMeshComponent& Component);
	UUbstreamStageEditorMode* GetStageEditorMode() const;
	bool ConvertStaticPreviewsToEditable();
	bool SelectPreviewComponentsForEditing();
	void DeselectAllStageComponents();
	bool IsStageComponentSelected(const UPrimitiveComponent* Component) const;
	void PushStageSelectionToProxies();
	bool SelectPreviewComponentsForTool(const FString& ToolIdentifier);
	void AppendSourcePreviewComponents(TArray<UMeshComponent*>& OutComponents) const;
	void AppendCSGPrimitiveComponents(TArray<UMeshComponent*>& OutComponents) const;
	int32 AdoptUntrackedPreviewWorldMeshComponents();
	/** All staged components live on a transient actor in the preview world so
	    tools that assume component->GetOwner() (PlaneCut, Mirror, ...) work. */
	AActor* EnsureStageActor();
	void AttachPreviewComponentToStage(UMeshComponent* Component, const FTransform& WorldTransform);
	/** The single editable source component the CSG session operates on. */
	UDynamicMeshComponent* ResolveCSGSessionSubject() const;
	int32 FindCSGSessionBrushIndex(const UMeshComponent* Component) const;
	void ApplySessionGhostMaterials(const TArray<UDynamicMeshComponent*>& StackCutters);
	void PushSessionBrushTransforms();
	void ApplyCSGSnapToGizmo(class UCombinedTransformGizmo& Gizmo) const;
	FTransform GetStageGizmoAnchorTransform(UMeshComponent* Target) const;
	void HandleCSGPreviewReady(const FUbstreamCSGResult& Result);
	/** Tears down preview component + ghost brush styling; End()s the session. */
	void EndCSGSessionInternal();
	/**
	 * Keeps Unreal's Modeling Mode transform gizmo (UCombinedTransformGizmo)
	 * attached to the current selection - the CSG cutter when one is selected,
	 * otherwise the selected stage components. The gizmo follows the mode
	 * manager's W/E/R widget mode and emits undoable drag transactions.
	 */
	void UpdateStageTransformGizmo();
	void DestroyStageTransformGizmo();
	void HandleGizmoTransformChanged(UTransformProxy* Proxy, FTransform Transform);
	void FocusStageViewport();
	void SetStagePreviewComponentsSuppressed(bool bSuppressed);
	bool EnsureEngineAssetSourcesAreEditable(FString& OutErrorMessage);
	void HandleModeToolStateChanged();
	/** The live vertex sculpt tool instance, or null when it is not the active
	    stage tool (also null during the deferred-start grace window). */
	UMeshVertexSculptTool* GetActiveVertexSculptTool() const;
	/** The active brush's op property set (holds per-brush Strength). */
	UMeshSculptBrushOpProps* GetActiveSculptBrushOpProps() const;
	/** Push the cached brush + size/strength/falloff/symmetry onto the live tool. */
	void ApplySculptSettingsToActiveTool();
	/** Synchronously finish (bake) an active sculpt so the cutter workflow can
	    take over - used when a CSG shape is placed mid-sculpt. */
	void CommitActiveSculptImmediate();
	/** The vertex sculpt tool creates its preview mesh with no RF_Transactional
	    flag, which crashes the editor transactor when it records the per-stroke
	    undo in this stage. Flag the tool's preview objects transactional (like
	    our subject, whose undo works) once the tool is live. */
	void MakeActiveSculptToolTransactional();
	/** Show/hide every cutter, so sculpt/remesh work on a clean base while the
	    cutters wait, editable, to be re-applied afterwards. */
	void SetCSGPrimitiveComponentsHidden(bool bHidden);
	void HandleHostedToolkitChanged();
	void HandleHostOverlayAdded(const TSharedRef<SWidget>& OverlayWidget);
	void HandleHostOverlayRemoved(const TSharedRef<SWidget>& OverlayWidget);
	/** Whole-scene capture is needed while tools render through temporary preview geometry. */
	bool ShouldCaptureWholeScene() const;
	UInteractiveTool* GetAnyActiveStageToolInstance() const;
	void ApplyActiveToolDisplayPolicy();
	/** All real mesh components in the stage world (incl. tool working meshes),
	    excluding gizmos and line/point/triangle tool visualization. */
	void AppendEditSessionMeshComponents(TArray<UPrimitiveComponent*>& OutComponents) const;
	/** During edits the stream renders from a private mirror world containing
	    only copies of the real geometry, so gizmos and tool overlays can never
	    leak into the captured frames. */
	void SyncCaptureMirrorScene();
	void ResetCaptureMirrorScene();
	AActor* EnsureCaptureMirrorActor();
	/** Lazily creates the persistent capture component and (re-)registers it in
	    the requested scene, so streaming captures reuse one component instead of
	    creating/destroying one every frame. Re-registration only happens on
	    preview<->mirror transitions, not per frame. */
	USceneCaptureComponent2D* EnsureCaptureComponent(const TSharedPtr<FAdvancedPreviewScene>& Scene);
	/** Async depth pass needs its own component: two same-frame captures on
	    one component race each other's target/source state. */
	USceneCaptureComponent2D* EnsureDepthCaptureComponent(const TSharedPtr<FAdvancedPreviewScene>& Scene);
	void ReleaseCaptureComponent();
	void HandlePostUndoRedo();
	bool bHandlingUndoRedo = false;
	void ReconcileCuttersAfterUndoRedo();
	void StoreCutterMeshForUndo(UDynamicMeshComponent* Cutter);
	void RefreshCSGPrimitiveVisuals();
	void RemoveCSGPrimitiveComponents(bool bNotify);
	UMeshComponent* ResolveCSGPrimitiveForTool() const;
	/** Rendered camera pose (orbit-aware) plus FOV/aspect/near-clip for stage captures. */
	FUbstreamCaptureCamera BuildCaptureCamera(int32 Width, int32 Height) const;

	enum class ESourceKind : uint8
	{
		None,
		StaticMeshAsset,
		ActorSelection,
		ComponentSelection,
		GeneratedPreview
	};

	FUbstreamStageLighting StageLighting;
	void InitializeSceneLighting(FAdvancedPreviewScene& Scene);
	void ApplySceneLighting(FAdvancedPreviewScene& Scene, const FRotator& Direction);
	TSharedPtr<FAdvancedPreviewScene> PreviewScene;
	TSharedPtr<FAssetEditorModeManager> StageModeManager;
	TSharedPtr<FUbstreamStageToolkitHost> StageToolkitHost;
	TSharedPtr<FUbstreamViewportClient> ViewportClient;
	mutable FUbstreamCaptureTargetPool CaptureTargetPool;
	FSimpleDelegate OnSoloMeshChanged;
	FSimpleDelegate OnEditingStateChanged;
	FDelegateHandle ToolStateChangedHandle;
	/** Owns the off-thread boolean recompute; created on first StartCSGOperation. */
	TSharedPtr<FUbstreamCSGSession, ESPMode::ThreadSafe> CSGSession;
	/** Source component the active session will commit into (hidden meanwhile). */
	TWeakObjectPtr<UDynamicMeshComponent> CSGSessionSubject;
	/** Ordered cutter stack driving the active session (index-matched with
	    the session's brushes); all render as translucent ghosts. */
	TArray<TWeakObjectPtr<UMeshComponent>> CSGSessionBrushes;
	/** Pre-session subject mesh: restored on cancel, Before-state on commit. */
	TSharedPtr<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> CSGSessionOriginalMesh;
	/** Fingerprint of the mesh the session last wrote into the subject;
	    a mismatch means undo/redo rewrote it from outside. */
	uint64 CSGSessionSubjectFingerprint = 0;
	/** Open edges the subject already had, so warnings cover only new ones. */
	int32 CSGSessionSubjectOpenBoundaryEdges = 0;
	FString CSGSessionMessage;
	/** Epic's Modeling Mode gizmo, spawned on the stage tools context. */
	TWeakObjectPtr<UCombinedTransformGizmo> StageTransformGizmo;
	TWeakObjectPtr<UTransformProxy> StageTransformGizmoProxy;
	/** Components the live gizmo currently drives; rebuilt on selection change. */
	TArray<TWeakObjectPtr<UMeshComponent>> StageTransformGizmoTargets;
	int32 GizmoTransformChangeCount = 0;
	EUbstreamCSGOperation PendingCSGOperation = EUbstreamCSGOperation::Subtract;
	EUbstreamCSGTransformMode CSGTransformMode = EUbstreamCSGTransformMode::Move;
	/** Sculpt brush + normalised controls; the panel's source of truth, pushed
	    onto the live tool whenever it is active (so they persist across strokes
	    and brush switches). */
	EUbstreamSculptBrush SculptBrush = EUbstreamSculptBrush::Draw;
	float SculptBrushSize = 0.25f;
	float SculptBrushStrength = 0.5f;
	float SculptBrushFalloff = 0.5f;
	bool bSculptSymmetryEnabled = false;
	/** Target triangle budget for the surface-detail remesh; panel-persistent. */
	int32 SculptTargetTriangleCount = 20000;
	TSharedPtr<FUbstreamRemeshJob, ESPMode::ThreadSafe> RemeshJob;
	TWeakObjectPtr<UDynamicMeshComponent> RemeshSubject;
	FDelegateHandle RemeshMeshChangedHandle;
	FString RemeshStatus;
	void AbandonRemesh();
	void FinishRemesh(const TSharedRef<FUbstreamRemeshJob, ESPMode::ThreadSafe>& Job, bool bHadCutters);
	bool bEditModeActive = false;
	bool bHasUncommittedStageEdits = false;
	bool bStagePreviewComponentsSuppressed = false;
	TArray<TObjectPtr<UMeshComponent>> PreviewComponents;
	TWeakObjectPtr<AActor> StageActor;

	struct FUbstreamCaptureMirror
	{
		TWeakObjectPtr<UMeshComponent> Source;
		TObjectPtr<UMeshComponent> Mirror;
		uint64 MeshFingerprint = 0;
		TWeakObjectPtr<UUbstreamVertexSculptTool> RevisionTool;
		uint64 SculptRevision = 0;
	};
	TSharedPtr<FAdvancedPreviewScene> CaptureMirrorScene;
	TWeakObjectPtr<AActor> CaptureMirrorActor;
	TArray<FUbstreamCaptureMirror> CaptureMirrors;
	bool bCaptureMirrorRefreshPending = false;
	/** Persistent scene-capture component reused across frames, plus the scene it
	    is currently registered in (preview or mirror). TStrongObjectPtr is the
	    GC anchor (this is a Slate widget, not a UObject). */
	TStrongObjectPtr<USceneCaptureComponent2D> CaptureComponent;
	TStrongObjectPtr<USceneCaptureComponent2D> DepthCaptureComponent;
	TWeakPtr<FAdvancedPreviewScene> DepthCaptureComponentScene;
	TWeakPtr<FAdvancedPreviewScene> CaptureComponentScene;
	FDelegateHandle PostUndoRedoHandle;
	TWeakObjectPtr<UStaticMesh> SoloMesh;
	TArray<TWeakObjectPtr<AActor>> SourceActors;
	TArray<TWeakObjectPtr<UMeshComponent>> SourceComponents;
	FString SoloMeshLabel;
	FString SoloMeshPath;
	ESourceKind SourceKind = ESourceKind::None;
	uint64 SourceSignatureAtLoad = 0;
	mutable double LastSourceDirtyCheckSeconds = -1.0;
	mutable uint64 LastSourceDirtyCheckBaseline = 0;
	mutable bool bCachedSourceDirty = false;
	mutable FVector LastCaptureCameraLocation = FVector::ZeroVector;
	mutable FRotator LastCaptureCameraRotation = FRotator::ZeroRotator;
	mutable float LastCaptureCameraFov = 0.0f;
	mutable double LastCaptureCameraChangeTimeSeconds = -1.0;
	mutable bool bHasLastCaptureCamera = false;
	int32 NextBoxIndex = 1;
	TOptional<EUbstreamCSGPrimitiveType> PendingCSGPlacementType;
	TOptional<EUbstreamCSGPrimitiveType> LastCSGPrimitiveType;
	double LastCSGPrimitiveHalfSize = 0.0;
	TWeakObjectPtr<UMeshComponent> HoveredCSGPrimitive;
	TWeakObjectPtr<UMaterialInterface> LastSessionGhostMaterial;
	double CSGSnapIncrement = 0.0;
};
