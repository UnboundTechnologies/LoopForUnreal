using UnrealBuildTool;

public class UbstreamModelerEditor : ModuleRules
{
	public UbstreamModelerEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		if (Target.Platform == UnrealTargetPlatform.Mac) PublicFrameworks.Add("Security");
		if (Target.Platform == UnrealTargetPlatform.Win64) PublicSystemLibraries.Add("Advapi32.lib");

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"UbstreamModelerRuntime"
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"LevelEditor",
			"ToolMenus",
			"InputCore",
			"AdvancedPreviewScene",
			"AppFramework",
			"AssetRegistry",
			"AssetTools",
			"ContentBrowser",
			"DesktopPlatform",
			"DeveloperSettings",
			"DynamicMesh",
			"Draco",
			"EditorFramework",
			"EditorInteractiveToolsFramework",
			"GeometryCore",
			"GeometryFramework",
			"HTTP",
			"ImageCore",
			"InteractiveToolsFramework",
			"Json",
			"JsonUtilities",
			"MeshConversion",
			"MeshDescription",
			"MeshModelingTools",
			"ModelingComponents",
			"ModelingOperators",
			"ModelingComponentsEditorOnly",
			"ModelingToolsEditorMode",
			"Projects",
			"PropertyEditor",
			"StaticMeshDescription",
			"RenderCore",
			"RHI",
			"Settings",
			"WebSockets"
		});
	}
}
