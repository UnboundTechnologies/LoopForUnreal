#include "Widgets/SUbstreamSculptPanel.h"

#include "Framework/Notifications/NotificationManager.h"
#include "Styling/AppStyle.h"
#include "Styling/UbstreamModelerStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SUbstreamModelerViewport.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SUbstreamSculptPanel"

namespace
{
	FSlateFontInfo PanelFont(const int32 Size, const bool bBold = false)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size);
	}

	TSharedRef<SWidget> LabelText(const FText& Label)
	{
		return SNew(STextBlock)
			.Text(Label)
			.Font(PanelFont(8))
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()));
	}

	FString FormatTriangleCount(int32 Count)
	{
		return Count >= 1000
			? FString::Printf(TEXT("%.1fk"), Count / 1000.0f)
			: FString::Printf(TEXT("%d"), Count);
	}

	// The tri budget spans two-plus orders of magnitude, so the slider is log-scaled.
	float TrianglesToSlider(int32 Triangles, int32 MinTri, int32 MaxTri)
	{
		const float LogMin = FMath::Loge(static_cast<float>(MinTri));
		const float LogMax = FMath::Loge(static_cast<float>(MaxTri));
		const float LogValue = FMath::Loge(static_cast<float>(FMath::Clamp(Triangles, MinTri, MaxTri)));
		return (LogValue - LogMin) / (LogMax - LogMin);
	}

	int32 SliderToTriangles(float SliderValue, int32 MinTri, int32 MaxTri)
	{
		const float LogMin = FMath::Loge(static_cast<float>(MinTri));
		const float LogMax = FMath::Loge(static_cast<float>(MaxTri));
		return FMath::RoundToInt(FMath::Exp(FMath::Lerp(LogMin, LogMax, FMath::Clamp(SliderValue, 0.0f, 1.0f))));
	}
}

void SUbstreamSculptPanel::Construct(const FArguments& InArgs)
{
	SoloViewport = InArgs._SoloViewport;

	ChildSlot
	[
		// Keep progress and cancellation visible while editing is disabled.
		SNew(SVerticalBox)
		.Visibility_Lambda([this]()
		{
			const auto Viewport = SoloViewport.Pin();
			return CanSculpt() || (Viewport && Viewport->IsRemeshing()) ? EVisibility::Visible : EVisibility::Collapsed;
		})
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 14.0f, 0.0f, 12.0f)
		[
			SNew(SSeparator)
			.Thickness(1.0f)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildHeaderRow()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			BuildBrushGrid()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			BuildParameters()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			BuildDetailSection()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Font(PanelFont(8))
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
			.Text_Lambda([this]()
			{
				return IsSculptActive()
					? LOCTEXT("SculptActiveHint", "Drag to sculpt · Shift smooths · Ctrl inverts · [ ] or S/D size · W/E strength")
					: LOCTEXT("SculptIdleHint", "Pick a brush, then drag on the model. Live cuts bake in first.");
			})
		]
	];
	if (const auto Viewport = SoloViewport.Pin())
	{
		RemeshStateHandle = Viewport->OnRemeshStateChanged.AddSP(this, &SUbstreamSculptPanel::UpdateRemeshProgress);
	}
	UpdateRemeshProgress();
}

SUbstreamSculptPanel::~SUbstreamSculptPanel()
{
	if (const auto Viewport = SoloViewport.Pin()) Viewport->OnRemeshStateChanged.Remove(RemeshStateHandle);
}

void SUbstreamSculptPanel::UpdateRemeshProgress()
{
	const auto Viewport = SoloViewport.Pin();
	// SetPercent unregisters the marquee timer when switching back to a
	// determinate value, including when the panel is hidden or closing.
	if (RemeshProgressBar)
		RemeshProgressBar->SetPercent(Viewport && Viewport->IsRemeshing() ? TOptional<float>() : TOptional<float>(0.0f));
}

