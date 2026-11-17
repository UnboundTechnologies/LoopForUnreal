#include "Auth/UbstreamAuthService.h"
#include "Auth/UbstreamAuthClock.h"
#include "Auth/UbstreamTransportSecurity.h"
#include "Auth/UbstreamCredentialStore.h"
#include "AssetRegistry/AssetData.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Widgets/SUbstreamAuthPanel.h"
#include "Widgets/SUbstreamModelerWidget.h"
#include "Widgets/SUbstreamModelerViewport.h"
#include "Viewport/UbstreamViewportClient.h"
#include "Widgets/SWindow.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamAuthenticationTest,
	"Ubstream.Auth.LifecycleAndVisuals", EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FUbstreamAuthenticationTest::RunTest(const FString&)
{
	auto& Auth = FUbstreamAuthService::Get();
	if (Auth.HasKey() || Auth.IsBusy() || Auth.IsAuthenticated() || Auth.IsBrowserSignInPending()) { AddError(TEXT("Run this test in an editor with no signed-in account.")); return false; }
	// Layout restoration may have opened the welcome panel already.
	if (Auth.bInitialized) Auth.Shutdown();
	// Isolated fixture: no production account, network request or credential read.
	Auth.bInitialized = true;
	double TestTime = UbstreamAuthClock::Seconds();
	TestTrue(TEXT("Continuous auth clock is available"), FMath::IsFinite(TestTime) && TestTime < 1e15);
	Auth.TestClock = [&TestTime] { return TestTime; };
	ON_SCOPE_EXIT { Auth.TestClock = nullptr; };
	Auth.Origin = TEXT("fixture://unreal-auth-test/") + FGuid::NewGuid().ToString();
	ON_SCOPE_EXIT { Auth.Shutdown(); };
	TestFalse(TEXT("No account initially"), Auth.IsAuthenticated());
	Auth.SignIn(TEXT("ubw_wrong_key_type"));
	TestFalse(TEXT("MCP key cannot start verification"), Auth.IsBusy());
	TestFalse(TEXT("Invalid key is not retained"), Auth.HasKey());
	TestTrue(TEXT("Invalid key has actionable feedback"), Auth.GetMessage().Contains(TEXT("ubue_")));
	Auth.Message.Reset();
	const FString AccountKey = TEXT("ubue_0123456789abcdef0123_abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG");
	TestTrue(TEXT("Well-formed Unreal key accepted"), Auth.IsValidKey(AccountKey));
	TestFalse(TEXT("Account key prefix is case-sensitive"), Auth.IsValidKey(TEXT("UBUE_") + AccountKey.Mid(5)));
	TestFalse(TEXT("Device prefix is case-sensitive"), Auth.IsValidDeviceToken(TEXT("UBUD_") + AccountKey.Mid(5)));
	for (const FString Url : {TEXT("https://example.test/api/v2"), TEXT("http://localhost:3000"), TEXT("http://127.0.0.1:8000"), TEXT("http://[::1]:8000")})
		TestTrue(TEXT("HTTPS and exact loopback HTTP allowed"), UbstreamTransportSecurity::IsAllowedUrl(Url));
	for (const FString Url : {TEXT("http://example.test"), TEXT("http://localhost.evil.test"), TEXT("http://localhost@evil.test"), TEXT("https://user@evil.test"), TEXT("http://127.0.0.1:0"), TEXT("http://localhost:65536"), TEXT("https://example.test\\@evil.test"), TEXT("https://example.test\n")})
		TestFalse(TEXT("Unsafe credential endpoints rejected"), UbstreamTransportSecurity::IsAllowedUrl(Url));
	TestTrue(TEXT("Secure stream allowed"), UbstreamTransportSecurity::IsAllowedUrl(TEXT("wss://example.test/api/ws/user"), true));
	TestTrue(TEXT("Loopback stream allowed"), UbstreamTransportSecurity::IsAllowedUrl(TEXT("ws://localhost:3002/api/ws/user"), true));
	TestFalse(TEXT("Remote plaintext stream rejected"), UbstreamTransportSecurity::IsAllowedUrl(TEXT("ws://example.test/api/ws/user"), true));
	const FString DeviceToken = TEXT("ubud_fedcba9876543210fedc_abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG");
	const FString ApprovedResponse = FString::Printf(TEXT("{\"status\":\"approved\",\"key\":\"%s\"}"), *AccountKey);
	TestTrue(TEXT("Well-formed device token accepted"), Auth.IsValidDeviceToken(DeviceToken));
	TestFalse(TEXT("Device credential cannot authenticate an account"), Auth.IsValidKey(DeviceToken));
	TestFalse(TEXT("Account credential cannot poll a device request"), Auth.IsValidDeviceToken(AccountKey));
	Auth.SignIn(DeviceToken);
	TestFalse(TEXT("Manual input never retains a device credential"), Auth.HasKey());
	auto PendingBrowser = [&]
	{
		Auth.DeviceToken = DeviceToken; Auth.DeviceRequestId = DeviceToken.Mid(5, 20);
		Auth.VerificationCode = TEXT("AB12-CD34");
		Auth.DeviceExpiresAt = Auth.Now() + 480;
		Auth.Message = TEXT("Waiting for browser approval. Unreal will connect automatically.");
	};
	PendingBrowser();
	const FString BrowserUrl = Auth.BuildBrowserSignInUrl();
	TestEqual(TEXT("Browser URL prefills the confirmation code"), BrowserUrl,
		Auth.Origin + TEXT("/authorize-app?u=unreal&request=fedcba9876543210fedc&code=AB12-CD34"));
	TestFalse(TEXT("Browser URL never contains the device credential"), BrowserUrl.Contains(DeviceToken));
	TestFalse(TEXT("Browser URL never contains the account credential"), BrowserUrl.Contains(AccountKey));
	Auth.VerificationCode = TEXT("&code=bad");
	TestTrue(TEXT("Code cannot inject query parameters"), Auth.BuildBrowserSignInUrl().EndsWith(TEXT("&code=%26code%3Dbad")));
	PendingBrowser();
	TestFalse(TEXT("Manual input stays available while polling"), Auth.IsBusy());
	const double PollResponseAt = Auth.Now();
	Auth.HandleBrowserPollResponse(Auth.DeviceGeneration, 200, TEXT("{\"status\":\"pending\"}"), true);
	TestTrue(TEXT("Next normal poll waits 3.5 seconds"), Auth.NextDevicePollAt >= PollResponseAt + 3.5 && Auth.NextDevicePollAt <= Auth.Now() + 3.5);
	TestTrue(TEXT("Pending approval keeps polling"), Auth.IsBrowserSignInPending());
	Auth.HandleBrowserPollResponse(Auth.DeviceGeneration, 429, TEXT("{}"), true);
	TestTrue(TEXT("Rate limit backs off polling"), Auth.NextDevicePollAt > Auth.Now() + 55);
	Auth.HandleBrowserPollResponse(Auth.DeviceGeneration, 503, TEXT("{}"), true);
	TestTrue(TEXT("Temporary outage retains request"), Auth.IsBrowserSignInPending());
	Auth.HandleBrowserPollResponse(Auth.DeviceGeneration, 200, TEXT("{\"status\":\"denied\"}"), true);
	TestFalse(TEXT("Decline stops polling"), Auth.IsBrowserSignInPending());
	PendingBrowser();
	const uint64 CancelledEpoch = Auth.DeviceGeneration;
	Auth.CancelBrowserSignIn();
	Auth.HandleBrowserPollResponse(CancelledEpoch, 200, ApprovedResponse, true);
	TestFalse(TEXT("Late approval cannot restore cancelled login"), Auth.HasKey());
	PendingBrowser(); Auth.DeviceExpiresAt = Auth.Now() - 1;
	Auth.HandleBrowserPollResponse(Auth.DeviceGeneration, 200, ApprovedResponse, true);
	TestFalse(TEXT("Expired approval is never accepted"), Auth.HasKey());
	TestFalse(TEXT("Expired flow stops polling"), Auth.IsBrowserSignInPending());
	PendingBrowser(); Auth.SignIn(AccountKey);
	TestFalse(TEXT("Manual key supersedes browser polling"), Auth.IsBrowserSignInPending());
	TestFalse(TEXT("Rejected transport cannot appear to be signing in"), Auth.IsSigningIn());
	TestTrue(TEXT("An unsafe account URL keeps its actionable error"), Auth.GetMessage().Contains(TEXT("requires HTTPS")));
	Auth.ApiKey.Reset(); Auth.Message.Reset();
	auto BeginVerificationFixture = [&]
	{
		// An inert request models in-flight verification without sending credentials.
		// Do not show the fake fixture:// transport error as a real startup screen.
		Auth.ApiKey = AccountKey; Auth.bAuthRejected = false;
		Auth.Request = FHttpModule::Get().CreateRequest();
		Auth.Message = TEXT("Verifying your key...");
	};

	const FString Directory = FPaths::ProjectSavedDir() / TEXT("UbstreamAuthCheck");
	IFileManager::Get().MakeDirectory(*Directory, true);
	TSharedRef<SWindow> Window = SNew(SWindow).Title(FText::FromString(TEXT("Unbound authentication check")))
		.ClientSize(FVector2D(1120, 820)).SupportsMaximize(false).SupportsMinimize(false);
	TSharedRef<SUbstreamModelerWidget> Panel = SNew(SUbstreamModelerWidget);
	Window->SetContent(Panel);
	FSlateApplication::Get().AddWindow(Window, true);
	ON_SCOPE_EXIT { Window->RequestDestroyWindow(); };
	auto Capture = [this, &Window, &Directory](const FString& Name)
	{
		for (int32 I = 0; I < 8; ++I) { FSlateApplication::Get().PumpMessages(); FSlateApplication::Get().Tick(); FPlatformProcess::Sleep(0.02f); }
		TArray<FColor> Pixels; FIntVector Size;
		if (!TestTrue(TEXT("Slate screen captured"), FSlateApplication::Get().TakeScreenshot(Window, Pixels, Size))) return;
		TArray64<uint8> Png;
		if (TestTrue(TEXT("Screen encoded"), FImageUtils::CompressImage(Png, TEXT("png"), FImageView(Pixels.GetData(), Size.X, Size.Y))))
			TestTrue(TEXT("Screen saved"), FFileHelper::SaveArrayToFile(TArrayView<const uint8>(Png.GetData(), Png.Num()), *(Directory / Name)));
	};
	Capture(TEXT("01-welcome.png"));
	BeginVerificationFixture();
	TestTrue(TEXT("Saved-key verification shows signing-in state"), Auth.IsSigningIn());
	TestFalse(TEXT("Pending verification does not unlock the workspace"), Auth.IsAuthenticated());
	Capture(TEXT("10-signing-in.png"));
	Auth.HandleVerificationResponse(Auth.Generation, false, 0, TEXT(""), false);
	TestFalse(TEXT("Connection failure exits the signing-in state"), Auth.IsSigningIn());
	TestTrue(TEXT("Connection failure keeps the key available for retry"), Auth.HasKey());
	Capture(TEXT("11-sign-in-unavailable.png"));
	Auth.ApiKey.Reset(); Auth.Message.Reset();
	Auth.SignIn(TEXT("not-an-unreal-key"));
	Capture(TEXT("02-invalid-key.png"));
	PendingBrowser();
	Capture(TEXT("06-browser-approval.png"));
	Auth.HandleBrowserPollResponse(Auth.DeviceGeneration, 200, TEXT("{\"status\":\"approved\"}"), true);
	TestFalse(TEXT("Approval without an account key never promotes the device token"), Auth.HasKey());
	TestTrue(TEXT("Incomplete delivery remains retryable"), Auth.IsBrowserSignInPending());
	Auth.HandleBrowserPollResponse(Auth.DeviceGeneration, 200,
		FString::Printf(TEXT("{\"status\":\"approved\",\"key\":\"%s\"}"), *DeviceToken), true);
	TestFalse(TEXT("Device token in a key response is rejected"), Auth.HasKey());
	Auth.HandleBrowserPollResponse(Auth.DeviceGeneration, 200, ApprovedResponse, true);
	TestFalse(TEXT("Approval ends polling"), Auth.IsBrowserSignInPending());
	TestTrue(TEXT("Approval installs the separately issued account key"), Auth.ApiKey == AccountKey);
	TestTrue(TEXT("Delivered device token is discarded from memory"), Auth.DeviceToken.IsEmpty());
	TestFalse(TEXT("Approval alone does not skip key verification"), Auth.IsAuthenticated());
	BeginVerificationFixture();
	TestTrue(TEXT("Browser handoff also shows signing-in state"), Auth.IsSigningIn());
	Auth.HandleVerificationResponse(Auth.Generation, false, 200,
		TEXT("{\"keyId\":\"0123456789abcdef0123\",\"account\":\"alex@example.test\",\"keyLabel\":\"Unreal Editor · a1b2c3\"}"), true);
	TestTrue(TEXT("Verified key unlocks workspace"), Auth.IsAuthenticated());
	TestFalse(TEXT("Successful verification ends signing-in state"), Auth.IsSigningIn());
	Auth.Request = FHttpModule::Get().CreateRequest();
	TestFalse(TEXT("Background key recheck does not replace an authenticated workspace"), Auth.IsSigningIn());
	Auth.Request.Reset();
	TestTrue(TEXT("Only the Unbound key is supplied to transports"), Auth.GetAuthKey() == AccountKey);
	TestEqual(TEXT("Key is rechecked every fifteen minutes"), Auth.NextVerificationAt - Auth.Now(), 900.0);
	TestEqual(TEXT("Verification outage grace is bounded"), Auth.VerifiedUntil - Auth.NextVerificationAt, 60.0);
	Capture(TEXT("03-workspace.png"));
	// Exercise the actual placeholder's drop forwarding while the scene is hidden.
	TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>)> FindEmptyStage = [&FindEmptyStage](TSharedRef<SWidget> Widget) -> TSharedPtr<SWidget>
	{
		if (Widget->GetTypeAsString() == TEXT("SEmptyStagePreview")) return Widget;
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Index < Children->Num(); ++Index)
			if (auto Found = FindEmptyStage(Children->GetChildAt(Index))) return Found;
		return nullptr;
	};
	const auto EmptyStage = FindEmptyStage(Panel);
	const auto Stage = Panel->GetSoloViewport();
	TestFalse(TEXT("Empty stage does not render the live viewport"), Stage->GetUbstreamViewportClient()->IsVisible());
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (TestTrue(TEXT("Static drop target exists"), EmptyStage.IsValid()) && TestNotNull(TEXT("Drop fixture mesh exists"), Cube))
	{
		const auto Drag = FAssetDragDropOp::New(TArray<FAssetData>{FAssetData(Cube)});
		const FDragDropEvent Drop(FPointerEvent(), Drag);
		TestTrue(TEXT("Static image accepts a mesh drag"), EmptyStage->OnDragOver(EmptyStage->GetCachedGeometry(), Drop).IsEventHandled());
		TestTrue(TEXT("Static image loads a dropped mesh"), EmptyStage->OnDrop(EmptyStage->GetCachedGeometry(), Drop).IsEventHandled());
		Capture(TEXT("08-stage-loaded.png")); // Pump Slate so bound visibility attributes refresh.
		TestTrue(TEXT("Mesh drop reveals live viewport"), Stage->HasSoloMesh() && Stage->GetVisibility().IsVisible());
		TestTrue(TEXT("Mesh drop hides static image"), EmptyStage->GetVisibility() == EVisibility::Collapsed);
		Stage->ClearObjects();
		Capture(TEXT("09-stage-cleared.png"));
		TestTrue(TEXT("Clearing stage restores static image"), EmptyStage->GetVisibility().IsVisible());
		TestFalse(TEXT("Cleared stage stops rendering live viewport"), Stage->GetUbstreamViewportClient()->IsVisible());
	}
	Auth.HandleVerificationResponse(Auth.Generation, false, 401, TEXT("{}"), true);
	TestFalse(TEXT("Rejected key locks the workspace"), Auth.IsAuthenticated());
	TestTrue(TEXT("Rejected key is never returned"), Auth.GetAuthKey().IsEmpty());
	TestTrue(TEXT("Rejected key stops automatic verification retries"), Auth.bAuthRejected);
	TestTrue(TEXT("Rejected key explains replacement and browser recovery"), Auth.GetMessage().Contains(TEXT("replaced")) && Auth.GetMessage().Contains(TEXT("Continue in browser")));
	TestTrue(TEXT("Rejected key explains manual recovery"), Auth.GetMessage().Contains(TEXT("enter a current auth token")));
	TestFalse(TEXT("Rejected key leaves sign-in controls available"), Auth.IsBusy());
	Capture(TEXT("07-key-replaced.png"));
	Auth.HandleVerificationResponse(Auth.Generation, false, 200,
		TEXT("{\"keyId\":\"0123456789abcdef0123\",\"account\":\"alex@example.test\",\"keyLabel\":\"Unreal Editor · a1b2c3\"}"), true);
	TestTrue(TEXT("Successful reauthentication unlocks the workspace"), Auth.IsAuthenticated());
	TestFalse(TEXT("Successful reauthentication resumes automatic verification"), Auth.bAuthRejected);
	Window->SetContent(SNew(SUbstreamAuthPanel).AccountMode(true));
	Window->Resize(FVector2D(720, 960));
	Capture(TEXT("04-account-settings.png"));
	TestTime += 3601; // Simulate an hour asleep: no ticker ran during this interval.
	TestFalse(TEXT("Stale verification blocks access after sleep"), Auth.IsAuthenticated());
	TestTrue(TEXT("Unverified key is never returned"), Auth.GetAuthKey().IsEmpty());
	Auth.VerifiedUntil = Auth.Now() + 3600;
	bool bCancelBeforeClear = false;
	Auth.OnSigningOut.AddLambda([&] { bCancelBeforeClear = !Auth.GetAuthKey().IsEmpty(); });
	// Only a random test-specific item is touched; never read user credentials.
