#include "Widgets/SUbstreamControlPanel.h"
#include "Auth/UbstreamTransportSecurity.h"
#include "Auth/UbstreamAuthService.h"
#include "Utils/UbstreamImageResourceNames.h"

#include "Algo/Sort.h"
#include "Async/Async.h"
#include "Bridge/UbstreamModelingModeBridge.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Capture/UbstreamCaptureImageUtils.h"
#include "Capture/UbstreamCaptureService.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "InputCoreTypes.h"
#include "ISettingsModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Import/UbstreamResultImportService.h"
#include "Protocol/UbstreamProtocolTypes.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Settings/UbstreamModelerSettings.h"
#include "Selection.h"
#include "Styling/UbstreamModelerStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Transport/UbstreamWebSocketClient.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SUbstreamMeshGenPanel.h"
#include "UbstreamModelerEditorModule.h"
#include "Widgets/SUbstreamModelerViewport.h"
#include "Widgets/Text/STextBlock.h"
#include "Types/WidgetActiveTimerDelegate.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "SUbstreamControlPanel"

namespace
{
	enum class EFastStreamClientMessage : uint8
	{
		Frame = 0x01,
		Flush = 0x02,
		Params = 0x03,
		Auth = 0x04
	};

	enum class EFastStreamServerMessage : uint8
	{
		FrameResponse = 0x01,
		Status = 0x02
	};

	enum class EFastStreamServerStatus : uint8
	{
		Loading = 0x00,
		Ready = 0x01
	};

	constexpr double ModelingModeAutoConnectDelaySeconds = 0.5;
	constexpr double ModelingModeExitDisconnectDelaySeconds = 1.0;
	constexpr double ReconnectDelaySeconds = 5.0;
	constexpr double FastStreamFlushDebounceSeconds = 0.5;
	constexpr float ServerStreamTargetFps = 15.0f;
	constexpr int32 MaxTrackedFastStreamFrames = 120;
	constexpr float PreviewMinHeight = 240.0f;
	constexpr float PreviewMaxHeight = 900.0f;
	constexpr int32 MinimumPromptCharactersForGeneration = 2;
	constexpr int32 MaxSnapshotCount = 12;
	constexpr int32 MaxGenerationSeed = 999999999;
	constexpr float SnapshotThumbnailSize = 72.0f;

	const FSliderStyle& GetLargeSliderStyle()
	{
		return FUbstreamModelerStyle::Get().GetWidgetStyle<FSliderStyle>(TEXT("Ubstream.Slider"));
	}

	const FSlateBrush* GetStatusPillBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor::White, 8.0f, FVector2D(1.0f, 1.0f));
		return &Brush;
	}

	TSharedRef<SWidget> BuildIconWidget(FName IconName, const FSlateColor& Color)
	{
		if (IconName.IsNone())
		{
			return SNullWidget::NullWidget;
		}

		return SNew(SImage)
			.Image(FAppStyle::GetBrush(IconName))
			.ColorAndOpacity(Color);
	}

	TSharedRef<SWidget> BuildButton(
		const FText& Label,
		const FOnClicked& OnClicked,
		const FText& ToolTip = FText::GetEmpty(),
		FName IconName = NAME_None,
		bool bIsDanger = false)
	{
		const FSlateColor Foreground = bIsDanger
			? FSlateColor(FUbstreamModelerStyle::StatusError())
			: FSlateColor::UseForeground();

		return SNew(SBox)
			.HeightOverride(30.0f)
			[
				SNew(SButton)
				.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.Button")
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.ToolTipText(ToolTip)
				.ForegroundColor(Foreground)
				.OnClicked(OnClicked)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, IconName.IsNone() ? 0.0f : 6.0f, 0.0f)
					[
						BuildIconWidget(IconName, Foreground)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(Label)
						.ColorAndOpacity(Foreground)
					]
				]
			];
	}

	TSharedRef<SWidget> BuildPrimaryButton(const FText& Label, const FOnClicked& OnClicked, const FText& ToolTip = FText::GetEmpty())
	{
		return SNew(SBox)
			.HeightOverride(38.0f)
			[
				SNew(SButton)
				.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.PrimaryButton")
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.ToolTipText(ToolTip)
				.Text(Label)
				.OnClicked(OnClicked)
			];
	}

	TSharedRef<SWidget> BuildCompactIconButton(
		const TAttribute<FText>& ToolTip,
		const TAttribute<const FSlateBrush*>& Icon,
		const FOnClicked& OnClicked,
		const TAttribute<bool>& IsEnabled = true)
	{
		return SNew(SButton)
			.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
			.ContentPadding(FMargin(4.0f))
			.ToolTipText(ToolTip)
			.IsEnabled(IsEnabled)
			.OnClicked(OnClicked)
			[
				SNew(SBox)
				.WidthOverride(16.0f)
				.HeightOverride(16.0f)
				[
					SNew(SImage)
					.Image(Icon)
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			];
	}

	TSharedRef<SWidget> BuildSection(const FText& Label, const TSharedRef<SWidget>& Content, const FText& ToolTip = FText::GetEmpty())
	{
		return SNew(SBorder)
			.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Panel"))
			.Padding(1.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FUbstreamModelerStyle::CardBackground())
				.Padding(FUbstreamModelerStyle::CardPadding)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(Label)
						.ToolTipText(ToolTip)
						.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, FUbstreamModelerStyle::OuterPadding, 0.0f, 0.0f)
					[
						Content
					]
				]
			];
	}

	TSharedRef<SExpandableArea> BuildExpandableSection(const FText& Label, const TSharedRef<SWidget>& Content, bool bInitiallyCollapsed, FOnBooleanValueChanged OnExpansionChanged = FOnBooleanValueChanged())
	{
		return SNew(SExpandableArea)
			.InitiallyCollapsed(bInitiallyCollapsed)
			.OnAreaExpansionChanged(OnExpansionChanged)
			.AreaTitle(Label)
			.AreaTitleFont(FCoreStyle::GetDefaultFontStyle("Regular", 10))
			.HeaderPadding(FMargin(10.0f, 7.0f))
			.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Panel"))
			.BodyContent()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FUbstreamModelerStyle::CardBackground())
				.Padding(FUbstreamModelerStyle::CardPadding)
				[
					Content
				]
			];
	}

	void AppendUint32LE(TArray<uint8>& Bytes, uint32 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
	}

	bool ReadUint32LE(const TArray<uint8>& Bytes, int32 Offset, uint32& OutValue)
	{
		if (Offset < 0 || Bytes.Num() < Offset + 4)
		{
			return false;
		}

		OutValue =
			static_cast<uint32>(Bytes[Offset])
			| (static_cast<uint32>(Bytes[Offset + 1]) << 8)
			| (static_cast<uint32>(Bytes[Offset + 2]) << 16)
			| (static_cast<uint32>(Bytes[Offset + 3]) << 24);
		return true;
	}

	void AppendUtf8(TArray<uint8>& Bytes, const FString& Text)
	{
		FTCHARToUTF8 Converter(*Text);
		Bytes.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
	}

	FString DecodeUtf8(const TArray<uint8>& Bytes, int32 Offset)
	{
		if (Offset < 0 || Bytes.Num() <= Offset)
		{
			return FString();
		}

		const int32 Length = Bytes.Num() - Offset;
		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset), Length);
		return FString(Converter.Length(), Converter.Get());
	}

	uint64 MixSceneSignature(uint64 Hash, uint64 Value)
	{
		Hash ^= Value + 0x9e3779b97f4a7c15ull + (Hash << 6) + (Hash >> 2);
		return Hash;
	}

	uint64 QuantizeSceneFloat(double Value)
	{
		return static_cast<uint64>(static_cast<int64>(FMath::RoundToDouble(Value * 100.0)));
	}

	uint64 MixSceneString(uint64 Hash, const FString& Value)
	{
		return MixSceneSignature(Hash, static_cast<uint64>(GetTypeHash(Value)));
	}

	uint64 MixSceneObjectPath(uint64 Hash, const UObject* Object)
	{
		return MixSceneString(Hash, Object ? Object->GetPathName() : FString());
	}

	uint64 MixSceneVector(uint64 Hash, const FVector& Value)
	{
		Hash = MixSceneSignature(Hash, QuantizeSceneFloat(Value.X));
		Hash = MixSceneSignature(Hash, QuantizeSceneFloat(Value.Y));
		Hash = MixSceneSignature(Hash, QuantizeSceneFloat(Value.Z));
		return Hash;
	}

	uint64 MixSceneTransform(uint64 Hash, const FTransform& Value)
	{
		Hash = MixSceneVector(Hash, Value.GetLocation());
		Hash = MixSceneVector(Hash, Value.GetRotation().Rotator().Euler());
		Hash = MixSceneVector(Hash, Value.GetScale3D());
		return Hash;
	}

	FString DetectReturnedImageExtension(const TArray<uint8>& Bytes, const FString& Format)
	{
		const FString LowerFormat = Format.ToLower();
		if (LowerFormat == TEXT("jpeg") || LowerFormat == TEXT("jpg"))
		{
			return TEXT("jpg");
		}

		if (LowerFormat.Contains(TEXT("png")))
		{
			return TEXT("png");
		}

		if (Bytes.Num() >= 3 && Bytes[0] == 0xff && Bytes[1] == 0xd8 && Bytes[2] == 0xff)
		{
			return TEXT("jpg");
		}

		if (Bytes.Num() >= 8
			&& Bytes[0] == 0x89
			&& Bytes[1] == 0x50
			&& Bytes[2] == 0x4e
			&& Bytes[3] == 0x47
			&& Bytes[4] == 0x0d
			&& Bytes[5] == 0x0a
			&& Bytes[6] == 0x1a
			&& Bytes[7] == 0x0a)
		{
			return TEXT("png");
		}

		return TEXT("bin");
	}

	TSharedPtr<FSlateDynamicImageBrush> CreateBrushFromCompressedImage(
		const TArray<uint8>& Bytes,
		const TCHAR* DebugPrefix,
		FString& OutErrorMessage)
	{
		if (Bytes.IsEmpty())
		{
			OutErrorMessage = TEXT("Image payload is empty");
			return nullptr;
		}

		FImage DecodedImage;
		if (!FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), DecodedImage))
		{
			OutErrorMessage = TEXT("Payload is not a supported compressed image");
			return nullptr;
		}

		if (DecodedImage.SizeX <= 0 || DecodedImage.SizeY <= 0)
		{
			OutErrorMessage = TEXT("Image decoded to an invalid size");
			return nullptr;
		}

		DecodedImage.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);

		const FName ResourceName = UbstreamImageResourceNames::Next(DebugPrefix);
		const FVector2D ImageSize(static_cast<float>(DecodedImage.SizeX), static_cast<float>(DecodedImage.SizeY));
		TArray<uint8> RawImageBytes;
		if (!UbstreamCaptureImageUtils::TakeImageBytes(DecodedImage.RawData, RawImageBytes))
		{
			OutErrorMessage = TEXT("Decoded image is too large for a preview texture");
			return nullptr;
		}

		TSharedPtr<FSlateDynamicImageBrush> Brush = FSlateDynamicImageBrush::CreateWithImageData(ResourceName, ImageSize, RawImageBytes);
		if (!Brush.IsValid())
		{
			OutErrorMessage = TEXT("Failed to create preview texture");
		}

		return Brush;
	}
}

SUbstreamControlPanel::~SUbstreamControlPanel()
{
	CancelPendingCapture();
	UnregisterEditorChangeDelegates();

	bStreaming = false;
	InFlightFrames.Reset();

	if (StreamTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(StreamTickerHandle);
		StreamTickerHandle.Reset();
	}

	if (TSharedPtr<FActiveTimerHandle> TimerHandle = ModeStateTimerHandle.Pin())
	{
		UnRegisterActiveTimer(TimerHandle.ToSharedRef());
		ModeStateTimerHandle.Reset();
	}

	if (TSharedPtr<FActiveTimerHandle> TimerHandle = ParamsPushTimerHandle.Pin())
	{
		UnRegisterActiveTimer(TimerHandle.ToSharedRef());
		ParamsPushTimerHandle.Reset();
	}

	if (TSharedPtr<FActiveTimerHandle> TimerHandle = ReconnectTimerHandle.Pin())
	{
		UnRegisterActiveTimer(TimerHandle.ToSharedRef());
		ReconnectTimerHandle.Reset();
	}

	if (TSharedPtr<FActiveTimerHandle> TimerHandle = FastStreamFlushTimerHandle.Pin())
	{
		UnRegisterActiveTimer(TimerHandle.ToSharedRef());
		FastStreamFlushTimerHandle.Reset();
	}

	if (WebSocketClient.IsValid())
	{
		WebSocketClient->Disconnect();
	}
}

void SUbstreamControlPanel::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	TryCompletePendingFrameReadback();
}

void SUbstreamControlPanel::TryCompletePendingFrameReadback()
{
	if (!PendingFrameReadback.IsValid())
	{
		return;
	}

	if (!PendingFrameReadback->IsReady())
	{
		// Only expire unfinished GPU work. An editor hitch may delay this poll
		// beyond the deadline even though the readback has already completed.
		if (PendingFrameReadback->HasTimedOut())
		{
			CancelPendingCapture();
		}
		return;
	}

	const bool bTrackInFlight = bPendingFrameTrackInFlight;
	const FUbstreamCaptureTicket Ticket = CaptureState.GetActiveTicket();
	TWeakPtr<SUbstreamControlPanel> WeakThis = StaticCastSharedRef<SUbstreamControlPanel>(AsShared());
	PendingFrameReadback->CompleteAndEncodeAsync(
		[WeakThis, bTrackInFlight, Ticket](FUbstreamEditorWorldCaptureResult&& Result)
	{
		if (const TSharedPtr<SUbstreamControlPanel> Pinned = WeakThis.Pin())
		{
			Pinned->FinishAsyncCaptureAndSend(MoveTemp(Result), bTrackInFlight, Ticket);
		}
	});
	PendingFrameReadback.Reset();
}

void SUbstreamControlPanel::Construct(const FArguments& InArgs)
{
	// Frames are JPEG-encoded on a worker thread (see BeginAsyncCaptureAndSend);
	// FImageUtils::CompressImage lazy-loads the ImageWrapper module, and
	// FModuleManager::LoadModule is not safe off the game thread. Pre-load it now.
	FModuleManager::Get().LoadModule(TEXT("ImageWrapper"));

	SoloViewport = InArgs._SoloViewport;
	OnLoadSelectedRequested = InArgs._OnLoadSelectedRequested;
	PromptStrength = FMath::Clamp(GetDefault<UUbstreamModelerSettings>()->PromptStrength, 0.0f, 1.0f);
	DepthStrength = FMath::Clamp(GetDefault<UUbstreamModelerSettings>()->DepthStrength, 0.0f, 1.0f);
	GenerationSeed = FMath::Clamp(GetDefault<UUbstreamModelerSettings>()->GenerationSeed, 0, MaxGenerationSeed);

	WebSocketClient = MakeShared<FUbstreamWebSocketClient>();
	WebSocketClient->OnConnected.AddLambda([this]()
	{
		CancelReconnect();
		CancelFastStreamFlushTimer();
		bFastStreamReady = false;
		bFastStreamDepthSupported = false;
		bFastStreamParamsPending = true;
		bFastStreamStartWhenReady = false;
		FastStreamCaptureWidth = 512;
		FastStreamCaptureHeight = 512;
		FrameSendTimes.Reset();
		ResetFrameUploadSignature();

		if (!IsCaptureWorkspaceReady())
		{
			StopStreaming(TEXT("Stream stopped: no isolated subject is loaded"));
			SetWorkflowState(
				EWorkflowState::Inactive,
				TEXT("Connected, waiting for mesh"),
				TEXT("Load a mesh into the stage to activate streaming."));
			SetStatus(TEXT("Connected, but inactive. Load a mesh into the stage before sending frames."));
			NotifyUser(LOCTEXT("ConnectedInactiveToast", "Connected. Load a mesh to start streaming."));
			return;
		}

		const UUbstreamModelerSettings* Settings = GetDefault<UUbstreamModelerSettings>();
		if (IsFastStreamProtocol())
		{
			bFastStreamStartWhenReady = !bUserPausedStream && (Settings->bAutoStartStreamOnConnect || HasPromptText());
			const bool bAuthSent = SendFastStreamAuthMessage();
			SetWorkflowState(
				EWorkflowState::Loading,
				TEXT("Connected. Loading model"),
				bFastStreamStartWhenReady
					? TEXT("The server is preparing the stream. Live capture will start automatically.")
					: TEXT("The server is preparing the stream."));
			SetStatus(bAuthSent
				? TEXT("Connected to FastStream; waiting for model loading to finish")
				: TEXT("Connected to FastStream; auth token is empty, waiting for model loading to finish"));
			SetStreamInfo(bFastStreamStartWhenReady
				? TEXT("Stream queued; it will start automatically when FastStream is Ready")
				: TEXT("Stream waiting for FastStream Ready"));
			EnsureLocalPassthroughPreviewRunning();
			NotifyUser(LOCTEXT("ConnectedFastStreamToast", "Connected to the stream server."));
			return;
		}

		const bool bHelloSent = SendHelloMessage();
		const bool bAuthSent = SendAuthMessage();
		const bool bSelectionSent = !Settings->bSendSelectionStateOnConnect || SendSelectionStateMessage();
		SendParamsUpdateMessage();
		SetStatus(FString::Printf(
			TEXT("Connected; hello=%s auth=%s selection=%s"),
			bHelloSent ? TEXT("sent") : TEXT("failed"),
			bAuthSent ? TEXT("sent/no-token") : TEXT("failed"),
			bSelectionSent ? TEXT("sent/skipped") : TEXT("failed")));
		SetWorkflowState(
			EWorkflowState::Ready,
			TEXT("Connected"),
			Settings->bAutoStartStreamOnConnect
				? TEXT("Starting the live stream.")
				: TEXT("Ready to send selection state or frames."));
		NotifyUser(LOCTEXT("ConnectedToast", "Connected to the stream server."));

		if (Settings->bAutoStartStreamOnConnect && !bUserPausedStream)
		{
			StartStreaming();
		}
	});
	WebSocketClient->OnConnectionError.AddLambda([this](const FString& Error)
	{
		StopStreaming(TEXT("Stream stopped: connection ended"));
		CancelFastStreamFlushTimer();
		bFastStreamReady = false;
		bFastStreamDepthSupported = false;
		bFastStreamParamsPending = false;
		bFastStreamStartWhenReady = false;
		FastStreamCaptureWidth = 512;
		FastStreamCaptureHeight = 512;
		ResetFrameUploadSignature();
		SetWorkflowState(
			EWorkflowState::Error,
			TEXT("Connection failed"),
			Error.IsEmpty() ? TEXT("The WebSocket connection could not be opened.") : Error);
		SetStatus(FString::Printf(TEXT("Connection error: %s"), *Error));
		SetStreamInfo(TEXT("Stream stopped"));
		NotifyUser(
			Error.IsEmpty()
				? LOCTEXT("ConnectionFailedToast", "Connection failed.")
				: FText::Format(LOCTEXT("ConnectionFailedToastFormat", "Connection failed: {0}"), FText::FromString(Error)),
			true);
		if (!IsNonRetryableDisconnectReason(Error))
		{
			ScheduleReconnect(Error);
		}
		EnsureLocalPassthroughPreviewRunning();
	});
	WebSocketClient->OnClosed.AddLambda([this](int32 StatusCode, const FString& Reason, bool bWasClean)
	{
		StopStreaming(TEXT("Stream stopped: connection ended"));
		CancelFastStreamFlushTimer();
		bFastStreamReady = false;
		bFastStreamDepthSupported = false;
		bFastStreamParamsPending = false;
		bFastStreamStartWhenReady = false;
		FastStreamCaptureWidth = 512;
		FastStreamCaptureHeight = 512;
		ResetFrameUploadSignature();
		SetWorkflowState(
			IsCaptureWorkspaceReady() ? EWorkflowState::Disconnected : EWorkflowState::Inactive,
			IsCaptureWorkspaceReady() ? TEXT("Disconnected") : TEXT("Load a mesh"),
			Reason.IsEmpty() ? TEXT("The WebSocket connection closed.") : Reason);
		SetStatus(FString::Printf(TEXT("Closed (%d, clean=%s): %s"), StatusCode, bWasClean ? TEXT("true") : TEXT("false"), *Reason));
		SetStreamInfo(TEXT("Stream stopped"));
		if (ShouldAutoReconnectAfterClose(StatusCode, Reason, bWasClean))
		{
			ScheduleReconnect(Reason.IsEmpty() ? TEXT("The WebSocket connection closed.") : Reason);
		}
		else if (!Reason.IsEmpty())
		{
			NotifyUser(FText::Format(LOCTEXT("ConnectionClosedReasonToast", "Connection closed: {0}"), FText::FromString(Reason)), true);
		}
		EnsureLocalPassthroughPreviewRunning();
	});
	WebSocketClient->OnJsonMessage.AddLambda([this](const FString& Message)
	{
		HandleJsonMessage(Message);
	});
	WebSocketClient->OnBinaryMessage.AddLambda([this](const TArray<uint8>& Message)
	{
		HandleBinaryMessage(Message);
	});

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FUbstreamModelerStyle::PanelBackground())
		.Padding(10.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				BuildPanelContent()
			]
		]
	];

	bLastWorkspaceReady = IsCaptureWorkspaceReady();
	if (bLastWorkspaceReady)
	{
		WorkspaceReadySinceSeconds = FPlatformTime::Seconds();
		WorkspaceUnreadySinceSeconds = -1.0;
	}
	else
	{
		WorkspaceReadySinceSeconds = -1.0;
		WorkspaceUnreadySinceSeconds = FPlatformTime::Seconds();
	}
	SetWorkflowState(
		IsCaptureWorkspaceReady() ? EWorkflowState::Disconnected : EWorkflowState::Inactive,
		IsCaptureWorkspaceReady() ? TEXT("Ready to connect") : TEXT("Load a mesh"),
			IsCaptureWorkspaceReady()
				? TEXT("Connect to the stream server to start sending frames.")
				: TEXT("Load a mesh into the stage to begin."));
	ModeStateTimerHandle = RegisterActiveTimer(
		0.2f,
		FWidgetActiveTimerDelegate::CreateSP(this, &SUbstreamControlPanel::HandleModeStateTick));
	RegisterEditorChangeDelegates();
	bHadAuthenticatedAccount = FUbstreamAuthService::Get().IsAuthenticated();
	FUbstreamAuthService::Get().OnChanged.AddSP(this, &SUbstreamControlPanel::HandleAuthenticationChanged);
}

