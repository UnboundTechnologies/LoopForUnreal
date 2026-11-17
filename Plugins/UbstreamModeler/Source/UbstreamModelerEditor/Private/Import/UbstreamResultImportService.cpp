#include "Import/UbstreamResultImportService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "Factories/TextureFactory.h"
#include "HttpModule.h"
#include "IContentBrowserSingleton.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUbstreamResultImport, Log, All);

namespace
{
	FString DetectImageType(const FString& SourceName, const TArray<uint8>& Bytes)
	{
		const FString Extension = FPaths::GetExtension(SourceName).ToUpper();
		if (Extension == TEXT("JPG") || Extension == TEXT("JPEG") || Extension == TEXT("PNG") || Extension == TEXT("EXR") || Extension == TEXT("HDR") || Extension == TEXT("TGA"))
		{
			return Extension == TEXT("JPEG") ? FString(TEXT("JPG")) : Extension;
		}

		if (Bytes.Num() >= 3 && Bytes[0] == 0xff && Bytes[1] == 0xd8 && Bytes[2] == 0xff)
		{
			return TEXT("JPG");
		}

		if (Bytes.Num() >= 8
			&& Bytes[0] == 0x89
			&& Bytes[1] == 0x50
			&& Bytes[2] == 0x4e
			&& Bytes[3] == 0x47
			&& Bytes[4] == 0x0d
			&& Bytes[5] == 0x0a
			&& Bytes[6] == 0x1a
			&& Bytes[7] == 0x0a)
		{
			return TEXT("PNG");
		}

		return TEXT("PNG");
	}

	FString MakeSafeAssetStem(const FString& SourceName)
	{
		FString Stem = FPaths::GetBaseFilename(SourceName);
		if (Stem.IsEmpty())
		{
			Stem = TEXT("Result");
		}

		for (TCHAR& Character : Stem)
		{
			if (!FChar::IsAlnum(Character) && Character != TCHAR('_'))
			{
				Character = TCHAR('_');
			}
		}

		return FString::Printf(TEXT("Ubstream_%s"), *Stem);
	}

	bool LoadLocalImageBytes(const FString& Url, TArray<uint8>& OutBytes, FString& OutSourceName)
	{
		FString LocalPath = Url;
		if (LocalPath.StartsWith(TEXT("file://")))
		{
			LocalPath.RightChopInline(7);
		}

		if (!FPaths::FileExists(LocalPath))
		{
			return false;
		}

		OutSourceName = FPaths::GetCleanFilename(LocalPath);
		return FFileHelper::LoadFileToArray(OutBytes, *LocalPath);
	}
}

bool FUbstreamResultImportService::ImportImageUrl(const FString& Url, FUbstreamResultImportComplete OnComplete)
{
	if (Url.IsEmpty())
	{
		OnComplete.ExecuteIfBound(false, TEXT("No result URL available"), FString());
		return false;
	}

	TArray<uint8> LocalBytes;
	FString LocalSourceName;
	if (LoadLocalImageBytes(Url, LocalBytes, LocalSourceName))
	{
		FString AssetPath;
		FString ErrorMessage;
		const bool bImported = ImportImageBytes(LocalSourceName, LocalBytes, AssetPath, ErrorMessage);
		OnComplete.ExecuteIfBound(bImported, bImported ? FString::Printf(TEXT("Imported %s"), *AssetPath) : ErrorMessage, AssetPath);
		return bImported;
	}

	if (!Url.StartsWith(TEXT("http://")) && !Url.StartsWith(TEXT("https://")))
	{
		OnComplete.ExecuteIfBound(false, FString::Printf(TEXT("Unsupported result URL: %s"), *Url), FString());
		return false;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->OnProcessRequestComplete().BindLambda([Url, OnComplete](FHttpRequestPtr RequestPtr, FHttpResponsePtr Response, bool bWasSuccessful)
	{
		if (!bWasSuccessful || !Response.IsValid())
		{
			OnComplete.ExecuteIfBound(false, FString::Printf(TEXT("Failed to download result: %s"), *Url), FString());
			return;
		}

		const int32 ResponseCode = Response->GetResponseCode();
		if (ResponseCode < 200 || ResponseCode >= 300)
		{
			OnComplete.ExecuteIfBound(false, FString::Printf(TEXT("Result download failed with HTTP %d"), ResponseCode), FString());
			return;
		}

		FString AssetPath;
		FString ErrorMessage;
		const bool bImported = FUbstreamResultImportService::ImportImageBytes(Url, Response->GetContent(), AssetPath, ErrorMessage);
		OnComplete.ExecuteIfBound(bImported, bImported ? FString::Printf(TEXT("Imported %s"), *AssetPath) : ErrorMessage, AssetPath);
	});

	const bool bStarted = Request->ProcessRequest();
	if (!bStarted)
	{
		OnComplete.ExecuteIfBound(false, FString::Printf(TEXT("Failed to start result download: %s"), *Url), FString());
	}
	return bStarted;
}

bool FUbstreamResultImportService::ImportImageBytes(
	const FString& SourceName,
	const TArray<uint8>& Bytes,
	FString& OutAssetPath,
	FString& OutErrorMessage)
{
	if (Bytes.IsEmpty())
	{
		OutErrorMessage = TEXT("Result image payload is empty");
		return false;
	}

	const FString AssetStem = MakeSafeAssetStem(SourceName);
	const FString BasePackageName = FString::Printf(TEXT("/Game/Ubstream/Results/%s"), *AssetStem);

	FString PackageName;
	FString AssetName;
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	AssetToolsModule.Get().CreateUniqueAssetName(BasePackageName, TEXT(""), PackageName, AssetName);

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutErrorMessage = FString::Printf(TEXT("Failed to create package %s"), *PackageName);
		return false;
	}

	const FString Type = DetectImageType(SourceName, Bytes);
	UTextureFactory* TextureFactory = NewObject<UTextureFactory>();
	TextureFactory->AddToRoot();
	UTextureFactory::SuppressImportOverwriteDialog();
	TextureFactory->bCreateMaterial = false;

	const uint8* BufferStart = Bytes.GetData();
	const uint8* BufferEnd = BufferStart + Bytes.Num();
	UObject* ImportedObject = TextureFactory->FactoryCreateBinary(
		UTexture2D::StaticClass(),
		Package,
		FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional,
		nullptr,
		*Type,
		BufferStart,
		BufferEnd,
		GWarn);
	TextureFactory->RemoveFromRoot();

	if (!ImportedObject)
	{
		OutErrorMessage = FString::Printf(TEXT("Failed to import result image as %s"), *Type);
		return false;
	}

	FAssetRegistryModule::AssetCreated(ImportedObject);
	Package->MarkPackageDirty();
	OutAssetPath = ImportedObject->GetPathName();

	TArray<UObject*> AssetsToSync;
	AssetsToSync.Add(ImportedObject);
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	ContentBrowserModule.Get().SyncBrowserToAssets(AssetsToSync);

	UE_LOG(LogUbstreamResultImport, Log, TEXT("Imported result image %s from %s"), *OutAssetPath, *SourceName);
	return true;
}
