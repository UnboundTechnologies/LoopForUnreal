#include "UbstreamModelerEditorModule.h"
#include "Auth/UbstreamAuthService.h"

#include "Commands/UbstreamModelerCommands.h"
#include "Components/StaticMeshComponent.h"
#include "ContentBrowserModule.h"
#include "Engine/StaticMesh.h"
#include "Utils/UbstreamMeshComponentUtils.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GameFramework/Actor.h"
#include "LevelEditor.h"
#include "LevelEditorContextMenu.h"
#include "LevelEditorMenuContext.h"
#include "RenderingThread.h"
#include "Styling/UbstreamModelerStyle.h"
#include "ToolMenus.h"

#include <atomic>
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SUbstreamModelerWidget.h"

#define LOCTEXT_NAMESPACE "FUbstreamModelerEditorModule"

DEFINE_LOG_CATEGORY(LogUbstreamModeler);

namespace
{
	const FName UbstreamModelerTabName(TEXT("UbstreamModeler"));

	// Depth of nested FlushRenderingCommands calls on the game thread, maintained
	// via the engine's balanced OnFlushRenderingCommandsStart/End delegates. See
	// FUbstreamModelerEditorModule::IsInsideRenderFlush.
	std::atomic<int32> GUbstreamRenderFlushDepth{0};
}

void FUbstreamModelerEditorModule::StartupModule()
{
	// Build stamp: the single source of truth for "which binary is this editor
	// actually running" when live behavior does not match a fresh build.
	UE_LOG(LogUbstreamModeler, Log, TEXT("UbstreamModelerEditor module built %hs %hs"), __DATE__, __TIME__);

	FUbstreamModelerStyle::Initialize();
	FUbstreamModelerCommands::Register();
	RegisterCommands();

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		UbstreamModelerTabName,
		FOnSpawnTab::CreateRaw(this, &FUbstreamModelerEditorModule::SpawnModelerTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Unbound Loop"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Open the Unbound Loop workspace"))
		.SetIcon(FSlateIcon(FUbstreamModelerStyle::GetStyleSetName(), TEXT("UbstreamModeler.Icon20")))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUbstreamModelerEditorModule::RegisterMenus));
	RegisterContentBrowserExtender();
	RegisterLevelViewportExtender();

	// UE 5.8 added a game-thread FAppTime time-context ensure inside the scene-
	// capture render path (FScene::Update -> UpdateReflectionSceneData ->
	// FApp::GetCurrentTime). A streaming capture uses the immediate
	// USceneCaptureComponent2D::CaptureScene(), which flushes rendering commands
	// internally; when that flush nests inside another FlushRenderingCommands the
	// capture render executes on the render thread without an inherited time context
	// and trips the ensure. It is non-fatal (the editor keeps running) but pops the
	// crash reporter. Track flush depth so capture code can defer while a flush is in
	// progress. The delegates are balanced (both broadcast once per flush), so the
	// counter cannot drift.
	FlushRenderingCommandsStartHandle = FCoreRenderDelegates::OnFlushRenderingCommandsStart.AddLambda([]
	{
		GUbstreamRenderFlushDepth.fetch_add(1, std::memory_order_relaxed);
	});
	FlushRenderingCommandsEndHandle = FCoreRenderDelegates::OnFlushRenderingCommandsEnd.AddLambda([]
	{
		GUbstreamRenderFlushDepth.fetch_sub(1, std::memory_order_relaxed);
	});
}

void FUbstreamModelerEditorModule::ShutdownModule()
{
	FUbstreamAuthService::Get().Shutdown();
	// Remove the flush-depth trackers before this module's code is unloaded — a
	// dangling lambda would crash if the engine flushes during teardown.
	FCoreRenderDelegates::OnFlushRenderingCommandsStart.Remove(FlushRenderingCommandsStartHandle);
	FCoreRenderDelegates::OnFlushRenderingCommandsEnd.Remove(FlushRenderingCommandsEndHandle);
	FlushRenderingCommandsStartHandle.Reset();
	FlushRenderingCommandsEndHandle.Reset();

	UnregisterLevelViewportExtender();
	UnregisterContentBrowserExtender();
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UbstreamModelerTabName);
	FUbstreamModelerCommands::Unregister();
	FUbstreamModelerStyle::Shutdown();
}

