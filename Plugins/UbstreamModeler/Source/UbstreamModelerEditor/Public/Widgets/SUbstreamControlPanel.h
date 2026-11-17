#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Capture/UbstreamCaptureTargetPool.h"
#include "Capture/UbstreamCaptureState.h"
#include "Capture/UbstreamEditorWorldCaptureService.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"

class FActiveTimerHandle;
class FUbstreamWebSocketClient;
class AActor;
class SMultiLineEditableTextBox;
class SEditableTextBox;
class SBox;
class SExpandableArea;
class SHorizontalBox;
class SImage;
class SSlider;
class STextBlock;
class SUbstreamModelerViewport;
class UUbstreamModelerSettings;
struct FCaptureLostEvent;
struct FGeometry;
struct FPropertyChangedEvent;
struct FPointerEvent;
struct FSlateDynamicImageBrush;

class SUbstreamControlPanel final : public SCompoundWidget
{
public:
	virtual void Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime) override;
	SLATE_BEGIN_ARGS(SUbstreamControlPanel) {}
		SLATE_ARGUMENT(TWeakPtr<SUbstreamModelerViewport>, SoloViewport)
		/** Invoked by the empty-workspace card to load the current editor selection into the stage. */
		SLATE_EVENT(FSimpleDelegate, OnLoadSelectedRequested)
	SLATE_END_ARGS()

	~SUbstreamControlPanel() override;

	void Construct(const FArguments& InArgs);
	FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	void OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;
	void NotifyCaptureWorkspaceChanged();
	/** Cheap per-edit notification: stream dirtiness only. */
	void NotifyStageContentDirty();
	void ClearCaptureWorkspaceState();
	void HandleAuthenticationChanged();

