#include "Widgets/SUbstreamElementsPanel.h"

#include "Components/MeshComponent.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "Styling/UbstreamModelerStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SRotatorInputBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SVectorInputBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SUbstreamModelerViewport.h"
#include "Widgets/SUbstreamSculptPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SUbstreamElementsPanel"

namespace
{
	const FLinearColor SubtractOpColor(1.0f, 0.45f, 0.32f);
	const FLinearColor UnionOpColor(0.4f, 0.95f, 0.55f);
	const FLinearColor IntersectOpColor(0.4f, 0.68f, 1.0f);

	FSlateFontInfo ElementsPanelFont(const int32 Size, const bool bBold = false)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size);
	}
}

void SUbstreamElementsPanel::Construct(const FArguments& InArgs)
{
	SoloViewport = InArgs._SoloViewport;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Panel"))
		.Padding(FMargin(10.0f, 8.0f))
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildHeaderRow()
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					BuildModelRow()
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SAssignNew(CutterRowsBox, SVerticalBox)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					BuildPropertiesSection()
				]
				// Sculpting shares this base mesh with the cutters; it lives inline
				// beneath them so both surface workflows sit in one column. The
				// panel hides itself (divider included) when no mesh is staged.
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SUbstreamSculptPanel)
						.SoloViewport(SoloViewport)
				]
			]
		]
	];

	Refresh();
}

TSharedRef<SWidget> SUbstreamElementsPanel::BuildHeaderRow()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ElementsHeader", "ELEMENTS"))
			.Font(ElementsPanelFont(10, true))
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 2.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
			.ToolTipText(LOCTEXT("BakeTooltip", "Bake all cutters into the stage mesh (Enter). Removes the cutters; Undo restores them. Apply to source writes the edited mesh back to its source."))
			.IsEnabled_Lambda([this]()
			{
				const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
				return Viewport.IsValid() && Viewport->CanCommitCSGSession();
			})
			.OnClicked_Lambda([this]()
			{
				if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
				{
					Viewport->CommitCSGSession();
				}
				Refresh();
				return FReply::Handled();
			})
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Check"))
				.DesiredSizeOverride(FVector2D(12.0f, 12.0f))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
			.ToolTipText(LOCTEXT("ClearAllTooltip", "Remove all cutters and their live effects. Undo restores them."))
			.IsEnabled_Lambda([this]()
			{
				const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
				return Viewport.IsValid() && Viewport->GetCSGPrimitiveCount() > 0;
			})
			.OnClicked_Lambda([this]()
			{
				if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
				{
					Viewport->ClearCSGPrimitives();
				}
				Refresh();
				return FReply::Handled();
			})
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Delete"))
				.DesiredSizeOverride(FVector2D(12.0f, 12.0f))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
}

TSharedRef<SWidget> SUbstreamElementsPanel::BuildModelRow()
{
	return SNew(SCheckBox)
		.Style(&FUbstreamModelerStyle::Get(), "Ubstream.OutlinerRow")
		.ToolTipText(LOCTEXT("ModelRowTooltip", "The carving subject. Click to clear the cutter selection."))
		.Visibility_Lambda([this]()
		{
			const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
			return Viewport.IsValid() && Viewport->HasSoloMesh() ? EVisibility::Visible : EVisibility::Collapsed;
		})
		.IsChecked_Lambda([this]()
		{
			const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
			return Viewport.IsValid() && !Viewport->HasSelectedCSGPrimitive()
				? ECheckBoxState::Checked
				: ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState)
		{
			if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
			{
				Viewport->DeselectAllStageComponentsForUI();
			}
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(10.0f)
				.HeightOverride(10.0f)
				[
					SNew(SBorder)
					.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Dot"))
					.BorderBackgroundColor(FLinearColor(0.65f, 0.72f, 0.82f, 1.0f))
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
					return Viewport.IsValid()
						? FText::FromString(Viewport->GetSoloMeshLabel())
						: FText::GetEmpty();
				})
				.Font(ElementsPanelFont(9))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
			]
		];
}

