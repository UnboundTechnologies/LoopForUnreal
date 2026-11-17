#include "Auth/UbstreamAuthService.h"
#include "Auth/UbstreamAuthClock.h"
#include "Auth/UbstreamTransportSecurity.h"
#include "Auth/UbstreamCredentialStore.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Settings/UbstreamModelerSettings.h"

// Auth events only: never keys, tokens or account details.
DEFINE_LOG_CATEGORY_STATIC(LogUbstreamAuth, Log, All);

namespace
{
constexpr double BrowserPollIntervalSeconds = 3.5;
constexpr double BrowserApprovalTimeoutSeconds = 480.0;
}

FUbstreamAuthService& FUbstreamAuthService::Get()
{
	static FUbstreamAuthService Service;
	return Service;
}

void FUbstreamAuthService::Initialize()
{
	if (bInitialized) return;
	bInitialized = true;
	Origin = GetDefault<UUbstreamModelerSettings>()->AccountBaseUrl.TrimStartAndEnd();
	while (Origin.EndsWith(TEXT("/"))) Origin.LeftChopInline(1);
	Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FUbstreamAuthService::Tick), 0.1f);
	if (FSlateApplication::IsInitialized())
	{
		ActivationHandle = FSlateApplication::Get().OnApplicationActivationStateChanged()
			.AddRaw(this, &FUbstreamAuthService::HandleApplicationActivationChanged);
	}
	bPersisted = UbstreamCredentialStore::Read(Origin, ApiKey);
	if (!ApiKey.IsEmpty()) VerifyKey(false);
}

void FUbstreamAuthService::Shutdown()
{
	ResetBrowserSignIn(true);
	++Generation;
	if (Request) { Request->OnProcessRequestComplete().Unbind(); Request->CancelRequest(); Request.Reset(); }
	FTSTicker::RemoveTicker(Ticker);
	Ticker.Reset();
	if (ActivationHandle.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().OnApplicationActivationStateChanged().Remove(ActivationHandle);
	}
	ActivationHandle.Reset();
	LastVerificationStartedAt = -1.0e9;
	ApiKey.Reset(); bVerified = false; Account.Reset(); KeyLabel.Reset(); Message.Reset();
	VerifiedUntil = NextVerificationAt = 0;
	bInitialized = bPersisted = bAuthRejected = false;
	OnChanged.Clear();
	OnSigningOut.Clear();
}

double FUbstreamAuthService::Now() const
{
	return TestClock ? TestClock() : UbstreamAuthClock::Seconds();
}

bool FUbstreamAuthService::IsAuthenticated() const
{
	return bVerified && Now() < VerifiedUntil;
}

FString FUbstreamAuthService::GetAuthKey() const
{
	return IsAuthenticated() ? ApiKey : FString();
}

void FUbstreamAuthService::SignIn(FString Key)
{
	if (IsBusy() || IsAuthenticated()) return;
	Key.TrimStartAndEndInline();
	if (!IsValidKey(Key))
	{
		Message = TEXT("Enter an Unreal key beginning with ubue_. Use the manual sign-in link to create one.");
		OnChanged.Broadcast(); return;
	}
	ResetBrowserSignIn(true);
	ApiKey = MoveTemp(Key);
	bPersisted = false;
	bAuthRejected = false;
	VerifyKey(true);
}

void FUbstreamAuthService::Retry()
{
	if (!IsBusy() && HasKey()) { ResetBrowserSignIn(true); bAuthRejected = false; VerifyKey(!bPersisted); }
}

