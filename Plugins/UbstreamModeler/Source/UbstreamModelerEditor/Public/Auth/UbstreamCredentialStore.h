#pragma once
#include "CoreMinimal.h"

namespace UbstreamCredentialStore
{
	bool Read(const FString& Account, FString& OutKey);
	bool Write(const FString& Account, const FString& Key);
	bool Remove(const FString& Account);
}
