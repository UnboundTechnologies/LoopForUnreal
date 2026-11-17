#include "AdvancedPreviewScene.h"
#include "Async/TaskGraphInterfaces.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Framework/Application/SlateApplication.h"
#include "Generators/MinimalBoxMeshGenerator.h"
#include "Generators/SphereGenerator.h"
#include "Capture/UbstreamCaptureService.h"
#include "Capture/UbstreamCaptureTypes.h"
#include "Geometry/UbstreamCSGKernel.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UDynamicMesh.h"
#include "UbstreamModelerEditorModule.h"
#include "Viewport/UbstreamViewportClient.h"
#include "Widgets/SUbstreamElementsPanel.h"
#include "Widgets/SUbstreamModelerWidget.h"
#include "Widgets/SUbstreamModelerViewport.h"
#include "Editing/UbstreamStageEditorMode.h"
#include "InputState.h"
#include "InputRouter.h"
#include "Slate/SceneViewport.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "Widgets/SWindow.h"
#include "Tests/UbstreamWorkspaceTestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FUbstreamProfileStats
	{
		double TotalMs = 0.0;
		double MaxMs = 0.0;
		int32 Samples = 0;
		TArray<double> All;

		void Add(const double Ms)
		{
			TotalMs += Ms;
			MaxMs = FMath::Max(MaxMs, Ms);
			++Samples;
			All.Add(Ms);
		}

		double Average() const { return Samples > 0 ? TotalMs / Samples : 0.0; }

		double Percentile95()
		{
			if (All.IsEmpty())
			{
				return 0.0;
			}
			All.Sort();
			return All[FMath::Clamp(FMath::FloorToInt32(All.Num() * 0.95), 0, All.Num() - 1)];
		}
	};

	void PumpOnce(const TSharedPtr<FUbstreamViewportClient>& Client, SUbstreamModelerViewport* Viewport)
	{
		FSlateApplication::Get().PumpMessages();
		FSlateApplication::Get().Tick();
		if (Client.IsValid())
		{
			Client->Tick(1.0f / 60.0f);
		}
		if (Viewport)
		{
			Viewport->TickCSGSession();
			Viewport->GetSceneViewport()->Draw();
		}
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FPlatformProcess::Sleep(0.005f);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUbstreamStageProfileTest,
	"Ubstream.Modeler.StageProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FUbstreamStageProfileTest::RunTest(const FString& Parameters)
{
	AddExpectedError(
		TEXT("UWorld::DestroyActor: World has no context!.*"),
		EAutomationExpectedErrorFlags::Contains,
		-1);

	// Profile against a light and a heavy subject: the heavy sphere is
	// representative of imported/generated meshes.
	struct FScenario
	{
		const TCHAR* Name;
		int32 SphereSteps;
	};
	const FScenario Scenarios[] = {
		{ TEXT("Light(cube-class ~1k tris)"), 24 },
		{ TEXT("Heavy(~80k tris)"), 200 },
	};

	for (const FScenario& Scenario : Scenarios)
	{
		UE::Geometry::FSphereGenerator SphereGenerator;
		SphereGenerator.Radius = 50.0;
		SphereGenerator.NumPhi = Scenario.SphereSteps;
		SphereGenerator.NumTheta = Scenario.SphereSteps;
		SphereGenerator.Generate();
		UE::Geometry::FDynamicMesh3 SubjectMesh(&SphereGenerator);
		const int32 SubjectTriangles = SubjectMesh.TriangleCount();

		// Raw kernel cost first: one boolean on this subject.
		UE::Geometry::FMinimalBoxMeshGenerator CutterGenerator;
		CutterGenerator.Box = UE::Geometry::FOrientedBox3d(FVector3d(40.0, 0.0, 0.0), FVector3d(15.0, 15.0, 15.0));
		CutterGenerator.Generate();
		UE::Geometry::FDynamicMesh3 CutterMesh(&CutterGenerator);
		const double KernelStart = FPlatformTime::Seconds();
		FUbstreamCSGResult KernelResult = FUbstreamCSGKernel::Apply(
			SubjectMesh, FTransform::Identity, CutterMesh, FTransform::Identity, EUbstreamCSGOp::Subtract);
		const double KernelMs = (FPlatformTime::Seconds() - KernelStart) * 1000.0;

		// FULL production widget (viewport + elements panel + streaming control
		// panel, all delegates bound) - the notify chain only exists here.
		TSharedRef<SUbstreamModelerWidget> FullWidget = SNew(SUbstreamModelerWidget);
		if (!TestTrue(TEXT("Offline workspace layout is visible"), UbstreamWorkspaceTestUtils::ShowOfflineWorkspace(FullWidget.Get()))) return false;
		TSharedRef<SWindow> Window = SNew(SWindow)
			.ClientSize(FVector2D(1200.0f, 800.0f))
			.CreateTitleBar(false)
			[
				FullWidget
			];
		FSlateApplication::Get().AddWindow(Window);

		// Stage the subject via a source component in its own scene.
		TSharedRef<FAdvancedPreviewScene> SourceScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParameters.ObjectFlags = RF_Transient;
		AActor* SourceActor = SourceScene->GetWorld()->SpawnActor<AActor>(SpawnParameters);
		USceneComponent* SourceRoot = NewObject<USceneComponent>(SourceActor, TEXT("ProfileRoot"), RF_Transient);
		SourceActor->SetRootComponent(SourceRoot);
		SourceActor->AddInstanceComponent(SourceRoot);
		SourceRoot->RegisterComponent();
		UDynamicMeshComponent* SourceComponent = NewObject<UDynamicMeshComponent>(SourceActor, NAME_None, RF_Transient);
		SourceComponent->SetFlags(RF_Transactional);
		SourceComponent->CreationMethod = EComponentCreationMethod::Instance;
		SourceComponent->GetDynamicMesh()->SetMesh(MoveTemp(SubjectMesh));
		SourceActor->AddInstanceComponent(SourceComponent);
		SourceComponent->AttachToComponent(SourceRoot, FAttachmentTransformRules::KeepRelativeTransform);
		SourceComponent->RegisterComponent();

		TArray<TWeakObjectPtr<UMeshComponent>> SourceComponentWeaks;
		SourceComponentWeaks.Add(SourceComponent);
		if (!TestTrue(TEXT("Profile subject loads"), FullWidget->LoadSoloComponents(SourceComponentWeaks)))
		{
			Window->RequestDestroyWindow();
			continue;
		}
		TSharedRef<SUbstreamModelerViewport> Viewport = FullWidget->GetSoloViewport().ToSharedRef();
		TSharedRef<SUbstreamElementsPanel> Panel = SNew(SUbstreamElementsPanel).SoloViewport(TWeakPtr<SUbstreamModelerViewport>(Viewport));
		TSharedPtr<FUbstreamViewportClient> Client = Viewport->GetUbstreamViewportClient();
		for (int32 Warmup = 0; Warmup < 10; ++Warmup)
		{
			PumpOnce(Client, &Viewport.Get());
		}

		// The user's exact flow: add a cube, live subtract, drag it around.
		TestTrue(TEXT("Profile cutter adds"), Viewport->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));
		TestTrue(TEXT("Profile session starts"), Viewport->StartCSGOperation(EUbstreamCSGOperation::Subtract));

		FUbstreamProfileStats DragPumpStats;
		const int32 DragSteps = 60;
		const double DragStart = FPlatformTime::Seconds();
		for (int32 Step = 0; Step < DragSteps; ++Step)
		{
			const double StepStart = FPlatformTime::Seconds();
			Viewport->ApplyCSGTransformDelta(
				FVector(FMath::Sin(Step * 0.4) * 2.0, FMath::Cos(Step * 0.4) * 2.0, 0.5),
				FRotator::ZeroRotator, FVector::ZeroVector);
			PumpOnce(Client, &Viewport.Get());
			DragPumpStats.Add((FPlatformTime::Seconds() - StepStart) * 1000.0);
		}
		const double DragTotalSeconds = FPlatformTime::Seconds() - DragStart;

		// Same drag WITH the streaming capture pipeline running at stream
		// cadence (every 4th pump ~ 15fps), the way a connected session runs.
		FUbstreamProfileStats StreamingDragStats;
		FUbstreamProfileStats CaptureBeginStats;
		TSharedPtr<FUbstreamAsyncFrameReadback, ESPMode::ThreadSafe> PendingProfileFrame;
		uint64 ProfileFrameId = 90000;
		// Encoding callbacks can arrive after this scenario has left the stack.
		auto CompletedCaptures = MakeShared<int32, ESPMode::ThreadSafe>(0);
		const double StreamingDragStart = FPlatformTime::Seconds();
		for (int32 Step = 0; Step < DragSteps; ++Step)
		{
			const double StepStart = FPlatformTime::Seconds();
			Viewport->ApplyCSGTransformDelta(
				FVector(FMath::Sin(Step * 0.4) * -2.0, FMath::Cos(Step * 0.4) * -2.0, -0.5),
				FRotator::ZeroRotator, FVector::ZeroVector);
			if (Step % 4 == 0)
			{
				if (PendingProfileFrame.IsValid() && PendingProfileFrame->IsReady())
				{
					PendingProfileFrame->CompleteAndEncodeAsync([CompletedCaptures](FUbstreamEditorWorldCaptureResult&&)
					{
						++*CompletedCaptures;
					});
					PendingProfileFrame.Reset();
				}
				if (!PendingProfileFrame.IsValid())
				{
					const double CaptureStart = FPlatformTime::Seconds();
					FUbstreamCaptureSettings CaptureSettings;
					CaptureSettings.Width = 512;
					CaptureSettings.Height = 512;
					PendingProfileFrame = Viewport->BeginAsyncCaptureFramePixels(ProfileFrameId++, CaptureSettings);
					CaptureBeginStats.Add((FPlatformTime::Seconds() - CaptureStart) * 1000.0);
				}
			}
			PumpOnce(Client, &Viewport.Get());
			StreamingDragStats.Add((FPlatformTime::Seconds() - StepStart) * 1000.0);
		}
		const double StreamingDragTotalSeconds = FPlatformTime::Seconds() - StreamingDragStart;
		PendingProfileFrame.Reset();

		// How stale is the preview when the drag stops?
		const double SettleStart = FPlatformTime::Seconds();
		while (Viewport->HasActiveCSGSession() && !Viewport->CanCommitCSGSession()
			&& FPlatformTime::Seconds() - SettleStart < 30.0)
		{
			PumpOnce(Client, &Viewport.Get());
		}
		const double SettleMs = (FPlatformTime::Seconds() - SettleStart) * 1000.0;

		// THE REAL USER DRAG PATH: a captured gizmo drag routed through
		// FEditorViewportClient::CapturedMouseMove -> ModeTools -> ITF router.
		// No other probe covers this layer.
		FUbstreamProfileStats CapturedMoveStats;
		{
			UEditorInteractiveToolsContext* ToolsContext = Viewport->GetStageToolsContextForTesting();
			UInputRouter* Router = ToolsContext ? ToolsContext->InputRouter : nullptr;
			FVector GizmoCenter = FVector::ZeroVector;
			if (Router && Viewport->GetCSGTransformWidgetLocation(GizmoCenter) && Client.IsValid()
				&& Viewport->GetSceneViewport().IsValid())
			{
				const FIntPoint ViewSize = Viewport->GetSceneViewport()->GetSize();
				const FVector Camera = Client->GetViewLocation();
				const double Distance = FVector::Distance(Camera, GizmoCenter);
				const double HalfFov = FMath::DegreesToRadians(Client->ViewFOV * 0.5);
				const float PixelToWorld = static_cast<float>((2.0 * Distance * FMath::Tan(HalfFov)) / FMath::Max(1, ViewSize.Y));

				// Capture a translate arrow like a real press.
				bool bCaptured = false;
				for (const FVector& Axis : { FVector::XAxisVector, -FVector::XAxisVector, FVector::YAxisVector, -FVector::YAxisVector })
				{
					const FVector Grab = GizmoCenter + Axis * (45.0 * PixelToWorld);
					FInputDeviceState Hover;
					Hover.InputDevice = EInputDevices::Mouse;
					Hover.Mouse.WorldRay = FRay(Camera, (Grab - Camera).GetSafeNormal(), true);
					Router->PostInputEvent(Hover);
					FInputDeviceState Press = Hover;
					Press.Mouse.Left.SetStates(true, true, false);
					Router->PostInputEvent(Press);
					if (Router->HasActiveMouseCapture())
					{
						bCaptured = true;
						break;
					}
					FInputDeviceState Reset = Hover;
					Reset.Mouse.Left.SetStates(false, false, true);
					Router->PostInputEvent(Reset);
				}

				if (bCaptured)
				{
					// Storm of captured mouse moves through the REAL client path.
					for (int32 Move = 0; Move < 40; ++Move)
					{
						const int32 MouseX = ViewSize.X / 2 + 40 + (Move % 20) * 3;
						const int32 MouseY = ViewSize.Y / 2 + (Move % 7) * 2;
						const double MoveStart = FPlatformTime::Seconds();
						Client->CapturedMouseMove(Viewport->GetSceneViewport()->GetViewport(), MouseX, MouseY);
						CapturedMoveStats.Add((FPlatformTime::Seconds() - MoveStart) * 1000.0);
						if (Move % 5 == 0)
						{
							PumpOnce(Client, &Viewport.Get());
						}
					}
					FInputDeviceState Release;
					Release.InputDevice = EInputDevices::Mouse;
					Release.Mouse.WorldRay = FRay(Camera, (GizmoCenter - Camera).GetSafeNormal(), true);
					Release.Mouse.Left.SetStates(false, false, true);
					Router->PostInputEvent(Release);
				}
			}
		}
		AddInfo(FString::Printf(TEXT("[%s] REAL captured-move path: %d moves, avg %.2fms, p95 %.2fms, max %.2fms"),
			Scenario.Name, CapturedMoveStats.Samples, CapturedMoveStats.Average(), CapturedMoveStats.Percentile95(), CapturedMoveStats.MaxMs));

		// The hover regression class: a hit-proxy query after invalidation
		// forces a full extra scene render. This is what a per-mouse-move
		// hover lookup costs mid-drag.
		FUbstreamProfileStats HitProxyStats;
		if (Viewport->GetSceneViewport().IsValid())
		{
			for (int32 Query = 0; Query < 10; ++Query)
			{
				Client->Invalidate();
				PumpOnce(Client, &Viewport.Get());
				const double QueryStart = FPlatformTime::Seconds();
				Viewport->GetSceneViewport()->GetHitProxy(400, 300);
				HitProxyStats.Add((FPlatformTime::Seconds() - QueryStart) * 1000.0);
			}
		}
		AddInfo(FString::Printf(TEXT("[%s] hit-proxy query after invalidation: avg %.2fms, max %.2fms"),
			Scenario.Name, HitProxyStats.Average(), HitProxyStats.MaxMs));

		// Elements panel rebuild cost in isolation.
		FUbstreamProfileStats PanelRefreshStats;
		for (int32 Refresh = 0; Refresh < 50; ++Refresh)
		{
			const double RefreshStart = FPlatformTime::Seconds();
			Panel->Refresh();
			PanelRefreshStats.Add((FPlatformTime::Seconds() - RefreshStart) * 1000.0);
		}

		// Subject SetMesh cost (the per-preview render rebuild).
		UDynamicMeshComponent* StageSubject = Viewport->GetPreviewComponents().Num() > 0
			? Cast<UDynamicMeshComponent>(Viewport->GetPreviewComponents()[0].Get())
			: nullptr;
		double SetMeshMs = 0.0;
		if (StageSubject)
		{
			UE::Geometry::FDynamicMesh3 CopyForTiming;
			StageSubject->ProcessMesh([&CopyForTiming](const UE::Geometry::FDynamicMesh3& Mesh)
			{
				CopyForTiming = Mesh;
			});
			const double SetMeshStart = FPlatformTime::Seconds();
			StageSubject->GetDynamicMesh()->SetMesh(MoveTemp(CopyForTiming));
			SetMeshMs = (FPlatformTime::Seconds() - SetMeshStart) * 1000.0;
		}

		AddInfo(FString::Printf(
			TEXT("[%s] subject=%d tris | kernel one boolean=%.1fms | drag: %d steps in %.2fs (avg pump %.2fms, p95 %.2fms, max %.2fms) | drag+stream: %.2fs (avg %.2fms, p95 %.2fms, max %.2fms; capture begin avg %.2fms; %d frames encoded) | settle after drag=%.0fms | panel refresh avg %.3fms | subject SetMesh=%.1fms"),
			Scenario.Name,
			SubjectTriangles,
			KernelMs,
			DragSteps, DragTotalSeconds,
			DragPumpStats.Average(), DragPumpStats.Percentile95(), DragPumpStats.MaxMs,
			StreamingDragTotalSeconds,
			StreamingDragStats.Average(), StreamingDragStats.Percentile95(), StreamingDragStats.MaxMs,
			CaptureBeginStats.Average(), *CompletedCaptures,
			SettleMs,
			PanelRefreshStats.Average(),
			SetMeshMs));

		Viewport->CancelCSGSession();
		Viewport->ClearObjects();
		Window->RequestDestroyWindow();
		for (int32 Cooldown = 0; Cooldown < 5; ++Cooldown)
		{
			PumpOnce(Client, nullptr);
		}
	}

	return true;
}

#endif
