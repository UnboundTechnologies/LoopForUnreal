#include "Viewport/UbstreamViewportClient.h"
#include "HAL/IConsoleManager.h"

#include "EditorModeManager.h"
#include "UbstreamModelerEditorModule.h"

#include "AdvancedPreviewScene.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "MouseDeltaTracker.h"
#include "PrimitiveDrawInterface.h"
#include "PrimitiveDrawingUtils.h"
#include "SceneView.h"
#include "ScopedTransaction.h"
#include "SEditorViewport.h"
#include "Settings/EditorViewportSettings.h"
#include "Settings/LevelEditorViewportSettings.h"
#include "StaticMeshResources.h"
#include "UnrealWidget.h"
#include "Viewport/UbstreamCleanViewportFlags.h"
#include "Widgets/SUbstreamModelerViewport.h"

namespace
{
	TAutoConsoleVariable<int32> CVarUbstreamProfileDrag(
		TEXT("ubstream.ProfileDrag"), 0,
		TEXT("Enable drag timing summaries. Disabled during normal editing."));

	constexpr int32 MaxInspectionOverlaySamples = 6000;
	constexpr float ModelerDefaultFovDegrees = 45.0f;
	constexpr float ModelerMinFovDegrees = 10.0f;
	constexpr float ModelerMaxFovDegrees = 90.0f;
	constexpr double ModelerMinFrameExtent = 1.0e-5;
	constexpr double ModelerMinFrameDistance = 1.0e-4;
	constexpr double ModelerFramePadding = 1.18;

	float ClampModelerFov(const float FovDegrees)
	{
		return FMath::Clamp(FovDegrees, ModelerMinFovDegrees, ModelerMaxFovDegrees);
	}

	UE::Widget::EWidgetMode ToWidgetMode(const EUbstreamCSGTransformMode TransformMode)
	{
		switch (TransformMode)
		{
		case EUbstreamCSGTransformMode::Rotate:
			return UE::Widget::WM_Rotate;
		case EUbstreamCSGTransformMode::Scale:
			return UE::Widget::WM_Scale;
		case EUbstreamCSGTransformMode::Move:
		default:
			return UE::Widget::WM_Translate;
		}
	}

	bool FromWidgetMode(const UE::Widget::EWidgetMode WidgetMode, EUbstreamCSGTransformMode& OutTransformMode)
	{
		switch (WidgetMode)
		{
		case UE::Widget::WM_Translate:
			OutTransformMode = EUbstreamCSGTransformMode::Move;
			return true;
		case UE::Widget::WM_Rotate:
			OutTransformMode = EUbstreamCSGTransformMode::Rotate;
			return true;
		case UE::Widget::WM_Scale:
			OutTransformMode = EUbstreamCSGTransformMode::Scale;
			return true;
		default:
			return false;
		}
	}

	float GetOverlayVectorLength(const UMeshComponent& Component)
	{
		FBoxSphereBounds Bounds = Component.CalcBounds(Component.GetComponentTransform());
		if (Bounds.ContainsNaN() || !Bounds.GetBox().IsValid)
		{
			Bounds = Component.Bounds;
		}

		const double Radius = FMath::Max<double>(Bounds.SphereRadius, ModelerMinFrameExtent);
		return static_cast<float>(FMath::Clamp(Radius * 0.035, Radius * 0.0025, Radius * 0.25));
	}

	void DrawStaticMeshInspectionOverlay(
		const UStaticMeshComponent& Component,
		const bool bDrawNormals,
		const bool bDrawTangents,
		const bool bDrawBinormals,
		const bool bDrawVertices,
		FPrimitiveDrawInterface* PDI)
	{
		const UStaticMesh* StaticMesh = Component.GetStaticMesh();
		const FStaticMeshRenderData* RenderData = StaticMesh ? StaticMesh->GetRenderData() : nullptr;
		if (!RenderData || RenderData->LODResources.IsEmpty())
		{
			return;
		}

		const FStaticMeshLODResources& LODModel = RenderData->LODResources[0];
		const uint32 PositionVertexCount = LODModel.VertexBuffers.PositionVertexBuffer.GetNumVertices();
		const uint32 TangentVertexCount = LODModel.VertexBuffers.StaticMeshVertexBuffer.GetNumVertices();
		const uint32 VertexCount = FMath::Min(PositionVertexCount, TangentVertexCount);
		if (VertexCount == 0)
		{
			return;
		}

		const uint32 SampleStep = FMath::Max<uint32>(1, VertexCount / MaxInspectionOverlaySamples);
		const FTransform ComponentTransform = Component.GetComponentTransform();
		const FMatrix LocalToWorldInverseTranspose = ComponentTransform.ToMatrixWithScale().InverseFast().GetTransposed();
		const float VectorLength = GetOverlayVectorLength(Component);

		for (uint32 VertexIndex = 0; VertexIndex < VertexCount; VertexIndex += SampleStep)
		{
			const FVector WorldPosition = ComponentTransform.TransformPosition(
				static_cast<FVector>(LODModel.VertexBuffers.PositionVertexBuffer.VertexPosition(VertexIndex)));

			if (bDrawNormals)
			{
				const FVector Direction = LocalToWorldInverseTranspose.TransformVector(
					static_cast<FVector>(LODModel.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(VertexIndex))).GetSafeNormal();
				PDI->DrawLine(WorldPosition, WorldPosition + Direction * VectorLength, FLinearColor::Green, SDPG_World, 0.0f);
			}

			if (bDrawTangents)
			{
				const FVector Direction = LocalToWorldInverseTranspose.TransformVector(
					static_cast<FVector>(LODModel.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(VertexIndex))).GetSafeNormal();
				PDI->DrawLine(WorldPosition, WorldPosition + Direction * VectorLength, FLinearColor::Red, SDPG_World, 0.0f);
			}

			if (bDrawBinormals)
			{
				const FVector Direction = LocalToWorldInverseTranspose.TransformVector(
					static_cast<FVector>(LODModel.VertexBuffers.StaticMeshVertexBuffer.VertexTangentY(VertexIndex))).GetSafeNormal();
				PDI->DrawLine(WorldPosition, WorldPosition + Direction * VectorLength, FLinearColor::Blue, SDPG_World, 0.0f);
			}

			if (bDrawVertices)
			{
				PDI->DrawPoint(WorldPosition, FLinearColor::Green, 4.0f, SDPG_Foreground);
			}
		}
	}

