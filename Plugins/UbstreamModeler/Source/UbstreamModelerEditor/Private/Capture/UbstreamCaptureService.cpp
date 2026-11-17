#include "Capture/UbstreamCaptureService.h"
#include "UbstreamModelerEditorModule.h"
#include "Capture/UbstreamCaptureImageUtils.h"
#include "Capture/UbstreamCaptureFrameUtils.h"
#include "Capture/UbstreamCaptureSceneUtils.h"
#include "Capture/UbstreamCaptureEncodingCache.h"

#include "AdvancedPreviewScene.h"
#include "Capture/UbstreamCaptureTargetPool.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Protocol/UbstreamFrameEnvelope.h"
#include "RenderingThread.h"

DEFINE_LOG_CATEGORY_STATIC(LogUbstreamCapture, Log, All);

namespace
{
	struct FDepthNormalizationRange
	{
		float Near = 0.0f;
		float Far = 0.0f;
		bool bValid = false;
	};

	FString MakeCapturePath()
	{
		const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		return FPaths::Combine(UbstreamCaptureFrameUtils::GetCaptureOutputDir(), FString::Printf(TEXT("rgb_%s.png"), *Timestamp));
	}

	FString BuildCapturePath(uint64 FrameId, const TCHAR* Suffix)
	{
		return FPaths::Combine(UbstreamCaptureFrameUtils::GetCaptureOutputDir(), FString::Printf(TEXT("solo_frame_%llu_%s"), FrameId, Suffix));
	}

	// Game-thread color capture: renders and reads back raw pixels only - no
	// encoding. The nearly-black test reformats OutColorImage to BGRA8/sRGB,
	// which is also the format the JPEG encode wants, so the result is encode
	// ready.
	bool CaptureColorPixels(
		USceneCaptureComponent2D& CaptureComponent,
		UTextureRenderTarget2D& ColorRenderTarget,
		FImage& OutColorImage,
		FString& OutErrorMessage,
		bool* bOutNearlyBlack = nullptr)
	{
		CaptureComponent.TextureTarget = &ColorRenderTarget;
		CaptureComponent.CaptureSource = SCS_FinalColorLDR;
		CaptureComponent.CaptureScene();
		FlushRenderingCommands();

		if (!FImageUtils::GetRenderTargetImage(&ColorRenderTarget, OutColorImage))
		{
			OutErrorMessage = TEXT("Failed to read color render target");
			return false;
		}

		if (bOutNearlyBlack)
		{
			*bOutNearlyBlack = UbstreamCaptureImageUtils::IsNearlyBlackImage(OutColorImage);
		}

		return true;
	}

