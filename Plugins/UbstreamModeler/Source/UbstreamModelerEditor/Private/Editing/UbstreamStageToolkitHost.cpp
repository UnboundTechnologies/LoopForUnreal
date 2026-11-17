#include "Editing/UbstreamStageToolkitHost.h"

#include "AssetEditorModeManager.h"
#include "PreviewScene.h"
#include "Widgets/SNullWidget.h"

void FUbstreamStageToolkitHost::Initialize(
	FAssetEditorModeManager* InModeManager,
	FPreviewScene* InPreviewScene,
	const TSharedRef<SWidget>& InParentWidget)
{
	ModeManager = InModeManager;
	PreviewScene = InPreviewScene;
	ParentWidget = InParentWidget;
}

TSharedRef<SWidget> FUbstreamStageToolkitHost::GetParentWidget()
{
	const TSharedPtr<SWidget> Parent = ParentWidget.Pin();
	return Parent.IsValid() ? Parent.ToSharedRef() : SNullWidget::NullWidget;
}

void FUbstreamStageToolkitHost::OnToolkitHostingStarted(const TSharedRef<IToolkit>& Toolkit)
{
	HostedToolkit = Toolkit;
	OnHostedToolkitChanged.Broadcast();
}

void FUbstreamStageToolkitHost::OnToolkitHostingFinished(const TSharedRef<IToolkit>& Toolkit)
{
	if (HostedToolkit == Toolkit)
	{
		HostedToolkit.Reset();
	}
	OnHostedToolkitChanged.Broadcast();
}

UWorld* FUbstreamStageToolkitHost::GetWorld() const
{
	return PreviewScene ? PreviewScene->GetWorld() : nullptr;
}

FEditorModeTools& FUbstreamStageToolkitHost::GetEditorModeManager() const
{
	check(ModeManager);
	return *ModeManager;
}

void FUbstreamStageToolkitHost::AddViewportOverlayWidget(TSharedRef<SWidget> InOverlaidWidget, int32 ZOrder, TSharedPtr<IAssetViewport> InViewport)
{
	OnOverlayWidgetAdded.Broadcast(InOverlaidWidget);
}

void FUbstreamStageToolkitHost::RemoveViewportOverlayWidget(TSharedRef<SWidget> InOverlaidWidget, TSharedPtr<IAssetViewport> InViewport)
{
	OnOverlayWidgetRemoved.Broadcast(InOverlaidWidget);
}