TSharedRef<SWidget> SUbstreamSculptPanel::BuildHeaderRow()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SculptHeader", "SCULPT"))
			.Font(PanelFont(10, true))
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 4.0f, 0.0f)
		[
			// Discard the stroke session and restore the pre-sculpt mesh; sits
			// just left of Done and, like it, only while sculpting is live.
			SNew(SButton)
			.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
			.ToolTipText(LOCTEXT("SculptCancelTooltip", "Cancel sculpting and restore the mesh (discards this session's strokes)."))
			.Visibility_Lambda([this]()
			{
				return IsSculptActive() ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.OnClicked_Lambda([this]()
			{
				if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
				{
					Viewport->StopSculpt(/*bAccept*/ false);
				}
				return FReply::Handled();
			})
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.X"))
				.DesiredSizeOverride(FVector2D(11.0f, 11.0f))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			// Bakes the deformation into the base mesh and returns to the cutter
			// workflow; only meaningful while a sculpt stroke session is live.
			SNew(SButton)
			.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.IconButton")
			.ToolTipText(LOCTEXT("SculptDoneTooltip", "Finish sculpting and bake the deformation into the mesh."))
			.Visibility_Lambda([this]()
			{
				return IsSculptActive() ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.OnClicked_Lambda([this]()
			{
				if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
				{
					Viewport->StopSculpt(/*bAccept*/ true);
				}
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Check"))
					.DesiredSizeOverride(FVector2D(12.0f, 12.0f))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SculptDone", "Done"))
					.Font(PanelFont(8, true))
				]
			]
		];
}

TSharedRef<SWidget> SUbstreamSculptPanel::BuildBrushGrid()
{
	auto MakeRow = [this](
		const FText& LabelA, EUbstreamSculptBrush BrushA, const FText& TipA,
		const FText& LabelB, EUbstreamSculptBrush BrushB, const FText& TipB,
		const FText& LabelC, EUbstreamSculptBrush BrushC, const FText& TipC)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				MakeBrushToggle(LabelA, BrushA, TipA)
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f, 2.0f, 0.0f)
			[
				MakeBrushToggle(LabelB, BrushB, TipB)
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				MakeBrushToggle(LabelC, BrushC, TipC)
			];
	};

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(
				LOCTEXT("BrushMove", "Move"), EUbstreamSculptBrush::Move,
				LOCTEXT("BrushMoveTip", "Grab and drag vertices parallel to the view."),
				LOCTEXT("BrushDraw", "Draw"), EUbstreamSculptBrush::Draw,
				LOCTEXT("BrushDrawTip", "Push or pull the surface along its normal (Ctrl inverts)."),
				LOCTEXT("BrushSmooth", "Smooth"), EUbstreamSculptBrush::Smooth,
				LOCTEXT("BrushSmoothTip", "Relax and soften the surface."))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			MakeRow(
				LOCTEXT("BrushInflate", "Inflate"), EUbstreamSculptBrush::Inflate,
				LOCTEXT("BrushInflateTip", "Expand along vertex normals (Ctrl deflates)."),
				LOCTEXT("BrushFlatten", "Flatten"), EUbstreamSculptBrush::Flatten,
				LOCTEXT("BrushFlattenTip", "Pull vertices toward the average plane of the brush."),
				LOCTEXT("BrushPinch", "Pinch"), EUbstreamSculptBrush::Pinch,
				LOCTEXT("BrushPinchTip", "Draw vertices toward the brush centre (Ctrl pushes out)."))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			MakeRow(
				LOCTEXT("BrushClay", "Clay"), EUbstreamSculptBrush::Clay,
				LOCTEXT("BrushClayTip", "Build up to a bounded height along the normal - classic clay (Ctrl carves)."),
				LOCTEXT("BrushScrape", "Scrape"), EUbstreamSculptBrush::Scrape,
				LOCTEXT("BrushScrapeTip", "Shave high spots down toward a plane set where the stroke began (Ctrl inverts)."),
				LOCTEXT("BrushGrab", "Grab"), EUbstreamSculptBrush::Grab,
				LOCTEXT("BrushGrabTip", "Soft volumetric grab with a Kelvinlet falloff - great for large organic pulls."))
		];
}

TSharedRef<SWidget> SUbstreamSculptPanel::MakeBrushToggle(
	const FText& Label, const EUbstreamSculptBrush Brush, const FText& ToolTip)
{
	return SNew(SCheckBox)
		.Style(&FUbstreamModelerStyle::Get(), "Ubstream.IconToggle")
		.IsEnabled_Lambda([this]() { return CanSculpt(); })
		.HAlign(HAlign_Center)
		.ToolTipText(ToolTip)
		.IsChecked_Lambda([this, Brush]()
		{
			const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
			return Viewport.IsValid()
				&& Viewport->IsSculptActive()
				&& Viewport->GetSculptBrush() == Brush
				? ECheckBoxState::Checked
				: ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, Brush](const ECheckBoxState)
		{
			// A brush is always a "start/switch"; ignore the toggle direction so
			// clicking the active brush cannot turn sculpting off by accident.
			if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
			{
				Viewport->StartSculpt(Brush);
			}
		})
		[
			SNew(SBox)
			.HeightOverride(26.0f)
			.VAlign(VAlign_Center)
			.Padding(FMargin(2.0f, 0.0f))
			[
				SNew(STextBlock)
				.Justification(ETextJustify::Center)
				.Text(Label)
				.Font(PanelFont(9))
				.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
			]
		];
}

