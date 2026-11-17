#include "Auth/UbstreamCredentialStore.h"

#if PLATFORM_MAC
#include <Security/Security.h>
#elif PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincred.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace UbstreamCredentialStore
{
#if PLATFORM_MAC
static CFMutableDictionaryRef Query(const FString& Account)
{
	CFMutableDictionaryRef Result = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(Result, kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(Result, kSecAttrService, CFSTR("io.unbound.loop.unreal"));
	CFStringRef Name = CFStringCreateWithCString(nullptr, TCHAR_TO_UTF8(*Account), kCFStringEncodingUTF8);
	CFDictionarySetValue(Result, kSecAttrAccount, Name);
	CFRelease(Name);
	return Result;
}
#endif

bool Read(const FString& Account, FString& OutKey)
{
	OutKey.Reset();
#if PLATFORM_MAC
	CFMutableDictionaryRef Q = Query(Account);
	CFDictionarySetValue(Q, kSecReturnData, kCFBooleanTrue);
	CFTypeRef Data = nullptr;
	const OSStatus Status = SecItemCopyMatching(Q, &Data);
	CFRelease(Q);
	if (Status != errSecSuccess || !Data) return false;
	const CFDataRef Bytes = static_cast<CFDataRef>(Data);
	FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(CFDataGetBytePtr(Bytes)), CFDataGetLength(Bytes));
	OutKey = FString(Text.Length(), Text.Get());
	CFRelease(Data);
	return true;
#elif PLATFORM_WINDOWS
	PCREDENTIALW Credential = nullptr;
	const FString Target = TEXT("UnboundLoop/") + Account;
	if (!CredReadW(*Target, CRED_TYPE_GENERIC, 0, &Credential)) return false;
	FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Credential->CredentialBlob), Credential->CredentialBlobSize);
	OutKey = FString(Text.Length(), Text.Get());
	CredFree(Credential);
	return true;
#else
	return false;
#endif
}

bool Write(const FString& Account, const FString& Key)
{
	FTCHARToUTF8 Bytes(*Key);
#if PLATFORM_MAC
	CFMutableDictionaryRef Q = Query(Account);
	CFDataRef Data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(Bytes.Get()), Bytes.Length());
	CFMutableDictionaryRef Update = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(Update, kSecValueData, Data);
	OSStatus Status = SecItemUpdate(Q, Update);
	if (Status == errSecItemNotFound)
	{
		CFDictionarySetValue(Q, kSecValueData, Data);
		CFDictionarySetValue(Q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
		Status = SecItemAdd(Q, nullptr);
	}
	CFRelease(Update);
	CFRelease(Data);
	CFRelease(Q);
	return Status == errSecSuccess;
#elif PLATFORM_WINDOWS
	FString Target = TEXT("UnboundLoop/") + Account;
	CREDENTIALW Credential = {};
	Credential.Type = CRED_TYPE_GENERIC;
	Credential.TargetName = Target.GetCharArray().GetData();
	Credential.CredentialBlobSize = Bytes.Length();
	Credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<ANSICHAR*>(Bytes.Get()));
	Credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	return CredWriteW(&Credential, 0) != 0;
#else
	return false;
#endif
}

bool Remove(const FString& Account)
{
#if PLATFORM_MAC
	CFMutableDictionaryRef Q = Query(Account);
	const OSStatus Status = SecItemDelete(Q);
	CFRelease(Q);
	return Status == errSecSuccess || Status == errSecItemNotFound;
#elif PLATFORM_WINDOWS
	const FString Target = TEXT("UnboundLoop/") + Account;
	return CredDeleteW(*Target, CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
#else
	return true;
#endif
}
}