void SUbstreamElementsPanel::Refresh()
{
	if (!CutterRowsBox.IsValid())
	{
		return;
	}
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	TArray<TPair<UMeshComponent*, FString>> Cutters;
	if (Viewport.IsValid())
	{
		Viewport->GetCSGPrimitiveDisplayList(Cutters);
	}

	bool bRowsUnchanged = Cutters.Num() == DisplayedCutters.Num();
	for (int32 Index = 0; bRowsUnchanged && Index < Cutters.Num(); ++Index)
	{
		bRowsUnchanged = DisplayedCutters[Index].Key.Get() == Cutters[Index].Key
			&& DisplayedCutters[Index].Value == Cutters[Index].Value;
	}
	if (bRowsUnchanged)
	{
		// Selection, operation color and numeric properties use bound attributes.
		Invalidate(EInvalidateWidgetReason::Paint);
		return;
	}

	CutterRowsBox->ClearChildren();
	DisplayedCutters.Reset(Cutters.Num());
	for (const TPair<UMeshComponent*, FString>& Cutter : Cutters)
	{
		DisplayedCutters.Emplace(Cutter.Key, Cutter.Value);
		CutterRowsBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 1.0f)
		[
			MakeCutterRow(Cutter.Key, Cutter.Value)
		];
	}
}

TSharedRef<SWidget> SUbstreamElementsPanel::MakeCutterRow(TWeakObjectPtr<UMeshComponent> Cutter, const FString& Name)
{
	return SNew(SCheckBox)
		.Style(&FUbstreamModelerStyle::Get(), "Ubstream.OutlinerRow")
		.ToolTipText(LOCTEXT("CutterRowTooltip", "Select this cutter (Shift adds, Ctrl toggles). The dot color shows its operation."))
		.IsChecked_Lambda([this, Cutter]()
		{
			return IsCutterSelected(Cutter) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, Cutter](ECheckBoxState)
		{
			HandleRowClicked(Cutter);
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(10.0f)
				.HeightOverride(10.0f)
				[
					SNew(SBorder)
					.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Dot"))
					.BorderBackgroundColor_Lambda([this, Cutter]()
					{
						return FSlateColor(GetOperationColor(Cutter.Get()));
					})
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Name))
				.Font(ElementsPanelFont(9))
				.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
				.ToolTipText(LOCTEXT("DeleteCutterTooltip", "Delete this cutter."))
				.Visibility_Lambda([this, Cutter]()
				{
					// Hidden (not Collapsed) so rows keep a stable width.
					return IsCutterSelected(Cutter) || IsHovered()
						? EVisibility::Visible
						: EVisibility::Hidden;
				})
				.OnClicked_Lambda([this, Cutter]()
				{
					DeleteCutter(Cutter);
					return FReply::Handled();
				})
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.X"))
					.DesiredSizeOverride(FVector2D(10.0f, 10.0f))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		];
}