	void DrawDynamicMeshInspectionOverlay(
		const UDynamicMeshComponent& Component,
		const bool bDrawNormals,
		const bool bDrawTangents,
		const bool bDrawBinormals,
		const bool bDrawVertices,
		FPrimitiveDrawInterface* PDI)
	{
		const FTransform ComponentTransform = Component.GetComponentTransform();
		const FMatrix LocalToWorldInverseTranspose = ComponentTransform.ToMatrixWithScale().InverseFast().GetTransposed();
		const float VectorLength = GetOverlayVectorLength(Component);

		Component.ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			if (Mesh.VertexCount() == 0)
			{
				return;
			}

			const int32 SampleStep = FMath::Max(1, Mesh.VertexCount() / MaxInspectionOverlaySamples);
			int32 VisitedVertexCount = 0;
			for (const int32 VertexID : Mesh.VertexIndicesItr())
			{
				if ((VisitedVertexCount++ % SampleStep) != 0)
				{
					continue;
				}

				const FVector WorldPosition = ComponentTransform.TransformPosition(static_cast<FVector>(Mesh.GetVertex(VertexID)));
				const FVector Normal = LocalToWorldInverseTranspose.TransformVector(
					static_cast<FVector>(Mesh.GetVertexNormal(VertexID))).GetSafeNormal();

				FVector Tangent;
				FVector Binormal;
				Normal.FindBestAxisVectors(Tangent, Binormal);

				if (bDrawNormals)
				{
					PDI->DrawLine(WorldPosition, WorldPosition + Normal * VectorLength, FLinearColor::Green, SDPG_World, 0.0f);
				}

				if (bDrawTangents)
				{
					PDI->DrawLine(WorldPosition, WorldPosition + Tangent * VectorLength, FLinearColor::Red, SDPG_World, 0.0f);
				}

				if (bDrawBinormals)
				{
					PDI->DrawLine(WorldPosition, WorldPosition + Binormal * VectorLength, FLinearColor::Blue, SDPG_World, 0.0f);
				}

				if (bDrawVertices)
				{
					PDI->DrawPoint(WorldPosition, FLinearColor::Green, 4.0f, SDPG_Foreground);
				}
			}
		});
	}
}

FUbstreamViewportClient::FUbstreamViewportClient(
	FEditorModeTools* InModeTools,
	const TSharedRef<FAdvancedPreviewScene>& InPreviewScene,
	const TSharedRef<SEditorViewport>& InViewport)
	: FEditorViewportClient(InModeTools, &InPreviewScene.Get(), InViewport)
	, PreviewSceneRef(InPreviewScene)
{
	SetViewMode(VMI_Lit);
	ViewFOV = ModelerDefaultFovDegrees;
	FOVAngle = ModelerDefaultFovDegrees;
	SetViewLocation(FVector(-350.0f, -350.0f, 220.0f));
	SetViewRotation(FRotator(-24.0f, 45.0f, 0.0f));
	OverrideNearClipPlane(0.1f);
	// Orbit by default: a single-subject stage is an inspection camera, not a
	// fly-through. The eye-menu toggle still switches back.
	ToggleOrbitCamera(true);
	SetRealtime(false);

	bDrawAxes = false;
	bDrawAxesGame = false;
	bShowWidget = false;
	bUsesDrawHelper = false;
	DrawHelper.bDrawGrid = false;
	DrawHelper.bDrawPivot = false;
	DrawHelper.bDrawWorldBox = false;
	DrawHelper.bDrawKillZ = false;

	ApplyStageShowFlags();
}

FUbstreamViewportClient::~FUbstreamViewportClient() = default;

void FUbstreamViewportClient::SetStageViewport(const TWeakPtr<SUbstreamModelerViewport>& InStageViewport)
{
	StageViewport = InStageViewport;
}

void FUbstreamViewportClient::FocusOnBounds(const FBoxSphereBounds& Bounds)
{
	const FRotator FocusRotation(-24.0f, 45.0f, 0.0f);
	FocusOnBounds(Bounds, FocusRotation);
}

void FUbstreamViewportClient::FocusOnBounds(const FBoxSphereBounds& Bounds, const FRotator& FocusRotation)
{
	const FVector Center = Bounds.Origin;
	const FRotationMatrix ViewRotationMatrix(FocusRotation);
	const FVector Forward = ViewRotationMatrix.GetUnitAxis(EAxis::X);
	const FVector Right = ViewRotationMatrix.GetUnitAxis(EAxis::Y);
	const FVector Up = ViewRotationMatrix.GetUnitAxis(EAxis::Z);
	const FVector RawExtent = Bounds.BoxExtent.GetAbs();
	const double BoundsScale = FMath::Max<double>(Bounds.SphereRadius, RawExtent.GetMax());
	const double EffectiveScale = FMath::Max(BoundsScale, ModelerMinFrameExtent);
	const double AxisFloor = FMath::Max(EffectiveScale * 1.0e-4, ModelerMinFrameExtent);
	const FVector Extent(
		FMath::Max<double>(RawExtent.X, AxisFloor),
		FMath::Max<double>(RawExtent.Y, AxisFloor),
		FMath::Max<double>(RawExtent.Z, AxisFloor));

	ViewFOV = ClampModelerFov(ViewFOV);
	FOVAngle = ViewFOV;

	const double VerticalHalfFovRadians = FMath::DegreesToRadians(static_cast<double>(ViewFOV) * 0.5);
	double Aspect = AspectRatio > KINDA_SMALL_NUMBER ? AspectRatio : 16.0 / 9.0;
	if (Viewport)
	{
		const FIntPoint ViewportSize = Viewport->GetSizeXY();
		if (ViewportSize.Y > 0)
		{
			Aspect = static_cast<double>(ViewportSize.X) / static_cast<double>(ViewportSize.Y);
		}
	}
	const double HorizontalHalfFovRadians = FMath::Atan(FMath::Tan(VerticalHalfFovRadians) * FMath::Max(Aspect, 0.1));

	auto ProjectExtent = [&Extent](const FVector& Axis)
	{
		return FMath::Abs(Axis.X) * Extent.X
			+ FMath::Abs(Axis.Y) * Extent.Y
			+ FMath::Abs(Axis.Z) * Extent.Z;
	};

	const double ProjectedHalfWidth = FMath::Max(ProjectExtent(Right), AxisFloor);
	const double ProjectedHalfHeight = FMath::Max(ProjectExtent(Up), AxisFloor);
	const double ProjectedHalfDepth = FMath::Max(ProjectExtent(Forward), AxisFloor);
	const double WidthDistance = ProjectedHalfWidth / FMath::Tan(FMath::Max(HorizontalHalfFovRadians, 0.01));
	const double HeightDistance = ProjectedHalfHeight / FMath::Tan(FMath::Max(VerticalHalfFovRadians, 0.01));
	const double FrameDistance = (ProjectedHalfDepth + FMath::Max(WidthDistance, HeightDistance)) * ModelerFramePadding;
	const double Distance = FMath::Max(FrameDistance, FMath::Max(EffectiveScale * 0.05, ModelerMinFrameDistance));
	const double NearClipPlane = FMath::Clamp(Distance * 0.0005, 1.0e-5, 10.0);
	FEditorViewportCameraSpeedSettings SpeedSettings = GetCameraSpeedSettings();
	SpeedSettings.SetCurrentSpeed(FMath::Clamp(static_cast<float>(Distance / 220.0), 0.0001f, 1000.0f));

	OrbitPivot = Center;
	bHasOrbitPivot = true;
	OverrideNearClipPlane(static_cast<float>(NearClipPlane));
	SetCameraSpeedSettings(SpeedSettings);
	// Apply framing in free transform space, then re-arm orbit so wheel zoom
	// and drag-orbit keep working around the new pivot.
	ToggleOrbitCamera(false);
	SetViewRotation(FocusRotation);
	SetLookAtLocation(Center);
	SetViewLocation(Center - Forward * Distance);
	if (bOrbitCameraUserEnabled)
	{
		ToggleOrbitCamera(true);
	}
	Invalidate();
}

