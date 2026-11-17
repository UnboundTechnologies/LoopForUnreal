#include "AdvancedPreviewScene.h"
#include "Async/TaskGraphInterfaces.h"
#include "Components/DynamicMeshComponent.h"
#include "Editing/UbstreamVertexSculptTool.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Generators/SphereGenerator.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/AutomationTest.h"
#include "Slate/SceneViewport.h"
#include "UDynamicMesh.h"
#include "UObject/UnrealType.h"
#include "Viewport/UbstreamViewportClient.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SUbstreamModelerViewport.h"
#include "Widgets/SUbstreamSculptPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	bool ContainsLabel(const TSharedRef<SWidget>& Widget, const FString& Label)
	{
		if (Widget->GetTypeAsString() == TEXT("STextBlock")
			&& StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString() == Label)
		{
			return true;
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 I = 0; I < Children->Num(); ++I)
		{
			if (ContainsLabel(Children->GetChildAt(I), Label)) return true;
		}
		return false;
	}

	TSharedPtr<SCheckBox> FindBrush(const TSharedRef<SWidget>& Widget, const FString& Label)
	{
		if (Widget->GetTypeAsString() == TEXT("SCheckBox") && ContainsLabel(Widget, Label))
		{
			return StaticCastSharedRef<SCheckBox>(Widget);
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 I = 0; I < Children->Num(); ++I)
		{
			if (TSharedPtr<SCheckBox> Found = FindBrush(Children->GetChildAt(I), Label)) return Found;
		}
		return nullptr;
	}
	TSharedPtr<SButton> FindButton(const TSharedRef<SWidget>& Widget, const FString& Label)
	{
		if (Widget->GetTypeAsString() == TEXT("SButton") && ContainsLabel(Widget, Label))
			return StaticCastSharedRef<SButton>(Widget);
		FChildren* Children = Widget->GetChildren();
		for (int32 I = 0; I < Children->Num(); ++I)
			if (auto Found = FindButton(Children->GetChildAt(I), Label)) return Found;
		return nullptr;
	}

	TSharedPtr<SWidget> FindProgressBar(const TSharedRef<SWidget>& Widget)
	{
		if (Widget->GetTypeAsString() == TEXT("SProgressBar")) return Widget;
		FChildren* Children = Widget->GetChildren();
		for (int32 I = 0; I < Children->Num(); ++I)
			if (auto Found = FindProgressBar(Children->GetChildAt(I))) return Found;
		return nullptr;
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamSculptToolsAutomationTest,
	"Ubstream.Modeler.SculptTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FUbstreamSculptToolsAutomationTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("UWorld::DestroyActor: World has no context!.*CombinedTransformGizmoActor"),
		EAutomationExpectedErrorFlags::Contains, -1);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	GEditor->ResetTransaction(NSLOCTEXT("UbstreamSculptTests", "Start", "Sculpt tools test"));
	bool bPassed = true;

	// Generated geometry and a transient source: never alter a user's asset.
	TSharedRef<FAdvancedPreviewScene> SourceScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient;
	AActor* Owner = SourceScene->GetWorld()->SpawnActor<AActor>(Spawn);
	UDynamicMeshComponent* Source = NewObject<UDynamicMeshComponent>(Owner, NAME_None, RF_Transient | RF_Transactional);
	Owner->SetRootComponent(Source);
	Owner->AddInstanceComponent(Source);
	UE::Geometry::FSphereGenerator Generator;
	Generator.Radius = 50;
	Generator.NumPhi = 24;
	Generator.NumTheta = 32;
	Generator.Generate();
	Source->GetDynamicMesh()->SetMesh(UE::Geometry::FDynamicMesh3(&Generator));
	Source->RegisterComponent();
	Source->UpdateBounds();

	TSharedRef<SUbstreamModelerViewport> Stage = SNew(SUbstreamModelerViewport);
	TSharedRef<SUbstreamSculptPanel> SculptPanel = SNew(SUbstreamSculptPanel).SoloViewport(Stage);
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(NSLOCTEXT("UbstreamSculptTests", "Title", "Unbound tool regression check"))
		.ClientSize(FVector2D(1100, 820))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(320)[SculptPanel]]
			+ SHorizontalBox::Slot().FillWidth(1)[Stage]
		];
	FSlateApplication::Get().AddWindow(Window);
	auto Pump = [&](int32 Frames = 3)
	{
		for (int32 I = 0; I < Frames; ++I)
		{
			FSlateApplication::Get().PumpMessages();
			FSlateApplication::Get().Tick();
			Stage->GetUbstreamViewportClient()->Tick(1.0f / 60.0f);
			Stage->TickCSGSession();
			Stage->GetSceneViewport()->Draw();
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.01f);
		}
	};
	auto WaitForRemesh = [&]()
	{
		const double Deadline = FPlatformTime::Seconds() + 30;
		while (Stage->IsRemeshing() && FPlatformTime::Seconds() < Deadline) Pump(1);
		bPassed &= TestFalse(TEXT("Background remesh completes"), Stage->IsRemeshing());
	};

	auto Load = [&]()
	{
		bPassed &= TestTrue(TEXT("Load generated sphere"), Stage->SetMeshComponents({Source}, TEXT("Sculpt test"),
			TEXT("Automation.Sculpt"), {}, true));
		Pump();
	};
	Load();
	auto CopySubject = [&]()
	{
		TArray<UMeshComponent*> Subjects;
		Stage->AppendSourcePreviewComponents(Subjects);
		UE::Geometry::FDynamicMesh3 Copy;
		if (Subjects.Num() == 1)
		{
			CastChecked<UDynamicMeshComponent>(Subjects[0])->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh) { Copy = Mesh; });
		}
		return Copy;
	};
	UE::Geometry::FDynamicMesh3::FSameAsOptions Exact;
	Exact.bCheckAttributes = true;
	Exact.bCheckGroups = true;
	Exact.bCheckEdgeIDs = true;

	// Drive the actual remesh/cancel control, not only the viewport API.
	const auto BeforeCancel = CopySubject();
	const auto ProgressBar = FindProgressBar(SculptPanel);
	bPassed &= TestNotNull(TEXT("Remesh progress bar exists"), ProgressBar.Get());
	if (ProgressBar) bPassed &= TestFalse(TEXT("Idle progress has no animation timer"), ProgressBar->HasActiveTimers());
	Stage->SetSculptTargetTriangleCount(200000);
	if (auto RemeshButton = FindButton(SculptPanel, TEXT("Remesh")))
	{
		RemeshButton->SimulateClick();
		bPassed &= TestTrue(TEXT("Remesh button starts background work"), Stage->IsRemeshing());
		if (ProgressBar) bPassed &= TestTrue(TEXT("Running remesh animates progress"), ProgressBar->HasActiveTimers());
		Pump(1); // Let Slate evaluate the enabled/visibility attributes.
		bPassed &= TestTrue(TEXT("Busy panel stays visible"), SculptPanel->GetChildren()->GetChildAt(0)->GetVisibility().IsVisible());
		if (auto BrushButton = FindBrush(SculptPanel, TEXT("Draw")))
			bPassed &= TestFalse(TEXT("Brush controls disable during remesh"), BrushButton->IsEnabled());
		TArray<FColor> Screenshot;
		FIntVector ScreenshotSize;
		if (FSlateApplication::Get().TakeScreenshot(SculptPanel, Screenshot, ScreenshotSize) && !Screenshot.IsEmpty())
		{
			TArray64<uint8> Png;
			if (FImageUtils::CompressImage(Png, TEXT("png"), FImageView(Screenshot.GetData(), ScreenshotSize.X, ScreenshotSize.Y)))
				FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("RemeshProgress.png")));
		}
		SculptPanel->SetVisibility(EVisibility::Collapsed);
		if (auto CancelButton = FindButton(SculptPanel, TEXT("Cancel"))) CancelButton->SimulateClick();
		else { bPassed = false; AddError(TEXT("Remesh exposes a Cancel button")); Stage->CancelRemesh(); }
		WaitForRemesh();
		if (ProgressBar) bPassed &= TestFalse(TEXT("Hidden completed progress stops its timer"), ProgressBar->HasActiveTimers());
		SculptPanel->SetVisibility(EVisibility::Visible);
		bPassed &= TestTrue(TEXT("Cancel button keeps exact geometry"), BeforeCancel.IsSameAs(CopySubject(), Exact));
	}
	else { bPassed = false; AddError(TEXT("Remesh button exists")); }

	// Exercise the surface-detail action (distinct from Epic's Remesh tool).
	for (int32 Target : {500, 2000})
	{
		const int32 Before = Stage->GetSubjectTriangleCount();
		Stage->SetSculptTargetTriangleCount(Target);
		FString Error;
		bPassed &= TestTrue(TEXT("Surface detail remesh succeeds: ") + Error, Stage->RemeshSubjectToTargetDetail(Error));
		WaitForRemesh();
		Pump();
		const int32 After = Stage->GetSubjectTriangleCount();
		bPassed &= TestTrue(TEXT("Remesh changes density"), Before != After && After > 0);
		GEditor->UndoTransaction(); Pump();
		bPassed &= TestEqual(TEXT("Undo restores pre-remesh triangles"), Stage->GetSubjectTriangleCount(), Before);
		GEditor->RedoTransaction(); Pump();
		bPassed &= TestEqual(TEXT("Redo restores remesh triangles"), Stage->GetSubjectTriangleCount(), After);
	}

	struct FBrushCase { const TCHAR* Label; const TCHAR* Props; };
	const FBrushCase Brushes[] = {
		{TEXT("Move"), TEXT("MoveBrushOpProps")}, {TEXT("Draw"), TEXT("StandardSculptBrushOpProps")},
		{TEXT("Smooth"), TEXT("SmoothBrushOpProps")}, {TEXT("Inflate"), TEXT("InflateBrushOpProps")},
		{TEXT("Flatten"), TEXT("FlattenBrushOpProps")}, {TEXT("Pinch"), TEXT("PinchBrushOpProps")},
		{TEXT("Clay"), TEXT("SculptMaxBrushOpProps")}, {TEXT("Scrape"), TEXT("PlaneBrushOpProps")},
		{TEXT("Grab"), TEXT("PullKelvinletBrushOpProps")}
	};
	for (int32 Round = 0; Round < 2; ++Round)
	{
		const UE::Geometry::FDynamicMesh3 SessionBefore = CopySubject();
		for (const FBrushCase& Brush : Brushes)
		{
			UE_LOG(LogTemp, Display, TEXT("Sculpt regression round %d: %s"), Round + 1, Brush.Label);
			TSharedPtr<SCheckBox> Button = FindBrush(SculptPanel, Brush.Label);
			if (!TestNotNull(Brush.Label, Button.Get())) { bPassed = false; continue; }
			const FGeometry Geometry = Button->GetCachedGeometry();
			const FKeyEvent Activate(EKeys::SpaceBar, FModifierKeysState(), 0, false, 0, 0);
			Button->OnKeyDown(Geometry, Activate);
			Button->OnKeyUp(Geometry, Activate);
			Pump();
			UUbstreamVertexSculptTool* Tool = Cast<UUbstreamVertexSculptTool>(Stage->GetActiveVertexSculptTool());
			if (!TestNotNull(TEXT("Brush activates a sculpt tool"), Tool)) { bPassed = false; continue; }
			UMeshSculptBrushOpProps* Props = Stage->GetActiveSculptBrushOpProps();
			bPassed &= TestTrue(TEXT("Selected brush has the correct live operation"), Props && Props->GetClass()->GetName() == Brush.Props);
			Stage->SetSculptBrushSize(0.3f);
			Stage->SetSculptBrushStrength(0.45f);
			Stage->SetSculptBrushFalloff(Round == 0 ? 0.0f : 1.0f);
			Stage->SetSculptSymmetryEnabled(Round == 1);
			Pump();
			if (Props && FindFProperty<FFloatProperty>(Props->GetClass(), TEXT("Falloff")))
			{
				bPassed &= TestEqual(TEXT("Falloff slider controls the live brush"), Props->GetFalloff(), Round == 0 ? 0.0f : 1.0f);
			}
			UE::Geometry::FDynamicMesh3 Before, After;
			uint64 Revision = 0;
			bPassed &= TestTrue(TEXT("Copy mesh before stroke"), Tool->CopyMeshIfChanged(Revision, Before));
			bPassed &= TestTrue(TEXT("Sculpt uses dense IDs even after remeshing"), Before.IsCompact());
			const FRay Start(FVector(200, 4, 3), FVector(-1, 0, 0));
			Tool->OnBeginDrag(Start);
			uint64 DuringRevision = 0;
			bPassed &= TestFalse(TEXT("Active stroke defers capture copies"), Tool->CopyMeshIfChanged(DuringRevision, After));
			for (int32 Step = 1; Step <= 5; ++Step)
			{
				Tool->OnUpdateDrag(FRay(FVector(200, 4 + Step * 1.5, 3 + Step), FVector(-1, 0, 0)));
				Pump(2);
			}
			Tool->OnEndDrag(Start);
			Pump();
			bPassed &= TestTrue(TEXT("Copy completed stroke"), Tool->CopyMeshIfChanged(Revision, After));
			bPassed &= TestTrue(TEXT("Stroke preserves valid topology"), After.CheckValidity({}, UE::Geometry::EValidityCheckFailMode::ReturnOnly));
			bool bChanged = false;
			for (int32 V : Before.VertexIndicesItr()) bChanged |= !Before.GetVertex(V).Equals(After.GetVertex(V), 1.e-6);
			bPassed &= TestTrue(FString(Brush.Label) + TEXT(" deforms the surface"), bChanged);
			GEditor->UndoTransaction(); Pump();
			GEditor->RedoTransaction(); Pump();
		}
		Stage->StopSculpt(Round == 0); // Done, then Cancel on the second pass.
		Pump();
		bPassed &= TestFalse(TEXT("Done/Cancel ends sculpt"), Stage->IsSculptActive());
		const UE::Geometry::FDynamicMesh3 SessionAfter = CopySubject();
		if (Round == 0)
		{
			bPassed &= TestFalse(TEXT("Done commits deformations"), SessionAfter.IsSameAs(SessionBefore, Exact));
			GEditor->UndoTransaction(); Pump();
			bPassed &= TestTrue(TEXT("Undo sculpt restores sparse source including attributes"), CopySubject().IsSameAs(SessionBefore, Exact));
			GEditor->RedoTransaction(); Pump();
			bPassed &= TestTrue(TEXT("Redo restores committed sculpt including attributes"), CopySubject().IsSameAs(SessionAfter, Exact));
		}
		else
		{
			bPassed &= TestTrue(TEXT("Cancel preserves the exact staged mesh"), SessionAfter.IsSameAs(SessionBefore, Exact));
		}
	}

	// Every cutter shape and operation, followed by sculpting the baked result.
	for (EUbstreamCSGPrimitiveType Shape : {EUbstreamCSGPrimitiveType::Box, EUbstreamCSGPrimitiveType::Sphere, EUbstreamCSGPrimitiveType::Cylinder})
	{
		Stage->ClearObjects(); Load();
		bPassed &= TestTrue(TEXT("Cutter shape can be added"), Stage->AddCSGPrimitive(Shape));
		TArray<UMeshComponent*> Cutters;
		Stage->GetCSGPrimitiveComponents(Cutters);
		if (Cutters.Num() == 1)
		{
			CastChecked<UDynamicMeshComponent>(Cutters[0])->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
			{
				bPassed &= TestTrue(TEXT("Every cutter is centered on its placement origin"), Mesh.GetBounds().Center().IsNearlyZero(0.001));
			});
		}
		for (EUbstreamCSGOperation Operation : {EUbstreamCSGOperation::Subtract, EUbstreamCSGOperation::Union, EUbstreamCSGOperation::Intersect})
		{
			bPassed &= TestTrue(TEXT("Cutter operation starts"), Stage->StartCSGOperation(Operation));
			const double Deadline = FPlatformTime::Seconds() + 10.0;
			do { Pump(); } while (!Stage->CanCommitCSGSession() && FPlatformTime::Seconds() < Deadline);
			bPassed &= TestTrue(FString::Printf(TEXT("Cutter %d operation %d is committable: %s"),
				static_cast<int32>(Shape), static_cast<int32>(Operation), *Stage->GetCSGSessionMessage()), Stage->CanCommitCSGSession());
		}
		bPassed &= TestTrue(TEXT("Sculpt bakes the live cutter"), Stage->StartSculpt(EUbstreamSculptBrush::Inflate));
		Pump();
		bPassed &= TestEqual(TEXT("Cutter is consumed by sculpt transition"), Stage->GetCSGPrimitiveCount(), 0);
		if (UUbstreamVertexSculptTool* Tool = Cast<UUbstreamVertexSculptTool>(Stage->GetActiveVertexSculptTool()))
		{
			uint64 Revision = 0;
			UE::Geometry::FDynamicMesh3 Copy;
			bPassed &= TestTrue(TEXT("Boolean result has a safe sculpt copy"), Tool->CopyMeshIfChanged(Revision, Copy) && Copy.IsCompact());
		}
		Stage->StopSculpt(false); Pump();
	}


	// Remesh while sculpting, rapid pending requests, and clear/close teardown.
	bPassed &= TestTrue(TEXT("Start sculpt before remesh"), Stage->StartSculpt(EUbstreamSculptBrush::Draw));
	Pump();
	Stage->SetSculptTargetTriangleCount(4000);
	FString Error;
	bPassed &= TestTrue(TEXT("Remesh ends sculpt safely"), Stage->RemeshSubjectToTargetDetail(Error));
	WaitForRemesh();
	Pump();
	bPassed &= TestFalse(TEXT("Remesh leaves no old sculpt tool"), Stage->IsSculptActive());
	for (int32 I = 0; I < 5; ++I)
	{
		Stage->StartSculpt(EUbstreamSculptBrush::Grab);
		Stage->StopSculpt(false);
		Pump();
		bPassed &= TestFalse(TEXT("Quick cancellation clears queued start"), Stage->HasActiveEditTool());
	}
	Stage->StartSculpt(EUbstreamSculptBrush::Draw);
	Pump();
	Stage->ClearObjects();
	Pump();
	bPassed &= TestFalse(TEXT("Clear removes active tool"), Stage->HasActiveEditTool());
	bPassed &= TestFalse(TEXT("Empty stage disables brushes"), Stage->CanSculpt());
	Load();
	Stage->StartSculpt(EUbstreamSculptBrush::Grab);
	Pump();
	Window->RequestDestroyWindow();
	Stage->ClearObjects();
	Pump();
	return bPassed;
}
#endif