bool FUbstreamModelerEditorModule::IsInsideRenderFlush()
{
	return GUbstreamRenderFlushDepth.load(std::memory_order_relaxed) > 0;
}

TSharedRef<SDockTab> FUbstreamModelerEditorModule::SpawnModelerTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SUbstreamModelerWidget> Widget = SNew(SUbstreamModelerWidget);
	ActiveWidget = Widget;
	if (PendingSoloMesh.IsValid())
	{
		Widget->LoadSoloMesh(PendingSoloMesh.Get());
		PendingSoloMesh.Reset();
	}
	else if (!PendingSoloComponents.IsEmpty())
	{
		Widget->LoadSoloComponents(PendingSoloComponents);
		PendingSoloComponents.Reset();
	}
	else if (!PendingSoloActors.IsEmpty())
	{
		Widget->LoadSoloActors(PendingSoloActors);
		PendingSoloActors.Reset();
	}

	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.OnCanCloseTab(SDockTab::FCanCloseTab::CreateSP(Widget, &SUbstreamModelerWidget::CanCloseWorkspace))
		[
			Widget
		];
}

void FUbstreamModelerEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
	FToolMenuSection& Section = WindowMenu->FindOrAddSection(TEXT("WindowLayout"));
	FToolMenuEntry MenuEntry = FToolMenuEntry::InitMenuEntry(
		FUbstreamModelerCommands::Get().OpenPluginWindow,
		LOCTEXT("OpenModelerLabel", "Unbound Loop"),
		LOCTEXT("OpenModelerTooltip", "Open the Unbound Loop workspace"),
		FSlateIcon(FUbstreamModelerStyle::GetStyleSetName(), TEXT("UbstreamModeler.Icon20")));
	MenuEntry.SetCommandList(PluginCommands);
	Section.AddEntry(MenuEntry);

	UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.User"));
	FToolMenuSection& ToolbarSection = ToolbarMenu->FindOrAddSection(TEXT("UnboundLoop"));
	FToolMenuEntry ToolbarEntry = FToolMenuEntry::InitToolBarButton(
		TEXT("OpenUnboundLoopToolbar"),
		FUIAction(FExecuteAction::CreateRaw(this, &FUbstreamModelerEditorModule::OpenModelerTab)),
		LOCTEXT("OpenModelerToolbarLabel", "Unbound Loop"),
		LOCTEXT("OpenModelerToolbarTooltip", "Open the isolated Unbound renderer and Modeling Mode workspace"),
		FSlateIcon(FUbstreamModelerStyle::GetStyleSetName(), TEXT("UbstreamModeler.Icon20")));
	ToolbarSection.AddEntry(ToolbarEntry);

	RegisterLevelContextMenuEntries();
}

void FUbstreamModelerEditorModule::RegisterLevelContextMenuEntries()
{
	FLevelEditorContextMenu::RegisterComponentContextMenu();

	UToolMenu* ComponentMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.ComponentContextMenu"));
	ComponentMenu->AddDynamicSection(
		TEXT("UbstreamComponentContext"),
		FNewToolMenuDelegate::CreateRaw(this, &FUbstreamModelerEditorModule::AddComponentContextMenuEntries));
}

void FUbstreamModelerEditorModule::RegisterCommands()
{
	PluginCommands = MakeShared<FUICommandList>();
	PluginCommands->MapAction(
		FUbstreamModelerCommands::Get().OpenPluginWindow,
		FExecuteAction::CreateRaw(this, &FUbstreamModelerEditorModule::OpenModelerTab));

	if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		LevelEditorModule.GetGlobalLevelEditorActions()->Append(PluginCommands.ToSharedRef());
	}
}

