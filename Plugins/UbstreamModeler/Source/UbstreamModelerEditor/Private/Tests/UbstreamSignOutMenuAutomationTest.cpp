#include "Auth/UbstreamAuthService.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SUbstreamModelerWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	void PumpSlate(int32 Frames = 8)
	{
		for (int32 I = 0; I < Frames; ++I)
		{
			FSlateApplication::Get().PumpMessages();
			FTSTicker::GetCoreTicker().Tick(0.02f); // Deferred work, such as the menu's sign-out.
			FSlateApplication::Get().Tick();
			FPlatformProcess::Sleep(0.02f);
		}
	}

	/**
	 * The widget of type T whose subtree holds a text block reading Text, searching Root's
	 * window and its popup child windows only. The editor may have restored its own copy of
	 * the plugin tab elsewhere; that copy must not be touched.
	 */
	template <typename T>
	TSharedPtr<T> FindByText(const TSharedRef<SWindow>& Root, const FString& TypeName, const FString& Text)
	{
		TSharedPtr<T> Found;
		TFunction<void(const TSharedRef<SWidget>&)> Visit = [&](const TSharedRef<SWidget>& Widget)
		{
			if (Found) return;
			if (Widget->GetType() == TEXT("STextBlock") && StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString() == Text)
			{
				for (TSharedPtr<SWidget> Parent = Widget->GetParentWidget(); Parent; Parent = Parent->GetParentWidget())
				{
					if (Parent->GetType() == FName(*TypeName)) { Found = StaticCastSharedPtr<T>(Parent); return; }
				}
			}
			FChildren* Children = Widget->GetAllChildren();
			for (int32 I = 0; Children && I < Children->Num() && !Found; ++I) Visit(Children->GetChildAt(I));
		};
		TFunction<void(const TSharedRef<SWindow>&)> VisitWindow = [&](const TSharedRef<SWindow>& Window)
		{
			Visit(Window);
			for (const TSharedRef<SWindow>& Child : Window->GetChildWindows()) VisitWindow(Child);
		};
		VisitWindow(Root);
		return Found;
	}

	/** A button labelled Text that a user could see: reachable from a window through visible widgets only. */
	bool IsShown(const TSharedRef<SWindow>& Root, const FString& Text)
	{
		const TSharedPtr<SButton> Button = FindByText<SButton>(Root, TEXT("SButton"), Text);
		FWidgetPath Path;
		return Button.IsValid() && FSlateApplication::Get().GeneratePathToWidgetUnchecked(Button.ToSharedRef(), Path, EVisibility::Visible);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamSignOutMenuTest,
	"Ubstream.Auth.SignOutFromSettingsMenu", EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FUbstreamSignOutMenuTest::RunTest(const FString&)
{
	auto& Auth = FUbstreamAuthService::Get();
	if (Auth.HasKey() || Auth.IsBusy() || Auth.IsAuthenticated()) { AddError(TEXT("Run this test in an editor with no signed-in account.")); return false; }
	if (Auth.bInitialized) Auth.Shutdown();
	// Isolated fixture: a random origin, so the user's real saved key is never read or removed.
	Auth.bInitialized = true;
	Auth.Origin = TEXT("fixture://unreal-signout-test/") + FGuid::NewGuid().ToString();
	ON_SCOPE_EXIT { Auth.Shutdown(); };
	Auth.ApiKey = TEXT("ubue_0123456789abcdef0123_abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG");
	Auth.bVerified = true;
	Auth.Account = TEXT("alex@example.test");
	Auth.KeyLabel = TEXT("Unreal Editor · a1b2c3");
	Auth.NextVerificationAt = Auth.Now() + 900.0;
	Auth.VerifiedUntil = Auth.NextVerificationAt + 60.0;
	if (!TestTrue(TEXT("Fixture starts signed in"), Auth.IsAuthenticated())) return false;

	// A key revoked or replaced in the browser must be noticed on return to the
	// editor, not at the next 15-minute check. No ticker runs in this fixture, so
	// only the schedule is checked; no request is sent.
	Auth.HandleApplicationActivationChanged(false);
	TestTrue(TEXT("Leaving the editor schedules nothing"), Auth.NextVerificationAt > Auth.Now() + 60.0);
	Auth.HandleApplicationActivationChanged(true);
	TestTrue(TEXT("Returning to the editor rechecks the key now"), Auth.NextVerificationAt <= Auth.Now());
	Auth.NextVerificationAt = Auth.Now() + 900.0;
	Auth.LastVerificationStartedAt = Auth.Now();
	Auth.HandleApplicationActivationChanged(true);
	TestTrue(TEXT("Quick refocus does not repeat a check just made"), Auth.NextVerificationAt > Auth.Now() + 60.0);
	Auth.bAuthRejected = true;
	Auth.LastVerificationStartedAt = -1.0e9;
	Auth.HandleApplicationActivationChanged(true);
	TestTrue(TEXT("A rejected key is not retried on focus"), Auth.NextVerificationAt > Auth.Now() + 60.0);
	Auth.bAuthRejected = false;

	TSharedRef<SWindow> Window = SNew(SWindow).Title(FText::FromString(TEXT("Unbound sign-out check"))).ClientSize(FVector2D(1280, 900));
	Window->SetContent(SNew(SUbstreamModelerWidget));
	FSlateApplication::Get().AddWindow(Window, true);
	ON_SCOPE_EXIT { Window->RequestDestroyWindow(); };
	PumpSlate();

	const TSharedPtr<SButton> Settings = FindByText<SButton>(Window, TEXT("SButton"), TEXT("Settings"));
	if (!TestNotNull(TEXT("Settings button exists"), Settings.Get())) return false;
	TestFalse(TEXT("Account panel starts closed"), IsShown(Window, TEXT("Remove key & sign out")));
	Settings->SimulateClick();
	PumpSlate();
	TestTrue(TEXT("Settings opens the account panel"), IsShown(Window, TEXT("Remove key & sign out")));
	Settings->SimulateClick();
	PumpSlate();
	TestFalse(TEXT("Settings toggles the account panel closed"), IsShown(Window, TEXT("Remove key & sign out")));
	Settings->SimulateClick();
	PumpSlate();

	const TSharedPtr<SButton> SignOut = FindByText<SButton>(Window, TEXT("SButton"), TEXT("Remove key & sign out"));
	if (!TestNotNull(TEXT("Account panel shows Remove key & sign out"), SignOut.Get())) return false;
	TestFalse(TEXT("Sign-in screen is hidden while signed in"), IsShown(Window, TEXT("Continue in browser  ↗")));
	SignOut->SimulateClick();
	PumpSlate();

	TestFalse(TEXT("Clicking signs out"), Auth.IsAuthenticated());
	TestFalse(TEXT("Clicking forgets the key"), Auth.HasKey());
	TestFalse(TEXT("Account panel is gone after sign-out"), IsShown(Window, TEXT("Remove key & sign out")));
	TestTrue(TEXT("Sign-in screen appears after sign-out"), IsShown(Window, TEXT("Continue in browser  ↗")));
	AddInfo(FString::Printf(TEXT("After click: authenticated=%d hasKey=%d rejected=%d message=\"%s\""),
		Auth.IsAuthenticated(), Auth.HasKey(), Auth.bAuthRejected, *Auth.GetMessage()));

	// A key revoked in the browser is rejected while the account panel is open.
	Auth.ApiKey = TEXT("ubue_0123456789abcdef0123_abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG");
	Auth.bVerified = true; Auth.bAuthRejected = false;
	Auth.Account = TEXT("alex@example.test");
	Auth.NextVerificationAt = Auth.Now() + 900.0;
	Auth.VerifiedUntil = Auth.NextVerificationAt + 60.0;
	PumpSlate();
	TestFalse(TEXT("Signing back in starts with the panel closed"), IsShown(Window, TEXT("Remove key & sign out")));
	Settings->SimulateClick();
	PumpSlate();
	if (TestTrue(TEXT("Account panel reopens when signed in again"), IsShown(Window, TEXT("Remove key & sign out"))))
	{
		Auth.HandleVerificationResponse(Auth.Generation, false, 401, TEXT("{}"), true);
		PumpSlate();
		TestFalse(TEXT("Revoked key signs out"), Auth.IsAuthenticated());
		TestFalse(TEXT("Revoked key removes the account panel"), IsShown(Window, TEXT("Remove key & sign out")));
		TestTrue(TEXT("Revoked key shows the sign-in screen"), IsShown(Window, TEXT("Continue in browser  ↗")));
	}
	return true;
}
#endif
