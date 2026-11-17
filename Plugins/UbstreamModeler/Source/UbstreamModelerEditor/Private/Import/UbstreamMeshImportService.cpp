#include "Import/UbstreamMeshImportService.h"
#include "Import/UbstreamGlbCompatibility.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMeshToMeshDescription.h"
#include "Engine/StaticMesh.h"
#include "Geometry/UbstreamMeshOrientation.h"
#include "HAL/FileManager.h"
#include "IContentBrowserSingleton.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogUbstreamMeshImport, Log, All);

namespace
{
	FString SanitizeAssetStem(const FString& PreferredStem)
	{
		FString Stem = PreferredStem;
		if (Stem.IsEmpty())
		{
			return TEXT("UB_01");
		}

		for (TCHAR& Character : Stem)
		{
			if (!FChar::IsAlnum(Character) && Character != TCHAR('_'))
			{
				Character = TCHAR('_');
			}
		}

		while (Stem.Contains(TEXT("__")))
		{
			Stem.ReplaceInline(TEXT("__"), TEXT("_"));
		}
		while (Stem.StartsWith(TEXT("_")))
		{
			Stem.RightChopInline(1);
		}
		while (Stem.EndsWith(TEXT("_")))
		{
			Stem.LeftChopInline(1);
		}

		if (!Stem.StartsWith(TEXT("SM_")))
		{
			Stem = FString::Printf(TEXT("SM_Ubstream_%s"), *Stem);
		}

		return Stem.Left(64);
	}

	FString NormalizeImportDirectory(const FString& ImportDirectory)
	{
		FString Directory = ImportDirectory;
		Directory.TrimStartAndEndInline();
		if (Directory.IsEmpty())
		{
			Directory = TEXT("/Game/Ubstream/Generated");
		}

		Directory.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Directory.EndsWith(TEXT("/")))
		{
			Directory.LeftChopInline(1);
		}

		if (!Directory.StartsWith(TEXT("/Game")))
		{
			Directory = TEXT("/Game/Ubstream/Generated");
		}