#if PLATFORM_MAC || PLATFORM_WINDOWS
	TestTrue(TEXT("System credential store saves test key"), UbstreamCredentialStore::Write(Auth.Origin, TEXT("isolated-test-key-not-valid")));
	FString StoredTestKey;
	TestTrue(TEXT("System credential store reads test key"), UbstreamCredentialStore::Read(Auth.Origin, StoredTestKey));
	TestTrue(TEXT("Stored key round-trips"), StoredTestKey == TEXT("isolated-test-key-not-valid"));
#endif
	const uint64 SignedOutEpoch = Auth.Generation;
	Auth.RemoveKey();
	Auth.HandleVerificationResponse(SignedOutEpoch, false, 200, TEXT("{\"keyId\":\"0123456789abcdef0123\",\"account\":\"ignored\",\"keyLabel\":\"ignored\"}"), true);
#if PLATFORM_MAC || PLATFORM_WINDOWS
	TestFalse(TEXT("Sign-out deletes system credential"), UbstreamCredentialStore::Read(Auth.Origin, StoredTestKey));
#endif
	TestTrue(TEXT("Jobs can cancel before clearing credentials"), bCancelBeforeClear);
	TestFalse(TEXT("Sign-out clears authentication"), Auth.IsAuthenticated());
	TestFalse(TEXT("Sign-out clears saved key in memory"), Auth.HasKey());
	TestTrue(TEXT("Sign-out clears account metadata"), Auth.GetAccount().IsEmpty());
	Window->SetContent(Panel); Window->Resize(FVector2D(2240, 1640));
	Capture(TEXT("05-signed-out.png"));
	return true;
}
#endif