void FUbstreamModelerEditorModule::RegisterContentBrowserExtender()
{
	if (ContentBrowserAssetExtenderDelegateHandle.IsValid())
	{
		return;
	}

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
	Extenders.Add(FContentBrowserMenuExtender_SelectedAssets::CreateRaw(this, &FUbstreamModelerEditorModule::ExtendContentBrowserAssetSelectionMenu));
	ContentBrowserAssetExtenderDelegateHandle = Extenders.Last().GetHandle();
}

void FUbstreamModelerEditorModule::UnregisterContentBrowserExtender()
{
	if (!ContentBrowserAssetExtenderDelegateHandle.IsValid() || !FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
	{
		return;
	}

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
	Extenders.RemoveAll([this](const FContentBrowserMenuExtender_SelectedAssets& Delegate)
	{
		return Delegate.GetHandle() == ContentBrowserAssetExtenderDelegateHandle;
	});
	ContentBrowserAssetExtenderDelegateHandle.Reset();
}

void FUbstreamModelerEditorModule::RegisterLevelViewportExtender()
{
	if (LevelViewportActorExtenderDelegateHandle.IsValid())
	{
		return;
	}

	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
	TArray<FLevelEditorModule::FLevelViewportMenuExtender_SelectedActors>& Extenders = LevelEditorModule.GetAllLevelViewportContextMenuExtenders();
	Extenders.Add(FLevelEditorModule::FLevelViewportMenuExtender_SelectedActors::CreateRaw(this, &FUbstreamModelerEditorModule::ExtendLevelViewportActorMenu));
	LevelViewportActorExtenderDelegateHandle = Extenders.Last().GetHandle();
}

void FUbstreamModelerEditorModule::UnregisterLevelViewportExtender()
{
	if (!LevelViewportActorExtenderDelegateHandle.IsValid() || !FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
	{
		return;
	}

	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
	TArray<FLevelEditorModule::FLevelViewportMenuExtender_SelectedActors>& Extenders = LevelEditorModule.GetAllLevelViewportContextMenuExtenders();
	Extenders.RemoveAll([this](const FLevelEditorModule::FLevelViewportMenuExtender_SelectedActors& Delegate)
	{
		return Delegate.GetHandle() == LevelViewportActorExtenderDelegateHandle;
	});
	LevelViewportActorExtenderDelegateHandle.Reset();
}

TSharedRef<FExtender> FUbstreamModelerEditorModule::ExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();
	if (ResolveFirstStaticMesh(SelectedAssets))
	{
		Extender->AddMenuExtension(
			TEXT("GetAssetActions"),
			EExtensionHook::After,
			nullptr,
			FMenuExtensionDelegate::CreateRaw(this, &FUbstreamModelerEditorModule::AddStaticMeshAssetMenuEntry, SelectedAssets));
	}
	return Extender;
}

void FUbstreamModelerEditorModule::AddStaticMeshAssetMenuEntry(FMenuBuilder& MenuBuilder, TArray<FAssetData> SelectedAssets)
{
	MenuBuilder.AddMenuEntry(
		LOCTEXT("OpenInSoloModelerLabel", "Open in Unbound Loop"),
		LOCTEXT("OpenInSoloModelerTooltip", "Open this Static Mesh in a clean isolated Unbound renderer for RGB/depth capture."),
		FSlateIcon(FUbstreamModelerStyle::GetStyleSetName(), TEXT("UbstreamModeler.Icon20")),
		FUIAction(FExecuteAction::CreateRaw(this, &FUbstreamModelerEditorModule::OpenSoloModelerForAssetSelection, SelectedAssets)));
}

void FUbstreamModelerEditorModule::OpenSoloModelerForAssetSelection(TArray<FAssetData> SelectedAssets)
{
	OpenModelerTabWithStaticMesh(ResolveFirstStaticMesh(SelectedAssets));
}

UStaticMesh* FUbstreamModelerEditorModule::ResolveFirstStaticMesh(const TArray<FAssetData>& SelectedAssets) const
{
	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(AssetData.GetAsset()))
		{
			return StaticMesh;
		}
	}
	return nullptr;
}