	bool BuildGrayDepthImage(
		const TArray<FLinearColor>& DepthPixels,
		int32 Width,
		int32 Height,
		const FDepthNormalizationRange& DepthRange,
		FImage& OutDepthImage,
		FString& OutErrorMessage)
	{
		if (DepthPixels.Num() != Width * Height)
		{
			OutErrorMessage = TEXT("Depth readback size did not match render target dimensions");
			return false;
		}

		float MinDepth = DepthRange.Near;
		float MaxDepth = DepthRange.Far;
		if (!DepthRange.bValid)
		{
			MinDepth = TNumericLimits<float>::Max();
			MaxDepth = 0.0f;
			for (const FLinearColor& Pixel : DepthPixels)
			{
				const float Depth = Pixel.R;
				if (UbstreamCaptureImageUtils::IsUsableSceneDepth(Depth))
				{
					MinDepth = FMath::Min(MinDepth, Depth);
					MaxDepth = FMath::Max(MaxDepth, Depth);
				}
			}
		}

		if (MinDepth == TNumericLimits<float>::Max() || MaxDepth < MinDepth)
		{
			MinDepth = 0.0f;
			MaxDepth = 1.0f;
		}
		else if (MaxDepth == MinDepth)
		{
			// A single visible depth still needs a white foreground silhouette.
			MaxDepth = MinDepth + 1.0f;
		}

		OutDepthImage = FImage(Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FImageView DepthImageView = OutDepthImage;
		TArrayView64<FColor> GrayPixels = DepthImageView.AsBGRA8();
		const float InvRange = 1.0f / FMath::Max(MaxDepth - MinDepth, KINDA_SMALL_NUMBER);

		for (int32 Index = 0; Index < DepthPixels.Num(); ++Index)
		{
			const float Depth = DepthPixels[Index].R;
			float ControlDepth = 0.0f;
			if (UbstreamCaptureImageUtils::IsUsableSceneDepth(Depth))
			{
				const float NormalizedDistance = FMath::Clamp((Depth - MinDepth) * InvRange, 0.0f, 1.0f);
				ControlDepth = 1.0f - NormalizedDistance;
			}
			const uint8 Gray = static_cast<uint8>(FMath::RoundToInt(ControlDepth * 255.0f));
			GrayPixels[Index] = FColor(Gray, Gray, Gray, 255);
		}

		return true;
	}

	// Game-thread depth capture: renders scene depth and reads back the raw
	// linear pixels only. The gray-image conversion + JPEG encode happen later in
	// EncodeCapturedFrame, off the game thread.
	bool CaptureDepthPixels(
		USceneCaptureComponent2D& CaptureComponent,
		UTextureRenderTarget2D& DepthRenderTarget,
		TArray<FLinearColor>& OutDepthPixels,
		FString& OutErrorMessage)
	{
		CaptureComponent.TextureTarget = &DepthRenderTarget;
		CaptureComponent.CaptureSource = SCS_SceneDepth;
		CaptureComponent.CaptureScene();
		FlushRenderingCommands();

		FTextureRenderTargetResource* DepthResource = DepthRenderTarget.GameThread_GetRenderTargetResource();
		if (!DepthResource)
		{
			OutErrorMessage = TEXT("Depth render target has no render resource");
			return false;
		}

		if (!DepthResource->ReadLinearColorPixels(OutDepthPixels, FReadSurfaceDataFlags(RCM_MinMax)))
		{
			OutErrorMessage = TEXT("Failed to read depth render target");
			return false;
		}

		return true;
	}


}

FUbstreamCaptureResult FUbstreamCaptureService::CaptureRgb(
	FAdvancedPreviewScene& PreviewScene,
	const FUbstreamCaptureCamera& Camera,
	const FUbstreamCaptureSettings& Settings)
{
	FUbstreamCaptureResult Result;
	if (FUbstreamModelerEditorModule::IsInsideRenderFlush())
	{
		Result.ErrorMessage = TEXT("Capture deferred while rendering commands are being flushed.");
		return Result;
	}
	Result.Size = FIntPoint(Settings.Width, Settings.Height);
	Result.Camera = Camera;

	if (Settings.Width <= 0 || Settings.Height <= 0)
	{
		Result.ErrorMessage = TEXT("Capture resolution must be positive");
		return Result;
	}

	UWorld* PreviewWorld = PreviewScene.GetWorld();
	if (!PreviewWorld)
	{
		Result.ErrorMessage = TEXT("Preview scene has no world");
		return Result;
	}

	UTextureRenderTarget2D* ColorRenderTarget =
		UbstreamCaptureSceneUtils::CreateRenderTarget(Settings.Width, Settings.Height, PF_B8G8R8A8);

	USceneCaptureComponent2D* CaptureComponent = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
	CaptureComponent->TextureTarget = ColorRenderTarget;
	CaptureComponent->CaptureSource = SCS_FinalColorLDR;
	CaptureComponent->bCaptureEveryFrame = false;
	CaptureComponent->bCaptureOnMovement = false;
	CaptureComponent->FOVAngle = Camera.FovDegrees;
	if (Camera.NearClipPlane > 0.0f)
	{
		CaptureComponent->bOverride_CustomNearClippingPlane = true;
		CaptureComponent->CustomNearClippingPlane = Camera.NearClipPlane;
	}
	UbstreamCaptureSceneUtils::ConfigureCleanCapture(*CaptureComponent);

	const FTransform CaptureTransform(Camera.Rotation, Camera.Location, FVector::OneVector);
	PreviewScene.AddComponent(CaptureComponent, CaptureTransform);
	ON_SCOPE_EXIT
	{
		PreviewScene.RemoveComponent(CaptureComponent);
		CaptureComponent->DestroyComponent();
	};
	CaptureComponent->SetWorldTransform(CaptureTransform);
	CaptureComponent->CaptureScene();

	FlushRenderingCommands();

	FImage ColorImage;
	if (!FImageUtils::GetRenderTargetImage(ColorRenderTarget, ColorImage))
	{
		Result.ErrorMessage = TEXT("Failed to read color render target");
		return Result;
	}

	if (!FImageUtils::CompressImage(Result.ColorPngBytes, TEXT("png"), ColorImage))
	{
		Result.ErrorMessage = TEXT("Failed to encode color image as PNG");
		return Result;
	}

	if (Settings.bSaveDebugImage)
	{
		Result.ColorDebugPath = MakeCapturePath();
		if (!FFileHelper::SaveArrayToFile(Result.ColorPngBytes, *Result.ColorDebugPath))
		{
			UE_LOG(LogUbstreamCapture, Warning, TEXT("Failed to save debug RGB capture to %s"), *Result.ColorDebugPath);
			Result.ColorDebugPath.Reset();
		}
	}

	Result.bSuccess = true;
	return Result;
}

FUbstreamEditorWorldCaptureResult FUbstreamCaptureService::CaptureFrameBundle(
	FAdvancedPreviewScene& PreviewScene,
	const FUbstreamCaptureCamera& Camera,
	uint64 FrameId,
	const FUbstreamCaptureSettings& Settings)
{
	const TArray<UPrimitiveComponent*> EmptyShowOnlyList;
	return CaptureFrameBundle(PreviewScene, Camera, FrameId, Settings, EmptyShowOnlyList);
}

FUbstreamEditorWorldCaptureResult FUbstreamCaptureService::CaptureFrameBundle(
	FAdvancedPreviewScene& PreviewScene,
	const FUbstreamCaptureCamera& Camera,
	uint64 FrameId,
	const FUbstreamCaptureSettings& Settings,
	const TArray<UPrimitiveComponent*>& ShowOnlyComponents,
	FUbstreamCaptureTargetPool* TargetPool)
{
	return EncodeCapturedFrame(
		CaptureFramePixels(PreviewScene, Camera, FrameId, Settings, ShowOnlyComponents, TargetPool, /*ReusedComponent*/ nullptr));
}

FUbstreamCapturePixels FUbstreamCaptureService::CaptureFramePixels(
	FAdvancedPreviewScene& PreviewScene,
	const FUbstreamCaptureCamera& Camera,
	uint64 FrameId,
	const FUbstreamCaptureSettings& Settings,
	const TArray<UPrimitiveComponent*>& ShowOnlyComponents,
	FUbstreamCaptureTargetPool* TargetPool,
	USceneCaptureComponent2D* ReusedComponent)
{
	FUbstreamCapturePixels Pixels;
	if (FUbstreamModelerEditorModule::IsInsideRenderFlush())
	{
		Pixels.ErrorMessage = TEXT("Capture deferred while rendering commands are being flushed.");
		return Pixels;
	}
	Pixels.FrameId = FrameId;
	Pixels.Size = FIntPoint(Settings.Width, Settings.Height);
	Pixels.Camera = Camera;
	Pixels.SessionId = Settings.SessionId;
	Pixels.JpegQuality = Settings.JpegQuality;
	Pixels.EncodingCache = TargetPool ? TargetPool->GetEncodingCache() : nullptr;
	Pixels.bCaptureDepth = Settings.bCaptureDepth;
	Pixels.bSaveDebugImage = Settings.bSaveDebugImage;
	Pixels.RequestedShowOnlyComponentCount = ShowOnlyComponents.Num();

	if (Settings.Width <= 0 || Settings.Height <= 0)
	{
		Pixels.ErrorMessage = TEXT("Capture resolution must be positive");
		return Pixels;
	}

	UWorld* PreviewWorld = PreviewScene.GetWorld();
	if (!PreviewWorld)
	{
		Pixels.ErrorMessage = TEXT("Preview scene has no world");
		return Pixels;
	}

	UbstreamCaptureSceneUtils::FinishMeshCompilationForCapture(ShowOnlyComponents);

	UTextureRenderTarget2D* ColorRenderTarget = TargetPool
		? TargetPool->AcquireColorTarget(Settings.Width, Settings.Height)
		: UbstreamCaptureSceneUtils::CreateRenderTarget(Settings.Width, Settings.Height, PF_B8G8R8A8);
	UTextureRenderTarget2D* DepthRenderTarget = nullptr;
	if (Settings.bCaptureDepth)
	{
		DepthRenderTarget = TargetPool
			? TargetPool->AcquireDepthTarget(Settings.Width, Settings.Height)
			: UbstreamCaptureSceneUtils::CreateRenderTarget(Settings.Width, Settings.Height, PF_FloatRGBA);
	}

	// Reuse the caller's persistent component when provided (streaming path), or
	// create a transient one for this call (one-shot callers). A reused component
	// is already registered in the scene and carries show-flag/show-only state
	// from prior frames, so it is fully reconfigured below.
	USceneCaptureComponent2D* CaptureComponent = ReusedComponent;
	const bool bOwnsComponent = (CaptureComponent == nullptr);
	if (bOwnsComponent)
	{
		CaptureComponent = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
		CaptureComponent->bCaptureEveryFrame = false;
		CaptureComponent->bCaptureOnMovement = false;
		PreviewScene.AddComponent(CaptureComponent, FTransform::Identity);
	}

	ON_SCOPE_EXIT
	{
		if (bOwnsComponent && CaptureComponent)
		{
			PreviewScene.RemoveComponent(CaptureComponent);
			CaptureComponent->DestroyComponent();
		}
	};

	CaptureComponent->TextureTarget = ColorRenderTarget;
	CaptureComponent->CaptureSource = SCS_FinalColorLDR;
	UbstreamCaptureSceneUtils::ConfigureCamera(*CaptureComponent, Camera);
	UbstreamCaptureSceneUtils::ConfigureCleanCapture(*CaptureComponent);
	// A reused component may have been left in flat-color fallback mode by a prior
	// frame (which disables these); restore the lit pass the normal capture wants.
	CaptureComponent->ShowFlags.SetLighting(true);
	CaptureComponent->ShowFlags.SetMaterials(true);
	CaptureComponent->ShowFlags.SetPostProcessing(true);
	UbstreamCaptureSceneUtils::ClearShowOnlyComponents(*CaptureComponent);
	Pixels.AppliedShowOnlyComponentCount = UbstreamCaptureSceneUtils::ApplyShowOnlyComponents(*CaptureComponent, ShowOnlyComponents);

	const FTransform CaptureTransform(Camera.Rotation, Camera.Location, FVector::OneVector);
	CaptureComponent->SetWorldTransform(CaptureTransform);

	bool bColorNearlyBlack = false;
	if (!CaptureColorPixels(*CaptureComponent, *ColorRenderTarget, Pixels.ColorImage, Pixels.ErrorMessage, &bColorNearlyBlack))
	{
		return Pixels;
	}

	if (bColorNearlyBlack && Pixels.AppliedShowOnlyComponentCount > 0)
	{
		UbstreamCaptureSceneUtils::ClearShowOnlyComponents(*CaptureComponent);
		Pixels.AppliedShowOnlyComponentCount = 0;
		Pixels.bUsedSceneFallback = true;

		if (!CaptureColorPixels(*CaptureComponent, *ColorRenderTarget, Pixels.ColorImage, Pixels.ErrorMessage, &bColorNearlyBlack))
		{
			return Pixels;
		}
	}

	if (bColorNearlyBlack)
	{
		UbstreamCaptureSceneUtils::ConfigureFlatColorFallbackCapture(*CaptureComponent);
		Pixels.bUsedFlatColorFallback = true;

		if (!CaptureColorPixels(*CaptureComponent, *ColorRenderTarget, Pixels.ColorImage, Pixels.ErrorMessage, &bColorNearlyBlack))
		{
			return Pixels;
		}
	}

	if (Settings.bCaptureDepth && DepthRenderTarget)
	{
		if (!CaptureDepthPixels(*CaptureComponent, *DepthRenderTarget, Pixels.DepthPixels, Pixels.ErrorMessage))
		{
			return Pixels;
		}
	}

	Pixels.bSuccess = true;
	return Pixels;
}

FUbstreamEditorWorldCaptureResult FUbstreamCaptureService::EncodeCapturedFrame(FUbstreamCapturePixels Pixels)
{
	FUbstreamEditorWorldCaptureResult Result;
	Result.SessionId = Pixels.SessionId;
	Result.FrameId = Pixels.FrameId;
	Result.Size = Pixels.Size;
	Result.Camera = Pixels.Camera;
	Result.RequestedShowOnlyComponentCount = Pixels.RequestedShowOnlyComponentCount;
	Result.AppliedShowOnlyComponentCount = Pixels.AppliedShowOnlyComponentCount;
	Result.bUsedSceneFallback = Pixels.bUsedSceneFallback;
	Result.bUsedFlatColorFallback = Pixels.bUsedFlatColorFallback;

	if (!Pixels.bSuccess)
	{
		Result.ErrorMessage = Pixels.ErrorMessage;
		return Result;
	}

	Result.ColorSampleHash = UbstreamCaptureImageUtils::BuildDeterministicColorSampleHash(Pixels.ColorImage);

	TArray64<uint8> CompressedColor;
	const bool bColorEncoded = Pixels.EncodingCache.IsValid()
		? Pixels.EncodingCache->Compress(MoveTemp(Pixels.ColorImage), Pixels.JpegQuality, false, CompressedColor)
		: FImageUtils::CompressImage(CompressedColor, TEXT("jpg"), Pixels.ColorImage, Pixels.JpegQuality);
	if (!bColorEncoded)
	{
		Result.ErrorMessage = TEXT("Failed to encode color image as JPEG");
		return Result;
	}
	if (!UbstreamCaptureImageUtils::TakeImageBytes(CompressedColor, Result.ColorJpegBytes))
	{
		Result.ErrorMessage = TEXT("Color JPEG is too large for the current frame bundle path");
		return Result;
	}

	if (Pixels.bCaptureDepth && Pixels.DepthPixels.Num() > 0)
	{
		FImage DepthImage;
		if (!BuildGrayDepthImage(Pixels.DepthPixels, Pixels.Size.X, Pixels.Size.Y, FDepthNormalizationRange(), DepthImage, Result.ErrorMessage))
		{
			return Result;
		}

		TArray64<uint8> CompressedDepth;
		const bool bDepthEncoded = Pixels.EncodingCache.IsValid()
			? Pixels.EncodingCache->Compress(MoveTemp(DepthImage), Pixels.JpegQuality, true, CompressedDepth)
			: FImageUtils::CompressImage(CompressedDepth, TEXT("jpg"), DepthImage, Pixels.JpegQuality);
		if (!bDepthEncoded)
		{
			Result.ErrorMessage = TEXT("Failed to encode depth image as grayscale JPEG");
			return Result;
		}
		if (!UbstreamCaptureImageUtils::TakeImageBytes(CompressedDepth, Result.DepthJpegBytes))
		{
			Result.ErrorMessage = TEXT("Depth JPEG is too large for the current frame bundle path");
			return Result;
		}
	}

	Result.HeaderJson = UbstreamCaptureFrameUtils::BuildFrameHeaderJson(Result);
	if (!FUbstreamFrameEnvelope::BuildFrameBundle(Result.FrameId, Result.HeaderJson, Result.ColorJpegBytes, Result.DepthJpegBytes, Result.FrameBundleBytes))
	{
		Result.ErrorMessage = TEXT("Failed to build UBS1 frame bundle");
		return Result;
	}

	if (Pixels.bSaveDebugImage)
	{
		Result.HeaderDebugPath = BuildCapturePath(Result.FrameId, TEXT("header.json"));
		Result.ColorDebugPath = BuildCapturePath(Result.FrameId, TEXT("color.jpg"));
		Result.DepthDebugPath = Result.DepthJpegBytes.IsEmpty() ? FString() : BuildCapturePath(Result.FrameId, TEXT("depth.jpg"));

		if (!FFileHelper::SaveStringToFile(Result.HeaderJson, *Result.HeaderDebugPath))
		{
			UE_LOG(LogUbstreamCapture, Warning, TEXT("Failed to save solo frame header to %s"), *Result.HeaderDebugPath);
			Result.HeaderDebugPath.Reset();
		}

		if (!FFileHelper::SaveArrayToFile(Result.ColorJpegBytes, *Result.ColorDebugPath))
		{
			UE_LOG(LogUbstreamCapture, Warning, TEXT("Failed to save solo color capture to %s"), *Result.ColorDebugPath);
			Result.ColorDebugPath.Reset();
		}

		if (!Result.DepthJpegBytes.IsEmpty() && !FFileHelper::SaveArrayToFile(Result.DepthJpegBytes, *Result.DepthDebugPath))
		{
			UE_LOG(LogUbstreamCapture, Warning, TEXT("Failed to save solo depth capture to %s"), *Result.DepthDebugPath);
			Result.DepthDebugPath.Reset();
		}
	}

	Result.bSuccess = true;
	return Result;
}