FReply SUbstreamControlPanel::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!bResizingPreview)
	{
		return SCompoundWidget::OnMouseMove(MyGeometry, MouseEvent);
	}

	const float DeltaY = MouseEvent.GetScreenSpacePosition().Y - PreviewResizeStartScreenY;
	PreviewHeight = FMath::Clamp(PreviewResizeStartHeight + DeltaY, PreviewMinHeight, PreviewMaxHeight);
	if (ReturnedFrameBox.IsValid())
	{
		ReturnedFrameBox->SetHeightOverride(PreviewHeight);
	}

	return FReply::Handled();
}

FReply SUbstreamControlPanel::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!bResizingPreview || MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return SCompoundWidget::OnMouseButtonUp(MyGeometry, MouseEvent);
	}

	bResizingPreview = false;
	return FReply::Handled().ReleaseMouseCapture();
}

void SUbstreamControlPanel::OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
	bResizingPreview = false;
	SCompoundWidget::OnMouseCaptureLost(CaptureLostEvent);
}

void SUbstreamControlPanel::NotifyStageContentDirty()
{
	// Per-drag-delta path: the stage's pixels changed but the WORKSPACE did
	// not. Only the stream needs to know. The full workspace update below
	// costs 5-19ms (passthrough ensure + autoconnect + panel-wide layout
	// invalidation) and must never run per mouse move.
	MarkFrameUploadDirty();
}

void SUbstreamControlPanel::NotifyCaptureWorkspaceChanged()
{
	const double N0 = FPlatformTime::Seconds();
	MarkFrameUploadDirty();
	const double N1 = FPlatformTime::Seconds();

	if (HasSoloMeshLoaded())
	{
		const double N2 = FPlatformTime::Seconds();
		if ((N2 - N1) * 1000.0 > 2.0)
		{
			UE_LOG(LogUbstreamModeler, Log, TEXT("NotifyCaptureWorkspaceChanged: HasSoloMeshLoaded %.2fms (dirty %.2fms)"),
				(N2 - N1) * 1000.0, (N1 - N0) * 1000.0);
		}
		const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
		SetSelectionInfo(Viewport.IsValid()
			? FString::Printf(TEXT("Isolated subject: %s\nSource: %s"), *Viewport->GetSoloMeshLabel(), *Viewport->GetSoloMeshPath())
			: TEXT("Isolated subject loaded"));

		// Auto-resync re-enters here on every source edit; while streaming, keep
		// the Streaming workflow state instead of downgrading it per resync.
		if (!bStreaming)
		{
			SetWorkflowState(
				WebSocketClient.IsValid() && WebSocketClient->IsConnected() ? EWorkflowState::Ready : EWorkflowState::Disconnected,
				WebSocketClient.IsValid() && WebSocketClient->IsConnected() ? TEXT("Isolated subject connected") : TEXT("Isolated subject ready"),
				TEXT("RGB and depth capture will use only the isolated subject."));
			SetStatus(Viewport.IsValid()
				? FString::Printf(TEXT("Isolated capture source ready: %s"), *Viewport->GetSoloMeshLabel())
				: TEXT("Isolated capture source ready"));
		}

		WorkspaceReadySinceSeconds = FPlatformTime::Seconds();
		WorkspaceUnreadySinceSeconds = -1.0;
		bAutoConnectAttemptedForCurrentWorkspace = false;
		EnsureLocalPassthroughPreviewRunning();
		TryAutoConnectForWorkspace();
	}
	else
	{
		if (bStreaming)
		{
			StopStreaming(TEXT("Stream stopped: no isolated subject is loaded"));
		}

		SetWorkflowState(
			EWorkflowState::Inactive,
			TEXT("Load a mesh"),
			TEXT("Load a mesh into the stage to begin."));
		SetSelectionInfo(TEXT("No isolated subject loaded"));
		SetStatus(TEXT("No isolated subject loaded"));
		WorkspaceReadySinceSeconds = -1.0;
		WorkspaceUnreadySinceSeconds = FPlatformTime::Seconds();
	}

	Invalidate(EInvalidateWidgetReason::LayoutAndVolatility);
}

void SUbstreamControlPanel::HandleAuthenticationChanged()
{
	const bool bAuthenticated = FUbstreamAuthService::Get().IsAuthenticated();
	if (bHadAuthenticatedAccount == bAuthenticated) return;
	bHadAuthenticatedAccount = bAuthenticated;
	if (!FUbstreamAuthService::Get().IsAuthenticated())
	{
		bManualDisconnectDuringCurrentWorkspace = true;
		CancelReconnect();
		CancelFastStreamFlushTimer();
		StopStreaming(TEXT("Signed out"));
		WebSocketClient->Disconnect();
		bFastStreamReady = false;
		bFastStreamStartWhenReady = false;
		ClearCaptureWorkspaceState();
	}
	else
	{
		bManualDisconnectDuringCurrentWorkspace = false;
		bAutoConnectAttemptedForCurrentWorkspace = false;
		TryAutoConnectForWorkspace();
	}
}

void SUbstreamControlPanel::ClearCaptureWorkspaceState()
{
	if (bStreaming)
	{
		StopStreaming(TEXT("Stream stopped: capture workspace cleared"));
	}

	InFlightFrames.Reset();
	FrameSendTimes.Reset();
	CancelPendingCapture();
	LastCapturedFrame = FUbstreamEditorWorldCaptureResult();
	LastReturnedFramePath.Reset();
	LastResultUrl.Reset();
	LastAckedFrameId = 0;
	LastPreviewedFrameId = 0;
	MinimumPreviewableFastStreamFrameId = 0;
	CurrentPreviewImageBytes.Reset();
	ReturnedFrameBrush.Reset();
	bHasReturnedFramePreview = false;
	bCurrentPreviewIsServerFrame = false;
	ClearPendingReturnedFrameState();
	ClearDebugFramePreviews();

	Snapshots.Empty();
	ViewedSnapshotIndex = INDEX_NONE;
	RebuildSnapshotStrip();
	RefreshPreviewImage();
	ResetFrameUploadSignature();

	SetSelectionInfo(TEXT("No isolated subject loaded"));
	SetStreamInfo(TEXT("Stream stopped: no capture workspace is active"));
	SetResultInfo(TEXT("No returned frame"));
	SetStatus(TEXT("Cleared isolated workspace"));
	Invalidate(EInvalidateWidgetReason::LayoutAndVolatility);
}

TSharedRef<SWidget> SUbstreamControlPanel::BuildPanelContent()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.0f, 0.0f, 2.0f, 12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SImage)
					.Image(FUbstreamModelerStyle::Get().GetBrush("UbstreamModeler.BrandLogo"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Title", "Unbound Loop"))
					.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
					.Font(FUbstreamModelerStyle::BrandFont(15))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Subtitle", "Isolated renderer and Modeling Mode companion"))
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.Visibility(this, &SUbstreamControlPanel::GetModeGateVisibility)
			[
				BuildModeGate()
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.Visibility(this, &SUbstreamControlPanel::GetModelingControlsVisibility)
			[
				BuildActiveModePanel()
			]
		];
}

TSharedRef<SWidget> SUbstreamControlPanel::BuildModeGate()
{
	return SNew(SBorder)
		.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Panel"))
		.Padding(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FUbstreamModelerStyle::CardBackground())
			.Padding(18.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SBox)
					.HeightOverride(220.0f)
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						.HAlign(HAlign_Fill)
						.VAlign(VAlign_Fill)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
							.BorderBackgroundColor(FUbstreamModelerStyle::PreviewBackground())
							.Padding(0.0f)
						]
						+ SOverlay::Slot()
						.HAlign(HAlign_Fill)
						.VAlign(VAlign_Center)
						.Padding(24.0f)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot()
							.AutoHeight()
							.HAlign(HAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("ModeGateTitle", "Load a Mesh to Begin"))
								.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 18))
								.Justification(ETextJustify::Center)
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.HAlign(HAlign_Fill)
							.Padding(0.0f, 8.0f, 0.0f, 0.0f)
							[
								SAssignNew(ModeGateBodyBox, SBox)
								.MaxDesiredWidth(560.0f)
								.Clipping(EWidgetClipping::ClipToBounds)
								[
									SNew(STextBlock)
									.Text(this, &SUbstreamControlPanel::GetModeGateBodyText)
									.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
									.WrapTextAt(this, &SUbstreamControlPanel::GetModeGateBodyWrapWidth)
									.Justification(ETextJustify::Center)
									.Clipping(EWidgetClipping::ClipToBounds)
								]
							]
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 14.0f, 0.0f, 0.0f)
				[
					BuildPrimaryButton(
						LOCTEXT("ModeGateButton", "Use Selected Mesh"),
						FOnClicked::CreateSP(this, &SUbstreamControlPanel::HandleGateLoadSelectedClicked),
						LOCTEXT("ModeGateButtonTooltip", "Load the selected Static Mesh asset, component, or actor into the isolated stage."))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SAssignNew(ModeGateStatusText, STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
					.Text(LOCTEXT("ModeGateStatus", "Inactive"))
				]
			]
		];
}

TSharedRef<SWidget> SUbstreamControlPanel::BuildActiveModePanel()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildReturnedFrameSection()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			BuildMeshGenSection()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			BuildPromptSection()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			BuildConnectionSection()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			BuildLogsSection()
		];
}

TSharedRef<SWidget> SUbstreamControlPanel::BuildMeshGenSection()
{
	return SAssignNew(MeshGenPanel, SUbstreamMeshGenPanel)
		.SoloViewport(SoloViewport)
		.OnGetSourceImageBytes(SUbstreamMeshGenPanel::FGetImageBytes::CreateSP(
			this, &SUbstreamControlPanel::GetMeshGenSourceImageBytes))
		.OnHasSourceImage(SUbstreamMeshGenPanel::FHasSourceImage::CreateSP(
			this, &SUbstreamControlPanel::HasMeshGenSourceImage))
		.OnGetSourceImageLabel(SUbstreamMeshGenPanel::FGetImageLabel::CreateSP(
			this, &SUbstreamControlPanel::GetMeshGenSourceImageLabel))
		.OnGetBlockedReason(SUbstreamMeshGenPanel::FGetBlockedReason::CreateSP(
			this, &SUbstreamControlPanel::GetMeshGenBlockedReason));
}

TArray<uint8> SUbstreamControlPanel::GetMeshGenSourceImageBytes() const
{
	if (IsViewingSnapshot() && Snapshots.IsValidIndex(ViewedSnapshotIndex))
	{
		return Snapshots[ViewedSnapshotIndex].ImageBytes;
	}
	return CurrentPreviewImageBytes;
}

bool SUbstreamControlPanel::HasMeshGenSourceImage() const
{
	// Match stream UX: no 3D gen during local passthrough, and only after a real
	// prompt steers the server into generating frames.
	if (!HasPromptText())
	{
		return false;
	}
	if (IsLocalPassthroughPreviewMode())
	{
		return false;
	}

	if (IsViewingSnapshot() && Snapshots.IsValidIndex(ViewedSnapshotIndex))
	{
		const FUbstreamFrameSnapshot& Snapshot = Snapshots[ViewedSnapshotIndex];
		return Snapshot.bFromServer && !Snapshot.ImageBytes.IsEmpty();
	}

	return bCurrentPreviewIsServerFrame && !CurrentPreviewImageBytes.IsEmpty();
}

FString SUbstreamControlPanel::GetMeshGenSourceImageLabel() const
{
	if (IsViewingSnapshot() && Snapshots.IsValidIndex(ViewedSnapshotIndex))
	{
		return FString::Printf(TEXT("Source: snapshot %d of %d"), ViewedSnapshotIndex + 1, Snapshots.Num());
	}
	if (bCurrentPreviewIsServerFrame && !CurrentPreviewImageBytes.IsEmpty())
	{
		return TEXT("Source: server preview frame");
	}
	if (!CurrentPreviewImageBytes.IsEmpty())
	{
		return TEXT("Source: local passthrough (3D gen disabled)");
	}
	return TEXT("Source: waiting for a server frame");
}

FString SUbstreamControlPanel::GetMeshGenBlockedReason() const
{
	if (HasMeshGenSourceImage())
	{
		return FString();
	}
	if (!HasPromptText())
	{
		return TEXT("Enter a prompt to leave passthrough and enable 3D generation.");
	}
	if (IsLocalPassthroughPreviewMode())
	{
		return TEXT("3D generation is disabled during local passthrough — connect and wait for the stream server.");
	}
	if (IsViewingSnapshot() && Snapshots.IsValidIndex(ViewedSnapshotIndex)
		&& !Snapshots[ViewedSnapshotIndex].bFromServer)
	{
		return TEXT("This snapshot is a local passthrough frame. Pick a server frame or wait for live generation.");
	}
	if (CurrentPreviewImageBytes.IsEmpty() || !bCurrentPreviewIsServerFrame)
	{
		return TEXT("Waiting for a generated frame from the stream server.");
	}
	return TEXT("3D generation is not ready yet.");
}

FReply SUbstreamControlPanel::GenerateMeshFromPreviewClicked()
{
	if (MeshGenPanel.IsValid() && IsGenerateMeshFromPreviewEnabled())
	{
		MeshGenPanel->RequestGenerate();
	}
	return FReply::Handled();
}

bool SUbstreamControlPanel::IsGenerateMeshFromPreviewEnabled() const
{
	return MeshGenPanel.IsValid() && MeshGenPanel->CanRequestGenerate();
}

void SUbstreamControlPanel::RefreshMeshGenSourcePreview()
{
	if (MeshGenPanel.IsValid())
	{
		MeshGenPanel->RefreshSourcePreview();
	}
}

TSharedRef<SWidget> SUbstreamControlPanel::BuildReturnedFrameSection()
{
	return SNew(SBorder)
		.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Panel"))
		.Padding(FUbstreamModelerStyle::SmallGap)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f, 0.0f, 2.0f, FUbstreamModelerStyle::SmallGap)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ReturnedFramePreviewLabel", "Live Preview"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 2.0f, 0.0f)
				[
					BuildCompactIconButton(
						TAttribute<FText>::CreateSP(this, &SUbstreamControlPanel::GetPauseButtonToolTip),
						TAttribute<const FSlateBrush*>::CreateSP(this, &SUbstreamControlPanel::GetPauseButtonIcon),
						FOnClicked::CreateSP(this, &SUbstreamControlPanel::TogglePauseClicked))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 2.0f, 0.0f)
				[
					BuildCompactIconButton(
						LOCTEXT("SnapshotToolTip", "Keep the current frame in the snapshot strip."),
						FAppStyle::GetBrush("Icons.Star"),
						FOnClicked::CreateSP(this, &SUbstreamControlPanel::SnapshotClicked),
						TAttribute<bool>::CreateSP(this, &SUbstreamControlPanel::IsSnapshotButtonEnabled))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 2.0f, 0.0f)
				[
					BuildCompactIconButton(
						LOCTEXT("ImportFrameToolTip", "Import the displayed frame into the project as a texture asset."),
						FAppStyle::GetBrush("Icons.Import"),
						FOnClicked::CreateSP(this, &SUbstreamControlPanel::ImportDisplayedFrameClicked),
						TAttribute<bool>::CreateSP(this, &SUbstreamControlPanel::IsImportButtonEnabled))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					BuildCompactIconButton(
						LOCTEXT("Generate3DFromPreviewToolTip", "Generate a 3D mesh from the displayed frame and import it into the project."),
						FAppStyle::GetBrush("ClassIcon.StaticMesh"),
						FOnClicked::CreateSP(this, &SUbstreamControlPanel::GenerateMeshFromPreviewClicked),
						TAttribute<bool>::CreateSP(this, &SUbstreamControlPanel::IsGenerateMeshFromPreviewEnabled))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f, 0.0f, 2.0f, FUbstreamModelerStyle::SmallGap)
			[
				SNew(SBorder)
				.BorderImage(GetStatusPillBrush())
				.BorderBackgroundColor(FLinearColor(0.025f, 0.025f, 0.025f, 0.72f))
				.Padding(FMargin(8.0f, 3.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SBox)
						.WidthOverride(7.0f)
						.HeightOverride(7.0f)
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("Sequencer.KeyCircle"))
							.ColorAndOpacity(this, &SUbstreamControlPanel::GetWorkflowStateAccentColor)
						]
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					.Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						SAssignNew(WorkflowStateTitleText, STextBlock)
						.AutoWrapText(true)
						.Text(LOCTEXT("WorkflowStateInitialTitle", "Connecting"))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.90f, 0.90f, 0.90f)))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FUbstreamModelerStyle::PreviewBackground())
				.Padding(0.0f)
				[
					SAssignNew(ReturnedFrameBox, SBox)
					.HeightOverride(PreviewHeight)
					.MinDesiredWidth(300.0f)
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						.HAlign(HAlign_Fill)
						.VAlign(VAlign_Fill)
						[
							SNew(SScaleBox)
							.Stretch(EStretch::ScaleToFit)
							[
								SAssignNew(ReturnedFrameImage, SImage)
							]
						]
						+ SOverlay::Slot()
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.Padding(32.0f)
						[
							SNew(SVerticalBox)
							.Visibility(this, &SUbstreamControlPanel::GetReturnedFramePlaceholderVisibility)
							+ SVerticalBox::Slot()
							.AutoHeight()
							.HAlign(HAlign_Center)
							[
								SNew(SCircularThrobber)
								.Visibility(this, &SUbstreamControlPanel::GetLoadingIndicatorVisibility)
								.Radius(9.0f)
								.NumPieces(8)
								.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.0f, 12.0f, 0.0f, 0.0f)
							[
								SNew(STextBlock)
								.AutoWrapText(true)
								.Justification(ETextJustify::Center)
								.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
								.Text(LOCTEXT("ReturnedFramePlaceholder", "Waiting for the first returned frame"))
							]
						]
						+ SOverlay::Slot()
						.HAlign(HAlign_Left)
						.VAlign(VAlign_Bottom)
						.Padding(12.0f)
						[
							SNew(SBorder)
							.Visibility(this, &SUbstreamControlPanel::GetPassthroughHintVisibility)
							.BorderImage(GetStatusPillBrush())
							.BorderBackgroundColor(FLinearColor(0.025f, 0.025f, 0.025f, 0.76f))
							.Padding(FMargin(10.0f, 6.0f))
							[
								SNew(STextBlock)
								.Text(LOCTEXT("PassthroughPreviewHint", "Passthrough preview. Describe a prompt to start generation."))
								.AutoWrapText(true)
								.ColorAndOpacity(FSlateColor(FLinearColor(0.88f, 0.90f, 0.92f)))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
							]
						]
						+ SOverlay::Slot()
						.HAlign(HAlign_Left)
						.VAlign(VAlign_Top)
						.Padding(12.0f)
						[
							SNew(SBorder)
							.Visibility(this, &SUbstreamControlPanel::GetSnapshotViewPillVisibility)
							.BorderImage(GetStatusPillBrush())
							.BorderBackgroundColor(FLinearColor(0.025f, 0.025f, 0.025f, 0.76f))
							.Padding(FMargin(10.0f, 4.0f))
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.Text(this, &SUbstreamControlPanel::GetViewedSnapshotLabel)
									.ColorAndOpacity(FSlateColor(FLinearColor(0.88f, 0.90f, 0.92f)))
									.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
								]
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								.Padding(8.0f, 0.0f, 0.0f, 0.0f)
								[
									SNew(SButton)
									.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
									.ContentPadding(FMargin(4.0f, 2.0f))
									.ToolTipText(LOCTEXT("BackToLiveToolTip", "Return to the live preview."))
									.OnClicked(this, &SUbstreamControlPanel::BackToLiveClicked)
									[
										SNew(SHorizontalBox)
										+ SHorizontalBox::Slot()
										.AutoWidth()
										.VAlign(VAlign_Center)
										.Padding(0.0f, 0.0f, 4.0f, 0.0f)
										[
											SNew(SBox)
											.WidthOverride(12.0f)
											.HeightOverride(12.0f)
											[
												SNew(SImage)
												.Image(FAppStyle::GetBrush("Icons.Toolbar.Play"))
												.ColorAndOpacity(FSlateColor::UseForeground())
											]
										]
										+ SHorizontalBox::Slot()
										.AutoWidth()
										.VAlign(VAlign_Center)
										[
											SNew(STextBlock)
											.Text(LOCTEXT("BackToLiveLabel", "Live"))
											.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
										]
									]
								]
							]
						]
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FUbstreamModelerStyle::CardBackground())
				.Cursor(EMouseCursor::ResizeUpDown)
				.ToolTipText(LOCTEXT("PreviewResizeTooltip", "Drag to resize the live preview."))
				.OnMouseButtonDown(this, &SUbstreamControlPanel::HandlePreviewResizeMouseDown)
				.Padding(FMargin(0.0f, 5.0f))
				[
					SNew(SBox)
					.HeightOverride(10.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						.Padding(60.0f, 0.0f)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
							.BorderBackgroundColor(FUbstreamModelerStyle::BorderColor())
							.Padding(FMargin(0.0f, 1.0f))
						]
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBox)
				.Visibility(this, &SUbstreamControlPanel::GetSnapshotSectionVisibility)
				[
					BuildSnapshotSection()
				]
			]
		];
}

