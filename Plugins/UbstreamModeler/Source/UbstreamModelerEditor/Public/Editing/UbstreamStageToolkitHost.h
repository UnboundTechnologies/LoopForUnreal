#pragma once

#include "CoreMinimal.h"
#include "Toolkits/IToolkitHost.h"

class FAssetEditorModeManager;
class FPreviewScene;
class IToolkit;
class SWidget;

/**
 * Minimal toolkit host for the Unbound Loop stage. Lets toolkit-based editor
 * modes (most importantly Unreal's own Modeling Mode) run on the stage's
 * FAssetEditorModeManager: the mode's toolkit initializes against this host,
 * its inline content is surfaced into the stage panel as-is, and viewport
 * overlay widgets (tool accept/cancel) are forwarded to the stage viewport.
 */
class FUbstreamStageToolkitHost : public IToolkitHost, public TSharedFromThis<FUbstreamStageToolkitHost>
{
public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnOverlayWidgetChanged, const TSharedRef<SWidget>&);

	void Initialize(FAssetEditorModeManager* InModeManager, FPreviewScene* InPreviewScene, const TSharedRef<SWidget>& InParentWidget);

	// IToolkitHost interface
	virtual TSharedRef<SWidget> GetParentWidget() override;
	virtual void BringToFront() override {}
	virtual TSharedPtr<FTabManager> GetTabManager() const override { return nullptr; }
	virtual void OnToolkitHostingStarted(const TSharedRef<IToolkit>& Toolkit) override;
	virtual void OnToolkitHostingFinished(const TSharedRef<IToolkit>& Toolkit) override;
	virtual UWorld* GetWorld() const override;
	virtual FEditorModeTools& GetEditorModeManager() const override;
	virtual UTypedElementCommonActions* GetCommonActions() const override { return nullptr; }
	virtual FName GetStatusBarName() const override { return NAME_None; }
	virtual void AddViewportOverlayWidget(TSharedRef<SWidget> InOverlaidWidget, int32 ZOrder, TSharedPtr<IAssetViewport> InViewport = nullptr) override;
	using IToolkitHost::AddViewportOverlayWidget;
	virtual void RemoveViewportOverlayWidget(TSharedRef<SWidget> InOverlaidWidget, TSharedPtr<IAssetViewport> InViewport = nullptr) override;
	virtual FOnActiveViewportChanged& OnActiveViewportChanged() override { return ActiveViewportChangedDelegate; }

	TSharedPtr<IToolkit> GetHostedToolkit() const { return HostedToolkit; }

	/** Fires when a toolkit starts or stops being hosted. */
	FSimpleMulticastDelegate OnHostedToolkitChanged;
	/** Fired so the stage viewport can attach/detach tool overlay widgets. */
	FOnOverlayWidgetChanged OnOverlayWidgetAdded;
	FOnOverlayWidgetChanged OnOverlayWidgetRemoved;

private:
	FAssetEditorModeManager* ModeManager = nullptr;
	FPreviewScene* PreviewScene = nullptr;
	TWeakPtr<SWidget> ParentWidget;
	TSharedPtr<IToolkit> HostedToolkit;
	FOnActiveViewportChanged ActiveViewportChangedDelegate;
};
