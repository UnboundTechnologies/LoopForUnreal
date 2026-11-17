#include "Widgets/SUbstreamMeshGenPanel.h"
#include "Auth/UbstreamAuthService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Containers/Ticker.h"
#include "Widgets/Layout/SWrapBox.h"
#include "HAL/PlatformMisc.h"
#include "IContentBrowserSingleton.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "MeshGen/UbstreamMeshGenError.h"
#include "MeshGen/UbstreamMeshGenService.h"
#include "Settings/UbstreamModelerSettings.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/UbstreamModelerStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SUbstreamModelerViewport.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Notifications/SProgressBar.h"

#define LOCTEXT_NAMESPACE "SUbstreamMeshGenPanel"

namespace
{

	FString DetectImageContentType(const FString& FileName, const TArray<uint8>& Bytes)
	{
		const FString Ext = FPaths::GetExtension(FileName).ToLower();
		if (Ext == TEXT("png"))
		{
			return TEXT("image/png");
		}
		if (Ext == TEXT("webp"))
		{
			return TEXT("image/webp");
		}
		if (Ext == TEXT("jpg") || Ext == TEXT("jpeg"))
		{
			return TEXT("image/jpeg");
		}

		if (Bytes.Num() >= 8
			&& Bytes[0] == 0x89 && Bytes[1] == 0x50 && Bytes[2] == 0x4e && Bytes[3] == 0x47)
		{
			return TEXT("image/png");
		}
		if (Bytes.Num() >= 3 && Bytes[0] == 0xff && Bytes[1] == 0xd8 && Bytes[2] == 0xff)
		{
			return TEXT("image/jpeg");
		}

		return TEXT("image/jpeg");
	}
}

void SUbstreamMeshGenPanel::Construct(const FArguments& InArgs)
{
	SoloViewport = InArgs._SoloViewport;
	OnGetSourceImageBytes = InArgs._OnGetSourceImageBytes;
	OnHasSourceImage = InArgs._OnHasSourceImage;
	OnGetSourceImageLabel = InArgs._OnGetSourceImageLabel;
	OnGetBlockedReason = InArgs._OnGetBlockedReason;

	MeshGenService = MakeShared<FUbstreamMeshGenService>();
	FUbstreamAuthService::Get().OnSigningOut.AddSPLambda(this, [this]
	{
		if (MeshGenService.IsValid() && MeshGenService->IsBusy()) MeshGenService->Cancel();
	});
	FUbstreamAuthService::Get().OnChanged.AddSPLambda(this, [this]
	{
		if (!FUbstreamAuthService::Get().IsAuthenticated() && MeshGenService.IsValid() && MeshGenService->IsBusy()) MeshGenService->Cancel();
	});
	MeshGenService->OnProgress.AddSP(this, &SUbstreamMeshGenPanel::HandleProgress);
	MeshGenService->OnCompleted.AddSP(this, &SUbstreamMeshGenPanel::HandleCompleted);

	ChildSlot
	[
		BuildContent()
	];
}

SUbstreamMeshGenPanel::~SUbstreamMeshGenPanel()
{
	if (MeshGenService.IsValid())
	{
		MeshGenService->OnProgress.RemoveAll(this);
		MeshGenService->OnCompleted.RemoveAll(this);
		if (MeshGenService->IsBusy())
		{
			MeshGenService->Cancel();
		}
	}
}