TSharedRef<SWidget> SUbstreamControlPanel::BuildSnapshotSection()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.0f, FUbstreamModelerStyle::SmallGap, 2.0f, FUbstreamModelerStyle::SmallGap)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SUbstreamControlPanel::GetSnapshotCountText)
				.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				BuildCompactIconButton(
					LOCTEXT("ClearSnapshotsToolTip", "Clear all snapshots."),
					FAppStyle::GetBrush("Icons.Delete"),
					FOnClicked::CreateSP(this, &SUbstreamControlPanel::ClearSnapshotsClicked))
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.0f, 0.0f, 2.0f, 2.0f)
		[
			SNew(SScrollBox)
			.Orientation(Orient_Horizontal)
			.ScrollBarThickness(FVector2D(5.0f, 5.0f))
			+ SScrollBox::Slot()
			[
				SAssignNew(SnapshotStripBox, SHorizontalBox)
			]
		];
}

FReply SUbstreamControlPanel::HandlePreviewResizeMouseDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	bResizingPreview = true;
	PreviewResizeStartHeight = PreviewHeight;
	PreviewResizeStartScreenY = MouseEvent.GetScreenSpacePosition().Y;
	return FReply::Handled().CaptureMouse(AsShared());
}

TSharedRef<SWidget> SUbstreamControlPanel::BuildPromptSection()
{
	return BuildSection(
		LOCTEXT("PromptSection", "Prompt and Guidance"),
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.MinDesiredHeight(76.0f)
			[
				SAssignNew(PromptTextBox, SMultiLineEditableTextBox)
				.Style(&FUbstreamModelerStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("Ubstream.TextBox"))
				.AutoWrapText(true)
				.HintText(LOCTEXT("PromptHint", "Describe the look you want..."))
				.OnTextChanged(this, &SUbstreamControlPanel::HandlePromptChanged)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 14.0f, 0.0f, 0.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PromptStrengthLabel", "Prompt strength"))
					.ToolTipText(LOCTEXT("PromptStrengthTooltip", "How strongly the prompt steers the generated image."))
					.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SBorder)
					.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.ValueReadout"))
					.Padding(FMargin(6.0f, 2.0f))
					[
						SAssignNew(PromptStrengthReadout, STextBlock)
						.MinDesiredWidth(32.0f)
						.Justification(ETextJustify::Right)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
						.Text(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(PromptStrength * 100.0f))))
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.HeightOverride(32.0f)
				[
					SAssignNew(PromptStrengthSlider, SSlider)
					.Style(&GetLargeSliderStyle())
					.MinValue(0.0f)
					.MaxValue(1.0f)
					.Value(PromptStrength)
					.OnValueChanged(this, &SUbstreamControlPanel::HandlePromptStrengthChanged)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PromptStrengthLow", "Subtle"))
					.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PromptStrengthHigh", "Strong"))
					.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 14.0f, 0.0f, 0.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ShapeStrengthLabel", "Shape Strength"))
					.ToolTipText(LOCTEXT("ShapeStrengthTooltip", "How strongly the generated image keeps the modeled shape."))
					.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SBorder)
					.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.ValueReadout"))
					.Padding(FMargin(6.0f, 2.0f))
					[
						SAssignNew(DepthStrengthReadout, STextBlock)
						.MinDesiredWidth(32.0f)
						.Justification(ETextJustify::Right)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
						.Text(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(DepthStrength * 100.0f))))
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.HeightOverride(32.0f)
				[
					SAssignNew(DepthStrengthSlider, SSlider)
					.Style(&GetLargeSliderStyle())
					.MinValue(0.0f)
					.MaxValue(1.0f)
					.Value(DepthStrength)
					.OnValueChanged(this, &SUbstreamControlPanel::HandleDepthStrengthChanged)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DepthStrengthLow", "Loose"))
					.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DepthStrengthHigh", "Faithful"))
					.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 14.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("VariationSeedLabel", "Variation seed"))
				.ToolTipText(LOCTEXT("VariationSeedTooltip", "Seed used by the generation backend. Keep it fixed for a stable look while modeling; reroll to explore a new variation."))
				.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SUbstreamControlPanel::GetSeedReadoutText)
				.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				BuildButton(
					LOCTEXT("RerollSeed", "Reroll"),
					FOnClicked::CreateSP(this, &SUbstreamControlPanel::RerollSeedClicked),
					LOCTEXT("RerollSeedTooltip", "Pick a new random seed and refresh the generated image."),
					TEXT("Icons.Refresh"))
			]
		],
		LOCTEXT("PromptTooltip", "Prompt updates stream parameters automatically."));
}

TSharedRef<SWidget> SUbstreamControlPanel::BuildConnectionSection()
{
	return BuildExpandableSection(
		LOCTEXT("ConnectionSection", "Connection and Debug"),
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("EndpointLabel", "WebSocket endpoint"))
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 10.0f)
		[
			SAssignNew(UrlTextBox, SEditableTextBox)
			.Text(FText::FromString(GetDefault<UUbstreamModelerSettings>()->DefaultWebSocketUrl))
			.ToolTipText(LOCTEXT("EndpointTooltip", "Use wss://. Plain ws:// is allowed only for localhost development. Saved per project/user settings."))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
							.Text(LOCTEXT("ConnectionAutoNote", "Auto-connects when an isolated subject or Modeling Mode workspace is active. Unexpected disconnects retry after 5 seconds."))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SUniformGridPanel)
			.SlotPadding(FMargin(0.0f, 0.0f, 8.0f, 8.0f))
			+ SUniformGridPanel::Slot(0, 0)
			[
				BuildButton(
					LOCTEXT("Connect", "Reconnect"),
					FOnClicked::CreateSP(this, &SUbstreamControlPanel::ConnectClicked),
					LOCTEXT("ReconnectTooltip", "Open the WebSocket connection now."),
					TEXT("Icons.Refresh"))
			]
			+ SUniformGridPanel::Slot(1, 0)
			[
				BuildButton(
					LOCTEXT("Disconnect", "Disconnect"),
					FOnClicked::CreateSP(this, &SUbstreamControlPanel::DisconnectClicked),
					LOCTEXT("DisconnectTooltip", "Stop streaming and pause auto-reconnect."),
					TEXT("Icons.Unlink"),
					true)
			]
			+ SUniformGridPanel::Slot(0, 1)
			[
				BuildButton(
					LOCTEXT("UseSelectedMesh", "Capture Selected Actors"),
					FOnClicked::CreateSP(this, &SUbstreamControlPanel::UseSelectedMeshClicked),
					LOCTEXT("UseSelectedMeshTooltip", "Use selected actors as the depth-range target."),
					TEXT("Icons.BulletPoint"))
			]
			+ SUniformGridPanel::Slot(1, 1)
			[
				BuildButton(
					LOCTEXT("SaveSettings", "Save Settings"),
					FOnClicked::CreateSP(this, &SUbstreamControlPanel::SaveSettingsClicked),
					LOCTEXT("SaveSettingsTooltip", "Persist endpoint and generation settings."),
					TEXT("Icons.Save"))
			]
			+ SUniformGridPanel::Slot(0, 2)
			[
				BuildButton(
					LOCTEXT("OpenSettings", "Open Settings"),
					FOnClicked::CreateSP(this, &SUbstreamControlPanel::OpenSettingsClicked),
					LOCTEXT("OpenSettingsTooltip", "Open the full plugin settings page."),
					TEXT("Icons.Settings"))
			]
		],
		true);
}

