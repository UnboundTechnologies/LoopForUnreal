#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Modules/ModuleInterface.h"

UBSTREAMMODELEREDITOR_API DECLARE_LOG_CATEGORY_EXTERN(LogUbstreamModeler, Log, All);

class AActor;
class FSpawnTabArgs;
class FExtender;
class FMenuBuilder;
class FUICommandList;
class SDockTab;
class SUbstreamModelerWidget;
class UMeshComponent;
class UStaticMesh;
class UToolMenu;

class FUbstreamModelerEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * True while the game thread is executing inside FlushRenderingCommands (any
	 * nesting depth). UE 5.8 trips a game-thread FAppTime time-context ensure when a
	 * scene-capture render runs nested inside another flush; capture code polls this
	 * to defer that frame. Maintained via the engine's balanced flush delegates.
	 */
	static bool IsInsideRenderFlush();

private:
	TSharedRef<SDockTab> SpawnModelerTab(const FSpawnTabArgs& Args);
	void RegisterCommands();
	void RegisterMenus();
	void RegisterLevelContextMenuEntries();
	void RegisterContentBrowserExtender();
	void UnregisterContentBrowserExtender();
	void RegisterLevelViewportExtender();
	void UnregisterLevelViewportExtender();
	void ShowFirstRunNotification();
	void OpenModelerTab();
	void OpenModelerTabWithStaticMesh(UStaticMesh* StaticMesh);
	void OpenModelerTabWithActors(TArray<TWeakObjectPtr<AActor>> Actors);
	void OpenModelerTabWithComponents(TArray<TWeakObjectPtr<UMeshComponent>> Components);
	TSharedRef<FExtender> ExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets);
	void AddStaticMeshAssetMenuEntry(FMenuBuilder& MenuBuilder, TArray<FAssetData> SelectedAssets);
	void OpenSoloModelerForAssetSelection(TArray<FAssetData> SelectedAssets);
	TSharedRef<FExtender> ExtendLevelViewportActorMenu(const TSharedRef<FUICommandList> CommandList, const TArray<AActor*> SelectedActors);
	void AddSelectedActorMenuEntry(FMenuBuilder& MenuBuilder, TArray<TWeakObjectPtr<AActor>> SelectedActors);
	void OpenSoloModelerForActorSelection(TArray<TWeakObjectPtr<AActor>> SelectedActors);
	void AddComponentContextMenuEntries(UToolMenu* Menu);
	void OpenSoloModelerForComponentSelection(TArray<TWeakObjectPtr<UMeshComponent>> SelectedComponents);
	UStaticMesh* ResolveFirstStaticMesh(const TArray<FAssetData>& SelectedAssets) const;
	bool ActorSelectionHasRenderableMesh(const TArray<TWeakObjectPtr<AActor>>& SelectedActors) const;
	bool ComponentSelectionHasRenderableMesh(const TArray<TWeakObjectPtr<UMeshComponent>>& SelectedComponents) const;

	TSharedPtr<FUICommandList> PluginCommands;
	TWeakPtr<SUbstreamModelerWidget> ActiveWidget;
	TWeakObjectPtr<UStaticMesh> PendingSoloMesh;
	TArray<TWeakObjectPtr<AActor>> PendingSoloActors;
	TArray<TWeakObjectPtr<UMeshComponent>> PendingSoloComponents;
	FDelegateHandle ContentBrowserAssetExtenderDelegateHandle;
	FDelegateHandle LevelViewportActorExtenderDelegateHandle;
	FDelegateHandle FlushRenderingCommandsStartHandle;
	FDelegateHandle FlushRenderingCommandsEndHandle;
	bool bFirstRunNotificationShown = false;
};