TSharedRef<SWidget> SUbstreamMeshGenPanel::BuildContent()
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
				SNew(STextBlock)
				.Visibility(this, &SUbstreamMeshGenPanel::GetIdleHintVisibility)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StatusWarning()))
				.Text(this, &SUbstreamMeshGenPanel::GetIdleHintText)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f, 0.0f, 2.0f, FUbstreamModelerStyle::SmallGap)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("QuadsLabel", "Quads"))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SSpinBox<int32>)
					.IsEnabled_Lambda([this] { return !IsBusy(); })
					.MinValue(UbstreamMeshGenApi::MinQuads)
					.MaxValue(UbstreamMeshGenApi::MaxQuads)
					.MinSliderValue(UbstreamMeshGenApi::MinQuads)
					.MaxSliderValue(UbstreamMeshGenApi::MaxQuads)
					.Delta(500)
					.ToolTipText(LOCTEXT("QuadsTip", "Target quad count of the generated mesh. The result lands near the target."))
					.Value_Lambda([] { return UbstreamMeshGenApi::ClampQuads(GetDefault<UUbstreamModelerSettings>()->MeshGenQuads); })
					.OnValueChanged_Lambda([](int32 Quads)
					{
						GetMutableDefault<UUbstreamModelerSettings>()->MeshGenQuads = UbstreamMeshGenApi::ClampQuads(Quads);
					})
					// Save once per edit rather than on every drag step.
					.OnValueCommitted_Lambda([](int32 Quads, ETextCommit::Type)
					{
						UUbstreamModelerSettings* Settings = GetMutableDefault<UUbstreamModelerSettings>();
						Settings->MeshGenQuads = UbstreamMeshGenApi::ClampQuads(Quads);
						Settings->UpdateSinglePropertyInConfigFile(
							Settings->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UUbstreamModelerSettings, MeshGenQuads)),
							Settings->GetDefaultConfigFilename());
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsEnabled_Lambda([this] { return !IsBusy(); })
					.ToolTipText(LOCTEXT("NormalMapTip", "Include a normal map of the fine surface detail. Off makes the download about 1.4 MB smaller."))
					.IsChecked_Lambda([] { return GetDefault<UUbstreamModelerSettings>()->bMeshGenNormalMap ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([](ECheckBoxState State)
					{
						UUbstreamModelerSettings* Settings = GetMutableDefault<UUbstreamModelerSettings>();
						Settings->bMeshGenNormalMap = State == ECheckBoxState::Checked;
						Settings->UpdateSinglePropertyInConfigFile(
							Settings->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UUbstreamModelerSettings, bMeshGenNormalMap)),
							Settings->GetDefaultConfigFilename());
					})
					[
						SNew(STextBlock).Text(LOCTEXT("NormalMapLabel", "Normal map"))
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.PrimaryButton")
					.HAlign(HAlign_Center)
					.Text(this, &SUbstreamMeshGenPanel::GetGenerateButtonText)
					.ToolTipText(this, &SUbstreamMeshGenPanel::GetGenerateTooltipText)
					.IsEnabled(this, &SUbstreamMeshGenPanel::IsGenerateEnabled)
					.OnClicked(this, &SUbstreamMeshGenPanel::GenerateClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("CancelGen", "Cancel"))
					.Visibility_Lambda([this]()
					{
						return IsCancelEnabled() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.IsEnabled(this, &SUbstreamMeshGenPanel::IsCancelEnabled)
					.OnClicked(this, &SUbstreamMeshGenPanel::CancelClicked)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, FUbstreamModelerStyle::SmallGap, 0.0f, 0.0f)
			[
				SNew(SVerticalBox)
				.Visibility(this, &SUbstreamMeshGenPanel::GetProgressVisibility)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SAssignNew(ProgressBar, SProgressBar)
					.Percent(this, &SUbstreamMeshGenPanel::GetProgressFraction)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SAssignNew(StatusTextBlock, STextBlock)
					.Text(this, &SUbstreamMeshGenPanel::GetStatusText)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, FUbstreamModelerStyle::SmallGap, 0.0f, 0.0f)
			[
				BuildSuccessBanner()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, FUbstreamModelerStyle::SmallGap, 0.0f, 0.0f)
			[
				BuildErrorBanner()
			]
		];
}