TSharedRef<SWidget> SUbstreamControlPanel::BuildLogsSection()
{
	LogsSection = BuildExpandableSection(
		LOCTEXT("LogsSection", "Status and Logs"),
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(SelectionInfoText, STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
			.Text(LOCTEXT("SelectionInfoInitial", "No selected modeling actors"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("FrameTraceLabel", "Frame trace"))
				.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SAssignNew(DebugSentFrameText, STextBlock)
						.Text(LOCTEXT("DebugSentFrameInitial", "Sent --"))
						.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 3.0f, 0.0f, 0.0f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
						.BorderBackgroundColor(FLinearColor(0.055f, 0.055f, 0.055f, 1.0f))
						.Padding(1.0f)
						[
							SNew(SBox)
							.WidthOverride(92.0f)
							.HeightOverride(56.0f)
							[
								SNew(SScaleBox)
								.Stretch(EStretch::ScaleToFit)
								[
									SAssignNew(DebugSentFrameImage, SImage)
								]
							]
						]
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SAssignNew(DebugReceivedFrameText, STextBlock)
						.Text(LOCTEXT("DebugReceivedFrameInitial", "Received --"))
						.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 3.0f, 0.0f, 0.0f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
						.BorderBackgroundColor(FLinearColor(0.055f, 0.055f, 0.055f, 1.0f))
						.Padding(1.0f)
						[
							SNew(SBox)
							.WidthOverride(92.0f)
							.HeightOverride(56.0f)
							[
								SNew(SScaleBox)
								.Stretch(EStretch::ScaleToFit)
								[
									SAssignNew(DebugReceivedFrameImage, SImage)
								]
							]
						]
					]
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SAssignNew(StatusText, STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
			.Text(LOCTEXT("InitialStatus", "Disconnected"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SAssignNew(StreamInfoText, STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
			.Text(LOCTEXT("InitialStreamStatus", "Stream stopped"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SAssignNew(ResultInfoText, STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
			.Text(LOCTEXT("InitialResultStatus", "No generation result yet"))
		],
		true,
		FOnBooleanValueChanged::CreateLambda([this](bool bExpanded)
		{
			if (bExpanded && LastDebugSentFrameId > 0)
			{
				UpdateDebugSentFramePreview(LastDebugSentFrameId);
			}
			else if (!bExpanded)
			{
				DebugSentFrameImage->SetImage(nullptr);
				DebugSentFrameBrush.Reset();
			}
		}));
	return LogsSection.ToSharedRef();
}

bool SUbstreamControlPanel::IsModelingModeActive() const
{
	return FUbstreamModelingModeBridge::IsModelingModeActive();
}

bool SUbstreamControlPanel::IsFastStreamProtocol() const
{
	return GetDefault<UUbstreamModelerSettings>()->WebSocketProtocol == EUnboundLoopWebSocketProtocol::FastStream;
}

EVisibility SUbstreamControlPanel::GetModeGateVisibility() const
{
	return IsCaptureWorkspaceReady() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SUbstreamControlPanel::GetModelingControlsVisibility() const
{
	return IsCaptureWorkspaceReady() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SUbstreamControlPanel::GetReturnedFramePlaceholderVisibility() const
{
	return bHasReturnedFramePreview ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SUbstreamControlPanel::GetLoadingIndicatorVisibility() const
{
	return WorkflowState == EWorkflowState::Connecting || WorkflowState == EWorkflowState::Loading
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

EVisibility SUbstreamControlPanel::GetPassthroughHintVisibility() const
{
	return bHasReturnedFramePreview && IsLocalPassthroughPreviewMode()
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

EVisibility SUbstreamControlPanel::GetSnapshotSectionVisibility() const
{
	return Snapshots.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SUbstreamControlPanel::GetSnapshotViewPillVisibility() const
{
	return IsViewingSnapshot() ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SUbstreamControlPanel::GetModeGateBodyText() const
{
	const float AvailableWidth = ModeGateBodyBox.IsValid()
		? ModeGateBodyBox->GetCachedGeometry().GetLocalSize().X
		: 0.0f;

	if (AvailableWidth > 0.0f && AvailableWidth < 180.0f)
	{
		return LOCTEXT("ModeGateBodyTiny", "Drop mesh or press Use Selected Mesh.");
	}

	if (AvailableWidth > 0.0f && AvailableWidth < 280.0f)
	{
		return LOCTEXT("ModeGateBodyCompact", "Drop a mesh here, or select one and press Use Selected Mesh.");
	}

	if (AvailableWidth > 0.0f && AvailableWidth < 420.0f)
	{
		return LOCTEXT("ModeGateBodyMedium", "Drop a mesh, actor, component, or 3D file here, or select one and press Use Selected Mesh.");
	}

	return LOCTEXT("ModeGateBody", "Select a Static Mesh asset, component, or actor anywhere in the editor and press Use Selected Mesh below - or drag a mesh or 3D file straight into the stage viewport.");
}

float SUbstreamControlPanel::GetModeGateBodyWrapWidth() const
{
	const float AvailableWidth = ModeGateBodyBox.IsValid()
		? ModeGateBodyBox->GetCachedGeometry().GetLocalSize().X
		: 0.0f;

	if (AvailableWidth <= 1.0f)
	{
		return 360.0f;
	}

	return FMath::Clamp(AvailableWidth - 2.0f, 48.0f, 560.0f);
}

const FSlateBrush* SUbstreamControlPanel::GetPauseButtonIcon() const
{
	return FAppStyle::GetBrush(bStreaming ? TEXT("Icons.Toolbar.Pause") : TEXT("Icons.Toolbar.Play"));
}

FText SUbstreamControlPanel::GetPauseButtonToolTip() const
{
	if (bStreaming)
	{
		return LOCTEXT("PauseStreamToolTip", "Pause the live stream. The connection stays open.");
	}

	return bUserPausedStream
		? LOCTEXT("ResumeStreamToolTip", "Resume the live stream.")
		: LOCTEXT("StartStreamToolTip", "Start the live stream.");
}

FText SUbstreamControlPanel::GetSnapshotCountText() const
{
	return FText::Format(LOCTEXT("SnapshotCountFormat", "Snapshots  {0} / {1}"), Snapshots.Num(), MaxSnapshotCount);
}

FText SUbstreamControlPanel::GetViewedSnapshotLabel() const
{
	if (!IsViewingSnapshot())
	{
		return FText::GetEmpty();
	}

	return FText::Format(LOCTEXT("ViewedSnapshotFormat", "Snapshot {0} of {1}"), ViewedSnapshotIndex + 1, Snapshots.Num());
}

FText SUbstreamControlPanel::GetSeedReadoutText() const
{
	return FText::FromString(FString::Printf(TEXT("#%d"), GenerationSeed));
}

bool SUbstreamControlPanel::IsSnapshotButtonEnabled() const
{
	return !CurrentPreviewImageBytes.IsEmpty();
}

bool SUbstreamControlPanel::IsImportButtonEnabled() const
{
	return IsViewingSnapshot() || !CurrentPreviewImageBytes.IsEmpty();
}

FSlateColor SUbstreamControlPanel::GetWorkflowStateAccentColor() const
{
	switch (WorkflowState)
	{
	case EWorkflowState::Inactive:
		return FSlateColor(FUbstreamModelerStyle::StatusIdle());
	case EWorkflowState::Disconnected:
		return FSlateColor(FUbstreamModelerStyle::StatusConnecting());
	case EWorkflowState::Connecting:
		return FSlateColor(FUbstreamModelerStyle::StatusConnecting());
	case EWorkflowState::Loading:
		return FSlateColor(FUbstreamModelerStyle::StatusWarning());
	case EWorkflowState::Ready:
		return FSlateColor(FUbstreamModelerStyle::StatusReady());
	case EWorkflowState::Streaming:
		return FSlateColor(FUbstreamModelerStyle::StatusStreaming());
	case EWorkflowState::Paused:
		return FSlateColor(FUbstreamModelerStyle::StatusWarning());
	case EWorkflowState::Warning:
		return FSlateColor(FUbstreamModelerStyle::StatusWarning());
	case EWorkflowState::Error:
		return FSlateColor(FUbstreamModelerStyle::StatusError());
	default:
		return FSlateColor(FLinearColor::White);
	}
}

bool SUbstreamControlPanel::HasSoloMeshLoaded() const
{
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	return Viewport.IsValid() && Viewport->HasSoloMesh();
}

bool SUbstreamControlPanel::IsCaptureWorkspaceReady() const
{
	// Mesh-first workflow: the workspace exists only around a loaded isolated
	// subject. Level-editor Modeling Mode on its own does not start capture.
	return HasSoloMeshLoaded();
}

bool SUbstreamControlPanel::IsLocalPassthroughPreviewMode() const
{
	if (!IsFastStreamProtocol())
	{
		return false;
	}

	if (!HasPromptText())
	{
		return true;
	}

	return !WebSocketClient.IsValid()
		|| !WebSocketClient->IsConnected()
		|| bReconnectPending
		|| !bFastStreamReady;
}

void SUbstreamControlPanel::EnsureLocalPassthroughPreviewRunning()
{
	if (!IsLocalPassthroughPreviewMode()
		|| !IsCaptureWorkspaceReady()
		|| bUserPausedStream
		|| bManualDisconnectDuringCurrentWorkspace)
	{
		return;
	}

	if (!bStreaming)
	{
		StartStreaming();
		return;
	}

	CaptureAndSendFrame(/*bTrackInFlight*/ false, /*bForceSend*/ true);
}

bool SUbstreamControlPanel::RequireCaptureSubject(const FString& ActionName)
{
	if (IsCaptureWorkspaceReady())
	{
		return true;
	}

	StopStreaming(TEXT("Stream stopped: load a mesh into the stage to continue"));
	SetWorkflowState(
		EWorkflowState::Inactive,
		TEXT("Load a mesh"),
		FString::Printf(TEXT("Load a mesh into the stage first. Requested action: %s."), *ActionName));
	SetStatus(FString::Printf(TEXT("Load a mesh into the stage to %s"), *ActionName));
	return false;
}

void SUbstreamControlPanel::TryAutoConnectForWorkspace()
{
	if (!FUbstreamAuthService::Get().IsAuthenticated()) { return; }
	if (!IsCaptureWorkspaceReady()
		|| bAutoConnectAttemptedForCurrentWorkspace
		|| bManualDisconnectDuringCurrentWorkspace
		|| bReconnectPending
		|| !WebSocketClient.IsValid()
		|| WebSocketClient->IsConnected())
	{
		return;
	}

	const FString EndpointUrl = GetEndpointUrl();
	if (EndpointUrl.IsEmpty())
	{
		bAutoConnectAttemptedForCurrentWorkspace = true;
		SetWorkflowState(
			EWorkflowState::Warning,
			TEXT("Endpoint required"),
			TEXT("Set a WebSocket endpoint before starting the live stream."));
		SetStatus(TEXT("Auto-connect skipped: WebSocket endpoint is empty"));
		return;
	}
	if (!UbstreamTransportSecurity::IsAllowedUrl(EndpointUrl, true))
	{
		bAutoConnectAttemptedForCurrentWorkspace = true;
		SetWorkflowState(
			EWorkflowState::Warning,
			TEXT("Invalid endpoint"),
			TEXT("WebSocket endpoints require wss:// (ws:// is allowed only for localhost development)."));
		SetStatus(TEXT("Auto-connect skipped: endpoint require wss:// (ws:// is allowed only for localhost development)"));
		return;
	}

	bAutoConnectAttemptedForCurrentWorkspace = true;
	SetWorkflowState(
		EWorkflowState::Connecting,
		TEXT("Connecting automatically"),
		FString::Printf(TEXT("Opening WebSocket connection to %s."), *EndpointUrl));
	SetStatus(FString::Printf(TEXT("Auto-connecting to %s"), *EndpointUrl));
	WebSocketClient->Connect(EndpointUrl);
}

void SUbstreamControlPanel::ScheduleReconnect(const FString& Reason)
{
	if (!FUbstreamAuthService::Get().IsAuthenticated()) { return; }
	if (!IsCaptureWorkspaceReady()
		|| bManualDisconnectDuringCurrentWorkspace
		|| !WebSocketClient.IsValid()
		|| WebSocketClient->IsConnected()
		|| bReconnectPending)
	{
		return;
	}

	const FString EndpointUrl = GetEndpointUrl();
	if (EndpointUrl.IsEmpty())
	{
		SetWorkflowState(
			EWorkflowState::Warning,
			TEXT("Endpoint required"),
			TEXT("Set a WebSocket endpoint before reconnecting."));
		SetStatus(TEXT("Reconnect skipped: WebSocket endpoint is empty"));
		return;
	}
	if (!UbstreamTransportSecurity::IsAllowedUrl(EndpointUrl, true))
	{
		SetWorkflowState(
			EWorkflowState::Warning,
			TEXT("Invalid endpoint"),
			TEXT("WebSocket endpoints require wss:// (ws:// is allowed only for localhost development)."));
		SetStatus(TEXT("Reconnect skipped: endpoint require wss:// (ws:// is allowed only for localhost development)"));
		return;
	}

	bReconnectPending = true;
	bAutoConnectAttemptedForCurrentWorkspace = true;
	SetWorkflowState(
		EWorkflowState::Disconnected,
		TEXT("Reconnecting in 5s"),
		Reason.IsEmpty() ? TEXT("The stream server disconnected unexpectedly.") : Reason);
	SetStatus(Reason.IsEmpty()
		? TEXT("Unexpected disconnect; reconnecting in 5 seconds")
		: FString::Printf(TEXT("Unexpected disconnect: %s. Reconnecting in 5 seconds"), *Reason));
	SetStreamInfo(TEXT("Stream paused while reconnecting"));
	NotifyUser(Reason.IsEmpty()
		? LOCTEXT("ReconnectScheduledToast", "Connection lost. Reconnecting in 5 seconds.")
		: FText::Format(LOCTEXT("ReconnectScheduledReasonToast", "Connection lost: {0}. Reconnecting in 5 seconds."), FText::FromString(Reason)));

	ReconnectTimerHandle = RegisterActiveTimer(
		ReconnectDelaySeconds,
		FWidgetActiveTimerDelegate::CreateSP(this, &SUbstreamControlPanel::HandleReconnectTimerTick));
}

void SUbstreamControlPanel::CancelReconnect()
{
	bReconnectPending = false;

	if (TSharedPtr<FActiveTimerHandle> TimerHandle = ReconnectTimerHandle.Pin())
	{
		UnRegisterActiveTimer(TimerHandle.ToSharedRef());
		ReconnectTimerHandle.Reset();
	}
}

bool SUbstreamControlPanel::ShouldAutoReconnectAfterClose(int32 StatusCode, const FString& Reason, bool bWasClean) const
{
	if (bManualDisconnectDuringCurrentWorkspace)
	{
		return false;
	}
	// Explicit server retry codes also apply to a clean WebSocket close.
	// A verified Unbound key remains available for a retryable service outage.
	if (StatusCode == 1013) return true;
	if (bWasClean) return false;

	if (IsNonRetryableDisconnectReason(Reason))
	{
		return false;
	}

	// 1008 is a policy violation. The backend uses it for auth, quota, and
	// duplicate-session failures that require user/server action rather than a
	// blind reconnect loop.
	if (StatusCode == 1008)
	{
		return false;
	}

	return true;
}

bool SUbstreamControlPanel::IsNonRetryableDisconnectReason(const FString& Reason) const
{
	FString NormalizedReason = Reason;
	NormalizedReason.ToLowerInline();

	return NormalizedReason.Contains(TEXT("auth required"))
		|| NormalizedReason.Contains(TEXT("auth failed"))
		|| NormalizedReason.Contains(TEXT("empty auth token"))
		|| NormalizedReason.Contains(TEXT("auth token too large"))
		|| NormalizedReason.Contains(TEXT("only one connection per user"))
		|| NormalizedReason.Contains(TEXT("credits depleted"));
}

EActiveTimerReturnType SUbstreamControlPanel::HandleReconnectTimerTick(double InCurrentTime, float InDeltaTime)
{
	if (!FUbstreamAuthService::Get().IsAuthenticated()) { return EActiveTimerReturnType::Stop; }
	ReconnectTimerHandle.Reset();

	if (!bReconnectPending)
	{
		return EActiveTimerReturnType::Stop;
	}

	bReconnectPending = false;

	if (!IsCaptureWorkspaceReady() || bManualDisconnectDuringCurrentWorkspace || !WebSocketClient.IsValid())
	{
		return EActiveTimerReturnType::Stop;
	}

	if (WebSocketClient->IsConnected())
	{
		return EActiveTimerReturnType::Stop;
	}

	const FString EndpointUrl = GetEndpointUrl();
	if (EndpointUrl.IsEmpty())
	{
		SetWorkflowState(
			EWorkflowState::Warning,
			TEXT("Endpoint required"),
			TEXT("Set a WebSocket endpoint before reconnecting."));
		SetStatus(TEXT("Reconnect skipped: WebSocket endpoint is empty"));
		return EActiveTimerReturnType::Stop;
	}
	if (!UbstreamTransportSecurity::IsAllowedUrl(EndpointUrl, true))
	{
		SetWorkflowState(
			EWorkflowState::Warning,
			TEXT("Invalid endpoint"),
			TEXT("WebSocket endpoints require wss:// (ws:// is allowed only for localhost development)."));
		SetStatus(TEXT("Reconnect skipped: endpoint require wss:// (ws:// is allowed only for localhost development)"));
		return EActiveTimerReturnType::Stop;
	}

	SetWorkflowState(
		EWorkflowState::Connecting,
		TEXT("Reconnecting"),
		FString::Printf(TEXT("Opening WebSocket connection to %s."), *EndpointUrl));
	SetStatus(FString::Printf(TEXT("Reconnecting to %s"), *EndpointUrl));
	WebSocketClient->Connect(EndpointUrl);
	return EActiveTimerReturnType::Stop;
}

EActiveTimerReturnType SUbstreamControlPanel::HandleModeStateTick(double InCurrentTime, float InDeltaTime)
{
	const bool bWorkspaceReady = IsCaptureWorkspaceReady();

	if (bWorkspaceReady != bLastWorkspaceReady)
	{
		bLastWorkspaceReady = bWorkspaceReady;
		if (bWorkspaceReady)
		{
			WorkspaceReadySinceSeconds = InCurrentTime;
			WorkspaceUnreadySinceSeconds = -1.0;
			// NotifyCaptureWorkspaceChanged owns the ready-state messaging.
		}
		else
		{
			WorkspaceReadySinceSeconds = -1.0;
			WorkspaceUnreadySinceSeconds = InCurrentTime;
			SetWorkflowState(
				EWorkflowState::Paused,
				TEXT("Subject removed"),
				TEXT("Live capture stops unless a mesh is loaded again shortly."));
			SetStatus(TEXT("Isolated subject removed; waiting before disconnect"));
		}

		Invalidate(EInvalidateWidgetReason::LayoutAndVolatility);
	}

	if (bWorkspaceReady)
	{
		if (WorkspaceReadySinceSeconds < 0.0)
		{
			WorkspaceReadySinceSeconds = InCurrentTime;
		}

		if (InCurrentTime - WorkspaceReadySinceSeconds >= ModelingModeAutoConnectDelaySeconds)
		{
			TryAutoConnectForWorkspace();
		}
	}
	else if (WorkspaceUnreadySinceSeconds >= 0.0
		&& InCurrentTime - WorkspaceUnreadySinceSeconds >= ModelingModeExitDisconnectDelaySeconds)
	{
		if (bStreaming)
		{
			StopStreaming(TEXT("Stream stopped: no isolated subject is loaded"));
		}

		if (WebSocketClient.IsValid() && WebSocketClient->IsConnected())
		{
			WebSocketClient->Disconnect();
		}

		CancelReconnect();
		bAutoConnectAttemptedForCurrentWorkspace = false;
		bManualDisconnectDuringCurrentWorkspace = false;
		bUserPausedStream = false;
		WorkspaceUnreadySinceSeconds = -1.0;
		SetWorkflowState(
			EWorkflowState::Inactive,
			TEXT("Load a mesh"),
			TEXT("Disconnected automatically because no subject is loaded."));
		SetStatus(TEXT("Disconnected automatically after the stage was cleared"));
	}

	return EActiveTimerReturnType::Continue;
}

FReply SUbstreamControlPanel::ConnectClicked()
{
	if (!FUbstreamAuthService::Get().IsAuthenticated()) { return FReply::Handled(); }
	if (!RequireCaptureSubject(TEXT("connect WebSocket")))
	{
		return FReply::Handled();
	}

	bManualDisconnectDuringCurrentWorkspace = false;
	bAutoConnectAttemptedForCurrentWorkspace = true;
	bUserPausedStream = false;
	CancelReconnect();
	if (!UbstreamTransportSecurity::IsAllowedUrl(GetEndpointUrl(), true))
	{
		SetWorkflowState(
			EWorkflowState::Warning,
			TEXT("Invalid endpoint"),
			TEXT("WebSocket endpoints require wss:// (ws:// is allowed only for localhost development)."));
		SetStatus(TEXT("Cannot connect: endpoint require wss:// (ws:// is allowed only for localhost development)"));
		NotifyUser(LOCTEXT("InvalidEndpointToast", "WebSocket endpoint require wss:// (ws:// is allowed only for localhost development)."), true);
		return FReply::Handled();
	}
	SetWorkflowState(
		EWorkflowState::Connecting,
		TEXT("Connecting"),
		FString::Printf(TEXT("Opening WebSocket connection to %s."), *GetEndpointUrl()));
	SetStatus(FString::Printf(TEXT("Connecting to %s"), *GetEndpointUrl()));
	WebSocketClient->Connect(GetEndpointUrl());
	return FReply::Handled();
}

FReply SUbstreamControlPanel::DisconnectClicked()
{
	bManualDisconnectDuringCurrentWorkspace = true;
	bAutoConnectAttemptedForCurrentWorkspace = true;
	CancelReconnect();
	CancelFastStreamFlushTimer();
	StopStreaming(TEXT("Stream stopped"));
	WebSocketClient->Disconnect();
	SetWorkflowState(
		IsCaptureWorkspaceReady() ? EWorkflowState::Disconnected : EWorkflowState::Inactive,
		IsCaptureWorkspaceReady() ? TEXT("Disconnected") : TEXT("Load a mesh"),
			IsCaptureWorkspaceReady()
				? TEXT("Connect again when you are ready to stream.")
				: TEXT("Streaming and capture are paused until a mesh is loaded into the stage."));
	SetStatus(TEXT("Disconnected"));
	NotifyUser(LOCTEXT("DisconnectedToast", "Disconnected. Auto-reconnect paused."));
	return FReply::Handled();
}

FReply SUbstreamControlPanel::HandleGateLoadSelectedClicked()
{
	if (!OnLoadSelectedRequested.IsBound())
	{
		SetStatus(TEXT("Select a mesh and press + in the stage toolbar to load it"));
		return FReply::Handled();
	}

	OnLoadSelectedRequested.Execute();
	if (!HasSoloMeshLoaded())
	{
		SetStatus(TEXT("Nothing loadable is selected: pick a Static Mesh asset, component, or actor and try again"));
	}
	return FReply::Handled();
}

FReply SUbstreamControlPanel::UseSelectedMeshClicked()
{
	if (!RequireCaptureSubject(TEXT("use the selected mesh")))
	{
		return FReply::Handled();
	}

	if (!IsModelingModeActive())
	{
		if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin(); Viewport.IsValid() && Viewport->HasSoloMesh())
		{
				MarkFrameUploadDirty();
				SetSelectionInfo(FString::Printf(TEXT("Isolated subject: %s\nSource: %s"), *Viewport->GetSoloMeshLabel(), *Viewport->GetSoloMeshPath()));
				SetWorkflowState(
					WebSocketClient.IsValid() && WebSocketClient->IsConnected() ? EWorkflowState::Ready : EWorkflowState::Disconnected,
					TEXT("Isolated subject ready"),
					TEXT("RGB and depth capture will use only the isolated subject."));
				SetStatus(FString::Printf(TEXT("Using isolated source: %s"), *Viewport->GetSoloMeshLabel()));
		}
		return FReply::Handled();
	}

	const int32 TaggedCount = FUbstreamModelingModeBridge::TagSelectedModelingActors();
	const TArray<FUbstreamSelectedModelingObject> Objects = FUbstreamModelingModeBridge::GetSelectedModelingActors();
	MarkFrameUploadDirty();

	SetSelectionInfo(FUbstreamModelingModeBridge::BuildSelectionSummary(Objects));
	SetWorkflowState(
		WebSocketClient.IsValid() && WebSocketClient->IsConnected() ? EWorkflowState::Ready : EWorkflowState::Disconnected,
		TaggedCount > 0 ? TEXT("Selection ready") : TEXT("No modeling selection"),
		TaggedCount > 0
			? TEXT("Depth range will use the selected actor bounds.")
			: TEXT("Select one or more modeling actors to lock the depth range."));
	SetStatus(FString::Printf(
		TEXT("Tagged %d selected actor(s) with %s and %s"),
		TaggedCount,
		*FUbstreamModelingModeBridge::SandboxTag.ToString(),
		*FUbstreamModelingModeBridge::DoNotCommitTag.ToString()));
	NotifyUser(
		TaggedCount > 0
			? FText::Format(LOCTEXT("SelectionCapturedToastFormat", "Captured {0} selected actor(s)."), FText::AsNumber(TaggedCount))
			: LOCTEXT("NoSelectionCapturedToast", "Select modeling actors before capturing selection."),
		TaggedCount <= 0);

	return FReply::Handled();
}

FReply SUbstreamControlPanel::GenerateClicked()
{
	if (!RequireCaptureSubject(TEXT("generate")))
	{
		return FReply::Handled();
	}

	if (!WebSocketClient->IsConnected())
	{
		SetStatus(TEXT("Failed to generate: socket is disconnected"));
		return FReply::Handled();
	}

	if (IsFastStreamProtocol())
	{
		MarkFrameUploadDirty();
		if (!bFastStreamReady)
		{
			QueueFastStreamAutoRun(TEXT("Prompt queued; FastStream is still loading models"));
			return FReply::Handled();
		}

		SendParamsUpdateMessage();
		KickFastStreamFrameIfReady();
		return FReply::Handled();
	}

	SendSelectionStateMessage();

	if (!CaptureAndSendFrame(true, true))
	{
		return FReply::Handled();
	}

	if (WebSocketClient->SendJson(BuildGenerateRequestJson()))
	{
		SetStatus(FString::Printf(TEXT("Sent generate_request for frame %llu"), LastCapturedFrame.FrameId));
	}
	else
	{
		SetStatus(TEXT("Failed to send generate_request"));
	}

	return FReply::Handled();
}

FReply SUbstreamControlPanel::TogglePauseClicked()
{
	if (bStreaming)
	{
		bUserPausedStream = true;

		StopStreaming(TEXT("Stream paused"));
		SetWorkflowState(
			EWorkflowState::Paused,
			TEXT("Paused"),
			TEXT("Live capture is paused. Press play to resume."));
		SetStatus(TEXT("Stream paused; the connection stays open"));
		return FReply::Handled();
	}

	bUserPausedStream = false;
	StartStreaming();
	return FReply::Handled();
}

FReply SUbstreamControlPanel::RerollSeedClicked()
{
	GenerationSeed = FMath::RandRange(0, MaxGenerationSeed);
	SetStatus(FString::Printf(TEXT("New variation seed: %d"), GenerationSeed));
	MarkFrameUploadDirty();
	QueueFastStreamAutoRun(TEXT("Seed updated; waiting for FastStream Ready"));
	SchedulePushParams(0.05f);
	return FReply::Handled();
}

FReply SUbstreamControlPanel::SnapshotClicked()
{
	if (CurrentPreviewImageBytes.IsEmpty())
	{
		SetStatus(TEXT("Nothing to snapshot yet; wait for the first frame"));
		return FReply::Handled();
	}

	FUbstreamFrameSnapshot Snapshot;
	Snapshot.FrameId = LastPreviewedFrameId;
	Snapshot.Prompt = GetPromptText().TrimStartAndEnd();
	Snapshot.Seed = GenerationSeed;
	Snapshot.CapturedAtUtc = FDateTime::UtcNow();
	Snapshot.ImageBytes = CurrentPreviewImageBytes;
	Snapshot.bFromServer = bCurrentPreviewIsServerFrame && !IsLocalPassthroughPreviewMode();

	FString ErrorMessage;
	Snapshot.Brush = CreateBrushFromCompressedImage(Snapshot.ImageBytes, TEXT("Snapshot"), ErrorMessage);
	if (!Snapshot.Brush.IsValid())
	{
		SetStatus(FString::Printf(TEXT("Snapshot failed: %s"), *ErrorMessage));
		return FReply::Handled();
	}

	Snapshots.Insert(MoveTemp(Snapshot), 0);
	if (ViewedSnapshotIndex != INDEX_NONE)
	{
		++ViewedSnapshotIndex;
	}

	while (Snapshots.Num() > MaxSnapshotCount)
	{
		Snapshots.RemoveAt(Snapshots.Num() - 1);
		if (ViewedSnapshotIndex >= Snapshots.Num())
		{
			ViewedSnapshotIndex = INDEX_NONE;
			RefreshPreviewImage();
		}
	}

	RebuildSnapshotStrip();
	SetStatus(FString::Printf(TEXT("Kept snapshot of frame %llu"), Snapshots[0].FrameId));
	return FReply::Handled();
}

FReply SUbstreamControlPanel::BackToLiveClicked()
{
	SelectSnapshot(INDEX_NONE);
	SetStatus(TEXT("Showing live preview"));
	return FReply::Handled();
}

FReply SUbstreamControlPanel::ImportDisplayedFrameClicked()
{
	const bool bFromSnapshot = IsViewingSnapshot();
	const TArray<uint8>& ImageBytes = bFromSnapshot ? Snapshots[ViewedSnapshotIndex].ImageBytes : CurrentPreviewImageBytes;
	const uint64 FrameId = bFromSnapshot ? Snapshots[ViewedSnapshotIndex].FrameId : LastPreviewedFrameId;
	const int32 FrameSeed = bFromSnapshot ? Snapshots[ViewedSnapshotIndex].Seed : GenerationSeed;

	if (ImageBytes.IsEmpty())
	{
		SetStatus(TEXT("No frame available to import yet"));
		return FReply::Handled();
	}

	const FString SourceName = FString::Printf(TEXT("Frame%llu_Seed%d.jpg"), FrameId, FrameSeed);
	FString AssetPath;
	FString ErrorMessage;
	if (FUbstreamResultImportService::ImportImageBytes(SourceName, ImageBytes, AssetPath, ErrorMessage))
	{
		LastImportedAssetPath = AssetPath;
		SetStatus(FString::Printf(TEXT("Imported %s"), *AssetPath));
		SetResultInfo(FString::Printf(TEXT("Imported asset: %s"), *AssetPath));
		NotifyUser(FText::Format(LOCTEXT("FrameImportedToast", "Imported {0}."), FText::FromString(AssetPath)));
	}
	else
	{
		SetStatus(FString::Printf(TEXT("Import failed: %s"), *ErrorMessage));
		NotifyUser(FText::Format(LOCTEXT("FrameImportFailedToast", "Import failed: {0}"), FText::FromString(ErrorMessage)), true);
	}

	return FReply::Handled();
}

FReply SUbstreamControlPanel::ClearSnapshotsClicked()
{
	Snapshots.Empty();
	ViewedSnapshotIndex = INDEX_NONE;
	RebuildSnapshotStrip();
	RefreshPreviewImage();
	SetStatus(TEXT("Cleared snapshots"));
	return FReply::Handled();
}

FReply SUbstreamControlPanel::SaveSettingsClicked()
{
	UUbstreamModelerSettings* Settings = GetMutableDefault<UUbstreamModelerSettings>();
	Settings->DefaultWebSocketUrl = GetEndpointUrl();
	Settings->PromptStrength = GetPromptStrength();
	Settings->DepthStrength = GetDepthStrength();
	Settings->GenerationSeed = GenerationSeed;
	Settings->SaveConfig();
	SetStatus(TEXT("Saved Unbound Loop editor settings"));
	NotifyUser(LOCTEXT("SettingsSavedToast", "Unbound Loop settings saved."));
	return FReply::Handled();
}

FReply SUbstreamControlPanel::OpenSettingsClicked()
{
	const UUbstreamModelerSettings* Settings = GetDefault<UUbstreamModelerSettings>();
	if (ISettingsModule* SettingsModule = FModuleManager::LoadModulePtr<ISettingsModule>(TEXT("Settings")))
	{
		SettingsModule->ShowViewer(Settings->GetContainerName(), Settings->GetCategoryName(), Settings->GetSectionName());
		SetStatus(TEXT("Opened Unbound Loop settings"));
	}
	else
	{
		SetStatus(TEXT("Failed to open settings viewer"));
	}

	return FReply::Handled();
}

bool SUbstreamControlPanel::CaptureEditorWorldFrame()
{
	bLastCapturedFrameWasSoloViewport = false;

	if (!RequireCaptureSubject(TEXT("capture the viewport")))
	{
		return false;
	}

	if (HasSoloMeshLoaded())
	{
		return CaptureSoloViewportFrame();
	}

	LastCapturedTicket = CaptureState.Snapshot(BuildSelectedModelingSceneSignature());
	LastCapturedFrame = FUbstreamEditorWorldCaptureService::CaptureFrameBundle(NextFrameId++, BuildCaptureSettings(), &EditorCaptureTargetPool);
	if (!LastCapturedFrame.bSuccess)
	{
		SetStatus(FString::Printf(TEXT("Capture failed: %s"), *LastCapturedFrame.ErrorMessage));
		return false;
	}

	SetStatus(FString::Printf(
		TEXT("Captured frame %llu: color=%d bytes, depth=%d bytes, debug=%s"),
		LastCapturedFrame.FrameId,
		LastCapturedFrame.ColorJpegBytes.Num(),
		LastCapturedFrame.DepthJpegBytes.Num(),
		*LastCapturedFrame.ColorDebugPath));
	return true;
}

bool SUbstreamControlPanel::CaptureSoloViewportFrame()
{
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (!Viewport.IsValid() || !Viewport->HasSoloMesh())
	{
		SetStatus(TEXT("Solo capture failed: no isolated subject is loaded"));
		return false;
	}

	bLastCapturedFrameWasSoloViewport = true;
	LastCapturedTicket = CaptureState.Snapshot(BuildSelectedModelingSceneSignature());
	LastCapturedFrame = Viewport->CaptureFrameBundle(NextFrameId++, BuildCaptureSettings());
	if (!LastCapturedFrame.bSuccess)
	{
		SetStatus(FString::Printf(TEXT("Solo capture failed: %s"), *LastCapturedFrame.ErrorMessage));
		return false;
	}

	SetSelectionInfo(FString::Printf(TEXT("Isolated subject: %s\nSource: %s"), *Viewport->GetSoloMeshLabel(), *Viewport->GetSoloMeshPath()));
	const FString FallbackLabel = LastCapturedFrame.bUsedFlatColorFallback
		? TEXT(", flat fallback")
		: LastCapturedFrame.bUsedSceneFallback
			? TEXT(", scene fallback")
			: FString();
	SetStatus(FString::Printf(
		TEXT("Captured solo frame %llu: color=%d bytes, depth=%d bytes%s, debug=%s"),
		LastCapturedFrame.FrameId,
		LastCapturedFrame.ColorJpegBytes.Num(),
		LastCapturedFrame.DepthJpegBytes.Num(),
		*FallbackLabel,
		*LastCapturedFrame.ColorDebugPath));
	return true;
}

bool SUbstreamControlPanel::PreviewCapturedFrameLocally()
{
	if (LastCapturedFrame.ColorJpegBytes.IsEmpty())
	{
		SetStatus(TEXT("Local preview failed: captured color frame is empty"));
		return false;
	}

	ClearPendingReturnedFrameState();
	PendingReturnedFrameId = LastCapturedFrame.FrameId;
	PendingReturnedFramePayloadType = TEXT("local_passthrough");
	PendingReturnedFrameFormat = TEXT("jpeg");
	PendingReturnedFrameBytes = LastCapturedFrame.ColorJpegBytes.Num();
	bHasPendingReturnedFrame = true;

	FString ErrorMessage;
	if (!UpdateReturnedFramePreviewFromBytes(LastCapturedFrame.ColorJpegBytes, ErrorMessage))
	{
		ClearPendingReturnedFrameState();
		SetStatus(FString::Printf(TEXT("Local preview failed: %s"), *ErrorMessage));
		return false;
	}

	LastPreviewedFrameId = LastCapturedFrame.FrameId;
	LastReturnedFramePath.Reset();
	LastResultUrl.Reset();
	ClearPendingReturnedFrameState();
	const FString SourceLabel = bLastCapturedFrameWasSoloViewport
		? TEXT("isolated renderer")
		: TEXT("editor viewport");
	const FString FallbackLabel = LastCapturedFrame.bUsedFlatColorFallback
		? TEXT(" / flat visibility fallback")
		: LastCapturedFrame.bUsedSceneFallback
			? TEXT(" / scene fallback")
			: FString();
	SetStatus(FString::Printf(TEXT("Rendered local passthrough frame %llu from %s"), LastCapturedFrame.FrameId, *SourceLabel));
	SetResultInfo(FString::Printf(TEXT("Local Unreal passthrough\nSource: %s%s"), *SourceLabel, *FallbackLabel));
	return true;
}

bool SUbstreamControlPanel::CaptureAndSendFrame(bool bTrackInFlight, bool bForceSend)
{
	if (FUbstreamModelerEditorModule::IsInsideRenderFlush())
	{
		CaptureState.MarkDirty();
		return true; // The next stream tick retries without stopping the session.
	}

	const bool bLocalPreviewOnly = IsLocalPassthroughPreviewMode();

	if (!bLocalPreviewOnly && !WebSocketClient->IsConnected())
	{
		SetStatus(TEXT("Failed to send frame_bundle: socket is disconnected"));
		return false;
	}

	if (!bLocalPreviewOnly && IsFastStreamProtocol() && !bFastStreamReady)
	{
		SetStatus(TEXT("Skipped capture send: waiting for FastStream Ready status"));
		SetStreamInfo(TEXT("Stream waiting for FastStream Ready"));
		return false;
	}

	if (!bLocalPreviewOnly && !FUbstreamCaptureState::HasSendCapacity(
		IsFastStreamProtocol(), InFlightFrames.Num(), GetMaxInFlightFrames()))
	{
		SetStreamInfo(TEXT("Waiting for UBS1 frame acknowledgement"));
		return true;
	}

	float CadenceWaitSeconds = 0.0f;
	if (ShouldDelayFrameUploadForCadence(bForceSend, CadenceWaitSeconds))
	{
		SetStreamInfo(FString::Printf(
			TEXT("Streaming at %.1f FPS; queued latest dirty frame (next send in %d ms)"),
			1.0f / GetStreamIntervalSeconds(),
			FMath::Max(0, FMath::RoundToInt(CadenceWaitSeconds * 1000.0f))));
		return true;
	}

	if (!CaptureEditorWorldFrame())
	{
		return false;
	}

	return SendCapturedFrame(bTrackInFlight, bForceSend);
}

bool SUbstreamControlPanel::SendCapturedFrame(bool bTrackInFlight, bool bForceSend)
{
	// Dedup -> send -> remember-signature/cadence. Shared by the synchronous
	// CaptureAndSendFrame and the async FinishAsyncCaptureAndSend; reads the
	// already-populated LastCapturedFrame.
	const bool bLocalPreviewOnly = IsLocalPassthroughPreviewMode();

	if (ShouldSkipCapturedFrameUpload(bForceSend))
	{
		++SkippedUnchangedFrameCount;
		if (SkippedUnchangedFrameCount == 1 || SkippedUnchangedFrameCount % 30 == 0)
		{
			SetStreamInfo(FString::Printf(TEXT("Captured frame unchanged; skipped %llu duplicate upload(s)"), SkippedUnchangedFrameCount));
		}
		return true;
	}

	if (bLocalPreviewOnly)
	{
		const bool bPreviewed = PreviewCapturedFrameLocally();
		if (bPreviewed)
		{
			InFlightFrames.Reset();
			FrameSendTimes.Reset();
			RememberUploadedFrameSignature();
			RememberFrameUploadCadence();
		}
		return bPreviewed;
	}

	if (IsFastStreamProtocol())
	{
		const bool bSent = SendFastStreamFrameMessage(bTrackInFlight);
		if (bSent)
		{
			RememberUploadedFrameSignature();
			RememberFrameUploadCadence();
		}
		return bSent;
	}

	if (!FUbstreamCaptureState::HasSendCapacity(false, InFlightFrames.Num(), GetMaxInFlightFrames()))
	{
		return false;
	}

	const int32 MaxFrameBundleBytes = GetMaxFrameBundleBytes();
	if (LastCapturedFrame.FrameBundleBytes.Num() > MaxFrameBundleBytes)
	{
		SetStatus(FString::Printf(
			TEXT("Skipped frame_bundle %llu: %d bytes exceeds configured limit of %d bytes"),
			LastCapturedFrame.FrameId,
			LastCapturedFrame.FrameBundleBytes.Num(),
			MaxFrameBundleBytes));
		return false;
	}

	if (!WebSocketClient->SendBinary(LastCapturedFrame.FrameBundleBytes))
	{
		SetStatus(TEXT("Failed to send frame_bundle"));
		return false;
	}
	UpdateDebugSentFramePreview(LastCapturedFrame.FrameId);

	if (bTrackInFlight)
	{
		InFlightFrames.Add(LastCapturedFrame.FrameId);
	}

	RememberUploadedFrameSignature();
	RememberFrameUploadCadence();

	SetStatus(FString::Printf(
		TEXT("Sent frame_bundle %llu (%d bytes, color=%d, depth=%d, in-flight=%d)"),
		LastCapturedFrame.FrameId,
		LastCapturedFrame.FrameBundleBytes.Num(),
		LastCapturedFrame.ColorJpegBytes.Num(),
		LastCapturedFrame.DepthJpegBytes.Num(),
		InFlightFrames.Num()));
	return true;
}

bool SUbstreamControlPanel::StreamLatestFrame(bool bTrackInFlight)
{
	float CadenceWaitSeconds = 0.0f;
	if (ShouldDelayFrameUploadForCadence(false, CadenceWaitSeconds))
	{
		return true;
	}

	// UE 5.8: a scene-capture render that runs while another FlushRenderingCommands
	// is already on the stack executes on the render thread without an inherited
	// FAppTime context and trips a non-fatal game-thread ensure
	// (UpdateReflectionSceneData -> FApp::GetCurrentTime), which pops the crash
	// reporter. Our immediate CaptureScene() flushes internally, so defer the capture
	// whenever a flush is in progress (e.g. a CSG session-start render on the same
	// frame). The stream is latest-wins, so the next tick captures a fresh frame and
	// the skip is invisible.
	if (FUbstreamModelerEditorModule::IsInsideRenderFlush())
	{
		CaptureState.MarkDirty();
		return true;
	}

	// Solo + FastStream remote streaming captures on the game thread but encodes
	// the JPEG off-thread (webapp-style); every other path stays fully
	// synchronous.
	if (HasSoloMeshLoaded() && IsFastStreamProtocol() && !IsLocalPassthroughPreviewMode())
	{
		return BeginAsyncCaptureAndSend(bTrackInFlight);
	}
	return CaptureAndSendFrame(bTrackInFlight, /*bForceSend*/ true);
}

bool SUbstreamControlPanel::BeginAsyncCaptureAndSend(bool bTrackInFlight)
{
	if (FUbstreamModelerEditorModule::IsInsideRenderFlush())
	{
		CaptureState.MarkDirty();
		return true; // The next stream tick retries without stopping the session.
	}

	if (!WebSocketClient.IsValid() || !WebSocketClient->IsConnected())
	{
		SetStatus(TEXT("Failed to send frame_bundle: socket is disconnected"));
		return false;
	}

	if (!bFastStreamReady)
	{
		SetStatus(TEXT("Skipped capture send: waiting for FastStream Ready status"));
		SetStreamInfo(TEXT("Stream waiting for FastStream Ready"));
		return false;
	}

	// Latest-wins: keep at most one capture (pending readback or encode) in
	// flight. A newer tick just keeps the dirty flag; the next tick captures
	// fresh.
	if (CaptureState.IsBusy())
	{
		TryCompletePendingFrameReadback();
		return true;
	}

	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (!Viewport.IsValid() || !Viewport->HasSoloMesh())
	{
		return CaptureAndSendFrame(bTrackInFlight, /*bForceSend*/ true);
	}

	// Async GPU readback: render + enqueue the copy now, zero game-thread
	// stall; completion is polled from Tick and encoded off-thread.
	bLastCapturedFrameWasSoloViewport = true;
	const FUbstreamCaptureTicket Ticket = CaptureState.BeginAsync(BuildSelectedModelingSceneSignature());
	PendingFrameReadback = Viewport->BeginAsyncCaptureFramePixels(NextFrameId++, BuildCaptureSettings());
	if (PendingFrameReadback.IsValid())
	{
		bPendingFrameTrackInFlight = bTrackInFlight;
		TryCompletePendingFrameReadback();
		return true;
	}

	// Async path unavailable (first frame before the persistent capture
	// component exists, or capture setup failure): synchronous fallback.
	FUbstreamCapturePixels Pixels = Viewport->CaptureFramePixels(NextFrameId++, BuildCaptureSettings());
	if (!Pixels.bSuccess)
	{
		CaptureState.FinishAsync(Ticket);
		SetStatus(FString::Printf(TEXT("Solo capture failed: %s"), *Pixels.ErrorMessage));
		return false;
	}

	TWeakPtr<SUbstreamControlPanel> WeakThis = StaticCastSharedRef<SUbstreamControlPanel>(AsShared());
	Async(EAsyncExecution::ThreadPool, [WeakThis, Pixels = MoveTemp(Pixels), bTrackInFlight, Ticket]() mutable
	{
		// Off the game thread: JPEG-encode + build the frame bundle.
		FUbstreamEditorWorldCaptureResult Result = FUbstreamCaptureService::EncodeCapturedFrame(MoveTemp(Pixels));
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result = MoveTemp(Result), bTrackInFlight, Ticket]() mutable
		{
			if (const TSharedPtr<SUbstreamControlPanel> Pinned = WeakThis.Pin())
			{
				Pinned->FinishAsyncCaptureAndSend(MoveTemp(Result), bTrackInFlight, Ticket);
			}
		});
	});

	return true;
}

void SUbstreamControlPanel::FinishAsyncCaptureAndSend(FUbstreamEditorWorldCaptureResult&& Result, bool bTrackInFlight, FUbstreamCaptureTicket Ticket)
{
	// Drop completions from a capture begun before a stop/restart: a newer
	// capture (or none) now owns the in-flight state, so we must not clear its
	// guard or send a stale frame.
	if (!CaptureState.FinishAsync(Ticket))
	{
		return;
	}

	if (Result.bNearlyBlackWithShowOnly)
	{
		// The async path cannot run the black-frame fallback chain (pixels
		// arrive after the render); redo this frame through the synchronous
		// path, which can.
		CaptureAndSendFrame(bTrackInFlight, /*bForceSend*/ true);
		return;
	}

	if (!Result.bSuccess)
	{
		SetStatus(FString::Printf(TEXT("Frame encode failed: %s"), *Result.ErrorMessage));
		return;
	}

	LastCapturedTicket = Ticket;
	LastCapturedFrame = MoveTemp(Result);
	SendCapturedFrame(bTrackInFlight, /*bForceSend*/ true);

	// Changes during capture remain dirty. The cadence-bound stream pump
	// captures them on its next tick; completing a frame must not bypass pacing.
}

void SUbstreamControlPanel::HandleJsonMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		SetStatus(FString::Printf(TEXT("Message: %s"), *Message));
		return;
	}

	FString Type;
	if (!Root->TryGetStringField(TEXT("type"), Type))
	{
		SetStatus(FString::Printf(TEXT("Message: %s"), *Message));
		return;
	}

	if (Type == TEXT("ack"))
	{
		int64 AckFrameId = 0;
		if (Root->TryGetNumberField(TEXT("frameId"), AckFrameId) && AckFrameId > 0)
		{
			LastAckedFrameId = static_cast<uint64>(AckFrameId);
			InFlightFrames.Remove(LastAckedFrameId);
		}

		FString AckType;
		Root->TryGetStringField(TEXT("ackType"), AckType);
		SetStatus(FString::Printf(TEXT("Ack: %s frame=%lld in-flight=%d"), *AckType, AckFrameId, InFlightFrames.Num()));
		SetStreamInfo(bStreaming
			? FString::Printf(TEXT("Streaming at %.1f FPS, in-flight=%d, last ack=%llu"), 1.0f / GetStreamIntervalSeconds(), InFlightFrames.Num(), LastAckedFrameId)
			: FString::Printf(TEXT("Stream stopped, last ack=%llu"), LastAckedFrameId));
		return;
	}

	if (Type == TEXT("returned_frame_header"))
	{
		int64 FrameId = 0;
		Root->TryGetNumberField(TEXT("frameId"), FrameId);
		Root->TryGetStringField(TEXT("payloadType"), PendingReturnedFramePayloadType);
		Root->TryGetStringField(TEXT("format"), PendingReturnedFrameFormat);

		int64 Bytes = 0;
		Root->TryGetNumberField(TEXT("bytes"), Bytes);

		PendingReturnedFrameId = FrameId > 0 ? static_cast<uint64>(FrameId) : 0;
		PendingReturnedFrameBytes = Bytes > 0 && Bytes <= MAX_int32 ? static_cast<int32>(Bytes) : 0;
		bHasPendingReturnedFrame = true;

		SetResultInfo(FString::Printf(
			TEXT("Receiving returned %s frame %llu over WebSocket (%s, %d bytes)"),
			PendingReturnedFramePayloadType.IsEmpty() ? TEXT("binary") : *PendingReturnedFramePayloadType,
			PendingReturnedFrameId,
			PendingReturnedFrameFormat.IsEmpty() ? TEXT("unknown format") : *PendingReturnedFrameFormat,
			PendingReturnedFrameBytes));
		return;
	}

	if (Type == TEXT("generation_result"))
	{
		int64 FrameId = 0;
		Root->TryGetNumberField(TEXT("frameId"), FrameId);

		FString Status;
		Root->TryGetStringField(TEXT("status"), Status);
		if (const TSharedPtr<FJsonObject>* ResultObject = nullptr; Root->TryGetObjectField(TEXT("result"), ResultObject) && ResultObject && ResultObject->IsValid())
		{
			(*ResultObject)->TryGetStringField(TEXT("url"), LastResultUrl);
		}

		SetStatus(FString::Printf(TEXT("Generation result: frame=%lld status=%s"), FrameId, *Status));
		SetResultInfo(LastResultUrl.IsEmpty() ? Message : FString::Printf(TEXT("%s\nResult URL: %s"), *Message, *LastResultUrl));
		return;
	}

	if (Type == TEXT("generation_status"))
	{
		int64 FrameId = 0;
		Root->TryGetNumberField(TEXT("frameId"), FrameId);

		FString Status;
		Root->TryGetStringField(TEXT("status"), Status);
		SetStatus(FString::Printf(TEXT("Generation status: frame=%lld status=%s"), FrameId, *Status));
		SetResultInfo(Message);
		return;
	}

	if (Type == TEXT("error"))
	{
		FString ErrorMessage;
		Root->TryGetStringField(TEXT("message"), ErrorMessage);
		SetStatus(FString::Printf(TEXT("Backend error: %s"), *ErrorMessage));
		SetResultInfo(Message);
		return;
	}

	SetStatus(FString::Printf(TEXT("Message: %s"), *Message));
}

void SUbstreamControlPanel::HandleBinaryMessage(const TArray<uint8>& Message)
{
	if (IsFastStreamProtocol())
	{
		HandleFastStreamBinaryMessage(Message);
		return;
	}

	HandleLegacyBinaryMessage(Message);
}

void SUbstreamControlPanel::HandleLegacyBinaryMessage(const TArray<uint8>& Message)
{
	FString ErrorMessage;
	if (!UpdateReturnedFramePreviewFromBytes(Message, ErrorMessage))
	{
		ClearPendingReturnedFrameState();
		SetStatus(FString::Printf(TEXT("Failed to preview returned WebSocket frame: %s"), *ErrorMessage));
		return;
	}

	FString SavedSuffix;
	if (GetDefault<UUbstreamModelerSettings>()->bSaveDebugCaptures)
	{
		FString ReturnedFramePath;
		if (!SaveReturnedFrameBytes(Message, ReturnedFramePath, ErrorMessage))
		{
			SetStatus(FString::Printf(TEXT("Previewed returned WebSocket frame, but failed to save debug copy: %s"), *ErrorMessage));
		}
		else
		{
			LastReturnedFramePath = ReturnedFramePath;
			LastResultUrl = FString::Printf(TEXT("file://%s"), *LastReturnedFramePath);
			SavedSuffix = FString::Printf(TEXT("\nSaved: %s"), *LastReturnedFramePath);
		}
	}
	else
	{
		LastReturnedFramePath.Reset();
		LastResultUrl.Reset();
	}

	const uint64 ReturnedFrameId = PendingReturnedFrameId;
	ClearPendingReturnedFrameState();

	SetResultInfo(FString::Printf(
		TEXT("Returned RGB frame %llu over WebSocket%s"),
		ReturnedFrameId,
		*SavedSuffix));
}

void SUbstreamControlPanel::HandleFastStreamBinaryMessage(const TArray<uint8>& Message)
{
	if (Message.IsEmpty())
	{
		return;
	}

	const uint8 MessageType = Message[0];
	if (MessageType == static_cast<uint8>(EFastStreamServerMessage::Status))
	{
		if (Message.Num() < 2)
		{
			SetStatus(TEXT("FastStream status message was too small"));
			return;
		}

		const uint8 Status = Message[1];
		if (Status == static_cast<uint8>(EFastStreamServerStatus::Loading))
		{
			bFastStreamReady = false;
			FString LoadingText = TEXT("FastStream is loading models");
			if (Message.Num() > 2)
			{
				const FString LoadingJson = DecodeUtf8(Message, 2);
				TSharedPtr<FJsonObject> Root;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(LoadingJson);
				if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
				{
					int64 Step = 0;
					int64 Total = 0;
					if (Root->TryGetNumberField(TEXT("step"), Step) && Root->TryGetNumberField(TEXT("total"), Total) && Total > 0)
					{
						LoadingText = FString::Printf(TEXT("FastStream loading models: %lld / %lld"), Step, Total);
					}
				}
				SetResultInfo(LoadingJson);
			}
			SetWorkflowState(
				EWorkflowState::Loading,
				TEXT("Loading stream model"),
				bFastStreamStartWhenReady || GetDefault<UUbstreamModelerSettings>()->bAutoStartStreamOnConnect
					? FString::Printf(TEXT("%s. Live capture will start automatically."), *LoadingText)
					: LoadingText);
			SetStatus(LoadingText);
			SetStreamInfo(bFastStreamStartWhenReady || GetDefault<UUbstreamModelerSettings>()->bAutoStartStreamOnConnect
				? TEXT("Stream queued; it will start automatically when FastStream is Ready")
				: TEXT("Stream waiting for server Ready"));
			return;
		}

		if (Status == static_cast<uint8>(EFastStreamServerStatus::Ready))
		{
			bFastStreamReady = true;
			const FString StatusJson = DecodeUtf8(Message, 2);

			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StatusJson);
			if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
			{
				Root->TryGetBoolField(TEXT("depth_support"), bFastStreamDepthSupported);

				int64 ReadyWidth = 0;
				int64 ReadyHeight = 0;
				if (Root->TryGetNumberField(TEXT("width"), ReadyWidth) && Root->TryGetNumberField(TEXT("height"), ReadyHeight) && ReadyWidth > 0 && ReadyHeight > 0)
				{
					FastStreamCaptureWidth = FMath::Clamp(static_cast<int32>(ReadyWidth), 64, 4096);
					FastStreamCaptureHeight = FMath::Clamp(static_cast<int32>(ReadyHeight), 64, 4096);
				}
			}

			SetStatus(FString::Printf(TEXT("FastStream server ready (%dx%d, depth=%s)"),
				FastStreamCaptureWidth,
				FastStreamCaptureHeight,
				bFastStreamDepthSupported ? TEXT("on") : TEXT("off")));
			SetWorkflowState(
				EWorkflowState::Ready,
				TEXT("Server ready"),
				FString::Printf(
					TEXT("Capture size %dx%d. Depth control is %s."),
					FastStreamCaptureWidth,
					FastStreamCaptureHeight,
					bFastStreamDepthSupported ? TEXT("enabled") : TEXT("disabled")));
			SetResultInfo(StatusJson.IsEmpty() ? TEXT("FastStream Ready") : StatusJson);
			MarkFrameUploadDirty();
			SendParamsUpdateMessage();
			bFastStreamParamsPending = false;

			if (!bUserPausedStream
				&& (bFastStreamStartWhenReady || GetDefault<UUbstreamModelerSettings>()->bAutoStartStreamOnConnect))
			{
				bFastStreamStartWhenReady = false;
				StartStreaming();
			}
			return;
		}

		SetStatus(FString::Printf(TEXT("FastStream status: 0x%02x"), Status));
		if (Message.Num() > 2)
		{
			SetResultInfo(DecodeUtf8(Message, 2));
		}
		return;
	}

	if (MessageType == static_cast<uint8>(EFastStreamServerMessage::FrameResponse))
	{
		uint32 FrameId = 0;
		if (!ReadUint32LE(Message, 1, FrameId) || Message.Num() <= 5)
		{
			SetStatus(TEXT("FastStream frame response was malformed"));
			return;
		}

		const uint64 FrameId64 = static_cast<uint64>(FrameId);
		LastAckedFrameId = FMath::Max(LastAckedFrameId, FrameId64);
		InFlightFrames.Remove(FrameId64);
		const double* SentAtSeconds = FrameSendTimes.Find(FrameId64);
		const int32 RoundTripMs = SentAtSeconds
			? FMath::Max(0, FMath::RoundToInt((FPlatformTime::Seconds() - *SentAtSeconds) * 1000.0))
			: -1;
		FrameSendTimes.Remove(FrameId64);
		TArray<uint8> ImageBytes;
		ImageBytes.Append(Message.GetData() + 5, Message.Num() - 5);

		if (HasSelectedModelingSceneSignatureChanged())
		{
			CaptureState.MarkDirty();
			SkippedUnchangedFrameCount = 0;
		}

		// Equal ids are allowed through: the server's Flush passes re-run inference
		// on the last received image and echo its frame id, so the settled frames
		// arrive with an id we already previewed and must still refresh the preview.
		if (FrameId64 < MinimumPreviewableFastStreamFrameId || FrameId64 < LastPreviewedFrameId)
		{
			SetStreamInfo(FString::Printf(
				TEXT("Ignored stale FastStream frame %u; latest preview=%llu"),
				FrameId,
				LastPreviewedFrameId));
			MaybeStreamLatestFastStreamFrame();
			return;
		}

		PendingReturnedFrameId = FrameId64;
		PendingReturnedFramePayloadType = TEXT("faststream");
		PendingReturnedFrameFormat = TEXT("jpeg");
		PendingReturnedFrameBytes = ImageBytes.Num();
		bHasPendingReturnedFrame = true;

		FString ErrorMessage;
		if (!UpdateReturnedFramePreviewFromBytes(ImageBytes, ErrorMessage))
		{
			ClearPendingReturnedFrameState();
			SetStatus(FString::Printf(TEXT("Failed to preview FastStream frame: %s"), *ErrorMessage));
			MaybeStreamLatestFastStreamFrame();
			return;
		}

		LastPreviewedFrameId = FrameId64;

		FString SavedSuffix;
		if (GetDefault<UUbstreamModelerSettings>()->bSaveDebugCaptures)
		{
			FString ReturnedFramePath;
			if (!SaveReturnedFrameBytes(ImageBytes, ReturnedFramePath, ErrorMessage))
			{
				SetStatus(FString::Printf(TEXT("Previewed FastStream frame, but failed to save debug copy: %s"), *ErrorMessage));
			}
			else
			{
				LastReturnedFramePath = ReturnedFramePath;
				LastResultUrl = FString::Printf(TEXT("file://%s"), *LastReturnedFramePath);
				SavedSuffix = FString::Printf(TEXT("\nSaved: %s"), *LastReturnedFramePath);
			}
		}
		else
		{
			LastReturnedFramePath.Reset();
			LastResultUrl.Reset();
		}

		ClearPendingReturnedFrameState();

		SetStatus(RoundTripMs >= 0
			? FString::Printf(TEXT("FastStream frame %u received in %d ms, in-flight=%d"), FrameId, RoundTripMs, InFlightFrames.Num())
			: FString::Printf(TEXT("FastStream frame %u received, in-flight=%d"), FrameId, InFlightFrames.Num()));
		SetResultInfo(RoundTripMs >= 0
			? FString::Printf(TEXT("Returned FastStream frame %u (%d ms)%s"), FrameId, RoundTripMs, *SavedSuffix)
			: FString::Printf(TEXT("Returned FastStream frame %u%s"), FrameId, *SavedSuffix));

		// Backpressure just cleared - immediately stream the freshest capture
		// rather than idling until the next poll tick.
		MaybeStreamLatestFastStreamFrame();
		return;
	}

	SetStatus(FString::Printf(TEXT("Unknown FastStream binary message type: 0x%02x"), MessageType));
}

void SUbstreamControlPanel::StartStreaming()
{
	if (!FUbstreamAuthService::Get().IsAuthenticated()) { return; }
	if (!RequireCaptureSubject(TEXT("start streaming")))
	{
		return;
	}

	const bool bLocalPreviewOnly = IsLocalPassthroughPreviewMode();

	if (!bLocalPreviewOnly && !WebSocketClient->IsConnected())
	{
		SetStatus(TEXT("Cannot start stream: socket is disconnected"));
		return;
	}

	if (!bLocalPreviewOnly && IsFastStreamProtocol() && !bFastStreamReady)
	{
		bFastStreamStartWhenReady = true;
		SetWorkflowState(
			EWorkflowState::Loading,
			TEXT("Waiting for server"),
			TEXT("The stream will start automatically when the model is ready."));
		SetStatus(TEXT("Stream queued; FastStream is still loading models"));
		SetStreamInfo(TEXT("Stream will start automatically when FastStream is Ready"));
		return;
	}

	if (bStreaming)
	{
		SetStreamInfo(FString::Printf(TEXT("Already streaming at %.1f FPS"), 1.0f / GetStreamIntervalSeconds()));
		return;
	}

	bStreaming = true;
	InFlightFrames.Reset();
	FrameSendTimes.Reset();
	MinimumPreviewableFastStreamFrameId = 0;
	ResetFrameUploadSignature();
		SetWorkflowState(
			EWorkflowState::Streaming,
		bLocalPreviewOnly
			? TEXT("Local preview")
			: TEXT("Connected"),
		bLocalPreviewOnly
			? HasPromptText()
				? TEXT("Rendering Unreal frames locally until the stream server is ready.")
				: TEXT("Rendering Unreal frames locally until a prompt is entered.")
			: TEXT("Live stream is running."));
	if (bLocalPreviewOnly)
	{
		SetStreamInfo(FString::Printf(TEXT("Local preview at %.1f FPS"), 1.0f / GetStreamIntervalSeconds()));
	}
	else
	{
		SetStreamInfo(FString::Printf(TEXT("Streaming at %.1f FPS"), 1.0f / GetStreamIntervalSeconds()));
	}
	RegisteredStreamIntervalSeconds = GetStreamIntervalSeconds();
	bRegisteredLocalPreviewOnly = bLocalPreviewOnly;
	if (StreamTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(StreamTickerHandle);
		StreamTickerHandle.Reset();
	}
	StreamTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateSP(this, &SUbstreamControlPanel::HandleStreamTicker),
		RegisteredStreamIntervalSeconds);

	// Keep the editor loop awake for the whole streaming session. Without this a
	// non-realtime stage viewport lets the editor sleep the moment the camera
	// stops, which strands the stream ticker and the WebSocket response dispatch
	// and freezes the preview on the last in-motion frame.
	if (const TSharedPtr<FActiveTimerHandle> KeepAlive = StreamKeepAliveTimerHandle.Pin())
	{
		UnRegisterActiveTimer(KeepAlive.ToSharedRef());
		StreamKeepAliveTimerHandle.Reset();
	}
	StreamKeepAliveTimerHandle = RegisterActiveTimer(
		1.0f / 30.0f,
		FWidgetActiveTimerDelegate::CreateSP(this, &SUbstreamControlPanel::HandleStreamKeepAliveTick));

	if (!CaptureAndSendFrame(!bLocalPreviewOnly, /*bForceSend*/ true))
	{
		SetWorkflowState(
			EWorkflowState::Warning,
			TEXT("Stream did not start"),
			bLocalPreviewOnly
				? TEXT("Initial local preview failed. Check the active viewport.")
				: TEXT("Initial capture failed. Check the active viewport and selected model."));
		StopStreaming(TEXT("Stream stopped: initial capture failed"));
	}
}

void SUbstreamControlPanel::StopStreaming(const FString& StatusMessage)
{
	bStreaming = false;
	InFlightFrames.Reset();
	FrameSendTimes.Reset();
	CancelPendingCapture();

	if (StreamTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(StreamTickerHandle);
		StreamTickerHandle.Reset();
	}

	if (const TSharedPtr<FActiveTimerHandle> KeepAlive = StreamKeepAliveTimerHandle.Pin())
	{
		UnRegisterActiveTimer(KeepAlive.ToSharedRef());
		StreamKeepAliveTimerHandle.Reset();
	}

	if (IsModelingModeActive())
	{
		SetWorkflowState(
			WebSocketClient.IsValid() && WebSocketClient->IsConnected() ? EWorkflowState::Ready : EWorkflowState::Disconnected,
			WebSocketClient.IsValid() && WebSocketClient->IsConnected() ? TEXT("Stream paused") : TEXT("Disconnected"),
			StatusMessage);
	}
	else if (HasSoloMeshLoaded())
	{
		SetWorkflowState(
			WebSocketClient.IsValid() && WebSocketClient->IsConnected() ? EWorkflowState::Ready : EWorkflowState::Disconnected,
			WebSocketClient.IsValid() && WebSocketClient->IsConnected() ? TEXT("Solo stream paused") : TEXT("Disconnected"),
			StatusMessage);
	}
	else
	{
		SetWorkflowState(EWorkflowState::Inactive, TEXT("Open a mesh"), StatusMessage);
	}

	SetStreamInfo(StatusMessage);
}

bool SUbstreamControlPanel::HandleStreamTicker(float DeltaTime)
{
	if (!bStreaming)
	{
		StreamTickerHandle.Reset();
		return false;
	}

	// Local passthrough runs faster; remote streaming polls at the configured rate but only
	// captures/uploads when the camera, model, or scene state is dirty.
	// Re-register when the mode or configured cadence changes.
	if (!FMath::IsNearlyEqual(RegisteredStreamIntervalSeconds, GetStreamIntervalSeconds(), 0.001f))
	{
		RegisteredStreamIntervalSeconds = GetStreamIntervalSeconds();
		const bool bNowLocalPreviewOnly = IsLocalPassthroughPreviewMode();
		if (bRegisteredLocalPreviewOnly != bNowLocalPreviewOnly)
		{
			ResetFrameUploadSignature();
			InFlightFrames.Reset();
			FrameSendTimes.Reset();
		}
		bRegisteredLocalPreviewOnly = bNowLocalPreviewOnly;
		StreamTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateSP(this, &SUbstreamControlPanel::HandleStreamTicker),
			RegisteredStreamIntervalSeconds);
		return false;
	}

	if (!IsCaptureWorkspaceReady())
	{
		StopStreaming(TEXT("Stream stopped: no capture workspace is active"));
		SetWorkflowState(EWorkflowState::Inactive, TEXT("Open a mesh"), TEXT("Streaming stopped because no capture workspace is active."));
		SetStreamInfo(TEXT("Stream stopped: no capture workspace is active"));
		return false;
	}

	const bool bLocalPreviewOnly = IsLocalPassthroughPreviewMode();

	if (bLocalPreviewOnly)
	{
		if (CaptureAndSendFrame(/*bTrackInFlight*/ false, /*bForceSend*/ true))
		{
			SetStreamInfo(HasPromptText()
				? FString::Printf(TEXT("Local passthrough at %.1f FPS while the stream server is unavailable"), 1.0f / GetStreamIntervalSeconds())
				: FString::Printf(TEXT("Local preview at %.1f FPS"), 1.0f / GetStreamIntervalSeconds()));
		}
		return true;
	}

	if (!bLocalPreviewOnly && !WebSocketClient->IsConnected())
	{
		StopStreaming(TEXT("Stream stopped: socket disconnected"));
		SetWorkflowState(EWorkflowState::Disconnected, TEXT("Disconnected"), TEXT("Streaming stopped because the WebSocket disconnected."));
		SetStreamInfo(TEXT("Stream stopped: socket disconnected"));
		return false;
	}

	if (!bLocalPreviewOnly && IsFastStreamProtocol() && !bFastStreamReady)
	{
		SetWorkflowState(EWorkflowState::Loading, TEXT("Waiting for server"), TEXT("The stream is paused until the backend reports Ready."));
		SetStreamInfo(TEXT("Streaming paused: waiting for FastStream Ready"));
		return true;
	}

	const bool bStreamInputDirty =
		CaptureState.IsDirty()
		|| HasSelectedModelingSceneSignatureChanged();
	if (!bStreamInputDirty)
	{
		SetStreamInfo(FString::Printf(
			TEXT("Streaming idle at %.1f FPS; last sent frame=%llu, in-flight=%d, last ack=%llu"),
			1.0f / GetStreamIntervalSeconds(),
			LastCapturedFrame.FrameId,
			InFlightFrames.Num(),
			LastAckedFrameId));
		return true;
	}

	// The FastStream backend is latest-wins: when frames arrive faster than
	// inference, it discards every queued frame except the newest and only
	// responds to frames it actually processes (see ubstream
	// ConnectionManager::getLatestFrame). Dropped frames are never acked, so
	// gating the next send on an in-flight ack stalls the stream indefinitely.
	// We therefore stream the freshest frame every tick without waiting and let
	// the server drop what it cannot keep up with - exactly what the reference
	// web client does.
	if (StreamLatestFrame(/*bTrackInFlight*/ true))
	{
		SetStreamInfo(FString::Printf(
			TEXT("Streaming at %.1f FPS, sent frame=%llu, last ack=%llu"),
			1.0f / GetStreamIntervalSeconds(),
			LastCapturedFrame.FrameId,
			LastAckedFrameId));
	}
	return true;
}

EActiveTimerReturnType SUbstreamControlPanel::HandleStreamKeepAliveTick(double InCurrentTime, float InDeltaTime)
{
	if (!bStreaming)
	{
		StreamKeepAliveTimerHandle.Reset();
		return EActiveTimerReturnType::Stop;
	}

	// Returning Continue keeps Slate (and therefore the editor main loop) awake,
	// so the stream ticker keeps firing and queued WebSocket frame responses keep
	// being dispatched to the game thread even while the user is idle. Also drive
	// the latest-frame sender here so the resting frame goes out the moment the
	// camera stops rather than waiting on the next poll tick. It only sends dirty
	// state and is cadence-limited, so this neither floods the server nor
	// double-sends with the ticker.
	MaybeStreamLatestFastStreamFrame();
	return EActiveTimerReturnType::Continue;
}

bool SUbstreamControlPanel::SendHelloMessage()
{
	if (IsFastStreamProtocol())
	{
		return SendFastStreamParamsMessage();
	}

	return WebSocketClient->SendJson(BuildHelloJson());
}

FString SUbstreamControlPanel::ResolveAuthToken() const
{
	return FUbstreamAuthService::Get().GetAuthKey();
}

bool SUbstreamControlPanel::SendAuthMessage()
{
	if (IsFastStreamProtocol())
	{
		return SendFastStreamAuthMessage();
	}

	if (ResolveAuthToken().IsEmpty())
	{
		return true;
	}

	return WebSocketClient->SendJson(BuildAuthJson());
}

bool SUbstreamControlPanel::SendSelectionStateMessage()
{
	if (!IsCaptureWorkspaceReady())
	{
		SetSelectionInfo(TEXT("Load an isolated subject or enter Modeling Mode to inspect the capture source"));
		return false;
	}

	if (!IsModelingModeActive())
	{
		const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
		if (!Viewport.IsValid() || !Viewport->HasSoloMesh())
		{
			return false;
		}

			SetSelectionInfo(FString::Printf(TEXT("Isolated subject: %s\nSource: %s"), *Viewport->GetSoloMeshLabel(), *Viewport->GetSoloMeshPath()));

			if (IsFastStreamProtocol())
			{
				SetStatus(TEXT("Isolated subject state is local-only for FastStream; frames carry the visual stream"));
				return true;
			}

		if (!WebSocketClient->IsConnected())
		{
			return false;
		}

		return WebSocketClient->SendJson(Viewport->BuildSceneStateJson(GetSessionId()));
	}

	const TArray<FUbstreamSelectedModelingObject> Objects = FUbstreamModelingModeBridge::GetSelectedModelingActors();
	SetSelectionInfo(FUbstreamModelingModeBridge::BuildSelectionSummary(Objects));

	if (IsFastStreamProtocol())
	{
		SetStatus(TEXT("Selection state is local-only for FastStream; frames carry the visual stream"));
		return true;
	}

	if (!WebSocketClient->IsConnected())
	{
		return false;
	}

	return WebSocketClient->SendJson(FUbstreamModelingModeBridge::BuildSelectedModelingStateJson(GetSessionId()));
}

bool SUbstreamControlPanel::SendFastStreamAuthMessage()
{
	if (!WebSocketClient.IsValid() || !WebSocketClient->IsConnected())
	{
		return false;
	}

	const FString AuthToken = ResolveAuthToken();
	if (AuthToken.IsEmpty())
	{
		return true;
	}

	TArray<uint8> Message;
	Message.Reserve(1 + AuthToken.Len());
	Message.Add(static_cast<uint8>(EFastStreamClientMessage::Auth));
	AppendUtf8(Message, AuthToken);
	return WebSocketClient->SendBinary(Message);
}

bool SUbstreamControlPanel::SendFastStreamParamsMessage()
{
	if (!WebSocketClient.IsValid() || !WebSocketClient->IsConnected())
	{
		return false;
	}

	TArray<uint8> Message;
	const FString ParamsJson = BuildFastStreamParamsJson();
	Message.Reserve(1 + ParamsJson.Len());
	Message.Add(static_cast<uint8>(EFastStreamClientMessage::Params));
	AppendUtf8(Message, ParamsJson);
	return WebSocketClient->SendBinary(Message);
}

bool SUbstreamControlPanel::SendFastStreamFrameMessage(bool bTrackInFlight)
{
	if (!WebSocketClient.IsValid() || !WebSocketClient->IsConnected())
	{
		SetStatus(TEXT("Failed to send FastStream frame: socket is disconnected"));
		return false;
	}

	if (!bFastStreamReady)
	{
		SetStatus(TEXT("Skipped FastStream frame: waiting for server Ready status"));
		SetStreamInfo(TEXT("Stream waiting for FastStream Ready"));
		return false;
	}

	if (LastCapturedFrame.ColorJpegBytes.IsEmpty())
	{
		SetStatus(TEXT("Skipped FastStream frame: color JPEG payload is empty"));
		return false;
	}

	const uint32 FrameId = static_cast<uint32>(LastCapturedFrame.FrameId & 0xffffffffu);
	const bool bSendDepth = bFastStreamDepthSupported && !LastCapturedFrame.DepthJpegBytes.IsEmpty();
	const int32 ColorBytes = LastCapturedFrame.ColorJpegBytes.Num();
	const int32 DepthBytes = bSendDepth ? LastCapturedFrame.DepthJpegBytes.Num() : 0;
	const int32 TotalBytes = 13 + ColorBytes + DepthBytes;
	const int32 MaxFastStreamBytes = FMath::Min(GetMaxFrameBundleBytes(), 10 * 1024 * 1024);
	if (TotalBytes > MaxFastStreamBytes)
	{
		SetStatus(FString::Printf(
			TEXT("Skipped FastStream frame %u: %d bytes exceeds max %d bytes"),
			FrameId,
			TotalBytes,
			MaxFastStreamBytes));
		return false;
	}

	TArray<uint8> Message;
	Message.Reserve(TotalBytes);
	Message.Add(static_cast<uint8>(EFastStreamClientMessage::Frame));
	AppendUint32LE(Message, FrameId);
	AppendUint32LE(Message, static_cast<uint32>(ColorBytes));
	AppendUint32LE(Message, static_cast<uint32>(DepthBytes));
	Message.Append(LastCapturedFrame.ColorJpegBytes);
	if (DepthBytes > 0)
	{
		Message.Append(LastCapturedFrame.DepthJpegBytes);
	}

	if (!WebSocketClient->SendBinary(Message))
	{
		SetStatus(TEXT("Failed to send FastStream frame"));
		return false;
	}
	UpdateDebugSentFramePreview(static_cast<uint64>(FrameId));

	ScheduleFastStreamFlush(FrameId);

	if (bTrackInFlight)
	{
		InFlightFrames.Add(static_cast<uint64>(FrameId));
		FrameSendTimes.Add(static_cast<uint64>(FrameId), FPlatformTime::Seconds());
		while (FrameSendTimes.Num() > MaxTrackedFastStreamFrames || InFlightFrames.Num() > MaxTrackedFastStreamFrames)
		{
			uint64 OldestFrameId = MAX_uint64;
			for (const TPair<uint64, double>& Pair : FrameSendTimes)
			{
				OldestFrameId = FMath::Min(OldestFrameId, Pair.Key);
			}
			for (const uint64 InFlightFrameId : InFlightFrames)
			{
				OldestFrameId = FMath::Min(OldestFrameId, InFlightFrameId);
			}
			if (OldestFrameId == MAX_uint64)
			{
				break;
			}
			FrameSendTimes.Remove(OldestFrameId);
			InFlightFrames.Remove(OldestFrameId);
		}
	}

	SetStatus(FString::Printf(
		TEXT("Sent FastStream frame %u (%d bytes, color=%d, depth=%d, in-flight=%d)"),
		FrameId,
		TotalBytes,
		ColorBytes,
		DepthBytes,
		InFlightFrames.Num()));
	return true;
}

void SUbstreamControlPanel::QueueFastStreamAutoRun(const FString& StatusMessage)
{
	if (!IsFastStreamProtocol())
	{
		return;
	}

	bFastStreamParamsPending = true;
	bFastStreamStartWhenReady = !bUserPausedStream;

	if (!WebSocketClient.IsValid() || !WebSocketClient->IsConnected())
	{
		EnsureLocalPassthroughPreviewRunning();
		return;
	}

	if (!bFastStreamReady)
	{
		SetWorkflowState(
			EWorkflowState::Loading,
			TEXT("Waiting for server"),
			TEXT("Prompt changes are queued and will apply when the backend is ready."));
		SetStatus(StatusMessage);
		SetStreamInfo(TEXT("Stream queued; it will start automatically when FastStream is Ready"));
		EnsureLocalPassthroughPreviewRunning();
	}
}

bool SUbstreamControlPanel::KickFastStreamFrameIfReady()
{
	if (!IsFastStreamProtocol() || !WebSocketClient.IsValid() || bUserPausedStream)
	{
		return false;
	}

	if (!IsCaptureWorkspaceReady())
	{
		StopStreaming(TEXT("Stream stopped: no capture workspace is active"));
		return false;
	}

	if (!HasPromptText())
	{
		if (!bStreaming)
		{
			StartStreaming();
			return true;
		}

		return CaptureAndSendFrame(/*bTrackInFlight*/ false, /*bForceSend*/ true);
	}

	if (!WebSocketClient->IsConnected() || !bFastStreamReady)
	{
		EnsureLocalPassthroughPreviewRunning();
		return false;
	}

	if (!bStreaming)
	{
		StartStreaming();
		return true;
	}

	return true;
}

bool SUbstreamControlPanel::MaybeStreamLatestFastStreamFrame()
{
	// Low-latency companion to the stream ticker: pushes the freshest capture as
	// soon as dirty state appears, so the resting frame goes out the moment the
	// camera stops instead of waiting for the next poll tick. The keep-alive
	// timer and each incoming frame response both call this. The FastStream
	// backend is latest-wins (it drops all but the newest queued frame and never
	// acks the dropped ones), so we deliberately do NOT gate on an in-flight ack
	// - doing so stalls the stream forever. The send rate is bounded only by the
	// stream cadence below.
	if (!bStreaming
		|| bUserPausedStream
		|| !IsFastStreamProtocol()
		|| IsLocalPassthroughPreviewMode()
		|| !WebSocketClient.IsValid()
		|| !WebSocketClient->IsConnected()
		|| !bFastStreamReady
		|| !IsCaptureWorkspaceReady())
	{
		return false;
	}

	const bool bStreamInputDirty =
		CaptureState.IsDirty()
		|| HasSelectedModelingSceneSignatureChanged();
	if (!bStreamInputDirty)
	{
		return false;
	}

	// Bound the upload rate to the stream cadence. The keep-alive timer ticks
	// faster than the target FPS and the ticker also fires, so without this the
	// three drivers would oversend; the cadence gate keeps the effective rate at
	// the configured stream FPS while still letting whichever driver fires first
	// send immediately.
	float CadenceWaitSeconds = 0.0f;
	if (ShouldDelayFrameUploadForCadence(/*bForceSend*/ false, CadenceWaitSeconds))
	{
		return false;
	}

	return StreamLatestFrame(/*bTrackInFlight*/ true);
}

bool SUbstreamControlPanel::SendFastStreamFlush(uint32 FrameId)
{
	if (!WebSocketClient.IsValid() || !WebSocketClient->IsConnected() || !IsFastStreamProtocol())
	{
		return false;
	}

	TArray<uint8> Message;
	Message.Reserve(5);
	Message.Add(static_cast<uint8>(EFastStreamClientMessage::Flush));
	AppendUint32LE(Message, FrameId);
	return WebSocketClient->SendBinary(Message);
}

void SUbstreamControlPanel::ScheduleFastStreamFlush(uint32 FrameId)
{
	PendingFastStreamFlushFrameId = FrameId;

	if (TSharedPtr<FActiveTimerHandle> TimerHandle = FastStreamFlushTimerHandle.Pin())
	{
		UnRegisterActiveTimer(TimerHandle.ToSharedRef());
		FastStreamFlushTimerHandle.Reset();
	}

	FastStreamFlushTimerHandle = RegisterActiveTimer(
		FastStreamFlushDebounceSeconds,
		FWidgetActiveTimerDelegate::CreateSP(this, &SUbstreamControlPanel::HandleFastStreamFlushTick));
}

void SUbstreamControlPanel::CancelFastStreamFlushTimer()
{
	PendingFastStreamFlushFrameId = 0;

	if (TSharedPtr<FActiveTimerHandle> TimerHandle = FastStreamFlushTimerHandle.Pin())
	{
		UnRegisterActiveTimer(TimerHandle.ToSharedRef());
		FastStreamFlushTimerHandle.Reset();
	}
}

EActiveTimerReturnType SUbstreamControlPanel::HandleFastStreamFlushTick(double InCurrentTime, float InDeltaTime)
{
	FastStreamFlushTimerHandle.Reset();

	const uint32 FrameId = PendingFastStreamFlushFrameId;
	PendingFastStreamFlushFrameId = 0;
	if (FrameId == 0 || !IsFastStreamProtocol() || !bFastStreamReady)
	{
		return EActiveTimerReturnType::Stop;
	}

	if (SendFastStreamFlush(FrameId))
	{
		SetStreamInfo(FString::Printf(TEXT("Sent idle FastStream flush for frame %u"), FrameId));
	}

	return EActiveTimerReturnType::Stop;
}

FString SUbstreamControlPanel::BuildHelloJson() const
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("type"), TEXT("hello"));
	Root->SetStringField(TEXT("protocol"), UbstreamProtocol::Name);
	Root->SetStringField(TEXT("client"), TEXT("ue-editor-plugin"));
	Root->SetStringField(TEXT("pluginVersion"), TEXT("0.1.0"));

	const TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
	Capabilities->SetBoolField(TEXT("binaryFrameBundles"), true);
	Capabilities->SetBoolField(TEXT("modelingSelectionState"), true);
	Capabilities->SetBoolField(TEXT("generateRequest"), true);
	Capabilities->SetBoolField(TEXT("lowFpsStreaming"), true);
	Capabilities->SetBoolField(TEXT("returnedFramePassthrough"), true);
	Capabilities->SetNumberField(TEXT("maxFrameBundleBytes"), GetMaxFrameBundleBytes());

	TArray<TSharedPtr<FJsonValue>> RgbFormats;
	RgbFormats.Add(MakeShared<FJsonValueString>(TEXT("jpeg")));
	Capabilities->SetArrayField(TEXT("rgb"), RgbFormats);

	TArray<TSharedPtr<FJsonValue>> DepthFormats;
	DepthFormats.Add(MakeShared<FJsonValueString>(TEXT("gray_jpeg")));
	Capabilities->SetArrayField(TEXT("depth"), DepthFormats);
	Root->SetObjectField(TEXT("capabilities"), Capabilities);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	return Json;
}

FString SUbstreamControlPanel::BuildAuthJson() const
{
	const UUbstreamModelerSettings* Settings = GetDefault<UUbstreamModelerSettings>();

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("type"), TEXT("auth"));
	Root->SetStringField(TEXT("protocol"), UbstreamProtocol::Name);
	Root->SetStringField(TEXT("sessionId"), GetSessionId());
	Root->SetStringField(TEXT("projectId"), Settings->ProjectId);
	Root->SetStringField(TEXT("token"), ResolveAuthToken());

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	return Json;
}

FString SUbstreamControlPanel::BuildGenerateRequestJson() const
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("type"), TEXT("generate_request"));
	Root->SetStringField(TEXT("protocol"), UbstreamProtocol::Name);
	Root->SetStringField(TEXT("sessionId"), GetSessionId());
	Root->SetStringField(TEXT("projectId"), GetDefault<UUbstreamModelerSettings>()->ProjectId);
	Root->SetNumberField(TEXT("frameId"), static_cast<double>(LastCapturedFrame.FrameId));
	Root->SetStringField(TEXT("prompt"), GetPromptText());
	Root->SetNumberField(TEXT("promptStrength"), GetPromptStrength());
	Root->SetNumberField(TEXT("depthStrength"), GetDepthStrength());
	Root->SetNumberField(TEXT("seed"), static_cast<double>(GenerationSeed));
	Root->SetNumberField(TEXT("slopiness"), GetPromptStrength());
	Root->SetStringField(TEXT("mode"), TEXT("single_frame"));

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	return Json;
}

FString SUbstreamControlPanel::GetPromptText() const
{
	return PromptTextBox.IsValid() ? PromptTextBox->GetText().ToString() : FString();
}

float SUbstreamControlPanel::GetPromptStrength() const
{
	return FMath::Clamp(PromptStrength, 0.0f, 1.0f);
}

float SUbstreamControlPanel::GetDepthStrength() const
{
	return FMath::Clamp(DepthStrength, 0.0f, 1.0f);
}

void SUbstreamControlPanel::HandlePromptStrengthChanged(float NewValue)
{
	PromptStrength = FMath::Clamp(NewValue, 0.0f, 1.0f);
	if (PromptStrengthReadout.IsValid())
	{
		PromptStrengthReadout->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(PromptStrength * 100.0f))));
	}
	MarkFrameUploadDirty();
	QueueFastStreamAutoRun(TEXT("Prompt controls queued; waiting for FastStream Ready"));
	SchedulePushParams(0.05f);
}