void FUbstreamAuthService::VerifyKey(bool bSave)
{
	if (Request || ApiKey.IsEmpty()) return;
	if (!IsAccountOriginAllowed())
	{
		Message = TEXT("The account service requires HTTPS. Check the plugin connection settings.");
		bAuthRejected = true; OnChanged.Broadcast(); return;
	}
	LastVerificationStartedAt = Now();
	Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Origin + TEXT("/api/unreal/verify"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + ApiKey);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(TEXT("{\"acknowledge\":true}"));
	Request->SetTimeout(15.0f);
	const uint64 Epoch = ++Generation;
	Message = IsAuthenticated() ? FString() : TEXT("Verifying your key...");
	Request->OnProcessRequestComplete().BindLambda([this, Epoch, bSave](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
	{
		HandleVerificationResponse(Epoch, bSave, Response ? Response->GetResponseCode() : 0, Response ? Response->GetContentAsString() : FString(), bOk);
	});
	if (!Request->ProcessRequest())
	{
		Request.Reset(); NextVerificationAt = Now() + 30;
		Message = TEXT("Could not connect. Check your connection and try again.");
	}
	OnChanged.Broadcast();
}

bool FUbstreamAuthService::Tick(float)
{
	if (!DeviceToken.IsEmpty())
	{
		if (Now() >= DeviceExpiresAt)
		{
			ResetBrowserSignIn(true);
			Message = TEXT("Browser sign-in expired. Select Continue in browser to try again.");
			OnChanged.Broadcast();
		}
		else if (!DeviceRequest && Now() >= NextDevicePollAt) PollBrowserSignIn();
	}
	if (bVerified && !IsAuthenticated())
	{
		bVerified = false; Message = TEXT("Checking your account connection..."); OnChanged.Broadcast();
	}
	if (!IsBrowserSignInPending() && !bAuthRejected && HasKey() && !IsBusy() && Now() >= NextVerificationAt) VerifyKey(!bPersisted);
	return true;
}

void FUbstreamAuthService::HandleApplicationActivationChanged(bool bActive)
{
	// A key revoked or replaced in the browser otherwise stays usable here until
	// the next scheduled check (up to 15 minutes). Returning to the editor is the
	// moment that matters, so check then; alt-tabbing back and forth is limited
	// to one check every few seconds. The Tick performs the request.
	constexpr double MinSecondsBetweenFocusChecks = 5.0;
	if (bActive && HasKey() && !bAuthRejected && !IsBusy() && !IsBrowserSignInPending()
		&& Now() - LastVerificationStartedAt >= MinSecondsBetweenFocusChecks)
	{
		NextVerificationAt = FMath::Min(NextVerificationAt, Now());
	}
}

void FUbstreamAuthService::RemoveKey()
{
	ResetBrowserSignIn(true);
	OnSigningOut.Broadcast();
	++Generation;
	if (Request) { Request->OnProcessRequestComplete().Unbind(); Request->CancelRequest(); Request.Reset(); }
	const bool bRemoved = UbstreamCredentialStore::Remove(Origin);
	UE_LOG(LogUbstreamAuth, Log, TEXT("Signed out; saved key %s."), bRemoved ? TEXT("removed") : TEXT("could not be removed"));
	bVerified = false; Account.Reset(); KeyLabel.Reset(); VerifiedUntil = NextVerificationAt = 0;
	bAuthRejected = true;
	if (bRemoved) { ApiKey.Reset(); bPersisted = false; }
	Message = bRemoved ? TEXT("Key removed from this computer. You can connect again whenever you're ready.")
		: TEXT("Signed out, but the saved key could not be removed. Unlock your credential store and select Remove key again.");
	OnChanged.Broadcast();
}

void FUbstreamAuthService::OpenKeyPage() const
{
	FPlatformProcess::LaunchURL(*(Origin + TEXT("/authorize-app?u=unreal&mode=key")), nullptr, nullptr);
}

FString FUbstreamAuthService::GetStorageLabel() const
{
	if (!bPersisted) return TEXT("This editor session only");
#if PLATFORM_MAC
	return TEXT("Saved in macOS Keychain");
#elif PLATFORM_WINDOWS
	return TEXT("Saved in Windows Credential Manager");
#else
	return TEXT("This editor session only");
#endif
}

FString FUbstreamAuthService::BuildBrowserSignInUrl() const
{
	// Only the request ID and confirmation code go to the browser, never bearer credentials.
	return Origin + TEXT("/authorize-app?u=unreal&request=") + FGenericPlatformHttp::UrlEncode(DeviceRequestId)
		+ TEXT("&code=") + FGenericPlatformHttp::UrlEncode(VerificationCode);
}

void FUbstreamAuthService::BeginBrowserSignIn()
{
	if (IsBusy() || IsAuthenticated()) return;
	if (IsBrowserSignInPending())
	{
		if (!DeviceRequestId.IsEmpty()) FPlatformProcess::LaunchURL(*BuildBrowserSignInUrl(), nullptr, nullptr);
		return;
	}
	if (!IsAccountOriginAllowed())
	{
		Message = TEXT("The account service requires HTTPS. Check the plugin connection settings.");
		OnChanged.Broadcast(); return;
	}
	bAuthRejected = true; // Do not race a saved-key retry against browser approval.
	DeviceRequest = FHttpModule::Get().CreateRequest();
	DeviceRequest->SetURL(Origin + TEXT("/api/unreal/device/start"));
	DeviceRequest->SetVerb(TEXT("POST"));
	DeviceRequest->SetHeader(TEXT("Accept"), TEXT("application/json"));
	DeviceRequest->SetTimeout(25.0f);
	const uint64 Epoch = ++DeviceGeneration;
	const double StartedAt = Now();
	Message = TEXT("Preparing browser sign-in...");
	DeviceRequest->OnProcessRequestComplete().BindLambda([this, Epoch, StartedAt](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
	{
		if (Epoch != DeviceGeneration) return;
		DeviceRequest.Reset();
		TSharedPtr<FJsonObject> Body;
		FString Token, Id, Code;
		double Lifetime = 0;
		if (bOk && Response && Response->GetResponseCode() == 201
			&& Now() < StartedAt + BrowserApprovalTimeoutSeconds
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Body) && Body
			&& Body->TryGetStringField(TEXT("deviceToken"), Token) && IsValidDeviceToken(Token)
			&& Body->TryGetStringField(TEXT("requestId"), Id) && Id == Token.Mid(5, 20)
			&& Body->TryGetStringField(TEXT("verificationCode"), Code) && Code.Len() == 9
			&& Body->TryGetNumberField(TEXT("expiresIn"), Lifetime) && Lifetime > 0)
		{
			DeviceToken = MoveTemp(Token); DeviceRequestId = MoveTemp(Id); VerificationCode = MoveTemp(Code);
			DeviceExpiresAt = StartedAt + FMath::Min(Lifetime, BrowserApprovalTimeoutSeconds);
			NextDevicePollAt = Now() + BrowserPollIntervalSeconds;
			Message = TEXT("Your code is filled in for you. Select Connect Unreal Engine in your browser to finish signing in.");
			FPlatformProcess::LaunchURL(*BuildBrowserSignInUrl(), nullptr, nullptr);
		}
		else
		{
			Message = TEXT("Could not start browser sign-in. Try again or get a key to enter manually.");
		}
		OnChanged.Broadcast();
	});
	if (!DeviceRequest->ProcessRequest())
	{
		DeviceRequest.Reset(); Message = TEXT("Could not open sign-in. Check your connection and try again.");
	}
	OnChanged.Broadcast();
}

bool FUbstreamAuthService::IsBrowserSignInPending() const
{
	return DeviceRequest.IsValid() || !DeviceToken.IsEmpty();
}

void FUbstreamAuthService::ResetBrowserSignIn(bool bNotifyServer)
{
	++DeviceGeneration;
	if (DeviceRequest) { DeviceRequest->OnProcessRequestComplete().Unbind(); DeviceRequest->CancelRequest(); DeviceRequest.Reset(); }
	if (bNotifyServer && !DeviceToken.IsEmpty() && IsAccountOriginAllowed())
	{
		// The HTTP module owns this best-effort request until completion. There is
		// no callback into the auth service, so unloading cannot revive sign-in.
		auto Cancel = FHttpModule::Get().CreateRequest();
		Cancel->SetURL(Origin + TEXT("/api/unreal/device/cancel"));
		Cancel->SetVerb(TEXT("POST"));
		Cancel->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + DeviceToken);
		Cancel->SetTimeout(10.0f);
		Cancel->ProcessRequest();
	}
	DeviceToken.Reset(); DeviceRequestId.Reset(); VerificationCode.Reset();
	DeviceExpiresAt = NextDevicePollAt = 0;
}