TSharedRef<SWidget> SUbstreamMeshGenPanel::BuildErrorBanner()
{
	static const FSlateRoundedBoxBrush ErrorBannerBrush(
		FUbstreamModelerStyle::ErrorBannerBackground(),
		6.0f,
		FUbstreamModelerStyle::ErrorBannerBorder(),
		1.5f);

	return SNew(SBorder)
		.Visibility(this, &SUbstreamMeshGenPanel::GetErrorVisibility)
		.BorderImage(&ErrorBannerBrush)
		.Padding(FMargin(10.0f, 10.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Top)
				.Padding(0.0f, 1.0f, 8.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(16.0f)
					.HeightOverride(16.0f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.ErrorWithColor"))
						.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StatusError()))
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(this, &SUbstreamMeshGenPanel::GetErrorTitleText)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
						.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.82f, 0.82f, 1.0f)))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(this, &SUbstreamMeshGenPanel::GetErrorMessageText)
						.ColorAndOpacity(FSlateColor(FLinearColor(0.95f, 0.88f, 0.88f, 1.0f)))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Visibility(this, &SUbstreamMeshGenPanel::GetErrorHintVisibility)
						.Text(this, &SUbstreamMeshGenPanel::GetErrorHintText)
						.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.72f, 0.72f, 1.0f)))
						.AutoWrapText(true)
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("RetryGen", "Retry"))
					.ToolTipText(LOCTEXT("RetryGenTip", "Run generation again with the current preview/snapshot (new random seed)."))
					.IsEnabled_Lambda([this]() { return !IsBusy(); })
					.OnClicked(this, &SUbstreamMeshGenPanel::RetryClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("DismissError", "Dismiss"))
					.OnClicked(this, &SUbstreamMeshGenPanel::DismissResultClicked)
				]
			]
		];
}

TSharedRef<SWidget> SUbstreamMeshGenPanel::BuildSuccessBanner()
{
	static const FSlateRoundedBoxBrush SuccessBannerBrush(
		FUbstreamModelerStyle::SuccessBannerBackground(),
		6.0f,
		FUbstreamModelerStyle::SuccessBannerBorder(),
		1.5f);

	return SNew(SBorder)
		.Visibility(this, &SUbstreamMeshGenPanel::GetResultVisibility)
		.BorderImage(&SuccessBannerBrush)
		.Padding(FMargin(10.0f, 10.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Top)
				.Padding(0.0f, 1.0f, 8.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(16.0f)
					.HeightOverride(16.0f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.SuccessWithColor"))
						.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StatusReady()))
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("SuccessTitle", "Mesh saved to project"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.82f, 1.0f, 0.88f, 1.0f)))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(this, &SUbstreamMeshGenPanel::GetResultText)
						.ColorAndOpacity(FSlateColor(FLinearColor(0.86f, 0.95f, 0.88f, 1.0f)))
						.AutoWrapText(true)
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				// A wrap box, not an SHorizontalBox: four fixed-width buttons
				// overflow a narrow sidebar, and their unbreakable row width
				// stretched the whole card past the panel edge (clipping the
				// buttons AND the wrapped path text above).
				SNew(SWrapBox)
				.UseAllottedSize(true)
				+ SWrapBox::Slot()
				.Padding(0.0f, 0.0f, 6.0f, 4.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenPreview", "Open Preview"))
					.ToolTipText(LOCTEXT("OpenPreviewTip", "Open the Static Mesh Editor for this asset. Does not replace the CSG stage."))
					.OnClicked(this, &SUbstreamMeshGenPanel::OpenPreviewClicked)
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 0.0f, 6.0f, 4.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("ShowInCB", "Content Browser"))
					.ToolTipText(LOCTEXT("ShowInCBTip", "Focus the imported asset in the Content Browser / Content Drawer."))
					.OnClicked(this, &SUbstreamMeshGenPanel::ShowInContentBrowserClicked)
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 0.0f, 6.0f, 4.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("LoadStage", "Load in Stage"))
					.ToolTipText(LOCTEXT("LoadStageTip", "Optional: replace the Unbound CSG stage subject with this mesh."))
					.OnClicked(this, &SUbstreamMeshGenPanel::LoadInStageClicked)
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 0.0f, 0.0f, 4.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("DismissResult", "Dismiss"))
					.OnClicked(this, &SUbstreamMeshGenPanel::DismissResultClicked)
				]
			]
		];
}

void SUbstreamMeshGenPanel::RefreshSourcePreview()
{
	// Source thumbnail chrome removed — parent may still call this after captures.
}