void SUbstreamControlPanel::HandleDepthStrengthChanged(float NewValue)
{
	DepthStrength = FMath::Clamp(NewValue, 0.0f, 1.0f);
	if (DepthStrengthReadout.IsValid())
	{
		DepthStrengthReadout->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(DepthStrength * 100.0f))));
	}
	MarkFrameUploadDirty();
	QueueFastStreamAutoRun(TEXT("Prompt controls queued; waiting for FastStream Ready"));
	SchedulePushParams(0.05f);
}

void SUbstreamControlPanel::HandlePromptChanged(const FText& NewText)
{
	MarkFrameUploadDirty();
	if (IsFastStreamProtocol() && HasPromptText() && WebSocketClient.IsValid() && !WebSocketClient->IsConnected())
	{
		bAutoConnectAttemptedForCurrentWorkspace = false;
		TryAutoConnectForWorkspace();
	}
	if (IsFastStreamProtocol())
	{
		EnsureLocalPassthroughPreviewRunning();
	}
	QueueFastStreamAutoRun(TEXT("Prompt queued; waiting for FastStream Ready"));
	SchedulePushParams(0.8f);
}

bool SUbstreamControlPanel::HasPromptText() const
{
	return GetPromptText().TrimStartAndEnd().Len() >= MinimumPromptCharactersForGeneration;
}

