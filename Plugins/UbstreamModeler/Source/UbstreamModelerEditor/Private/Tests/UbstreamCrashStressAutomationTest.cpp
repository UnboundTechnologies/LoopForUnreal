#include "AdvancedPreviewScene.h"
#include "Async/TaskGraphInterfaces.h"
#include "Async/Async.h"
#include "Capture/UbstreamAsyncFrameReadback.h"
#include "Components/DynamicMeshComponent.h"
#include "Editing/UbstreamVertexSculptTool.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Generators/SphereGenerator.h"
#include "HAL/PlatformProcess.h"
#include "InputRouter.h"
#include "InputState.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "ScopedTransaction.h"
#include "Slate/SceneViewport.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "UDynamicMesh.h"
#include "Viewport/UbstreamViewportClient.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SUbstreamModelerViewport.h"
#include "Widgets/SWindow.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	using namespace UE::Geometry;

	void PumpBackground()
	{
		FSlateApplication::Get().PumpMessages();
		FSlateApplication::Get().Tick();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}

	FDynamicMesh3 MakeSphere(int32 Resolution = 24)
	{
		FSphereGenerator Generator;
		Generator.Radius = 50;
		Generator.NumPhi = Resolution;
		Generator.NumTheta = Resolution * 2;
		Generator.Generate();
		return FDynamicMesh3(&Generator);
	}

	struct FCrashFixture
	{
		TSharedRef<FAdvancedPreviewScene> Scene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
		UDynamicMeshComponent* Source = nullptr;
		TSharedPtr<SUbstreamModelerViewport> Stage;
		TSharedPtr<SWindow> Window;

		FCrashFixture()
		{
			FActorSpawnParameters Spawn;
			Spawn.ObjectFlags = RF_Transient;
			AActor* Actor = Scene->GetWorld()->SpawnActor<AActor>(Spawn);
			Source = NewObject<UDynamicMeshComponent>(Actor, NAME_None, RF_Transient | RF_Transactional);
			Actor->SetRootComponent(Source);
			Actor->AddInstanceComponent(Source);
			Source->RegisterComponent();
			Stage = SNew(SUbstreamModelerViewport);
			Window = SNew(SWindow).Title(FText::FromString(TEXT("Unbound crash stress check")))
				.ClientSize(FVector2D(700, 500))[Stage.ToSharedRef()];
			FSlateApplication::Get().AddWindow(Window.ToSharedRef());
		}

		bool Load(FDynamicMesh3 Mesh)
		{
			Stage->ClearObjects();
			Source->SetMesh(MoveTemp(Mesh));
			Source->UpdateBounds();
			const bool Loaded = Stage->SetMeshComponents({Source}, TEXT("Crash fixture"), TEXT("Automation.CrashStress"), {}, true);
			Pump();
			return Loaded;
		}

		void Pump(int32 Frames = 2)
		{
			for (int32 I = 0; I < Frames; ++I)
			{
				PumpBackground();
				if (Stage)
				{
					Stage->GetUbstreamViewportClient()->Tick(1.0f / 60.0f);
					Stage->TickCSGSession();
					Stage->GetSceneViewport()->Draw();
				}
				FPlatformProcess::Sleep(0.005f);
			}
		}

		bool WaitForRemesh()
		{
			const double Deadline = FPlatformTime::Seconds() + 30;
			while (Stage->IsRemeshing() && FPlatformTime::Seconds() < Deadline) Pump(1);
			return !Stage->IsRemeshing();
		}

		UInputRouter* Router() const { return Stage->GetStageToolsContextForTesting()->InputRouter; }

		void Mouse(int32 Phase, double Radius = 50, double Offset = 0, double HeightScale = 1)
		{
			FInputDeviceState Event;
			Event.InputDevice = EInputDevices::Mouse;
			Event.Mouse.Position2D = FVector2D(350 + Offset, 250);
			Event.Mouse.Delta2D = FVector2D(1, 0);
			Event.Mouse.WorldRay = FRay(FVector(Radius * 4, Offset, Radius * 0.05 * HeightScale), FVector(-1, 0, 0));
			Event.Mouse.Left.SetStates(Phase == 0, Phase != 2, Phase == 2);
			Router()->PostInputEvent(Event);
		}

		void Close()
		{
			if (Window)
			{
				Window->SetContent(SNullWidget::NullWidget);
				Window->RequestDestroyWindow();
				Window.Reset();
			}
			Stage.Reset();
			PumpBackground();
			FlushRenderingCommands();
		}
		~FCrashFixture() { Close(); }
	};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FUbstreamCrashStressTest, "Ubstream.Modeler.CrashStress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

void FUbstreamCrashStressTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : {TEXT("SparseMeshes"), TEXT("InterruptedStrokes"), TEXT("UndoThenClose"),
		TEXT("PendingBooleanTeardown"), TEXT("CaptureTeardown"), TEXT("RemeshExtremes"), TEXT("ShutdownWaitsForWorkers"), TEXT("SourceLifetime"),
		TEXT("StrokeTransitions"), TEXT("ClosedToolUndo"), TEXT("EmptyGeometry"), TEXT("RemeshCancellation")})
	{
		Names.Add(Name);
		Commands.Add(Name);
	}
}

bool FUbstreamCrashStressTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("UWorld::DestroyActor: World has no context!.*CombinedTransformGizmoActor"),
		EAutomationExpectedErrorFlags::Contains, -1);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	GEditor->ResetTransaction(FText::FromString(TEXT("Crash stress start")));
	bool Passed = true;
	UE_LOG(LogTemp, Display, TEXT("Crash stress case: %s"), *Parameters);

	if (Parameters == TEXT("SparseMeshes"))
	{
		FCrashFixture F;
		for (int32 Variant = 0; Variant < 6; ++Variant)
		{
			FDynamicMesh3 Mesh = MakeSphere();
			double Scale = Variant == 4 ? 0.0001 : Variant == 5 ? 1000.0 : 1.0;
			if (Variant == 0)
			{
				for (int32 T = 0; T < 128; ++T) Mesh.RemoveTriangle(T);
				Passed &= TestFalse(TEXT("Fixture really has sparse IDs"), Mesh.IsCompact());
			}
			if (Variant == 1) Mesh.DiscardAttributes();
			if (Variant == 2)
			{
				const int32 A = Mesh.AppendVertex(FVector3d(-100, 0, 0));
				const int32 B = Mesh.AppendVertex(FVector3d(-100, 5, 0));
				const int32 C = Mesh.AppendVertex(FVector3d(-100, 0, 5));
				Mesh.AppendTriangle(A, B, C);
			}
			for (int32 V : Mesh.VertexIndicesItr())
			{
				FVector3d P = Mesh.GetVertex(V) * Scale;
				if (Variant == 3) P.Z *= 0.001;
				Mesh.SetVertex(V, P);
			}
			Passed &= TestTrue(TEXT("Load mesh variant"), F.Load(MoveTemp(Mesh)));
			for (int32 Brush = 0; Brush < 9; ++Brush)
			{
				UE_LOG(LogTemp, Display, TEXT("Mesh variant %d brush %d"), Variant, Brush);
				Passed &= TestTrue(TEXT("Start brush"), F.Stage->StartSculpt(static_cast<EUbstreamSculptBrush>(Brush)));
				F.Pump();
				F.Stage->SetSculptBrushSize(0.5f);
				F.Stage->SetSculptBrushStrength(0.3f);
				// Thin meshes need a ray through their equator rather than above it.
				F.Mouse(0, 50 * Scale, 0, Variant == 3 ? 0.001 : 1);
				Passed &= TestTrue(TEXT("Router captures actual stroke"), F.Router()->HasActiveMouseCapture());
				F.Pump(1);
				F.Mouse(1, 50 * Scale, 2 * Scale, Variant == 3 ? 0.001 : 1); F.Pump(1);
				F.Mouse(2, 50 * Scale, 2 * Scale, Variant == 3 ? 0.001 : 1); F.Pump(1);
				if (UUbstreamVertexSculptTool* Tool = Cast<UUbstreamVertexSculptTool>(F.Stage->GetActiveEditToolInstance()))
				{
					FDynamicMesh3 Copy;
					uint64 Revision = 0;
					Passed &= TestTrue(TEXT("Completed mesh copies"), Tool->CopyMeshIfChanged(Revision, Copy));
					Passed &= TestTrue(TEXT("Working copy has valid dense topology"), Copy.IsCompact()
						&& Copy.CheckValidity({}, EValidityCheckFailMode::ReturnOnly));
					bool Finite = true;
					for (int32 V : Copy.VertexIndicesItr()) Finite &= !Copy.GetVertex(V).ContainsNaN();
					Passed &= TestTrue(TEXT("No non-finite vertex positions"), Finite);
				}
				// Each brush gets the same surface. Earlier strokes can flatten the
				// very thin fixture out of the next brush's fixed test ray.
				F.Stage->StopSculpt(false); F.Pump();
			}
			F.Stage->StopSculpt(false); F.Pump();
		}
	}
	else if (Parameters == TEXT("InterruptedStrokes") || Parameters == TEXT("UndoThenClose"))
	{
		for (int32 Iteration = 0; Iteration < 12; ++Iteration)
		{
			UE_LOG(LogTemp, Display, TEXT("Lifecycle iteration %d"), Iteration);
			FCrashFixture F;
			Passed &= TestTrue(TEXT("Load lifecycle mesh"), F.Load(MakeSphere(40)));
			Passed &= TestTrue(TEXT("Start lifecycle sculpt"), F.Stage->StartSculpt(EUbstreamSculptBrush::Draw));
			F.Pump();
			F.Mouse(0); Passed &= TestTrue(TEXT("Stroke captured"), F.Router()->HasActiveMouseCapture());
			F.Pump(1); F.Mouse(1, 50, 3); F.Pump(1);
			if (Parameters == TEXT("UndoThenClose"))
			{
				F.Mouse(2, 50, 3);
				GEditor->UndoTransaction(); // Do not tick: the undo worker can still be running.
				if (Iteration % 2 == 0) GEditor->RedoTransaction();
				F.Close();
			}
			else
			{
				switch (Iteration % 4)
				{
				case 0: F.Stage->StopSculpt(false); F.Pump(); break;
				case 1: F.Stage->ClearObjects(); break;
				case 2: Passed &= TestTrue(TEXT("Replace mesh during stroke"), F.Load(MakeSphere())); break;
				case 3: F.Close(); break;
				}
			}
			F.Close();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
			PumpBackground();
		}
	}
	else if (Parameters == TEXT("RemeshCancellation"))
	{
		for (int32 Variant = 0; Variant < 6; ++Variant)
		{
			FCrashFixture F;
			Passed &= TestTrue(TEXT("Load async remesh fixture"), F.Load(MakeSphere()));
			if (Variant == 4)
			{
				FScopedTransaction Transaction(NSLOCTEXT("CrashStress", "MoveSource", "Move test source"));
				F.Source->Modify();
				F.Source->SetRelativeLocation(FVector(1, 0, 0));
			}
			if (Variant == 5)
			{
				Passed &= TestTrue(TEXT("Add live cutter before remesh"), F.Stage->AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box));
				Passed &= TestTrue(TEXT("Start live cut before remesh"), F.Stage->StartCSGOperation(EUbstreamCSGOperation::Subtract));
				F.Pump();
			}
			F.Stage->SetSculptTargetTriangleCount(200000);
			const int32 BeforeCount = F.Stage->GetSubjectTriangleCount();
			FString Error;
			const double Start = FPlatformTime::Seconds();
			Passed &= TestTrue(TEXT("Background remesh starts"), F.Stage->RemeshSubjectToTargetDetail(Error));
			Passed &= TestTrue(TEXT("Launch returns promptly"), FPlatformTime::Seconds() - Start < 1.0);
			Passed &= TestTrue(TEXT("Work remains asynchronous"), F.Stage->IsRemeshing());
			Passed &= TestEqual(TEXT("Original stays visible until completion"), F.Stage->GetSubjectTriangleCount(), BeforeCount);
			Passed &= TestFalse(TEXT("Duplicate remesh is rejected"), F.Stage->RemeshSubjectToTargetDetail(Error));
			Passed &= TestFalse(TEXT("Sculpt waits for remesh"), F.Stage->StartSculpt(EUbstreamSculptBrush::Draw));
			if (Variant == 0)
			{
				F.Pump(2); // Render/tick the editor while compute is still running.
				F.Stage->CancelRemesh();
				Passed &= TestTrue(TEXT("Cancellation completes"), F.WaitForRemesh());
				Passed &= TestEqual(TEXT("Cancellation retains original"), F.Stage->GetSubjectTriangleCount(), BeforeCount);
				Passed &= TestTrue(TEXT("Cancellation is explained"), F.Stage->GetRemeshStatus().Contains(TEXT("cancelled")));
			}
			else if (Variant == 1)
			{
				Passed &= TestTrue(TEXT("Replace during remesh"), F.Load(MakeSphere(16)));
				F.Stage->SetSculptTargetTriangleCount(500);
				Passed &= TestTrue(TEXT("Replacement starts its own remesh"), F.Stage->RemeshSubjectToTargetDetail(Error));
				Passed &= TestTrue(TEXT("Replacement completes despite stale callback"), F.WaitForRemesh());
				Passed &= TestTrue(TEXT("Old density never overwrites replacement"), F.Stage->GetSubjectTriangleCount() < 2000);
			}
			else if (Variant == 2)
			{
				CastChecked<UDynamicMeshComponent>(F.Stage->GetPreviewComponents()[0])->SetMesh(MakeSphere(16));
				const int32 ExternalCount = F.Stage->GetSubjectTriangleCount();
				Passed &= TestTrue(TEXT("External change cancels old snapshot"), F.WaitForRemesh());
				Passed &= TestEqual(TEXT("External geometry is preserved"), F.Stage->GetSubjectTriangleCount(), ExternalCount);
			}
			else if (Variant == 4)
			{
				GEditor->UndoTransaction();
				Passed &= TestTrue(TEXT("Undo cancels pending remesh"), F.WaitForRemesh());
				Passed &= TestEqual(TEXT("Undo cannot be overwritten by remesh"), F.Stage->GetSubjectTriangleCount(), BeforeCount);
				Passed &= TestTrue(TEXT("Undo cancellation is explicit"), F.Stage->GetRemeshStatus().Contains(TEXT("cancelled")));
			}
			else if (Variant == 5)
			{
				Passed &= TestTrue(TEXT("Live cutter preview persists while remeshing"), F.Stage->HasActiveCSGSession());
				F.Stage->CancelRemesh();
				Passed &= TestTrue(TEXT("Cancel with cutters completes"), F.WaitForRemesh());
				Passed &= TestTrue(TEXT("Cancel retains live cutters"), F.Stage->HasActiveCSGSession());
				F.Stage->SetSculptTargetTriangleCount(500);
				Passed &= TestTrue(TEXT("Remesh with cutters starts"), F.Stage->RemeshSubjectToTargetDetail(Error));
				Passed &= TestTrue(TEXT("Remesh with cutters completes"), F.WaitForRemesh());
				Passed &= TestTrue(TEXT("Success restores live cutters"), F.Stage->HasActiveCSGSession());
				Passed &= TestEqual(TEXT("Success retains cutter count"), F.Stage->GetCSGPrimitiveCount(), 1);
			}
			else
			{
				F.Close();
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
				for (int32 I = 0; I < 30; ++I) { PumpBackground(); FPlatformProcess::Sleep(0.01f); }
			}
		}
	}

	else if (Parameters == TEXT("StrokeTransitions"))
	{
		FCrashFixture F;
		Passed &= TestTrue(TEXT("Load transition mesh"), F.Load(MakeSphere()));
		for (int32 Brush = 0; Brush < 9; ++Brush)
		{
			Passed &= TestTrue(TEXT("Start transition brush"), F.Stage->StartSculpt(static_cast<EUbstreamSculptBrush>(Brush)));
			F.Pump();
			F.Mouse(0); F.Pump(1); F.Mouse(1, 50, 2); F.Pump(1);
			Passed &= TestTrue(TEXT("Transition begins during a captured drag"), F.Router()->HasActiveMouseCapture());
			Passed &= TestTrue(TEXT("Switch brush during drag"), F.Stage->StartSculpt(static_cast<EUbstreamSculptBrush>((Brush + 1) % 9)));
			F.Mouse(1, 50, 3); F.Pump(1); F.Mouse(2, 50, 3); F.Pump();
			Passed &= TestFalse(TEXT("Release ends capture after brush swap"), F.Router()->HasActiveMouseCapture());
		}
		F.Mouse(0); F.Pump(1); F.Mouse(1, 50, 2); F.Pump(1);
		F.Stage->SetSculptTargetTriangleCount(1000);
		FString Error;
		Passed &= TestTrue(TEXT("Remesh can finish an active stroke safely"), F.Stage->RemeshSubjectToTargetDetail(Error));
		Passed &= TestTrue(TEXT("Background remesh finishes"), F.WaitForRemesh());
		F.Pump();
		Passed &= TestFalse(TEXT("Remesh releases input capture"), F.Router()->HasActiveMouseCapture());
		Passed &= TestTrue(TEXT("Sculpt restarts on remesh result"), F.Stage->StartSculpt(EUbstreamSculptBrush::Draw));
		F.Pump(); F.Mouse(0); F.Pump(1); F.Mouse(2); F.Pump();
		F.Stage->StopSculpt(false); F.Pump();
	}
	else if (Parameters == TEXT("ClosedToolUndo"))
	{
		for (int32 Variant = 0; Variant < 3; ++Variant)
		{
			FCrashFixture F;
			Passed &= TestTrue(TEXT("Load closed-tool mesh"), F.Load(MakeSphere()));
			Passed &= TestTrue(TEXT("Start closed-tool sculpt"), F.Stage->StartSculpt(EUbstreamSculptBrush::Draw));
			F.Pump(); F.Mouse(0); F.Pump(1); F.Mouse(1, 50, 3); F.Pump(1); F.Mouse(2); F.Pump();
			if (Variant < 2) { F.Stage->StopSculpt(Variant == 0); F.Pump(); }
			else F.Close();
			// Exercise expired tool transactions before GC can reset editor history.
			for (int32 I = 0; I < 3; ++I) { GEditor->UndoTransaction(); F.Pump(); }
			for (int32 I = 0; I < 3; ++I) { GEditor->RedoTransaction(); F.Pump(); }
			if (F.Stage) Passed &= TestFalse(TEXT("Undo never resurrects a closed tool"), F.Stage->IsSculptActive());
			F.Close(); CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
		}
	}
	else if (Parameters == TEXT("EmptyGeometry"))
	{
		FCrashFixture F;
		F.Load(FDynamicMesh3());
		Passed &= TestFalse(TEXT("Empty geometry cannot start sculpt"), F.Stage->StartSculpt(EUbstreamSculptBrush::Draw));
		F.Pump();
		Passed &= TestFalse(TEXT("No orphan sculpt starts on empty geometry"), F.Stage->IsSculptActive());
		FString Error;
		Passed &= TestFalse(TEXT("Empty geometry cannot remesh"), F.Stage->RemeshSubjectToTargetDetail(Error));
		Passed &= TestFalse(TEXT("Empty remesh explains refusal"), Error.IsEmpty());
	}
	else if (Parameters == TEXT("PendingBooleanTeardown"))
	{
		for (int32 Iteration = 0; Iteration < 12; ++Iteration)
		{
			FCrashFixture F;
			Passed &= TestTrue(TEXT("Load boolean mesh"), F.Load(MakeSphere(40)));
			for (int32 Shape = 0; Shape < 3; ++Shape)
			{
				Passed &= TestTrue(TEXT("Add pending cutter"), F.Stage->AddCSGPrimitive(static_cast<EUbstreamCSGPrimitiveType>(Shape)));
				Passed &= TestTrue(TEXT("Start pending boolean"), F.Stage->StartCSGOperation(EUbstreamCSGOperation::Subtract));
				F.Stage->TickCSGSession();
			}
			if (Iteration % 3 == 0) F.Stage->DeleteSelectedCSGPrimitives();
			else if (Iteration % 3 == 1) F.Load(MakeSphere());
			F.Close();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
			PumpBackground();
		}
		// Drain completions from destroyed sessions; weak callbacks must be inert.
		for (int32 I = 0; I < 50; ++I) { PumpBackground(); FPlatformProcess::Sleep(0.01f); }
	}
	else if (Parameters == TEXT("CaptureTeardown"))
	{
		for (int32 Iteration = 0; Iteration < 8; ++Iteration)
		{
			FCrashFixture F;
			Passed &= TestTrue(TEXT("Load capture mesh"), F.Load(MakeSphere()));
			F.Stage->StartSculpt(EUbstreamSculptBrush::Draw); F.Pump();
			FUbstreamCaptureSettings Settings;
			Settings.Width = Settings.Height = 64;
			Settings.bCaptureDepth = true;
			auto Frame = F.Stage->BeginAsyncCaptureFramePixels(Iteration + 1, Settings);
			Passed &= TestTrue(TEXT("GPU readback starts"), Frame.IsValid());
			if (!Frame) continue;
			F.Close();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
			const double Deadline = FPlatformTime::Seconds() + 10;
			while (!Frame->IsReady() && FPlatformTime::Seconds() < Deadline) { PumpBackground(); FPlatformProcess::Sleep(0.01f); }
			Passed &= TestTrue(TEXT("Pending readback survives stage teardown"), Frame->IsReady());
			if (Frame->IsReady())
			{
				auto Completed = MakeShared<bool, ESPMode::ThreadSafe>(false);
				Frame->CompleteAndEncodeAsync([Completed](FUbstreamEditorWorldCaptureResult&& Result) { *Completed = true; });
				Frame.Reset();
				while (!*Completed && FPlatformTime::Seconds() < Deadline) { PumpBackground(); FPlatformProcess::Sleep(0.01f); }
				Passed &= TestTrue(TEXT("Encoding callback completes after owner teardown"), *Completed);
			}
		}
	}
	else if (Parameters == TEXT("ShutdownWaitsForWorkers"))
	{
		for (bool UndoWorker : {false, true})
		{
			FCrashFixture F;
			Passed &= TestTrue(TEXT("Load shutdown fixture"), F.Load(MakeSphere()));
			F.Stage->StartSculpt(EUbstreamSculptBrush::Draw); F.Pump();
			UUbstreamVertexSculptTool* Tool = Cast<UUbstreamVertexSculptTool>(F.Stage->GetActiveEditToolInstance());
			if (!TestNotNull(TEXT("Real sculpt tool exists"), Tool)) { Passed = false; continue; }
			Tool->WaitForPendingStampUpdateConst();
			Tool->WaitForPendingUndoRedoUpdate();
			auto Finished = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false);
			// Represent a slow worker without touching mesh memory after shutdown.
			// This turns a timing-dependent use-after-free into a deterministic assertion.
			TFuture<void> Pending = Async(EAsyncExecution::ThreadPool, [Finished]()
			{
				FPlatformProcess::Sleep(0.5f);
				Finished->Store(true);
			});
			if (UndoWorker) Tool->UndoUpdateFuture = MoveTemp(Pending);
			else Tool->StampUpdateOctreeFuture = MoveTemp(Pending);
			F.Stage->ClearObjects();
			Passed &= TestTrue(UndoWorker ? TEXT("Shutdown joins undo worker") : TEXT("Shutdown joins stamp worker"), Finished->Load());
			// Keep failure runs isolated too; no background test work escapes the case.
			if (UndoWorker) Tool->UndoUpdateFuture.Wait();
			else Tool->StampUpdateOctreeFuture.Wait();
		}
	}
	else if (Parameters == TEXT("SourceLifetime"))
	{
		for (bool Accept : {false, true})
		{
			FCrashFixture F;
			Passed &= TestTrue(TEXT("Load source-lifetime fixture"), F.Load(MakeSphere()));
			F.Stage->StartSculpt(EUbstreamSculptBrush::Draw); F.Pump();
			F.Mouse(0); F.Pump(1); F.Mouse(1, 50, 3); F.Pump(1); F.Mouse(2, 50, 3);
			F.Source->DestroyComponent();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
			F.Stage->StopSculpt(Accept); F.Pump();
			Passed &= TestFalse(TEXT("Deleted source cannot be resynced"), F.Stage->CanResyncFromSource());
			FString Error;
			Passed &= TestFalse(TEXT("Apply refuses deleted source"), F.Stage->ApplyEditsToSource(Error));
			Passed &= TestFalse(TEXT("Missing source has an explanation"), Error.IsEmpty());
			Passed &= TestTrue(TEXT("Detached stage remains available"), F.Stage->HasSoloMesh());
		}
	}
	else if (Parameters == TEXT("RemeshExtremes"))
	{
		FCrashFixture F;
		for (int32 Variant = 0; Variant < 3; ++Variant)
		{
			UE_LOG(LogTemp, Display, TEXT("Remesh variant %d"), Variant);
			FDynamicMesh3 Mesh = MakeSphere();
			if (Variant == 1) for (int32 V : Mesh.VertexIndicesItr()) Mesh.SetVertex(V, Mesh.GetVertex(V) * 0.0001);
			if (Variant == 2)
			{
				Mesh.Clear();
				Mesh.AppendVertex(FVector3d(0,0,0)); Mesh.AppendVertex(FVector3d(1,0,0)); Mesh.AppendVertex(FVector3d(2,0,0));
				Mesh.AppendTriangle(0,1,2); // Valid connectivity but zero area.
			}
			Passed &= TestTrue(TEXT("Load remesh edge case"), F.Load(MoveTemp(Mesh)));
			F.Stage->SetSculptTargetTriangleCount(Variant == 0 ? 200000 : 500);
			FString Error;
			bool Remeshed = F.Stage->RemeshSubjectToTargetDetail(Error);
			if (Remeshed) Passed &= TestTrue(TEXT("Background remesh finishes"), F.WaitForRemesh());
			if (Variant < 2) Passed &= TestTrue(TEXT("Remesh finite nonzero geometry"), Remeshed);
			else Passed &= TestTrue(TEXT("Degenerate mesh is rejected with an explanation"), !Remeshed && !Error.IsEmpty());
			if (Remeshed) Passed &= TestTrue(TEXT("Triangle growth stays bounded"), F.Stage->GetSubjectTriangleCount() <= F.Stage->GetSculptTargetTriangleCount() * 4);
			F.Pump();
			if (Remeshed)
			{
				Passed &= TestTrue(TEXT("Remesh result starts sculpt"), F.Stage->StartSculpt(EUbstreamSculptBrush::Draw));
				F.Pump();
				const double Radius = Variant == 1 ? 0.005 : 50.0;
				F.Mouse(0, Radius); Passed &= TestTrue(TEXT("Dense/tiny result accepts a stroke"), F.Router()->HasActiveMouseCapture());
				F.Pump(1); F.Mouse(1, Radius, Radius * 0.04); F.Pump(1); F.Mouse(2, Radius, Radius * 0.04);
				F.Stage->StopSculpt(false); F.Pump();
			}
		}
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	PumpBackground();
	return Passed;
}
#endif