bool SUbstreamMeshGenPanel::RequestGenerate()
{
	if (!FUbstreamAuthService::Get().IsAuthenticated()) return false;
	if (!MeshGenService.IsValid() || MeshGenService->IsBusy())
	{
		return false;
	}

	TArray<uint8> ImageBytes;
	FString FileName;
	FString ContentType;
	if (!ResolveSourceImage(ImageBytes, FileName, ContentType))
	{
		const FUbstreamMeshGenUserError Error = UbstreamMeshGenError::MakeNoSourceError();
		SetDisplayError(Error);
		NotifyUserError(Error);
		return false;
	}

	ClearDisplayError();
	bShowResult = false;
	LastResult = FUbstreamMeshGenResult();
	LastProgress = FUbstreamMeshGenProgress();

	const FUbstreamMeshGenRequest Request = BuildRequest(ImageBytes, FileName, ContentType);
	if (!MeshGenService->Start(Request))
	{
		const FUbstreamMeshGenUserError Error = UbstreamMeshGenError::MakeGeneric(
			TEXT("Could not start 3D generation (service busy or invalid request)."),
			EUbstreamMeshGenPhase::Idle);
		SetDisplayError(Error);
		NotifyUserError(Error);
		return false;
	}

	return true;
}

bool SUbstreamMeshGenPanel::CanRequestGenerate() const
{
	return IsGenerateEnabled();
}

bool SUbstreamMeshGenPanel::IsGenerationBusy() const
{
	return IsBusy();
}

FReply SUbstreamMeshGenPanel::GenerateClicked()
{
	RequestGenerate();
	return FReply::Handled();
}

FReply SUbstreamMeshGenPanel::CancelClicked()
{
	if (MeshGenService.IsValid())
	{
		MeshGenService->Cancel();
	}
	return FReply::Handled();
}

UStaticMesh* SUbstreamMeshGenPanel::ResolveResultMesh() const
{
	if (UStaticMesh* Mesh = LastResult.StaticMesh.Get())
	{
		return Mesh;
	}
	if (!LastResult.AssetPath.IsEmpty())
	{
		return LoadObject<UStaticMesh>(nullptr, *LastResult.AssetPath);
	}
	return nullptr;
}

FReply SUbstreamMeshGenPanel::ShowInContentBrowserClicked()
{
	UStaticMesh* Mesh = ResolveResultMesh();
	if (!Mesh)
	{
		return FReply::Handled();
	}

	TArray<UObject*> Assets;
	Assets.Add(Mesh);
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	ContentBrowserModule.Get().SyncBrowserToAssets(Assets);
	return FReply::Handled();
}

FReply SUbstreamMeshGenPanel::OpenPreviewClicked()
{
	UStaticMesh* Mesh = ResolveResultMesh();
	if (!Mesh)
	{
		NotifyUser(LOCTEXT("NoMeshToPreview", "No generated mesh is available to preview."), true);
		return FReply::Handled();
	}
	if (!OpenGeneratedMeshPreview(Mesh))
	{
		NotifyUser(LOCTEXT("OpenPreviewFailed", "Could not open the mesh preview editor."), true);
	}
	return FReply::Handled();
}

FReply SUbstreamMeshGenPanel::LoadInStageClicked()
{
	UStaticMesh* Mesh = ResolveResultMesh();
	if (!Mesh)
	{
		NotifyUser(LOCTEXT("NoMeshToLoad", "No generated mesh is available to load."), true);
		return FReply::Handled();
	}

	if (!LoadGeneratedMeshIntoStage(Mesh))
	{
		NotifyUser(LOCTEXT("LoadStageFailed", "Could not load the mesh into the stage."), true);
	}
	return FReply::Handled();
}

FReply SUbstreamMeshGenPanel::RetryClicked()
{
	return GenerateClicked();
}

FReply SUbstreamMeshGenPanel::DismissResultClicked()
{
	bShowResult = false;
	ClearDisplayError();
	if (!IsBusy())
	{
		LastProgress = FUbstreamMeshGenProgress();
	}
	return FReply::Handled();
}

bool SUbstreamMeshGenPanel::IsGenerateEnabled() const
{
	if (!FUbstreamAuthService::Get().IsAuthenticated()) return false;
	return !IsBusy() && HasSourceImage();
}

bool SUbstreamMeshGenPanel::IsCancelEnabled() const
{
	return IsBusy();
}

bool SUbstreamMeshGenPanel::IsBusy() const
{
	return MeshGenService.IsValid() && MeshGenService->IsBusy();
}

