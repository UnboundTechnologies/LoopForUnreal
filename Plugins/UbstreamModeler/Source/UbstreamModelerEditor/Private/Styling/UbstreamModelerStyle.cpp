#include "Styling/UbstreamModelerStyle.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Fonts/CompositeFont.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"

TSharedPtr<FSlateStyleSet> FUbstreamModelerStyle::StyleSet;
TSharedPtr<const FCompositeFont> FUbstreamModelerStyle::BrandCompositeFont;

void FUbstreamModelerStyle::Initialize()
{
	if (!StyleSet.IsValid())
	{
		StyleSet = Create();
		BrandCompositeFont = MakeShared<FStandaloneCompositeFont>(
			TEXT("Bold"),
			FPaths::Combine(StyleSet->GetContentRootDir(), TEXT("Fonts/WorkSans-Bold.ttf")),
			EFontHinting::Default,
			EFontLoadingPolicy::LazyLoad);
		FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
	}
}

void FUbstreamModelerStyle::Shutdown()
{
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
		ensure(StyleSet.IsUnique());
		StyleSet.Reset();
		BrandCompositeFont.Reset();
	}
}

FName FUbstreamModelerStyle::GetStyleSetName()
{
	static const FName StyleSetName(TEXT("UbstreamModelerStyle"));
	return StyleSetName;
}

const ISlateStyle& FUbstreamModelerStyle::Get()
{
	return *StyleSet;
}

FSlateFontInfo FUbstreamModelerStyle::BrandFont(float Size)
{
	// Work Sans Bold is the website's brand face (see Resources/Fonts).
	return FSlateFontInfo(BrandCompositeFont, Size);
}

FLinearColor FUbstreamModelerStyle::PreviewBackground()
{
	return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("191B1E")));
}

FLinearColor FUbstreamModelerStyle::AppBackground()
{
	return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("17191C")));
}

FLinearColor FUbstreamModelerStyle::CardBackground()
{
	return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("292C30")));
}

FLinearColor FUbstreamModelerStyle::StrongText()
{
	return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("EDEEF2")));
}

FLinearColor FUbstreamModelerStyle::MutedText()
{
	return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("ACB1BB")));
}

FLinearColor FUbstreamModelerStyle::StatusIdle()
{
	return FLinearColor(0.48f, 0.48f, 0.48f, 1.0f);
}

FLinearColor FUbstreamModelerStyle::StatusConnecting()
{
	return FLinearColor(0.32f, 0.62f, 1.0f, 1.0f);
}

FLinearColor FUbstreamModelerStyle::StatusReady()
{
	return FLinearColor(0.28f, 0.72f, 0.45f, 1.0f);
}

FLinearColor FUbstreamModelerStyle::StatusStreaming()
{
	return FLinearColor(0.18f, 0.80f, 0.62f, 1.0f);
}

FLinearColor FUbstreamModelerStyle::StatusWarning()
{
	return FLinearColor(0.95f, 0.62f, 0.20f, 1.0f);
}

FLinearColor FUbstreamModelerStyle::StatusError()
{
	return FLinearColor(0.95f, 0.22f, 0.22f, 1.0f);
}

FLinearColor FUbstreamModelerStyle::ErrorBannerBackground()
{
	return FLinearColor(0.28f, 0.08f, 0.08f, 0.92f);
}

FLinearColor FUbstreamModelerStyle::ErrorBannerBorder()
{
	return FLinearColor(0.75f, 0.22f, 0.22f, 0.95f);
}

FLinearColor FUbstreamModelerStyle::SuccessBannerBackground()
{
	return FLinearColor(0.07f, 0.20f, 0.12f, 0.92f);
}

FLinearColor FUbstreamModelerStyle::SuccessBannerBorder()
{
	return FLinearColor(0.28f, 0.72f, 0.45f, 0.95f);
}

FLinearColor FUbstreamModelerStyle::WarningBannerBackground()
{
	return FLinearColor(0.28f, 0.18f, 0.06f, 0.92f);
}

FLinearColor FUbstreamModelerStyle::WarningBannerBorder()
{
	return FLinearColor(0.95f, 0.62f, 0.20f, 0.95f);
}

FLinearColor FUbstreamModelerStyle::ActiveAccent()
{
	// The product's signature active purple (web editor --active-color).
	return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("602CFF")));
}

FLinearColor FUbstreamModelerStyle::PanelBackground()
{
	return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("202226")));
}

FLinearColor FUbstreamModelerStyle::StageBackground()
{
	return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("272B2D")));
}

FLinearColor FUbstreamModelerStyle::BorderColor()
{
	return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("41454D")));
}

FLinearColor FUbstreamModelerStyle::NumericAccent()
{
	return FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("0099FF")));
}