FString SUbstreamControlPanel::BuildParamsUpdateJson() const
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("type"), TEXT("params_update"));
	Root->SetStringField(TEXT("protocol"), UbstreamProtocol::Name);
	Root->SetStringField(TEXT("sessionId"), GetSessionId());
	Root->SetStringField(TEXT("prompt"), GetPromptText());
	Root->SetNumberField(TEXT("promptStrength"), GetPromptStrength());
	Root->SetNumberField(TEXT("depthStrength"), GetDepthStrength());
	Root->SetNumberField(TEXT("seed"), static_cast<double>(GenerationSeed));
	Root->SetNumberField(TEXT("slopiness"), GetPromptStrength());

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	return Json;
}

FString SUbstreamControlPanel::BuildFastStreamParamsJson() const
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("prompt"), GetPromptText());
	Root->SetNumberField(TEXT("strength"), GetPromptStrength());
	Root->SetNumberField(TEXT("depthStrength"), GetDepthStrength());
	Root->SetNumberField(TEXT("seed"), static_cast<double>(GenerationSeed));
	Root->SetBoolField(TEXT("passthrough"), !HasPromptText());

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	return Json;
}

bool SUbstreamControlPanel::SendParamsUpdateMessage()
{
	if (IsFastStreamProtocol() && !HasPromptText())
	{
		bFastStreamParamsPending = false;
		return true;
	}

	if (!WebSocketClient.IsValid() || !WebSocketClient->IsConnected())
	{
		return false;
	}

	if (IsFastStreamProtocol())
	{
		if (!bFastStreamReady)
		{
			bFastStreamParamsPending = true;
			bFastStreamStartWhenReady = !bUserPausedStream;
			SetWorkflowState(
				EWorkflowState::Loading,
				TEXT("Waiting for server"),
				TEXT("Prompt changes are queued and will apply when the backend is ready."));
			SetStatus(TEXT("Prompt queued; waiting for FastStream Ready"));
			SetStreamInfo(bUserPausedStream
				? TEXT("Stream paused; prompt changes will apply when you resume")
				: TEXT("Stream will start automatically when FastStream is Ready"));
			return true;
		}

		bFastStreamParamsPending = false;
		return SendFastStreamParamsMessage();
	}

	return WebSocketClient->SendJson(BuildParamsUpdateJson());
}

