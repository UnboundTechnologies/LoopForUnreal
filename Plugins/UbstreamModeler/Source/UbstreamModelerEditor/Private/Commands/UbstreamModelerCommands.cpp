#include "Commands/UbstreamModelerCommands.h"

#define LOCTEXT_NAMESPACE "FUbstreamModelerCommands"

void FUbstreamModelerCommands::RegisterCommands()
{
	UI_COMMAND(
		OpenPluginWindow,
		"Unbound Loop",
		"Open the Unbound Loop companion panel.",
		EUserInterfaceActionType::Button,
		FInputChord(EModifierKey::Shift, EKeys::U));
}

#undef LOCTEXT_NAMESPACE