TSharedRef<SWidget> SUbstreamSculptPanel::BuildParameters()
{
	return SNew(SBorder)
		.IsEnabled_Lambda([this]() { return CanSculpt(); })
		.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Group"))
		.Padding(FMargin(10.0f))
		[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("BrushSettingsHeader", "BRUSH"))
			.Font(PanelFont(10, true))
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeParamSlider(
				LOCTEXT("SculptSize", "Size"),
				LOCTEXT("SculptSizeTip", "Brush radius, relative to the mesh size."),
				[this]()
				{
					const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
					return Viewport.IsValid() ? Viewport->GetSculptBrushSize() : 0.0f;
				},
				[this](float NewValue)
				{
					if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
					{
						Viewport->SetSculptBrushSize(NewValue);
					}
				})
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			MakeParamSlider(
				LOCTEXT("SculptStrength", "Strength"),
				LOCTEXT("SculptStrengthTip", "How strongly each stamp moves the surface."),
				[this]()
				{
					const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
					return Viewport.IsValid() ? Viewport->GetSculptBrushStrength() : 0.0f;
				},
				[this](float NewValue)
				{
					if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
					{
						Viewport->SetSculptBrushStrength(NewValue);
					}
				})
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			MakeParamSlider(
				LOCTEXT("SculptFalloff", "Falloff"),
				LOCTEXT("SculptFalloffTip", "Softness of the brush edge (0 hard, 1 soft)."),
				[this]()
				{
					const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
					return Viewport.IsValid() ? Viewport->GetSculptBrushFalloff() : 0.0f;
				},
				[this](float NewValue)
				{
					if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
					{
						Viewport->SetSculptBrushFalloff(NewValue);
					}
				})
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			// Symmetry only lights up once the tool detects a symmetric base mesh.
			SNew(SCheckBox)
			.IsEnabled_Lambda([this]()
			{
				const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
				return Viewport.IsValid() && Viewport->CanEnableSculptSymmetry();
			})
			.ToolTipText(LOCTEXT("SculptSymmetryTip", "Mirror strokes across the mesh's symmetry plane (available once symmetry is detected)."))
			.IsChecked_Lambda([this]()
			{
				const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
				return Viewport.IsValid() && Viewport->IsSculptSymmetryEnabled()
					? ECheckBoxState::Checked
					: ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this](const ECheckBoxState NewState)
			{
				if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
				{
					Viewport->SetSculptSymmetryEnabled(NewState == ECheckBoxState::Checked);
				}
			})
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SculptSymmetry", "Symmetry"))
				.Font(PanelFont(9))
				.Margin(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
			]
		]
		];
}

TSharedRef<SWidget> SUbstreamSculptPanel::MakeParamSlider(
	const FText& Label,
	const FText& ToolTip,
	TFunction<float()> Getter,
	TFunction<void(float)> Setter)
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				LabelText(Label)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBorder)
				.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.ValueReadout"))
				.Padding(FMargin(6.0f, 2.0f))
				[
					SNew(STextBlock)
					.Font(PanelFont(8))
					.MinDesiredWidth(28.0f)
					.Justification(ETextJustify::Right)
					.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::NumericAccent()))
					.Text_Lambda([Getter]()
					{
						return FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Getter() * 100.0f)));
					})
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSlider)
			.Style(&FUbstreamModelerStyle::Get().GetWidgetStyle<FSliderStyle>(TEXT("Ubstream.Slider")))
			.ToolTipText(ToolTip)
			.MinValue(0.0f)
			.MaxValue(1.0f)
			.Value_Lambda([Getter]() { return Getter(); })
			.OnValueChanged_Lambda([Setter](float NewValue) { Setter(NewValue); })
		];
}