void FUbstreamViewportClient::SetInspectionViewMode(EViewModeIndex ViewMode)
{
	InspectionViewMode = ViewMode;
	SetViewMode(InspectionViewMode);
	ApplyStageShowFlags();
	Invalidate();
}

EViewModeIndex FUbstreamViewportClient::GetInspectionViewMode() const
{
	return InspectionViewMode;
}

void FUbstreamViewportClient::ToggleOverlay(EOverlay Overlay)
{
	switch (Overlay)
	{
	case EOverlay::Grid:
		bShowStageGrid = !bShowStageGrid;
		break;
	case EOverlay::Bounds:
		bShowStageBounds = !bShowStageBounds;
		break;
	case EOverlay::Normals:
		bDrawNormals = !bDrawNormals;
		break;
	case EOverlay::Tangents:
		bDrawTangents = !bDrawTangents;
		break;
	case EOverlay::Binormals:
		bDrawBinormals = !bDrawBinormals;
		break;
	case EOverlay::Vertices:
		bDrawVertices = !bDrawVertices;
		break;
	default:
		break;
	}

	ApplyStageShowFlags();
	Invalidate();
}

bool FUbstreamViewportClient::IsOverlayEnabled(EOverlay Overlay) const
{
	switch (Overlay)
	{
	case EOverlay::Grid:
		return bShowStageGrid;
	case EOverlay::Bounds:
		return bShowStageBounds;
	case EOverlay::Normals:
		return bDrawNormals;
	case EOverlay::Tangents:
		return bDrawTangents;
	case EOverlay::Binormals:
		return bDrawBinormals;
	case EOverlay::Vertices:
		return bDrawVertices;
	default:
		return false;
	}
}

bool FUbstreamViewportClient::GetPivotForOrbit(FVector& OutPivot) const
{
	if (bHasOrbitPivot)
	{
		OutPivot = OrbitPivot;
		return true;
	}

	// Never defer to the base implementation: the hosted Modeling Mode answers
	// it with the LEVEL editor viewport's look-at, which is the wrong host.
	// Orbit the stage camera around its own look-at instead.
	OutPivot = GetViewTransform().GetLookAt();
	return true;
}

void FUbstreamViewportClient::SetOrbitCameraEnabled(const bool bEnabled)
{
	bOrbitCameraUserEnabled = bEnabled;
	if (bUsingOrbitCamera != bEnabled)
	{
		ToggleOrbitCamera(bEnabled);
		Invalidate();
	}
}

bool FUbstreamViewportClient::IsOrbitCameraEnabled() const
{
	// Report user intent, not the live engine flag: focus framing briefly
	// drops orbit and the engine re-enables it a tick later via
	// ShouldOrbitCamera(); reading bUsingOrbitCamera would flicker the UI.
	return bOrbitCameraUserEnabled;
}

bool FUbstreamViewportClient::ShouldOrbitCamera() const
{
	// UpdateCameraMovement force-disables orbit on every tick where this
	// returns false; without honoring the user toggle here, enabling orbit
	// immediately unchecks itself. Alt-orbit (base behavior) still works.
	return bOrbitCameraUserEnabled || FEditorViewportClient::ShouldOrbitCamera();
}

UE::Widget::EWidgetMode FUbstreamViewportClient::GetWidgetMode() const
{
	const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();
	if (Stage.IsValid() && ShouldShowCSGTransformWidget())
	{
		return ToWidgetMode(Stage->GetCSGTransformMode());
	}

	return FEditorViewportClient::GetWidgetMode();
}

void FUbstreamViewportClient::SetWidgetMode(const UE::Widget::EWidgetMode NewMode)
{
	EUbstreamCSGTransformMode TransformMode = EUbstreamCSGTransformMode::Move;
	const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();
	if (Stage.IsValid()
		&& !Stage->HasAnyActiveStageTool()
		&& FromWidgetMode(NewMode, TransformMode)
		&& Stage->SetCSGTransformMode(TransformMode))
	{
		Invalidate();
		return;
	}

	FEditorViewportClient::SetWidgetMode(NewMode);
}