		return Directory;
	}

	FString MakeUniqueTempGlbPath(const FString& TempDir, const FString& SafeStem)
	{
		IFileManager::Get().MakeDirectory(*TempDir, true);
		// Stable, human-readable temp name (not CreateTempFilename random suffix only).
		for (int32 Attempt = 0; Attempt < 64; ++Attempt)
		{
			const FString CandidateName = Attempt == 0
				? FString::Printf(TEXT("%s.glb"), *SafeStem)
				: FString::Printf(TEXT("%s_%d.glb"), *SafeStem, Attempt);
			const FString CandidatePath = FPaths::Combine(TempDir, CandidateName);
			if (!IFileManager::Get().FileExists(*CandidatePath))
			{
				return CandidatePath;
			}
		}
		return FPaths::CreateTempFilename(*TempDir, *SafeStem, TEXT(".glb"));
	}

	UMaterialInterface* ResolveVertexColorMaterial()
	{
		// Engine materials that sample vertex color — FlexiCubes GLBs are typically vertex-colored only.
		static const TCHAR* Candidates[] = {
			TEXT("/Engine/EngineDebugMaterials/VertexColorViewMode_ColorOnly.VertexColorViewMode_ColorOnly"),
			TEXT("/Engine/EngineDebugMaterials/M_VertexColorViewMode.M_VertexColorViewMode"),
			TEXT("/Engine/EngineMaterials/WorldVertexColor.WorldVertexColor"),
		};

		for (const TCHAR* Path : Candidates)
		{
			if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, Path))
			{
				return Material;
			}
		}
		return nullptr;
	}

	/** Slots Interchange left without a real material (none, or the engine fallback). */
	bool NeedsVertexColorMaterial(const UMaterialInterface* Material)
	{
		return !Material || Material == UMaterial::GetDefaultMaterial(MD_Surface);
	}

	void ApplyVertexColorMaterialIfNeeded(UStaticMesh& Mesh)
	{
		// Unbound Zero GLBs carry PBR materials (base color, metallic/roughness,
		// normal map) that Interchange imports; keep them. Only untextured,
		// vertex-colored meshes need the engine vertex-color material.
		TArray<FStaticMaterial> Materials = Mesh.GetStaticMaterials();
		if (!Materials.IsEmpty() && !Materials.ContainsByPredicate([](const FStaticMaterial& Slot)
			{
				return NeedsVertexColorMaterial(Slot.MaterialInterface);
			}))
		{
			return;
		}

		UMaterialInterface* VertexColorMaterial = ResolveVertexColorMaterial();
		if (!VertexColorMaterial)
		{
			UE_LOG(LogUbstreamMeshImport, Warning,
				TEXT("No engine vertex-color material found for %s; mesh may render flat grey"),
				*Mesh.GetPathName());
			return;
		}

		if (Materials.IsEmpty())
		{
			Materials.Add(FStaticMaterial(VertexColorMaterial));
		}
		else
		{
			for (FStaticMaterial& Slot : Materials)
			{
				if (NeedsVertexColorMaterial(Slot.MaterialInterface))
				{
					Slot.MaterialInterface = VertexColorMaterial;
				}
			}
		}
		Mesh.SetStaticMaterials(Materials);
		Mesh.MarkPackageDirty();
		UE_LOG(LogUbstreamMeshImport, Log,
			TEXT("Assigned vertex-color material on %s (%d slots)"),
			*Mesh.GetPathName(),
			Materials.Num());
	}

	/** Generated GLBs (and some Interchange paths) arrive with inverted winding,
	    which makes the stage show only backfaces. Detect + fix once on import. */
	void FixImportedMeshOrientationIfNeeded(UStaticMesh& Mesh)
	{
		FMeshDescription* MeshDescription = Mesh.GetMeshDescription(0);
		if (!MeshDescription)
		{
			return;
		}

		UE::Geometry::FDynamicMesh3 DynamicMesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.bUseCompactedPolygonGroupIDValues = true;
		Converter.Convert(MeshDescription, DynamicMesh);

		const bool bReversed = UbstreamMeshOrientation::EnsureOutwardOrientation(DynamicMesh, /*bRayProbe*/ true);
		// Always rewrite after recompute so normals/winding match render data.
		FlushRenderingCommands();
		Mesh.Modify();
		if (Mesh.GetNumSourceModels() < 1)
		{
			Mesh.SetNumSourceModels(1);
		}
		Mesh.ModifyMeshDescription(0);
		FMeshDescription* WritableDescription = Mesh.GetMeshDescription(0);
		if (!WritableDescription)
		{
			WritableDescription = Mesh.CreateMeshDescription(0);
		}
		if (!WritableDescription)
		{
			return;
		}

		FDynamicMeshToMeshDescription WriteBack;
		WriteBack.Convert(&DynamicMesh, *WritableDescription, true);
		Mesh.CommitMeshDescription(0);
		Mesh.GetSourceModel(0).ResetReductionSetting();
		Mesh.PostEditChange();
		Mesh.MarkPackageDirty();

		UE_LOG(LogUbstreamMeshImport, Log,
			TEXT("%s orientation on imported mesh %s (outward score after fix applied)"),
			bReversed ? TEXT("Reversed") : TEXT("Preserved"),
			*Mesh.GetPathName());
	}

	bool RenameImportedMesh(UStaticMesh& Mesh, const FString& DestinationPath, const FString& DesiredName, FString& OutError)
	{
		const FString CurrentName = Mesh.GetName();
		const FString CurrentPackagePath = FPackageName::GetLongPackagePath(Mesh.GetOutermost()->GetName());
		if (CurrentName.Equals(DesiredName, ESearchCase::CaseSensitive)
			&& CurrentPackagePath.Equals(DestinationPath, ESearchCase::CaseSensitive))
		{
			return true;
		}

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		FString UniquePackageName;
		FString UniqueAssetName;
		AssetToolsModule.Get().CreateUniqueAssetName(DestinationPath / DesiredName, TEXT(""), UniquePackageName, UniqueAssetName);

		TArray<FAssetRenameData> RenameData;
		RenameData.Emplace(&Mesh, DestinationPath, UniqueAssetName);
		if (!AssetToolsModule.Get().RenameAssets(RenameData))
		{
			OutError = FString::Printf(
				TEXT("Imported mesh but failed to rename to %s/%s"),
				*DestinationPath,
				*UniqueAssetName);
			return false;
		}

		return true;
	}
}

