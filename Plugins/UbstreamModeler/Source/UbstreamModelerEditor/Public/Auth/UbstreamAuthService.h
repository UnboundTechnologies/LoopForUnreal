#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpRequest.h"

/** One editor account, shared by 2D and 3D. Secrets never enter project config. */
class FUbstreamAuthService
{
public:
	static FUbstreamAuthService& Get();
	void Initialize();
	void Shutdown();
	void SignIn(FString Key);
	void Retry();
	void RemoveKey();
	void OpenKeyPage() const;
	void BeginBrowserSignIn();
	void CancelBrowserSignIn();
	bool IsBrowserSignInPending() const;
	FString GetVerificationCode() const { return VerificationCode; }
	bool IsAuthenticated() const;
	bool IsBusy() const { return Request.IsValid(); }
	bool IsSigningIn() const { return IsBusy() && !IsAuthenticated(); }
	bool HasKey() const { return !ApiKey.IsEmpty(); }
	FString GetAuthKey() const;
	FString GetAccount() const { return Account; }
	FString GetKeyLabel() const { return KeyLabel; }
	FString GetMessage() const { return Message; }
	FString GetStorageLabel() const;
	FSimpleMulticastDelegate OnChanged;
	FSimpleMulticastDelegate OnSigningOut;

private:
	friend class FUbstreamAuthenticationTest;
	friend class FUbstreamSignOutMenuTest;
	void VerifyKey(bool bSave);
	FString BuildBrowserSignInUrl() const;
	void ResetBrowserSignIn(bool bNotifyServer);
	void PollBrowserSignIn();
	void HandleBrowserPollResponse(uint64 Epoch, int32 Code, const FString& ResponseBody, bool bOk);
	void HandleVerificationResponse(uint64 Epoch, bool bSave, int32 Code, const FString& ResponseBody, bool bOk);
	static bool IsValidKey(const FString& Key);
	static bool IsValidDeviceToken(const FString& Token);
	bool IsAccountOriginAllowed() const;
	bool Tick(float DeltaTime);
	/** Recheck the key when the editor regains focus, e.g. after managing keys in the browser. */
	void HandleApplicationActivationChanged(bool bActive);
	FDelegateHandle ActivationHandle;
	double LastVerificationStartedAt = -1.0e9;
	double Now() const;
	TFunction<double()> TestClock;
	FString DeviceToken;
	FString DeviceRequestId;
	FString VerificationCode;
	double DeviceExpiresAt = 0;
	double NextDevicePollAt = 0;
	uint64 DeviceGeneration = 0;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> DeviceRequest;
	FString Origin;
	FString ApiKey;
	bool bVerified = false;
	FString Account;
	FString KeyLabel;
	FString Message;
	double VerifiedUntil = 0;
	double NextVerificationAt = 0;
	uint64 Generation = 0;
	bool bInitialized = false;
	bool bPersisted = false;
	bool bAuthRejected = false;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
	FTSTicker::FDelegateHandle Ticker;
};
