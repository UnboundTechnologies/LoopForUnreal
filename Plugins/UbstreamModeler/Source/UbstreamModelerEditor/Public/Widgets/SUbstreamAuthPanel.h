#pragma once
#include "Widgets/SCompoundWidget.h"

/** Shared sign-in card and compact account settings content. */
class SUbstreamAuthPanel final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUbstreamAuthPanel) {}
		SLATE_ARGUMENT(bool, AccountMode)
	SLATE_END_ARGS()
	void Construct(const FArguments& InArgs);
private:
	FReply ConnectClicked();
	TSharedPtr<class SEditableTextBox> KeyInput;
};
