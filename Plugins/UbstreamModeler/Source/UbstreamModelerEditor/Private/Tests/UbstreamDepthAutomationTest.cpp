#include "Capture/UbstreamCaptureService.h"
#include "Capture/UbstreamCaptureTargetPool.h"
#include "AdvancedPreviewScene.h"
#include "Async/TaskGraphInterfaces.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	bool DecodeDepth(const FUbstreamEditorWorldCaptureResult& Result, FImage& Image)
	{
		if (!Result.bSuccess || !FImageUtils::DecompressImage(Result.DepthJpegBytes.GetData(), Result.DepthJpegBytes.Num(), Image)) return false;
		Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		return true;
	}

	int32 GrayAt(FImage& Image, int32 X, int32 Y)
	{
		FImageView View = Image;
		return View.AsBGRA8()[Y * Image.SizeX + X].R;
	}

	void SaveDepth(const FString& Name, const FUbstreamEditorWorldCaptureResult& Result)
	{
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DepthDiagnostics"));
		IFileManager::Get().MakeDirectory(*Directory, true);
		FFileHelper::SaveArrayToFile(Result.DepthJpegBytes, *FPaths::Combine(Directory, Name + TEXT("_depth.jpg")));
		FFileHelper::SaveArrayToFile(Result.ColorJpegBytes, *FPaths::Combine(Directory, Name + TEXT("_color.jpg")));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamDepthEncodingTest,
	"Ubstream.Modeler.Depth.Encoding", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamDepthEncodingTest::RunTest(const FString& Parameters)
{
	auto EncodeBands = [](const TArray<float>& Values)
	{
		FUbstreamCapturePixels Pixels;
		Pixels.bSuccess = true;
		Pixels.Size = FIntPoint(Values.Num() * 32, 32);
		Pixels.JpegQuality = 100;
		Pixels.ColorImage = FImage(Pixels.Size.X, Pixels.Size.Y, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		Pixels.ColorImage.RawData.Init(255, Pixels.Size.X * Pixels.Size.Y * 4);
		for (int32 Y = 0; Y < Pixels.Size.Y; ++Y)
			for (int32 X = 0; X < Pixels.Size.X; ++X)
				Pixels.DepthPixels.Add(FLinearColor(Values[X / 32], 0, 0, 0));
		return FUbstreamCaptureService::EncodeCapturedFrame(MoveTemp(Pixels));
	};
	const TArray<float> Values = {100, 200, 300, 0, -1, 65504, 70000, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
	const auto Result = EncodeBands(Values);
	FImage Image;
	if (!TestTrue(TEXT("Production depth JPEG decodes"), DecodeDepth(Result, Image))) return false;
	const TArray<int32> Expected = {255, 128, 0, 0, 0, 0, 0, 0, 0};
	for (int32 Index = 0; Index < Expected.Num(); ++Index)
	{
		const int32 Actual = GrayAt(Image, Index * 32 + 16, 16);
		TestTrue(FString::Printf(TEXT("Band %d: JPEG gray %d, expected %d +/- 2"), Index, Actual, Expected[Index]), FMath::Abs(Actual - Expected[Index]) <= 2);
	}
	SaveDepth(TEXT("synthetic_near_mid_far_invalid"), Result);
	// A constant-depth subject must retain its silhouette against background.
	const auto Flat = EncodeBands({300, 300, 0});
	if (TestTrue(TEXT("Flat-depth JPEG decodes"), DecodeDepth(Flat, Image)))
		{
		TestTrue(TEXT("Flat surface stays white"), GrayAt(Image, 16, 16) >= 253);
		TestTrue(TEXT("Flat surface background stays black"), GrayAt(Image, 80, 16) <= 2);
	}
	SaveDepth(TEXT("synthetic_flat_surface"), Flat);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamDepthGpuTest,
	"Ubstream.Modeler.Depth.GpuNearFar", EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FUbstreamDepthGpuTest::RunTest(const FString& Parameters)
{
	FAdvancedPreviewScene Scene{FPreviewScene::ConstructionValues()};
	Scene.SetFloorVisibility(false, true);
	Scene.SetEnvironmentVisibility(false, true);
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Cube available"), Cube)) return false;
	TArray<UPrimitiveComponent*> Subjects;
	for (const FVector Location : {FVector(300, -100, 50), FVector(500, 0, 0), FVector(700, 260, -130)})
	{
		auto* Subject = NewObject<UStaticMeshComponent>();
		Subject->SetStaticMesh(Cube);
		Subject->SetMobility(EComponentMobility::Movable);
		Scene.AddComponent(Subject, FTransform(Location));
		Subjects.Add(Subject);
	}
	auto* Color = NewObject<USceneCaptureComponent2D>();
	auto* Depth = NewObject<USceneCaptureComponent2D>();
	for (auto* Component : {Color, Depth})
	{
		Component->bCaptureEveryFrame = false;
		Component->bCaptureOnMovement = false;
		Scene.AddComponent(Component, FTransform::Identity);
	}
	ON_SCOPE_EXIT
	{
		FlushRenderingCommands();
		Scene.RemoveComponent(Color);
		Scene.RemoveComponent(Depth);
		for (auto* Subject : Subjects) Scene.RemoveComponent(Subject);
	};
	FUbstreamCaptureTargetPool Pool;
	FUbstreamCaptureSettings Settings;
	Settings.Width = Settings.Height = 128;
	Settings.JpegQuality = 100;
	FUbstreamCaptureCamera Camera;
	Camera.FovDegrees = 90;
	Camera.NearClipPlane = 1;
	const TArray<FIntPoint> Samples = {FIntPoint(38, 51), FIntPoint(64, 64), FIntPoint(90, 77)};
	for (int32 Swap = 0; Swap < 2; ++Swap)
	{
		if (Swap)
		{
			Subjects[0]->SetWorldLocation(FVector(700, -260, 130));
			Subjects[2]->SetWorldLocation(FVector(300, 100, -50));
		}
		auto Pixels = FUbstreamCaptureService::CaptureFramePixels(Scene, Camera, 100 + Swap, Settings, Subjects, &Pool);
		if (!TestTrue(TEXT("GPU synchronous capture succeeds"), Pixels.bSuccess)) return false;
		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			const auto P = Samples[Index];
			const float Actual = Pixels.DepthPixels[P.Y * 128 + P.X].R;
			const float Expected = Swap ? 650.0f - 200.0f * Index : 250.0f + 200.0f * Index;
			AddInfo(FString::Printf(TEXT("Swap=%d sample=%d raw depth=%.3f cm expected=%.3f"), Swap, Index, Actual, Expected));
			TestTrue(TEXT("Scene depth is linear distance, not reversed device Z"), FMath::Abs(Actual - Expected) < 2);
		}
		AddInfo(FString::Printf(TEXT("Swap=%d raw background=%f"), Swap, Pixels.DepthPixels[0].R));
		const auto Sync = FUbstreamCaptureService::EncodeCapturedFrame(MoveTemp(Pixels));
		SaveDepth(FString::Printf(TEXT("gpu_%d_sync"), Swap), Sync);
		FImage SyncImage;
		if (!TestTrue(TEXT("Sync depth JPEG decodes"), DecodeDepth(Sync, SyncImage))) return false;
		auto Frame = FUbstreamAsyncFrameReadback::Begin(Scene, Camera, 200 + Swap, Settings, Subjects, &Pool, Color, Depth);
		if (!TestTrue(TEXT("Async capture starts"), Frame.IsValid())) return false;
		FlushRenderingCommands();
		const double Deadline = FPlatformTime::Seconds() + 15;
		while (!Frame->IsReady() && FPlatformTime::Seconds() < Deadline) FPlatformProcess::Sleep(0.001f);
		if (!TestTrue(TEXT("Async GPU readback ready"), Frame->IsReady())) return false;
		struct FCompletion { bool bDone = false; FUbstreamEditorWorldCaptureResult Result; };
		const auto Completion = MakeShared<FCompletion>();
		Frame->CompleteAndEncodeAsync([Completion](FUbstreamEditorWorldCaptureResult&& Result)
		{
			Completion->Result = MoveTemp(Result);
			Completion->bDone = true;
		});
		while (!Completion->bDone && FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.001f);
		}
		if (!TestTrue(TEXT("Async encoding completes"), Completion->bDone)) return false;
		SaveDepth(FString::Printf(TEXT("gpu_%d_async"), Swap), Completion->Result);
		FImage AsyncImage;
		if (!TestTrue(TEXT("Async depth JPEG decodes"), DecodeDepth(Completion->Result, AsyncImage))) return false;
		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			const auto P = Samples[Index];
			const int32 SyncGray = GrayAt(SyncImage, P.X, P.Y);
			const int32 AsyncGray = GrayAt(AsyncImage, P.X, P.Y);
			const bool bNear = Index != 1 && ((Index == 0) != !!Swap);
			// Off-axis cube sides extend beyond the sampled front faces, so
			// assert meaningful contrast rather than assuming front-face extrema.
			const int32 Expected = Index == 1 ? 150 : (bNear ? 255 : 50);
			AddInfo(FString::Printf(TEXT("Swap=%d sample=%d JPEG sync=%d async=%d expected=%d"), Swap, Index, SyncGray, AsyncGray, Expected));
			TestTrue(TEXT("Near white / middle gray / far black survives JPEG and swapping"), FMath::Abs(SyncGray - Expected) <= 25);
			TestTrue(TEXT("Async agrees with sync at known surface"), FMath::Abs(SyncGray - AsyncGray) <= 3);
		}
		TestTrue(TEXT("Sync background is black"), GrayAt(SyncImage, 0, 0) <= 2);
		TestTrue(TEXT("Async background is black"), GrayAt(AsyncImage, 0, 0) <= 2);
	}
	return true;
}
#endif
