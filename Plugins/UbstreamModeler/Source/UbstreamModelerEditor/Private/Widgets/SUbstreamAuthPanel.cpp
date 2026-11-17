#include "Widgets/SUbstreamAuthPanel.h"
#include "Auth/UbstreamAuthService.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/AppStyle.h"
#include "Styling/UbstreamModelerStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UbstreamAuth"

namespace
{
TSharedRef<STextBlock> Label(const FText& Text, int32 Size = 11, bool bMuted = false, bool bBold = false)
{
	return SNew(STextBlock).Text(Text).AutoWrapText(true)
		.Font(FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size))
		.ColorAndOpacity(bMuted ? FUbstreamModelerStyle::MutedText() : FUbstreamModelerStyle::StrongText());
}
TSharedRef<SWidget> Step(const FText& Number, const FText& Text)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 14, 0)[Label(Number, 10, true)]
		+ SHorizontalBox::Slot().FillWidth(1)[Label(Text, 11, true)];
}
}

void SUbstreamAuthPanel::Construct(const FArguments& InArgs)
{
	if (InArgs._AccountMode)
	{
		ChildSlot
		[
			SNew(SBox).WidthOverride(320)
			[
				SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FUbstreamModelerStyle::PanelBackground()).Padding(22)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[Label(LOCTEXT("AccountEyebrow", "ACCOUNT & ACCESS"), 9, true, true)]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 4)
					[
						SNew(STextBlock).Text_Lambda([] { return FText::FromString(FUbstreamAuthService::Get().GetAccount()); })
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13)).AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16)
					[
						SNew(STextBlock)
						.Text_Lambda([] { return FUbstreamAuthService::Get().IsAuthenticated()
							? LOCTEXT("Connected", "Connected to Unbound Loop") : LOCTEXT("SignedOut", "Signed out"); })
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
						.ColorAndOpacity_Lambda([] { return FSlateColor(FUbstreamAuthService::Get().IsAuthenticated()
							? FUbstreamModelerStyle::StatusReady() : FUbstreamModelerStyle::MutedText()); })
					]
					+ SVerticalBox::Slot().AutoHeight()[SNew(SSeparator)]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 16, 0, 6)[Label(LOCTEXT("KeyLabel", "AUTHENTICATION KEY"), 9, true, true)]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text_Lambda([] { return FText::FromString(FUbstreamAuthService::Get().GetKeyLabel()); })
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11)).AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
					[
						SNew(STextBlock).Text(LOCTEXT("MaskedKey", "ubue_  ••••••••••••••••••••"))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11)).ColorAndOpacity(FUbstreamModelerStyle::MutedText())
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 18)
					[
						SNew(STextBlock).Text_Lambda([] { return FText::FromString(FUbstreamAuthService::Get().GetStorageLabel()); })
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10)).ColorAndOpacity(FUbstreamModelerStyle::MutedText()).AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SButton).Text(LOCTEXT("ManageKeys", "Manage key in browser  ↗")).ContentPadding(FMargin(12, 9))
						.OnClicked_Lambda([] { FUbstreamAuthService::Get().OpenKeyPage(); return FReply::Handled(); })
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 8)
					[
						SNew(SButton).Text(LOCTEXT("RemoveKey", "Remove key & sign out")).ContentPadding(FMargin(12, 9))
						.ForegroundColor(FUbstreamModelerStyle::StatusError())
						.OnClicked_Lambda([] { FUbstreamAuthService::Get().RemoveKey(); return FReply::Handled(); })
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
					[Label(LOCTEXT("RemoveHelp", "Removes access on this computer and stops active generation. Your stage edits stay in place."), 10, true)]
				]
			]
		];
		return;
	}

	ChildSlot
	[
		SNew(SBox).MaxDesiredWidth(460)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 22)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
				[SNew(SImage).Image(FUbstreamModelerStyle::Get().GetBrush("UbstreamModeler.Icon40"))]
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(LOCTEXT("Brand", "UNBOUND LOOP"), 12, false, true)]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Label(LOCTEXT("Platform", "FOR UNREAL ENGINE"), 9, true)]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SWidgetSwitcher)
				.WidgetIndex_Lambda([] { return FUbstreamAuthService::Get().IsSigningIn() ? 1 : 0; })
				+ SWidgetSwitcher::Slot()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[Label(LOCTEXT("Welcome", "Real-time Augmented Sculpting"), 30, false, true)]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 26)
					[Label(LOCTEXT("Intro", "Connect your Unbound Loop account to bring live previews and 3D generation into your workflow."), 12, true)]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBorder).BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Panel"))
						.Padding(24)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()[Label(LOCTEXT("ConnectTitle", "Connect your account"), 16, false, true)]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 20, 0, 10)[Step(LOCTEXT("StepOne", "01"), LOCTEXT("BrowserStep", "Sign in on the web. Unreal connects automatically."))]
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SButton).Text_Lambda([]
								{
									const auto& Auth = FUbstreamAuthService::Get();
									return Auth.IsBrowserSignInPending()
										? (Auth.GetVerificationCode().IsEmpty() ? LOCTEXT("OpeningBrowser", "Opening browser...") : LOCTEXT("ReopenBrowser", "Open browser again  ↗"))
										: LOCTEXT("BrowserSignIn", "Continue in browser  ↗");
								})
								.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.PrimaryButton")
								.IsEnabled_Lambda([] { const auto& Auth = FUbstreamAuthService::Get(); return !Auth.IsBusy() && (!Auth.IsBrowserSignInPending() || !Auth.GetVerificationCode().IsEmpty()); })
								.ContentPadding(FMargin(16, 12)).HAlign(HAlign_Center)
								.OnClicked_Lambda([] { FUbstreamAuthService::Get().BeginBrowserSignIn(); return FReply::Handled(); })
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 16, 0, 0)
							[
								SNew(SVerticalBox).Visibility_Lambda([] { return FUbstreamAuthService::Get().IsBrowserSignInPending() ? EVisibility::Visible : EVisibility::Collapsed; })
								+ SVerticalBox::Slot().AutoHeight()[Label(LOCTEXT("MatchCode", "BROWSER CONFIRMATION CODE"), 9, true, true)]
								+ SVerticalBox::Slot().AutoHeight().Padding(0, 8)
								[
									SNew(STextBlock).Text_Lambda([] { return FText::FromString(FUbstreamAuthService::Get().GetVerificationCode()); })
									.Font(FCoreStyle::GetDefaultFontStyle("Bold", 24))
								]
								+ SVerticalBox::Slot().AutoHeight()[Label(LOCTEXT("AutoConnectHint", "Waiting for approval. You can return to Unreal now; it will connect in the background."), 10, true)]
								+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0, 8, 0, 0)
								[
									SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").Text(LOCTEXT("CancelBrowser", "Cancel browser sign-in"))
									.OnClicked_Lambda([] { FUbstreamAuthService::Get().CancelBrowserSignIn(); return FReply::Handled(); })
								]
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 22, 0, 10)[Label(LOCTEXT("ManualFallback", "OR CONNECT WITH AN AUTH TOKEN"), 9, true, true)]
							+ SVerticalBox::Slot().AutoHeight()
							[
								SAssignNew(KeyInput, SEditableTextBox).IsPassword(true)
								.HintText(LOCTEXT("TokenPlaceholder", "Enter auth token"))
								.Padding(FMargin(12, 12)).IsEnabled_Lambda([] { return !FUbstreamAuthService::Get().IsBusy(); })
								.OnTextCommitted_Lambda([this](const FText&, ETextCommit::Type Type) { if (Type == ETextCommit::OnEnter) ConnectClicked(); })
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
							[
								SNew(SButton).HAlign(HAlign_Center).ContentPadding(FMargin(16, 12))
								.IsEnabled_Lambda([this] { return !FUbstreamAuthService::Get().IsBusy() && (FUbstreamAuthService::Get().HasKey() || !KeyInput->GetText().IsEmpty()); })
								.OnClicked(this, &SUbstreamAuthPanel::ConnectClicked)
								[
									SNew(STextBlock).Text_Lambda([] { return FUbstreamAuthService::Get().IsBusy() ? LOCTEXT("Verifying", "Connecting...") : LOCTEXT("Connect", "Connect account"); })
									.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
								]
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
							[
								SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").Text(LOCTEXT("GetManualKey", "Get a key for manual sign-in  ↗"))
								.OnClicked_Lambda([] { FUbstreamAuthService::Get().OpenKeyPage(); return FReply::Handled(); })
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 0)
							[
								SNew(STextBlock).Text_Lambda([] { return FText::FromString(FUbstreamAuthService::Get().GetMessage()); })
								.Visibility_Lambda([] { return FUbstreamAuthService::Get().GetMessage().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10)).AutoWrapText(true)
								.ColorAndOpacity(FUbstreamModelerStyle::StatusWarning())
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
							[
								SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton")
								.Text(LOCTEXT("ForgetSaved", "Remove saved key"))
								.Visibility_Lambda([] { return FUbstreamAuthService::Get().HasKey() ? EVisibility::Visible : EVisibility::Collapsed; })
								.OnClicked_Lambda([this] { KeyInput->SetText(FText::GetEmpty()); FUbstreamAuthService::Get().RemoveKey(); return FReply::Handled(); })
							]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(2, 18, 2, 0)
					[Label(LOCTEXT("StorageHint", "Your key stays in your system credential store.\nManage or remove it anytime from Settings."), 10, true)]
				]
				+ SWidgetSwitcher::Slot()
				[
					SNew(SBorder).BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Panel"))
					.Padding(FMargin(32, 40))
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
						[
							SNew(SCircularThrobber).Radius(16).NumPieces(8)
							.ColorAndOpacity(FUbstreamModelerStyle::ActiveAccent())
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 24, 0, 10)
						[
							SNew(STextBlock).Text(LOCTEXT("SigningInTitle", "Signing you in…"))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 24))
							.ColorAndOpacity(FUbstreamModelerStyle::StrongText()).Justification(ETextJustify::Center)
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock).Text(LOCTEXT("SigningInDetail", "Checking your Unbound account.\nYour workspace will open automatically."))
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 12))
							.ColorAndOpacity(FUbstreamModelerStyle::MutedText()).Justification(ETextJustify::Center).AutoWrapText(true)
						]
					]
				]
			]
		]
	];
	FUbstreamAuthService::Get().OnChanged.AddSPLambda(this, [this]
	{
		if (FUbstreamAuthService::Get().IsAuthenticated() && KeyInput.IsValid()) KeyInput->SetText(FText::GetEmpty());
	});
}

FReply SUbstreamAuthPanel::ConnectClicked()
{
	if (FUbstreamAuthService::Get().IsBusy()) return FReply::Handled();
	FString Key = KeyInput->GetText().ToString();
	KeyInput->SetText(FText::GetEmpty());
	if (Key.TrimStartAndEnd().IsEmpty()) FUbstreamAuthService::Get().Retry();
	else FUbstreamAuthService::Get().SignIn(MoveTemp(Key));
	return FReply::Handled();
}
#undef LOCTEXT_NAMESPACE
