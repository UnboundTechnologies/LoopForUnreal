#include "Async/TaskGraphInterfaces.h"
#include "AdvancedPreviewScene.h"
#include "Capture/UbstreamEditorWorldCaptureService.h"
#include "RenderingThread.h"
#include "Slate/SceneViewport.h"
#include "Viewport/UbstreamViewportClient.h"
#include "Transport/UbstreamWebSocketClient.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Capture/UbstreamCaptureState.h"
#include "Capture/UbstreamCaptureService.h"
#include "Components/DynamicMeshComponent.h"
#include "Editing/UbstreamVertexSculptTool.h"
#include "Generators/MinimalBoxMeshGenerator.h"
#include "Geometry/UbstreamCSGSession.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "IWebSocket.h"
#include "Interfaces/IHttpRequest.h"
#include "MeshGen/UbstreamMeshGenService.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SUbstreamControlPanel.h"
#include "Widgets/SUbstreamModelerViewport.h"
#include "Settings/UbstreamModelerSettings.h"
#include "Utils/UbstreamImageResourceNames.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Control button state without moving the user's OS cursor or opening a window.
	class FCursorTestViewport final : public FSceneViewport
	{
	public:
		FCursorTestViewport() : FSceneViewport(static_cast<FViewportClient*>(nullptr), nullptr) {}
		FKey HeldButton;
		bool KeyState(FKey Key) const override { return Key == HeldButton; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamCursorLifecycleTest,
	"Ubstream.Modeler.Lifecycle.CursorState", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamCursorLifecycleTest::RunTest(const FString& Parameters)
{
	const auto Stage = SNew(SUbstreamModelerViewport);
	const auto Client = Stage->GetUbstreamViewportClient();
	if (!TestTrue(TEXT("Stage viewport client exists"), Client.IsValid())) return false;
	FCursorTestViewport Input;
	for (const FKey Button : { EKeys::LeftMouseButton, EKeys::MiddleMouseButton, EKeys::RightMouseButton })
	{
		Input.HeldButton = Button;
		Client->SetRequiredCursorOverride(false);
		Client->SetRequiredCursor(false, false);
		TestEqual(*FString::Printf(TEXT("%s drag honors hidden hardware cursor"), *Button.ToString()),
			Client->GetCursor(&Input, 100, 100), EMouseCursor::None);
		Client->SetRequiredCursor(false, true);
		TestEqual(TEXT("Software-cursor drags do not show a second hardware cursor"),
			Client->GetCursor(&Input, 100, 100), EMouseCursor::None);
		Client->SetRequiredCursor(true, false);
		Client->SetRequiredCursorOverride(true, EMouseCursor::CardinalCross);
		TestEqual(TEXT("Visible transform cursor keeps Unreal's requested shape"),
			Client->GetCursor(&Input, 100, 100), EMouseCursor::CardinalCross);
		Input.HeldButton = FKey();
		Client->SetRequiredCursorOverride(false);
		Client->SetRequiredCursor(false, false);
		TestEqual(TEXT("Hidden cursor is honored at mouse release"),
			Client->GetCursor(&Input, 100, 100), EMouseCursor::None);
		Client->SetRequiredCursor(true, false);
		Client->SetRequiredCursorOverride(true, EMouseCursor::Crosshairs);
		TestEqual(TEXT("Visibility changes refresh without moving the mouse"),
			Client->GetCursor(&Input, 100, 100), EMouseCursor::Crosshairs);
	}
	Client->SetRequiredCursor(true, false);
	Client->SetRequiredCursorOverride(false);
	return true;
}

namespace
{
	// Transport lifetime test double: no network or credentials are involved.
	class FConnectingTestSocket final : public IWebSocket
	{
	public:
		int32 CloseCount = 0;
		virtual void Connect() override {}
		virtual void Close(int32, const FString&) override { ++CloseCount; }
		virtual bool IsConnected() override { return false; }
		virtual void Send(const FString&) override {}
		virtual void Send(const void*, SIZE_T, bool) override {}
		virtual void SetTextMessageMemoryLimit(uint64) override {}
		FWebSocketConnectedEvent OnConnectedEvent;
		virtual FWebSocketConnectedEvent& OnConnected() override { return OnConnectedEvent; }
		FWebSocketConnectionErrorEvent OnConnectionErrorEvent;
		virtual FWebSocketConnectionErrorEvent& OnConnectionError() override { return OnConnectionErrorEvent; }
		FWebSocketClosedEvent OnClosedEvent;
		virtual FWebSocketClosedEvent& OnClosed() override { return OnClosedEvent; }
		FWebSocketMessageEvent OnMessageEvent;
		virtual FWebSocketMessageEvent& OnMessage() override { return OnMessageEvent; }
		FWebSocketBinaryMessageEvent OnBinaryMessageEvent;
		virtual FWebSocketBinaryMessageEvent& OnBinaryMessage() override { return OnBinaryMessageEvent; }
		FWebSocketRawMessageEvent OnRawMessageEvent;
		virtual FWebSocketRawMessageEvent& OnRawMessage() override { return OnRawMessageEvent; }
		FWebSocketMessageSentEvent OnMessageSentEvent;
		virtual FWebSocketMessageSentEvent& OnMessageSent() override { return OnMessageSentEvent; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamSocketLifecycleTest,
	"Ubstream.Modeler.Lifecycle.SocketHandshake", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamSocketLifecycleTest::RunTest(const FString& Parameters)
{
	const auto Socket = MakeShared<FConnectingTestSocket>();
	const auto Client = MakeShared<FUbstreamWebSocketClient>();
	Client->Socket = Socket;
	Client->BindSocketEvents();
	Client->Disconnect();
	TestEqual(TEXT("Disconnect closes an unfinished handshake"), Socket->CloseCount, 1);
	TestFalse(TEXT("Disconnected socket has no connected callback"), Socket->OnConnected().IsBound());
	TestFalse(TEXT("Disconnected socket has no error callback"), Socket->OnConnectionError().IsBound());
	TestFalse(TEXT("Disconnected socket has no closed callback"), Socket->OnClosed().IsBound());
	TestFalse(TEXT("Disconnected socket has no message callback"), Socket->OnMessage().IsBound());
	TestFalse(TEXT("Disconnected socket has no binary callback"), Socket->OnBinaryMessage().IsBound());
	Client->Disconnect();
	TestEqual(TEXT("Repeated disconnect is harmless"), Socket->CloseCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamCaptureLifecycleTest,
	"Ubstream.Modeler.Lifecycle.Capture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamCaptureLifecycleTest::RunTest(const FString& Parameters)
{
	FUbstreamCaptureState State;
	const FUbstreamCaptureTicket MovingFrame = State.BeginAsync(100);
	State.MarkDirty(); // Camera moves after capture, before readback/encoding finishes.
	TestTrue(TEXT("First encode completes"), State.FinishAsync(MovingFrame));
	State.RememberUploaded(MovingFrame, 500);
	TestTrue(TEXT("Uploading the old frame preserves a newer edit"), State.IsDirty());
	TestTrue(TEXT("Uploaded signature belongs to capture time"), State.HasSceneChanged(101));

	const FUbstreamCaptureTicket RestingFrame = State.BeginAsync(101);
	TestTrue(TEXT("Resting frame completes"), State.FinishAsync(RestingFrame));
	State.RememberUploaded(RestingFrame, 500);
	TestFalse(TEXT("Current frame clears dirtiness"), State.IsDirty());
	TestFalse(TEXT("Current frame clears scene mismatch"), State.HasSceneChanged(101));

	const FUbstreamCaptureTicket PendingReadback = State.BeginAsync(102);
	State.Cancel(); // Pause while the GPU still owns the frame.
	TestFalse(TEXT("Paused readback cannot complete"), State.FinishAsync(PendingReadback));
	State.ResetUploads();
	const FUbstreamCaptureTicket Restarted = State.BeginAsync(103);
	TestFalse(TEXT("Old encode cannot finish a restarted capture"), State.FinishAsync(PendingReadback));
	TestTrue(TEXT("Restarted capture still owns the busy guard"), State.IsBusy());
	State.RememberUploaded(PendingReadback, 500);
	TestFalse(TEXT("Old capture cannot overwrite upload state"), State.HasUploaded());
	TestTrue(TEXT("Restarted capture completes"), State.FinishAsync(Restarted));
	State.RememberUploaded(Restarted, 500);
	TestFalse(TEXT("Restart is clean after its own frame"), State.IsDirty());

	TestTrue(TEXT("UBS1 admits the first frame"), FUbstreamCaptureState::HasSendCapacity(false, 0, 1));
	TestFalse(TEXT("UBS1 waits at the configured window"), FUbstreamCaptureState::HasSendCapacity(false, 1, 1));
	TestTrue(TEXT("UBS1 resumes when an ack removes a frame"), FUbstreamCaptureState::HasSendCapacity(false, 1, 2));
	TestTrue(TEXT("FastStream never stalls on dropped frames"), FUbstreamCaptureState::HasSendCapacity(true, 100, 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamMeshGenLifecycleTest,
	"Ubstream.Modeler.Lifecycle.MeshGeneration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamMeshGenLifecycleTest::RunTest(const FString& Parameters)
{
	const auto Service = MakeShared<FUbstreamMeshGenService, ESPMode::ThreadSafe>();
	Service->bBusy = true;
	Service->Progress.TaskId = TEXT("existing-job");
	Service->Progress.Phase = EUbstreamMeshGenPhase::Reconstruction;
	for (int32 Attempt = 0; Attempt < 5; ++Attempt)
		TestTrue(TEXT("Transient read failures retry the same job"), Service->RetryReadRequest(503, TEXT("5")));
	TestTrue(TEXT("Retry keeps the remote task identity"), Service->Progress.TaskId == TEXT("existing-job"));
	TestTrue(TEXT("Retry keeps generation busy"), Service->IsBusy());
	TestFalse(TEXT("Read retries are bounded"), Service->RetryReadRequest(503));
	Service->ReadRetryCount = 0;
	TestFalse(TEXT("Invalid credentials are not retried"), Service->RetryReadRequest(401));
	Service->bReadRetryPending = false;
	Service->CancelPollTicker();
	const FHttpRequestPtr CancelledRequest = FHttpModule::Get().CreateRequest();
	const FHttpRequestPtr NewRequest = FHttpModule::Get().CreateRequest();
	Service->bBusy = true;
	Service->bCancelRequested = false; // State after immediately retrying a cancelled job.
	Service->InflightRequest = NewRequest;
	Service->Progress.Phase = EUbstreamMeshGenPhase::Uploading;
	int32 CompletionCount = 0;
	Service->OnCompleted.AddLambda([&CompletionCount](const FUbstreamMeshGenResult&) { ++CompletionCount; });

	// No network: deliver delayed failures directly, as Unreal's HTTP manager does.
	Service->HandleCreateTaskComplete(CancelledRequest, nullptr, false);
	Service->HandlePollTaskComplete(CancelledRequest, nullptr, false);
	Service->HandleDownloadComplete(CancelledRequest, nullptr, false);
	TestTrue(TEXT("Old callbacks preserve the replacement request"), Service->InflightRequest == NewRequest);
	TestTrue(TEXT("New job stays busy"), Service->IsBusy());
	TestEqual(TEXT("No old callback completes the new job"), CompletionCount, 0);
	TestTrue(TEXT("New job's phase is unchanged"), Service->Progress.Phase == EUbstreamMeshGenPhase::Uploading);

	AddExpectedError(TEXT("Mesh gen failed"), EAutomationExpectedErrorFlags::Contains, 1);
	Service->HandleCreateTaskComplete(NewRequest, nullptr, false);
	TestFalse(TEXT("Active request failure is still handled"), Service->IsBusy());
	TestEqual(TEXT("Active failure completes exactly once"), CompletionCount, 1);
	Service->HandleCreateTaskComplete(NewRequest, nullptr, false);
	TestEqual(TEXT("Duplicate callback is ignored"), CompletionCount, 1);
	// A server that never changes phase must not leave the panel busy forever.
	Service->bBusy = true;
	Service->ActiveRequest.StallTimeoutSeconds = 60.0;
	Service->Progress.Phase = EUbstreamMeshGenPhase::Reconstruction;
	Service->PhaseStartTimeSeconds = FPlatformTime::Seconds() - 61.0;
	Service->InflightRequest = NewRequest;
	AddExpectedError(TEXT("Mesh gen failed"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Stalled job stops ticking"), Service->HandlePollTicker(0.01f));
	TestFalse(TEXT("Stalled job releases busy state"), Service->IsBusy());
	TestFalse(TEXT("Stalled request is released"), Service->InflightRequest.IsValid());
	TestEqual(TEXT("Timeout completes once"), CompletionCount, 2);
	Service->HandlePollTaskComplete(NewRequest, nullptr, false);
	TestEqual(TEXT("Late timeout response is ignored"), CompletionCount, 2);
	Service->bBusy = true;
	Service->ActiveRequest.StallTimeoutSeconds = 0.0;
	TestTrue(TEXT("Long-job timeout can be disabled"), Service->HandlePollTicker(0.01f));
	Service->ActiveRequest.StallTimeoutSeconds = 60.0;
	Service->SetPhase(EUbstreamMeshGenPhase::Exporting);
	TestTrue(TEXT("Real phase change resets waiting deadline"), Service->HandlePollTicker(0.01f));
	Service->bBusy = false;
	Service->OnCompleted.Clear();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamCapturePanelLifecycleTest,
	"Ubstream.Modeler.Lifecycle.PanelPause", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamCapturePanelLifecycleTest::RunTest(const FString& Parameters)
{
	// Exercise lifecycle methods without constructing the UI or opening a socket.
	const auto Panel = MakeShared<SUbstreamControlPanel>();
	TestTrue(TEXT("Retryable service close reconnects even with a clean handshake"), Panel->ShouldAutoReconnectAfterClose(1013, TEXT("verification unavailable; retry"), true));
	TestFalse(TEXT("Revoked credentials never cause a reconnect loop"), Panel->ShouldAutoReconnectAfterClose(1008, TEXT("auth failed"), true));
	Panel->bManualDisconnectDuringCurrentWorkspace = true;
	TestFalse(TEXT("Manual disconnect takes priority over retries"), Panel->ShouldAutoReconnectAfterClose(1013, TEXT("retry"), false));
	Panel->bManualDisconnectDuringCurrentWorkspace = false;

	UUbstreamModelerSettings* Settings = GetMutableDefault<UUbstreamModelerSettings>();
	TGuardValue<EUnboundLoopWebSocketProtocol> ProtocolGuard(Settings->WebSocketProtocol, EUnboundLoopWebSocketProtocol::UbstreamEnvelope);
	TGuardValue<float> FpsGuard(Settings->StreamFps, 5.0f);
	TestEqual(TEXT("Remote cadence reads project settings"), Panel->GetStreamIntervalSeconds(), 0.2f);
	Settings->StreamFps = 100.0f;
	TestEqual(TEXT("Remote cadence retains the server safety cap"), Panel->GetStreamIntervalSeconds(), 1.0f / 15.0f);
	Settings->StreamFps = 0.0f;
	TestEqual(TEXT("Invalid low FPS stays bounded"), Panel->GetStreamIntervalSeconds(), 5.0f);
	Settings->WebSocketProtocol = EUnboundLoopWebSocketProtocol::FastStream;
	TestEqual(TEXT("Offline passthrough retains its interactive cadence"), Panel->GetStreamIntervalSeconds(), 1.0f / 24.0f);
	Settings->WebSocketProtocol = EUnboundLoopWebSocketProtocol::UbstreamEnvelope;
	Panel->bStreaming = true;
	Panel->RegisteredStreamIntervalSeconds = 1.0f / 15.0f;
	Panel->InFlightFrames.Add(42);
	Panel->FrameSendTimes.Add(42, 1.0);
	Settings->StreamFps = 5.0f;
	TestFalse(TEXT("FPS change replaces the old ticker"), Panel->HandleStreamTicker(0.0f));
	TestEqual(TEXT("Ticker adopts configured cadence"), Panel->RegisteredStreamIntervalSeconds, 0.2f);
	TestTrue(TEXT("FPS change preserves pending acknowledgements"), Panel->InFlightFrames.Contains(42));
	TestTrue(TEXT("FPS change preserves acknowledgement timing"), Panel->FrameSendTimes.Contains(42));
	const FUbstreamCaptureTicket OldTicket = Panel->CaptureState.BeginAsync(100);
	Panel->PendingFrameReadback = MakeShared<FUbstreamAsyncFrameReadback, ESPMode::ThreadSafe>();
	Panel->StopStreaming(TEXT("Paused by lifecycle test"));
	TestFalse(TEXT("Pause releases the pending GPU readback"), Panel->PendingFrameReadback.IsValid());
	TestFalse(TEXT("Pause clears the capture guard"), Panel->CaptureState.IsBusy());
	TestFalse(TEXT("Pause stops streaming"), Panel->bStreaming);
	Panel->TryCompletePendingFrameReadback();
	TestFalse(TEXT("Widget ticks cannot revive paused readbacks"), Panel->CaptureState.IsBusy());

	const FUbstreamCaptureTicket NewTicket = Panel->CaptureState.BeginAsync(101);
	FUbstreamEditorWorldCaptureResult OldResult;
	OldResult.bSuccess = true;
	OldResult.FrameId = 999;
	Panel->FinishAsyncCaptureAndSend(MoveTemp(OldResult), true, OldTicket);
	TestTrue(TEXT("Late encode does not clear the new capture"), Panel->CaptureState.IsBusy());
	TestTrue(TEXT("Late encode does not replace the displayed capture"), Panel->LastCapturedFrame.FrameId != 999);
	Panel->ClearCaptureWorkspaceState();
	TestFalse(TEXT("Workspace teardown also invalidates captures while paused"), Panel->CaptureState.FinishAsync(NewTicket));
	const auto WaitingTicket = Panel->CaptureState.BeginAsync(102);
	Panel->PendingFrameReadback = MakeShared<FUbstreamAsyncFrameReadback, ESPMode::ThreadSafe>();
	Panel->PendingFrameReadback->BeginTimeSeconds = FPlatformTime::Seconds();
	Panel->TryCompletePendingFrameReadback();
	TestTrue(TEXT("Unready readback within deadline remains pending"), Panel->PendingFrameReadback.IsValid());
	TestTrue(TEXT("Unready readback within deadline keeps its capture"), Panel->CaptureState.IsBusy());
	Panel->PendingFrameReadback->BeginTimeSeconds = FPlatformTime::Seconds() - 3.0;
	Panel->TryCompletePendingFrameReadback();
	TestFalse(TEXT("Unready readback past deadline is released"), Panel->PendingFrameReadback.IsValid());
	TestFalse(TEXT("Expired unready readback clears busy state"), Panel->CaptureState.IsBusy());
	TestFalse(TEXT("Expired unready readback invalidates its ticket"), Panel->CaptureState.FinishAsync(WaitingTicket));
	TestTrue(TEXT("Expired unready readback remains dirty for retry"), Panel->CaptureState.IsDirty());
	// Direct startup/local-preview routes must defer too, not only StreamLatestFrame.
	const auto Scene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
	const auto Viewport = SNew(SUbstreamModelerViewport);
	FCoreRenderDelegates::OnFlushRenderingCommandsStart.Broadcast();
	FUbstreamCaptureSettings CaptureSettings;
	CaptureSettings.Width = CaptureSettings.Height = 64;
	TestTrue(TEXT("Direct synchronous streaming defers inside flush"), Panel->CaptureAndSendFrame(false, true));
	TestTrue(TEXT("Direct asynchronous streaming defers inside flush"), Panel->BeginAsyncCaptureAndSend(false));
	TestFalse(TEXT("Deferral does not begin an async capture"), Panel->CaptureState.IsBusy());
	TestTrue(TEXT("Deferred frame remains dirty"), Panel->CaptureState.IsDirty());
	TestTrue(TEXT("RGB capture rejects nested flush"), FUbstreamCaptureService::CaptureRgb(*Scene, FUbstreamCaptureCamera(), CaptureSettings).ErrorMessage.Contains(TEXT("deferred")));
	TestTrue(TEXT("Pixel capture rejects nested flush"), FUbstreamCaptureService::CaptureFramePixels(*Scene, FUbstreamCaptureCamera(), 1, CaptureSettings, {}).ErrorMessage.Contains(TEXT("deferred")));
	TestTrue(TEXT("Editor capture rejects nested flush"), FUbstreamEditorWorldCaptureService::CaptureFrameBundle(1, CaptureSettings).ErrorMessage.Contains(TEXT("deferred")));
	TestTrue(TEXT("Viewport pixel capture defers before scene preparation"), Viewport->CaptureFramePixels(1, CaptureSettings).ErrorMessage.Contains(TEXT("deferred")));
	TestTrue(TEXT("Viewport bundle capture defers before scene preparation"), Viewport->CaptureFrameBundle(1, CaptureSettings).ErrorMessage.Contains(TEXT("deferred")));
	TestTrue(TEXT("Manual RGB capture defers"), Viewport->CaptureRgbFrame().Contains(TEXT("deferred")));
	TestFalse(TEXT("Viewport async capture defers before scene preparation"), Viewport->BeginAsyncCaptureFramePixels(1, CaptureSettings).IsValid());
	FCoreRenderDelegates::OnFlushRenderingCommandsEnd.Broadcast();

	// Exercise the actual bound socket event handlers with no loaded workspace.
	const auto BoundPanel = SNew(SUbstreamControlPanel);
	BoundPanel->bManualDisconnectDuringCurrentWorkspace = true;
	for (bool bError : {false, true})
	{
		BoundPanel->bStreaming = true;
		const auto Ticket = BoundPanel->CaptureState.BeginAsync(500);
		BoundPanel->PendingFrameReadback = MakeShared<FUbstreamAsyncFrameReadback, ESPMode::ThreadSafe>();
		if (bError)
		{
			BoundPanel->WebSocketClient->OnConnectionError.Broadcast(TEXT("Offline test"));
		}
		else
		{
			BoundPanel->WebSocketClient->OnClosed.Broadcast(1000, FString(), true);
		}
		TestFalse(TEXT("Socket loss releases pending readback"), BoundPanel->PendingFrameReadback.IsValid());
		TestFalse(TEXT("Socket loss invalidates old encode ticket"), BoundPanel->CaptureState.FinishAsync(Ticket));
		TestFalse(TEXT("Socket loss stops the old ticker"), BoundPanel->StreamTickerHandle.IsValid());
	}
	return true;
}

namespace
{
	UE::Geometry::FDynamicMesh3 MakeLifecycleBox(const FVector3d& Center, const FVector3d& Extent)
	{
		UE::Geometry::FMinimalBoxMeshGenerator Generator;
		Generator.Box = UE::Geometry::FOrientedBox3d(Center, Extent);
		Generator.Generate();
		return UE::Geometry::FDynamicMesh3(&Generator);
	}

	bool WaitForSession(FUbstreamCSGSession& Session)
	{
		const double Deadline = FPlatformTime::Seconds() + 10.0;
		while (Session.HasPendingWork() && FPlatformTime::Seconds() < Deadline)
		{
			Session.Tick();
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.001f);
		}
		return !Session.HasPendingWork();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamCSGSubjectRevisionTest,
	"Ubstream.Modeler.Lifecycle.CSGSubjectRevision", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamCSGSubjectRevisionTest::RunTest(const FString& Parameters)
{
	const auto Session = MakeShared<FUbstreamCSGSession, ESPMode::ThreadSafe>();
	TArray<FUbstreamCSGSessionBrush> Brushes;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		FUbstreamCSGSessionBrush& Brush = Brushes.AddDefaulted_GetRef();
		Brush.Mesh = MakeLifecycleBox(FVector3d(50.0 * Index, 0, 0), FVector3d(15, 15, 200));
	}
	Session->Begin(MakeLifecycleBox(FVector3d::Zero(), FVector3d(100, 100, 100)), FTransform::Identity, MoveTemp(Brushes));
	int32 PreviewCount = 0;
	Session->OnPreviewReady.BindLambda([&PreviewCount](const FUbstreamCSGResult&) { ++PreviewCount; });
	Session->Tick();
	// Completion is posted to the game thread, so this edit is guaranteed to
	// occur before the first worker result can be accepted, even on a fast CPU.
	Session->UpdateSubjectMesh(MakeLifecycleBox(FVector3d(500, 0, 0), FVector3d(100, 100, 100)));
	TestTrue(TEXT("Updated chain completes"), WaitForSession(*Session));
	const FUbstreamCSGResult* Result = Session->GetLatestResult();
	if (TestNotNull(TEXT("Updated chain has a result"), Result))
	{
		TestTrue(TEXT("Updated chain succeeds"), Result->bSuccess);
		TestTrue(TEXT("New subject, not the old cached base, survives"), Result->Mesh.GetBounds().Min.X > 399.0);
	}
	TestEqual(TEXT("Stale subject result was never published"), PreviewCount, 1);
	TestEqual(TEXT("Subject edit recomputes the whole chain"), Session->GetLastComputeStartIndexForTesting(), 0);
	Session->SetBrushTransform(1, FTransform(FVector(0, 5, 0)));
	TestTrue(TEXT("Later brush edit completes"), WaitForSession(*Session));
	TestEqual(TEXT("Valid prefix caching still works"), Session->GetLastComputeStartIndexForTesting(), 1);
	Session->OnPreviewReady.Unbind();
	Session->End();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamSculptRevisionTest,
	"Ubstream.Modeler.Lifecycle.SculptRevision", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamSculptRevisionTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UUbstreamVertexSculptTool> Tool(NewObject<UUbstreamVertexSculptTool>());
	Tool->DynamicMeshComponent = NewObject<UDynamicMeshComponent>(Tool.Get());
	UE::Geometry::FDynamicMesh3 Mesh;
	for (int32 Index = 0; Index < 1024; ++Index)
	{
		Mesh.AppendVertex(FVector3d(Index, 0, 0));
	}
	Tool->DynamicMeshComponent->SetMesh(MoveTemp(Mesh));
	Tool->BindMeshChanges();
	TPromise<void> ReadyUndo;
	Tool->UndoUpdateFuture = ReadyUndo.GetFuture();
	ReadyUndo.SetValue();
	uint64 Revision = 0;
	UE::Geometry::FDynamicMesh3 Copy;
	TestTrue(TEXT("First revision is copied"), Tool->CopyMeshIfChanged(Revision, Copy));
	Copy.SetVertex(1, FVector3d(-1, -1, -1)); // Sentinel detects unwanted idle copies.
	TestFalse(TEXT("Idle tick skips copy"), Tool->CopyMeshIfChanged(Revision, Copy));
	TestTrue(TEXT("Idle tick leaves destination untouched"), Copy.GetVertex(1).Equals(FVector3d(-1, -1, -1)));

	// Vertex 1 is excluded by the old 64-sample fingerprint (stride 16).
	Tool->DynamicMeshComponent->GetMesh()->SetVertex(1, FVector3d(1, 2, 3));
	Tool->DynamicMeshComponent->OnMeshRegionChanged.Broadcast(Tool->DynamicMeshComponent, nullptr, true);
	TestTrue(TEXT("Unsampled vertex change is copied after undo/redo notification"), Tool->CopyMeshIfChanged(Revision, Copy));
	TestTrue(TEXT("Copy includes the small edit"), Copy.GetVertex(1).Equals(FVector3d(1, 2, 3)));
	Tool->bInStroke = true;
	++Tool->MeshRevision;
	TestFalse(TEXT("Active stroke defers copying"), Tool->CopyMeshIfChanged(Revision, Copy));
	Tool->bInStroke = false;
	TestTrue(TEXT("Completed stroke is copied"), Tool->CopyMeshIfChanged(Revision, Copy));
	uint64 CaptureRevision = 0;
	TestTrue(TEXT("Idle sculpt publishes a capture revision"), Tool->TryGetStableCaptureRevision(Tool->DynamicMeshComponent, CaptureRevision));
	const uint64 BeforeIdleRevision = CaptureRevision;
	TestTrue(TEXT("Repeated idle revision query succeeds"), Tool->TryGetStableCaptureRevision(Tool->DynamicMeshComponent, CaptureRevision));
	TestEqual(TEXT("Idle revision is stable"), CaptureRevision, BeforeIdleRevision);
	Tool->bInStroke = true;
	TestFalse(TEXT("Live stroke bypasses idle optimization"), Tool->TryGetStableCaptureRevision(Tool->DynamicMeshComponent, CaptureRevision));
	Tool->bInStroke = false;
	Tool->bIsStampPending = true;
	TestFalse(TEXT("Pending stamp bypasses idle optimization"), Tool->TryGetStableCaptureRevision(Tool->DynamicMeshComponent, CaptureRevision));
	Tool->bIsStampPending = false;
	TestFalse(TEXT("Unrelated meshes cannot use the sculpt revision"), Tool->TryGetStableCaptureRevision(nullptr, CaptureRevision));
	Tool->DynamicMeshComponent->OnMeshChanged.Broadcast();
	TestTrue(TEXT("General mesh edits publish a capture revision"), Tool->TryGetStableCaptureRevision(Tool->DynamicMeshComponent, CaptureRevision));
	TestTrue(TEXT("General edit invalidates idle revision"), CaptureRevision != BeforeIdleRevision);
	Tool->DynamicMeshComponent->OnMeshChanged.Remove(Tool->MeshChangedHandle);
	Tool->DynamicMeshComponent->OnMeshRegionChanged.Remove(Tool->MeshRegionChangedHandle);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamImageResourceNameTest,
	"Ubstream.Modeler.Performance.ImageResourceNames", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamImageResourceNameTest::RunTest(const FString& Parameters)
{
	const FName First = UbstreamImageResourceNames::Next(TEXT("ResourceNameTest"));
	FName Previous = First;
	bool bSameBaseEntry = true;
	bool bUniqueNumbers = true;
	// One hour of frames at 15 FPS must share one interned string while each
	// renderer resource remains distinct (old brushes may still be referenced).
	for (int32 Frame = 1; Frame < 54000; ++Frame)
	{
		const FName Next = UbstreamImageResourceNames::Next(TEXT("ResourceNameTest"));
		bSameBaseEntry &= Next.GetComparisonIndex() == First.GetComparisonIndex();
		bUniqueNumbers &= Next.GetNumber() == Previous.GetNumber() + 1;
		Previous = Next;
	}
	TestTrue(TEXT("54k frame names share one base name entry"), bSameBaseEntry);
	TestTrue(TEXT("Every frame still has a unique resource name"), bUniqueNumbers);
	TestTrue(TEXT("Different resource kinds stay distinct"),
		UbstreamImageResourceNames::Next(TEXT("OtherResourceNameTest")).GetComparisonIndex() != First.GetComparisonIndex());
	return true;
}

#endif