TSharedRef<FExtender> FUbstreamModelerEditorModule::ExtendLevelViewportActorMenu(const TSharedRef<FUICommandList> CommandList, const TArray<AActor*> SelectedActors)
{
	TArray<TWeakObjectPtr<AActor>> WeakActors;
	WeakActors.Reserve(SelectedActors.Num());
	for (AActor* Actor : SelectedActors)
	{
		if (IsValid(Actor))
		{
			WeakActors.Add(Actor);
		}
	}

	TSharedRef<FExtender> Extender = MakeShared<FExtender>();
	if (ActorSelectionHasRenderableMesh(WeakActors))
	{
			Extender->AddMenuExtension(
				TEXT("ActorGeneral"),
				EExtensionHook::After,
				nullptr,
				FMenuExtensionDelegate::CreateRaw(this, &FUbstreamModelerEditorModule::AddSelectedActorMenuEntry, WeakActors));
	}
	return Extender;
}

void FUbstreamModelerEditorModule::AddSelectedActorMenuEntry(FMenuBuilder& MenuBuilder, TArray<TWeakObjectPtr<AActor>> SelectedActors)
{
	MenuBuilder.AddMenuEntry(
		LOCTEXT("OpenSelectedActorsInSoloModelerLabel", "Open Selection in Unbound Loop"),
		LOCTEXT("OpenSelectedActorsInSoloModelerTooltip", "Open the selected actor or static mesh components in a clean isolated Unbound renderer for RGB/depth capture."),
		FSlateIcon(FUbstreamModelerStyle::GetStyleSetName(), TEXT("UbstreamModeler.Icon20")),
		FUIAction(FExecuteAction::CreateRaw(this, &FUbstreamModelerEditorModule::OpenSoloModelerForActorSelection, SelectedActors)));
}

void FUbstreamModelerEditorModule::OpenSoloModelerForActorSelection(TArray<TWeakObjectPtr<AActor>> SelectedActors)
{
	OpenModelerTabWithActors(SelectedActors);
}

void FUbstreamModelerEditorModule::AddComponentContextMenuEntries(UToolMenu* Menu)
{
	if (!Menu)
	{
		return;
	}

	const ULevelEditorContextMenuContext* Context = Menu->FindContext<ULevelEditorContextMenuContext>();
	if (!Context)
	{
		return;
	}

	TArray<TWeakObjectPtr<UMeshComponent>> MeshComponents;
	for (UActorComponent* Component : Context->SelectedComponents)
	{
		UMeshComponent* MeshComponent = Cast<UMeshComponent>(Component);
		if (UbstreamMeshComponents::HasRenderableGeometry(MeshComponent))
		{
			MeshComponents.Add(MeshComponent);
		}
	}

	if (MeshComponents.IsEmpty())
	{
		return;
	}

	FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("UnboundLoop"));
	Section.Label = LOCTEXT("UnboundLoopContextSection", "Unbound Loop");
	Section.AddMenuEntry(
		TEXT("OpenSelectedComponentsInUnboundLoop"),
		MeshComponents.Num() == 1
			? LOCTEXT("OpenSelectedComponentInUnboundLoopLabel", "Open Component in Unbound Loop")
			: LOCTEXT("OpenSelectedComponentsInUnboundLoopLabel", "Open Components in Unbound Loop"),
		LOCTEXT("OpenSelectedComponentsInUnboundLoopTooltip", "Open the selected mesh component(s) in the clean isolated Unbound renderer for RGB/depth capture."),
		FSlateIcon(FUbstreamModelerStyle::GetStyleSetName(), TEXT("UbstreamModeler.Icon20")),
		FUIAction(FExecuteAction::CreateRaw(this, &FUbstreamModelerEditorModule::OpenSoloModelerForComponentSelection, MeshComponents)));
}

void FUbstreamModelerEditorModule::OpenSoloModelerForComponentSelection(TArray<TWeakObjectPtr<UMeshComponent>> SelectedComponents)
{
	OpenModelerTabWithComponents(SelectedComponents);
}

bool FUbstreamModelerEditorModule::ActorSelectionHasRenderableMesh(const TArray<TWeakObjectPtr<AActor>>& SelectedActors) const
{
	for (const TWeakObjectPtr<AActor>& ActorPtr : SelectedActors)
	{
		if (UbstreamMeshComponents::ActorHasRenderableMeshComponent(ActorPtr.Get()))
		{
			return true;
		}
	}
	return false;
}

