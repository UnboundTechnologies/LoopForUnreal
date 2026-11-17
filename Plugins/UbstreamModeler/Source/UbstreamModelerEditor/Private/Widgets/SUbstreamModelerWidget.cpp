#include "Widgets/SUbstreamModelerWidget.h"
#include "Auth/UbstreamAuthService.h"
#include "Widgets/SUbstreamAuthPanel.h"
#include "Widgets/Layout/SScrollBox.h"

#include "AssetRegistry/AssetData.h"
#include "Bridge/UbstreamModelingModeBridge.h"
#include "Components/StaticMeshComponent.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "ContentBrowserModule.h"
#include "Editing/UbstreamStageEditorMode.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "IContentBrowserSingleton.h"
#include "InteractiveTool.h"
#include "Modules/ModuleManager.h"
#include "Selection.h"
#include "Settings/UbstreamModelerSettings.h"
#include "Styling/UbstreamModelerStyle.h"
#include "Utils/UbstreamMeshComponentUtils.h"
#include "Viewport/UbstreamViewportClient.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Types/WidgetActiveTimerDelegate.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SUbstreamControlPanel.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SRotatorInputBox.h"
#include "Widgets/Input/SVectorInputBox.h"
#include "Widgets/SUbstreamElementsPanel.h"
#include "UbstreamModelerEditorModule.h"
#include "Widgets/SUbstreamModelerViewport.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SUbstreamModelerWidget"

namespace
{
	/** Static empty-state image; forwards mesh drops while the live viewport is hidden. */
	class SEmptyStagePreview final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SEmptyStagePreview) {}
			SLATE_ARGUMENT(TSharedPtr<SUbstreamModelerViewport>, Viewport)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Viewport = InArgs._Viewport;
			ChildSlot
			[
				SNew(SScaleBox).Stretch(EStretch::ScaleToFill).Clipping(EWidgetClipping::ClipToBounds)
				[
					SNew(SImage).Image(FUbstreamModelerStyle::Get().GetBrush("Ubstream.EmptyStage"))
					.ColorAndOpacity(FLinearColor(0.45f, 0.45f, 0.45f, 1.0f))
				]
			];
		}

		virtual FReply OnDragOver(const FGeometry& Geometry, const FDragDropEvent& Event) override
		{
			const auto Target = Viewport.Pin();
			return Target ? Target->OnDragOver(Geometry, Event) : FReply::Unhandled();
		}

		virtual FReply OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event) override
		{
			const auto Target = Viewport.Pin();
			return Target ? Target->OnDrop(Geometry, Event) : FReply::Unhandled();
		}

	private:
		TWeakPtr<SUbstreamModelerViewport> Viewport;
	};

	const FSlateBrush* GetStagePillBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor::White, 8.0f, FVector2D(1.0f, 1.0f));
		return &Brush;
	}

	TSharedRef<SWidget> BuildStageIconButton(
		const FText& ToolTip,
		const FName IconName,
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
					.Image(FAppStyle::GetBrush(IconName))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			];
	}

	TSharedRef<SWidget> BuildStageToggleIconButton(
		const FText& ToolTip,
		const FName IconName,
		const FName StyleName,
		const TAttribute<ECheckBoxState>& CheckState,
		const FOnCheckStateChanged& OnChanged,
		const TAttribute<bool>& IsEnabled = true)
	{
		return SNew(SCheckBox)
			.Style(FAppStyle::Get(), StyleName)
			.Padding(FMargin(4.0f))
			.ToolTipText(ToolTip)
			.IsEnabled(IsEnabled)
			.IsChecked(CheckState)
			.OnCheckStateChanged(OnChanged)
			[
				SNew(SBox)
				.WidthOverride(16.0f)
				.HeightOverride(16.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush(IconName))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			];
	}

	TSharedRef<SWidget> BuildStageCommandButton(
		const FText& Label,
		const FText& ToolTip,
		const FName IconName,
		const FOnClicked& OnClicked,
		const TAttribute<bool>& IsEnabled = true,
		const float MinWidth = 78.0f)
	{
		return SNew(SButton)
			.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
			.ContentPadding(FMargin(7.0f, 4.0f))
			.ToolTipText(ToolTip)
			.IsEnabled(IsEnabled)
			.OnClicked(OnClicked)
			[
				SNew(SBox)
				.MinDesiredWidth(MinWidth)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 5.0f, 0.0f)
					[
						SNew(SBox)
						.WidthOverride(14.0f)
						.HeightOverride(14.0f)
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush(IconName))
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(Label)
						.Justification(ETextJustify::Center)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			];
	}

	TSharedRef<SWidget> BuildCameraPresetButton(
		const FText& Label,
		const FText& ToolTip,
		const FOnClicked& OnClicked,
		const TAttribute<bool>& IsEnabled = true)
	{
		return SNew(SButton)
			.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
			.ContentPadding(FMargin(5.0f, 2.0f))
			.ToolTipText(ToolTip)
			.IsEnabled(IsEnabled)
			.OnClicked(OnClicked)
			[
				SNew(STextBlock)
				.Text(Label)
				.Justification(ETextJustify::Center)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.ColorAndOpacity(FSlateColor::UseForeground())
			];
	}

	TSharedRef<SWidget> BuildDisplayMenuHeader(const FText& Label)
	{
		return SNew(SBorder)
			.Padding(FMargin(6.0f, 6.0f, 6.0f, 2.0f))
			.BorderImage(FAppStyle::GetBrush("NoBorder"))
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.56f, 0.58f, 0.62f, 1.0f)))
			];
	}

	TSharedRef<SWidget> BuildDisplayMenuCheckRow(
		const FText& Label,
		const FText& ToolTip,
		const TAttribute<ECheckBoxState>& CheckState,
		const FOnCheckStateChanged& OnChanged)
	{
		return SNew(SCheckBox)
			.Padding(FMargin(6.0f, 3.0f))
			.ToolTipText(ToolTip)
			.IsChecked(CheckState)
			.OnCheckStateChanged(OnChanged)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			];
	}

	FText GetStageToolBadgeText(const FString& ToolIdentifier)
	{
		if (ToolIdentifier == UUbstreamStageEditorMode::PolyEditToolIdentifier)
		{
			return LOCTEXT("PolyEditToolBadge", "POLY EDIT TOOL");
		}
		if (ToolIdentifier == UUbstreamStageEditorMode::VertexSculptToolIdentifier)
		{
			return LOCTEXT("SculptToolBadge", "SCULPT TOOL");
		}
		if (ToolIdentifier == UUbstreamStageEditorMode::SmoothToolIdentifier)
		{
			return LOCTEXT("SmoothToolBadge", "SMOOTH TOOL");
		}
		if (ToolIdentifier == UUbstreamStageEditorMode::OffsetToolIdentifier)
		{
			return LOCTEXT("OffsetToolBadge", "OFFSET TOOL");
		}
		if (ToolIdentifier == UUbstreamStageEditorMode::RemeshToolIdentifier)
		{
			return LOCTEXT("RemeshToolBadge", "REMESH TOOL");
		}
		if (ToolIdentifier == UUbstreamStageEditorMode::CSGMeshesToolIdentifier)
		{
			return LOCTEXT("CSGMeshesToolBadge", "BOOLEAN TOOL");
		}
		if (ToolIdentifier == UUbstreamStageEditorMode::TrimMeshesToolIdentifier)
		{
			return LOCTEXT("TrimMeshesToolBadge", "TRIM TOOL");
		}
		if (ToolIdentifier == UUbstreamStageEditorMode::DisplaceToolIdentifier)
		{
			return LOCTEXT("DisplaceToolBadge", "DISPLACE TOOL");
		}
		if (ToolIdentifier == UUbstreamStageEditorMode::LatticeToolIdentifier)
		{
			return LOCTEXT("LatticeToolBadge", "LATTICE TOOL");
		}
		if (ToolIdentifier == UUbstreamStageEditorMode::WeldToolIdentifier)
		{
			return LOCTEXT("WeldToolBadge", "WELD TOOL");
		}
		if (ToolIdentifier == UUbstreamStageEditorMode::HoleFillToolIdentifier)
		{
			return LOCTEXT("HoleFillToolBadge", "FILL HOLES TOOL");
		}

		return LOCTEXT("ActiveToolBadge", "TOOL ACTIVE");
	}
}