void FUbstreamViewportClient::ProcessClick(
	FSceneView& View,
	HHitProxy* HitProxy,
	FKey Key,
	EInputEvent Event,
	uint32 HitX,
	uint32 HitY)
{
	const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();

	FVector RayOrigin = FVector::ZeroVector;
	FVector RayDirection = FVector::ForwardVector;
	FSceneView::DeprojectScreenToWorld(
		FVector2D(HitX, HitY), View.UnconstrainedViewRect,
		View.ViewMatrices.GetInvViewProjectionMatrix(),
		RayOrigin, RayDirection);

	// Armed click-to-place beats everything: the user explicitly asked to
	// drop a cutter on the model.
	if (Key == EKeys::LeftMouseButton
		&& Stage.IsValid()
		&& Stage->HasPendingCSGPlacement())
	{
		Stage->PlacePendingCSGPrimitiveAtRay(RayOrigin, RayDirection);
		return;
	}

	// Web-editor selection grammar: plain click replaces, Shift adds,
	// Ctrl/Cmd toggles (the add path toggles too, which covers both).
	const bool bAdditiveSelect = IsShiftPressed() || IsCtrlPressed() || IsCmdPressed();

	// Cutter picking is analytic: session cutter bodies render invisible
	// (outline-only), and translucent surfaces don't reach the hit-proxy
	// pass, so a pixel-based pick would fall through to the model behind
	// and teleport the cutter instead of selecting it. The model still wins
	// when it is strictly in front of the cutter.
	if (Key == EKeys::LeftMouseButton
		&& Stage.IsValid()
		&& !Stage->HasAnyActiveStageTool())
	{
		double CutterRayParameter = TNumericLimits<double>::Max();
		if (UMeshComponent* Cutter = Stage->FindCSGPrimitiveUnderRay(RayOrigin, RayDirection, CutterRayParameter))
		{
			FVector ModelHitPoint = FVector::ZeroVector;
			double ModelRayParameter = TNumericLimits<double>::Max();
			if (Stage->RaycastSourcePreview(RayOrigin, RayDirection, ModelHitPoint))
			{
				ModelRayParameter = FVector::DotProduct(ModelHitPoint - RayOrigin, RayDirection);
			}
			// Ties (cutter face coincident with the model surface) select the
			// cutter: selection beats repositioning.
			if (CutterRayParameter <= ModelRayParameter + UE_DOUBLE_KINDA_SMALL_NUMBER
				&& Stage->SelectCSGPrimitiveForEditing(Cutter, bAdditiveSelect))
			{
				return;
			}
		}
	}

	if (Key == EKeys::LeftMouseButton
		&& HitProxy
		&& HitProxy->IsA(HActor::StaticGetType()))
	{
		const HActor* ActorHitProxy = static_cast<HActor*>(HitProxy);
		if (Stage.IsValid()
			&& !Stage->HasAnyActiveStageTool()
			&& Stage->SelectCSGPrimitiveForEditing(ActorHitProxy->PrimComponent.Get(), bAdditiveSelect))
		{
			return;
		}
	}

	// With a cutter SELECTED, clicking the model jumps that cutter there - the
	// fastest way to steer a cut. With nothing selected, a click should select
	// instead (fall through to stage-component selection below), not silently
	// move a cutter.
	if (Key == EKeys::LeftMouseButton
		&& Stage.IsValid()
		&& Stage->HasActiveCSGSession()
		&& Stage->HasSelectedCSGPrimitive()
		&& Stage->RepositionSessionCutterAtRay(RayOrigin, RayDirection))
	{
		return;
	}

	if (Key == EKeys::LeftMouseButton
		&& HitProxy
		&& HitProxy->IsA(HActor::StaticGetType()))
	{
		const HActor* ActorHitProxy = static_cast<HActor*>(HitProxy);
		if (Stage.IsValid()
			&& !Stage->HasAnyActiveStageTool()
			&& Stage->SelectStageComponentForEditing(ActorHitProxy->PrimComponent.Get()))
		{
			return;
		}
	}

	// Clicking empty space clears the selection - the same "click nothing,
	// deselect" grammar as the web editor.
	if (Key == EKeys::LeftMouseButton
		&& !HitProxy
		&& !bAdditiveSelect
		&& Stage.IsValid()
		&& !Stage->HasAnyActiveStageTool())
	{
		FVector UnusedHit = FVector::ZeroVector;
		if (!Stage->RaycastSourcePreview(RayOrigin, RayDirection, UnusedHit))
		{
			Stage->DeselectAllStageComponentsForUI();
			return;
		}
	}

	FEditorViewportClient::ProcessClick(View, HitProxy, Key, Event, HitX, HitY);
}

bool FUbstreamViewportClient::CanSetWidgetMode(const UE::Widget::EWidgetMode NewMode) const
{
	if (NewMode == UE::Widget::WM_Translate
		|| NewMode == UE::Widget::WM_Rotate
		|| NewMode == UE::Widget::WM_Scale)
	{
		FVector WidgetLocation = FVector::ZeroVector;
		const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();
		if (Stage.IsValid() && Stage->GetCSGTransformWidgetLocation(WidgetLocation))
		{
			return true;
		}
	}

	return FEditorViewportClient::CanSetWidgetMode(NewMode);
}

bool FUbstreamViewportClient::InputWidgetDelta(
	FViewport* InViewport,
	const EAxisList::Type CurrentAxis,
	FVector& Drag,
	FRotator& Rot,
	FVector& Scale)
{
	// Legacy scale widget path: apply deltas to selected cutters.
	// Without this override ModeTools has nothing that owns the selected stage
	// components, so Scale mode looks interactive but changes nothing.
	if (const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin())
	{
		if (!Stage->HasAnyActiveStageTool() && ShouldUseLegacyScaleWidget())
		{
			if (Stage->HasSelectedCSGPrimitive() && !Scale.IsNearlyZero() && !ScaleDragTransaction)
			{
				ScaleDragTransaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UbstreamViewport", "ScaleDrag", "Scale Cutter"));
			}
			if (Stage->HasSelectedCSGPrimitive()
				&& Stage->ApplyCSGTransformDelta(Drag, Rot, Scale))
			{
				return true;
			}
		}
	}

	return FEditorViewportClient::InputWidgetDelta(InViewport, CurrentAxis, Drag, Rot, Scale);
}

void FUbstreamViewportClient::TrackingStarted(const FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge)
{
	FEditorViewportClient::TrackingStarted(InInputState, bIsDraggingWidget, bNudge);

	// The legacy scale widget delivers grid-SNAPPED deltas (FSnappingUtils::
	// SnapScale), so cutter scaling jumps in steps while move/rotate (our own
	// snap, off by default) stay smooth. Turn the editor's scale-snap off for
	// the duration of a scale-widget drag; restore it in TrackingStopped.
	if (bIsDraggingWidget && ShouldUseLegacyScaleWidget() && !bScaleSnapOverridden)
	{
		if (ULevelEditorViewportSettings* Settings = GetMutableDefault<ULevelEditorViewportSettings>())
		{
			bSavedScaleSnapEnabled = Settings->SnapScaleEnabled != 0;
			Settings->SnapScaleEnabled = false;
			bScaleSnapOverridden = true;
		}
	}
}

void FUbstreamViewportClient::TrackingStopped()
{
	ScaleDragTransaction.Reset();
	if (bScaleSnapOverridden)
	{
		if (ULevelEditorViewportSettings* Settings = GetMutableDefault<ULevelEditorViewportSettings>())
		{
			Settings->SnapScaleEnabled = bSavedScaleSnapEnabled;
		}
		bScaleSnapOverridden = false;
	}

	FEditorViewportClient::TrackingStopped();
}

FVector FUbstreamViewportClient::GetWidgetLocation() const
{
	FVector WidgetLocation = FVector::ZeroVector;
	const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();
	if (Stage.IsValid() && Stage->GetCSGTransformWidgetLocation(WidgetLocation))
	{
		return WidgetLocation;
	}

	return FEditorViewportClient::GetWidgetLocation();
}

void FUbstreamViewportClient::Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	bShowWidget = ShouldShowCSGTransformWidget();
	FEditorViewportClient::Draw(View, PDI);
	DrawCSGPrimitiveOverlays(View, PDI);
	DrawInspectionOverlays(View, PDI);
}

