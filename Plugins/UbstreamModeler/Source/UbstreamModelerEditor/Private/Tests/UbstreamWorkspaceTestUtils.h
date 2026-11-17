#pragma once

#include "Widgets/SUbstreamModelerWidget.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace UbstreamWorkspaceTestUtils
{
	// Exercise the real workspace layout offline. Change only this test widget's
	// visibility: never authenticate the account service or touch stored keys.
	inline bool ShowOfflineWorkspace(SUbstreamModelerWidget& Widget)
	{
		FChildren* Children = Widget.GetChildren();
		if (!Children || Children->Num() != 1) return false;
		TSharedRef<SWidget> Overlay = Children->GetChildAt(0);
		if (Overlay->GetType() != FName(TEXT("SOverlay"))) return false;
		FChildren* Layers = Overlay->GetChildren();
		if (!Layers || Layers->Num() != 2) return false;
		Layers->GetChildAt(0)->SetVisibility(EVisibility::Visible);
		Layers->GetChildAt(1)->SetVisibility(EVisibility::Collapsed);
		return true;
	}
}
#endif