void SUbstreamModelerWidget::Construct(const FArguments& InArgs)
{
	FUbstreamAuthService::Get().Initialize();
	LoadRecentLightColors();
	SAssignNew(SoloViewport, SUbstreamModelerViewport)
		.OnSoloMeshChanged(FSimpleDelegate::CreateSP(this, &SUbstreamModelerWidget::HandleSoloMeshChanged))
		.OnEditingStateChanged(FSimpleDelegate::CreateSP(this, &SUbstreamModelerWidget::HandleEditingStateChanged));
	SoloViewport->SetVisibility(TAttribute<EVisibility>::CreateLambda([this]
	{
		return IsSoloMeshLoaded() ? EVisibility::Visible : EVisibility::Hidden;
	}));
	SoloStageStatus = TEXT("No isolated subject loaded");

	TSharedRef<SWidget> Workspace =
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FUbstreamModelerStyle::AppBackground())
		.Padding(6.0f)
		[
			// Web-editor layout: elements/properties column on the left (its
			// own splitter slot so the user can resize it), stage + streaming
			// panel splitting the rest.
			SNew(SSplitter)
			.Orientation(Orient_Horizontal)
			+ SSplitter::Slot()
			.Value(0.18f)
			.MinSize(180.0f)
			[
				SAssignNew(ElementsPanel, SUbstreamElementsPanel)
					.SoloViewport(SoloViewport)
			]
			+ SSplitter::Slot()
			.Value(0.82f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Horizontal)
				+ SSplitter::Slot()
				.Value(0.62f)
				.MinSize(300.0f)
				[
					BuildSoloStage()
				]
				+ SSplitter::Slot()
				.Value(0.38f)
				.MinSize(340.0f)
				[
					SNew(SBorder)
					.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Panel"))
					.Padding(1.0f)
					[
						SAssignNew(ControlPanel, SUbstreamControlPanel)
							.SoloViewport(SoloViewport)
							.OnLoadSelectedRequested(FSimpleDelegate::CreateSP(this, &SUbstreamModelerWidget::HandleLoadSelectedRequested))
					]
				]
			]
		];

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SVerticalBox)
			.Visibility_Lambda([] { return FUbstreamAuthService::Get().IsAuthenticated() ? EVisibility::Visible : EVisibility::Collapsed; })
			+ SVerticalBox::Slot().FillHeight(1)
			[
				// The account panel is part of the workspace, not a popup menu: Slate
				// menus outlive their button, and one left on screen after sign-out
				// hid the sign-in screen. Inline, it leaves with the workspace.
				SNew(SOverlay)
				+ SOverlay::Slot()[Workspace]
				+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Bottom).Padding(12, 0, 12, 6)
				[
					SNew(SBorder).BorderImage(FAppStyle::GetBrush("Menu.Background")).Padding(1)
					.Visibility_Lambda([this] { return bAccountPanelOpen ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SNew(SUbstreamAuthPanel).AccountMode(true)
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(12, 6)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text_Lambda([]
					{
						const auto& Auth = FUbstreamAuthService::Get();
						return FText::FromString(Auth.GetAccount() + (Auth.GetMessage().IsEmpty() ? FString() : TEXT("  ·  ") + Auth.GetMessage()));
					}).AutoWrapText(true)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10)).ColorAndOpacity(FUbstreamModelerStyle::MutedText())
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.ToolTipText(LOCTEXT("AccountSettingsTip", "Account and key"))
					.OnClicked_Lambda([this] { bAccountPanelOpen = !bAccountPanelOpen; return FReply::Handled(); })
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)[SNew(SImage).Image(FAppStyle::GetBrush("Icons.Settings"))]
						+ SHorizontalBox::Slot().AutoWidth()[SNew(STextBlock).Text(LOCTEXT("AccountSettings", "Settings"))]
					]
				]
			]
		]
		+ SOverlay::Slot()
		[
			SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FUbstreamModelerStyle::AppBackground()).Padding(32)
			.Visibility_Lambda([] { return FUbstreamAuthService::Get().IsAuthenticated() ? EVisibility::Collapsed : EVisibility::Visible; })
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot().HAlign(HAlign_Center).Padding(0, 24)
				[SNew(SUbstreamAuthPanel)]
			]
		]
	];

	// Signing back in starts with the account panel closed.
	FUbstreamAuthService::Get().OnChanged.AddSPLambda(this, [this]
	{
		if (!FUbstreamAuthService::Get().IsAuthenticated() && bAccountPanelOpen)
		{
			UE_LOG(LogUbstreamModeler, Log, TEXT("Closing the account panel after sign-out."));
			bAccountPanelOpen = false;
		}
	});

	if (GetDefault<UUbstreamModelerSettings>()->bAutoActivateModelingModeOnOpen)
	{
		RegisterActiveTimer(
			1.0f,
			FWidgetActiveTimerDelegate::CreateSP(this, &SUbstreamModelerWidget::HandleDeferredModelingModeActivation));
	}

	RegisterActiveTimer(
		0.5f,
		FWidgetActiveTimerDelegate::CreateSP(this, &SUbstreamModelerWidget::HandleSourceStatusActiveTimer));
}

bool SUbstreamModelerWidget::CanCloseWorkspace() const
{
	return ConfirmDiscardStageEdits(LOCTEXT("DiscardOnClose",
		"The stage has edits that were not applied to the source.\n\nDiscard the stage edits and close this workspace?"));
}

bool SUbstreamModelerWidget::LoadSoloMesh(UStaticMesh* StaticMesh)
{
	if (IsValid(StaticMesh) && SoloViewport.IsValid() && !SoloViewport->ConfirmReplaceSubject())
	{
		return false;
	}
	const bool bLoaded = SoloViewport.IsValid() && SoloViewport->SetStaticMesh(StaticMesh);
	SoloStageStatus = bLoaded
		? FString::Printf(TEXT("Capture source: %s"), *SoloViewport->GetSoloMeshLabel())
		: TEXT("Could not load Static Mesh");
	if (!bLoaded)
	{
		NotifyLoadFailure(LOCTEXT("LoadStaticMeshFailedToast", "Could not load the Static Mesh into the isolated stage."));
	}
	return bLoaded;
}

bool SUbstreamModelerWidget::LoadSoloActors(const TArray<TWeakObjectPtr<AActor>>& Actors)
{
	if (!Actors.IsEmpty() && SoloViewport.IsValid() && !SoloViewport->ConfirmReplaceSubject())
	{
		return false;
	}
	const bool bLoaded = SoloViewport.IsValid() && SoloViewport->SetActors(Actors);
	SoloStageStatus = bLoaded
		? FString::Printf(TEXT("Capture source: %s"), *SoloViewport->GetSoloMeshLabel())
		: TEXT("Could not load selected actor or object");
	if (!bLoaded)
	{
		NotifyLoadFailure(LOCTEXT(
			"LoadActorsFailedToast",
			"The selection has no loadable mesh. Actors need Static Mesh or Dynamic Mesh geometry to isolate."));
	}
	return bLoaded;
}

bool SUbstreamModelerWidget::LoadSoloComponents(const TArray<TWeakObjectPtr<UMeshComponent>>& Components)
{
	TArray<UMeshComponent*> ValidComponents;
	TArray<TWeakObjectPtr<AActor>> SourceActors;
	TSet<AActor*> AddedActors;
	TArray<FString> ComponentPaths;
	ValidComponents.Reserve(Components.Num());
	ComponentPaths.Reserve(Components.Num());

	for (const TWeakObjectPtr<UMeshComponent>& ComponentPtr : Components)
	{
		UMeshComponent* Component = ComponentPtr.Get();
		if (!UbstreamMeshComponents::HasRenderableGeometry(Component))
		{
			continue;
		}

		ValidComponents.Add(Component);
		ComponentPaths.Add(Component->GetPathName());

		AActor* Owner = Component->GetOwner();
		if (IsValid(Owner) && !AddedActors.Contains(Owner))
		{
			SourceActors.Add(Owner);
			AddedActors.Add(Owner);
		}
	}

	if (!ValidComponents.IsEmpty() && SoloViewport.IsValid() && !SoloViewport->ConfirmReplaceSubject())
	{
		return false;
	}
	const AActor* SingleOwner = ValidComponents.Num() == 1 ? ValidComponents[0]->GetOwner() : nullptr;
	const FString SubjectLabel = ValidComponents.Num() == 1
		? FString::Printf(TEXT("%s.%s"), SingleOwner ? *SingleOwner->GetActorLabel() : TEXT("Component"), *ValidComponents[0]->GetName())
		: FString::Printf(TEXT("%d selected components"), ValidComponents.Num());
	const FString SubjectPath = FString::Join(ComponentPaths, TEXT(";"));
	const bool bLoaded = SoloViewport.IsValid()
		&& SoloViewport->SetMeshComponents(ValidComponents, SubjectLabel, SubjectPath, SourceActors);
	SoloStageStatus = bLoaded
		? FString::Printf(TEXT("Capture source: %s"), *SoloViewport->GetSoloMeshLabel())
		: TEXT("Could not load selected component");
	if (!bLoaded)
	{
		NotifyLoadFailure(LOCTEXT(
			"LoadComponentsFailedToast",
			"The selected components have no loadable mesh. Components need Static Mesh or Dynamic Mesh geometry to isolate."));
	}
	return bLoaded;
}