void SUbstreamControlPanel::SchedulePushParams(float DelaySeconds)
{
	if (ParamsPushTimerHandle.IsValid())
	{
		return;
	}
	ParamsPushTimerHandle = RegisterActiveTimer(
		FMath::Max(DelaySeconds, 0.01f),
		FWidgetActiveTimerDelegate::CreateSP(this, &SUbstreamControlPanel::HandleParamsPushTick));
}

EActiveTimerReturnType SUbstreamControlPanel::HandleParamsPushTick(double InCurrentTime, float InDeltaTime)
{
	ParamsPushTimerHandle.Reset();
	SendParamsUpdateMessage();
	if (IsFastStreamProtocol())
	{
		KickFastStreamFrameIfReady();
	}
	return EActiveTimerReturnType::Stop;
}

FString SUbstreamControlPanel::GetSessionId() const
{
	const FString SessionId = GetDefault<UUbstreamModelerSettings>()->SessionId;
	return SessionId.IsEmpty() ? FString(TEXT("editor_modeling_session")) : SessionId;
}

float SUbstreamControlPanel::GetStreamIntervalSeconds() const
{
	if (IsLocalPassthroughPreviewMode())
	{
		// Local passthrough costs no server work; run at interactive rate so
		// the preview tracks camera movement instead of feeling laggy.
		return 1.0f / 24.0f;
	}

	const float RequestedFps = GetDefault<UUbstreamModelerSettings>()->StreamFps;
	const float StreamFps = FMath::IsFinite(RequestedFps)
		? FMath::Clamp(RequestedFps, 0.2f, ServerStreamTargetFps)
		: ServerStreamTargetFps;
	return 1.0f / StreamFps;
}

int32 SUbstreamControlPanel::GetMaxInFlightFrames() const
{
	if (IsFastStreamProtocol())
	{
		return 1;
	}

	return FMath::Clamp(GetDefault<UUbstreamModelerSettings>()->MaxInFlightFrames, 1, 8);
}

int32 SUbstreamControlPanel::GetMaxFrameBundleBytes() const
{
	return FMath::Clamp(GetDefault<UUbstreamModelerSettings>()->MaxFrameBundleBytes, 1024 * 1024, 128 * 1024 * 1024);
}

FUbstreamCaptureSettings SUbstreamControlPanel::BuildCaptureSettings() const
{
	const UUbstreamModelerSettings* Settings = GetDefault<UUbstreamModelerSettings>();

	FUbstreamCaptureSettings CaptureSettings;
	CaptureSettings.Width = FMath::Clamp(IsFastStreamProtocol() ? FastStreamCaptureWidth : Settings->CaptureWidth, 64, 4096);
	CaptureSettings.Height = FMath::Clamp(IsFastStreamProtocol() ? FastStreamCaptureHeight : Settings->CaptureHeight, 64, 4096);
	CaptureSettings.JpegQuality = FMath::Clamp(Settings->JpegQuality, 1, 100);
	CaptureSettings.bCaptureDepth = !IsFastStreamProtocol() || bFastStreamDepthSupported;
	CaptureSettings.bSaveDebugImage = Settings->bSaveDebugCaptures;
	CaptureSettings.SessionId = GetSessionId();
	return CaptureSettings;
}

bool SUbstreamControlPanel::ShouldDelayFrameUploadForCadence(bool bForceSend, float& OutWaitSeconds) const
{
	OutWaitSeconds = 0.0f;
	if (bForceSend || IsLocalPassthroughPreviewMode() || LastFrameUploadTimeSeconds < 0.0)
	{
		return false;
	}

	const float StreamIntervalSeconds = GetStreamIntervalSeconds();
	if (StreamIntervalSeconds <= 0.0f)
	{
		return false;
	}

	const double ElapsedSeconds = FPlatformTime::Seconds() - LastFrameUploadTimeSeconds;
	if (ElapsedSeconds + KINDA_SMALL_NUMBER >= StreamIntervalSeconds)
	{
		return false;
	}

	OutWaitSeconds = static_cast<float>(StreamIntervalSeconds - FMath::Max(0.0, ElapsedSeconds));
	return true;
}

void SUbstreamControlPanel::RememberFrameUploadCadence()
{
	LastFrameUploadTimeSeconds = FPlatformTime::Seconds();
}

void SUbstreamControlPanel::MarkFrameUploadDirty()
{
	CaptureState.MarkDirty();
	SkippedUnchangedFrameCount = 0;
}

void SUbstreamControlPanel::MarkCaptureDirty()
{
	CaptureState.MarkDirty();
}

void SUbstreamControlPanel::ResetFrameUploadSignature()
{
	SkippedUnchangedFrameCount = 0;
	LastFrameUploadTimeSeconds = -1.0;
	CaptureState.ResetUploads();
}

void SUbstreamControlPanel::CancelPendingCapture()
{
	CaptureState.Cancel();
	PendingFrameReadback.Reset();
	bPendingFrameTrackInFlight = false;
}

uint64 SUbstreamControlPanel::BuildSelectedModelingSceneSignature() const
{
	if (HasSoloMeshLoaded())
	{
		const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
		return Viewport.IsValid() ? Viewport->BuildSceneSignature() : 0;
	}

	if (!GEditor || !IsModelingModeActive())
	{
		return 0;
	}

	TArray<AActor*> Actors;
	TSet<AActor*> AddedActors;

	for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
	{
		AActor* Actor = Cast<AActor>(*It);
		if (IsValid(Actor) && !AddedActors.Contains(Actor))
		{
			Actors.Add(Actor);
			AddedActors.Add(Actor);
		}
	}

	for (FSelectionIterator It(GEditor->GetSelectedComponentIterator()); It; ++It)
	{
		const UActorComponent* Component = Cast<UActorComponent>(*It);
		AActor* Owner = Component ? Component->GetOwner() : nullptr;
		if (IsValid(Owner) && !AddedActors.Contains(Owner))
		{
			Actors.Add(Owner);
			AddedActors.Add(Owner);
		}
	}

	Algo::SortBy(Actors, [](const AActor* Actor)
	{
		return Actor ? Actor->GetPathName() : FString();
	});

	uint64 Hash = MixSceneSignature(0x9ae16a3b2f90404full, static_cast<uint64>(Actors.Num()));

	const FViewport* ActiveViewport = GEditor->GetActiveViewport();
	const FEditorViewportClient* ActiveViewportClient = nullptr;
	for (const FEditorViewportClient* ViewportClient : GEditor->GetAllViewportClients())
	{
		if (!ViewportClient)
		{
			continue;
		}

		if (!ActiveViewport || ViewportClient->Viewport == ActiveViewport)
		{
			ActiveViewportClient = ViewportClient;
			break;
		}
	}
	if (ActiveViewportClient)
	{
		Hash = MixSceneVector(Hash, ActiveViewportClient->GetViewLocation());
		Hash = MixSceneVector(Hash, ActiveViewportClient->GetViewRotation().Euler());
		Hash = MixSceneSignature(Hash, QuantizeSceneFloat(ActiveViewportClient->ViewFOV));
		Hash = MixSceneSignature(Hash, static_cast<uint64>(ActiveViewportClient->ViewportType));
	}

	for (const AActor* Actor : Actors)
	{
		if (!IsValid(Actor))
		{
			continue;
		}

		Hash = MixSceneObjectPath(Hash, Actor);
		Hash = MixSceneObjectPath(Hash, Actor->GetClass());
		Hash = MixSceneTransform(Hash, Actor->GetActorTransform());

		FVector BoundsOrigin = FVector::ZeroVector;
		FVector BoundsExtent = FVector::ZeroVector;
		Actor->GetActorBounds(false, BoundsOrigin, BoundsExtent);
		Hash = MixSceneVector(Hash, BoundsOrigin);
		Hash = MixSceneVector(Hash, BoundsExtent);

		TInlineComponentArray<UActorComponent*> Components(Actor);
		Algo::SortBy(Components, [](const UActorComponent* Component)
		{
			return Component ? Component->GetPathName() : FString();
		});

		Hash = MixSceneSignature(Hash, static_cast<uint64>(Components.Num()));
		for (const UActorComponent* Component : Components)
		{
			if (!IsValid(Component))
			{
				continue;
			}

			Hash = MixSceneObjectPath(Hash, Component);
			Hash = MixSceneObjectPath(Hash, Component->GetClass());

			if (const USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
			{
				Hash = MixSceneTransform(Hash, SceneComponent->GetComponentTransform());
			}

			if (const UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Component))
			{
				Hash = MixSceneVector(Hash, PrimitiveComponent->Bounds.Origin);
				Hash = MixSceneVector(Hash, PrimitiveComponent->Bounds.BoxExtent);
				Hash = MixSceneSignature(Hash, QuantizeSceneFloat(PrimitiveComponent->Bounds.SphereRadius));
			}

			if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
			{
				Hash = MixSceneObjectPath(Hash, StaticMeshComponent->GetStaticMesh());
			}

			if (const UMeshComponent* MeshComponent = Cast<UMeshComponent>(Component))
			{
				const int32 MaterialCount = MeshComponent->GetNumMaterials();
				Hash = MixSceneSignature(Hash, static_cast<uint64>(MaterialCount));
				for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
				{
					Hash = MixSceneObjectPath(Hash, MeshComponent->GetMaterial(MaterialIndex));
				}
			}
		}
	}

	return Hash;
}