bool SUbstreamMeshGenPanel::HasSourceImage() const
{
	if (OnHasSourceImage.IsBound())
	{
		return OnHasSourceImage.Execute();
	}

	if (OnGetSourceImageBytes.IsBound())
	{
		return OnGetSourceImageBytes.Execute().Num() > 0;
	}

	return false;
}

bool SUbstreamMeshGenPanel::HasResultMesh() const
{
	return LastResult.bSuccess && (!LastResult.AssetPath.IsEmpty() || LastResult.StaticMesh.IsValid());
}

bool SUbstreamMeshGenPanel::HasDisplayError() const
{
	return DisplayError.IsSet();
}

EVisibility SUbstreamMeshGenPanel::GetProgressVisibility() const
{
	if (IsBusy())
	{
		return EVisibility::Visible;
	}
	if (LastProgress.Phase == EUbstreamMeshGenPhase::Uploading
		|| LastProgress.Phase == EUbstreamMeshGenPhase::Queued
		|| LastProgress.Phase == EUbstreamMeshGenPhase::MaskGeneration
		|| LastProgress.Phase == EUbstreamMeshGenPhase::Reconstruction
		|| LastProgress.Phase == EUbstreamMeshGenPhase::Exporting
		|| LastProgress.Phase == EUbstreamMeshGenPhase::Downloading
		|| LastProgress.Phase == EUbstreamMeshGenPhase::Importing)
	{
		return EVisibility::Visible;
	}
	return EVisibility::Collapsed;
}

