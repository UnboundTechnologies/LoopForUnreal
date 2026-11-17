#include "AdvancedPreviewScene.h"
#include "AssetEditorModeManager.h"
#include "Async/TaskGraphInterfaces.h"
#include "Components/DynamicMeshComponent.h"
#include "Changes/MeshVertexChange.h"
#include "Components/StaticMeshComponent.h"
#include "BaseGizmos/GizmoActor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Editing/UbstreamStageEditorMode.h"
#include "Editing/UbstreamVertexSculptTool.h"
#include "Geometry/UbstreamCSGSession.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Generators/MinimalBoxMeshGenerator.h"
#include "Generators/SphereGenerator.h"
#include "Generators/SweepGenerator.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "InteractiveToolManager.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Misc/AutomationTest.h"
#include "Selection.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "UDynamicMesh.h"
#include "UObject/UnrealType.h"
#include "Viewport/UbstreamViewportClient.h"
#include "Widgets/SUbstreamModelerViewport.h"
#include "Widgets/SUbstreamModelerWidget.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SUbstreamElementsPanel.h"
#include "Widgets/SBoxPanel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	void EnsureMultipleTriangleGroups(UE::Geometry::FDynamicMesh3& Mesh)
	{
		if (Mesh.TriangleCount() < 2)
		{
			return;
		}

		Mesh.EnableTriangleGroups();

		int32 GroupIndex = 0;
		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			Mesh.SetTriangleGroup(TriangleID, GroupIndex++ % 6);
		}
	}

	UDynamicMeshComponent* CreateStressDynamicMeshComponent(
		AActor* StageActor,
		USceneComponent* StageRoot,
		UE::Geometry::FDynamicMesh3&& Mesh,
		const FVector& RelativeLocation = FVector::ZeroVector)
	{
		UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(StageActor, NAME_None, RF_Transient);
		Component->SetFlags(RF_Transactional);
		Component->CreationMethod = EComponentCreationMethod::Instance;
		Component->GetDynamicMesh()->SetFlags(RF_Transactional);
		Component->GetDynamicMesh()->SetMesh(MoveTemp(Mesh));
		StageActor->AddInstanceComponent(Component);
		Component->AttachToComponent(StageRoot, FAttachmentTransformRules::KeepRelativeTransform);
		Component->SetRelativeLocation(RelativeLocation);
		Component->RegisterComponent();
		Component->UpdateBounds();
		return Component;
	}

	// The CSG session computes on the thread pool and completes via a queued
	// game-thread task; pump both until the preview is committable.
	bool WaitForCommittableCSGSession(SUbstreamModelerViewport& Viewport, double TimeoutSeconds = 10.0)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (Viewport.HasActiveCSGSession()
			&& !Viewport.CanCommitCSGSession()
			&& FPlatformTime::Seconds() < Deadline)
		{
			Viewport.TickCSGSession();
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.005f);
		}
		return Viewport.CanCommitCSGSession();
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUbstreamStageEditingAutomationTest,
	"Ubstream.Modeler.StageEditing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FUbstreamStageEditingAutomationTest::RunTest(const FString& Parameters)
{
	// UE 5.7 can destroy a CombinedTransformGizmoActor from delayed Slate
	// preview-world teardown after the local modeler viewport releases. The
	// CSG assertions below still prove the boolean result, cutter lifecycle,
	// and undo/redo behavior; this suppresses only that engine teardown log.
	AddExpectedError(
		TEXT("UWorld::DestroyActor: World has no context!.*CombinedTransformGizmoActor"),
		EAutomationExpectedErrorFlags::Contains,
		-1);

	// Preview worlds from earlier suite members die asynchronously; when GC
	// finally collects them the editor RESETS the transaction buffer, wiping
	// this test's undo history mid-flight (flaky full-suite failures with
	// green isolation runs). Flush the purge before any transaction exists.
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	if (GEditor && GEditor->Trans)
	{
		GEditor->ResetTransaction(NSLOCTEXT("UbstreamStageEditing", "TestStartReset", "Stage editing test start"));
	}

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh is available"), CubeMesh))
	{
		return false;
	}

	const FMeshDescription* MeshDescription = CubeMesh->GetMeshDescription(0);
	if (!TestNotNull(TEXT("Cube mesh description is available"), MeshDescription))
	{
		return false;
	}

	TSharedRef<FAdvancedPreviewScene> PreviewScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
	TSharedRef<FAssetEditorModeManager> ModeManager = MakeShared<FAssetEditorModeManager>();
	ModeManager->SetPreviewScene(&PreviewScene.Get());

	UE::Geometry::FDynamicMesh3 ConvertedMesh;
	FMeshDescriptionToDynamicMesh Converter;
	Converter.Convert(MeshDescription, ConvertedMesh);
	EnsureMultipleTriangleGroups(ConvertedMesh);

	// Mirror production: staged components live on a transient owner actor so
	// tools that assume Component->GetOwner() (PlaneCut etc.) are satisfied.
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParameters.ObjectFlags = RF_Transient;
	AActor* StageActor = PreviewScene->GetWorld()->SpawnActor<AActor>(SpawnParameters);
	if (!TestNotNull(TEXT("Stage actor spawns in the preview world"), StageActor))
	{
		return false;
	}
	USceneComponent* StageRoot = NewObject<USceneComponent>(StageActor, TEXT("StageRoot"), RF_Transient);
	StageActor->SetRootComponent(StageRoot);
	StageActor->AddInstanceComponent(StageRoot);
	StageRoot->RegisterComponent();

	UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(StageActor, NAME_None, RF_Transient);
	Component->SetFlags(RF_Transactional);
	Component->CreationMethod = EComponentCreationMethod::Instance;
	Component->GetDynamicMesh()->SetFlags(RF_Transactional);
	Component->GetDynamicMesh()->SetMesh(MoveTemp(ConvertedMesh));
	StageActor->AddInstanceComponent(Component);
	Component->AttachToComponent(StageRoot, FAttachmentTransformRules::KeepRelativeTransform);
	Component->RegisterComponent();
	Component->UpdateBounds();
	UDynamicMeshComponent* ComponentB = nullptr;

	bool bPassed = true;

	{
		TSharedRef<SUbstreamModelerViewport> CSGViewport = SNew(SUbstreamModelerViewport);
		TArray<UMeshComponent*> SourceForCSGViewport;
		SourceForCSGViewport.Add(Component);

		bPassed &= TestTrue(
			TEXT("CSG viewport accepts exactly one dynamic source component"),
			CSGViewport->SetMeshComponents(
				SourceForCSGViewport,
				TEXT("Automation Cube"),
				TEXT("Automation.DynamicCube"),
				TArray<TWeakObjectPtr<AActor>>(),
				/*bFocusCamera*/ false));

		bPassed &= TestFalse(TEXT("Newly loaded source starts clean"), CSGViewport->IsSourceDirty());
		const FTransform OriginalSourceTransform = Component->GetComponentTransform();
		Component->AddWorldOffset(FVector(1.0, 0.0, 0.0));
		FPlatformProcess::Sleep(0.51f);
		bPassed &= TestTrue(TEXT("Source changes appear after the shared status interval"), CSGViewport->IsSourceDirty());
		Component->SetWorldTransform(OriginalSourceTransform);
		FPlatformProcess::Sleep(0.51f);
		bPassed &= TestFalse(TEXT("Restored source becomes clean again"), CSGViewport->IsSourceDirty());

		TSharedPtr<FUbstreamViewportClient> CSGViewportClient = CSGViewport->GetUbstreamViewportClient();
		FVector SourceOnlyWidgetLocation = FVector::ZeroVector;
		bPassed &= TestTrue(
			TEXT("Source-only CSG viewport has no generated cutter widget location"),
			!CSGViewport->GetCSGTransformWidgetLocation(SourceOnlyWidgetLocation));
		bPassed &= TestFalse(
			TEXT("Source-only CSG viewport has no selected generated cutter"),
			CSGViewport->HasSelectedCSGPrimitive());
		if (CSGViewportClient.IsValid())
		{
			CSGViewportClient->Tick(0.0f);
			bPassed &= TestFalse(
				TEXT("Source-only CSG viewport hides the transform widget"),
				CSGViewportClient->GetShowWidget());
		}

		bPassed &= TestFalse(
			TEXT("CSG operations cannot start without a cutter"),
			CSGViewport->CanStartCSGOperation());

		{
		bPassed &= TestTrue(TEXT("CSG viewport can add a generated cutter"), CSGViewport->CanAddCSGPrimitive());
		bPassed &= TestTrue(
			TEXT("CSG viewport adds a generated cutter and selects it"),
			CSGViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));
		bPassed &= TestEqual(TEXT("CSG viewport has one generated cutter"), CSGViewport->GetCSGPrimitiveCount(), 1);
		bPassed &= TestTrue(
			TEXT("Generated CSG cutter is selected after add"),
			CSGViewport->HasSelectedCSGPrimitive());

		FVector WidgetLocationBefore = FVector::ZeroVector;
		bPassed &= TestTrue(
			TEXT("Generated CSG cutter supplies a transform widget location"),
			CSGViewport->GetCSGTransformWidgetLocation(WidgetLocationBefore));
		if (CSGViewportClient.IsValid())
		{
			CSGViewportClient->Tick(0.0f);
			bPassed &= TestTrue(
				TEXT("Selected generated CSG cutter shows the transform widget"),
				CSGViewportClient->GetShowWidget());
			// The stage owns its single ITF gizmo; the legacy mode-widget
			// channel stays OFF so editor subsystems can't draw duplicates.
			bPassed &= TestFalse(
				TEXT("CSG live viewport suppresses legacy mode-widget rendering"),
				CSGViewportClient->EngineShowFlags.ModeWidgets);
			bPassed &= TestTrue(
				TEXT("CSG live viewport allows ITF widget-component rendering"),
				CSGViewportClient->EngineShowFlags.WidgetComponents);
			bPassed &= TestTrue(
				TEXT("CSG viewport can set translate widget mode while cutter is selected"),
				CSGViewportClient->CanSetWidgetMode(UE::Widget::WM_Translate));
			bPassed &= TestEqual(
				TEXT("Selected generated CSG cutter defaults to translate widget mode"),
				CSGViewportClient->GetWidgetMode(),
				UE::Widget::WM_Translate);
			CSGViewportClient->SetWidgetMode(UE::Widget::WM_Translate);
			bPassed &= TestEqual(
				TEXT("Viewport widget mode routes to CSG move mode"),
				CSGViewport->GetCSGTransformMode(),
				EUbstreamCSGTransformMode::Move);
			// (Legacy InputWidgetDelta routing removed: the ITF gizmo is the
			// only interactive manipulator; programmatic deltas are covered by
			// ApplyCSGTransformDelta below and real drags by the visual check.)
		}

		bPassed &= TestTrue(
			TEXT("Generated CSG cutter can switch to move mode"),
			CSGViewport->SetCSGTransformMode(EUbstreamCSGTransformMode::Move));
		const FVector MoveDelta(12.0, -5.0, 3.0);
		bPassed &= TestTrue(
			TEXT("Generated CSG cutter moves from viewport widget delta"),
			CSGViewport->ApplyCSGTransformDelta(MoveDelta, FRotator::ZeroRotator, FVector::ZeroVector));
		FVector WidgetLocationAfterMove = FVector::ZeroVector;
		CSGViewport->GetCSGTransformWidgetLocation(WidgetLocationAfterMove);
		bPassed &= TestTrue(
			TEXT("Generated CSG cutter widget location follows move delta"),
			WidgetLocationAfterMove.Equals(WidgetLocationBefore + MoveDelta, 0.01));

		const TArray<TObjectPtr<UMeshComponent>>& PreviewComponents = CSGViewport->GetPreviewComponents();
		UMeshComponent* SourcePreviewComponent = PreviewComponents.Num() > 0 ? PreviewComponents[0].Get() : nullptr;
		UMeshComponent* CutterComponent = PreviewComponents.Num() > 0 ? PreviewComponents.Last().Get() : nullptr;
		bPassed &= TestNotNull(TEXT("Generated CSG cutter component remains available"), CutterComponent);
		if (CutterComponent)
		{
			if (SourcePreviewComponent)
			{
				const FBox SourceBox = SourcePreviewComponent->CalcBounds(SourcePreviewComponent->GetComponentTransform()).GetBox();
				const FBox CutterBox = CutterComponent->CalcBounds(CutterComponent->GetComponentTransform()).GetBox();
				const double SourceMaxDimension = SourceBox.GetSize().GetMax();
				const double CutterMaxDimension = CutterBox.GetSize().GetMax();
				bPassed &= TestTrue(
					TEXT("Generated CSG cutter starts at an interactive size"),
					SourceBox.IsValid
					&& CutterBox.IsValid
					&& SourceMaxDimension > 0.0
					&& CutterMaxDimension > SourceMaxDimension * 0.2
					&& CutterMaxDimension < SourceMaxDimension * 0.7);
			}

			UDynamicMeshComponent* DynamicCutter = Cast<UDynamicMeshComponent>(CutterComponent);
			bPassed &= TestNotNull(TEXT("Generated CSG cutter is a dynamic mesh operand"), DynamicCutter);
			if (DynamicCutter)
			{
				bPassed &= TestEqual(
					TEXT("Generated CSG cutter uses constant visual color override"),
					DynamicCutter->GetColorOverrideMode(),
					EDynamicMeshComponentColorOverrideMode::Constant);
				bPassed &= TestTrue(
					TEXT("Generated CSG cutter displays a wireframe overlay"),
					DynamicCutter->GetEnableWireframeRenderPass());
			}

			const FQuat RotationBefore = CutterComponent->GetComponentQuat();
			bPassed &= TestTrue(
				TEXT("Generated CSG cutter can switch to rotate mode"),
				CSGViewport->SetCSGTransformMode(EUbstreamCSGTransformMode::Rotate));
			if (CSGViewportClient.IsValid())
			{
				bPassed &= TestEqual(
					TEXT("CSG rotate mode draws rotate widget"),
					CSGViewportClient->GetWidgetMode(),
					UE::Widget::WM_Rotate);
			}
			bPassed &= TestTrue(
				TEXT("Generated CSG cutter rotates from viewport widget delta"),
				CSGViewport->ApplyCSGTransformDelta(FVector::ZeroVector, FRotator(0.0, 20.0, 0.0), FVector::ZeroVector));
			bPassed &= TestFalse(
				TEXT("Generated CSG cutter rotation changes"),
				CutterComponent->GetComponentQuat().Equals(RotationBefore, KINDA_SMALL_NUMBER));

			const FVector ScaleBefore = CutterComponent->GetComponentScale();
			bPassed &= TestTrue(
				TEXT("Generated CSG cutter can switch to scale mode"),
				CSGViewport->SetCSGTransformMode(EUbstreamCSGTransformMode::Scale));
			if (CSGViewportClient.IsValid())
			{
				bPassed &= TestEqual(
					TEXT("CSG scale mode draws scale widget"),
					CSGViewportClient->GetWidgetMode(),
					UE::Widget::WM_Scale);
			}
			bPassed &= TestTrue(
				TEXT("Generated CSG cutter scales from viewport widget delta"),
				CSGViewport->ApplyCSGTransformDelta(FVector::ZeroVector, FRotator::ZeroRotator, FVector(0.25, 0.15, 0.10)));
			bPassed &= TestTrue(
				TEXT("Generated CSG cutter scale changes"),
				!CutterComponent->GetComponentScale().Equals(ScaleBefore, 0.001));
		}

		bPassed &= TestTrue(
			TEXT("CSG viewport can add a second generated cutter"),
			CSGViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Sphere));
		bPassed &= TestEqual(TEXT("CSG viewport has two generated cutters"), CSGViewport->GetCSGPrimitiveCount(), 2);
		bPassed &= TestTrue(
			TEXT("Second generated CSG cutter is selected after add"),
			CSGViewport->HasSelectedCSGPrimitive());
		bPassed &= TestEqual(
			TEXT("Second generated CSG cutter reports selected index"),
			CSGViewport->GetSelectedCSGPrimitiveIndex(),
			1);
		const auto ElementsPanel = SNew(SUbstreamElementsPanel).SoloViewport(CSGViewport);
		bPassed &= TestEqual(TEXT("Outliner starts with both cutters"), ElementsPanel->CutterRowsBox->NumSlots(), 2);
		const TSharedRef<SWidget> OriginalFirstRow = ElementsPanel->CutterRowsBox->GetChildren()->GetChildAt(0);
		ElementsPanel->Refresh();
		bPassed &= TestTrue(TEXT("Preview refresh preserves existing outliner rows"),
			ElementsPanel->CutterRowsBox->GetChildren()->GetChildAt(0) == OriginalFirstRow);
		TArray<UMeshComponent*> GeneratedCutters;
		CSGViewport->GetCSGPrimitiveComponents(GeneratedCutters);
		bPassed &= TestEqual(
			TEXT("CSG viewport exposes generated cutter components for selection"),
			GeneratedCutters.Num(),
			2);
		bPassed &= TestTrue(
			TEXT("CSG viewport cycles to the next generated cutter"),
			CSGViewport->SelectNextCSGPrimitiveForEditing());
		bPassed &= TestEqual(
			TEXT("CSG viewport cycles selected cutter index"),
			CSGViewport->GetSelectedCSGPrimitiveIndex(),
			0);
		ElementsPanel->Refresh();
		bPassed &= TestTrue(TEXT("Selection refresh preserves outliner controls"),
			ElementsPanel->CutterRowsBox->GetChildren()->GetChildAt(0) == OriginalFirstRow);
		ElementsPanel->SoloViewport.Reset();
		ElementsPanel->Refresh();
		bPassed &= TestEqual(TEXT("Detached outliner removes old rows"), ElementsPanel->CutterRowsBox->NumSlots(), 0);
		ElementsPanel->SoloViewport = CSGViewport;
		ElementsPanel->Refresh();
		bPassed &= TestEqual(TEXT("Reattached outliner restores current rows"), ElementsPanel->CutterRowsBox->NumSlots(), 2);
		if (GeneratedCutters.IsValidIndex(1))
		{
			bPassed &= TestTrue(
				TEXT("CSG viewport can explicitly select a generated cutter component"),
				CSGViewport->SelectCSGPrimitiveForEditing(GeneratedCutters[1]));
			bPassed &= TestEqual(
				TEXT("Explicit generated cutter selection restores selected index"),
				CSGViewport->GetSelectedCSGPrimitiveIndex(),
				1);
		}
		bPassed &= TestFalse(
			TEXT("CSG viewport ignores the imported source component when selecting generated cutters"),
			CSGViewport->SelectCSGPrimitiveForEditing(Component));
		bPassed &= TestEqual(
			TEXT("Ignoring source component preserves selected generated cutter"),
			CSGViewport->GetSelectedCSGPrimitiveIndex(),
			1);
		// Live session: subtract the selected cutter, wait for the async
		// preview, then commit and verify the transaction/undo contract. The
		// stage edits its CLONE of the source, so all mesh assertions must
		// read the clone (PreviewComponents[0]), never the original component.
		UDynamicMeshComponent* StageSubject = CSGViewport->GetPreviewComponents().Num() > 0
			? Cast<UDynamicMeshComponent>(CSGViewport->GetPreviewComponents()[0].Get())
			: nullptr;
		if (!TestNotNull(TEXT("Stage subject clone exists for the CSG session"), StageSubject))
		{
			return false;
		}
		const int32 TriangleCountBeforeCSG = StageSubject->GetDynamicMesh()->GetTriangleCount();
		bPassed &= TestTrue(
			TEXT("CSG viewport starts a live subtract session"),
			CSGViewport->StartCSGOperation(EUbstreamCSGOperation::Subtract));
		bPassed &= TestTrue(TEXT("CSG session is active after start"), CSGViewport->HasActiveCSGSession());
		bPassed &= TestTrue(
			TEXT("Session brush stays selected so the gizmo keeps driving the preview"),
			CSGViewport->HasSelectedCSGPrimitive());

		// Outline-only ghosts: Constant color mode would force the opaque
		// vertex-color material over the invisible override material and
		// paint a solid body that hides the live cut (the proxy prefers
		// ColorMode over OverrideRenderMaterial). Guard both channels, and
		// the analytic pick that replaces hit-proxy clicks on that
		// now-invisible body.
		{
			UDynamicMeshComponent* SessionBrush = Cast<UDynamicMeshComponent>(
				CSGViewport->GetPreviewComponents().Last().Get());
			if (TestNotNull(TEXT("Session brush component exists"), SessionBrush))
			{
				bPassed &= TestTrue(
					TEXT("Session brush leaves Constant color mode OFF (invisible ghost body)"),
					SessionBrush->GetColorOverrideMode() == EDynamicMeshComponentColorOverrideMode::None);
				bPassed &= TestTrue(
					TEXT("Session brush carries the ghost override render material"),
					SessionBrush->HasOverrideRenderMaterial(0));
				// Aim slightly off-center: a dead-center ray on a minimal box
				// runs exactly along the top face's triangle diagonal, and an
				// exact edge hit can thread through the seam. Real clicks
				// never align like that.
				const FBoxSphereBounds CutterBounds = SessionBrush->Bounds;
				const FVector PickOrigin = CutterBounds.Origin
					+ FVector(CutterBounds.BoxExtent.X * 0.25, 0.0, CutterBounds.BoxExtent.Z + 100.0);
				double PickRayParameter = TNumericLimits<double>::Max();
				UMeshComponent* PickedCutter = CSGViewport->FindCSGPrimitiveUnderRay(
					PickOrigin, FVector(0.0, 0.0, -1.0), PickRayParameter);
				// Overlapping cutters may stack here; any cutter (never the
				// subject, never null) proves invisible bodies stay pickable.
				bPassed &= TestTrue(
					TEXT("Analytic ray pick finds an invisible session cutter"),
					PickedCutter != nullptr
						&& PickedCutter != CSGViewport->GetPreviewComponents()[0].Get());
			}
			else
			{
				bPassed = false;
			}
		}
		// Deleting mid-session is allowed in the stack workflow; it would end
		// this single-cutter session, so re-add and restart for the next
		// assertions instead of exercising it here (covered separately).

		// Switching the operation retargets the running session in place.
		bPassed &= TestTrue(
			TEXT("Operation switch retargets the running session"),
			CSGViewport->StartCSGOperation(EUbstreamCSGOperation::Union));
		bPassed &= TestTrue(
			TEXT("Session remains active across an operation switch"),
			CSGViewport->HasActiveCSGSession());
		bPassed &= TestTrue(
			TEXT("Switch back to subtract retargets again"),
			CSGViewport->StartCSGOperation(EUbstreamCSGOperation::Subtract));

		// Dragging the brush mid-session keeps latest-wins recompute flowing.
		bPassed &= TestTrue(
			TEXT("Session brush accepts a transform delta"),
			CSGViewport->ApplyCSGTransformDelta(FVector(2.0, 0.0, 0.0), FRotator::ZeroRotator, FVector::ZeroVector));

		bPassed &= TestTrue(
			TEXT("CSG session produces a committable preview"),
			WaitForCommittableCSGSession(CSGViewport.Get()));

		CSGViewport->CommitCSGSession();
		bPassed &= TestFalse(TEXT("CSG session ends after commit"), CSGViewport->HasActiveCSGSession());
		// Accept bakes the whole live stack: every cutter that participated
		// is consumed.
		bPassed &= TestEqual(
			TEXT("Committed CSG operation consumes the whole stack"),
			CSGViewport->GetCSGPrimitiveCount(),
			0);
		bPassed &= TestTrue(
			TEXT("Committed CSG subtract changes the staged mesh"),
			StageSubject->GetDynamicMesh()->GetTriangleCount() != TriangleCountBeforeCSG);
		bPassed &= TestTrue(
			TEXT("Committed CSG subtract keeps the staged mesh non-empty"),
			StageSubject->GetDynamicMesh()->GetTriangleCount() > 0);
		bPassed &= TestTrue(
			TEXT("Committed CSG marks uncommitted stage edits"),
			CSGViewport->HasUncommittedStageEdits());
		const int32 TriangleCountAfterCommit = StageSubject->GetDynamicMesh()->GetTriangleCount();

		if (GEditor)
		{
			bPassed &= TestTrue(TEXT("Undo transaction succeeds after CSG commit"), GEditor->UndoTransaction());
			bPassed &= TestEqual(
				TEXT("Undo restores the pre-CSG triangle count"),
				StageSubject->GetDynamicMesh()->GetTriangleCount(),
				TriangleCountBeforeCSG);
			bPassed &= TestTrue(TEXT("Redo transaction succeeds after CSG undo"), GEditor->RedoTransaction());
			bPassed &= TestEqual(
				TEXT("Redo reapplies the committed CSG result"),
				StageSubject->GetDynamicMesh()->GetTriangleCount(),
				TriangleCountAfterCommit);
		}

		// Cancel path: the live preview carves the subject in place, so cancel
		// must restore exactly the pre-session mesh and keep the cutter.
		// The stack was consumed on commit; stage a fresh cutter for the
		// cancel path.
		bPassed &= TestTrue(
			TEXT("Fresh cutter stages for the cancel path"),
			CSGViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));
		const int32 TriangleCountBeforeSecondSession = StageSubject->GetDynamicMesh()->GetTriangleCount();
		bPassed &= TestTrue(
			TEXT("CSG viewport starts a second session for the cancel path"),
			CSGViewport->StartCSGOperation(EUbstreamCSGOperation::Subtract));
		WaitForCommittableCSGSession(CSGViewport.Get());
		const int32 CutterCountBeforeCancel = CSGViewport->GetCSGPrimitiveCount();
		CSGViewport->CancelCSGSession();
		bPassed &= TestFalse(TEXT("CSG session ends after cancel"), CSGViewport->HasActiveCSGSession());
		bPassed &= TestEqual(
			TEXT("Cancel restores the pre-session mesh"),
			StageSubject->GetDynamicMesh()->GetTriangleCount(),
			TriangleCountBeforeSecondSession);
		bPassed &= TestEqual(
			TEXT("Cancelled CSG session keeps its cutter"),
			CSGViewport->GetCSGPrimitiveCount(),
			CutterCountBeforeCancel);

		const int32 CutterCountBeforeSelectedDelete = CSGViewport->GetCSGPrimitiveCount();
		bPassed &= TestTrue(
			TEXT("Generated CSG cutter deletes through selected-cutter delete"),
			CSGViewport->DeleteSelectedCSGPrimitives());
		bPassed &= TestEqual(
			TEXT("Selected CSG cutter delete removes one cutter"),
			CSGViewport->GetCSGPrimitiveCount(),
			FMath::Max(0, CutterCountBeforeSelectedDelete - 1));
		if (CSGViewport->GetCSGPrimitiveCount() > 0)
		{
			bPassed &= TestTrue(
				TEXT("Remaining CSG cutters clear through clear all"),
				CSGViewport->ClearCSGPrimitives());
		}
		bPassed &= TestEqual(
			TEXT("Generated CSG cutter count is empty after cleanup"),
			CSGViewport->GetCSGPrimitiveCount(),
			0);
		bPassed &= TestFalse(
			TEXT("CSG viewport has no selected generated cutter after cleanup"),
			CSGViewport->HasSelectedCSGPrimitive());
		if (CSGViewportClient.IsValid())
		{
			// Chrome only while there is something to drag: after cutter
			// cleanup the actual cutter gizmo must have been destroyed.
			CSGViewportClient->Tick(0.0f);
			bPassed &= TestFalse(
				TEXT("No transform widget lingers after cutter cleanup"),
				CSGViewport->StageTransformGizmo.IsValid());
		}
		}


		// Professional stack: multiple cutters live at once, each with its own
		// operation, all baked by one Accept.
		{
			UDynamicMeshComponent* StackSubject = CSGViewport->GetPreviewComponents().Num() > 0
				? Cast<UDynamicMeshComponent>(CSGViewport->GetPreviewComponents()[0].Get())
				: nullptr;
			if (!TestNotNull(TEXT("Stack subject clone exists"), StackSubject))
			{
				return false;
			}
			const int32 TrianglesBeforeStack = StackSubject->GetDynamicMesh()->GetTriangleCount();
			bPassed &= TestTrue(TEXT("Stack cutter A adds"), CSGViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));
			bPassed &= TestTrue(TEXT("Stack session starts on A"), CSGViewport->StartCSGOperation(EUbstreamCSGOperation::Subtract));

			bPassed &= TestTrue(TEXT("Stack cutter B adds mid-session"), CSGViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Sphere));
			bPassed &= TestTrue(
				TEXT("Stack session restarts with B as Union"),
				CSGViewport->StartCSGOperation(EUbstreamCSGOperation::Union));
			bPassed &= TestEqual(
				TEXT("Selected cutter op reads back Union"),
				CSGViewport->GetSelectedCSGOperation(), EUbstreamCSGOperation::Union);
			bPassed &= TestTrue(
				TEXT("Both cutters staged in the live stack"),
				CSGViewport->GetCSGPrimitiveCount() == 2 && CSGViewport->HasActiveCSGSession());

			// Let the full chain land once so the prefix cache is warm.
			bPassed &= TestTrue(TEXT("Initial stack preview lands"), WaitForCommittableCSGSession(CSGViewport.Get()));

			// Move B away from A so both ops contribute distinct geometry.
			bPassed &= TestTrue(
				TEXT("Stack cutter B moves"),
				CSGViewport->ApplyCSGTransformDelta(FVector(30.0, 25.0, 10.0), FRotator::ZeroRotator, FVector::ZeroVector));

			bPassed &= TestTrue(TEXT("Stack preview becomes committable"), WaitForCommittableCSGSession(CSGViewport.Get()));
			// Chain-prefix caching: editing the LAST cutter must not recompute
			// the whole chain - the compute starts at that cutter's index.
			if (FUbstreamCSGSession* Session = CSGViewport->GetCSGSessionForTesting())
			{
				bPassed &= TestEqual(
					TEXT("Moving the last cutter recomputes only its chain suffix"),
					Session->GetLastComputeStartIndexForTesting(), 1);
			}
			CSGViewport->CommitCSGSession();
			bPassed &= TestEqual(
				TEXT("Accept consumes the whole stack"),
				CSGViewport->GetCSGPrimitiveCount(), 0);
			bPassed &= TestTrue(
				TEXT("Stack commit changes the mesh"),
				StackSubject->GetDynamicMesh()->GetTriangleCount() != TrianglesBeforeStack);
		}

		// Click-to-place: a ray toward the subject plants a cutter exactly at
		// the surface hit point and starts the live subtract there.
		{
			const FVector RayOrigin(-500.0, 0.0, 0.0);
			const FVector RayDirection(1.0, 0.0, 0.0);
			FVector SurfaceHit = FVector::ZeroVector;
			bPassed &= TestTrue(
				TEXT("Raycast hits the staged subject"),
				CSGViewport->RaycastSourcePreview(RayOrigin, RayDirection, SurfaceHit));

			CSGViewport->ArmCSGPlacement(EUbstreamCSGPrimitiveType::Box);
			bPassed &= TestTrue(TEXT("Cutter placement arms"), CSGViewport->HasPendingCSGPlacement());
			bPassed &= TestTrue(
				TEXT("Armed click places a cutter on the model"),
				CSGViewport->PlacePendingCSGPrimitiveAtRay(RayOrigin, RayDirection));
			bPassed &= TestFalse(TEXT("Placement disarms after the click"), CSGViewport->HasPendingCSGPlacement());
			bPassed &= TestTrue(TEXT("Placement starts a live subtract"), CSGViewport->HasActiveCSGSession());

			FVector PlacedLocation = FVector::ZeroVector;
			bPassed &= TestTrue(
				TEXT("Placed cutter has a widget location"),
				CSGViewport->GetCSGTransformWidgetLocation(PlacedLocation));
			bPassed &= TestTrue(
				FString::Printf(TEXT("Placed cutter sits at the surface hit point (%.1f away)"),
					FVector::Distance(PlacedLocation, SurfaceHit)),
				FVector::Distance(PlacedLocation, SurfaceHit) < 1.0);
			WaitForCommittableCSGSession(CSGViewport.Get());
			CSGViewport->CancelCSGSession();
		}

		// Duplicate: Ctrl+D behavior clones the selected cutter and starts a
		// live subtract on the copy.
		{
			const int32 CutterCountBeforeDuplicate = CSGViewport->GetCSGPrimitiveCount();
			bPassed &= TestTrue(
				TEXT("Selected cutter duplicates"),
				CSGViewport->DuplicateSelectedCSGPrimitive());
			bPassed &= TestEqual(
				TEXT("Duplicate adds one cutter"),
				CSGViewport->GetCSGPrimitiveCount(), CutterCountBeforeDuplicate + 1);
			bPassed &= TestTrue(TEXT("Duplicate starts a live subtract"), CSGViewport->HasActiveCSGSession());
			WaitForCommittableCSGSession(CSGViewport.Get());
			CSGViewport->CancelCSGSession();
		}

		// Repeat: adds another cutter of the last type/size immediately and
		// starts the live subtract.
		{
			const int32 CutterCountBeforeRepeat = CSGViewport->GetCSGPrimitiveCount();
			bPassed &= TestTrue(TEXT("Repeat adds another cutter"), CSGViewport->RepeatLastCSGPrimitive());
			bPassed &= TestEqual(
				TEXT("Repeat adds exactly one cutter"),
				CSGViewport->GetCSGPrimitiveCount(), CutterCountBeforeRepeat + 1);
			bPassed &= TestTrue(TEXT("Repeat starts a live subtract"), CSGViewport->HasActiveCSGSession());
			WaitForCommittableCSGSession(CSGViewport.Get());
			CSGViewport->CancelCSGSession();
		}

		// Web-editor selection grammar: additive select accumulates, toggling
		// removes, and group delete consumes the whole selection.
		{
			CSGViewport->ClearCSGPrimitives();
			bPassed &= TestTrue(TEXT("Multi-select cutter A adds"), CSGViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));
			bPassed &= TestTrue(TEXT("Multi-select cutter B adds"), CSGViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Sphere));

			TArray<UMeshComponent*> AllCutters;
			CSGViewport->GetCSGPrimitiveComponents(AllCutters);
			if (TestEqual(TEXT("Two cutters staged for multi-select"), AllCutters.Num(), 2))
			{
				bPassed &= TestTrue(
					TEXT("Plain select replaces"),
					CSGViewport->SelectCSGPrimitiveForEditing(AllCutters[0]));
				bPassed &= TestTrue(
					TEXT("Additive select accumulates"),
					CSGViewport->SelectCSGPrimitiveForEditing(AllCutters[1], /*bAddToSelection*/ true));
				TArray<UMeshComponent*> Selected;
				CSGViewport->GetSelectedCSGPrimitiveComponents(Selected);
				bPassed &= TestEqual(TEXT("Both cutters selected"), Selected.Num(), 2);

				bPassed &= TestTrue(
					TEXT("Additive select toggles off"),
					CSGViewport->SelectCSGPrimitiveForEditing(AllCutters[1], /*bAddToSelection*/ true));
				Selected.Reset();
				CSGViewport->GetSelectedCSGPrimitiveComponents(Selected);
				bPassed &= TestEqual(TEXT("Toggle removed one cutter from the selection"), Selected.Num(), 1);

				bPassed &= TestTrue(
					TEXT("Re-adding the second cutter to the selection"),
					CSGViewport->SelectCSGPrimitiveForEditing(AllCutters[1], /*bAddToSelection*/ true));
				bPassed &= TestTrue(
					TEXT("Group delete removes the whole selection"),
					CSGViewport->DeleteSelectedCSGPrimitives());
				bPassed &= TestEqual(
					TEXT("No cutters remain after group delete"),
					CSGViewport->GetCSGPrimitiveCount(), 0);

				CSGViewport->DeselectAllStageComponentsForUI();
				bPassed &= TestFalse(
					TEXT("Deselect-all clears the cutter selection"),
					CSGViewport->HasSelectedCSGPrimitive());
			}
		}

		// Undo/redo across the non-destructive stack: add, op change, and
		// delete are all transactions; resurrected cutters keep their mesh.
		{
			CSGViewport->ClearCSGPrimitives();
			bPassed &= TestTrue(TEXT("Undo test cutter adds"), CSGViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));
			bPassed &= TestEqual(TEXT("One cutter before undo"), CSGViewport->GetCSGPrimitiveCount(), 1);

			GEditor->UndoTransaction();
			bPassed &= TestEqual(TEXT("Undo removes the added cutter"), CSGViewport->GetCSGPrimitiveCount(), 0);
			GEditor->RedoTransaction();
			bPassed &= TestEqual(TEXT("Redo restores the added cutter"), CSGViewport->GetCSGPrimitiveCount(), 1);

			TArray<UMeshComponent*> UndoCutters;
			CSGViewport->GetCSGPrimitiveComponents(UndoCutters);
			UDynamicMeshComponent* UndoCutter = UndoCutters.Num() > 0 ? Cast<UDynamicMeshComponent>(UndoCutters[0]) : nullptr;
			if (TestNotNull(TEXT("Redone cutter is a dynamic mesh"), UndoCutter))
			{
				bPassed &= TestTrue(
					TEXT("Redone cutter keeps its geometry"),
					UndoCutter->GetDynamicMesh()->GetTriangleCount() > 0);

				// Legacy scale has to be one real undo step, just like numeric edits.
				CSGViewport->SetCSGTransformMode(EUbstreamCSGTransformMode::Scale);
				GEditor->ResetTransaction(NSLOCTEXT("UbstreamStageEditing", "ScaleTestReset", "Scale transaction test"));
				const FVector BeforeScale = UndoCutter->GetComponentScale();
				FVector Drag = FVector::ZeroVector;
				FRotator Rotation = FRotator::ZeroRotator;
				FVector ScaleDelta(0.1, 0, 0);
				const auto Client = CSGViewport->GetUbstreamViewportClient();
				bPassed &= TestTrue(TEXT("Legacy scale handles a delta"), Client->InputWidgetDelta(nullptr, EAxisList::X, Drag, Rotation, ScaleDelta));
				Client->TrackingStopped();
				const FVector AfterScale = UndoCutter->GetComponentScale();
				bPassed &= TestFalse(TEXT("Legacy scale changes the cutter"), BeforeScale.Equals(AfterScale));
				GEditor->UndoTransaction();
				bPassed &= TestTrue(TEXT("One undo restores legacy scale"), IsValid(UndoCutter) && UndoCutter->GetComponentScale().Equals(BeforeScale));
				GEditor->RedoTransaction();
				bPassed &= TestTrue(TEXT("Redo restores legacy scale"), IsValid(UndoCutter) && UndoCutter->GetComponentScale().Equals(AfterScale));
				CSGViewport->SetCSGTransformMode(EUbstreamCSGTransformMode::Move);

				CSGViewport->DeselectAllStageComponentsForUI();
				const bool bWasLive = CSGViewport->HasActiveCSGSession();
				bPassed &= TestFalse(TEXT("Delete without selection does nothing"), CSGViewport->DeleteSelectedCSGPrimitives());
				bPassed &= TestEqual(TEXT("Empty delete preserves live session"), CSGViewport->HasActiveCSGSession(), bWasLive);
				CSGViewport->SelectCSGPrimitiveForEditing(UndoCutter);

				// Op change is one undo step (stored as component tags).
				bPassed &= TestTrue(
					TEXT("Undo test cutter selects"),
					CSGViewport->SelectCSGPrimitiveForEditing(UndoCutter));
				bPassed &= TestTrue(
					TEXT("Undo test op changes to Union"),
					CSGViewport->StartCSGOperation(EUbstreamCSGOperation::Union));
				bPassed &= TestEqual(
					TEXT("Op reads back Union"),
					CSGViewport->GetCSGPrimitiveOperation(UndoCutter), EUbstreamCSGOperation::Union);
				GEditor->UndoTransaction();
				bPassed &= TestEqual(
					TEXT("Undo restores the Subtract operation"),
					CSGViewport->GetCSGPrimitiveOperation(UndoCutter), EUbstreamCSGOperation::Subtract);

				// Delete + undo: the cutter returns WITH its mesh (explicit
				// mesh-restoration record; the transaction system skips
				// UDynamicMesh data).
				const int32 TrianglesBeforeDelete = UndoCutter->GetDynamicMesh()->GetTriangleCount();
				bPassed &= TestTrue(
					TEXT("Undo test cutter deletes"),
					CSGViewport->DeleteSelectedCSGPrimitives());
				bPassed &= TestEqual(TEXT("Cutter gone after delete"), CSGViewport->GetCSGPrimitiveCount(), 0);
				GEditor->UndoTransaction();
				bPassed &= TestEqual(TEXT("Undo resurrects the deleted cutter"), CSGViewport->GetCSGPrimitiveCount(), 1);
				TArray<UMeshComponent*> ResurrectedCutters;
				CSGViewport->GetCSGPrimitiveComponents(ResurrectedCutters);
				UDynamicMeshComponent* Resurrected = ResurrectedCutters.Num() > 0
					? Cast<UDynamicMeshComponent>(ResurrectedCutters[0]) : nullptr;
				if (TestNotNull(TEXT("Resurrected cutter exists"), Resurrected))
				{
					bPassed &= TestEqual(
						TEXT("Resurrected cutter keeps its geometry"),
						Resurrected->GetDynamicMesh()->GetTriangleCount(), TrianglesBeforeDelete);
				}
			}
			TArray<UMeshComponent*> BeforeClear;
			CSGViewport->GetCSGPrimitiveComponents(BeforeClear);
			const int32 BeforeClearCount = BeforeClear.Num();
			bPassed &= TestTrue(TEXT("Clear cutters succeeds"), CSGViewport->ClearCSGPrimitives());
			bPassed &= TestEqual(TEXT("Clear removes all cutters"), CSGViewport->GetCSGPrimitiveCount(), 0);
			GEditor->UndoTransaction();
			bPassed &= TestEqual(TEXT("Undo Clear restores cutters"), CSGViewport->GetCSGPrimitiveCount(), BeforeClearCount);
			TArray<UMeshComponent*> AfterClearUndo;
			CSGViewport->GetCSGPrimitiveComponents(AfterClearUndo);
			for (UMeshComponent* Cutter : AfterClearUndo)
			{
				auto* Dynamic = Cast<UDynamicMeshComponent>(Cutter);
				bPassed &= TestTrue(TEXT("Undo Clear restores cutter mesh"), Dynamic && Dynamic->GetDynamicMesh()->GetTriangleCount() > 0);
			}
			GEditor->RedoTransaction();
			bPassed &= TestEqual(TEXT("Redo Clear removes cutters"), CSGViewport->GetCSGPrimitiveCount(), 0);
			CSGViewport->ClearCSGPrimitives();
		}

		// Reshaping: per-axis dimensions turn a cube into a slab (and undo
		// restores it).
		{
			CSGViewport->ClearCSGPrimitives();
			bPassed &= TestTrue(TEXT("Reshape cutter adds"), CSGViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));
			FVector DimensionsBefore = FVector::ZeroVector;
			bPassed &= TestTrue(
				TEXT("Cutter dimensions readable"),
				CSGViewport->GetSelectedCSGPrimitiveDimensions(DimensionsBefore));
			bPassed &= TestTrue(
				TEXT("Cutter X dimension stretches"),
				CSGViewport->SetSelectedCSGPrimitiveDimension(0, DimensionsBefore.X * 3.0));
			FVector DimensionsAfter = FVector::ZeroVector;
			CSGViewport->GetSelectedCSGPrimitiveDimensions(DimensionsAfter);
			bPassed &= TestTrue(
				FString::Printf(TEXT("X tripled, Y/Z untouched (%.1f,%.1f,%.1f -> %.1f,%.1f,%.1f)"),
					DimensionsBefore.X, DimensionsBefore.Y, DimensionsBefore.Z,
					DimensionsAfter.X, DimensionsAfter.Y, DimensionsAfter.Z),
				FMath::IsNearlyEqual(DimensionsAfter.X, DimensionsBefore.X * 3.0, 0.1)
				&& FMath::IsNearlyEqual(DimensionsAfter.Y, DimensionsBefore.Y, 0.1)
				&& FMath::IsNearlyEqual(DimensionsAfter.Z, DimensionsBefore.Z, 0.1));

			GEditor->UndoTransaction();
			CSGViewport->GetSelectedCSGPrimitiveDimensions(DimensionsAfter);
			bPassed &= TestTrue(
				TEXT("Undo restores the original dimensions"),
				FMath::IsNearlyEqual(DimensionsAfter.X, DimensionsBefore.X, 0.1));
			CSGViewport->ClearCSGPrimitives();
		}

		// Multiple intersects crop the SAME base: two disjoint intersect
		// shapes must keep BOTH chunks (naive chaining would empty the mesh).
		{
			CSGViewport->ClearCSGPrimitives();
			bPassed &= TestTrue(TEXT("Intersect A adds"), CSGViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));
			bPassed &= TestTrue(TEXT("Intersect A goes live"), CSGViewport->StartCSGOperation(EUbstreamCSGOperation::Intersect));
			bPassed &= TestTrue(TEXT("Intersect B adds"), CSGViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));
			bPassed &= TestTrue(TEXT("Intersect B goes live"), CSGViewport->StartCSGOperation(EUbstreamCSGOperation::Intersect));
			// Separate the two crop regions to opposite corners.
			bPassed &= TestTrue(TEXT("Intersect B moves away"),
				CSGViewport->ApplyCSGTransformDelta(FVector(60.0, 60.0, 0.0), FRotator::ZeroRotator, FVector::ZeroVector));
			bPassed &= TestTrue(TEXT("Multi-intersect preview lands"), WaitForCommittableCSGSession(CSGViewport.Get()));

			UDynamicMeshComponent* IntersectSubject = CSGViewport->GetPreviewComponents().Num() > 0
				? Cast<UDynamicMeshComponent>(CSGViewport->GetPreviewComponents()[0].Get())
				: nullptr;
			if (TestNotNull(TEXT("Intersect subject exists"), IntersectSubject))
			{
				bPassed &= TestTrue(
					TEXT("Two disjoint intersects keep geometry (same-base semantics)"),
					IntersectSubject->GetDynamicMesh()->GetTriangleCount() > 0);
			}

			// Per-cutter Accept: accept the OVERLAPPING intersect (A); the
			// moved-away one would crop the base to nothing and is refused
			// by the empty-result guard (also asserted).
			bPassed &= TestFalse(
				TEXT("Accepting a non-overlapping intersect is refused"),
				CSGViewport->AcceptSelectedCutter());
			TArray<UMeshComponent*> IntersectCutters;
			CSGViewport->GetCSGPrimitiveComponents(IntersectCutters);
			if (IntersectCutters.Num() > 0)
			{
				CSGViewport->SelectCSGPrimitiveForEditing(IntersectCutters[0]);
			}
			bPassed &= TestTrue(TEXT("Selected intersect is acceptable"), CSGViewport->CanAcceptSelectedCutter());
			const int32 CuttersBeforeAccept = CSGViewport->GetCSGPrimitiveCount();
			bPassed &= TestTrue(TEXT("Accepting the intersect succeeds"), CSGViewport->AcceptSelectedCutter());
			bPassed &= TestEqual(
				TEXT("Accept consumed exactly one cutter"),
				CSGViewport->GetCSGPrimitiveCount(), CuttersBeforeAccept - 1);
			bPassed &= TestTrue(
				TEXT("Survivor cutter stays live after accept"),
				CSGViewport->HasActiveCSGSession());

			// Union is also supported by per-cutter Accept: bake the added
			// geometry into the base and consume only that cutter.
			bPassed &= TestTrue(TEXT("Union op applies for accept gating"), CSGViewport->StartCSGOperation(EUbstreamCSGOperation::Union));
			bPassed &= TestTrue(TEXT("Union cutter is per-cutter acceptable"), CSGViewport->CanAcceptSelectedCutter());
			const int32 CuttersBeforeUnionAccept = CSGViewport->GetCSGPrimitiveCount();
			bPassed &= TestTrue(TEXT("Accepting the union succeeds"), CSGViewport->AcceptSelectedCutter());
			bPassed &= TestEqual(TEXT("Union accept consumes only its cutter"),
				CSGViewport->GetCSGPrimitiveCount(), CuttersBeforeUnionAccept - 1);

			CSGViewport->ClearCSGPrimitives();
		}

		// Use a fresh source: previous accepts changed the mesh and its bounds.
		bPassed &= TestTrue(TEXT("Reload source for empty-result test"), CSGViewport->ResyncFromSource(true));
		// Empty intersections retain the last visible mesh and cannot be baked.
		CSGViewport->ClearCSGPrimitives();
		bPassed &= TestTrue(TEXT("Empty-result cutter adds"), CSGViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));
		FVector ValidCutterLocation;
		FRotator ValidCutterRotation;
		double ValidCutterSize;
		CSGViewport->GetSelectedCSGPrimitiveTransform(ValidCutterLocation, ValidCutterRotation, ValidCutterSize);
		CSGViewport->SetSelectedCSGPrimitiveLocation(FVector(100000, 100000, 100000));
		CSGViewport->StartCSGOperation(EUbstreamCSGOperation::Intersect);
		const double EmptyDeadline = FPlatformTime::Seconds() + 10.0;
		do
		{
			CSGViewport->TickCSGSession();
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.005f);
		} while (CSGViewport->IsCSGSessionComputing() && FPlatformTime::Seconds() < EmptyDeadline);
		bPassed &= TestFalse(TEXT("Empty preview finishes computing"), CSGViewport->IsCSGSessionComputing());
		bPassed &= TestFalse(TEXT("Whole-stack commit rejects an empty result"), CSGViewport->CanCommitCSGSession());
		FString EmptyApplyError;
		bPassed &= TestFalse(TEXT("Apply rejects a completed empty cut"), CSGViewport->ApplyEditsToSource(EmptyApplyError));
		bPassed &= TestEqual(TEXT("Apply explains the actual empty-cut problem"), EmptyApplyError, CSGViewport->GetCSGSessionMessage());
		bPassed &= TestFalse(TEXT("Finished empty cut does not tell the user to wait"), EmptyApplyError.Contains(TEXT("still computing")));
		CSGViewport->CommitCSGSession();
		bPassed &= TestTrue(TEXT("Refused empty commit keeps the session editable"), CSGViewport->HasActiveCSGSession());
		auto* EmptySubject = Cast<UDynamicMeshComponent>(CSGViewport->GetPreviewComponents()[0]);
		bPassed &= TestTrue(TEXT("Empty preview retains visible subject geometry"), EmptySubject && EmptySubject->GetDynamicMesh()->GetTriangleCount() > 0);
		CSGViewport->SetSelectedCSGPrimitiveLocation(ValidCutterLocation);
		bPassed &= TestTrue(TEXT("Moving the cutter recovers a valid result"), WaitForCommittableCSGSession(*CSGViewport));
		CSGViewport->ClearObjects();
	}

	{
		TUniquePtr<UE::Geometry::FMinimalBoxMeshGenerator> LongSourceGenerator = MakeUnique<UE::Geometry::FMinimalBoxMeshGenerator>();
		LongSourceGenerator->Box = UE::Geometry::FOrientedBox3d(
			FVector3d::Zero(),
			FVector3d(900.0, 20.0, 25.0));
		LongSourceGenerator->Generate();
		UDynamicMeshComponent* LongSourceComponent = CreateStressDynamicMeshComponent(
			StageActor,
			StageRoot,
			UE::Geometry::FDynamicMesh3(LongSourceGenerator.Get()),
			FVector(2500.0, 0.0, 0.0));

		TSharedRef<SUbstreamModelerViewport> LongSourceCSGViewport = SNew(SUbstreamModelerViewport);
		TArray<UMeshComponent*> LongSourceForCSGViewport;
		LongSourceForCSGViewport.Add(LongSourceComponent);
		bPassed &= TestTrue(
			TEXT("CSG viewport accepts a long thin source component"),
			LongSourceCSGViewport->SetMeshComponents(
				LongSourceForCSGViewport,
				TEXT("Automation Long Thin Box"),
				TEXT("Automation.DynamicLongThinBox"),
				TArray<TWeakObjectPtr<AActor>>{StageActor},
				/*bFocusCamera*/ false));
		bPassed &= TestTrue(TEXT("Component subject resync succeeds"), LongSourceCSGViewport->ResyncFromSource(true));
		bPassed &= TestEqual(TEXT("Resync preserves the subject label"), LongSourceCSGViewport->GetSoloMeshLabel(), FString(TEXT("Automation Long Thin Box")));
		bPassed &= TestEqual(TEXT("Resync preserves the source path"), LongSourceCSGViewport->GetSoloMeshPath(), FString(TEXT("Automation.DynamicLongThinBox")));
		bPassed &= TestTrue(TEXT("Resync preserves source actors"), LongSourceCSGViewport->GetSourceActors().Contains(StageActor));
		bPassed &= TestTrue(
			TEXT("Long thin CSG viewport adds a generated cutter"),
			LongSourceCSGViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));

		const TArray<TObjectPtr<UMeshComponent>>& LongPreviewComponents = LongSourceCSGViewport->GetPreviewComponents();
		UMeshComponent* LongCutterComponent = LongPreviewComponents.Num() > 0 ? LongPreviewComponents.Last().Get() : nullptr;
		bPassed &= TestNotNull(TEXT("Long thin generated CSG cutter component remains available"), LongCutterComponent);
		if (LongSourceComponent && LongCutterComponent)
		{
			LongSourceComponent->UpdateBounds();
			const FBox LongSourceBox = LongSourceComponent->Bounds.GetBox();
			const FBox LongCutterBox = LongCutterComponent->CalcBounds(LongCutterComponent->GetComponentTransform()).GetBox();
			const FVector LongSourceSize = LongSourceBox.GetSize();
			const double LongSourceMinDimension = FMath::Min3(LongSourceSize.X, LongSourceSize.Y, LongSourceSize.Z);
			const double LongSourceMaxDimension = FMath::Max3(LongSourceSize.X, LongSourceSize.Y, LongSourceSize.Z);
			const double LongSourceMedianDimension = LongSourceSize.X + LongSourceSize.Y + LongSourceSize.Z - LongSourceMinDimension - LongSourceMaxDimension;
			const double LongCutterMaxDimension = LongCutterBox.GetSize().GetMax();
			bPassed &= TestTrue(
				TEXT("Generated CSG cutter stays compact on a long thin source"),
				LongSourceBox.IsValid
				&& LongCutterBox.IsValid
				&& LongSourceMedianDimension > 0.0
				&& LongSourceMaxDimension > LongSourceMedianDimension
				&& LongCutterMaxDimension > LongSourceMedianDimension * 0.3
				&& LongCutterMaxDimension < LongSourceMedianDimension * 0.6
				&& LongCutterMaxDimension < LongSourceMaxDimension * 0.05);
		}

		LongSourceCSGViewport->ClearObjects();
	}

	// Scale invariance: the whole cutter flow must behave identically on a
	// tiny model and a huge one - proportional cutter, live session, commit.
	auto RunScaledSubjectCase = [&](double Scale, const TCHAR* CaseLabel) -> bool
	{
		bool bCasePassed = true;

		UE::Geometry::FDynamicMesh3 ScaledMesh;
		Converter.Convert(MeshDescription, ScaledMesh);
		EnsureMultipleTriangleGroups(ScaledMesh);
		for (const int32 VertexId : ScaledMesh.VertexIndicesItr())
		{
			ScaledMesh.SetVertex(VertexId, ScaledMesh.GetVertex(VertexId) * Scale);
		}
		UDynamicMeshComponent* ScaledSubject = CreateStressDynamicMeshComponent(
			StageActor, StageRoot, MoveTemp(ScaledMesh), FVector(5000.0, 5000.0, 0.0));

		TSharedRef<SUbstreamModelerViewport> ScaledViewport = SNew(SUbstreamModelerViewport);
		TArray<UMeshComponent*> ScaledSource;
		ScaledSource.Add(ScaledSubject);
		bCasePassed &= TestTrue(
			FString::Printf(TEXT("%s viewport accepts the subject"), CaseLabel),
			ScaledViewport->SetMeshComponents(
				ScaledSource,
				FString::Printf(TEXT("Automation %s Cube"), CaseLabel),
				FString::Printf(TEXT("Automation.%sCube"), CaseLabel),
				TArray<TWeakObjectPtr<AActor>>(),
				/*bFocusCamera*/ false));

		bCasePassed &= TestTrue(
			FString::Printf(TEXT("%s cutter can be added"), CaseLabel),
			ScaledViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));

		const TArray<TObjectPtr<UMeshComponent>>& ScaledPreviews = ScaledViewport->GetPreviewComponents();
		UMeshComponent* ScaledCutter = ScaledPreviews.Num() > 0 ? ScaledPreviews.Last().Get() : nullptr;
		UDynamicMeshComponent* ScaledStageSubject = ScaledPreviews.Num() > 0
			? Cast<UDynamicMeshComponent>(ScaledPreviews[0].Get())
			: nullptr;
		if (TestNotNull(*FString::Printf(TEXT("%s cutter exists"), CaseLabel), ScaledCutter)
			&& TestNotNull(*FString::Printf(TEXT("%s stage subject exists"), CaseLabel), ScaledStageSubject))
		{
			ScaledStageSubject->UpdateBounds();
			ScaledCutter->UpdateBounds();
			const double SubjectMaxDimension = ScaledStageSubject->Bounds.GetBox().GetSize().GetMax();
			const double CutterMaxDimension = ScaledCutter->Bounds.GetBox().GetSize().GetMax();
			bCasePassed &= TestTrue(
				FString::Printf(TEXT("%s cutter stays proportional (cutter %.4f vs subject %.4f)"),
					CaseLabel, CutterMaxDimension, SubjectMaxDimension),
				CutterMaxDimension > SubjectMaxDimension * 0.1
				&& CutterMaxDimension < SubjectMaxDimension * 0.6);

			const int32 TrianglesBefore = ScaledStageSubject->GetDynamicMesh()->GetTriangleCount();
			bCasePassed &= TestTrue(
				FString::Printf(TEXT("%s live subtract starts"), CaseLabel),
				ScaledViewport->StartCSGOperation(EUbstreamCSGOperation::Subtract));
			bCasePassed &= TestTrue(
				FString::Printf(TEXT("%s preview becomes committable"), CaseLabel),
				WaitForCommittableCSGSession(ScaledViewport.Get()));
			ScaledViewport->CommitCSGSession();
			bCasePassed &= TestTrue(
				FString::Printf(TEXT("%s commit changes the mesh"), CaseLabel),
				ScaledStageSubject->GetDynamicMesh()->GetTriangleCount() != TrianglesBefore);
			bCasePassed &= TestTrue(
				FString::Printf(TEXT("%s commit keeps the mesh non-empty"), CaseLabel),
				ScaledStageSubject->GetDynamicMesh()->GetTriangleCount() > 0);
		}

		ScaledViewport->ClearObjects();
		ScaledSubject->DestroyComponent();
		return bCasePassed;
	};

	bPassed &= RunScaledSubjectCase(0.02, TEXT("Tiny"));
	bPassed &= RunScaledSubjectCase(100.0, TEXT("Huge"));

	// Outlier immunity: generated meshes often carry stray triangles far from
	// the visible body. The cutter must size to the body, not the outlier.
	{
		UE::Geometry::FDynamicMesh3 OutlierMesh;
		Converter.Convert(MeshDescription, OutlierMesh);
		EnsureMultipleTriangleGroups(OutlierMesh);
		// A tiny degenerate-ish triangle 100 model-lengths away.
		const int32 VertexA = OutlierMesh.AppendVertex(FVector3d(10000.0, 10000.0, 10000.0));
		const int32 VertexB = OutlierMesh.AppendVertex(FVector3d(10001.0, 10000.0, 10000.0));
		const int32 VertexC = OutlierMesh.AppendVertex(FVector3d(10000.0, 10001.0, 10000.0));
		OutlierMesh.AppendTriangle(VertexA, VertexB, VertexC);
		UDynamicMeshComponent* OutlierSubject = CreateStressDynamicMeshComponent(
			StageActor, StageRoot, MoveTemp(OutlierMesh), FVector(-5000.0, -5000.0, 0.0));

		TSharedRef<SUbstreamModelerViewport> OutlierViewport = SNew(SUbstreamModelerViewport);
		TArray<UMeshComponent*> OutlierSource;
		OutlierSource.Add(OutlierSubject);
		bPassed &= TestTrue(
			TEXT("Outlier viewport accepts the subject"),
			OutlierViewport->SetMeshComponents(
				OutlierSource,
				TEXT("Automation Outlier Cube"),
				TEXT("Automation.OutlierCube"),
				TArray<TWeakObjectPtr<AActor>>(),
				/*bFocusCamera*/ false));
		bPassed &= TestTrue(
			TEXT("Outlier cutter can be added"),
			OutlierViewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));

		const TArray<TObjectPtr<UMeshComponent>>& OutlierPreviews = OutlierViewport->GetPreviewComponents();
		UMeshComponent* OutlierCutter = OutlierPreviews.Num() > 0 ? OutlierPreviews.Last().Get() : nullptr;
		if (TestNotNull(TEXT("Outlier cutter exists"), OutlierCutter))
		{
			OutlierCutter->UpdateBounds();
			const double CutterMaxDimension = OutlierCutter->Bounds.GetBox().GetSize().GetMax();
			// The cube body is ~100 units; the raw bounding box with the
			// outlier spans ~10000. Proportional-to-body means well under 100.
			bPassed &= TestTrue(
				FString::Printf(TEXT("Outlier cutter sizes to the body, not the stray triangle (%.1f units)"), CutterMaxDimension),
				CutterMaxDimension > 10.0 && CutterMaxDimension < 100.0);
		}

		OutlierViewport->ClearObjects();
		OutlierSubject->DestroyComponent();
	}


	// Undoing the automatic copy must restore the engine asset without opening
	// another transaction, replacing it again, or discarding the redo entry.
	{
		UStaticMeshComponent* EngineSource = NewObject<UStaticMeshComponent>(StageActor, NAME_None, RF_Transient | RF_Transactional);
		EngineSource->SetStaticMesh(CubeMesh);
		StageActor->AddInstanceComponent(EngineSource);
		EngineSource->AttachToComponent(StageRoot, FAttachmentTransformRules::KeepRelativeTransform);
		EngineSource->RegisterComponent();
		TSharedRef<SUbstreamModelerViewport> EngineViewport = SNew(SUbstreamModelerViewport);
		GEditor->ResetTransaction(NSLOCTEXT("UbstreamStageEditing", "CopyUndoReset", "Engine copy undo test"));
		bPassed &= TestTrue(TEXT("Engine component loads"), EngineViewport->SetMeshComponents(
			TArray<UMeshComponent*>{EngineSource}, TEXT("Engine cube"), TEXT("Automation.EngineCube"), {}));
		bPassed &= TestTrue(TEXT("Engine component enters edit mode"), EngineViewport->EnterEditMode());
		UStaticMesh* EditableCopy = EngineSource->GetStaticMesh();
		bPassed &= TestTrue(TEXT("Loading creates an editable copy"), EditableCopy && EditableCopy != CubeMesh);
		GEditor->UndoTransaction();
		bPassed &= TestEqual(TEXT("Undo restores the engine source without another copy"), EngineSource->GetStaticMesh().Get(), CubeMesh);
		GEditor->RedoTransaction();
		bPassed &= TestEqual(TEXT("Redo still restores the same editable copy"), EngineSource->GetStaticMesh().Get(), EditableCopy);

		bPassed &= TestTrue(TEXT("Viewport can queue sculpt"), EngineViewport->StartEditTool(UUbstreamStageEditorMode::VertexSculptToolIdentifier));
		AGizmoActor* ToolGizmo = EngineViewport->GetUbstreamViewportClient()->GetWorld()->SpawnActor<AGizmoActor>();
		if (TestNotNull(TEXT("Tool gizmo fixture exists"), ToolGizmo))
		{
			EngineViewport->DestroyForeignGizmoActors();
			bPassed &= TestFalse(TEXT("Cleanup preserves gizmos while a tool start is pending"), ToolGizmo->IsActorBeingDestroyed());
			EngineViewport->GetUbstreamViewportClient()->Tick(0.01f);
			EngineViewport->DestroyForeignGizmoActors();
			UEditorInteractiveToolsContext* EngineTools = EngineViewport->GetStageToolsContextForTesting();
			bPassed &= TestTrue(TEXT("Viewport tick activates the requested tool"), EngineTools && EngineTools->HasActiveTool());
			bPassed &= TestFalse(TEXT("Cleanup preserves gizmos while a tool is active"), ToolGizmo->IsActorBeingDestroyed());
			ToolGizmo->Destroy();
		}

		// Measure actual mirror copies while a real sculpt tool is idle.
		EngineViewport->SyncCaptureMirrorScene();
		UUbstreamVertexSculptTool* Sculpt = Cast<UUbstreamVertexSculptTool>(EngineViewport->GetActiveVertexSculptTool());
		bool bFoundSculptMirror = false;
		for (auto& Entry : EngineViewport->CaptureMirrors)
		{
			UDynamicMeshComponent* WorkingMesh = Cast<UDynamicMeshComponent>(Entry.Source.Get());
			UDynamicMeshComponent* MirrorMesh = Cast<UDynamicMeshComponent>(Entry.Mirror.Get());
			uint64 StableRevision = 0;
			if (!Sculpt || !WorkingMesh || !MirrorMesh || !Sculpt->TryGetStableCaptureRevision(WorkingMesh, StableRevision))
			{
				continue;
			}
			bFoundSculptMirror = true;
			int32 Copies = 0;
			const FDelegateHandle CopyHandle = MirrorMesh->OnMeshChanged.AddLambda([&Copies]() { ++Copies; });
			const double IdleStart = FPlatformTime::Seconds();
			for (int32 Index = 0; Index < 100; ++Index)
			{
				EngineViewport->SyncCaptureMirrorScene();
			}
			AddInfo(FString::Printf(TEXT("100 idle sculpt mirror syncs: %d copies, %.3f ms total"), Copies, (FPlatformTime::Seconds() - IdleStart) * 1000.0));
			bPassed &= TestEqual(TEXT("Idle sculpt captures perform no mesh copies"), Copies, 0);
			const int32 Vid = *WorkingMesh->GetMesh()->VertexIndicesItr().begin();
			const FVector3d NewPosition = WorkingMesh->GetMesh()->GetVertex(Vid) + FVector3d(0.000001, 0, 0);
			WorkingMesh->GetDynamicMesh()->EditMesh([Vid, NewPosition](UE::Geometry::FDynamicMesh3& Mesh)
			{
				Mesh.SetVertex(Vid, NewPosition);
				Mesh.EnableVertexColors(FVector3f::One());
				Mesh.SetVertexColor(Vid, FVector3f(1, 0, 0));
			});
			EngineViewport->SyncCaptureMirrorScene();
			bPassed &= TestEqual(TEXT("Notified sculpt edit copies exactly once"), Copies, 1);
			bPassed &= TestTrue(TEXT("Mirror includes sub-fingerprint sculpt edit"), MirrorMesh->GetMesh()->GetVertex(Vid).Equals(NewPosition, 1.e-9));
			bPassed &= TestTrue(TEXT("Mirror includes attribute changes"), MirrorMesh->GetMesh()->HasVertexColors() && MirrorMesh->GetMesh()->GetVertexColor(Vid).Equals(FVector3f(1, 0, 0)));
			FMeshVertexChange UndoChange;
			UndoChange.Vertices.Add(Vid);
			UndoChange.OldPositions.Add(NewPosition - FVector3d(0.000001, 0, 0));
			UndoChange.NewPositions.Add(NewPosition);
			UndoChange.Revert(WorkingMesh);
			EngineViewport->SyncCaptureMirrorScene();
			bPassed &= TestEqual(TEXT("Undo forces a mirror copy"), Copies, 2);
			bPassed &= TestTrue(TEXT("Mirror contains the reverted sculpt edit"), MirrorMesh->GetMesh()->GetVertex(Vid).Equals(UndoChange.OldPositions[0], 1.e-9));
			UndoChange.Apply(WorkingMesh);
			EngineViewport->SyncCaptureMirrorScene();
			bPassed &= TestEqual(TEXT("Redo forces a mirror copy"), Copies, 3);
			bPassed &= TestTrue(TEXT("Mirror contains the redone sculpt edit"), MirrorMesh->GetMesh()->GetVertex(Vid).Equals(NewPosition, 1.e-9));
			EngineViewport->SyncCaptureMirrorScene();
			bPassed &= TestEqual(TEXT("Mirror returns to idle after changes"), Copies, 3);
			MirrorMesh->OnMeshChanged.Remove(CopyHandle);
			break;
		}
		bPassed &= TestTrue(TEXT("Capture mirror contains the sculpt working mesh"), bFoundSculptMirror);

		EngineViewport->EndActiveEditTool(false);
		EngineViewport->ClearObjects();
		EngineSource->DestroyComponent();
	}


	// All user-facing replacement routes share one discard decision. Low-level
	// Set* methods remain usable for loading fixtures and confirmed resyncs.
	{
		TSharedRef<SUbstreamModelerWidget> Widget = SNew(SUbstreamModelerWidget);
		const auto Viewport = Widget->GetSoloViewport();
		int32 Prompts = 0;
		bool bAllowDiscard = false;
		Viewport->OnConfirmDiscardStageEdits.BindLambda([&](const FText&)
		{
			++Prompts;
			return bAllowDiscard;
		});
		bPassed &= TestTrue(TEXT("Clean workspace loads without asking"), Widget->LoadSoloMesh(CubeMesh));
		bPassed &= TestEqual(TEXT("Clean load has no discard prompt"), Prompts, 0);
		bPassed &= TestTrue(TEXT("Replacement fixture creates an editable cutter"), Viewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));
		bPassed &= TestTrue(TEXT("Live cutters count as unapplied work"), Viewport->HasUncommittedStageEdits());
		const FString OriginalLabel = Viewport->GetSoloMeshLabel();
		bPassed &= TestFalse(TEXT("Cancel protects a mesh replacement"), Widget->LoadSoloMesh(CubeMesh));
		bPassed &= TestFalse(TEXT("Cancel protects an actor replacement"), Widget->LoadSoloActors({StageActor}));
		bPassed &= TestFalse(TEXT("Cancel protects a component replacement"), Widget->LoadSoloComponents({Component}));
		bPassed &= TestFalse(TEXT("Cancel protects closing the workspace"), Widget->CanCloseWorkspace());
		const FDragDropEvent DropEvent(FPointerEvent(), FAssetDragDropOp::New(FAssetData(CubeMesh)));
		bPassed &= TestTrue(TEXT("Cancelled asset drop is consumed"), Viewport->OnDrop(FGeometry(), DropEvent).IsEventHandled());
		bPassed &= TestEqual(TEXT("Each user action asks once"), Prompts, 5);
		bPassed &= TestEqual(TEXT("Cancelled replacements retain the subject"), Viewport->GetSoloMeshLabel(), OriginalLabel);
		bPassed &= TestEqual(TEXT("Cancelled replacements retain the cutter"), Viewport->GetCSGPrimitiveCount(), 1);
		bAllowDiscard = true;
		bPassed &= TestTrue(TEXT("Confirmed replacement still loads"), Widget->LoadSoloMesh(CubeMesh));
		bPassed &= TestEqual(TEXT("Confirmed replacement prompts only once"), Prompts, 6);
		bPassed &= TestEqual(TEXT("Confirmed replacement clears the old cutter"), Viewport->GetCSGPrimitiveCount(), 0);
		bPassed &= TestTrue(TEXT("Clean workspace closes without asking"), Widget->CanCloseWorkspace());
		bPassed &= TestEqual(TEXT("Clean close has no prompt"), Prompts, 6);
		Viewport->OnConfirmDiscardStageEdits.Unbind();
		Viewport->ClearObjects();
	}

	{
		TSharedRef<SUbstreamModelerViewport> CameraSignatureViewport = SNew(SUbstreamModelerViewport);
		TArray<UMeshComponent*> SourceForCameraSignature;
		SourceForCameraSignature.Add(Component);
		bPassed &= TestTrue(
			TEXT("Camera signature viewport accepts a dynamic source component"),
			CameraSignatureViewport->SetMeshComponents(
				SourceForCameraSignature,
				TEXT("Automation Camera Signature Source"),
				TEXT("Automation.CameraSignatureSource"),
				TArray<TWeakObjectPtr<AActor>>(),
				/*bFocusCamera*/ false));

		TSharedPtr<FUbstreamViewportClient> CameraSignatureClient = CameraSignatureViewport->GetUbstreamViewportClient();
		bPassed &= TestNotNull(TEXT("Camera signature viewport client exists"), CameraSignatureClient.Get());
		if (CameraSignatureClient.IsValid())
		{
			CameraSignatureClient->SetOrbitCameraEnabled(true);
			CameraSignatureClient->SetViewLocation(FVector(-350.0, -350.0, 220.0));
			CameraSignatureClient->SetViewRotation(FRotator(-24.0, 45.0, 0.0));
			CameraSignatureClient->SetLookAtLocation(FVector::ZeroVector);
			const uint64 SignatureBeforeLookAtMove = CameraSignatureViewport->BuildSceneSignature();

			CameraSignatureClient->SetLookAtLocation(FVector(42.0, -17.0, 9.0));
			const uint64 SignatureAfterLookAtMove = CameraSignatureViewport->BuildSceneSignature();
			bPassed &= TestTrue(
				TEXT("Solo scene signature changes when the orbit-rendered camera pose changes"),
				SignatureBeforeLookAtMove != SignatureAfterLookAtMove);
		}

		CameraSignatureViewport->ClearObjects();
	}

	// Mirrors SUbstreamModelerViewport::SelectPreviewComponentsForEditing.
	ModeManager->GetSelectedComponents()->Select(Component);
	bPassed &= TestEqual(
		TEXT("Ownerless preview component is selectable in the mode manager"),
		ModeManager->GetSelectedComponents()->Num(),
		1);

	ModeManager->ActivateMode(UUbstreamStageEditorMode::EM_UbstreamStageEditorModeId);
	UUbstreamStageEditorMode* Mode = Cast<UUbstreamStageEditorMode>(
		ModeManager->GetActiveScriptableMode(UUbstreamStageEditorMode::EM_UbstreamStageEditorModeId));
	bPassed &= TestNotNull(TEXT("Stage editor mode activates on the asset editor mode manager"), Mode);

	if (Mode)
	{
		for (const FString& ToolIdentifier : {
			UUbstreamStageEditorMode::PolyEditToolIdentifier,
			UUbstreamStageEditorMode::VertexSculptToolIdentifier,
			UUbstreamStageEditorMode::SmoothToolIdentifier,
			UUbstreamStageEditorMode::RemeshToolIdentifier,
			UUbstreamStageEditorMode::DisplaceToolIdentifier })
		{
			bPassed &= TestTrue(
				FString::Printf(TEXT("%s can start for the staged component"), *ToolIdentifier),
				Mode->CanStartTool(ToolIdentifier));
		}

		// Start a warning-clean representative set through the immediate
		// base-class path (bypassing the deferred editor path, which needs
		// viewport ticks) to validate target building and Setup. PolyEdit and
		// Displace intentionally log an empty UserWarning during Setup to clear
		// their warning UI, so keep them at CanStart coverage here; the CSG
		// tools still get full setup and accept/undo/redo coverage below.
		UEditorInteractiveToolsContext* ToolsContext = Mode->GetStageToolsContext();
		bPassed &= TestNotNull(TEXT("Stage tools context exists"), ToolsContext);
		if (ToolsContext)
		{
			for (bool bImmediate : {false, true})
			{
				bPassed &= TestTrue(TEXT("Deferred sculpt request is accepted"), Mode->StartTool(UUbstreamStageEditorMode::VertexSculptToolIdentifier));
				if (bImmediate) Mode->EndActiveToolImmediate(false);
				else Mode->EndActiveTool(false);
				Mode->ModeTick(0.01f);
				ToolsContext->Tick(nullptr, 0.01f);
				bPassed &= TestFalse(TEXT("Cancelled start never activates a tool"), ToolsContext->HasActiveTool());
				bPassed &= TestFalse(TEXT("Cancelled start clears mode state"), Mode->HasActiveTool());
			}
			bPassed &= TestTrue(TEXT("A later sculpt request starts normally"), Mode->StartTool(UUbstreamStageEditorMode::VertexSculptToolIdentifier));
			Mode->ModeTick(0.01f);
			bPassed &= TestTrue(TEXT("Deferred sculpt becomes an actual ITF tool"), ToolsContext->HasActiveTool());
			bPassed &= TestEqual(TEXT("Deferred tool retains its identity"), Mode->GetActiveToolIdentifier(), UUbstreamStageEditorMode::VertexSculptToolIdentifier);
			bPassed &= TestTrue(TEXT("Active sculpt can queue a smooth tool"), Mode->StartTool(UUbstreamStageEditorMode::SmoothToolIdentifier));
			Mode->ModeTick(0.01f);
			bPassed &= TestTrue(TEXT("Tool switch keeps a real active tool"), ToolsContext->HasActiveTool());
			bPassed &= TestEqual(TEXT("Tool switch retains the replacement identity"), Mode->GetActiveToolIdentifier(), UUbstreamStageEditorMode::SmoothToolIdentifier);
			bPassed &= TestTrue(TEXT("Active tool can queue another switch"), Mode->StartTool(UUbstreamStageEditorMode::VertexSculptToolIdentifier));
			Mode->EndActiveTool(false);
			Mode->ModeTick(0.01f);
			ToolsContext->Tick(nullptr, 0.01f);
			bPassed &= TestFalse(TEXT("Cancelling a pending switch ends the old tool and cancels the new one"), ToolsContext->HasActiveTool());


			for (const FString& ToolIdentifier : {
				UUbstreamStageEditorMode::VertexSculptToolIdentifier,
				UUbstreamStageEditorMode::SmoothToolIdentifier,
				UUbstreamStageEditorMode::RemeshToolIdentifier })
			{
				const bool bStarted = ToolsContext->UInteractiveToolsContext::StartTool(EToolSide::Mouse, ToolIdentifier);
				bPassed &= TestTrue(
					FString::Printf(TEXT("%s starts and completes Setup"), *ToolIdentifier),
					bStarted);

				UInteractiveTool* ActiveTool = ToolsContext->ToolManager->GetActiveTool(EToolSide::Mouse);
				bPassed &= TestNotNull(
					*FString::Printf(TEXT("%s active tool instance exists"), *ToolIdentifier),
					ActiveTool);
				if (ActiveTool)
				{
					if (ToolIdentifier == UUbstreamStageEditorMode::VertexSculptToolIdentifier)
					{
						UUbstreamVertexSculptTool* SculptTool = Cast<UUbstreamVertexSculptTool>(ActiveTool);
						if (TestNotNull(TEXT("Stage uses the revision-aware Epic sculpt tool"), SculptTool))
						{
							uint64 Revision = 0;
							UE::Geometry::FDynamicMesh3 SculptCopy;
							bPassed &= TestTrue(TEXT("Sculpt initial revision copies"), SculptTool->CopyMeshIfChanged(Revision, SculptCopy));
							int32 EditedVertex = INDEX_NONE;
							FVector3d EditedPosition = FVector3d::Zero();
							SculptTool->UpdateToolMeshes([&](UE::Geometry::FDynamicMesh3& Mesh, int32) -> TUniquePtr<FMeshRegionChangeBase>
							{
								if (Mesh.VertexCount() > 0)
								{
									EditedVertex = *Mesh.VertexIndicesItr().begin();
									EditedPosition = Mesh.GetVertex(EditedVertex) + FVector3d(0, 0, 0.25);
									Mesh.SetVertex(EditedVertex, EditedPosition);
								}
								return nullptr;
							});
							bPassed &= TestTrue(TEXT("Completed sculpt edit copies"), SculptTool->CopyMeshIfChanged(Revision, SculptCopy));
							bPassed &= TestTrue(TEXT("Sculpt copy contains edited vertex"),
								EditedVertex != INDEX_NONE && SculptCopy.GetVertex(EditedVertex).Equals(EditedPosition));
							bPassed &= TestFalse(TEXT("Idle sculpt skips mesh copy"), SculptTool->CopyMeshIfChanged(Revision, SculptCopy));
						}
					}
					bPassed &= TestTrue(
						FString::Printf(TEXT("%s exposes tool properties for the settings panel"), *ToolIdentifier),
						ActiveTool->GetToolProperties().Num() > 0);
					ToolsContext->UInteractiveToolsContext::EndTool(EToolSide::Mouse, EToolShutdownType::Cancel);
				}
			}
		}

		UE::Geometry::FDynamicMesh3 ConvertedMeshB;
		Converter.Convert(MeshDescription, ConvertedMeshB);
		EnsureMultipleTriangleGroups(ConvertedMeshB);
		ComponentB = NewObject<UDynamicMeshComponent>(StageActor, NAME_None, RF_Transient);
		ComponentB->SetFlags(RF_Transactional);
		ComponentB->CreationMethod = EComponentCreationMethod::Instance;
		ComponentB->GetDynamicMesh()->SetFlags(RF_Transactional);
		ComponentB->GetDynamicMesh()->SetMesh(MoveTemp(ConvertedMeshB));
		StageActor->AddInstanceComponent(ComponentB);
		ComponentB->AttachToComponent(StageRoot, FAttachmentTransformRules::KeepRelativeTransform);
		ComponentB->SetRelativeLocation(FVector(35.0f, 0.0f, 0.0f));
		ComponentB->RegisterComponent();
		ComponentB->UpdateBounds();

		ModeManager->GetSelectedComponents()->DeselectAll();
		ModeManager->GetSelectedComponents()->Select(Component);
		ModeManager->GetSelectedComponents()->Select(ComponentB);
		bPassed &= TestEqual(
			TEXT("Two staged components are selectable for CSG tools"),
			ModeManager->GetSelectedComponents()->Num(),
			2);

		for (const FString& ToolIdentifier : {
			UUbstreamStageEditorMode::CSGMeshesToolIdentifier,
			UUbstreamStageEditorMode::TrimMeshesToolIdentifier })
		{
			bPassed &= TestTrue(
				FString::Printf(TEXT("%s can start for two staged components"), *ToolIdentifier),
				Mode->CanStartTool(ToolIdentifier));
		}

		if (ToolsContext)
		{
			for (const FString& ToolIdentifier : {
				UUbstreamStageEditorMode::CSGMeshesToolIdentifier,
				UUbstreamStageEditorMode::TrimMeshesToolIdentifier })
			{
				const bool bStarted = ToolsContext->UInteractiveToolsContext::StartTool(EToolSide::Mouse, ToolIdentifier);
				bPassed &= TestTrue(
					FString::Printf(TEXT("%s starts and completes Setup for two staged components"), *ToolIdentifier),
					bStarted);

				UInteractiveTool* ActiveTool = ToolsContext->ToolManager->GetActiveTool(EToolSide::Mouse);
				bPassed &= TestNotNull(
					*FString::Printf(TEXT("%s active tool instance exists"), *ToolIdentifier),
					ActiveTool);
				if (ActiveTool)
				{
					bPassed &= TestTrue(
						FString::Printf(TEXT("%s exposes tool properties for the settings panel"), *ToolIdentifier),
						ActiveTool->GetToolProperties().Num() > 0);
					ToolsContext->UInteractiveToolsContext::EndTool(EToolSide::Mouse, EToolShutdownType::Cancel);
				}
			}
		}

		ModeManager->GetSelectedComponents()->DeselectAll();
		ModeManager->GetSelectedComponents()->Select(Component);
		ModeManager->DeactivateMode(UUbstreamStageEditorMode::EM_UbstreamStageEditorModeId);
	}

	if (GEditor)
	{
		// Purge the whole element list rather than deselecting individual
		// components: targeted deselect can leave a stale element handle that
		// asserts in gizmo-display queries once a component is destroyed.
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true, /*WarnAboutManyActors*/ false);
	}

	ModeManager->GetSelectedComponents()->DeselectAll();
	if (ComponentB)
	{
		ComponentB->DestroyComponent();
	}
	Component->DestroyComponent();
	// The stage actor is left to the preview world's own teardown; explicit
	// DestroyActor in preview worlds trips typed-element shutdown assertions.
	ModeManager->SetPreviewScene(nullptr);

	return bPassed;
}

#endif