TSharedRef<SWidget> SUbstreamModelerWidget::BuildSoloStage()
{
	return SNew(SBorder)
		.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Panel"))
		.Padding(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FUbstreamModelerStyle::PanelBackground())
			.Padding(0.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(12.0f, 6.0f, 8.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("SoloStageTitle", "Isolated Subject"))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.94f, 0.94f, 0.94f, 1.0f)))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
					]
					// Frequent view controls, then source actions; Clear stays separate.
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						BuildStageIconButton(
							LOCTEXT("FocusSoloMeshTooltip", "Frame the isolated subject in the viewport."),
							TEXT("Icons.FrameActor"),
							FOnClicked::CreateSP(this, &SUbstreamModelerWidget::FocusSoloMeshClicked),
							TAttribute<bool>::CreateSP(this, &SUbstreamModelerWidget::IsSoloMeshLoaded))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(2.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SComboButton)
						.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
						.ContentPadding(FMargin(4.0f))
						.ToolTipText(LOCTEXT("CameraViewMenuTooltip", "Choose a camera view for the isolated subject."))
						.IsEnabled(this, &SUbstreamModelerWidget::IsSoloMeshLoaded)
						.OnGetMenuContent(this, &SUbstreamModelerWidget::BuildCameraViewMenu)
						.ButtonContent()
						[
							SNew(SBox)
							.WidthOverride(16.0f)
							.HeightOverride(16.0f)
							[
								SNew(SImage)
								.Image(FAppStyle::GetBrush("ViewportToolbar.ExactCameraView"))
								.ColorAndOpacity(FSlateColor::UseForeground())
							]
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(2.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SComboButton)
						.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
						.ContentPadding(FMargin(4.0f))
						.ToolTipText(LOCTEXT("LightingMenuTooltip", "Lights: adjust the isolated stage and capture lighting."))
						.IsEnabled(this, &SUbstreamModelerWidget::IsSoloMeshLoaded)
						.Tag(TEXT("StageLights"))
						.OnGetMenuContent(this, &SUbstreamModelerWidget::BuildLightingMenu)
						.OnMenuOpenChanged_Lambda([this](bool bOpen) { if (!bOpen) SoloViewport->SaveStageLighting(); })
						.ButtonContent()
						[
							SNew(SBox).WidthOverride(16.0f).HeightOverride(16.0f)
							[
								SNew(SImage).Image(FAppStyle::GetBrush("ClassIcon.DirectionalLightComponent"))
								.ColorAndOpacity(FSlateColor::UseForeground())
							]
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(2.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SComboButton)
						.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
						.ContentPadding(FMargin(4.0f))
						.ToolTipText(LOCTEXT("DisplayViewMenuTooltip", "Choose viewport display modes and mesh inspection overlays."))
						.IsEnabled(this, &SUbstreamModelerWidget::IsSoloMeshLoaded)
						.OnGetMenuContent(this, &SUbstreamModelerWidget::BuildDisplayViewMenu)
						.ButtonContent()
						[
							SNew(SBox)
							.WidthOverride(16.0f)
							.HeightOverride(16.0f)
							[
								SNew(SImage)
								.Image(FAppStyle::GetBrush("Icons.Visibility"))
								.ColorAndOpacity(FSlateColor::UseForeground())
							]
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						BuildStageIconButton(
							LOCTEXT("LoadSelectedMeshTooltip", "Load the selected Static Mesh asset, component, or actor into the isolated stage."),
							TEXT("Icons.Plus"),
							FOnClicked::CreateSP(this, &SUbstreamModelerWidget::LoadSelectedMeshClicked))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						BuildStageIconButton(
							LOCTEXT("ApplyEditsHeaderTooltip", "Apply to source: bake the live cutters and write stage edits back to the source asset or component. Undoable."),
							TEXT("Icons.Export"),
							FOnClicked::CreateSP(this, &SUbstreamModelerWidget::ApplyEditsClicked),
							TAttribute<bool>::CreateSP(this, &SUbstreamModelerWidget::IsApplyEditsEnabled))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						BuildStageIconButton(
							LOCTEXT("ResyncSourceTooltip", "Refresh the isolated preview clone from its source. Ends stage editing; un-applied edits are discarded (asks first)."),
							TEXT("Icons.Refresh"),
							FOnClicked::CreateSP(this, &SUbstreamModelerWidget::ResyncSourceClicked),
							TAttribute<bool>::CreateSP(this, &SUbstreamModelerWidget::IsResyncSourceEnabled))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						BuildStageIconButton(
							LOCTEXT("ClearSoloMeshTooltip", "Clear the isolated subject stage."),
							TEXT("Icons.Delete"),
							FOnClicked::CreateSP(this, &SUbstreamModelerWidget::ClearSoloMeshClicked),
							TAttribute<bool>::CreateSP(this, &SUbstreamModelerWidget::IsSoloMeshLoaded))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(12.0f, 0.0f, 12.0f, 6.0f)
				[
					SNew(STextBlock)
					.Text(this, &SUbstreamModelerWidget::GetSoloStatusText)
					.ColorAndOpacity(FSlateColor(FLinearColor(0.62f, 0.64f, 0.67f, 1.0f)))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SBox)
					.HeightOverride(2.0f)
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					.HAlign(HAlign_Fill)
					.VAlign(VAlign_Fill)
					[
						SoloViewport.ToSharedRef()
					]
					// A bundled still replaces the hidden live viewport until a mesh is loaded.
					+ SOverlay::Slot()
					[
						SNew(SEmptyStagePreview).Viewport(SoloViewport)
						.Visibility_Lambda([this] { return IsSoloMeshLoaded() ? EVisibility::Collapsed : EVisibility::Visible; })
					]
					// Floating web-editor-style pills over the viewport:
					// cutter shapes first, transform mode right under it.
					+ SOverlay::Slot()
					.HAlign(HAlign_Left)
					.VAlign(VAlign_Top)
					.Padding(12.0f, 12.0f, 0.0f, 0.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.HAlign(HAlign_Left)
						[
							BuildCutterToolbar()
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.HAlign(HAlign_Left)
						.Padding(0.0f, 8.0f, 0.0f, 0.0f)
						[
							BuildTransformModeToolbar()
						]
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Fill)
					.VAlign(VAlign_Bottom)
					.Padding(12.0f, 0.0f, 12.0f, 10.0f)
					[
						SNew(STextBlock)
						.Visibility(EVisibility::HitTestInvisible)
						.Text(this, &SUbstreamModelerWidget::GetCSGStatusText)
						.AutoWrapText(true)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.62f, 0.64f, 0.68f, 0.9f)))
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Fill)
					.VAlign(VAlign_Center)
					.Padding(12.0f)
					[
						SNew(SBox)
						.WidthOverride(this, &SUbstreamModelerWidget::GetSoloPlaceholderWidthOverride)
						.HAlign(HAlign_Center)
						[
							SNew(SVerticalBox)
							.Visibility(this, &SUbstreamModelerWidget::GetSoloPlaceholderVisibility)
							+ SVerticalBox::Slot()
							.AutoHeight()
							.HAlign(HAlign_Fill)
							[
								SNew(STextBlock)
								.Text(this, &SUbstreamModelerWidget::GetSoloPlaceholderTitleText)
								.Justification(ETextJustify::Center)
								.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
								.ColorAndOpacity(FSlateColor(FLinearColor(0.86f, 0.88f, 0.90f, 1.0f)))
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 18))
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.0f, 8.0f, 0.0f, 0.0f)
							.HAlign(HAlign_Fill)
							[
								SNew(STextBlock)
								.Text(this, &SUbstreamModelerWidget::GetSoloPlaceholderBodyText)
								.Justification(ETextJustify::Center)
								.AutoWrapText(true)
								.WrapTextAt(this, &SUbstreamModelerWidget::GetSoloPlaceholderWrapWidth)
								.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
								.ColorAndOpacity(FSlateColor(FLinearColor(0.58f, 0.60f, 0.63f, 1.0f)))
							]
						]
					]
					// Status badges live top-right so the top-left column stays
					// reserved for the interactive pills.
					+ SOverlay::Slot()
					.HAlign(HAlign_Right)
					.VAlign(VAlign_Top)
					.Padding(0.0f, 12.0f, 12.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						.Visibility_Lambda([this]()
						{
							return SoloViewport.IsValid() && SoloViewport->HasSoloMesh()
								? EVisibility::HitTestInvisible
								: EVisibility::Collapsed;
						})
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
							.BorderBackgroundColor(this, &SUbstreamModelerWidget::GetSourceBadgeColor)
							.Padding(FMargin(8.0f, 4.0f))
							[
								SNew(STextBlock)
								.Text(this, &SUbstreamModelerWidget::GetSourceBadgeText)
								.ColorAndOpacity(FSlateColor(FLinearColor::White))
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(6.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
							.BorderBackgroundColor(this, &SUbstreamModelerWidget::GetModelingModeBadgeColor)
							.Padding(FMargin(8.0f, 4.0f))
							[
								SNew(STextBlock)
								.Text(this, &SUbstreamModelerWidget::GetModelingModeBadgeText)
								.ColorAndOpacity(FSlateColor(FLinearColor::White))
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
							]
						]
					]

				]
			]
		];
}

TSharedRef<SWidget> SUbstreamModelerWidget::BuildCameraViewMenu()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Menu.Background"))
		.Padding(4.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildCameraPresetButton(
					LOCTEXT("CameraIsoLabel", "ISO"),
					LOCTEXT("CameraIsoTooltip", "Frame the isolated subject from an orbit-friendly perspective."),
					FOnClicked::CreateSP(this, &SUbstreamModelerWidget::CameraViewClicked, EUbstreamStageCameraView::Iso))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildCameraPresetButton(
					LOCTEXT("CameraFrontLabel", "Front"),
					LOCTEXT("CameraFrontTooltip", "Frame the isolated subject from the front."),
					FOnClicked::CreateSP(this, &SUbstreamModelerWidget::CameraViewClicked, EUbstreamStageCameraView::Front))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildCameraPresetButton(
					LOCTEXT("CameraRightLabel", "Right"),
					LOCTEXT("CameraRightTooltip", "Frame the isolated subject from the right side."),
					FOnClicked::CreateSP(this, &SUbstreamModelerWidget::CameraViewClicked, EUbstreamStageCameraView::Right))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildCameraPresetButton(
					LOCTEXT("CameraTopLabel", "Top"),
					LOCTEXT("CameraTopTooltip", "Frame the isolated subject from above."),
					FOnClicked::CreateSP(this, &SUbstreamModelerWidget::CameraViewClicked, EUbstreamStageCameraView::Top))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				BuildDisplayMenuCheckRow(
					LOCTEXT("OrbitCameraLabel", "Orbit Camera"),
					LOCTEXT("OrbitCameraTooltip", "Orbit around the subject instead of free-flying. Alt+drag always orbits."),
					TAttribute<ECheckBoxState>::CreateSP(this, &SUbstreamModelerWidget::GetOrbitCameraCheckState),
					FOnCheckStateChanged::CreateSP(this, &SUbstreamModelerWidget::HandleOrbitCameraChanged))
			]
		];
}

namespace
{
	constexpr int32 MaxRecentLightColors = 5;
	const TCHAR* const RecentLightColorsSection = TEXT("UnboundLoop.StageLighting");
	const TCHAR* const RecentMainLightColorsKey = TEXT("RecentMainColors");
	const TCHAR* const RecentAmbientLightColorsKey = TEXT("RecentAmbientColors");
}

void SUbstreamModelerWidget::LoadRecentLightColors()
{
	const auto Load = [](const TCHAR* Key, TArray<FLinearColor>& OutColors)
	{
		TArray<FString> Entries;
		GConfig->GetArray(RecentLightColorsSection, Key, Entries, GEditorPerProjectIni);
		OutColors.Reset();
		for (const FString& Entry : Entries)
		{
			FLinearColor Color;
			if (OutColors.Num() < MaxRecentLightColors && Color.InitFromString(Entry)) OutColors.Add(Color);
		}
	};
	Load(RecentMainLightColorsKey, RecentMainLightColors);
	Load(RecentAmbientLightColorsKey, RecentAmbientLightColors);
}

