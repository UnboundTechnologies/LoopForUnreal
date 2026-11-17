#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

struct FUbstreamMeshImportResult
{
	bool bSuccess = false;
	FString AssetPath;
	FString ErrorMessage;
	TWeakObjectPtr<UStaticMesh> StaticMesh;
	FString TempFilePath;
};

class UBSTREAMMODELEREDITOR_API FUbstreamMeshImportService
{
public:
	/**
	 * Writes GLB bytes to a temp file under Saved/UbstreamGenerated, imports
	 * them as a Static Mesh under ImportDirectory, and syncs the Content Browser.
	 * An empty PreferredAssetStem selects the first available UB_01, UB_02, etc.
	 */
	static FUbstreamMeshImportResult ImportGlbBytes(
		const TArray<uint8>& GlbBytes,
		const FString& ImportDirectory,
		const FString& PreferredAssetStem);
};