TSharedRef<SWidget> SUbstreamElementsPanel::BuildPropertiesSection()
{
	const auto LabelText = [](const FText& Text)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Font(ElementsPanelFont(8))
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()));
	};

	return SNew(SBox)
		.Visibility_Lambda([this]()
		{
			const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
			return Viewport.IsValid() && Viewport->HasSelectedCSGPrimitive()
				? EVisibility::Visible
				: EVisibility::Collapsed;
		})
		[
			SNew(SBorder)
			.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Group"))
			.Padding(FMargin(8.0f))
			[
				SNew(SVerticalBox)
				.IsEnabled_Lambda([this]()
				{
					// Numeric writes target the primary cutter; with several
					// selected the fields go read-only to avoid ambiguity.
					const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
					if (!Viewport.IsValid())
					{
						return false;
					}
					TArray<UMeshComponent*> SelectedCutters;
					Viewport->GetSelectedCSGPrimitiveComponents(SelectedCutters);
					return SelectedCutters.Num() <= 1;
				})
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PropertiesHeader", "PROPERTIES"))
					.Font(ElementsPanelFont(10, true))
					.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					// Per-cutter Accept: bakes THIS subtract/intersect into the
					// mesh and removes the cutter; other cutters stay live.
					// Visible only while the selected shape's op supports it.
					SNew(SBox)
					.Visibility_Lambda([this]()
					{
						const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
						return Viewport.IsValid() && Viewport->CanAcceptSelectedCutter()
							? EVisibility::Visible
							: EVisibility::Collapsed;
					})
					.Padding(0.0f, 0.0f, 0.0f, 8.0f)
					[
						SNew(SButton)
						.HAlign(HAlign_Center)
						.ToolTipText(LOCTEXT("AcceptCutTooltip", "Bake the selected cutter into the stage mesh and remove it (Union adds, Subtract/Intersect cut). Other cutters stay editable. Undo restores this cutter."))
						.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.PrimaryButton")
						.OnClicked_Lambda([this]()
						{
							if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
							{
								Viewport->AcceptSelectedCutter();
							}
							Refresh();
							return FReply::Handled();
						})
						[
							SNew(STextBlock)
							.Text(LOCTEXT("AcceptCutLabel", "Bake selected cutter"))
							.Font(ElementsPanelFont(9, true))
							.ColorAndOpacity(FSlateColor(FLinearColor::White))
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 2.0f, 0.0f)
					[
						MakeOperationToggle(LOCTEXT("SubtractOp", "Subtract"), EUbstreamCSGOperation::Subtract, SubtractOpColor)
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f)
					[
						MakeOperationToggle(LOCTEXT("UnionOp", "Union"), EUbstreamCSGOperation::Union, UnionOpColor)
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f, 0.0f, 0.0f)
					[
						MakeOperationToggle(LOCTEXT("IntersectOp", "Intersect"), EUbstreamCSGOperation::Intersect, IntersectOpColor)
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 2.0f)
				[
					LabelText(LOCTEXT("PositionLabel", "Position"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SNumericVectorInputBox<double>)
					.AllowSpin(true)
					.bColorAxisLabels(true)
					.X(this, &SUbstreamElementsPanel::GetLocationComponent, 0)
					.Y(this, &SUbstreamElementsPanel::GetLocationComponent, 1)
					.Z(this, &SUbstreamElementsPanel::GetLocationComponent, 2)
					.OnXChanged(this, &SUbstreamElementsPanel::HandleLocationChanged, 0)
					.OnYChanged(this, &SUbstreamElementsPanel::HandleLocationChanged, 1)
					.OnZChanged(this, &SUbstreamElementsPanel::HandleLocationChanged, 2)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 2.0f)
				[
					LabelText(LOCTEXT("RotationLabel", "Rotation"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SNumericRotatorInputBox<double>)
					.AllowSpin(true)
					.bColorAxisLabels(true)
					.Roll(this, &SUbstreamElementsPanel::GetRotationComponent, 0)
					.Pitch(this, &SUbstreamElementsPanel::GetRotationComponent, 1)
					.Yaw(this, &SUbstreamElementsPanel::GetRotationComponent, 2)
					.OnRollChanged(this, &SUbstreamElementsPanel::HandleRotationChanged, 0)
					.OnPitchChanged(this, &SUbstreamElementsPanel::HandleRotationChanged, 1)
					.OnYawChanged(this, &SUbstreamElementsPanel::HandleRotationChanged, 2)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 2.0f)
				[
					LabelText(LOCTEXT("DimensionsLabel", "Dimensions"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					// Oriented per-axis size: stretch a cube into a slab, a
					// sphere into an ellipsoid, a cylinder longer or shorter.
					SNew(SNumericVectorInputBox<double>)
					.AllowSpin(true)
					.bColorAxisLabels(true)
					.X(this, &SUbstreamElementsPanel::GetDimensionComponent, 0)
					.Y(this, &SUbstreamElementsPanel::GetDimensionComponent, 1)
					.Z(this, &SUbstreamElementsPanel::GetDimensionComponent, 2)
					.OnXChanged(this, &SUbstreamElementsPanel::HandleDimensionChanged, 0)
					.OnYChanged(this, &SUbstreamElementsPanel::HandleDimensionChanged, 1)
					.OnZChanged(this, &SUbstreamElementsPanel::HandleDimensionChanged, 2)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 2.0f)
				[
					LabelText(LOCTEXT("ScaleLabel", "Scale"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SSlider)
						.Style(&FUbstreamModelerStyle::Get().GetWidgetStyle<FSliderStyle>(TEXT("Ubstream.Slider")))
						.ToolTipText(LOCTEXT("ScaleSliderTooltip", "Uniform scale of the selected cutter (1.0 = default spawn size)."))
						.MinValue(0.05f)
						.MaxValue(8.0f)
						.Value(this, &SUbstreamElementsPanel::GetUniformScaleSliderValue)
						.OnValueChanged(this, &SUbstreamElementsPanel::HandleUniformScaleSliderChanged)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SBox)
						.MinDesiredWidth(56.0f)
						[
							SNew(SNumericEntryBox<double>)
							.AllowSpin(true)
							.MinValue(0.05)
							.MaxValue(32.0)
							.MinSliderValue(0.05)
							.MaxSliderValue(8.0)
							.Delta(0.05)
							.ToolTipText(LOCTEXT("ScaleNumericTooltip", "Uniform scale factor. Drag the slider or type a precise value."))
							.Value(this, &SUbstreamElementsPanel::GetUniformScale)
							.OnValueChanged(this, &SUbstreamElementsPanel::HandleUniformScaleChanged)
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)
						[
							LabelText(LOCTEXT("SizeLabel", "Size"))
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SNumericEntryBox<double>)
							.AllowSpin(true)
							.MinValue(0.001)
							.MinSliderValue(0.001)
							.ToolTipText(LOCTEXT("UniformSizeTooltip", "Uniform size (largest world-space dimension); use Dimensions above for per-axis reshaping."))
							.Value(this, &SUbstreamElementsPanel::GetSize)
							.OnValueChanged(this, &SUbstreamElementsPanel::HandleSizeChanged)
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Bottom)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)
						[
							LabelText(LOCTEXT("SnapLabel", "Snap"))
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SComboButton)
							.ToolTipText(LOCTEXT("SnapTooltip", "Drag snapping increment (rotation snaps to 15 degrees when on)."))
							.OnGetMenuContent(this, &SUbstreamElementsPanel::BuildSnapMenu)
							.ButtonContent()
							[
								SNew(STextBlock)
								.Font(ElementsPanelFont(9))
								.Text_Lambda([this]()
								{
									const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
									const double Snap = Viewport.IsValid() ? Viewport->GetCSGSnapIncrement() : 0.0;
									return Snap > 0.0
										? FText::FromString(FString::Printf(TEXT("%g"), Snap))
										: LOCTEXT("SnapOff", "Off");
								})
							]
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SUbstreamElementsPanel::MakeOperationToggle(
	const FText& Label, const EUbstreamCSGOperation Operation, const FLinearColor& OpColor)
{
	return SNew(SCheckBox)
		.Style(&FUbstreamModelerStyle::Get(), "Ubstream.IconToggle")
		.HAlign(HAlign_Center)
		.IsChecked_Lambda([this, Operation]()
		{
			const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
			return Viewport.IsValid()
				&& Viewport->HasSelectedCSGPrimitive()
				&& Viewport->GetSelectedCSGOperation() == Operation
				? ECheckBoxState::Checked
				: ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, Operation](const ECheckBoxState NewState)
		{
			const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
			if (NewState == ECheckBoxState::Checked && Viewport.IsValid())
			{
				Viewport->StartCSGOperation(Operation);
			}
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(7.0f)
				.HeightOverride(7.0f)
				[
					SNew(SBorder)
					.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Dot"))
					.BorderBackgroundColor(OpColor)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(ElementsPanelFont(8))
			]
		];
}

TSharedRef<SWidget> SUbstreamElementsPanel::BuildSnapMenu()
{
	FMenuBuilder MenuBuilder(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);
	const double SnapOptions[] = { 0.0, 0.5, 1.0, 5.0, 10.0 };
	for (const double SnapOption : SnapOptions)
	{
		MenuBuilder.AddMenuEntry(
			SnapOption > 0.0
				? FText::FromString(FString::Printf(TEXT("%g"), SnapOption))
				: LOCTEXT("SnapOffEntry", "Off"),
			FText::GetEmpty(),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([this, SnapOption]()
				{
					if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
					{
						Viewport->SetCSGSnapIncrement(SnapOption);
					}
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([this, SnapOption]()
				{
					const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
					return Viewport.IsValid()
						&& FMath::IsNearlyEqual(Viewport->GetCSGSnapIncrement(), SnapOption);
				})),
			NAME_None,
			EUserInterfaceActionType::RadioButton);
	}
	return MenuBuilder.MakeWidget();
}

bool SUbstreamElementsPanel::IsCutterSelected(const TWeakObjectPtr<UMeshComponent> Cutter) const
{
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (!Viewport.IsValid() || !Cutter.IsValid())
	{
		return false;
	}
	TArray<UMeshComponent*> SelectedCutters;
	Viewport->GetSelectedCSGPrimitiveComponents(SelectedCutters);
	return SelectedCutters.Contains(Cutter.Get());
}

void SUbstreamElementsPanel::HandleRowClicked(const TWeakObjectPtr<UMeshComponent> Cutter)
{
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (!Viewport.IsValid() || !Cutter.IsValid())
	{
		return;
	}
	// Same modifier grammar as the viewport: Shift adds, Ctrl/Cmd toggles.
	const FModifierKeysState Modifiers = FSlateApplication::Get().GetModifierKeys();
	const bool bAddToSelection = Modifiers.IsShiftDown() || Modifiers.IsControlDown() || Modifiers.IsCommandDown();
	Viewport->SelectCSGPrimitiveForEditing(Cutter.Get(), bAddToSelection);
}

void SUbstreamElementsPanel::DeleteCutter(const TWeakObjectPtr<UMeshComponent> Cutter)
{
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (!Viewport.IsValid() || !Cutter.IsValid())
	{
		return;
	}
	Viewport->SelectCSGPrimitiveForEditing(Cutter.Get());
	Viewport->DeleteSelectedCSGPrimitives();
	Refresh();
}

FLinearColor SUbstreamElementsPanel::GetOperationColor(const UMeshComponent* Cutter) const
{
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (Viewport.IsValid() && Cutter)
	{
		switch (Viewport->GetCSGPrimitiveOperation(Cutter))
		{
		case EUbstreamCSGOperation::Union: return UnionOpColor;
		case EUbstreamCSGOperation::Intersect: return IntersectOpColor;
		default: break;
		}
	}
	return SubtractOpColor;
}

TOptional<double> SUbstreamElementsPanel::GetLocationComponent(const int32 Axis) const
{
	FVector Location;
	FRotator Rotation;
	double Size = 0.0;
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (Viewport.IsValid() && Viewport->GetSelectedCSGPrimitiveTransform(Location, Rotation, Size))
	{
		return Location[Axis];
	}
	return TOptional<double>();
}

void SUbstreamElementsPanel::HandleLocationChanged(const double NewValue, const int32 Axis)
{
	FVector Location;
	FRotator Rotation;
	double Size = 0.0;
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (Viewport.IsValid() && Viewport->GetSelectedCSGPrimitiveTransform(Location, Rotation, Size))
	{
		Location[Axis] = NewValue;
		Viewport->SetSelectedCSGPrimitiveLocation(Location);
	}
}

TOptional<double> SUbstreamElementsPanel::GetRotationComponent(const int32 Axis) const
{
	FVector Location;
	FRotator Rotation;
	double Size = 0.0;
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (Viewport.IsValid() && Viewport->GetSelectedCSGPrimitiveTransform(Location, Rotation, Size))
	{
		return Axis == 0 ? Rotation.Roll : (Axis == 1 ? Rotation.Pitch : Rotation.Yaw);
	}
	return TOptional<double>();
}

void SUbstreamElementsPanel::HandleRotationChanged(const double NewValue, const int32 Axis)
{
	FVector Location;
	FRotator Rotation;
	double Size = 0.0;
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (Viewport.IsValid() && Viewport->GetSelectedCSGPrimitiveTransform(Location, Rotation, Size))
	{
		switch (Axis)
		{
		case 0: Rotation.Roll = NewValue; break;
		case 1: Rotation.Pitch = NewValue; break;
		default: Rotation.Yaw = NewValue; break;
		}
		Viewport->SetSelectedCSGPrimitiveRotation(Rotation);
	}
}

TOptional<double> SUbstreamElementsPanel::GetDimensionComponent(const int32 Axis) const
{
	FVector Dimensions;
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (Viewport.IsValid() && Viewport->GetSelectedCSGPrimitiveDimensions(Dimensions))
	{
		return Dimensions[Axis];
	}
	return TOptional<double>();
}

void SUbstreamElementsPanel::HandleDimensionChanged(const double NewValue, const int32 Axis)
{
	if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
	{
		Viewport->SetSelectedCSGPrimitiveDimension(Axis, NewValue);
	}
}

TOptional<double> SUbstreamElementsPanel::GetSize() const
{
	FVector Location;
	FRotator Rotation;
	double Size = 0.0;
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (Viewport.IsValid() && Viewport->GetSelectedCSGPrimitiveTransform(Location, Rotation, Size))
	{
		return Size;
	}
	return TOptional<double>();
}

void SUbstreamElementsPanel::HandleSizeChanged(const double NewValue)
{
	if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
	{
		Viewport->SetSelectedCSGPrimitiveSize(NewValue);
	}
}

TOptional<double> SUbstreamElementsPanel::GetUniformScale() const
{
	double Scale = 1.0;
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	if (Viewport.IsValid() && Viewport->GetSelectedCSGPrimitiveUniformScale(Scale))
	{
		return Scale;
	}
	return TOptional<double>();
}

void SUbstreamElementsPanel::HandleUniformScaleChanged(const double NewValue)
{
	if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
	{
		Viewport->SetSelectedCSGPrimitiveUniformScale(NewValue);
	}
}

float SUbstreamElementsPanel::GetUniformScaleSliderValue() const
{
	const TOptional<double> Scale = GetUniformScale();
	return Scale.IsSet() ? static_cast<float>(Scale.GetValue()) : 1.0f;
}

void SUbstreamElementsPanel::HandleUniformScaleSliderChanged(const float NewValue)
{
	HandleUniformScaleChanged(static_cast<double>(NewValue));
}

#undef LOCTEXT_NAMESPACE
