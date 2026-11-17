#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SUbstreamModelerViewport;
class SVerticalBox;
class UMeshComponent;
enum class EUbstreamCSGOperation : uint8;

/**
 * Left-hand elements panel: the cutter outliner (web-editor style rows with
 * op-colored dots) plus the selected cutter's properties (operation,
 * position, rotation, size, snap). Purely a view over SUbstreamModelerViewport
 * state - every action forwards to viewport APIs.
 */
class SUbstreamElementsPanel final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUbstreamElementsPanel) {}
		SLATE_ARGUMENT(TWeakPtr<SUbstreamModelerViewport>, SoloViewport)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Refreshes attributes; rebuilds rows only if membership/order/names changed. */
	void Refresh();

private:
	friend class FUbstreamStageEditingAutomationTest;
	TSharedRef<SWidget> BuildHeaderRow();
	TSharedRef<SWidget> BuildModelRow();
	TSharedRef<SWidget> BuildPropertiesSection();
	TSharedRef<SWidget> MakeCutterRow(TWeakObjectPtr<UMeshComponent> Cutter, const FString& Name);
	TSharedRef<SWidget> MakeOperationToggle(const FText& Label, EUbstreamCSGOperation Operation, const FLinearColor& OpColor);
	TSharedRef<SWidget> BuildSnapMenu();

	bool IsCutterSelected(TWeakObjectPtr<UMeshComponent> Cutter) const;
	void HandleRowClicked(TWeakObjectPtr<UMeshComponent> Cutter);
	void DeleteCutter(TWeakObjectPtr<UMeshComponent> Cutter);
	FLinearColor GetOperationColor(const UMeshComponent* Cutter) const;

	TOptional<double> GetLocationComponent(int32 Axis) const;
	void HandleLocationChanged(double NewValue, int32 Axis);
	TOptional<double> GetRotationComponent(int32 Axis) const;
	void HandleRotationChanged(double NewValue, int32 Axis);
	TOptional<double> GetDimensionComponent(int32 Axis) const;
	void HandleDimensionChanged(double NewValue, int32 Axis);
	TOptional<double> GetSize() const;
	void HandleSizeChanged(double NewValue);
	TOptional<double> GetUniformScale() const;
	void HandleUniformScaleChanged(double NewValue);
	float GetUniformScaleSliderValue() const;
	void HandleUniformScaleSliderChanged(float NewValue);

	TWeakPtr<SUbstreamModelerViewport> SoloViewport;
	TSharedPtr<SVerticalBox> CutterRowsBox;
	TArray<TPair<TWeakObjectPtr<UMeshComponent>, FString>> DisplayedCutters;
};