EVisibility SUbstreamMeshGenPanel::GetResultVisibility() const
{
	return (bShowResult && HasResultMesh() && !IsBusy()) ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SUbstreamMeshGenPanel::GetErrorVisibility() const
{
	return (HasDisplayError() && !IsBusy()) ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SUbstreamMeshGenPanel::GetIdleHintVisibility() const
{
	return (!IsBusy() && !HasSourceImage() && !bShowResult && !HasDisplayError())
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

TOptional<float> SUbstreamMeshGenPanel::GetProgressFraction() const
{
	return LastProgress.Fraction;
}

FText SUbstreamMeshGenPanel::GetStatusText() const
{
	if (!LastProgress.StatusText.IsEmpty())
	{
		return FText::FromString(LastProgress.StatusText);
	}
	return LOCTEXT("IdleStatus", "Ready");
}

FText SUbstreamMeshGenPanel::GetIdleHintText() const
{
	if (OnGetBlockedReason.IsBound())
	{
		const FString Reason = OnGetBlockedReason.Execute();
		if (!Reason.IsEmpty())
		{
			return FText::FromString(Reason);
		}
	}
	return LOCTEXT(
		"MeshGenNeedSource",
		"Enter a prompt and wait for a server-generated frame (disabled during local passthrough).");
}

FText SUbstreamMeshGenPanel::GetGenerateTooltipText() const
{
	if (!IsGenerateEnabled())
	{
		if (OnGetBlockedReason.IsBound())
		{
			const FString Reason = OnGetBlockedReason.Execute();
			if (!Reason.IsEmpty())
			{
				return FText::FromString(Reason);
			}
		}
		return LOCTEXT(
			"GenerateBlockedTip",
			"Disabled until a prompt is set and a frame from the stream server is available.");
	}
	return LOCTEXT(
		"GenerateTip",
		"Generate a 3D mesh from the live preview or selected server snapshot. A random seed is chosen each run.");
}

FText SUbstreamMeshGenPanel::GetResultText() const
{
	if (LastResult.AssetPath.IsEmpty())
	{
		return LOCTEXT("ResultReady", "Mesh imported.");
	}
	return FText::Format(LOCTEXT("ResultPathFmt", "Imported {0}"), FText::FromString(LastResult.AssetPath));
}

FText SUbstreamMeshGenPanel::GetErrorTitleText() const
{
	return DisplayError.Title.IsEmpty()
		? LOCTEXT("DefaultErrorTitle", "3D generation failed")
		: DisplayError.Title;
}

FText SUbstreamMeshGenPanel::GetErrorMessageText() const
{
	return DisplayError.Message.IsEmpty()
		? LOCTEXT("DefaultErrorMessage", "Something went wrong.")
		: DisplayError.Message;
}

FText SUbstreamMeshGenPanel::GetErrorHintText() const
{
	return DisplayError.Hint;
}

EVisibility SUbstreamMeshGenPanel::GetErrorHintVisibility() const
{
	return DisplayError.Hint.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SUbstreamMeshGenPanel::GetGenerateButtonText() const
{
	if (IsBusy())
	{
		return LOCTEXT("GenerateBusy", "Generating…");
	}
	return LOCTEXT("Generate3D", "Generate 3D Mesh");
}

void SUbstreamMeshGenPanel::HandleProgress(const FUbstreamMeshGenProgress& InProgress)
{
	LastProgress = InProgress;
}

void SUbstreamMeshGenPanel::HandleCompleted(const FUbstreamMeshGenResult& Result)
{
	LastResult = Result;

	if (Result.bSuccess)
	{
		ClearDisplayError();
		bShowResult = true;
		LastProgress.Phase = EUbstreamMeshGenPhase::Completed;
		LastProgress.Fraction = 1.0f;
		LastProgress.StatusText = TEXT("Complete");

		if (UStaticMesh* Mesh = Result.StaticMesh.Get())
		{
			if (const UUbstreamModelerSettings* Settings = GetDefault<UUbstreamModelerSettings>())
			{
				if (Settings->bMeshGenOpenPreviewEditor)
				{
					OpenGeneratedMeshPreview(Mesh);
				}
				if (Settings->bMeshGenAutoLoadIntoStage)
				{
					LoadGeneratedMeshIntoStage(Mesh);
				}
			}
		}

		NotifyUser(
			FText::Format(
				LOCTEXT("GenSuccessToast", "3D mesh ready: {0}"),
				FText::FromString(Result.AssetPath.IsEmpty() ? TEXT("imported") : Result.AssetPath)),
			false);
		return;
	}

	bShowResult = false;
	if (Result.UserError.Kind == EUbstreamMeshGenErrorKind::Cancelled
		|| Result.FailedPhase == EUbstreamMeshGenPhase::Cancelled
		|| Result.ErrorMessage.Equals(TEXT("Cancelled"), ESearchCase::IgnoreCase))
	{
		ClearDisplayError();
		LastProgress.Phase = EUbstreamMeshGenPhase::Cancelled;
		LastProgress.StatusText = TEXT("Cancelled");
		NotifyUser(LOCTEXT("GenCancelledToast", "3D generation cancelled."), false);
		return;
	}

	const FUbstreamMeshGenUserError Error = Result.UserError.IsSet()
		? Result.UserError
		: UbstreamMeshGenError::MakeGeneric(
			Result.ErrorMessage.IsEmpty() ? TEXT("3D generation failed.") : Result.ErrorMessage,
			Result.FailedPhase);
	SetDisplayError(Error);
	LastProgress.Phase = EUbstreamMeshGenPhase::Failed;
	LastProgress.StatusText = Error.Message.ToString();
	NotifyUserError(Error);
}

void SUbstreamMeshGenPanel::SetDisplayError(const FUbstreamMeshGenUserError& Error)
{
	DisplayError = Error;
}

void SUbstreamMeshGenPanel::ClearDisplayError()
{
	DisplayError = FUbstreamMeshGenUserError();
}

void SUbstreamMeshGenPanel::NotifyUser(const FText& Message, const bool bIsError) const
{
	FNotificationInfo Info(Message);
	Info.bFireAndForget = true;
	Info.ExpireDuration = bIsError ? 6.0f : 4.0f;
	Info.bUseSuccessFailIcons = true;
	Info.Image = FAppStyle::GetBrush(bIsError ? "Icons.ErrorWithColor" : "Icons.SuccessWithColor");
	FSlateNotificationManager::Get().AddNotification(Info);
}

void SUbstreamMeshGenPanel::NotifyUserError(const FUbstreamMeshGenUserError& Error) const
{
	const FText Title = Error.Title.IsEmpty()
		? LOCTEXT("DefaultErrorTitleNotify", "3D generation failed")
		: Error.Title;
	const FText Message = Error.Message.IsEmpty()
		? LOCTEXT("DefaultErrorMessageNotify", "Something went wrong.")
		: Error.Message;
	NotifyUser(FText::Format(LOCTEXT("ErrorNotifyFmt", "{0}: {1}"), Title, Message), true);
}

bool SUbstreamMeshGenPanel::ResolveSourceImage(
	TArray<uint8>& OutBytes,
	FString& OutFileName,
	FString& OutContentType) const
{
	OutBytes.Reset();
	OutFileName.Reset();
	OutContentType.Reset();

	if (OnGetSourceImageBytes.IsBound())
	{
		OutBytes = OnGetSourceImageBytes.Execute();
		if (!OutBytes.IsEmpty())
		{
			OutFileName = TEXT("capture.jpg");
			OutContentType = DetectImageContentType(OutFileName, OutBytes);
			if (OutContentType == TEXT("image/jpeg") && !OutFileName.EndsWith(TEXT(".jpg")))
			{
				OutFileName = TEXT("capture.jpg");
			}
			return true;
		}
	}

	return false;
}

FUbstreamMeshGenRequest SUbstreamMeshGenPanel::BuildRequest(
	const TArray<uint8>& ImageBytes,
	const FString& FileName,
	const FString& ContentType) const
{
	FUbstreamMeshGenRequest Request;
	Request.ImageBytes = ImageBytes;
	Request.ImageFileName = FileName;
	Request.ImageContentType = ContentType;
	// Fresh random seed every export — no seed UI.
	Request.Seed = FMath::RandRange(0, 999999999);

	if (const UUbstreamModelerSettings* Settings = GetDefault<UUbstreamModelerSettings>())
	{
		Request.BaseUrl = Settings->MeshGenBaseUrl;
		Request.Quads = UbstreamMeshGenApi::ClampQuads(Settings->MeshGenQuads);
		Request.bNormalMap = Settings->bMeshGenNormalMap;
		Request.UserId = FUbstreamAuthService::Get().GetAccount();
		Request.PollIntervalSeconds = Settings->MeshGenPollIntervalSeconds;
		Request.StallTimeoutSeconds = FMath::Max(0.0f, Settings->MeshGenStallTimeoutMinutes) * 60.0;
		Request.ImportDirectory = Settings->MeshGenImportDirectory;
	}

	return Request;
}

bool SUbstreamMeshGenPanel::OpenGeneratedMeshPreview(UStaticMesh* StaticMesh)
{
	if (!IsValid(StaticMesh) || !GEditor)
	{
		return false;
	}

	const TWeakObjectPtr<UStaticMesh> WeakMesh(StaticMesh);
	const TWeakPtr<SUbstreamMeshGenPanel> WeakPanel = SharedThis(this);
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakMesh, WeakPanel](float)
		{
			const TSharedPtr<SUbstreamMeshGenPanel> Panel = WeakPanel.Pin();
			UStaticMesh* Mesh = WeakMesh.Get();
			if (!Panel.IsValid() || !IsValid(Mesh) || !GEditor)
			{
				return false;
			}

			if (UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				const bool bOpened = AssetEditors->OpenEditorForAsset(Mesh);
				if (bOpened)
				{
					Panel->NotifyUser(
						FText::Format(
							LOCTEXT("PreviewOpenedToast", "Preview opened for {0}"),
							FText::FromString(Mesh->GetName())),
						false);
				}
				else
				{
					Panel->NotifyUser(
						LOCTEXT("PreviewOpenFailedToast", "Mesh was imported, but the preview editor could not open. Use Content Browser to open it."),
						true);
				}
			}
			return false;
		}),
		0.0f);

	return true;
}

bool SUbstreamMeshGenPanel::LoadGeneratedMeshIntoStage(UStaticMesh* StaticMesh)
{
	if (!IsValid(StaticMesh))
	{
		return false;
	}

	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (!Viewport.IsValid())
	{
		NotifyUser(LOCTEXT("NoStage", "Stage viewport is not available."), true);
		return false;
	}

	if (!Viewport->ConfirmReplaceSubject())
	{
		return false;
	}

	if (!Viewport->SetStaticMesh(StaticMesh, /*bFocusCamera*/ true))
	{
		return false;
	}

	NotifyUser(LOCTEXT("LoadedStageToast", "Generated mesh loaded into the CSG stage."), false);
	return true;
}

#undef LOCTEXT_NAMESPACE
