#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateTypes.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"

class FUbstreamWebSocketClient;
class AActor;
class IDetailsView;
class SEditableTextBox;
class STextBlock;
class SUbstreamControlPanel;
class SUbstreamModelerViewport;
class UMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;
enum class EUbstreamStageCameraView : uint8;
enum class EUbstreamCSGPrimitiveType : uint8;
enum class EUbstreamCSGTransformMode : uint8;

class SUbstreamModelerWidget final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUbstreamModelerWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	bool LoadSoloMesh(UStaticMesh* StaticMesh);
	bool CanCloseWorkspace() const;
	bool LoadSoloActors(const TArray<TWeakObjectPtr<AActor>>& Actors);
	bool LoadSoloComponents(const TArray<TWeakObjectPtr<UMeshComponent>>& Components);
	/** Stage viewport access for the visual-check automation. */
	TSharedPtr<SUbstreamModelerViewport> GetSoloViewport() const { return SoloViewport; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
	friend class FUbstreamStageLightingTest;
	friend class FUbstreamStreamProfileTest;
	TSharedRef<SWidget> BuildSoloStage();
	TSharedRef<SWidget> BuildCameraViewMenu();
	TSharedRef<SWidget> BuildDisplayViewMenu();
	TSharedRef<SWidget> BuildLightingMenu();
	/** Per-light most-recent-first colors confirmed in the picker, persisted per project. */
	void LoadRecentLightColors();
	void RememberLightColor(TArray<FLinearColor> SUbstreamModelerWidget::* Recents, const TCHAR* ConfigKey, const FLinearColor& Color);
	TSharedRef<SWidget> BuildTransformModeToolbar();
	TSharedRef<SWidget> BuildCutterToolbar();
	FReply LoadSelectedMeshClicked();
	void HandleLoadSelectedRequested();
	FReply FocusSoloMeshClicked();
	FReply CameraViewClicked(EUbstreamStageCameraView CameraView);
	void HandleDisplayViewModeChanged(ECheckBoxState NewState, int32 ViewMode);
	void HandleDisplayOverlayChanged(ECheckBoxState NewState, int32 Overlay);
	ECheckBoxState GetDisplayViewModeCheckState(int32 ViewMode) const;
	ECheckBoxState GetDisplayOverlayCheckState(int32 Overlay) const;
	ECheckBoxState GetOrbitCameraCheckState() const;
	void HandleOrbitCameraChanged(ECheckBoxState NewState);
	FReply ResyncSourceClicked();
	FReply ClearSoloMeshClicked();
	FReply ApplyEditsClicked();
	FReply AddCSGPrimitiveClicked(EUbstreamCSGPrimitiveType PrimitiveType);
	FReply CSGTransformModeClicked(EUbstreamCSGTransformMode TransformMode);
	void HandleCSGTransformModeChanged(ECheckBoxState NewState, EUbstreamCSGTransformMode TransformMode);
	bool IsApplyEditsEnabled() const;
	bool IsAddCSGPrimitiveEnabled() const;
	bool IsDeleteSelectedCSGPrimitiveEnabled() const;
	bool IsClearCSGPrimitivesEnabled() const;
	bool IsCSGTransformActionEnabled() const;
	bool IsCSGToolActionEnabled() const;
	ECheckBoxState GetCSGTransformModeCheckState(EUbstreamCSGTransformMode TransformMode) const;
	EVisibility GetCSGWorkflowVisibility() const;
	FText GetCSGStatusText() const;
	void HandleEditingStateChanged();
	EVisibility GetSoloPlaceholderVisibility() const;
	FOptionalSize GetSoloPlaceholderWidthOverride() const;
	float GetSoloPlaceholderWrapWidth() const;
	FText GetSoloPlaceholderTitleText() const;
	FText GetSoloPlaceholderBodyText() const;
	FText GetSoloStatusText() const;
	bool IsSoloMeshLoaded() const;
	FText GetSourceBadgeText() const;
	FText GetModelingModeBadgeText() const;
	FSlateColor GetSourceBadgeColor() const;
	FSlateColor GetModelingModeBadgeColor() const;
	bool IsResyncSourceEnabled() const;
	bool LoadSelectedSubject();
	TArray<UMeshComponent*> ResolveSelectedMeshComponents() const;
	TArray<TWeakObjectPtr<AActor>> ResolveSelectedActors() const;
	UStaticMesh* ResolveSelectedStaticMesh() const;
	int32 SelectLoadedSourceInEditor() const;
	void NotifyStage(const FText& Message, bool bIsError) const;
	void NotifyLoadFailure(const FText& Message) const;
	bool ConfirmDiscardStageEdits(const FText& Prompt) const;
	void HandleSoloMeshChanged();
	EActiveTimerReturnType HandleDeferredModelingModeActivation(double InCurrentTime, float InDeltaTime);
	EActiveTimerReturnType HandleSourceStatusActiveTimer(double InCurrentTime, float InDeltaTime);

	TSharedPtr<SUbstreamModelerViewport> SoloViewport;
	TArray<FLinearColor> RecentMainLightColors;
	TArray<FLinearColor> RecentAmbientLightColors;
	TSharedPtr<SUbstreamControlPanel> ControlPanel;
	/** The inline account panel, toggled by the Settings button; closed on sign-out. */
	bool bAccountPanelOpen = false;
	TSharedPtr<class SUbstreamElementsPanel> ElementsPanel;
	FString SoloStageStatus;
	FString LastNotifiedWorkspaceKey;
};