void SUbstreamModelerWidget::RememberLightColor(TArray<FLinearColor> SUbstreamModelerWidget::* Recents, const TCHAR* ConfigKey, const FLinearColor& Color)
{
	// Most recent first; re-picking an existing color moves it to the front.
	TArray<FLinearColor>& Colors = this->*Recents;
	const FColor Quantized = Color.ToFColor(true);
	Colors.RemoveAll([Quantized](const FLinearColor& Existing) { return Existing.ToFColor(true) == Quantized; });
	Colors.Insert(Color, 0);
	if (Colors.Num() > MaxRecentLightColors) Colors.SetNum(MaxRecentLightColors);

	TArray<FString> Entries;
	for (const FLinearColor& Entry : Colors) Entries.Add(Entry.ToString());
	GConfig->SetArray(RecentLightColorsSection, ConfigKey, Entries, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

TSharedRef<SWidget> SUbstreamModelerWidget::BuildLightingMenu()
{
	const auto NotifyChanged = [this]()
	{
		if (ControlPanel.IsValid()) ControlPanel->NotifyStageContentDirty();
	};
	const auto Save = [this]() { SoloViewport->SaveStageLighting(); };
	const auto Update = [this, NotifyChanged](TFunctionRef<void(FUbstreamStageLighting&)> Edit)
	{
		FUbstreamStageLighting Settings = SoloViewport->GetStageLighting();
		Edit(Settings);
		SoloViewport->SetStageLighting(Settings);
		NotifyChanged();
	};
	const auto ApplyPreset = [this, NotifyChanged, Save](const FUbstreamStageLighting& Settings)
	{
		SoloViewport->SetStageLighting(Settings);
		NotifyChanged();
		Save();
		return FReply::Handled();
	};
	const auto PresetButton = [ApplyPreset](const FText& Label, const FText& Tooltip, FUbstreamStageLighting Settings)
	{
		return SNew(SButton)
			.ToolTipText(Tooltip)
			.HAlign(HAlign_Center)
			.OnClicked_Lambda([ApplyPreset, Settings]() { return ApplyPreset(Settings); })
			[
				SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
			];
	};
	const auto SectionHeader = [](const FText& Label)
	{
		return SNew(STextBlock).Text(Label)
			.ColorAndOpacity(FUbstreamModelerStyle::MutedText())
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9));
	};
	const auto SliderRow = [this, Update, Save](const FText& Label, const FText& Tooltip,
		float FUbstreamStageLighting::* Member, float MinValue, float MaxValue, bool bDegrees)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[SNew(STextBlock).Text(Label).ToolTipText(Tooltip)]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock).ColorAndOpacity(FUbstreamModelerStyle::MutedText())
					.Text_Lambda([this, Member, bDegrees]()
					{
						const float Value = SoloViewport->GetStageLighting().*Member;
						return bDegrees
							? FText::Format(LOCTEXT("LightDegrees", "{0}°"), FText::AsNumber(FMath::RoundToInt(Value)))
							: FText::AsPercent(Value);
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SSlider).ToolTipText(Tooltip)
				.Style(&FUbstreamModelerStyle::Get().GetWidgetStyle<FSliderStyle>("Ubstream.Slider"))
				.MinValue(MinValue).MaxValue(MaxValue).StepSize(bDegrees ? 1.0f : 0.01f)
				.Value_Lambda([this, Member]() { return SoloViewport->GetStageLighting().*Member; })
				.OnValueChanged_Lambda([Update, Member](float Value)
				{
					Update([Member, Value](FUbstreamStageLighting& Settings) { Settings.*Member = Value; });
				})
				.OnMouseCaptureEnd_Lambda(Save)
				.OnControllerCaptureEnd_Lambda(Save)
			];
	};

	const auto IsSameTint = [](const FLinearColor& A, const FLinearColor& B)
	{
		return A.ToFColor(true) == B.ToFColor(true);
	};
	const auto ColorRow = [this, Update, Save, IsSameTint](const FText& Tooltip, FLinearColor FUbstreamStageLighting::* Member,
		TArray<FLinearColor> SUbstreamModelerWidget::* Recents, const TCHAR* RecentsKey)
	{
		const FSlateBrush* SelectedFrame = FUbstreamModelerStyle::Get().GetBrush("Ubstream.Swatch.Selected");
		const FSlateBrush* IdleFrame = FUbstreamModelerStyle::Get().GetBrush("Ubstream.Swatch");
		const auto Swatch = [](TAttribute<FLinearColor> Color, TAttribute<const FSlateBrush*> Frame)
		{
			return SNew(SBorder)
				.BorderImage(Frame)
				.Padding(2.0f)
				[
					SNew(SColorBlock)
					.Color(Color)
					.UseSRGB(true)
					.ShowBackgroundForAlpha(false)
					.CornerRadius(FVector4(3.0f, 3.0f, 3.0f, 3.0f))
					.Size(FVector2D(18.0f, 18.0f))
				];
		};

		// Recent slots are fixed and bound by index, so a color confirmed
		// while this popup is open appears without rebuilding the menu.
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox).ToolTipText(Tooltip);
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("NoRecentLightColors", "Picked colors appear here"))
			.ColorAndOpacity(FUbstreamModelerStyle::MutedText())
			.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
			.Visibility_Lambda([this, Recents]() { return (this->*Recents).IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
		];
		for (int32 Index = 0; Index < MaxRecentLightColors; ++Index)
		{
			const auto RecentColor = [this, Recents, Index]()
			{
				const TArray<FLinearColor>& Colors = this->*Recents;
				return Colors.IsValidIndex(Index) ? Colors[Index] : FLinearColor::White;
			};
			Row->AddSlot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "NoBorder")
				.ContentPadding(0.0f)
				.ToolTipText(LOCTEXT("RecentLightColorTip", "Recently picked color"))
				.Visibility_Lambda([this, Recents, Index]() { return (this->*Recents).IsValidIndex(Index) ? EVisibility::Visible : EVisibility::Collapsed; })
				.OnClicked_Lambda([Update, Save, Member, RecentColor]()
				{
					const FLinearColor Color = RecentColor();
					Update([Member, Color](FUbstreamStageLighting& Settings) { Settings.*Member = Color; });
					Save();
					return FReply::Handled();
				})
				[
					Swatch(TAttribute<FLinearColor>::CreateLambda(RecentColor),
						TAttribute<const FSlateBrush*>::CreateLambda([this, Member, RecentColor, IsSameTint, SelectedFrame, IdleFrame]()
						{
							return IsSameTint(SoloViewport->GetStageLighting().*Member, RecentColor()) ? SelectedFrame : IdleFrame;
						}))
				]
			];
		}

		// The current color opens the engine picker as a submenu so the
		// Lights popup stays open while dragging the wheel.
		TSharedRef<SButton> PickerButton = SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "NoBorder")
			.ContentPadding(0.0f)
			.ToolTipText(LOCTEXT("PickLightColorTip", "Pick a color. Confirmed colors are added to the recent list."));
		PickerButton->SetContent(
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				Swatch(TAttribute<FLinearColor>::CreateLambda([this, Member]() { return SoloViewport->GetStageLighting().*Member; }), IdleFrame)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.EyeDropper"))
				.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]);
		TWeakPtr<SButton> WeakPickerButton = PickerButton;
		PickerButton->SetOnClicked(FOnClicked::CreateLambda([this, Update, Save, Member, Recents, RecentsKey, WeakPickerButton]()
		{
			// Cancel restores the original and fires before the close event,
			// so anything else that closes the picker counts as accepted.
			const TSharedRef<bool> bCancelled = MakeShared<bool>(false);
			FColorPickerArgs Args;
			Args.bOpenAsMenu = true;
			Args.bUseAlpha = false;
			Args.bClampValue = true;
			Args.bExpandAdvancedSection = false;
			Args.sRGBOverride = true;
			Args.ParentWidget = WeakPickerButton.Pin();
			Args.InitialColor = SoloViewport->GetStageLighting().*Member;
			Args.OnColorCommitted = FOnLinearColorValueChanged::CreateLambda([Update, Member](FLinearColor Color)
			{
				Update([Member, Color](FUbstreamStageLighting& Settings) { Settings.*Member = Color; });
			});
			Args.OnColorPickerCancelled = FOnColorPickerCancelled::CreateLambda([Update, Member, bCancelled](FLinearColor Original)
			{
				*bCancelled = true;
				Update([Member, Original](FUbstreamStageLighting& Settings) { Settings.*Member = Original; });
			});
			TWeakPtr<SUbstreamModelerWidget> WeakThis = StaticCastSharedRef<SUbstreamModelerWidget>(AsShared());
			Args.OnColorPickerWindowClosed = FOnWindowClosed::CreateLambda([WeakThis, Save, Member, Recents, RecentsKey, bCancelled](const TSharedRef<SWindow>&)
			{
				const TSharedPtr<SUbstreamModelerWidget> Self = WeakThis.Pin();
				if (!Self.IsValid() || !Self->SoloViewport.IsValid()) return;
				Save();
				if (!*bCancelled) Self->RememberLightColor(Recents, RecentsKey, Self->SoloViewport->GetStageLighting().*Member);
			});
			OpenColorPicker(Args);
			return FReply::Handled();
		}));
		Row->AddSlot().FillWidth(1.0f).HAlign(HAlign_Right)[PickerButton];
		return Row;
	};

	const FUbstreamStageLighting Inspect;
	FUbstreamStageLighting Studio;
	Studio.AmbientFill = 1.0f;
	Studio.MainStrength = 1.0f;
	Studio.Rotation = 45.0f;
	Studio.Elevation = -40.0f;
	Studio.bFollowCamera = false;
	FUbstreamStageLighting Dramatic;
	Dramatic.AmbientFill = 0.3f;
	Dramatic.MainStrength = 1.2f;
	Dramatic.Rotation = 110.0f;
	Dramatic.Elevation = -25.0f;
	Dramatic.bFollowCamera = false;
	// Warm key against a cool fill: classic contrast that reads shape well.
	Dramatic.MainColor = FLinearColor::MakeFromColorTemperature(3600.0f).GetClamped(0.0f, 1.0f);
	Dramatic.AmbientColor = FLinearColor::MakeFromColorTemperature(9000.0f).GetClamped(0.0f, 1.0f);

	return SNew(SBox).WidthOverride(300.0f)
		[
			SNew(SBorder).BorderImage(FAppStyle::GetBrush("NoBorder")).Padding(12.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[SNew(STextBlock).Text(LOCTEXT("StageLightsTitle", "Lights")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SButton)
						.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
						.ToolTipText(LOCTEXT("ResetLightsTip", "Restore the default camera-following light, ambient fill and neutral colors."))
						.OnClicked_Lambda([ApplyPreset]() { return ApplyPreset(FUbstreamStageLighting()); })
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("Icons.Refresh"))
							.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 4.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[PresetButton(LOCTEXT("InspectPreset", "Inspect"), LOCTEXT("InspectPresetTip", "Default: camera-following light with ambient fill, keeping surfaces readable as you orbit."), Inspect)]
					+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(4.0f, 0.0f)
					[PresetButton(LOCTEXT("StudioPreset", "Studio"), LOCTEXT("StudioPresetTip", "Balanced, fixed, neutral lighting for presentation and material review."), Studio)]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[PresetButton(LOCTEXT("DramaticPreset", "Dramatic"), LOCTEXT("DramaticPresetTip", "Warm side key over a cool fill, with deeper shadows."), Dramatic)]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 6.0f)
				[SectionHeader(LOCTEXT("MainLightSection", "MAIN LIGHT"))]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 6.0f)
				[ColorRow(LOCTEXT("MainColorTip", "Tint the main light. Highlights and lit faces take this color."), &FUbstreamStageLighting::MainColor, &SUbstreamModelerWidget::RecentMainLightColors, RecentMainLightColorsKey)]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
				[SliderRow(LOCTEXT("MainLight", "Strength"), LOCTEXT("MainLightTip", "Control the strength of highlights and directional shading. 100% matches the Studio preset."), &FUbstreamStageLighting::MainStrength, 0.0f, 2.0f, false)]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
				[SliderRow(LOCTEXT("LightRotation", "Rotation"), LOCTEXT("LightRotationTip", "Rotate the main light around the subject, or around the view when Follow camera is enabled."), &FUbstreamStageLighting::Rotation, 0.0f, 359.0f, true)]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
				[SliderRow(LOCTEXT("LightElevation", "Pitch"), LOCTEXT("LightElevationTip", "Tilt the main light. Negative values light from above, positive from below."), &FUbstreamStageLighting::Elevation, -89.0f, 89.0f, true)]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.ToolTipText(LOCTEXT("FollowLightTip", "Place the light above and beside the camera and follow its movement. Turn off to hold the current light direction."))
					.IsChecked_Lambda([this]() { return SoloViewport->GetStageLighting().bFollowCamera ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this, NotifyChanged, Save](ECheckBoxState State)
					{
						SoloViewport->SetLightFollowsCamera(State == ECheckBoxState::Checked);
						NotifyChanged();
						Save();
					})
					[SNew(STextBlock).Text(LOCTEXT("FollowCameraLight", "Follow camera"))]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 6.0f)
				[SectionHeader(LOCTEXT("AmbientSection", "AMBIENT"))]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 6.0f)
				[ColorRow(LOCTEXT("AmbientColorTip", "Tint the ambient fill. Shadowed faces take this color."), &FUbstreamStageLighting::AmbientColor, &SUbstreamModelerWidget::RecentAmbientLightColors, RecentAmbientLightColorsKey)]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
				[SliderRow(LOCTEXT("AmbientFill", "Fill"), LOCTEXT("AmbientFillTip", "Lift dark faces and reduce contrast. 100% matches the Studio preset."), &FUbstreamStageLighting::AmbientFill, 0.0f, 2.0f, false)]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).AutoWrapText(true)
					.ColorAndOpacity(FUbstreamModelerStyle::MutedText()).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.Text(LOCTEXT("LightingScope", "Affects the stage and captured color. Use Lit view to see the result."))
				]
			]
		];
}

