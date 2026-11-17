#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Widgets/SCompoundWidget.h"

class SUbstreamModelerViewport;
class SProgressBar;
enum class EUbstreamSculptBrush : uint8;

/**
 * Sculpt controls for the stage panel: brush picker plus size / strength /
 * falloff / symmetry. Sits inline beneath the CSG cutter controls and forwards
 * everything to SUbstreamModelerViewport's sculpt API (which wraps Epic's
 * UMeshVertexSculptTool). Purely a view over viewport state - attribute-driven,
 * so it needs no explicit refresh.
 */
class SUbstreamSculptPanel final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUbstreamSculptPanel) {}
		SLATE_ARGUMENT(TWeakPtr<SUbstreamModelerViewport>, SoloViewport)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	~SUbstreamSculptPanel() override;

private:
	TSharedRef<SWidget> BuildHeaderRow();
	TSharedRef<SWidget> BuildBrushGrid();
	TSharedRef<SWidget> BuildParameters();
	TSharedRef<SWidget> BuildDetailSection();
	TSharedRef<SWidget> MakeBrushToggle(const FText& Label, EUbstreamSculptBrush Brush, const FText& ToolTip);
	TSharedRef<SWidget> MakeParamSlider(
		const FText& Label,
		const FText& ToolTip,
		TFunction<float()> Getter,
		TFunction<void(float)> Setter);

	bool CanSculpt() const;
	bool IsSculptActive() const;
	void UpdateRemeshProgress();

	TWeakPtr<SUbstreamModelerViewport> SoloViewport;
	TSharedPtr<SProgressBar> RemeshProgressBar;
	FDelegateHandle RemeshStateHandle;
};