void FUbstreamAuthService::CancelBrowserSignIn()
{
	ResetBrowserSignIn(true);
	Message = TEXT("Browser sign-in cancelled. You can try again or enter a key.");
	OnChanged.Broadcast();
}

void FUbstreamAuthService::PollBrowserSignIn()
{
	if (DeviceRequest || DeviceToken.IsEmpty()) return;
	DeviceRequest = FHttpModule::Get().CreateRequest();
	DeviceRequest->SetURL(Origin + TEXT("/api/unreal/device/poll"));
	DeviceRequest->SetVerb(TEXT("POST"));
	DeviceRequest->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + DeviceToken);
	DeviceRequest->SetHeader(TEXT("Accept"), TEXT("application/json"));
	DeviceRequest->SetTimeout(15.0f);
	const uint64 Epoch = DeviceGeneration;
	DeviceRequest->OnProcessRequestComplete().BindLambda([this, Epoch](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
	{
		HandleBrowserPollResponse(Epoch, Response ? Response->GetResponseCode() : 0, Response ? Response->GetContentAsString() : FString(), bOk);
	});
	if (!DeviceRequest->ProcessRequest())
	{
		DeviceRequest.Reset(); NextDevicePollAt = Now() + BrowserPollIntervalSeconds;
		Message = TEXT("Connection interrupted. Retrying browser sign-in..."); OnChanged.Broadcast();
	}
}