private:
	friend class FUbstreamCapturePanelLifecycleTest;
	friend class FUbstreamAsyncCaptureReuseTest;
	friend class FUbstreamPreviewReuseTest;
	friend class FUbstreamStreamProfileTest;
	enum class EWorkflowState : uint8
	{
		Inactive,
		Disconnected,
		Connecting,
		Loading,
		Ready,
		Streaming,
		Paused,
		Warning,
		Error
	};

	struct FUbstreamFrameSnapshot
	{
		uint64 FrameId = 0;
		FString Prompt;
		int32 Seed = 0;
		FDateTime CapturedAtUtc;
		TArray<uint8> ImageBytes;
		TSharedPtr<FSlateDynamicImageBrush> Brush;
		/** True when the snapshot was taken from a server-returned frame (not local passthrough). */
		bool bFromServer = false;
	};

	TSharedRef<SWidget> BuildPanelContent();
	TSharedRef<SWidget> BuildModeGate();
	TSharedRef<SWidget> BuildActiveModePanel();
	TSharedRef<SWidget> BuildReturnedFrameSection();
	TSharedRef<SWidget> BuildSnapshotSection();
	TSharedRef<SWidget> BuildMeshGenSection();
	TSharedRef<SWidget> BuildPromptSection();
	TSharedRef<SWidget> BuildConnectionSection();
	TSharedRef<SWidget> BuildLogsSection();

	TArray<uint8> GetMeshGenSourceImageBytes() const;
	/** True when 3D gen may run: non-empty prompt, not passthrough, and a server frame (or server snapshot). */
	bool HasMeshGenSourceImage() const;
	FString GetMeshGenSourceImageLabel() const;
	FString GetMeshGenBlockedReason() const;
	void RefreshMeshGenSourcePreview();
	FReply GenerateMeshFromPreviewClicked();
	bool IsGenerateMeshFromPreviewEnabled() const;

	FReply ConnectClicked();
	FReply DisconnectClicked();
	FReply HandleGateLoadSelectedClicked();
	FReply UseSelectedMeshClicked();
	FReply GenerateClicked();
	FReply TogglePauseClicked();
	FReply RerollSeedClicked();
	FReply SnapshotClicked();
	FReply BackToLiveClicked();
	FReply ImportDisplayedFrameClicked();
	FReply ClearSnapshotsClicked();
	FReply SaveSettingsClicked();
	FReply OpenSettingsClicked();
	FReply HandlePreviewResizeMouseDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);

	bool IsModelingModeActive() const;
	bool IsFastStreamProtocol() const;
	EVisibility GetModeGateVisibility() const;
	EVisibility GetModelingControlsVisibility() const;
	EVisibility GetReturnedFramePlaceholderVisibility() const;
	EVisibility GetLoadingIndicatorVisibility() const;
	EVisibility GetPassthroughHintVisibility() const;
	EVisibility GetSnapshotSectionVisibility() const;
	EVisibility GetSnapshotViewPillVisibility() const;
	FText GetModeGateBodyText() const;
	float GetModeGateBodyWrapWidth() const;
	const FSlateBrush* GetPauseButtonIcon() const;
	FText GetPauseButtonToolTip() const;
	FText GetSnapshotCountText() const;
	FText GetViewedSnapshotLabel() const;
	FText GetSeedReadoutText() const;
	bool IsSnapshotButtonEnabled() const;
	bool IsImportButtonEnabled() const;
	bool IsViewingSnapshot() const;
	void SelectSnapshot(int32 SnapshotIndex);
	void RebuildSnapshotStrip();
	void RefreshPreviewImage();
	FSlateColor GetWorkflowStateAccentColor() const;
	bool HasSoloMeshLoaded() const;
	bool IsCaptureWorkspaceReady() const;
	bool IsLocalPassthroughPreviewMode() const;
	bool RequireCaptureSubject(const FString& ActionName);
	void EnsureLocalPassthroughPreviewRunning();
	void TryAutoConnectForWorkspace();
	void ScheduleReconnect(const FString& Reason);
	void CancelReconnect();
	bool ShouldAutoReconnectAfterClose(int32 StatusCode, const FString& Reason, bool bWasClean) const;
	bool IsNonRetryableDisconnectReason(const FString& Reason) const;
	EActiveTimerReturnType HandleReconnectTimerTick(double InCurrentTime, float InDeltaTime);
	bool CaptureEditorWorldFrame();
	bool CaptureSoloViewportFrame();
	bool PreviewCapturedFrameLocally();
	bool CaptureAndSendFrame(bool bTrackInFlight, bool bForceSend = false);
	/** Dedup -> send -> remember, on an already-populated LastCapturedFrame.
	    Shared by the synchronous and async capture paths. */
	bool SendCapturedFrame(bool bTrackInFlight, bool bForceSend);
	/** Routes solo+FastStream streaming to the async (off-thread encode) path and
	    everything else to the synchronous CaptureAndSendFrame. */
	bool StreamLatestFrame(bool bTrackInFlight);
	/** Game-thread capture + off-thread JPEG encode + game-thread send, with a
	    single capture in flight (latest-wins). */
	bool BeginAsyncCaptureAndSend(bool bTrackInFlight);
	/** Completes a pending GPU readback frame when the fence has signaled. */
	void TryCompletePendingFrameReadback();
	void FinishAsyncCaptureAndSend(FUbstreamEditorWorldCaptureResult&& Result, bool bTrackInFlight, FUbstreamCaptureTicket Ticket);
	void CancelPendingCapture();
	void HandleJsonMessage(const FString& Message);
	void HandleBinaryMessage(const TArray<uint8>& Message);
	void HandleFastStreamBinaryMessage(const TArray<uint8>& Message);
	void HandleLegacyBinaryMessage(const TArray<uint8>& Message);
	void StartStreaming();
	void StopStreaming(const FString& StatusMessage);
	EActiveTimerReturnType HandleModeStateTick(double InCurrentTime, float InDeltaTime);
	bool HandleStreamTicker(float DeltaTime);
	/** While streaming, keeps the editor main loop awake so the stream ticker and
	    incoming WebSocket frame responses keep being serviced. A non-realtime
	    viewport otherwise lets the editor sleep the instant the user stops moving,
	    stranding the final resting frame and freezing the preview on the last
	    in-motion frame until the next input event. */
	EActiveTimerReturnType HandleStreamKeepAliveTick(double InCurrentTime, float InDeltaTime);
	bool SendHelloMessage();
	bool SendAuthMessage();
	/** Returns the current account session token. */
	FString ResolveAuthToken() const;
	bool SendSelectionStateMessage();
	bool SendFastStreamAuthMessage();
	bool SendFastStreamParamsMessage();
	bool SendFastStreamFrameMessage(bool bTrackInFlight);
	bool SendFastStreamFlush(uint32 FrameId);
	void ScheduleFastStreamFlush(uint32 FrameId);
	void CancelFastStreamFlushTimer();
	EActiveTimerReturnType HandleFastStreamFlushTick(double InCurrentTime, float InDeltaTime);
	void QueueFastStreamAutoRun(const FString& StatusMessage);
	bool KickFastStreamFrameIfReady();
	/** Cadence-bound FastStream pump; sends dirty state without waiting for an ack. */
	bool MaybeStreamLatestFastStreamFrame();
	FString BuildHelloJson() const;
	FString BuildAuthJson() const;
	FString BuildGenerateRequestJson() const;
	FString BuildParamsUpdateJson() const;
	FString BuildFastStreamParamsJson() const;
	bool SendParamsUpdateMessage();
	void SchedulePushParams(float DelaySeconds);
	EActiveTimerReturnType HandleParamsPushTick(double InCurrentTime, float InDeltaTime);
	void HandlePromptChanged(const FText& NewText);
	bool HasPromptText() const;
	FString GetPromptText() const;
	float GetPromptStrength() const;
	float GetDepthStrength() const;
	void HandlePromptStrengthChanged(float NewValue);
	void HandleDepthStrengthChanged(float NewValue);
	FString GetSessionId() const;
	float GetStreamIntervalSeconds() const;
	int32 GetMaxInFlightFrames() const;
	int32 GetMaxFrameBundleBytes() const;
	FUbstreamCaptureSettings BuildCaptureSettings() const;
	bool ShouldDelayFrameUploadForCadence(bool bForceSend, float& OutWaitSeconds) const;
	void RememberFrameUploadCadence();
	void MarkCaptureDirty();
	void MarkFrameUploadDirty();
	void ResetFrameUploadSignature();
	uint64 BuildSelectedModelingSceneSignature() const;
	bool HasSelectedModelingSceneSignatureChanged();
	bool ShouldSkipCapturedFrameUpload(bool bForceSend) const;
	void RememberUploadedFrameSignature();
	void RegisterEditorChangeDelegates();
	void UnregisterEditorChangeDelegates();
	void HandleResultImportComplete(bool bSuccess, const FString& Message, const FString& AssetPath);
	void ClearPendingReturnedFrameState();
	bool SaveReturnedFrameBytes(const TArray<uint8>& Bytes, FString& OutPath, FString& OutErrorMessage) const;
	bool UpdateReturnedFramePreviewFromBytes(const TArray<uint8>& Bytes, FString& OutErrorMessage);
	void UpdateReturnedFramePreview(const FString& FilePath);
	void UpdateDebugSentFramePreview(uint64 FrameId);
	void UpdateDebugReceivedFramePreview(uint64 FrameId, const TSharedPtr<FSlateDynamicImageBrush>& Brush);
	void ClearDebugFramePreviews();
	void SetWorkflowState(EWorkflowState NewState, const FString& Title, const FString& Detail);
	void SetStatus(const FString& Message);
	void SetSelectionInfo(const FString& Message);
	void SetStreamInfo(const FString& Message);
	void SetResultInfo(const FString& Message);
	void NotifyUser(const FText& Message, bool bIsError = false) const;
	FString GetEndpointUrl() const;

	TSharedPtr<FUbstreamWebSocketClient> WebSocketClient;
	TSharedPtr<SEditableTextBox> UrlTextBox;
	TSharedPtr<SMultiLineEditableTextBox> PromptTextBox;
	TSharedPtr<SSlider> PromptStrengthSlider;
	TSharedPtr<SSlider> DepthStrengthSlider;
	TSharedPtr<STextBlock> PromptStrengthReadout;
	TSharedPtr<STextBlock> DepthStrengthReadout;
	float PromptStrength = 0.5f;
	float DepthStrength = 0.5f;
	int32 GenerationSeed = 2;
	TSharedPtr<SBox> ModeGateBodyBox;
	TSharedPtr<STextBlock> WorkflowStateTitleText;
	TSharedPtr<STextBlock> ModeGateStatusText;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<STextBlock> SelectionInfoText;
	TSharedPtr<STextBlock> StreamInfoText;
	TSharedPtr<STextBlock> ResultInfoText;
	TSharedPtr<STextBlock> DebugSentFrameText;
	TSharedPtr<STextBlock> DebugReceivedFrameText;
	TSharedPtr<SBox> ReturnedFrameBox;
	TSharedPtr<SImage> ReturnedFrameImage;
	TSharedPtr<SImage> DebugSentFrameImage;
	TSharedPtr<SExpandableArea> LogsSection;
	TSharedPtr<SImage> DebugReceivedFrameImage;
	TSharedPtr<FSlateDynamicImageBrush> ReturnedFrameBrush;
	TSharedPtr<FSlateDynamicImageBrush> DebugSentFrameBrush;
	TSharedPtr<FSlateDynamicImageBrush> DebugReceivedFrameBrush;
	TSharedPtr<SHorizontalBox> SnapshotStripBox;
	TSharedPtr<class SUbstreamMeshGenPanel> MeshGenPanel;
	TArray<FUbstreamFrameSnapshot> Snapshots;
	TArray<uint8> CurrentPreviewImageBytes;
	int32 ViewedSnapshotIndex = INDEX_NONE;
	TWeakPtr<SUbstreamModelerViewport> SoloViewport;
	FSimpleDelegate OnLoadSelectedRequested;
	FUbstreamCaptureTargetPool EditorCaptureTargetPool;
	FUbstreamEditorWorldCaptureResult LastCapturedFrame;
	TWeakPtr<FActiveTimerHandle> ModeStateTimerHandle;
	FTSTicker::FDelegateHandle StreamTickerHandle;
	float RegisteredStreamIntervalSeconds = 0.0f;
	bool bRegisteredLocalPreviewOnly = false;
	TWeakPtr<FActiveTimerHandle> ParamsPushTimerHandle;
	TWeakPtr<FActiveTimerHandle> ReconnectTimerHandle;
	TWeakPtr<FActiveTimerHandle> FastStreamFlushTimerHandle;
	TWeakPtr<FActiveTimerHandle> StreamKeepAliveTimerHandle;
	FDelegateHandle EditorCameraMovedHandle;
	FDelegateHandle ActorsMovedHandle;
	FDelegateHandle ActorMovingHandle;
	FDelegateHandle ActorMovedHandle;
	FDelegateHandle ComponentTransformChangedHandle;
	FDelegateHandle LevelActorAddedHandle;
	FDelegateHandle LevelActorDeletedHandle;
	FDelegateHandle LevelActorListChangedHandle;
	FDelegateHandle MapChangedHandle;
	FDelegateHandle SelectionChangedHandle;
	FDelegateHandle ObjectModifiedHandle;
	FDelegateHandle ObjectPropertyChangedHandle;
	TSet<uint64> InFlightFrames;
	TMap<uint64, double> FrameSendTimes;
	FString LastResultUrl;
	FString LastImportedAssetPath;
	FString LastReturnedFramePath;
	FString PendingReturnedFramePayloadType;
	FString PendingReturnedFrameFormat;
	uint64 PendingReturnedFrameId = 0;
	int32 PendingReturnedFrameBytes = 0;
	int32 FastStreamCaptureWidth = 512;
	int32 FastStreamCaptureHeight = 512;
	uint32 PendingFastStreamFlushFrameId = 0;
	uint64 NextFrameId = 1;
	uint64 LastAckedFrameId = 0;
	uint64 LastPreviewedFrameId = 0;
	uint64 LastDebugSentFrameId = 0;
	uint64 LastDebugReceivedFrameId = 0;
	uint64 MinimumPreviewableFastStreamFrameId = 0;
	uint64 SkippedUnchangedFrameCount = 0;
	double WorkspaceReadySinceSeconds = -1.0;
	double WorkspaceUnreadySinceSeconds = -1.0;
	double LastFrameUploadTimeSeconds = -1.0;
	// Leave more room for guidance controls; the preview remains drag-resizable.
	float PreviewHeight = 300.0f;
	float PreviewResizeStartHeight = 420.0f;
	float PreviewResizeStartScreenY = 0.0f;
	FUbstreamCaptureState CaptureState;
	FUbstreamCaptureTicket LastCapturedTicket;
	EWorkflowState WorkflowState = EWorkflowState::Inactive;
	bool bFastStreamReady = false;
	bool bFastStreamDepthSupported = false;
	bool bFastStreamParamsPending = false;
	bool bFastStreamStartWhenReady = false;
	bool bAutoConnectAttemptedForCurrentWorkspace = false;
	bool bManualDisconnectDuringCurrentWorkspace = false;
	bool bReconnectPending = false;
	bool bUserPausedStream = false;
	bool bStreaming = false;
	bool bHadAuthenticatedAccount = false;
	bool bLastWorkspaceReady = false;
	bool bHasPendingReturnedFrame = false;
	bool bHasReturnedFramePreview = false;
	bool bReturnedPreviewFromBytes = false;
	/** Current live preview came from the stream server (not local Unreal passthrough). */
	bool bCurrentPreviewIsServerFrame = false;
	bool bResizingPreview = false;
	bool bLastCapturedFrameWasSoloViewport = false;
	TSharedPtr<class FUbstreamAsyncFrameReadback, ESPMode::ThreadSafe> PendingFrameReadback;
	bool bPendingFrameTrackInFlight = false;
};
