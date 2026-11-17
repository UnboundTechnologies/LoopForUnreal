#include "Auth/UbstreamAuthService.h"
#include "Engine/StaticMesh.h"
#include "Import/UbstreamMeshImportService.h"
#include "Import/UbstreamGlbCompatibility.h"
#include "MeshDescription.h"
#include "MeshGen/UbstreamMeshGenService.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Settings/UbstreamModelerSettings.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
class FUbstreamLiveMeshCommand final : public IAutomationLatentCommand
{
public:
	FUbstreamLiveMeshCommand(FAutomationTestBase* InTest, FUbstreamMeshGenRequest InRequest)
		: Test(InTest), Request(MoveTemp(InRequest)), Began(FPlatformTime::Seconds()) {}

	virtual bool Update() override
	{
		FUbstreamAuthService& Auth = FUbstreamAuthService::Get();
		if (!Service)
		{
			if (!Auth.IsAuthenticated())
			{
				if (!Auth.HasKey() || FPlatformTime::Seconds() - Began > 60)
				{
					Test->AddError(TEXT("Live test requires a valid stored Unbound account key. ") + Auth.GetMessage());
					return true;
				}
				return false;
			}
			Test->AddInfo(TEXT("LIVE: existing account authenticated; starting one Unbound Zero job (1 credit)."));
			Service = MakeShared<FUbstreamMeshGenService, ESPMode::ThreadSafe>();
			Service->OnProgress.AddLambda([this](const FUbstreamMeshGenProgress& Progress)
			{
				if (Progress.Phase != LastPhase || Progress.TaskId != LastTask)
				{
					LastPhase = Progress.Phase;
					LastTask = Progress.TaskId;
					Test->AddInfo(FString::Printf(TEXT("LIVE: %s task=%s"), *Progress.StatusText, *Progress.TaskId));
				}
			});
			if (!Service->Start(Request))
			{
				Test->AddError(TEXT("Live generation could not start: ") + Service->GetLastResult().ErrorMessage);
				return true;
			}
		}
		if (Service->IsBusy())
		{
			if (FPlatformTime::Seconds() - Began > 2700)
			{
				Service->Cancel();
				Test->AddError(TEXT("Live generation exceeded 45 minutes; inspect the logged task ID before starting another job."));
				return true;
			}
			return false;
		}

		const FUbstreamMeshGenResult& Result = Service->GetLastResult();
		if (!Result.bSuccess)
		{
			Test->AddError(FString::Printf(TEXT("LIVE FAILED: HTTP=%d task=%s error=%s"),
				Result.HttpStatusCode, *Result.TaskId, *Result.ErrorMessage));
			return true;
		}
		UStaticMesh* Mesh = Result.StaticMesh.Get();
		if (!Test->TestNotNull(TEXT("Generated GLB imported as a Static Mesh"), Mesh)) return true;
		const FMeshDescription* Description = Mesh->GetMeshDescription(0);
		if (!Test->TestNotNull(TEXT("Imported mesh has editable geometry"), Description)) return true;
		Test->TestTrue(TEXT("Imported mesh contains vertices"), Description->Vertices().Num() > 0);
		Test->TestTrue(TEXT("Imported mesh contains triangles"), Description->Triangles().Num() > 0);
		Test->TestTrue(TEXT("Downloaded payload is GLB"), Result.GlbBytes.Num() > 12
			&& FMemory::Memcmp(Result.GlbBytes.GetData(), "glTF", 4) == 0);
		UPackage* Package = Mesh->GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		Test->TestTrue(TEXT("Imported mesh saved to project"), UPackage::SavePackage(Package, Mesh, *Filename, SaveArgs));
		Test->AddInfo(FString::Printf(TEXT("LIVE RESULT: task=%s asset=%s vertices=%d triangles=%d glb_bytes=%d elapsed=%.1fs"),
			*Result.TaskId, *Result.AssetPath, Description->Vertices().Num(), Description->Triangles().Num(),
			Result.GlbBytes.Num(), FPlatformTime::Seconds() - Began));
		return true;
	}
private:
	FAutomationTestBase* Test;
	FUbstreamMeshGenRequest Request;
	double Began;
	TSharedPtr<FUbstreamMeshGenService, ESPMode::ThreadSafe> Service;
	EUbstreamMeshGenPhase LastPhase = EUbstreamMeshGenPhase::Idle;
	FString LastTask;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamMeshGenLiveTest,
	"Ubstream.Modeler.MeshGeneration.Live", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamMeshGenLiveTest::RunTest(const FString& Parameters)
{
	// Never spend credits as part of the normal offline suite.
	if (!FParse::Param(FCommandLine::Get(), TEXT("UbstreamLiveMeshGen")))
	{
		AddInfo(TEXT("Skipped: requires -UbstreamLiveMeshGen and -UbstreamLiveMeshImage=<image>. Uses real credits."));
		return true;
	}
	FString ImagePath;
	FUbstreamMeshGenRequest Request;
	if (!FParse::Value(FCommandLine::Get(), TEXT("UbstreamLiveMeshImage="), ImagePath)
		|| !FFileHelper::LoadFileToArray(Request.ImageBytes, *ImagePath))
	{
		AddError(TEXT("Provide a readable JPEG with -UbstreamLiveMeshImage=<image>."));
		return false;
	}
	Request.BaseUrl = GetDefault<UUbstreamModelerSettings>()->MeshGenBaseUrl;
	Request.Quads = UbstreamMeshGenApi::DefaultQuads;
	Request.ImportDirectory = TEXT("/Game/Ubstream/Verification");
	Request.AssetStem = TEXT("Zero_Live_Verification");
	FUbstreamAuthService::Get().Initialize();
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FUbstreamLiveMeshCommand>(this, MoveTemp(Request)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamMeshGenSavedAssetTest,
	"Ubstream.Modeler.MeshGeneration.SavedAsset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamMeshGenSavedAssetTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("UbstreamVerifyMeshAsset="), AssetPath))
	{
		AddInfo(TEXT("Skipped: pass -UbstreamVerifyMeshAsset=<object path> to verify a previously imported result. No generation is submitted."));
		return true;
	}
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!TestNotNull(TEXT("Saved generated mesh reloads from disk"), Mesh)) return false;
	const FMeshDescription* Description = Mesh->GetMeshDescription(0);
	if (!TestNotNull(TEXT("Saved mesh retains editable geometry"), Description)) return false;
	TestTrue(TEXT("Saved mesh has vertices"), Description->Vertices().Num() > 0);
	TestTrue(TEXT("Saved mesh has triangles"), Description->Triangles().Num() > 0);
	TestTrue(TEXT("Saved mesh has nonzero bounds in all three dimensions"), Mesh->GetBounds().BoxExtent.GetMin() > KINDA_SMALL_NUMBER);
	TestTrue(TEXT("Saved mesh has a material"), Mesh->GetStaticMaterials().Num() > 0 && Mesh->GetMaterial(0) != nullptr);
	AddInfo(FString::Printf(TEXT("Saved mesh verified: %s vertices=%d triangles=%d"),
		*AssetPath, Description->Vertices().Num(), Description->Triangles().Num()));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamMeshGenExistingGlbTest,
	"Ubstream.Modeler.MeshGeneration.ImportExistingGlb", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamMeshGenExistingGlbTest::RunTest(const FString& Parameters)
{
	FString SourcePath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("UbstreamImportExistingGlb="), SourcePath))
	{
		AddInfo(TEXT("Skipped: pass -UbstreamImportExistingGlb=<file> to import a downloaded GLB without generating again."));
		return true;
	}
	TArray<uint8> Source, Prepared, SecondPass;
	FString Error;
	if (!TestTrue(TEXT("Original downloaded GLB is readable"), FFileHelper::LoadFileToArray(Source, *SourcePath))) return false;
	if (!TestTrue(TEXT("GLB is prepared for Unreal"), UbstreamGlbCompatibility::PrepareForImport(Source, Prepared, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Normalization is repeatable"), UbstreamGlbCompatibility::PrepareForImport(Prepared, SecondPass, Error));
	TestTrue(TEXT("Normalization is idempotent"), Prepared == SecondPass);
	// Both metadata layouts retain the exact compressed bytes after the JSON chunk.
	auto JsonEnd = [](const TArray<uint8>& Bytes)
	{
		return 20 + int32(uint32(Bytes[12]) | (uint32(Bytes[13]) << 8) | (uint32(Bytes[14]) << 16) | (uint32(Bytes[15]) << 24));
	};
	const int32 SourceEnd = JsonEnd(Source), PreparedEnd = JsonEnd(Prepared);
	if (TestEqual(TEXT("Compressed chunk size preserved"), Source.Num() - SourceEnd, Prepared.Num() - PreparedEnd))
	{
		TestTrue(TEXT("Compressed geometry is unchanged"), FMemory::Memcmp(Source.GetData() + SourceEnd,
			Prepared.GetData() + PreparedEnd, Source.Num() - SourceEnd) == 0);
	}
	TArray<uint8> Truncated = Source;
	Truncated.Pop();
	TArray<uint8> Unused;
	TestFalse(TEXT("Truncated GLB is rejected before import"), UbstreamGlbCompatibility::PrepareForImport(Truncated, Unused, Error));
	const FUbstreamMeshImportResult Result = FUbstreamMeshImportService::ImportGlbBytes(Source, TEXT("/Game/Ubstream/Generated"), TEXT("Recovered_Mesh"));
	if (!Result.bSuccess) { AddError(Result.ErrorMessage); return false; }
	UStaticMesh* Mesh = Result.StaticMesh.Get();
	if (!TestNotNull(TEXT("Downloaded GLB imports as Static Mesh"), Mesh)) return false;
	const FMeshDescription* Description = Mesh->GetMeshDescription(0);
	if (!TestNotNull(TEXT("Recovered mesh has source geometry"), Description)) return false;
	TestTrue(TEXT("Recovered mesh has vertices"), Description->Vertices().Num() > 0);
	TestTrue(TEXT("Recovered mesh has triangles"), Description->Triangles().Num() > 0);
	int32 ExpectedTriangles = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("UbstreamExpectedTriangles="), ExpectedTriangles))
	{
		TestEqual(TEXT("All downloaded triangles survive import"), Description->Triangles().Num(), ExpectedTriangles);
	}
	UPackage* Package = Mesh->GetOutermost();
	const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	TestTrue(TEXT("Recovered mesh saves"), UPackage::SavePackage(Package, Mesh, *Filename, SaveArgs));
	AddInfo(FString::Printf(TEXT("RECOVERED: %s vertices=%d triangles=%d"), *Result.AssetPath,
		Description->Vertices().Num(), Description->Triangles().Num()));
	return true;
}
#endif
