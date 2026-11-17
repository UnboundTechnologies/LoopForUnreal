#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"

class ISlateStyle;
class FSlateStyleSet;
struct FCompositeFont;

class FUbstreamModelerStyle
{
public:
	static void Initialize();
	static void Shutdown();

	static FName GetStyleSetName();
	static const ISlateStyle& Get();

	static FSlateFontInfo BrandFont(float Size);

	static FLinearColor PreviewBackground();
	static FLinearColor AppBackground();
	static FLinearColor CardBackground();
	static FLinearColor StrongText();
	static FLinearColor MutedText();
	static FLinearColor StatusIdle();
	static FLinearColor StatusConnecting();
	static FLinearColor StatusReady();
	static FLinearColor StatusStreaming();
	static FLinearColor StatusWarning();
	static FLinearColor StatusError();
	static FLinearColor ErrorBannerBackground();
	static FLinearColor ErrorBannerBorder();
	static FLinearColor SuccessBannerBackground();
	static FLinearColor SuccessBannerBorder();
	static FLinearColor WarningBannerBackground();
	static FLinearColor WarningBannerBorder();
	static FLinearColor ActiveAccent();
	static FLinearColor PanelBackground();
	static FLinearColor StageBackground();
	static FLinearColor BorderColor();
	static FLinearColor NumericAccent();

	static constexpr float OuterPadding = 10.0f;
	static constexpr float CardPadding = 12.0f;
	static constexpr float StackGap = 12.0f;
	static constexpr float SmallGap = 6.0f;

private:
	static TSharedRef<FSlateStyleSet> Create();

	static TSharedPtr<FSlateStyleSet> StyleSet;
	static TSharedPtr<const FCompositeFont> BrandCompositeFont;
};