TSharedRef<SWidget> SUbstreamModelerWidget::BuildDisplayViewMenu()
{
	auto BuildViewModeRow = [this](const FText& Label, const FText& ToolTip, EViewModeIndex ViewMode)
	{
		return BuildDisplayMenuCheckRow(
			Label,
			ToolTip,
			TAttribute<ECheckBoxState>::Create(
				TAttribute<ECheckBoxState>::FGetter::CreateSP(
					this,
					&SUbstreamModelerWidget::GetDisplayViewModeCheckState,
					static_cast<int32>(ViewMode))),
			FOnCheckStateChanged::CreateSP(
				this,
				&SUbstreamModelerWidget::HandleDisplayViewModeChanged,
				static_cast<int32>(ViewMode)));
	};

	auto BuildOverlayRow = [this](const FText& Label, const FText& ToolTip, FUbstreamViewportClient::EOverlay Overlay)
	{
		return BuildDisplayMenuCheckRow(
			Label,
			ToolTip,
			TAttribute<ECheckBoxState>::Create(
				TAttribute<ECheckBoxState>::FGetter::CreateSP(
					this,
					&SUbstreamModelerWidget::GetDisplayOverlayCheckState,
					static_cast<int32>(Overlay))),
			FOnCheckStateChanged::CreateSP(
				this,
				&SUbstreamModelerWidget::HandleDisplayOverlayChanged,
				static_cast<int32>(Overlay)));
	};

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Menu.Background"))
		.Padding(4.0f)
		[
			SNew(SBox)
			.WidthOverride(210.0f)
			[
			SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildDisplayMenuHeader(LOCTEXT("DisplayViewModesHeader", "View Mode"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildViewModeRow(
						LOCTEXT("DisplayLitLabel", "Lit"),
						LOCTEXT("DisplayLitTooltip", "Use the standard lit material view."),
						VMI_Lit)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildViewModeRow(
						LOCTEXT("DisplayUnlitLabel", "Unlit"),
						LOCTEXT("DisplayUnlitTooltip", "Show base color without scene lighting."),
						VMI_Unlit)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildViewModeRow(
						LOCTEXT("DisplayWireframeLabel", "Wireframe"),
						LOCTEXT("DisplayWireframeTooltip", "Show the mesh as pure wireframe."),
						VMI_Wireframe)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildViewModeRow(
						LOCTEXT("DisplayLitWireframeLabel", "Lit Wireframe"),
						LOCTEXT("DisplayLitWireframeTooltip", "Show materials with mesh edges overlaid."),
						VMI_Lit_Wireframe)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildViewModeRow(
						LOCTEXT("DisplayDetailLightingLabel", "Detail Lighting"),
						LOCTEXT("DisplayDetailLightingTooltip", "Inspect surface normals and lighting response without material color."),
						VMI_Lit_DetailLighting)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildViewModeRow(
						LOCTEXT("DisplayLightingOnlyLabel", "Lighting Only"),
						LOCTEXT("DisplayLightingOnlyTooltip", "Inspect lighting without material detail."),
						VMI_LightingOnly)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildDisplayMenuHeader(LOCTEXT("DisplayOverlaysHeader", "Overlays"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildOverlayRow(
						LOCTEXT("DisplayGridLabel", "Grid"),
						LOCTEXT("DisplayGridTooltip", "Show the editor grid in the isolated viewport."),
						FUbstreamViewportClient::EOverlay::Grid)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildOverlayRow(
						LOCTEXT("DisplayBoundsLabel", "Bounds"),
						LOCTEXT("DisplayBoundsTooltip", "Show component bounds around the isolated subject."),
						FUbstreamViewportClient::EOverlay::Bounds)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildOverlayRow(
						LOCTEXT("DisplayNormalsLabel", "Normals"),
						LOCTEXT("DisplayNormalsTooltip", "Show surface normal vectors in green."),
						FUbstreamViewportClient::EOverlay::Normals)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildOverlayRow(
						LOCTEXT("DisplayTangentsLabel", "Tangents"),
						LOCTEXT("DisplayTangentsTooltip", "Show tangent vectors in red."),
						FUbstreamViewportClient::EOverlay::Tangents)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildOverlayRow(
						LOCTEXT("DisplayBinormalsLabel", "Binormals"),
						LOCTEXT("DisplayBinormalsTooltip", "Show binormal vectors in blue."),
						FUbstreamViewportClient::EOverlay::Binormals)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildOverlayRow(
						LOCTEXT("DisplayVerticesLabel", "Vertices"),
						LOCTEXT("DisplayVerticesTooltip", "Show sampled mesh vertices as green points."),
						FUbstreamViewportClient::EOverlay::Vertices)
				]
			]
		];
}

TSharedRef<SWidget> SUbstreamModelerWidget::BuildTransformModeToolbar()
{
	const auto MakeModeToggle = [this](const FName IconName, const FText& ToolTip, const EUbstreamCSGTransformMode Mode)
	{
		return SNew(SCheckBox)
			.Style(&FUbstreamModelerStyle::Get(), "Ubstream.IconToggle")
			.ToolTipText(ToolTip)
			.IsChecked(this, &SUbstreamModelerWidget::GetCSGTransformModeCheckState, Mode)
			.OnCheckStateChanged(this, &SUbstreamModelerWidget::HandleCSGTransformModeChanged, Mode)
			[
				SNew(SBox)
				.WidthOverride(16.0f)
				.HeightOverride(16.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush(IconName))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			];
	};

	return SNew(SBorder)
		.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Toolbar.Floating"))
		.Padding(FMargin(4.0f))
		.Visibility_Lambda([this]()
		{
			return SoloViewport.IsValid()
				&& SoloViewport->HasSelectedCSGPrimitive()
				? EVisibility::Visible
				: EVisibility::Collapsed;
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				MakeModeToggle(TEXT("EditorViewport.TranslateMode"), LOCTEXT("MoveModeTooltip", "Move (W)"), EUbstreamCSGTransformMode::Move)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				MakeModeToggle(TEXT("EditorViewport.RotateMode"), LOCTEXT("RotateModeTooltip", "Rotate (E)"), EUbstreamCSGTransformMode::Rotate)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				MakeModeToggle(TEXT("EditorViewport.ScaleMode"), LOCTEXT("ScaleModeTooltip", "Scale (R)"), EUbstreamCSGTransformMode::Scale)
			]
		];
}

TSharedRef<SWidget> SUbstreamModelerWidget::BuildCutterToolbar()
{
	const auto MakeShapeButton = [this](const FName IconName, const FText& ToolTip, const EUbstreamCSGPrimitiveType Type)
	{
		return SNew(SButton)
			.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
			.ToolTipText(ToolTip)
			.IsEnabled(this, &SUbstreamModelerWidget::IsAddCSGPrimitiveEnabled)
			.OnClicked(this, &SUbstreamModelerWidget::AddCSGPrimitiveClicked, Type)
			[
				SNew(SBox)
				.WidthOverride(16.0f)
				.HeightOverride(16.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush(IconName))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			];
	};

	return SNew(SBorder)
		.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Toolbar.Floating"))
		.Padding(FMargin(6.0f, 4.0f))
		.Visibility_Lambda([this]()
		{
			return SoloViewport.IsValid() && SoloViewport->HasSoloMesh()
				? EVisibility::Visible
				: EVisibility::Collapsed;
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("CutterToolbarLabel", "Cutter"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				MakeShapeButton(TEXT("Icons.box-perspective"), LOCTEXT("AddBoxTooltip", "Add a box cutter - the live cut starts immediately. Ctrl+D repeats."), EUbstreamCSGPrimitiveType::Box)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				MakeShapeButton(TEXT("Icons.sphere"), LOCTEXT("AddSphereTooltip", "Add a sphere cutter - the live cut starts immediately."), EUbstreamCSGPrimitiveType::Sphere)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				MakeShapeButton(TEXT("Icons.cylinder"), LOCTEXT("AddCylinderTooltip", "Add a cylinder cutter - the live cut starts immediately."), EUbstreamCSGPrimitiveType::Cylinder)
			]
		];
}

void SUbstreamModelerWidget::HandleLoadSelectedRequested()
{
	LoadSelectedSubject();
}

FReply SUbstreamModelerWidget::LoadSelectedMeshClicked()
{
	LoadSelectedSubject();
	return FReply::Handled();
}

FReply SUbstreamModelerWidget::ApplyEditsClicked()
{
	if (!SoloViewport.IsValid())
	{
		return FReply::Handled();
	}

	FString Message;
	if (SoloViewport->ApplyEditsToSource(Message))
	{
		SoloStageStatus = TEXT("Applied stage edits to source");
		NotifyStage(
			Message.IsEmpty()
				? LOCTEXT("AppliedEditsToast", "Stage edits applied to the source.")
				: FText::FromString(Message),
			/*bIsError*/ false);
	}
	else
	{
		SoloStageStatus = FString::Printf(TEXT("Apply failed: %s"), *Message);
		NotifyStage(FText::FromString(Message), /*bIsError*/ true);
	}

	return FReply::Handled();
}

FReply SUbstreamModelerWidget::AddCSGPrimitiveClicked(EUbstreamCSGPrimitiveType PrimitiveType)
{
	if (!SoloViewport.IsValid())
	{
		return FReply::Handled();
	}

	// Immediate placement: the cutter lands half-embedded on the model's
	// largest face and the live subtract starts right away. Clicking the
	// model DURING the preview jumps the cut to the clicked point.
	if (SoloViewport->AddCSGPrimitive(PrimitiveType))
	{
		if (SoloViewport->StartCSGOperation(EUbstreamCSGOperation::Subtract))
		{
			SoloStageStatus = TEXT("Live subtract preview active");
			NotifyStage(LOCTEXT("AddedCSGPrimitiveLiveToast", "Cutter added - live subtract running. Drag the ghost (or click the model) to move the cut; Enter applies, Esc cancels."), /*bIsError*/ false);
		}
		else
		{
			SoloStageStatus = TEXT("Added and selected CSG cutter");
		}
	}
	else
	{
		SoloStageStatus = TEXT("Could not add CSG cutter");
		NotifyStage(LOCTEXT("AddCSGPrimitiveFailedToast", "Could not add a CSG cutter. Load exactly one source mesh first."), /*bIsError*/ true);
	}

	return FReply::Handled();
}

FReply SUbstreamModelerWidget::CSGTransformModeClicked(const EUbstreamCSGTransformMode TransformMode)
{
	if (!SoloViewport.IsValid())
	{
		return FReply::Handled();
	}

	if (SoloViewport->SetCSGTransformMode(TransformMode))
	{
		switch (TransformMode)
		{
		case EUbstreamCSGTransformMode::Rotate:
			SoloStageStatus = TEXT("Rotate CSG cutter");
			break;
		case EUbstreamCSGTransformMode::Scale:
			SoloStageStatus = TEXT("Scale CSG cutter");
			break;
		case EUbstreamCSGTransformMode::Move:
		default:
			SoloStageStatus = TEXT("Move CSG cutter");
			break;
		}
	}
	else
	{
		SoloStageStatus = TEXT("No CSG cutter selected");
		NotifyStage(LOCTEXT("CSGTransformModeFailedToast", "Add a CSG cutter first, then move, rotate, or scale it with the viewport widget."), /*bIsError*/ true);
	}

	return FReply::Handled();
}

void SUbstreamModelerWidget::HandleCSGTransformModeChanged(
	const ECheckBoxState NewState,
	const EUbstreamCSGTransformMode TransformMode)
{
	if (NewState == ECheckBoxState::Checked)
	{
		CSGTransformModeClicked(TransformMode);
	}
}

bool SUbstreamModelerWidget::IsApplyEditsEnabled() const
{
	return SoloViewport.IsValid() && SoloViewport->HasUncommittedStageEdits();
}

bool SUbstreamModelerWidget::IsAddCSGPrimitiveEnabled() const
{
	return SoloViewport.IsValid()
		&& SoloViewport->CanAddCSGPrimitive();
}

bool SUbstreamModelerWidget::IsDeleteSelectedCSGPrimitiveEnabled() const
{
	return SoloViewport.IsValid()
		&& SoloViewport->HasSelectedCSGPrimitive()
		&& !SoloViewport->HasAnyActiveStageTool();
}

bool SUbstreamModelerWidget::IsClearCSGPrimitivesEnabled() const
{
	return SoloViewport.IsValid()
		&& SoloViewport->GetCSGPrimitiveCount() > 0
		&& !SoloViewport->HasAnyActiveStageTool();
}

bool SUbstreamModelerWidget::IsCSGTransformActionEnabled() const
{
	return SoloViewport.IsValid()
		&& SoloViewport->HasSelectedCSGPrimitive()
		&& !SoloViewport->HasAnyActiveStageTool();
}

bool SUbstreamModelerWidget::IsCSGToolActionEnabled() const
{
	return SoloViewport.IsValid() && SoloViewport->CanStartCSGOperation();
}

ECheckBoxState SUbstreamModelerWidget::GetCSGTransformModeCheckState(const EUbstreamCSGTransformMode TransformMode) const
{
	return SoloViewport.IsValid() && SoloViewport->GetCSGTransformMode() == TransformMode
		? ECheckBoxState::Checked
		: ECheckBoxState::Unchecked;
}

EVisibility SUbstreamModelerWidget::GetCSGWorkflowVisibility() const
{
	return SoloViewport.IsValid() && SoloViewport->HasSoloMesh()
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FText SUbstreamModelerWidget::GetCSGStatusText() const
{
	if (!SoloViewport.IsValid() || !SoloViewport->HasSoloMesh())
	{
		return FText::GetEmpty();
	}

	if (SoloViewport->HasPendingCSGPlacement())
	{
		return LOCTEXT("PendingCSGPlacementStatus", "Click on the model to place the cutter - Esc cancels.");
	}

	if (SoloViewport->HasActiveCSGSession())
	{
		const FString& SessionMessage = SoloViewport->GetCSGSessionMessage();
		if (!SessionMessage.IsEmpty())
		{
			return FText::FromString(SessionMessage);
		}

		if (SoloViewport->IsCSGSessionComputing())
		{
			return LOCTEXT("CSGStatusComputing", "Computing cuts...");
		}
		return LOCTEXT("CSGStatusLive", "Bake selected cutter or Bake all cutters merges cuts into the stage mesh. Both are undoable.");

	}

	if (SoloViewport->HasAnyActiveStageTool())
	{
		return LOCTEXT("CSGStatusToolActive", "Tool preview active. Accept applies the result; Cancel returns to cutter editing.");
	}

	if (!SoloViewport->HasEditableSource())
	{
		return LOCTEXT("CSGStatusNoEditableSource", "Load a source mesh to enable cutter operations.");
	}

	if (SoloViewport->GetSourcePreviewComponentCount() != 1)
	{
		return LOCTEXT("CSGStatusNeedsSingleSource", "CSG uses exactly one imported source mesh.");
	}

	const int32 CutterCount = SoloViewport->GetCSGPrimitiveCount();
	if (CutterCount == 0)
	{
		return LOCTEXT("CSGStatusNoCutters", "Add a cutter - the live cut starts immediately. Drag or click the model to move it, W/E/R transform, Ctrl+D repeats, Enter bakes all cutters.");
	}

	if (!SoloViewport->HasSelectedCSGPrimitive())
	{
		return LOCTEXT("CSGStatusNoSelectedCutter", "Select a generated cutter to transform it; operations fall back to the latest cutter.");
	}

	if (CutterCount == 1)
	{
		return LOCTEXT("CSGStatusOneCutter", "1 cutter selected. Transform it, choose an operation, then Bake selected cutter. Undo restores the cutter.");
	}

	const int32 SelectedIndex = SoloViewport->GetSelectedCSGPrimitiveIndex();
	if (SelectedIndex != INDEX_NONE)
	{
		return FText::Format(
			LOCTEXT("CSGStatusManyCuttersSelected", "{0}/{1} cutter selected. Click a cutter or use Next/Tab, then transform and apply an operation."),
			FText::AsNumber(SelectedIndex + 1),
			FText::AsNumber(CutterCount));
	}

	return FText::Format(
		LOCTEXT("CSGStatusManyCutters", "{0} cutters staged. Select one with click or Next/Tab before applying an operation."),
		FText::AsNumber(CutterCount));
}

void SUbstreamModelerWidget::HandleEditingStateChanged()
{
	if (ElementsPanel.IsValid())
	{
		ElementsPanel->Refresh();
	}
	if (ControlPanel.IsValid())
	{
		ControlPanel->NotifyCaptureWorkspaceChanged();
	}

	Invalidate(EInvalidateWidgetReason::LayoutAndVolatility);
}

FReply SUbstreamModelerWidget::FocusSoloMeshClicked()
{
	if (SoloViewport.IsValid() && SoloViewport->FocusSoloMesh())
	{
		SoloStageStatus = FString::Printf(TEXT("Framed: %s"), *SoloViewport->GetSoloMeshLabel());
		if (ControlPanel.IsValid())
		{
			ControlPanel->NotifyCaptureWorkspaceChanged();
		}
	}
	else
	{
		SoloStageStatus = TEXT("Load a subject before framing");
	}
	return FReply::Handled();
}

FReply SUbstreamModelerWidget::CameraViewClicked(EUbstreamStageCameraView CameraView)
{
	FSlateApplication::Get().DismissAllMenus();

	if (SoloViewport.IsValid() && SoloViewport->SetCameraView(CameraView))
	{
		SoloStageStatus = FString::Printf(TEXT("Camera framed: %s"), *SoloViewport->GetSoloMeshLabel());
		if (ControlPanel.IsValid())
		{
			ControlPanel->NotifyCaptureWorkspaceChanged();
		}
	}
	else
	{
		SoloStageStatus = TEXT("Load a subject before changing the camera");
	}

	return FReply::Handled();
}

void SUbstreamModelerWidget::HandleDisplayViewModeChanged(ECheckBoxState NewState, int32 ViewMode)
{
	if (NewState != ECheckBoxState::Checked || !SoloViewport.IsValid())
	{
		return;
	}

	const TSharedPtr<FUbstreamViewportClient> ViewportClient = SoloViewport->GetUbstreamViewportClient();
	if (!ViewportClient.IsValid())
	{
		return;
	}

	ViewportClient->SetInspectionViewMode(static_cast<EViewModeIndex>(ViewMode));
	SoloStageStatus = TEXT("Viewport display updated");
}

void SUbstreamModelerWidget::HandleDisplayOverlayChanged(ECheckBoxState NewState, int32 Overlay)
{
	if (!SoloViewport.IsValid())
	{
		return;
	}

	const TSharedPtr<FUbstreamViewportClient> ViewportClient = SoloViewport->GetUbstreamViewportClient();
	if (!ViewportClient.IsValid())
	{
		return;
	}

	const FUbstreamViewportClient::EOverlay OverlayType = static_cast<FUbstreamViewportClient::EOverlay>(Overlay);
	const bool bTargetEnabled = NewState == ECheckBoxState::Checked;
	if (ViewportClient->IsOverlayEnabled(OverlayType) != bTargetEnabled)
	{
		ViewportClient->ToggleOverlay(OverlayType);
		SoloStageStatus = TEXT("Viewport overlay updated");
	}
}

ECheckBoxState SUbstreamModelerWidget::GetDisplayViewModeCheckState(int32 ViewMode) const
{
	if (!SoloViewport.IsValid())
	{
		return ECheckBoxState::Unchecked;
	}

	const TSharedPtr<FUbstreamViewportClient> ViewportClient = SoloViewport->GetUbstreamViewportClient();
	return ViewportClient.IsValid() && ViewportClient->GetInspectionViewMode() == static_cast<EViewModeIndex>(ViewMode)
		? ECheckBoxState::Checked
		: ECheckBoxState::Unchecked;
}

ECheckBoxState SUbstreamModelerWidget::GetDisplayOverlayCheckState(int32 Overlay) const
{
	if (!SoloViewport.IsValid())
	{
		return ECheckBoxState::Unchecked;
	}

	const TSharedPtr<FUbstreamViewportClient> ViewportClient = SoloViewport->GetUbstreamViewportClient();
	return ViewportClient.IsValid() && ViewportClient->IsOverlayEnabled(static_cast<FUbstreamViewportClient::EOverlay>(Overlay))
		? ECheckBoxState::Checked
		: ECheckBoxState::Unchecked;
}

FReply SUbstreamModelerWidget::ResyncSourceClicked()
{
	if (!SoloViewport.IsValid() || !SoloViewport->HasSoloMesh())
	{
		SoloStageStatus = TEXT("Load a subject before resyncing");
		return FReply::Handled();
	}

	if (!ConfirmDiscardStageEdits(LOCTEXT(
		"DiscardEditsOnResyncPrompt",
		"The stage has edits that were not applied to the source. Resync will replace them with the source mesh.\n\nDiscard the stage edits?")))
	{
		return FReply::Handled();
	}

	const FString PreviousLabel = SoloViewport->GetSoloMeshLabel();
	if (SoloViewport->ResyncFromSource())
	{
		SoloStageStatus = FString::Printf(TEXT("Resynced isolated preview: %s"), *PreviousLabel);
		if (ControlPanel.IsValid())
		{
			ControlPanel->NotifyCaptureWorkspaceChanged();
		}
	}
	else
	{
		SoloStageStatus = TEXT("Could not resync from source; the source may have been deleted");
	}

	return FReply::Handled();
}

FReply SUbstreamModelerWidget::ClearSoloMeshClicked()
{
	if (!ConfirmDiscardStageEdits(LOCTEXT(
		"DiscardEditsOnClearPrompt",
		"The stage has edits that were not applied to the source.\n\nDiscard the stage edits and clear the stage?")))
	{
		return FReply::Handled();
	}

	if (SoloViewport.IsValid())
	{
		SoloViewport->ClearObjects();
	}
	if (ControlPanel.IsValid())
	{
		ControlPanel->ClearCaptureWorkspaceState();
	}
	SoloStageStatus = TEXT("No isolated subject loaded");
	return FReply::Handled();
}

EVisibility SUbstreamModelerWidget::GetSoloPlaceholderVisibility() const
{
	return SoloViewport.IsValid() && SoloViewport->HasSoloMesh()
		? EVisibility::Collapsed
		: EVisibility::HitTestInvisible;
}

FOptionalSize SUbstreamModelerWidget::GetSoloPlaceholderWidthOverride() const
{
	float ViewportWidth = 560.0f;
	if (SoloViewport.IsValid())
	{
		const float CachedWidth = SoloViewport->GetCachedGeometry().GetLocalSize().X;
		if (CachedWidth > 1.0f)
		{
			ViewportWidth = CachedWidth;
		}
	}

	const float HorizontalInset = ViewportWidth < 320.0f ? 20.0f : 48.0f;
	return FOptionalSize(FMath::Clamp(ViewportWidth - HorizontalInset, 168.0f, 680.0f));
}

float SUbstreamModelerWidget::GetSoloPlaceholderWrapWidth() const
{
	return FMath::Max(120.0f, GetSoloPlaceholderWidthOverride().Get() - 8.0f);
}

FText SUbstreamModelerWidget::GetSoloPlaceholderTitleText() const
{
	const float Width = GetSoloPlaceholderWidthOverride().Get();
	if (Width < 260.0f)
	{
		return LOCTEXT("SoloPlaceholderTitleCompact", "Drop Mesh Here");
	}

	return LOCTEXT("SoloPlaceholderTitle", "Drop a Mesh, Component, or Actor here");
}

FText SUbstreamModelerWidget::GetSoloPlaceholderBodyText() const
{
	const float Width = GetSoloPlaceholderWidthOverride().Get();
	if (Width < 260.0f)
	{
		return LOCTEXT("SoloPlaceholderBodyCompact", "Select a mesh and press +, or drop one here.");
	}
	if (Width < 420.0f)
	{
		return LOCTEXT("SoloPlaceholderBodyMedium", "Drop a mesh, actor, component, or 3D file here. You can also select one and press +.");
	}

	return LOCTEXT("SoloPlaceholderBody", "Drag one in from the Content Browser or Outliner, drop a 3D file (.obj, .fbx, .gltf, .stl, .ply) from your computer, right-click it and choose Isolate in Unbound Loop, or select it and press + above.");
}

FText SUbstreamModelerWidget::GetSoloStatusText() const
{
	if (SoloViewport.IsValid() && SoloViewport->HasSoloMesh())
	{
		FString SyncState;
		if (SoloViewport->HasActiveCSGSession() && !SoloViewport->GetCSGSessionMessage().IsEmpty())
		{
			// Surface the session's own guidance (kernel failures, open-boundary
			// warnings) exactly like the Level Editor status bar.
			SyncState = SoloViewport->GetCSGSessionMessage();
		}
		else if (SoloViewport->HasActiveCSGSession())
		{
			SyncState = SoloViewport->IsCSGSessionComputing()
				? TEXT("computing CSG preview")
				: TEXT("live CSG preview");
		}
		else if (SoloViewport->IsEditModeActive())
		{
			SyncState = SoloViewport->HasActiveEditTool()
				? TEXT("tool active")
				: TEXT("editing in stage - sync paused");
		}
		else if (SoloViewport->HasUncommittedStageEdits())
		{
			SyncState = TEXT("stage edits pending - apply or resync");
		}
		else if (!SoloViewport->CanResyncFromSource())
		{
			SyncState = TEXT("detached");
		}
		else if (SoloViewport->IsSourceDirty())
		{
			SyncState = GetDefault<UUbstreamModelerSettings>()->bAutoResyncIsolatedSubject
				? TEXT("syncing")
				: TEXT("source changed");
		}
		else
		{
			SyncState = TEXT("synced");
		}

		return FText::FromString(FString::Printf(
			TEXT("%s  ·  %s  ·  %s"),
			*SoloViewport->GetSoloMeshLabel(),
			*SoloViewport->GetSourceModeLabel(),
			*SyncState));
	}

	return FText::FromString(SoloStageStatus);
}

ECheckBoxState SUbstreamModelerWidget::GetOrbitCameraCheckState() const
{
	const TSharedPtr<FUbstreamViewportClient> Client = SoloViewport.IsValid()
		? SoloViewport->GetUbstreamViewportClient()
		: nullptr;
	return Client.IsValid() && Client->IsOrbitCameraEnabled()
		? ECheckBoxState::Checked
		: ECheckBoxState::Unchecked;
}

void SUbstreamModelerWidget::HandleOrbitCameraChanged(ECheckBoxState NewState)
{
	const bool bEnable = NewState == ECheckBoxState::Checked;

	// Turning orbit on: frame the subject first (same as the F hotkey) so the
	// camera orbits around the centered model instead of wherever the fly
	// camera happened to be pointing.
	if (bEnable && SoloViewport.IsValid())
	{
		SoloViewport->OnFocusViewportToSelection();
	}

	const TSharedPtr<FUbstreamViewportClient> Client = SoloViewport.IsValid()
		? SoloViewport->GetUbstreamViewportClient()
		: nullptr;
	if (Client.IsValid())
	{
		Client->SetOrbitCameraEnabled(bEnable);
	}
}

FReply SUbstreamModelerWidget::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (!FUbstreamAuthService::Get().IsAuthenticated()) return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	if (SoloViewport.IsValid())
	{
		if (SoloViewport->HasPendingCSGPlacement() && InKeyEvent.GetKey() == EKeys::Escape)
		{
			SoloViewport->CancelCSGPlacement();
			return FReply::Handled();
		}

		if (SoloViewport->HasActiveCSGSession())
		{
			if (InKeyEvent.GetKey() == EKeys::Enter)
			{
				SoloViewport->CommitCSGSession();
				return FReply::Handled();
			}
			if (InKeyEvent.GetKey() == EKeys::Escape)
			{
				SoloViewport->DeselectAllStageComponentsForUI();
				return FReply::Handled();
			}
		}

		if (!SoloViewport->HasAnyActiveStageTool()
			&& SoloViewport->HasSelectedCSGPrimitive())
		{
			const FKey Key = InKeyEvent.GetKey();
			if (Key == EKeys::W && SoloViewport->SetCSGTransformMode(EUbstreamCSGTransformMode::Move))
			{
				return FReply::Handled();
			}
			if (Key == EKeys::E && SoloViewport->SetCSGTransformMode(EUbstreamCSGTransformMode::Rotate))
			{
				return FReply::Handled();
			}
			if (Key == EKeys::R && SoloViewport->SetCSGTransformMode(EUbstreamCSGTransformMode::Scale))
			{
				return FReply::Handled();
			}
			if (Key == EKeys::Tab && SoloViewport->SelectNextCSGPrimitiveForEditing(InKeyEvent.IsShiftDown() ? -1 : 1))
			{
				SoloStageStatus = TEXT("Selected CSG cutter");
				return FReply::Handled();
			}
			if ((Key == EKeys::Delete || Key == EKeys::BackSpace) && SoloViewport->DeleteSelectedCSGPrimitives())
			{
				SoloStageStatus = TEXT("Deleted selected CSG cutter");
				return FReply::Handled();
			}
		}
	}

	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

bool SUbstreamModelerWidget::IsSoloMeshLoaded() const
{
	return SoloViewport.IsValid() && SoloViewport->HasSoloMesh();
}

FText SUbstreamModelerWidget::GetSourceBadgeText() const
{
	if (!SoloViewport.IsValid() || !SoloViewport->HasSoloMesh())
	{
		return LOCTEXT("NoSourceBadge", "NO SOURCE");
	}

	if (SoloViewport->IsEditModeActive())
	{
		return LOCTEXT("EditingBadge", "EDITING");
	}

	if (SoloViewport->HasUncommittedStageEdits())
	{
		return LOCTEXT("StageEditsBadge", "STAGE EDITS");
	}

	if (!SoloViewport->CanResyncFromSource())
	{
		return LOCTEXT("DetachedSourceBadge", "DETACHED CLONE");
	}

	return SoloViewport->IsSourceDirty()
		? LOCTEXT("SourceChangedBadge", "SOURCE CHANGED")
		: LOCTEXT("SourceSyncedBadge", "SYNCED CLONE");
}

FText SUbstreamModelerWidget::GetModelingModeBadgeText() const
{
	if (!SoloViewport.IsValid() || !SoloViewport->HasSoloMesh())
	{
		return LOCTEXT("StageModelingNoSubjectBadge", "NO SUBJECT");
	}

	if (SoloViewport->HasActiveCSGSession())
	{
		return SoloViewport->IsCSGSessionComputing()
			? LOCTEXT("CSGComputingBadge", "CSG COMPUTING")
			: LOCTEXT("CSGPreviewBadge", "CSG PREVIEW");
	}

	if (SoloViewport->HasActiveEditTool())
	{
		return GetStageToolBadgeText(SoloViewport->GetActiveEditToolIdentifier());
	}

	if (SoloViewport->IsEditModeActive())
	{
		return LOCTEXT("StageCSGActiveBadge", "CSG STAGE");
	}

	if (FUbstreamModelingModeBridge::IsModelingModeActive())
	{
		return LOCTEXT("SourceModelingActiveBadge", "SOURCE MODELING");
	}

	return LOCTEXT("StageModelingReadyBadge", "READY TO EDIT");
}

FSlateColor SUbstreamModelerWidget::GetSourceBadgeColor() const
{
	if (SoloViewport.IsValid() && SoloViewport->HasSoloMesh() && SoloViewport->IsEditModeActive())
	{
		return FSlateColor(FLinearColor(0.16f, 0.40f, 0.78f, 0.94f));
	}

	if (SoloViewport.IsValid() && SoloViewport->HasSoloMesh() && SoloViewport->HasUncommittedStageEdits())
	{
		return FSlateColor(FLinearColor(0.62f, 0.46f, 0.10f, 0.94f));
	}

	if (!SoloViewport.IsValid() || !SoloViewport->HasSoloMesh() || !SoloViewport->CanResyncFromSource())
	{
		return FSlateColor(FLinearColor(0.22f, 0.23f, 0.25f, 0.92f));
	}

	return SoloViewport->IsSourceDirty()
		? FSlateColor(FLinearColor(0.78f, 0.42f, 0.10f, 0.94f))
		: FSlateColor(FLinearColor(0.12f, 0.42f, 0.28f, 0.94f));
}

FSlateColor SUbstreamModelerWidget::GetModelingModeBadgeColor() const
{
	if (!SoloViewport.IsValid() || !SoloViewport->HasSoloMesh())
	{
		return FSlateColor(FLinearColor(0.20f, 0.21f, 0.23f, 0.92f));
	}

	if (SoloViewport->HasActiveCSGSession() || SoloViewport->HasActiveEditTool())
	{
		return FSlateColor(FLinearColor(0.78f, 0.42f, 0.10f, 0.94f));
	}

	if (SoloViewport->IsEditModeActive())
	{
		return FSlateColor(FLinearColor(0.18f, 0.30f, 0.58f, 0.94f));
	}

	if (FUbstreamModelingModeBridge::IsModelingModeActive())
	{
		return FSlateColor(FLinearColor(0.30f, 0.22f, 0.62f, 0.94f));
	}

	return FSlateColor(FLinearColor(0.12f, 0.42f, 0.28f, 0.94f));
}

bool SUbstreamModelerWidget::IsResyncSourceEnabled() const
{
	return SoloViewport.IsValid() && SoloViewport->CanResyncFromSource();
}

UStaticMesh* SUbstreamModelerWidget::ResolveSelectedStaticMesh() const
{
	if (FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		TArray<FAssetData> SelectedAssets;
		ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);
		for (const FAssetData& AssetData : SelectedAssets)
		{
			if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(AssetData.GetAsset()))
			{
				return StaticMesh;
			}
		}
	}

	if (!GEditor)
	{
		return nullptr;
	}

	for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
	{
		const AActor* Actor = Cast<AActor>(*It);
		const UStaticMeshComponent* StaticMeshComponent = Actor ? Actor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
		if (StaticMeshComponent && StaticMeshComponent->GetStaticMesh())
		{
			return StaticMeshComponent->GetStaticMesh();
		}
	}

	return nullptr;
}

bool SUbstreamModelerWidget::LoadSelectedSubject()
{
	TArray<UMeshComponent*> SelectedComponents = ResolveSelectedMeshComponents();
	if (!SelectedComponents.IsEmpty())
	{
		TArray<TWeakObjectPtr<UMeshComponent>> WeakComponents;
		WeakComponents.Reserve(SelectedComponents.Num());
		for (UMeshComponent* Component : SelectedComponents)
		{
			WeakComponents.Add(Component);
		}
		return LoadSoloComponents(WeakComponents);
	}

	TArray<TWeakObjectPtr<AActor>> SelectedActors = ResolveSelectedActors();
	if (!SelectedActors.IsEmpty())
	{
		return LoadSoloActors(SelectedActors);
	}

	if (UStaticMesh* StaticMesh = ResolveSelectedStaticMesh())
	{
		return LoadSoloMesh(StaticMesh);
	}

	SoloStageStatus = TEXT("Selection has no loadable mesh");
	NotifyLoadFailure(LOCTEXT(
		"NoLoadableSelectionToast",
		"Nothing loadable is selected. Select a Static Mesh asset, or an actor or component with Static Mesh or Dynamic Mesh geometry."));
	return false;
}

TArray<UMeshComponent*> SUbstreamModelerWidget::ResolveSelectedMeshComponents() const
{
	TArray<UMeshComponent*> Components;
	if (!GEditor)
	{
		return Components;
	}

	TSet<UMeshComponent*> AddedComponents;
	for (FSelectionIterator It(GEditor->GetSelectedComponentIterator()); It; ++It)
	{
		UMeshComponent* MeshComponent = Cast<UMeshComponent>(*It);
		if (UbstreamMeshComponents::HasRenderableGeometry(MeshComponent) && !AddedComponents.Contains(MeshComponent))
		{
			Components.Add(MeshComponent);
			AddedComponents.Add(MeshComponent);
		}
	}
	return Components;
}

TArray<TWeakObjectPtr<AActor>> SUbstreamModelerWidget::ResolveSelectedActors() const
{
	TArray<TWeakObjectPtr<AActor>> Actors;
	if (!GEditor)
	{
		return Actors;
	}

	TSet<AActor*> AddedActors;
	for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
	{
		AActor* Actor = Cast<AActor>(*It);
		if (!IsValid(Actor) || AddedActors.Contains(Actor))
		{
			continue;
		}

		if (!UbstreamMeshComponents::ActorHasRenderableMeshComponent(Actor))
		{
			continue;
		}

		Actors.Add(Actor);
		AddedActors.Add(Actor);
	}
	return Actors;
}

void SUbstreamModelerWidget::NotifyStage(const FText& Message, bool bIsError) const
{
	FNotificationInfo Info(Message);
	Info.bFireAndForget = true;
	Info.ExpireDuration = bIsError ? 6.0f : 4.0f;
	Info.Image = FUbstreamModelerStyle::Get().GetBrush(TEXT("UbstreamModeler.Icon20"));

	TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification.IsValid())
	{
		Notification->SetCompletionState(bIsError ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
	}
}

void SUbstreamModelerWidget::NotifyLoadFailure(const FText& Message) const
{
	NotifyStage(Message, /*bIsError*/ true);
}

bool SUbstreamModelerWidget::ConfirmDiscardStageEdits(const FText& Prompt) const
{
	return !SoloViewport.IsValid() || SoloViewport->ConfirmDiscardStageEdits(Prompt);
}

int32 SUbstreamModelerWidget::SelectLoadedSourceInEditor() const
{
	if (!GEditor || !SoloViewport.IsValid())
	{
		return 0;
	}

	int32 SelectedCount = 0;
	GEditor->SelectNone(false, true, false);

	const TArray<TWeakObjectPtr<UMeshComponent>> SourceComponents = SoloViewport->GetSourceComponents();
	for (const TWeakObjectPtr<UMeshComponent>& ComponentPtr : SourceComponents)
	{
		UMeshComponent* Component = ComponentPtr.Get();
		if (!UbstreamMeshComponents::HasRenderableGeometry(Component))
		{
			continue;
		}

		if (AActor* Owner = Component->GetOwner(); IsValid(Owner))
		{
			GEditor->SelectActor(Owner, true, false, true);
		}
		GEditor->SelectComponent(Component, true, false, true);
		++SelectedCount;
	}

	if (SelectedCount == 0)
	{
		for (const TWeakObjectPtr<AActor>& ActorPtr : SoloViewport->GetSourceActors())
		{
			AActor* Actor = ActorPtr.Get();
			if (IsValid(Actor))
			{
				GEditor->SelectActor(Actor, true, false, true);
				++SelectedCount;
			}
		}
	}

	if (SelectedCount > 0)
	{
		GEditor->NoteSelectionChange();
	}

	return SelectedCount;
}

void SUbstreamModelerWidget::HandleSoloMeshChanged()
{
	// This fires PER GIZMO DELTA during drags, so it must stay cheap. The
	// panel's full workspace update (passthrough ensure, autoconnect, layout
	// invalidation) runs only when the workspace identity actually changed -
	// i.e. a different subject was loaded or cleared, not moved.
	const bool bHasMesh = SoloViewport.IsValid() && SoloViewport->HasSoloMesh();
	const FString WorkspaceKey = bHasMesh
		? FString::Printf(TEXT("%s|%s"), *SoloViewport->GetSoloMeshLabel(), *SoloViewport->GetSoloMeshPath())
		: FString();
	SoloStageStatus = bHasMesh
		? FString::Printf(TEXT("Capture source: %s"), *SoloViewport->GetSoloMeshLabel())
		: TEXT("No isolated subject loaded");

	if (ControlPanel.IsValid())
	{
		if (WorkspaceKey != LastNotifiedWorkspaceKey)
		{
			LastNotifiedWorkspaceKey = WorkspaceKey;
			ControlPanel->NotifyCaptureWorkspaceChanged();
		}
		else
		{
			ControlPanel->NotifyStageContentDirty();
		}
	}
}

EActiveTimerReturnType SUbstreamModelerWidget::HandleDeferredModelingModeActivation(double InCurrentTime, float InDeltaTime)
{
	if (FUbstreamModelingModeBridge::IsModelingModeAvailable())
	{
		FUbstreamModelingModeBridge::ActivateModelingMode();
	}

	return EActiveTimerReturnType::Stop;
}

EActiveTimerReturnType SUbstreamModelerWidget::HandleSourceStatusActiveTimer(double InCurrentTime, float InDeltaTime)
{
	if (SoloViewport.IsValid() && SoloViewport->HasSoloMesh())
	{
		// Never auto-resync over an in-progress edit session or unapplied stage
		// edits; the stage owns the working copy until Apply or manual Resync.
		if (GetDefault<UUbstreamModelerSettings>()->bAutoResyncIsolatedSubject
			&& !SoloViewport->IsEditModeActive()
			&& !SoloViewport->HasUncommittedStageEdits()
			&& SoloViewport->CanResyncFromSource()
			&& SoloViewport->IsSourceDirty())
		{
			SoloViewport->ResyncFromSource(/*bPreserveCamera*/ true);
		}

		Invalidate(EInvalidateWidgetReason::Paint);
	}

	return EActiveTimerReturnType::Continue;
}

#undef LOCTEXT_NAMESPACE
