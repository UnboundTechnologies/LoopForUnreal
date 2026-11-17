#include "Capture/UbstreamCaptureService.h"

#include "AdvancedPreviewScene.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "UObject/UObjectIterator.h"
#include "Engine/StaticMesh.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "StaticMeshAttributes.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	UStaticMeshComponent* AddPreviewCube(
		FAdvancedPreviewScene& PreviewScene,
		UStaticMesh& CubeMesh,
		const FVector& Location,
		const FName Tag)
	{
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		Component->SetStaticMesh(&CubeMesh);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->ComponentTags.Add(Tag);

		const FTransform Transform(FRotator::ZeroRotator, Location, FVector::OneVector);
		Component->SetWorldTransform(Transform);
		PreviewScene.AddComponent(Component, Transform);
		Component->UpdateBounds();
		return Component;
	}

	void RemovePreviewComponent(FAdvancedPreviewScene& PreviewScene, UStaticMeshComponent* Component)
	{
		if (!IsValid(Component))
		{
			return;
		}

		PreviewScene.RemoveComponent(Component);
		Component->DestroyComponent();
	}

	uint64 HashBytes(const TArray<uint8>& Bytes)
	{
		uint64 Hash = 14695981039346656037ull;
		for (const uint8 Byte : Bytes)
		{
			Hash ^= Byte;
			Hash *= 1099511628211ull;
		}
		return Hash;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUbstreamCaptureIsolationAutomationTest,
	"Ubstream.Modeler.CaptureIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FUbstreamCaptureIsolationAutomationTest::RunTest(const FString& Parameters)
{
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh is available"), CubeMesh))
	{
		return false;
	}

	TSharedRef<FAdvancedPreviewScene> PreviewScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
	PreviewScene->SetFloorVisibility(false, true);
	PreviewScene->SetEnvironmentVisibility(false, true);

	UStaticMeshComponent* IncludedComponent = AddPreviewCube(*PreviewScene, *CubeMesh, FVector(0.0, -120.0, 0.0), TEXT("included_subject"));
	UStaticMeshComponent* ExcludedComponent = AddPreviewCube(*PreviewScene, *CubeMesh, FVector(0.0, 160.0, 0.0), TEXT("excluded_noise"));

	FUbstreamCaptureSettings Settings;
	Settings.Width = 128;
	Settings.Height = 128;
	Settings.JpegQuality = 90;
	Settings.bCaptureDepth = false;
	Settings.SessionId = TEXT("automation_capture_isolation");

	FUbstreamCaptureCamera Camera;
	Camera.Location = FVector(-550.0, 0.0, 0.0);
	Camera.Rotation = FRotator::ZeroRotator;
	Camera.FovDegrees = 60.0f;
	Camera.AspectRatio = 1.0f;

	const TArray<UPrimitiveComponent*> ShowOnlyComponents = { IncludedComponent };
	const FUbstreamEditorWorldCaptureResult IsolatedBeforeMove =
		FUbstreamCaptureService::CaptureFrameBundle(*PreviewScene, Camera, 1, Settings, ShowOnlyComponents);

	ExcludedComponent->SetWorldLocation(FVector(0.0, -240.0, 0.0));
	ExcludedComponent->UpdateBounds();

	const FUbstreamEditorWorldCaptureResult IsolatedAfterMove =
		FUbstreamCaptureService::CaptureFrameBundle(*PreviewScene, Camera, 2, Settings, ShowOnlyComponents);
	const FUbstreamEditorWorldCaptureResult Unisolated =
		FUbstreamCaptureService::CaptureFrameBundle(*PreviewScene, Camera, 3, Settings);

	auto CountRegisteredCaptures = [&PreviewScene]()
	{
		int32 Count = 0;
		for (TObjectIterator<USceneCaptureComponent2D> It; It; ++It)
		{
			if (It->GetWorld() == PreviewScene->GetWorld() && It->IsRegistered()) ++Count;
		}
		return Count;
	};
	const int32 CapturesBeforeRgb = CountRegisteredCaptures();
	const FUbstreamCaptureResult Rgb = FUbstreamCaptureService::CaptureRgb(*PreviewScene, Camera, Settings);
	TestTrue(TEXT("One-shot RGB capture succeeds"), Rgb.bSuccess);
	FImage RgbImage;
	TestTrue(TEXT("One-shot RGB PNG decodes"), FImageUtils::DecompressImage(Rgb.ColorPngBytes.GetData(), Rgb.ColorPngBytes.Num(), RgbImage));
	TestEqual(TEXT("RGB capture preserves width"), RgbImage.SizeX, Settings.Width);
	TestEqual(TEXT("RGB capture preserves height"), RgbImage.SizeY, Settings.Height);
	TestEqual(TEXT("One-shot RGB capture releases its transient component"), CountRegisteredCaptures(), CapturesBeforeRgb);

	const uint64 IsolatedBeforeMoveJpegHash = HashBytes(IsolatedBeforeMove.ColorJpegBytes);
	const uint64 IsolatedAfterMoveJpegHash = HashBytes(IsolatedAfterMove.ColorJpegBytes);
	const uint64 UnisolatedJpegHash = HashBytes(Unisolated.ColorJpegBytes);

	// Regression: small imported meshes (OBJ files in meter units) are framed
	// from a few units away with a tiny viewport near-plane override. The
	// capture must honor Camera.NearClipPlane; with the engine default
	// (10 units) the whole subject sits inside the near plane and every
	// fallback renders black.
	UStaticMeshComponent* TinyComponent = AddPreviewCube(*PreviewScene, *CubeMesh, FVector::ZeroVector, TEXT("tiny_subject"));
	TinyComponent->SetWorldScale3D(FVector(0.02));
	TinyComponent->UpdateBounds();

	FUbstreamCaptureCamera CloseCamera;
	CloseCamera.Location = FVector(-4.0, 0.0, 0.0);
	CloseCamera.Rotation = FRotator::ZeroRotator;
	CloseCamera.FovDegrees = 60.0f;
	CloseCamera.AspectRatio = 1.0f;
	CloseCamera.NearClipPlane = 0.05f;

	const TArray<UPrimitiveComponent*> TinyShowOnly = { TinyComponent };
	const FUbstreamEditorWorldCaptureResult TinyWithNearClip =
		FUbstreamCaptureService::CaptureFrameBundle(*PreviewScene, CloseCamera, 4, Settings, TinyShowOnly);

	CloseCamera.NearClipPlane = 0.0f;
	const FUbstreamEditorWorldCaptureResult TinyDefaultNearClip =
		FUbstreamCaptureService::CaptureFrameBundle(*PreviewScene, CloseCamera, 5, Settings, TinyShowOnly);

	RemovePreviewComponent(*PreviewScene, TinyComponent);
	RemovePreviewComponent(*PreviewScene, IncludedComponent);
	RemovePreviewComponent(*PreviewScene, ExcludedComponent);

	bool bPassed = true;
	bPassed &= TestTrue(TEXT("Show-only capture before moving excluded component succeeds"), IsolatedBeforeMove.bSuccess);
	bPassed &= TestTrue(TEXT("Show-only capture after moving excluded component succeeds"), IsolatedAfterMove.bSuccess);
	bPassed &= TestTrue(TEXT("Scene capture with excluded component succeeds"), Unisolated.bSuccess);
	bPassed &= TestEqual(TEXT("Exactly one show-only component is requested"), IsolatedBeforeMove.RequestedShowOnlyComponentCount, 1);
	bPassed &= TestEqual(TEXT("Exactly one show-only component is applied"), IsolatedBeforeMove.AppliedShowOnlyComponentCount, 1);
	bPassed &= TestEqual(
		TEXT("Moving excluded geometry does not change isolated color samples"),
		IsolatedBeforeMove.ColorSampleHash,
		IsolatedAfterMove.ColorSampleHash);
	bPassed &= TestEqual(
		TEXT("Moving excluded geometry does not change isolated JPEG output"),
		IsolatedBeforeMoveJpegHash,
		IsolatedAfterMoveJpegHash);
	bPassed &= TestNotEqual(
		TEXT("Unisolated scene capture includes extra visible geometry"),
		IsolatedBeforeMove.ColorSampleHash,
		Unisolated.ColorSampleHash);
	bPassed &= TestNotEqual(
		TEXT("Unisolated scene capture changes full JPEG output"),
		IsolatedBeforeMoveJpegHash,
		UnisolatedJpegHash);
	bPassed &= TestTrue(TEXT("Close-framed tiny subject capture succeeds"), TinyWithNearClip.bSuccess);
	bPassed &= TestTrue(
		TEXT("Tiny subject is visible when the capture honors the near-clip override"),
		!TinyWithNearClip.bUsedFlatColorFallback);
	bPassed &= TestNotEqual(
		TEXT("Near-clip override changes what a close-framed capture renders"),
		TinyWithNearClip.ColorSampleHash,
		TinyDefaultNearClip.ColorSampleHash);

	return bPassed;
}

// Regression for the post-import black passthrough: a freshly imported mesh is
// still async-compiling when the first frame is captured; without the capture
// service finishing compilation, the subject has no scene proxy and every
// pass - including the flat-color fallback - renders black.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUbstreamCaptureCompilingMeshAutomationTest,
	"Ubstream.Modeler.CaptureCompilingMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FUbstreamCaptureCompilingMeshAutomationTest::RunTest(const FString& Parameters)
{
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh is available"), CubeMesh))
	{
		return false;
	}

	const FMeshDescription* CubeDescription = CubeMesh->GetMeshDescription(0);
	if (!TestNotNull(TEXT("Cube mesh description is available"), CubeDescription))
	{
		return false;
	}

	// Unique geometry guarantees a DDC miss so the build actually runs (and,
	// with async static-mesh compilation enabled, runs asynchronously - the
	// same state a drag-dropped OBJ import is in when the first frame fires).
	FMeshDescription UniqueDescription = *CubeDescription;
	FStaticMeshAttributes Attributes(UniqueDescription);
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	const float UniqueScale = 1.0f + static_cast<float>(FPlatformTime::Cycles64() % 997ull) * 0.001f;
	for (const FVertexID VertexID : UniqueDescription.Vertices().GetElementIDs())
	{
		Positions[VertexID] = Positions[VertexID] * UniqueScale;
	}

	UStaticMesh* TransientMesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	TransientMesh->SetStaticMaterials(CubeMesh->GetStaticMaterials());
	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	BuildParams.bFastBuild = false;
	BuildParams.bMarkPackageDirty = false;
	BuildParams.bBuildSimpleCollision = false;
	if (!TestTrue(
		TEXT("Transient mesh builds from the unique cube description"),
		TransientMesh->BuildFromMeshDescriptions({ &UniqueDescription }, BuildParams)))
	{
		return false;
	}

	const bool bWasCompiling = TransientMesh->IsCompiling();
	AddInfo(FString::Printf(
		TEXT("Transient mesh compiling after build: %s"),
		bWasCompiling ? TEXT("yes (async repro active)") : TEXT("no (sync build; capture assertions still apply)")));

	TSharedRef<FAdvancedPreviewScene> PreviewScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
	PreviewScene->SetFloorVisibility(false, true);
	PreviewScene->SetEnvironmentVisibility(false, true);

	UStaticMeshComponent* Component = AddPreviewCube(*PreviewScene, *TransientMesh, FVector::ZeroVector, TEXT("compiling_subject"));
	if (bWasCompiling)
	{
		TestNull(TEXT("Compiling mesh has no scene proxy (the black-capture precondition)"), Component->SceneProxy);
	}

	FUbstreamCaptureSettings Settings;
	Settings.Width = 128;
	Settings.Height = 128;
	Settings.JpegQuality = 90;
	Settings.bCaptureDepth = false;
	Settings.SessionId = TEXT("automation_capture_compiling");

	FUbstreamCaptureCamera Camera;
	Camera.Rotation = FRotator(-24.0, 45.0, 0.0);
	Camera.Location = -FRotationMatrix(Camera.Rotation).GetUnitAxis(EAxis::X) * 400.0;
	Camera.FovDegrees = 45.0f;
	Camera.AspectRatio = 1.0f;
	Camera.NearClipPlane = 0.1f;

	const TArray<UPrimitiveComponent*> ShowOnly = { Component };
	const FUbstreamEditorWorldCaptureResult Capture =
		FUbstreamCaptureService::CaptureFrameBundle(*PreviewScene, Camera, 1, Settings, ShowOnly);

	bool bPassed = true;
	bPassed &= TestTrue(TEXT("Capture of a freshly built mesh succeeds"), Capture.bSuccess);
	bPassed &= TestFalse(TEXT("Mesh is no longer compiling after capture (guard ran)"), TransientMesh->IsCompiling());
	bPassed &= TestFalse(TEXT("No whole-scene fallback was needed"), Capture.bUsedSceneFallback);
	bPassed &= TestFalse(TEXT("No flat-color fallback was needed"), Capture.bUsedFlatColorFallback);

	FImage DecodedImage;
	if (TestTrue(
		TEXT("Captured JPEG decodes"),
		FImageUtils::DecompressImage(Capture.ColorJpegBytes.GetData(), Capture.ColorJpegBytes.Num(), DecodedImage)))
	{
		DecodedImage.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FImageView DecodedView = DecodedImage;
		const TArrayView64<FColor> Pixels = DecodedView.AsBGRA8();
		uint64 LumaSum = 0;
		for (const FColor& Pixel : Pixels)
		{
			LumaSum += static_cast<uint64>(Pixel.R) * 3 + static_cast<uint64>(Pixel.G) * 6 + static_cast<uint64>(Pixel.B);
		}
		const double MeanLuma = Pixels.Num() > 0
			? static_cast<double>(LumaSum) / static_cast<double>(Pixels.Num() * 10)
			: 0.0;
		bPassed &= TestTrue(
			FString::Printf(TEXT("Captured frame contains lit pixels (mean luma %.2f > 8)"), MeanLuma),
			MeanLuma > 8.0);
	}
	else
	{
		bPassed = false;
	}

	RemovePreviewComponent(*PreviewScene, Component);
	return bPassed;
}

#endif