bool FUbstreamAuthService::IsValidKey(const FString& Key)
{
	if (!Key.StartsWith(TEXT("ubue_"), ESearchCase::CaseSensitive) || Key.Len() != 69 || Key[25] != TEXT('_')) return false;
	for (int32 I = 5; I < 25; ++I) if (!((Key[I] >= '0' && Key[I] <= '9') || (Key[I] >= 'a' && Key[I] <= 'f'))) return false;
	for (int32 I = 26; I < Key.Len(); ++I)
	{
		const TCHAR C = Key[I];
		if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_' || C == '-')) return false;
	}
	return true;
}

bool FUbstreamAuthService::IsValidDeviceToken(const FString& Token)
{
	// Separate namespace and authority; device tokens can never enter the key store.
	return Token.StartsWith(TEXT("ubud_"), ESearchCase::CaseSensitive) && IsValidKey(TEXT("ubue_") + Token.Mid(5));
}

bool FUbstreamAuthService::IsAccountOriginAllowed() const
{
	return UbstreamTransportSecurity::IsAllowedUrl(Origin);
}


void FUbstreamAuthService::HandleVerificationResponse(uint64 Epoch, bool bSave, int32 Code, const FString& ResponseBody, bool bOk)
{
	if (Epoch != Generation) return;
	Request.Reset();
	TSharedPtr<FJsonObject> Body;
	FString KeyId, Email, Label;
	if (bOk && Code == 200 && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ResponseBody), Body)
		&& Body && IsValidKey(ApiKey) && Body->TryGetStringField(TEXT("keyId"), KeyId) && KeyId == ApiKey.Mid(5, 20)
		&& Body->TryGetStringField(TEXT("account"), Email) && Body->TryGetStringField(TEXT("keyLabel"), Label))
	{
		bVerified = true; Account = MoveTemp(Email); KeyLabel = MoveTemp(Label);
		// The key itself does not expire. Limit how long a local verification
		// may remain stale, including time spent asleep.
		NextVerificationAt = Now() + 900.0;
		VerifiedUntil = NextVerificationAt + 60.0;
		bAuthRejected = false;
		if (bSave) bPersisted = UbstreamCredentialStore::Write(Origin, ApiKey);
		Message = bPersisted ? FString() : TEXT("Connected for this editor session. The system credential store could not save the key.");
	}
	else if (Code == 401 || Code == 403)
	{
		UE_LOG(LogUbstreamAuth, Log, TEXT("Account key rejected (HTTP %d); signed out until a new key is connected."), Code);
		bVerified = false; VerifiedUntil = 0; bAuthRejected = true;
		Message = TEXT("This key is no longer valid. It may have been replaced or revoked. Continue in browser or enter a current auth token to reconnect.");
	}
	else
	{
		NextVerificationAt = Now() + 30;
		Message = Code == 404 ? TEXT("The Unreal account service is not available yet. Update the web app or check its URL.")
			: TEXT("Could not reach the account service. Check your connection and try again.");
	}
	OnChanged.Broadcast();
}

void FUbstreamAuthService::HandleBrowserPollResponse(uint64 Epoch, int32 Code, const FString& ResponseBody, bool bOk)
{
	if (Epoch != DeviceGeneration) return;
	DeviceRequest.Reset();
	// A response arriving after the deadline must not authenticate the editor.
	if (Now() >= DeviceExpiresAt)
	{
		ResetBrowserSignIn(true);
		Message = TEXT("Browser sign-in expired. Select Continue in browser to try again.");
		OnChanged.Broadcast(); return;
	}
	TSharedPtr<FJsonObject> Body;
	FString Status, Key;
	const bool bParsed = bOk && Code > 0 && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ResponseBody), Body)
		&& Body && Body->TryGetStringField(TEXT("status"), Status);
	if (Code == 200 && bParsed && Status == TEXT("approved")
		&& Body->TryGetStringField(TEXT("key"), Key) && IsValidKey(Key))
	{
		// The account key is issued only after browser consent. A successful
		// key verification acknowledges receipt and clears the server handoff.
		ResetBrowserSignIn(false);
		SignIn(MoveTemp(Key));
		return;
	}
	if (Code == 401 || Code == 403 || Code == 410 || (bParsed && (Status == TEXT("denied") || Status == TEXT("cancelled"))))
	{
		ResetBrowserSignIn(false);
		Message = Status == TEXT("denied") ? TEXT("Access was declined in your browser. You can try again or enter a key.")
			: TEXT("This sign-in request is no longer active. Start again or enter a key.");
	}
	else
	{
		NextDevicePollAt = Now() + (Code == 429 ? 60.0 : BrowserPollIntervalSeconds);
		Message = Code == 200 && bParsed && Status == TEXT("pending") ? TEXT("Waiting for browser approval. Unreal will connect automatically.")
			: TEXT("Connection interrupted. Still waiting for browser approval...");
	}
	OnChanged.Broadcast();
}
