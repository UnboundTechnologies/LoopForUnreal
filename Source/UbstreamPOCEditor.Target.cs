using UnrealBuildTool;
using System.Collections.Generic;

public class UbstreamPOCEditorTarget : TargetRules
{
	public UbstreamPOCEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("UbstreamPOC");
	}
}