bool FUbstreamModelerEditorModule::ComponentSelectionHasRenderableMesh(const TArray<TWeakObjectPtr<UMeshComponent>>& SelectedComponents) const
{
	for (const TWeakObjectPtr<UMeshComponent>& ComponentPtr : SelectedComponents)
	{
		if (UbstreamMeshComponents::HasRenderableGeometry(ComponentPtr.Get()))
		{
			return true;
		}
	}
	return false;
}

void FUbstreamModelerEditorModule::ShowFirstRunNotification()
{
	if (bFirstRunNotificationShown)
	{
		return;
	}

	bFirstRunNotificationShown = true;

	FNotificationInfo Info(LOCTEXT("FirstRunNotificationText", "Unbound Loop is ready. Load an isolated subject or enter Modeling Mode, then type a prompt."));
	Info.ExpireDuration = 6.0f;
	Info.bFireAndForget = true;
	Info.Image = FUbstreamModelerStyle::Get().GetBrush(TEXT("UbstreamModeler.Icon20"));
	FSlateNotificationManager::Get().AddNotification(Info);
}

void FUbstreamModelerEditorModule::OpenModelerTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(UbstreamModelerTabName);
	ShowFirstRunNotification();
}

void FUbstreamModelerEditorModule::OpenModelerTabWithActors(TArray<TWeakObjectPtr<AActor>> Actors)
{
	if (!ActorSelectionHasRenderableMesh(Actors))
	{
		OpenModelerTab();
		return;
	}

	PendingSoloMesh.Reset();
	PendingSoloComponents.Reset();
	PendingSoloActors = Actors;
	const bool bHadActiveWidget = ActiveWidget.Pin().IsValid();
	FGlobalTabmanager::Get()->TryInvokeTab(UbstreamModelerTabName);
	if (bHadActiveWidget)
	{
		if (TSharedPtr<SUbstreamModelerWidget> Widget = ActiveWidget.Pin())
		{
			Widget->LoadSoloActors(Actors);
			PendingSoloActors.Reset();
		}
	}
	ShowFirstRunNotification();
}

void FUbstreamModelerEditorModule::OpenModelerTabWithComponents(TArray<TWeakObjectPtr<UMeshComponent>> Components)
{
	if (!ComponentSelectionHasRenderableMesh(Components))
	{
		OpenModelerTab();
		return;
	}

	PendingSoloMesh.Reset();
	PendingSoloActors.Reset();
	PendingSoloComponents = Components;
	const bool bHadActiveWidget = ActiveWidget.Pin().IsValid();
	FGlobalTabmanager::Get()->TryInvokeTab(UbstreamModelerTabName);
	if (bHadActiveWidget)
	{
		if (TSharedPtr<SUbstreamModelerWidget> Widget = ActiveWidget.Pin())
		{
			Widget->LoadSoloComponents(Components);
			PendingSoloComponents.Reset();
		}
	}
	ShowFirstRunNotification();
}

void FUbstreamModelerEditorModule::OpenModelerTabWithStaticMesh(UStaticMesh* StaticMesh)
{
	if (!IsValid(StaticMesh))
	{
		OpenModelerTab();
		return;
	}

	PendingSoloMesh = StaticMesh;
	PendingSoloActors.Reset();
	PendingSoloComponents.Reset();
	const bool bHadActiveWidget = ActiveWidget.Pin().IsValid();
	FGlobalTabmanager::Get()->TryInvokeTab(UbstreamModelerTabName);
	if (bHadActiveWidget)
	{
		if (TSharedPtr<SUbstreamModelerWidget> Widget = ActiveWidget.Pin())
		{
			Widget->LoadSoloMesh(StaticMesh);
			PendingSoloMesh.Reset();
		}
	}
	ShowFirstRunNotification();
}

IMPLEMENT_MODULE(FUbstreamModelerEditorModule, UbstreamModelerEditor)

#undef LOCTEXT_NAMESPACE