bool FUbstreamViewportClient::ShouldShowCSGTransformWidget() const
{
	const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();
	if (!Stage.IsValid() || Stage->HasAnyActiveStageTool())
	{
		return false;
	}

	// Scale uses the legacy editor widget (see ShouldUseLegacyScaleWidget). The
	// legacy path needs ShowWidget / ModeWidgets; ITF gizmo stays for Move/Rotate.
	if (ShouldUseLegacyScaleWidget())
	{
		return true;
	}

	FVector WidgetLocation = FVector::ZeroVector;
	return Stage->GetCSGTransformWidgetLocation(WidgetLocation);
}

bool FUbstreamViewportClient::ShouldUseLegacyScaleWidget() const
{
	const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();
	if (!Stage.IsValid() || Stage->HasAnyActiveStageTool())
	{
		return false;
	}
	if (Stage->GetCSGTransformMode() != EUbstreamCSGTransformMode::Scale)
	{
		return false;
	}
	return Stage->HasSelectedCSGPrimitive();
}

void FUbstreamViewportClient::ApplyStageShowFlags()
{
	UbstreamCleanViewportFlags::Apply(EngineShowFlags);
	EngineShowFlags.SetGrid(bShowStageGrid);
	EngineShowFlags.SetBounds(bShowStageBounds);

	// The mode-manager's legacy widget must never render here (the stage's
	// ITF gizmo components are ordinary primitives, not gated by this flag).
	EngineShowFlags.SetModeWidgets(false);
	// The on-screen stage needs interactive-tool rendering (gizmos, tool
	// drawing, selection highlights); the streamed capture stays clean because
	// the capture component applies its own show flags.
	EngineShowFlags.SetCompositeEditorPrimitives(true);
	EngineShowFlags.SetModeWidgets(true);
	EngineShowFlags.SetWidgetComponents(true);
	EngineShowFlags.SetSelectionOutline(true);

	DrawHelper.bDrawGrid = bShowStageGrid;
	DrawHelper.bDrawPivot = false;
	DrawHelper.bDrawWorldBox = false;
	DrawHelper.bDrawKillZ = false;
}

void FUbstreamViewportClient::DrawCSGPrimitiveOverlays(const FSceneView* View, FPrimitiveDrawInterface* PDI) const
{
	if (!View || !PDI)
	{
		return;
	}

	const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();
	if (!Stage.IsValid())
	{
		return;
	}

	TArray<UMeshComponent*> CSGPrimitives;
	Stage->GetCSGPrimitiveComponents(CSGPrimitives);
	if (CSGPrimitives.IsEmpty())
	{
		return;
	}

	TArray<UMeshComponent*> SelectedPrimitives;
	Stage->GetSelectedCSGPrimitiveComponents(SelectedPrimitives);
	TSet<const UMeshComponent*> SelectedSet;
	for (const UMeshComponent* SelectedPrimitive : SelectedPrimitives)
	{
		SelectedSet.Add(SelectedPrimitive);
	}

	// Hairline thickness (0 = one screen pixel) and proportional padding:
	// PDI line thickness and box padding are WORLD units, so any absolute
	// value here turns screen-filling on small models. A 0.25-unit padding
	// floor and 2.75-unit lines were exactly the "huge yellow thing" reported
	// on tiny imported meshes.
	constexpr float UnselectedThickness = 0.0f;
	constexpr float SelectedThickness = 0.0f;
	const FLinearColor UnselectedColor(0.18f, 0.78f, 1.0f, 0.95f);
	const FLinearColor SelectedColor(1.0f, 0.76f, 0.05f, 1.0f);

	for (const UMeshComponent* Component : CSGPrimitives)
	{
		if (!IsValid(Component))
		{
			continue;
		}

		const FBoxSphereBounds Bounds = Component->CalcBounds(Component->GetComponentTransform());
		FBox Box = Bounds.GetBox();
		if (!Box.IsValid)
		{
			continue;
		}

		const bool bSelected = SelectedSet.Contains(Component);
		const double Padding = Bounds.SphereRadius * 0.02;
		Box = Box.ExpandBy(Padding);
		DrawWireBox(
			PDI,
			Box,
			bSelected ? SelectedColor : UnselectedColor,
			SDPG_Foreground,
			bSelected ? SelectedThickness : UnselectedThickness);
		PDI->DrawPoint(
			Bounds.Origin,
			bSelected ? SelectedColor : UnselectedColor,
			bSelected ? 9.0f : 5.0f,
			SDPG_Foreground);
	}
}

void FUbstreamViewportClient::DrawInspectionOverlays(const FSceneView* View, FPrimitiveDrawInterface* PDI) const
{
	if (!View || !PDI)
	{
		return;
	}

	if (!bDrawNormals && !bDrawTangents && !bDrawBinormals && !bDrawVertices)
	{
		return;
	}

	const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();
	if (!Stage.IsValid())
	{
		return;
	}

	for (const TObjectPtr<UMeshComponent>& ComponentPtr : Stage->GetPreviewComponents())
	{
		const UMeshComponent* Component = ComponentPtr.Get();
		if (!IsValid(Component))
		{
			continue;
		}

		if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
		{
			DrawStaticMeshInspectionOverlay(
				*StaticMeshComponent,
				bDrawNormals,
				bDrawTangents,
				bDrawBinormals,
				bDrawVertices,
				PDI);
			continue;
		}

		if (const UDynamicMeshComponent* DynamicMeshComponent = Cast<UDynamicMeshComponent>(Component))
		{
			DrawDynamicMeshInspectionOverlay(
				*DynamicMeshComponent,
				bDrawNormals,
				bDrawTangents,
				bDrawBinormals,
				bDrawVertices,
				PDI);
		}
	}
}