TSharedRef<FSlateStyleSet> FUbstreamModelerStyle::Create()
{
	TSharedRef<FSlateStyleSet> NewStyle = MakeShared<FSlateStyleSet>(GetStyleSetName());

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UbstreamModeler"));
	const FString ResourcesDir = Plugin.IsValid()
		? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"))
		: FPaths::ProjectPluginsDir() / TEXT("UbstreamModeler/Resources");

	NewStyle->SetContentRoot(ResourcesDir);
	NewStyle->Set(TEXT("Ubstream.EmptyStage"), new FSlateImageBrush(NewStyle->RootToContentDir(TEXT("EmptyStage"), TEXT(".png")), FVector2D(1024.0f, 1024.0f)));
	NewStyle->Set(TEXT("UbstreamModeler.Icon20"), new FSlateImageBrush(NewStyle->RootToContentDir(TEXT("Icon20"), TEXT(".png")), FVector2D(20.0f, 20.0f)));
	NewStyle->Set(TEXT("UbstreamModeler.Icon40"), new FSlateImageBrush(NewStyle->RootToContentDir(TEXT("Icon40"), TEXT(".png")), FVector2D(40.0f, 40.0f)));
	// Header lockup mirrors loop.unbound.io's navbar brand: 28px logo beside
	// the wordmark. Sampled down from the 128px source so it stays crisp.
	NewStyle->Set(TEXT("UbstreamModeler.BrandLogo"), new FSlateImageBrush(NewStyle->RootToContentDir(TEXT("Icon128"), TEXT(".png")), FVector2D(28.0f, 28.0f)));

	// Native polish: shared charcoal surfaces and restrained purple selection.
	// Keep colors in sRGB hex so panel tones match the design reference.
	const FLinearColor Panel = PanelBackground();
	const FLinearColor Border = BorderColor();
	const FLinearColor Accent = ActiveAccent();
	const FLinearColor RowHover(1.0f, 1.0f, 1.0f, 0.05f);
	const FLinearColor RowSelected(1.0f, 1.0f, 1.0f, 0.10f);

	NewStyle->Set(TEXT("Ubstream.AppBackground"), new FSlateColorBrush(AppBackground()));
	NewStyle->Set(TEXT("Ubstream.Panel"), new FSlateRoundedBoxBrush(Panel, 4.0f, Border, 1.0f));
	NewStyle->Set(TEXT("Ubstream.Panel.Flat"), new FSlateColorBrush(Panel));
	NewStyle->Set(TEXT("Ubstream.Toolbar.Floating"),
		new FSlateRoundedBoxBrush(Panel.CopyWithNewOpacity(0.92f), 8.0f, Border, 1.0f));
	NewStyle->Set(TEXT("Ubstream.Pill"),
		new FSlateRoundedBoxBrush(Panel.CopyWithNewOpacity(0.92f), 12.0f, Border, 1.0f));
	NewStyle->Set(TEXT("Ubstream.Dot"), new FSlateRoundedBoxBrush(FLinearColor::White, 5.0f));
	NewStyle->Set(TEXT("Ubstream.Group"),
		new FSlateRoundedBoxBrush(CardBackground(), 4.0f, Border, 1.0f));
	NewStyle->Set(TEXT("Ubstream.Swatch"),
		new FSlateRoundedBoxBrush(FLinearColor::Transparent, 5.0f, Border, 1.0f));
	NewStyle->Set(TEXT("Ubstream.Swatch.Selected"),
		new FSlateRoundedBoxBrush(FLinearColor::Transparent, 5.0f, StrongText(), 1.5f));
	NewStyle->Set(TEXT("Ubstream.ValueReadout"),
		new FSlateRoundedBoxBrush(PreviewBackground(), 4.0f, Border, 1.0f));

	FButtonStyle Button = FAppStyle::Get().GetWidgetStyle<FButtonStyle>("Button");
	Button.SetNormal(FSlateRoundedBoxBrush(CardBackground(), 4.0f, Border, 1.0f))
		.SetHovered(FSlateRoundedBoxBrush(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("383D45"))), 4.0f, MutedText(), 1.0f))
		.SetPressed(FSlateRoundedBoxBrush(Panel, 4.0f, Accent, 1.0f))
		.SetNormalForeground(StrongText())
		.SetHoveredForeground(FLinearColor::White)
		.SetPressedForeground(FLinearColor::White);
	NewStyle->Set(TEXT("Ubstream.Button"), Button);

	FButtonStyle PrimaryButton = Button;
	PrimaryButton.SetNormal(FSlateRoundedBoxBrush(Accent, 4.0f))
		.SetHovered(FSlateRoundedBoxBrush(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("794DFF"))), 4.0f))
		.SetPressed(FSlateRoundedBoxBrush(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("4B22CF"))), 4.0f))
		.SetNormalForeground(FLinearColor::White);
	NewStyle->Set(TEXT("Ubstream.PrimaryButton"), PrimaryButton);

	FEditableTextBoxStyle TextBox = FAppStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
	TextBox.SetBackgroundImageNormal(FSlateRoundedBoxBrush(PreviewBackground(), 4.0f, Border, 1.0f))
		.SetBackgroundImageHovered(FSlateRoundedBoxBrush(PreviewBackground(), 4.0f, MutedText(), 1.0f))
		.SetBackgroundImageFocused(FSlateRoundedBoxBrush(PreviewBackground(), 4.0f, Accent, 1.0f))
		.SetForegroundColor(StrongText());
	NewStyle->Set(TEXT("Ubstream.TextBox"), TextBox);

	NewStyle->Set(TEXT("Ubstream.OutlinerRow"), FCheckBoxStyle()
		.SetCheckBoxType(ESlateCheckBoxType::ToggleButton)
		.SetUncheckedImage(FSlateNoResource())
		.SetUncheckedHoveredImage(FSlateRoundedBoxBrush(RowHover, 4.0f))
		.SetUncheckedPressedImage(FSlateRoundedBoxBrush(RowSelected, 4.0f))
		.SetCheckedImage(FSlateRoundedBoxBrush(RowSelected, 4.0f))
		.SetCheckedHoveredImage(FSlateRoundedBoxBrush(RowSelected, 4.0f))
		.SetCheckedPressedImage(FSlateRoundedBoxBrush(RowSelected, 4.0f))
		.SetPadding(FMargin(6.0f, 3.0f)));

	NewStyle->Set(TEXT("Ubstream.IconToggle"), FCheckBoxStyle()
		.SetCheckBoxType(ESlateCheckBoxType::ToggleButton)
		.SetUncheckedImage(FSlateRoundedBoxBrush(CardBackground(), 4.0f, Border, 1.0f))
		.SetUncheckedHoveredImage(FSlateRoundedBoxBrush(CardBackground(), 4.0f, MutedText(), 1.0f))
		.SetUncheckedPressedImage(FSlateRoundedBoxBrush(Panel, 4.0f, Accent, 1.0f))
		.SetCheckedImage(FSlateRoundedBoxBrush(Accent, 4.0f))
		.SetCheckedHoveredImage(FSlateRoundedBoxBrush(Accent.CopyWithNewOpacity(0.85f), 4.0f))
		.SetCheckedPressedImage(FSlateRoundedBoxBrush(Accent, 4.0f))
		.SetPadding(FMargin(4.0f)));

	NewStyle->Set(TEXT("Ubstream.IconButton"), FButtonStyle()
		.SetNormal(FSlateNoResource())
		.SetHovered(FSlateRoundedBoxBrush(RowHover, 4.0f))
		.SetPressed(FSlateRoundedBoxBrush(RowSelected, 4.0f))
		.SetNormalForeground(FLinearColor(0.8f, 0.8f, 0.8f, 1.0f))
		.SetHoveredForeground(FLinearColor::White)
		.SetPressedForeground(FLinearColor::White)
		.SetNormalPadding(FMargin(4.0f))
		.SetPressedPadding(FMargin(4.0f)));

	// Keep the marquee texture neutral so the widget's brand tint is not
	// multiplied by the editor's default blue primary color.
	FProgressBarStyle ProgressBar = FAppStyle::Get().GetWidgetStyle<FProgressBarStyle>(TEXT("ProgressBar"));
	ProgressBar.MarqueeImage.TintColor = FLinearColor::White;
	ProgressBar.SetEnableFillAnimation(false);
	NewStyle->Set(TEXT("Ubstream.ProgressBar"), ProgressBar);

	// SSlider's clickable strip is exactly max(bar, thumb) tall, so the stock
	// editor slider is a fiddly ~11px target. The tall thumb here is what buys
	// the comfortable hit area.
	NewStyle->Set(TEXT("Ubstream.Slider"), FSliderStyle()
		.SetNormalBarImage(FSlateRoundedBoxBrush(FLinearColor(0.20f, 0.20f, 0.20f, 1.0f), 3.0f, FVector2D(8.0f, 8.0f)))
		.SetHoveredBarImage(FSlateRoundedBoxBrush(FLinearColor(0.26f, 0.26f, 0.26f, 1.0f), 3.0f, FVector2D(8.0f, 8.0f)))
		.SetDisabledBarImage(FSlateRoundedBoxBrush(FLinearColor(0.13f, 0.13f, 0.13f, 1.0f), 3.0f, FVector2D(8.0f, 8.0f)))
		.SetNormalThumbImage(FSlateRoundedBoxBrush(FLinearColor(0.86f, 0.88f, 0.90f, 1.0f), 9.0f, FVector2D(18.0f, 18.0f)))
		.SetHoveredThumbImage(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f), 10.0f, FVector2D(20.0f, 20.0f)))
		.SetDisabledThumbImage(FSlateRoundedBoxBrush(FLinearColor(0.42f, 0.42f, 0.42f, 1.0f), 9.0f, FVector2D(18.0f, 18.0f)))
		.SetBarThickness(5.0f));

	return NewStyle;
}