bool SUbstreamControlPanel::HasSelectedModelingSceneSignatureChanged()
{
	return CaptureState.HasSceneChanged(BuildSelectedModelingSceneSignature());
}

bool SUbstreamControlPanel::ShouldSkipCapturedFrameUpload(bool bForceSend) const
{
	return !bForceSend && CaptureState.IsDuplicate(LastCapturedTicket, LastCapturedFrame.ColorJpegBytes.Num());
}

void SUbstreamControlPanel::RememberUploadedFrameSignature()
{
	CaptureState.RememberUploaded(LastCapturedTicket, LastCapturedFrame.ColorJpegBytes.Num());
	SkippedUnchangedFrameCount = 0;
}

void SUbstreamControlPanel::RegisterEditorChangeDelegates()
{
	if (EditorCameraMovedHandle.IsValid())
	{
		return;
	}

	EditorCameraMovedHandle = FEditorDelegates::OnEditorCameraMoved.AddLambda(
		[this](const FVector&, const FRotator&, ELevelViewportType, int32)
		{
			if (IsModelingModeActive())
			{
				MarkFrameUploadDirty();
			}
		});

	MapChangedHandle = FEditorDelegates::MapChange.AddLambda(
		[this](uint32)
		{
			ResetFrameUploadSignature();
		});

	if (GEditor)
	{
		ActorsMovedHandle = GEditor->OnActorsMoved().AddLambda(
			[this](TArray<AActor*>&)
			{
				if (IsModelingModeActive())
				{
					MarkFrameUploadDirty();
				}
			});
	}

	if (GEngine)
	{
		ActorMovingHandle = GEngine->OnActorMoving().AddLambda(
			[this](AActor*)
			{
				if (IsModelingModeActive())
				{
					MarkFrameUploadDirty();
				}
			});

		ActorMovedHandle = GEngine->OnActorMoved().AddLambda(
			[this](AActor*)
			{
				if (IsModelingModeActive())
				{
					MarkFrameUploadDirty();
				}
			});

		ComponentTransformChangedHandle = GEngine->OnComponentTransformChanged().AddLambda(
			[this](USceneComponent*, ETeleportType)
			{
				if (IsModelingModeActive())
				{
					MarkFrameUploadDirty();
				}
			});

		LevelActorAddedHandle = GEngine->OnLevelActorAdded().AddLambda(
			[this](AActor*)
			{
				if (IsModelingModeActive())
				{
					MarkFrameUploadDirty();
				}
			});

		LevelActorDeletedHandle = GEngine->OnLevelActorDeleted().AddLambda(
			[this](AActor*)
			{
				if (IsModelingModeActive())
				{
					MarkFrameUploadDirty();
				}
			});

		LevelActorListChangedHandle = GEngine->OnLevelActorListChanged().AddLambda(
			[this]()
			{
				if (IsModelingModeActive())
				{
					MarkFrameUploadDirty();
				}
			});
	}

	SelectionChangedHandle = USelection::SelectionChangedEvent.AddLambda(
		[this](UObject*)
		{
			if (IsModelingModeActive())
			{
				MarkFrameUploadDirty();
			}
		});

	ObjectModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda(
		[this](UObject*)
		{
			if (IsModelingModeActive())
			{
				MarkFrameUploadDirty();
			}
		});

	ObjectPropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
		[this](UObject*, FPropertyChangedEvent&)
		{
			if (IsModelingModeActive())
			{
				MarkFrameUploadDirty();
			}
		});
}

void SUbstreamControlPanel::UnregisterEditorChangeDelegates()
{
	if (EditorCameraMovedHandle.IsValid())
	{
		FEditorDelegates::OnEditorCameraMoved.Remove(EditorCameraMovedHandle);
		EditorCameraMovedHandle.Reset();
	}

	if (ActorsMovedHandle.IsValid() && GEditor)
	{
		GEditor->OnActorsMoved().Remove(ActorsMovedHandle);
		ActorsMovedHandle.Reset();
	}

	if (ActorMovingHandle.IsValid() && GEngine)
	{
		GEngine->OnActorMoving().Remove(ActorMovingHandle);
		ActorMovingHandle.Reset();
	}

	if (ActorMovedHandle.IsValid() && GEngine)
	{
		GEngine->OnActorMoved().Remove(ActorMovedHandle);
		ActorMovedHandle.Reset();
	}

	if (ComponentTransformChangedHandle.IsValid() && GEngine)
	{
		GEngine->OnComponentTransformChanged().Remove(ComponentTransformChangedHandle);
		ComponentTransformChangedHandle.Reset();
	}

	if (LevelActorAddedHandle.IsValid() && GEngine)
	{
		GEngine->OnLevelActorAdded().Remove(LevelActorAddedHandle);
		LevelActorAddedHandle.Reset();
	}

	if (LevelActorDeletedHandle.IsValid() && GEngine)
	{
		GEngine->OnLevelActorDeleted().Remove(LevelActorDeletedHandle);
		LevelActorDeletedHandle.Reset();
	}

	if (LevelActorListChangedHandle.IsValid() && GEngine)
	{
		GEngine->OnLevelActorListChanged().Remove(LevelActorListChangedHandle);
		LevelActorListChangedHandle.Reset();
	}

	if (MapChangedHandle.IsValid())
	{
		FEditorDelegates::MapChange.Remove(MapChangedHandle);
		MapChangedHandle.Reset();
	}

	if (SelectionChangedHandle.IsValid())
	{
		USelection::SelectionChangedEvent.Remove(SelectionChangedHandle);
		SelectionChangedHandle.Reset();
	}

	if (ObjectModifiedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectModified.Remove(ObjectModifiedHandle);
		ObjectModifiedHandle.Reset();
	}

	if (ObjectPropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(ObjectPropertyChangedHandle);
		ObjectPropertyChangedHandle.Reset();
	}
}

void SUbstreamControlPanel::HandleResultImportComplete(bool bSuccess, const FString& Message, const FString& AssetPath)
{
	SetStatus(Message);
	NotifyUser(FText::FromString(Message), !bSuccess);
	if (bSuccess)
	{
		LastImportedAssetPath = AssetPath;
		SetResultInfo(FString::Printf(TEXT("%s\nImported asset: %s"), *Message, *LastImportedAssetPath));
	}
	else
	{
		SetResultInfo(Message);
	}
}

void SUbstreamControlPanel::ClearPendingReturnedFrameState()
{
	bHasPendingReturnedFrame = false;
	PendingReturnedFramePayloadType.Reset();
	PendingReturnedFrameFormat.Reset();
	PendingReturnedFrameId = 0;
	PendingReturnedFrameBytes = 0;
}

bool SUbstreamControlPanel::SaveReturnedFrameBytes(const TArray<uint8>& Bytes, FString& OutPath, FString& OutErrorMessage) const
{
	if (Bytes.IsEmpty())
	{
		OutErrorMessage = TEXT("Returned frame payload is empty");
		return false;
	}

	if (bHasPendingReturnedFrame && PendingReturnedFrameBytes > 0 && Bytes.Num() != PendingReturnedFrameBytes)
	{
		OutErrorMessage = FString::Printf(
			TEXT("Returned frame size mismatch: expected %d bytes, got %d bytes"),
			PendingReturnedFrameBytes,
			Bytes.Num());
		return false;
	}

	const FString Extension = DetectReturnedImageExtension(Bytes, PendingReturnedFrameFormat);
	const FString PayloadType = PendingReturnedFramePayloadType.IsEmpty() ? FString(TEXT("binary")) : PendingReturnedFramePayloadType;
	const uint64 FrameId = PendingReturnedFrameId > 0 ? PendingReturnedFrameId : LastCapturedFrame.FrameId;
	const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UbstreamReturnedFrames"));
	IFileManager::Get().MakeDirectory(*OutputDir, true);

	OutPath = FPaths::Combine(OutputDir, FString::Printf(TEXT("returned_frame_%llu_%s.%s"), FrameId, *PayloadType, *Extension));
	if (!FFileHelper::SaveArrayToFile(Bytes, *OutPath))
	{
		OutErrorMessage = FString::Printf(TEXT("Failed to write %s"), *OutPath);
		return false;
	}

	return true;
}

bool SUbstreamControlPanel::UpdateReturnedFramePreviewFromBytes(const TArray<uint8>& Bytes, FString& OutErrorMessage)
{
	if (Bytes.IsEmpty())
	{
		OutErrorMessage = TEXT("Returned frame payload is empty");
		return false;
	}

	if (bHasPendingReturnedFrame && PendingReturnedFrameBytes > 0 && Bytes.Num() != PendingReturnedFrameBytes)
	{
		OutErrorMessage = FString::Printf(
			TEXT("Returned frame size mismatch: expected %d bytes, got %d bytes"),
			PendingReturnedFrameBytes,
			Bytes.Num());
		return false;
	}

	// Compare all encoded bytes, not a sampled image hash. Identical frames
	// can share an immutable brush, including when a snapshot still owns it.
	// Keep processing frame/source metadata even when the pixels are unchanged.
	TSharedPtr<FSlateDynamicImageBrush> NewBrush = ReturnedFrameBrush;
	if (!NewBrush.IsValid() || !bReturnedPreviewFromBytes || CurrentPreviewImageBytes != Bytes)
	{
		NewBrush = CreateBrushFromCompressedImage(Bytes, TEXT("ReturnedFrame"), OutErrorMessage);
		if (!NewBrush.IsValid())
		{
			return false;
		}
		ReturnedFrameBrush = NewBrush;
		CurrentPreviewImageBytes = Bytes;
		bReturnedPreviewFromBytes = true;
	}
	bHasReturnedFramePreview = true;
	const bool bServerFrame = !PendingReturnedFramePayloadType.Equals(TEXT("local_passthrough"), ESearchCase::IgnoreCase);
	bCurrentPreviewIsServerFrame = bServerFrame;
	if (bServerFrame)
	{
		const uint64 FrameId = PendingReturnedFrameId > 0 ? PendingReturnedFrameId : LastCapturedFrame.FrameId;
		UpdateDebugReceivedFramePreview(FrameId, NewBrush);
	}
	RefreshPreviewImage();
	return true;
}

void SUbstreamControlPanel::UpdateReturnedFramePreview(const FString& FilePath)
{
	if (!ReturnedFrameImage.IsValid())
	{
		return;
	}

	const FUbstreamCaptureSettings CaptureSettings = BuildCaptureSettings();
	const FVector2D ImageSize(
		static_cast<float>(FMath::Max(CaptureSettings.Width, 1)),
		static_cast<float>(FMath::Max(CaptureSettings.Height, 1)));

	bReturnedPreviewFromBytes = false;
	ReturnedFrameBrush = MakeShared<FSlateDynamicImageBrush>(FName(*FilePath), ImageSize);
	bHasReturnedFramePreview = true;
	RefreshPreviewImage();
}

void SUbstreamControlPanel::UpdateDebugSentFramePreview(uint64 FrameId)
{
	LastDebugSentFrameId = FrameId;
	if (DebugSentFrameText.IsValid())
	{
		DebugSentFrameText->SetText(FText::FromString(FString::Printf(TEXT("Sent %llu"), FrameId)));
	}

	if (!LogsSection.IsValid() || !LogsSection->IsExpanded() || LastCapturedFrame.ColorJpegBytes.IsEmpty())
	{
		return;
	}

	FString ErrorMessage;
	TSharedPtr<FSlateDynamicImageBrush> NewBrush = CreateBrushFromCompressedImage(
		LastCapturedFrame.ColorJpegBytes,
		TEXT("DebugSentFrame"),
		ErrorMessage);
	if (!NewBrush.IsValid())
	{
		return;
	}

	DebugSentFrameBrush = NewBrush;
	if (DebugSentFrameImage.IsValid())
	{
		DebugSentFrameImage->SetImage(DebugSentFrameBrush.Get());
		DebugSentFrameImage->Invalidate(EInvalidateWidgetReason::Paint);
	}
}

void SUbstreamControlPanel::UpdateDebugReceivedFramePreview(uint64 FrameId, const TSharedPtr<FSlateDynamicImageBrush>& Brush)
{
	LastDebugReceivedFrameId = FrameId;
	DebugReceivedFrameBrush = Brush;

	if (DebugReceivedFrameText.IsValid())
	{
		DebugReceivedFrameText->SetText(FText::FromString(FString::Printf(TEXT("Received %llu"), FrameId)));
	}

	if (DebugReceivedFrameImage.IsValid())
	{
		DebugReceivedFrameImage->SetImage(DebugReceivedFrameBrush.IsValid() ? DebugReceivedFrameBrush.Get() : nullptr);
		DebugReceivedFrameImage->Invalidate(EInvalidateWidgetReason::Paint);
	}
}

void SUbstreamControlPanel::ClearDebugFramePreviews()
{
	LastDebugSentFrameId = 0;
	LastDebugReceivedFrameId = 0;
	DebugSentFrameBrush.Reset();
	DebugReceivedFrameBrush.Reset();

	if (DebugSentFrameText.IsValid())
	{
		DebugSentFrameText->SetText(LOCTEXT("DebugSentFrameCleared", "Sent --"));
	}
	if (DebugReceivedFrameText.IsValid())
	{
		DebugReceivedFrameText->SetText(LOCTEXT("DebugReceivedFrameCleared", "Received --"));
	}
	if (DebugSentFrameImage.IsValid())
	{
		DebugSentFrameImage->SetImage(nullptr);
		DebugSentFrameImage->Invalidate(EInvalidateWidgetReason::Paint);
	}
	if (DebugReceivedFrameImage.IsValid())
	{
		DebugReceivedFrameImage->SetImage(nullptr);
		DebugReceivedFrameImage->Invalidate(EInvalidateWidgetReason::Paint);
	}
}

void SUbstreamControlPanel::RefreshPreviewImage()
{
	if (ReturnedFrameImage.IsValid())
	{
		if (Snapshots.IsValidIndex(ViewedSnapshotIndex) && Snapshots[ViewedSnapshotIndex].Brush.IsValid())
		{
			ReturnedFrameImage->SetImage(Snapshots[ViewedSnapshotIndex].Brush.Get());
		}
		else
		{
			ReturnedFrameImage->SetImage(ReturnedFrameBrush.IsValid() ? ReturnedFrameBrush.Get() : nullptr);
		}

		ReturnedFrameImage->Invalidate(EInvalidateWidgetReason::Paint);
		if (ReturnedFrameBox.IsValid())
		{
			ReturnedFrameBox->Invalidate(EInvalidateWidgetReason::Paint);
		}
		Invalidate(EInvalidateWidgetReason::Paint);
	}

	RefreshMeshGenSourcePreview();
}

bool SUbstreamControlPanel::IsViewingSnapshot() const
{
	return Snapshots.IsValidIndex(ViewedSnapshotIndex);
}

void SUbstreamControlPanel::SelectSnapshot(int32 SnapshotIndex)
{
	ViewedSnapshotIndex = Snapshots.IsValidIndex(SnapshotIndex) ? SnapshotIndex : INDEX_NONE;
	RefreshPreviewImage();

	if (IsViewingSnapshot())
	{
		SetStatus(FString::Printf(TEXT("Viewing snapshot %d of %d"), ViewedSnapshotIndex + 1, Snapshots.Num()));
	}

	Invalidate(EInvalidateWidgetReason::Paint);
}

void SUbstreamControlPanel::RebuildSnapshotStrip()
{
	if (!SnapshotStripBox.IsValid())
	{
		return;
	}

	SnapshotStripBox->ClearChildren();

	for (int32 SnapshotIndex = 0; SnapshotIndex < Snapshots.Num(); ++SnapshotIndex)
	{
		const FUbstreamFrameSnapshot& Snapshot = Snapshots[SnapshotIndex];
		const FText ToolTip = Snapshot.Prompt.IsEmpty()
			? FText::Format(LOCTEXT("SnapshotTooltipNoPrompt", "Frame {0} - seed {1}\nClick to view; click again for live."), Snapshot.FrameId, Snapshot.Seed)
			: FText::Format(LOCTEXT("SnapshotTooltipFormat", "{0}\nFrame {1} - seed {2}\nClick to view; click again for live."), FText::FromString(Snapshot.Prompt), Snapshot.FrameId, Snapshot.Seed);

		SnapshotStripBox->AddSlot()
		.AutoWidth()
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this, SnapshotIndex]()
			{
				return SnapshotIndex == ViewedSnapshotIndex
					? FSlateColor(FUbstreamModelerStyle::StatusStreaming())
					: FSlateColor(FLinearColor(0.16f, 0.16f, 0.16f, 1.0f));
			})
			.Padding(2.0f)
			[
				SNew(SButton)
				.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
				.ContentPadding(0.0f)
				.ToolTipText(ToolTip)
				.OnClicked_Lambda([this, SnapshotIndex]()
				{
					SelectSnapshot(SnapshotIndex == ViewedSnapshotIndex ? INDEX_NONE : SnapshotIndex);
					return FReply::Handled();
				})
				[
					SNew(SBox)
					.WidthOverride(SnapshotThumbnailSize)
					.HeightOverride(SnapshotThumbnailSize)
					[
						SNew(SScaleBox)
						.Stretch(EStretch::ScaleToFit)
						[
							SNew(SImage)
							.Image(Snapshot.Brush.Get())
						]
					]
				]
			]
		];
	}

	Invalidate(EInvalidateWidgetReason::LayoutAndVolatility);
}

void SUbstreamControlPanel::SetWorkflowState(EWorkflowState NewState, const FString& Title, const FString& Detail)
{
	WorkflowState = NewState;

	if (WorkflowStateTitleText.IsValid())
	{
		WorkflowStateTitleText->SetText(FText::FromString(Title));
	}

	if (ModeGateStatusText.IsValid() && !Detail.IsEmpty())
	{
		ModeGateStatusText->SetText(FText::FromString(Detail));
	}
}

void SUbstreamControlPanel::SetStatus(const FString& Message)
{
	if (ModeGateStatusText.IsValid())
	{
		ModeGateStatusText->SetText(FText::FromString(Message));
	}

	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(Message));
	}
}

void SUbstreamControlPanel::SetSelectionInfo(const FString& Message)
{
	if (SelectionInfoText.IsValid())
	{
		SelectionInfoText->SetText(FText::FromString(Message));
	}
}

void SUbstreamControlPanel::SetStreamInfo(const FString& Message)
{
	if (StreamInfoText.IsValid())
	{
		StreamInfoText->SetText(FText::FromString(Message));
	}
}

void SUbstreamControlPanel::SetResultInfo(const FString& Message)
{
	if (ResultInfoText.IsValid())
	{
		ResultInfoText->SetText(FText::FromString(Message));
	}
}

void SUbstreamControlPanel::NotifyUser(const FText& Message, bool bIsError) const
{
	FNotificationInfo Info(Message);
	Info.bFireAndForget = true;
	Info.ExpireDuration = bIsError ? 7.0f : 4.0f;
	Info.Image = FUbstreamModelerStyle::Get().GetBrush(TEXT("UbstreamModeler.Icon20"));

	TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification.IsValid())
	{
		Notification->SetCompletionState(bIsError ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
	}
}

FString SUbstreamControlPanel::GetEndpointUrl() const
{
	return UrlTextBox.IsValid() ? UrlTextBox->GetText().ToString() : GetDefault<UUbstreamModelerSettings>()->DefaultWebSocketUrl;
}

#undef LOCTEXT_NAMESPACE