FUbstreamMeshImportResult FUbstreamMeshImportService::ImportGlbBytes(
	const TArray<uint8>& GlbBytes,
	const FString& ImportDirectory,
	const FString& PreferredAssetStem)
{
	FUbstreamMeshImportResult Result;

	if (GlbBytes.IsEmpty())
	{
		Result.ErrorMessage = TEXT("GLB payload is empty");
		return Result;
	}

	const FString DestinationPath = NormalizeImportDirectory(ImportDirectory);
	TArray<uint8> ImportBytes;
	if (!UbstreamGlbCompatibility::PrepareForImport(GlbBytes, ImportBytes, Result.ErrorMessage))
	{
		return Result;
	}
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	FString PackageName;
	FString SafeStem;
	// Unreal preserves the zero padding and advances UB_01 to UB_02, etc.
	// Choose the name before import to avoid overwriting an existing generated mesh.
	AssetToolsModule.Get().CreateUniqueAssetName(
		DestinationPath / SanitizeAssetStem(PreferredAssetStem), TEXT(""), PackageName, SafeStem);
	const FString TempDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UbstreamGenerated"));
	const FString TempFilePath = MakeUniqueTempGlbPath(TempDir, SafeStem);
	if (!FFileHelper::SaveArrayToFile(ImportBytes, *TempFilePath))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Failed to write temporary GLB: %s"), *TempFilePath);
		return Result;
	}
	Result.TempFilePath = TempFilePath;

	const TArray<UObject*> ImportedObjects = AssetToolsModule.Get().ImportAssets(
		TArray<FString>{ TempFilePath },
		DestinationPath);

	UStaticMesh* ImportedMesh = nullptr;
	for (UObject* Object : ImportedObjects)
	{
		if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Object))
		{
			ImportedMesh = StaticMesh;
			break;
		}
	}

	if (!ImportedMesh)
	{
		// Interchange sometimes returns a package/folder root first; search deeper.
		for (UObject* Object : ImportedObjects)
		{
			if (!Object)
			{
				continue;
			}
			if (UStaticMesh* Nested = Cast<UStaticMesh>(Object))
			{
				ImportedMesh = Nested;
				break;
			}
		}
	}

	if (!ImportedMesh)
	{
		Result.ErrorMessage = FString::Printf(
			TEXT("Asset import produced no Static Mesh from %s (objects=%d)"),
			*TempFilePath,
			ImportedObjects.Num());
		UE_LOG(LogUbstreamMeshImport, Error, TEXT("%s"), *Result.ErrorMessage);
		return Result;
	}

	// Wait for async mesh build before material edits / editor open.
	if (ImportedMesh->IsCompiling())
	{
		TArray<UStaticMesh*> CompilingMeshes;
		CompilingMeshes.Add(ImportedMesh);
		FStaticMeshCompilingManager::Get().FinishCompilation(CompilingMeshes);
	}

	const FMeshDescription* ImportedDescription = ImportedMesh->GetMeshDescription(0);
	if (!ImportedDescription || ImportedDescription->Vertices().Num() == 0 || ImportedDescription->Triangles().Num() == 0)
	{
		Result.ErrorMessage = FString::Printf(TEXT("The GLB was downloaded, but Unreal could not import usable mesh geometry. The file is preserved at %s."), *TempFilePath);
		UE_LOG(LogUbstreamMeshImport, Error, TEXT("%s"), *Result.ErrorMessage);
		return Result;
	}

	// GLB node names can contain server IDs. Enforce the chosen asset name even
	// when Interchange appends a node name or imports into a nested folder.
	FString RenameError;
	if (!RenameImportedMesh(*ImportedMesh, DestinationPath, SafeStem, RenameError))
	{
		Result.ErrorMessage = RenameError;
		UE_LOG(LogUbstreamMeshImport, Error, TEXT("%s"), *RenameError);
		return Result;
	}

	ApplyVertexColorMaterialIfNeeded(*ImportedMesh);
	FixImportedMeshOrientationIfNeeded(*ImportedMesh);

	FAssetRegistryModule::AssetCreated(ImportedMesh);
	if (UPackage* Package = ImportedMesh->GetOutermost())
	{
		Package->MarkPackageDirty();
	}

	TArray<UObject*> AssetsToSync;
	AssetsToSync.Add(ImportedMesh);
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	ContentBrowserModule.Get().SyncBrowserToAssets(AssetsToSync);

	Result.bSuccess = true;
	Result.StaticMesh = ImportedMesh;
	Result.AssetPath = ImportedMesh->GetPathName();
	UE_LOG(LogUbstreamMeshImport, Log, TEXT("Imported generated mesh %s from %s"), *Result.AssetPath, *TempFilePath);
	return Result;
}
