#include "AdvancedPreviewScene.h"
#include "Async/TaskGraphInterfaces.h"
#include "Components/DynamicMeshComponent.h"
#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "Generators/SphereGenerator.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Settings/UbstreamModelerSettings.h"
#include "Slate/SceneViewport.h"
#include "UbstreamWorkspaceTestUtils.h"
#include "Viewport/UbstreamViewportClient.h"
#include "Widgets/SUbstreamControlPanel.h"
#include "Widgets/SUbstreamModelerViewport.h"
#include "Widgets/SUbstreamModelerWidget.h"
#include "Widgets/SWindow.h"

#if WITH_DEV_AUTOMATION_TESTS

// Opt-in end-to-end profile of the offline local-passthrough loop: the full
// workspace widget, a ~79k-triangle subject and the real stream ticker. Pass
// -UbstreamStreamProfile to run it; otherwise it returns immediately so the
// regular suite stays cheap. Timings are diagnostic, never pass/fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamStreamProfileTest, "Ubstream.Modeler.Performance.StreamProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FUbstreamStreamProfileTest::RunTest(const FString& Parameters)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("UbstreamStreamProfile")))
	{
		AddInfo(TEXT("Skipped: pass -UbstreamStreamProfile to run the stream profile."));
		return true;
	}
	float Seconds = 6.0f;
	FParse::Value(FCommandLine::Get(), TEXT("UbstreamStreamProfileSeconds="), Seconds);
	AddExpectedError(TEXT("UWorld::DestroyActor: World has no context!"), EAutomationExpectedErrorFlags::Contains, -1);

	UUbstreamModelerSettings* Settings = GetMutableDefault<UUbstreamModelerSettings>();
	TGuardValue<EUnboundLoopWebSocketProtocol> ProtocolGuard(Settings->WebSocketProtocol, EUnboundLoopWebSocketProtocol::FastStream);

	const auto Panel = SNew(SUbstreamModelerWidget);
	if (!TestTrue(TEXT("Offline workspace visible"), UbstreamWorkspaceTestUtils::ShowOfflineWorkspace(Panel.Get()))) return false;
	const auto Window = SNew(SWindow).Title(FText::FromString(TEXT("Unbound stream profile"))).ClientSize(FVector2D(1440, 900))[Panel];
	FSlateApplication::Get().AddWindow(Window);
	const auto Stage = Panel->GetSoloViewport();
	const auto Control = Panel->ControlPanel;

	// Source subject: a dense sphere comparable to the earlier idle benchmark.
	const auto SourceScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient;
	AActor* Actor = SourceScene->GetWorld()->SpawnActor<AActor>(Spawn);
	UDynamicMeshComponent* Source = NewObject<UDynamicMeshComponent>(Actor, NAME_None, RF_Transient);
	Actor->SetRootComponent(Source);
	Source->RegisterComponent();
	UE::Geometry::FSphereGenerator Generator;
	Generator.Radius = 50;
	Generator.NumPhi = 141;
	Generator.NumTheta = 282;
	Generator.Generate();
	Source->SetMesh(UE::Geometry::FDynamicMesh3(&Generator));
	Source->UpdateBounds();
	if (!TestTrue(TEXT("Profile subject loads"), Panel->LoadSoloComponents({Source})))
	{
		Window->RequestDestroyWindow();
		return false;
	}
	AddInfo(FString::Printf(TEXT("Subject triangles: %d"), Source->GetMesh()->TriangleCount()));

	struct FPhaseTimes { double Ticker = 0, Slate = 0, Viewport = 0, Total = 0; int32 Frames = 0; };
	const auto RunPhase = [&](const TCHAR* Name, float Duration, bool bOrbit)
	{
		FPhaseTimes Times;
		const uint64 FirstFrameId = Control->LastCapturedFrame.FrameId;
		// Capture-to-preview latency: when each frame id was allocated versus
		// when the preview first shows it.
		TMap<uint64, double> FrameBegan;
		uint64 SeenNextId = Control->NextFrameId;
		uint64 SeenPreviewId = Control->LastPreviewedFrameId;
		double LatencySum = 0, LatencyMax = 0;
		int32 LatencyCount = 0;
		const auto ObserveFrames = [&]()
		{
			const double Now = FPlatformTime::Seconds();
			for (; SeenNextId < Control->NextFrameId; ++SeenNextId) FrameBegan.Add(SeenNextId, Now);
			if (Control->LastPreviewedFrameId != SeenPreviewId)
			{
				SeenPreviewId = Control->LastPreviewedFrameId;
				if (const double* Began = FrameBegan.Find(SeenPreviewId))
				{
					// A frame begun and shown within one step reports its own duration.
					LatencySum += Now - *Began;
					LatencyMax = FMath::Max(LatencyMax, Now - *Began);
					++LatencyCount;
				}
			}
		};
		const double End = FPlatformTime::Seconds() + Duration;
		double Last = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("StreamProfile BEGIN %s"), Name);
		while (FPlatformTime::Seconds() < End)
		{
			const double FrameStart = FPlatformTime::Seconds();
			const float Delta = float(FrameStart - Last);
			Last = FrameStart;
			if (bOrbit)
			{
				const auto Client = Stage->GetUbstreamViewportClient();
				Client->SetViewRotation(Client->GetViewRotation() + FRotator(0, 90.0f * Delta, 0));
			}
			double T = FPlatformTime::Seconds();
			const uint64 IdBeforeTicker = Control->NextFrameId;
			FTSTicker::GetCoreTicker().Tick(Delta);
			Times.Ticker += FPlatformTime::Seconds() - T;
			for (; SeenNextId < IdBeforeTicker; ++SeenNextId) FrameBegan.Add(SeenNextId, T);
			for (; SeenNextId < Control->NextFrameId; ++SeenNextId) FrameBegan.Add(SeenNextId, T);
			ObserveFrames();
			T = FPlatformTime::Seconds();
			FSlateApplication::Get().PumpMessages();
			FSlateApplication::Get().Tick();
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			Times.Slate += FPlatformTime::Seconds() - T;
			ObserveFrames();
			T = FPlatformTime::Seconds();
			Stage->GetUbstreamViewportClient()->Tick(Delta);
			Stage->GetSceneViewport()->Draw();
			Times.Viewport += FPlatformTime::Seconds() - T;
			ObserveFrames();
			Times.Total += FPlatformTime::Seconds() - FrameStart;
			++Times.Frames;
			// Pace like a 60 Hz editor loop so cadence-driven work is realistic.
			const double Remaining = 1.0 / 60.0 - (FPlatformTime::Seconds() - FrameStart);
			if (Remaining > 0) FPlatformProcess::Sleep(float(Remaining));
		}
		const uint64 Captures = Control->LastCapturedFrame.FrameId - FirstFrameId;
		const double N = FMath::Max(1, Times.Frames);
		const FString Line = FString::Printf(
			TEXT("StreamProfile RESULT %s frames=%d (%.1f/s) captures=%llu (%.1f/s) previews=%d latency avg=%.1fms max=%.1fms ms/frame total=%.3f ticker=%.3f slate=%.3f viewport=%.3f"),
			Name, Times.Frames, Times.Frames / Duration, Captures, Captures / Duration,
			LatencyCount, LatencyCount ? LatencySum * 1000.0 / LatencyCount : 0.0, LatencyMax * 1000.0,
			Times.Total * 1000.0 / N, Times.Ticker * 1000.0 / N, Times.Slate * 1000.0 / N, Times.Viewport * 1000.0 / N);
		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);
	};

	RunPhase(TEXT("warmup"), 1.5f, false);
	RunPhase(TEXT("loaded-paused"), Seconds, false);

	// Mirror StartStreaming's local-passthrough branch without the account gate.
	Control->bStreaming = true;
	Control->RegisteredStreamIntervalSeconds = Control->GetStreamIntervalSeconds();
	Control->bRegisteredLocalPreviewOnly = Control->IsLocalPassthroughPreviewMode();
	TestTrue(TEXT("Profile runs the local passthrough path"), Control->bRegisteredLocalPreviewOnly);
	Control->StreamTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateSP(Control.ToSharedRef(), &SUbstreamControlPanel::HandleStreamTicker),
		Control->RegisteredStreamIntervalSeconds);
	RunPhase(TEXT("local-idle"), Seconds, false);
	RunPhase(TEXT("local-orbit"), Seconds, true);
	Control->StopStreaming(TEXT("Profile finished"));

	Stage->ClearObjects();
	Window->RequestDestroyWindow();
	return true;
}
#endif
