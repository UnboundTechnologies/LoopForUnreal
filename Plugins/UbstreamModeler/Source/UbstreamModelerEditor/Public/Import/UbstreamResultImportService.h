#pragma once

#include "CoreMinimal.h"

DECLARE_DELEGATE_ThreeParams(FUbstreamResultImportComplete, bool /*bSuccess*/, const FString& /*Message*/, const FString& /*AssetPath*/);

class FUbstreamResultImportService
{
public:
	static bool ImportImageUrl(const FString& Url, FUbstreamResultImportComplete OnComplete);

	static bool ImportImageBytes(
		const FString& SourceName,
		const TArray<uint8>& Bytes,
		FString& OutAssetPath,
		FString& OutErrorMessage);
};