void FUbstreamViewportClient::Tick(float DeltaSeconds)
{
	const bool bProfileDrag = CVarUbstreamProfileDrag.GetValueOnGameThread() != 0;
	const double UbstreamTickStart = bProfileDrag ? FPlatformTime::Seconds() : 0.0;
	FEditorViewportClient::Tick(DeltaSeconds);
	bShowWidget = ShouldShowCSGTransformWidget();

	ViewFOV = ClampModelerFov(ViewFOV);
	FOVAngle = ViewFOV;

	if (IsRealtime())
	{
		if (UWorld* World = PreviewSceneRef->GetWorld())
		{
			World->Tick(LEVELTICK_All, DeltaSeconds);
		}
	}

	if (const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin())
	{
		Stage->UpdateStageLighting();
		Stage->RefreshToolPreviewVisibility();
		Stage->TickCSGSession();
	}

	// Move/Rotate: ITF CombinedTransformGizmo only (legacy widget off — it was
	// drawing a second dead set of axes). Scale: legacy editor widget, because
	// ITF scale handles with our stage gizmo setup do not receive usable input.
	const bool bLegacyScale = ShouldUseLegacyScaleWidget();
	if (FEditorModeTools* StageModeTools = GetModeTools())
	{
		const bool bWantShowWidget = bLegacyScale;
		if (StageModeTools->GetShowWidget() != bWantShowWidget)
		{
			StageModeTools->SetShowWidget(bWantShowWidget);
		}
		// Non-uniform scale requires Local; also matches stage ITF gizmo rules.
		if (StageModeTools->GetCoordSystem() != COORD_Local)
		{
			StageModeTools->SetCoordSystem(COORD_Local);
		}
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
		// UE 5.8 only draws + drags the legacy transform widget when the active mode
		// manager is NOT using the new viewport ITF gizmos
		// (FEditorModeTools::UsesViewportITF(), gated in MouseDeltaTracker). With the
		// new TRS gizmos default-on in 5.8 that predicate is true for our stage, so the
		// legacy scale axes vanish and a scale drag falls through to camera movement.
		// Report non-ITF while the legacy scale widget is active so the widget draws and
		// receives drags; restore ITF support for Move/Rotate + sculpt tools. This is
		// scoped to the stage's own mode manager, so the level editor is unaffected.
		// (No such API/gate on 5.7 — the legacy widget already works there.)
		if (StageModeTools->SupportsViewportITF() == bLegacyScale)
		{
			StageModeTools->SetSupportsViewportITF(!bLegacyScale);
		}
#endif
		if (bLegacyScale)
		{
			StageModeTools->SetWidgetMode(UE::Widget::WM_Scale);
		}
	}
	EngineShowFlags.SetModeWidgets(bLegacyScale);
	if (const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin())
	{
		// Hide ITF gizmo while legacy scale widget is active (avoids double chrome).
		Stage->SetStageTransformGizmoVisible(!bLegacyScale);
		// Do NOT sweep foreign gizmos every frame — destroying AGizmoActor instances
		// mid-frame made rotation rings blink. Sweep only when idle, throttled.
		if (!bLegacyScale && !IsAnyMouseButtonDown())
		{
			const double Now = FPlatformTime::Seconds();
			if (Now - LastForeignGizmoSweepSeconds > 1.0)
			{
				LastForeignGizmoSweepSeconds = Now;
				Stage->DestroyForeignGizmoActors();
			}
		}
	}

	if (bProfileDrag && IsAnyMouseButtonDown())
	{
		++DragProfile.Frames;
		AccumulateDragProfile(DragProfile.TickMs, DragProfile.TickMaxMs,
			(FPlatformTime::Seconds() - UbstreamTickStart) * 1000.0);
	}
	FlushDragProfileIfDue();
}

bool FUbstreamViewportClient::IsAnyMouseButtonDown() const
{
	return Viewport
		&& (Viewport->KeyState(EKeys::LeftMouseButton)
			|| Viewport->KeyState(EKeys::RightMouseButton)
			|| Viewport->KeyState(EKeys::MiddleMouseButton));
}

void FUbstreamViewportClient::AccumulateDragProfile(double& Bucket, double& MaxBucket, const double Ms)
{
	Bucket += Ms;
	MaxBucket = FMath::Max(MaxBucket, Ms);
}

