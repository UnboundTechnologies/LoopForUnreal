#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Styling/UbstreamModelerStyle.h"

class FUbstreamModelerCommands final : public TCommands<FUbstreamModelerCommands>
{
public:
	FUbstreamModelerCommands()
		: TCommands<FUbstreamModelerCommands>(
			TEXT("UbstreamModeler"),
			NSLOCTEXT("Contexts", "UbstreamModeler", "Unbound Loop"),
			NAME_None,
			FUbstreamModelerStyle::GetStyleSetName())
	{
	}

	virtual void RegisterCommands() override;

	TSharedPtr<FUICommandInfo> OpenPluginWindow;
};
