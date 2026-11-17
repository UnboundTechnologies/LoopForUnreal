#pragma once

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"
#include "EditorViewportClient.h"

class FAdvancedPreviewScene;
class FEditorModeTools;
class FScopedTransaction;
class SEditorViewport;
class SUbstreamModelerViewport;

class FUbstreamViewportClient final : public FEditorViewportClient
{
public:
	enum class EOverlay : uint8
	{
		Grid,
		Bounds,
		Normals,
		Tangents,
		Binormals,
		Vertices
	};

	FUbstreamViewportClient(
		FEditorModeTools* InModeTools,
		const TSharedRef<FAdvancedPreviewScene>& InPreviewScene,
		const TSharedRef<SEditorViewport>& InViewport);
	~FUbstreamViewportClient() override;

	void SetStageViewport(const TWeakPtr<SUbstreamModelerViewport>& InStageViewport);
	void FocusOnBounds(const FBoxSphereBounds& Bounds);
	void FocusOnBounds(const FBoxSphereBounds& Bounds, const FRotator& FocusRotation);
	void SetInspectionViewMode(EViewModeIndex ViewMode);
	EViewModeIndex GetInspectionViewMode() const;
	void ToggleOverlay(EOverlay Overlay);
	bool IsOverlayEnabled(EOverlay Overlay) const;
	void SetOrbitCameraEnabled(bool bEnabled);
	bool IsOrbitCameraEnabled() const;

	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;
	// UE 5.8 inserted an EGesturePhase parameter into InputGesture.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	virtual bool InputGesture(
		FViewport* InViewport,
		const FInputDeviceId DeviceId,
		EGestureEvent GestureType,
		const FVector2D& GestureDelta,
		bool bIsDirectionInvertedFromDevice,
		const uint64 Timestamp) override;
#else
	virtual bool InputGesture(
		FViewport* InViewport,
		const FInputDeviceId DeviceId,
		EGestureEvent GestureType,
		EGesturePhase GesturePhase,
		const FVector2D& GestureDelta,
		bool bIsDirectionInvertedFromDevice,
		const uint64 Timestamp) override;
#endif
	virtual void MouseMove(FViewport* InViewport, int32 X, int32 Y) override;
	virtual void CapturedMouseMove(FViewport* InViewport, int32 InMouseX, int32 InMouseY) override;
	virtual void Draw(FViewport* InViewport, FCanvas* Canvas) override;
	virtual void MouseLeave(FViewport* InViewport) override;
	virtual EMouseCursor::Type GetCursor(FViewport* InViewport, int32 X, int32 Y) override;
	virtual void ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY) override;
	virtual UE::Widget::EWidgetMode GetWidgetMode() const override;
	virtual void SetWidgetMode(UE::Widget::EWidgetMode NewMode) override;
	virtual bool CanSetWidgetMode(UE::Widget::EWidgetMode NewMode) const override;
	virtual FVector GetWidgetLocation() const override;
	virtual bool InputWidgetDelta(
		FViewport* InViewport,
		EAxisList::Type CurrentAxis,
		FVector& Drag,
		FRotator& Rot,
		FVector& Scale) override;
	virtual void TrackingStarted(const struct FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge) override;
	virtual void TrackingStopped() override;
	virtual bool GetPivotForOrbit(FVector& OutPivot) const override;
	virtual bool ShouldOrbitCamera() const override;

private:
	friend class FUbstreamCursorLifecycleTest;
	TUniquePtr<class FScopedTransaction> ScaleDragTransaction;
	/** Shared wheel/pinch zoom core. Positive notches zoom in; each notch
	    multiplies the orbit radius (or remaining fly distance) by a constant
	    factor. Returns false only when no stage is bound. */
	bool ApplyStageWheelZoom(FViewport* InViewport, double Notches);
	bool ShouldShowCSGTransformWidget() const;
	/** True when Scale mode should use the legacy editor TRS widget (ITF scale handles are unreliable in this stage). */
	bool ShouldUseLegacyScaleWidget() const;
	void ApplyStageShowFlags();
	void DrawCSGPrimitiveOverlays(const FSceneView* View, FPrimitiveDrawInterface* PDI) const;
	void DrawInspectionOverlays(const FSceneView* View, FPrimitiveDrawInterface* PDI) const;
	TSharedRef<FAdvancedPreviewScene> PreviewSceneRef;
	TWeakPtr<SUbstreamModelerViewport> StageViewport;
	EViewModeIndex InspectionViewMode = VMI_Lit;
	FVector OrbitPivot = FVector::ZeroVector;
	bool bHasOrbitPivot = false;
	/** Persistent user toggle; bUsingOrbitCamera alone is force-synced to
	    ShouldOrbitCamera() every tick by UpdateCameraMovement. */
	bool bOrbitCameraUserEnabled = true;
	FIntPoint LastHoverQueryPosition = FIntPoint(-1000, -1000);

	// Optional drag instrumentation, enabled with ubstream.ProfileDrag.
	struct FUbstreamDragProfile
	{
		double WindowStartSeconds = 0.0;
		int32 Frames = 0;
		int32 MouseMoves = 0;
		int32 CursorBaseQueries = 0;
		double TickMs = 0.0;
		double TickMaxMs = 0.0;
		double DrawMs = 0.0;
		double DrawMaxMs = 0.0;
		double CapturedMoveMs = 0.0;
		double CapturedMoveMaxMs = 0.0;
		void Reset(double Now) { *this = FUbstreamDragProfile(); WindowStartSeconds = Now; }
	};
	FUbstreamDragProfile DragProfile;
	bool IsAnyMouseButtonDown() const;
	void AccumulateDragProfile(double& Bucket, double& MaxBucket, double Ms);
	void FlushDragProfileIfDue();
	bool bShowStageGrid = false;
	bool bShowStageBounds = false;
	bool bDrawNormals = false;
	bool bDrawTangents = false;
	bool bDrawBinormals = false;
	bool bDrawVertices = false;
	/** Throttle DestroyForeignGizmoActors — running it every tick blinked rotation rings. */
	double LastForeignGizmoSweepSeconds = 0.0;
	/** While dragging the legacy scale widget on a cutter we turn the editor's
	    scale-snap off so scaling is smooth (restored on drag end). */
	bool bScaleSnapOverridden = false;
	bool bSavedScaleSnapEnabled = false;
};