TSharedRef<SWidget> SUbstreamSculptPanel::BuildDetailSection()
{
	return SNew(SBorder)
		.BorderImage(FUbstreamModelerStyle::Get().GetBrush("Ubstream.Group"))
		.Padding(FMargin(10.0f))
		[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("DetailHeader", "SURFACE DETAIL"))
				.Font(PanelFont(10, true))
				.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				// Live triangle count of the current mesh.
				SNew(STextBlock)
				.Font(PanelFont(8))
				.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
				.Text_Lambda([this]()
				{
					const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
					const int32 Tris = Viewport.IsValid() ? Viewport->GetSubjectTriangleCount() : 0;
					return Tris > 0
						? FText::FromString(FString::Printf(TEXT("%s tris"), *FormatTriangleCount(Tris)))
						: FText::GetEmpty();
				})
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SSlider)
				.Style(&FUbstreamModelerStyle::Get().GetWidgetStyle<FSliderStyle>(TEXT("Ubstream.Slider")))
				.ToolTipText(LOCTEXT("DetailSliderTip", "Target triangle budget - higher density lets brushes carve finer detail. Press Remesh to apply."))
				.IsEnabled_Lambda([this]() { const auto Viewport = SoloViewport.Pin(); return Viewport && !Viewport->IsRemeshing(); })
				.MinValue(0.0f)
				.MaxValue(1.0f)
				.Value_Lambda([this]()
				{
					const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
					if (!Viewport.IsValid())
					{
						return 0.5f;
					}
					return TrianglesToSlider(
						Viewport->GetSculptTargetTriangleCount(),
						SUbstreamModelerViewport::GetSculptTargetTriangleMin(),
						SUbstreamModelerViewport::GetSculptTargetTriangleMax());
				})
				.OnValueChanged_Lambda([this](float NewValue)
				{
					if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
					{
						Viewport->SetSculptTargetTriangleCount(SliderToTriangles(
							NewValue,
							SUbstreamModelerViewport::GetSculptTargetTriangleMin(),
							SUbstreamModelerViewport::GetSculptTargetTriangleMax()));
					}
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.VAlign(VAlign_Center)
				.ContentPadding(FMargin(12.0f, 4.0f))
				.ButtonStyle(&FUbstreamModelerStyle::Get(), "Ubstream.PrimaryButton")
				.ToolTipText(LOCTEXT("DetailApplyTip", "Refine the surface in the background. Cancel keeps the current mesh; a completed remesh is undoable."))
				.OnClicked_Lambda([this]()
				{
					if (const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin())
					{
						if (Viewport->IsRemeshing())
						{
							Viewport->CancelRemesh();
							return FReply::Handled();
						}
						FString Error;
						if (!Viewport->RemeshSubjectToTargetDetail(Error))
						{
							FNotificationInfo Info(FText::FromString(Error));
							Info.ExpireDuration = 5.0f;
							if (TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info))
							{
								Notification->SetCompletionState(SNotificationItem::CS_Fail);
							}
						}
					}
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						const auto Viewport = SoloViewport.Pin();
						return Viewport && Viewport->IsRemeshing() ? LOCTEXT("CancelRemesh", "Cancel") : LOCTEXT("DetailApply", "Remesh");
					})
					.Font(PanelFont(8, true))
					.ColorAndOpacity(FSlateColor(FLinearColor::White))
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			SAssignNew(RemeshProgressBar, SProgressBar)
			.Style(&FUbstreamModelerStyle::Get(), "Ubstream.ProgressBar")
			.Percent(0.0f)
			.FillColorAndOpacity(FUbstreamModelerStyle::ActiveAccent())
			.Visibility_Lambda([this]()
			{
				const auto Viewport = SoloViewport.Pin();
				return Viewport && Viewport->IsRemeshing() ? EVisibility::Visible : EVisibility::Collapsed;
			})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(STextBlock).AutoWrapText(true).Font(PanelFont(8))
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::StrongText()))
			.Text_Lambda([this]() { const auto Viewport = SoloViewport.Pin(); return Viewport ? FText::FromString(Viewport->GetRemeshStatus()) : FText::GetEmpty(); })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 3.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Font(PanelFont(8))
			.ColorAndOpacity(FSlateColor(FUbstreamModelerStyle::MutedText()))
			.Text_Lambda([this]()
			{
				const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
				const int32 Target = Viewport.IsValid() ? Viewport->GetSculptTargetTriangleCount() : 0;
				return FText::FromString(FString::Printf(TEXT("Target: %s tris"), *FormatTriangleCount(Target)));
			})
		]
		];
}

bool SUbstreamSculptPanel::CanSculpt() const
{
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	return Viewport.IsValid() && Viewport->CanSculpt();
}

bool SUbstreamSculptPanel::IsSculptActive() const
{
	const TSharedPtr<SUbstreamModelerViewport> Viewport = SoloViewport.Pin();
	return Viewport.IsValid() && Viewport->IsSculptActive();
}

#undef LOCTEXT_NAMESPACE