void FUbstreamViewportClient::FlushDragProfileIfDue()
{
	if (CVarUbstreamProfileDrag.GetValueOnGameThread() == 0)
	{
		DragProfile.Reset(0.0);
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (DragProfile.WindowStartSeconds <= 0.0)
	{
		DragProfile.Reset(Now);
		return;
	}
	if (Now - DragProfile.WindowStartSeconds < 1.0)
	{
		return;
	}
	if (DragProfile.Frames > 0 && IsAnyMouseButtonDown())
	{
		UE_LOG(LogUbstreamModeler, Log,
			TEXT("UbstreamDragProfile: %d frames, %d captured moves | tick avg %.2fms max %.2fms | draw avg %.2fms max %.2fms | capturedMove avg %.2fms max %.2fms | cursor base queries %d"),
			DragProfile.Frames,
			DragProfile.MouseMoves,
			DragProfile.Frames > 0 ? DragProfile.TickMs / DragProfile.Frames : 0.0,
			DragProfile.TickMaxMs,
			DragProfile.Frames > 0 ? DragProfile.DrawMs / DragProfile.Frames : 0.0,
			DragProfile.DrawMaxMs,
			DragProfile.MouseMoves > 0 ? DragProfile.CapturedMoveMs / DragProfile.MouseMoves : 0.0,
			DragProfile.CapturedMoveMaxMs,
			DragProfile.CursorBaseQueries);
	}
	DragProfile.Reset(Now);
}

void FUbstreamViewportClient::CapturedMouseMove(FViewport* InViewport, const int32 InMouseX, const int32 InMouseY)
{
	const bool bProfileDrag = CVarUbstreamProfileDrag.GetValueOnGameThread() != 0;
	const double Start = bProfileDrag ? FPlatformTime::Seconds() : 0.0;
	// The base dispatches to ModeTools once and handles the camera fallback.
	FEditorViewportClient::CapturedMouseMove(InViewport, InMouseX, InMouseY);
	if (bProfileDrag)
	{
		++DragProfile.MouseMoves;
		AccumulateDragProfile(DragProfile.CapturedMoveMs, DragProfile.CapturedMoveMaxMs,
			(FPlatformTime::Seconds() - Start) * 1000.0);
	}
}

void FUbstreamViewportClient::Draw(FViewport* InViewport, FCanvas* Canvas)
{
	const bool bProfileDrag = CVarUbstreamProfileDrag.GetValueOnGameThread() != 0;
	const double Start = bProfileDrag ? FPlatformTime::Seconds() : 0.0;
	FEditorViewportClient::Draw(InViewport, Canvas);
	if (bProfileDrag && IsAnyMouseButtonDown())
	{
		AccumulateDragProfile(DragProfile.DrawMs, DragProfile.DrawMaxMs,
			(FPlatformTime::Seconds() - Start) * 1000.0);
	}
}

void FUbstreamViewportClient::MouseMove(FViewport* InViewport, const int32 X, const int32 Y)
{
	FEditorViewportClient::MouseMove(InViewport, X, Y);

	// Hover feedback: cutters brighten under the cursor. Resolved with an
	// analytic ray test, never a hit-proxy query: session cutter bodies are
	// invisible (translucent surfaces skip the hit-proxy pass), and a proxy
	// query after an invalidation forces a FULL extra scene render (measured
	// 5-56ms). Still skipped while any mouse button is down, and throttled
	// to real cursor travel.
	const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();
	const bool bAnyMouseButtonDown = InViewport
		&& (InViewport->KeyState(EKeys::LeftMouseButton)
			|| InViewport->KeyState(EKeys::RightMouseButton)
			|| InViewport->KeyState(EKeys::MiddleMouseButton));
	if (Stage.IsValid() && InViewport && !bIsTracking && !bAnyMouseButtonDown)
	{
		const FIntPoint CursorPosition(X, Y);
		if (FMath::Abs(CursorPosition.X - LastHoverQueryPosition.X) >= 4
			|| FMath::Abs(CursorPosition.Y - LastHoverQueryPosition.Y) >= 4)
		{
			LastHoverQueryPosition = CursorPosition;
			const FViewportCursorLocation CursorRay = GetCursorWorldLocationFromMousePos();
			double HoverRayParameter = TNumericLimits<double>::Max();
			Stage->SetHoveredCSGPrimitiveComponent(
				Stage->FindCSGPrimitiveUnderRay(CursorRay.GetOrigin(), CursorRay.GetDirection(), HoverRayParameter));
		}
	}
	else if (Stage.IsValid() && bAnyMouseButtonDown)
	{
		// No hover states mid-drag; also resets the throttle anchor.
		Stage->SetHoveredCSGPrimitiveComponent(nullptr);
		LastHoverQueryPosition = FIntPoint(-1000, -1000);
	}
}

void FUbstreamViewportClient::MouseLeave(FViewport* InViewport)
{
	if (const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin())
	{
		Stage->SetHoveredCSGPrimitiveComponent(nullptr);
	}
	FEditorViewportClient::MouseLeave(InViewport);
}

EMouseCursor::Type FUbstreamViewportClient::GetCursor(FViewport* InViewport, const int32 X, const int32 Y)
{
	// Cursor visibility is part of Unreal's mouse-capture lifecycle, not just
	// appearance. Forcing a visible hand while the engine hides the cursor
	// exposes its restore-to-start behavior as a jump on mouse release.
	if (CVarUbstreamProfileDrag.GetValueOnGameThread() != 0)
	{
		++DragProfile.CursorBaseQueries;
	}
	const EMouseCursor::Type Cursor = FEditorViewportClient::GetCursor(InViewport, X, Y);
	const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();
	if (Cursor != EMouseCursor::None && Stage.IsValid() && Stage->HasPendingCSGPlacement())
	{
		return EMouseCursor::Crosshairs;
	}
	return Cursor;
}

bool FUbstreamViewportClient::ApplyStageWheelZoom(FViewport* InViewport, const double Notches)
{
	const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();
	if (!Stage.IsValid() || !InViewport)
	{
		return false;
	}

	// Custom zoom is an ORBIT-camera feature. With the free camera the stage
	// keeps bone-stock Unreal controls: report unhandled so the base client
	// does its usual wheel dolly / gesture moves.
	if (!(IsOrbitCameraEnabled() || bUsingOrbitCamera))
	{
		return false;
	}

	// UE orbit view uses ComputeOrbitMatrix with radius = |ViewLocation-LookAt|
	// only (rotation is separate). Zoom must change that radius and nothing
	// else — no ToggleOrbitCamera, no pivot swaps, no aggressive near-plane.
	// Notches are clamped so trackpad momentum bursts can't fling the camera.
	const double ClampedNotches = FMath::Clamp(Notches, -3.0, 3.0);
	if (FMath::IsNearlyZero(ClampedNotches))
	{
		return true;
	}
	{
		static const FName CSGPrimitiveTagName(TEXT("ubstream_csg_primitive"));

		// Subject scale only (ignore CSG cutters — they can sit far from the
		// model and inflate steps enough to fling the camera).
		double SubjectRadius = 0.0;
		{
			FBox SubjectBounds(ForceInit);
			for (const TObjectPtr<UMeshComponent>& ComponentPtr : Stage->GetPreviewComponents())
			{
				const UMeshComponent* Component = ComponentPtr.Get();
				if (!IsValid(Component) || !Component->IsRegistered() || !Component->IsVisible())
				{
					continue;
				}
				if (Component->ComponentTags.Contains(CSGPrimitiveTagName))
				{
					continue;
				}
				SubjectBounds += Component->Bounds.GetBox();
			}
			if (SubjectBounds.IsValid)
			{
				const FBoxSphereBounds SphereBounds(SubjectBounds);
				SubjectRadius = FMath::Max3(
					static_cast<double>(SphereBounds.SphereRadius),
					static_cast<double>(SubjectBounds.GetExtent().GetMax()),
					1.0);
			}
			else
			{
				// Fall back to distance-to-look-at so empty/corrupt bounds never
				// produce a zero scale.
				SubjectRadius = FMath::Max(FVector::Dist(GetViewLocation(), GetLookAtLocation()) * 0.4, 1.0);
			}
		}

		// One notch MULTIPLIES the distance by a constant factor - the same
		// perceived zoom at every range, ~25 notches across the whole span.
		// (The previous additive step, capped at 8% of subject size, needed
		// literal hundreds of notches when zoomed out - the "broken" feel.)
		constexpr double ZoomInFactor = 0.90;
		const double MinDistance = FMath::Max(SubjectRadius * 0.05, 0.1);
		const double MaxDistance = FMath::Max(SubjectRadius * 40.0, MinDistance * 16.0);

		if (IsOrbitCameraEnabled() || bUsingOrbitCamera)
		{
			const FVector LookAt = GetLookAtLocation();

			// The only quantity the orbit matrix reads for zoom is this radius.
			FVector Offset = GetViewLocation() - LookAt;
			double Radius = Offset.Size();
			if (!FMath::IsFinite(Radius) || Radius < KINDA_SMALL_NUMBER)
			{
				// Degenerate transform: recover a framed radius from the subject.
				Radius = FMath::Max(SubjectRadius * 2.5, MinDistance * 4.0);
				if (bUsingOrbitCamera)
				{
					const FVector WorldEye = GetViewTransform().ComputeOrbitMatrix().InverseFast().GetOrigin();
					Offset = WorldEye - LookAt;
				}
				if (Offset.SizeSquared() < KINDA_SMALL_NUMBER)
				{
					Offset = FVector(-1.0, -1.0, 0.6);
				}
			}

			const double NewRadius = FMath::Clamp(
				Radius * FMath::Pow(ZoomInFactor, ClampedNotches),
				MinDistance,
				MaxDistance);

			// Rewrite ViewLocation so |ViewLocation - LookAt| == NewRadius while
			// keeping the same radial direction the orbit matrix already uses.
			OrbitPivot = LookAt;
			bHasOrbitPivot = true;
			SetViewLocation(LookAt + Offset.GetSafeNormal() * NewRadius);

			// Same near-plane policy as FocusOnBounds (hard cap 10). A previous
			// version allowed near ≈ 2% of distance and could clip the subject.
			OverrideNearClipPlane(static_cast<float>(
				FMath::Clamp(NewRadius * 0.0005, 1.0e-5, 10.0)));
			Invalidate();
			return true;
		}

	}
	return true;
}

#if UE_VERSION_OLDER_THAN(5, 8, 0)
bool FUbstreamViewportClient::InputGesture(
	FViewport* InViewport,
	const FInputDeviceId DeviceId,
	const EGestureEvent GestureType,
	const FVector2D& GestureDelta,
	const bool bIsDirectionInvertedFromDevice,
	const uint64 Timestamp)
#else
bool FUbstreamViewportClient::InputGesture(
	FViewport* InViewport,
	const FInputDeviceId DeviceId,
	const EGestureEvent GestureType,
	const EGesturePhase GesturePhase,
	const FVector2D& GestureDelta,
	const bool bIsDirectionInvertedFromDevice,
	const uint64 Timestamp)
#endif
{
	const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();
	if (Stage.IsValid() && !bDisableInput && InViewport && !IsTracking())
	{
		const bool bAnyMouseButtonDown = InViewport->KeyState(EKeys::LeftMouseButton)
			|| InViewport->KeyState(EKeys::RightMouseButton)
			|| InViewport->KeyState(EKeys::MiddleMouseButton);

		// Two-finger swipe zooms the orbit camera. Fingers-together scrolling
		// reaches this viewport ONLY as this gesture (no synthesized wheel
		// keys), and the stock handler would turn it into a camera-angle
		// change instead. Direction honors the editor's scroll-gesture
		// setting exactly like the stock handler does. When the orbit camera
		// is off, ApplyStageWheelZoom declines and the base client runs the
		// usual Unreal controls.
		if (GestureType == EGestureEvent::Scroll && !bAnyMouseButtonDown)
		{
			const EScrollGestureDirection DirectionSetting =
				GetDefault<ULevelEditorViewportSettings>()->ScrollGestureDirectionFor3DViewports;
			const bool bUseDirectionInvertedFromDevice =
				DirectionSetting == EScrollGestureDirection::Natural
				|| (DirectionSetting == EScrollGestureDirection::UseSystemSetting && bIsDirectionInvertedFromDevice);
			const double AdjustedDeltaY = (bUseDirectionInvertedFromDevice == bIsDirectionInvertedFromDevice)
				? GestureDelta.Y
				: -GestureDelta.Y;
			// ~20px of swipe per zoom notch: a full flick covers several
			// notches, matching the wheel's pace.
			if (ApplyStageWheelZoom(InViewport, AdjustedDeltaY / 20.0))
			{
				return true;
			}
		}
		// Pinch = the same radius zoom (its stock behavior is a free-fly
		// forward impulse that walks the camera off the orbit pivot).
		else if (GestureType == EGestureEvent::Magnify)
		{
			if (ApplyStageWheelZoom(InViewport, static_cast<double>(GestureDelta.X) * 0.3))
			{
				return true;
			}
		}
	}

	return FEditorViewportClient::InputGesture(
		InViewport, DeviceId, GestureType,
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
		GesturePhase,
#endif
		GestureDelta, bIsDirectionInvertedFromDevice, Timestamp);
}

bool FUbstreamViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	const TSharedPtr<SUbstreamModelerViewport> Stage = StageViewport.Pin();

	// Mouse wheel zoom (Pressed + Repeat for trackpads).
	if ((EventArgs.Key == EKeys::MouseScrollUp || EventArgs.Key == EKeys::MouseScrollDown)
		&& (EventArgs.Event == IE_Pressed || EventArgs.Event == IE_Repeat)
		&& EventArgs.Viewport
		&& !IsTracking()
		&& ApplyStageWheelZoom(EventArgs.Viewport, EventArgs.Key == EKeys::MouseScrollUp ? 1.0 : -1.0))
	{
		return true;
	}

	if (EventArgs.Event == IE_Pressed)
	{
		// F frames the selection (cutter first, else the model) and re-anchors
		// the orbit pivot there - the carving-editor staple.
		if (Stage.IsValid() && EventArgs.Key == EKeys::F && !IsCtrlPressed() && !IsAltPressed())
		{
			Stage->OnFocusViewportToSelection();
			return true;
		}

		if (Stage.IsValid() && Stage->HasPendingCSGPlacement() && EventArgs.Key == EKeys::Escape)
		{
			Stage->CancelCSGPlacement();
			return true;
		}

		// Esc with no live session dismisses the selection (and its gizmo).
		if (Stage.IsValid()
			&& EventArgs.Key == EKeys::Escape
			&& !Stage->HasActiveCSGSession()
			&& !Stage->HasAnyActiveStageTool()
			&& Stage->HasSelectedCSGPrimitive())
		{
			Stage->DeselectAllStageComponentsForUI();
			return true;
		}

		if (Stage.IsValid()
			&& EventArgs.Key == EKeys::D
			&& IsCtrlPressed()
			&& !Stage->HasAnyActiveStageTool())
		{
			// Ctrl+D: another one. Duplicates the selected cutter, or re-arms
			// placement with the last cutter used.
			if (Stage->DuplicateSelectedCSGPrimitive() || Stage->RepeatLastCSGPrimitive())
			{
				return true;
			}
		}

		if (Stage.IsValid() && Stage->HasActiveCSGSession())
		{
			if (EventArgs.Key == EKeys::Enter)
			{
				// No-op until the latest recompute settles; committing a stale
				// preview would apply geometry the user is not looking at.
				Stage->CommitCSGSession();
				return true;
			}

			if (EventArgs.Key == EKeys::Escape)
			{
				// Non-destructive model: cuts are elements, not proposals.
				// Esc only dismisses the selection; deleting cutters (or
				// undo) is how cuts are removed.
				Stage->DeselectAllStageComponentsForUI();
				return true;
			}
		}

		if (Stage.IsValid() && Stage->HasAnyActiveStageTool())
		{
			if (EventArgs.Key == EKeys::Enter)
			{
				Stage->EndActiveEditTool(/*bAccept*/ true);
				return true;
			}

			if (EventArgs.Key == EKeys::Escape)
			{
				Stage->EndActiveEditTool(/*bAccept*/ false);
				return true;
			}
		}

		if (Stage.IsValid()
			&& !Stage->HasAnyActiveStageTool()
			&& Stage->HasSelectedCSGPrimitive())
		{
			if (EventArgs.Key == EKeys::W)
			{
				return Stage->SetCSGTransformMode(EUbstreamCSGTransformMode::Move);
			}
			if (EventArgs.Key == EKeys::E)
			{
				return Stage->SetCSGTransformMode(EUbstreamCSGTransformMode::Rotate);
			}
			if (EventArgs.Key == EKeys::R)
			{
				return Stage->SetCSGTransformMode(EUbstreamCSGTransformMode::Scale);
			}
			if (EventArgs.Key == EKeys::Tab)
			{
				return Stage->SelectNextCSGPrimitiveForEditing();
			}
			if (EventArgs.Key == EKeys::Delete || EventArgs.Key == EKeys::BackSpace)
			{
				return Stage->DeleteSelectedCSGPrimitives();
			}
		}

	}

	return FEditorViewportClient::InputKey(EventArgs);
}
