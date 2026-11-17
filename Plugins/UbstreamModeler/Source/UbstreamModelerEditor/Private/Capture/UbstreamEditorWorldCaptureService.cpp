#include "Capture/UbstreamEditorWorldCaptureService.h"
#include "UbstreamModelerEditorModule.h"
#include "Capture/UbstreamCaptureImageUtils.h"
#include "Capture/UbstreamCaptureFrameUtils.h"
#include "Capture/UbstreamCaptureSceneUtils.h"

#include "Capture/UbstreamCaptureTargetPool.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Editor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Actor.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "LevelEditorViewport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Protocol/UbstreamFrameEnvelope.h"
#include "RenderingThread.h"
#include "Selection.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogUbstreamEditorWorldCapture, Log, All);

namespace
{
	struct FEditorWorldDepthRange
	{
		float Near = 0.0f;
		float Far = 0.0f;
		bool bValid = false;
	};

	FString BuildEditorWorldCapturePath(uint64 FrameId, const TCHAR* Suffix)
	{
		return FPaths::Combine(UbstreamCaptureFrameUtils::GetCaptureOutputDir(), FString::Printf(TEXT("frame_%llu_%s"), FrameId, Suffix));
	}

	UWorld* GetEditorCaptureWorld()
	{
		if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->GetWorld())
		{
			return GCurrentLevelEditingViewportClient->GetWorld();
		}

		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	FUbstreamCaptureCamera ResolveCaptureCamera(const FUbstreamCaptureSettings& Settings)
	{
		FUbstreamCaptureCamera Camera;
		Camera.FovDegrees = 45.0f;
		Camera.AspectRatio = Settings.Height > 0 ? static_cast<float>(Settings.Width) / static_cast<float>(Settings.Height) : 1.0f;

		if (GCurrentLevelEditingViewportClient)
		{
			Camera.Location = GCurrentLevelEditingViewportClient->GetViewLocation();
			Camera.Rotation = GCurrentLevelEditingViewportClient->GetViewRotation();
			Camera.FovDegrees = GCurrentLevelEditingViewportClient->ViewFOV;
			Camera.NearClipPlane = GCurrentLevelEditingViewportClient->GetNearClipPlane();
		}
		else
		{
			Camera.Location = FVector(300.0, -300.0, 200.0);
			Camera.Rotation = FRotator(-30.0, 135.0, 0.0);
		}

		return Camera;
	}

	FEditorWorldDepthRange ResolveThreeJsStyleDepthRange(const FUbstreamCaptureCamera& Camera)
	{
		FEditorWorldDepthRange Range;
		if (!GEditor)
		{
			return Range;
		}

		FBox SelectedBounds(ForceInit);
		for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
		{
			const AActor* Actor = Cast<AActor>(*It);
			if (!IsValid(Actor))
			{
				continue;
			}

			const FBox ActorBounds = Actor->GetComponentsBoundingBox(true);
			if (ActorBounds.IsValid)
			{
				SelectedBounds += ActorBounds;
			}
		}

		if (!SelectedBounds.IsValid)
		{
			return Range;
		}

		const FVector SceneCenter = SelectedBounds.GetCenter();
		const FVector SceneSize = SelectedBounds.GetSize();
		const float HalfSize = FMath::Max3(SceneSize.X, SceneSize.Y, SceneSize.Z) * 0.5f * 1.7320508075688772f;
		if (HalfSize <= KINDA_SMALL_NUMBER)
		{
			return Range;
		}

		const float SceneDistance = static_cast<float>(FVector::Dist(Camera.Location, SceneCenter));
		Range.Near = FMath::Max(0.0001f, SceneDistance - HalfSize);
		Range.Far = FMath::Max(Range.Near + KINDA_SMALL_NUMBER, SceneDistance + HalfSize);
		Range.bValid = true;
		return Range;
	}

	void ConfigureStableCapture(USceneCaptureComponent2D& CaptureComponent)
	{
		CaptureComponent.ShowFlags.SetCloud(false);
		CaptureComponent.ShowFlags.SetAtmosphere(false);
		CaptureComponent.ShowFlags.SetFog(false);
		CaptureComponent.ShowFlags.SetVolumetricFog(false);
		CaptureComponent.ShowFlags.SetGrid(false);
		CaptureComponent.ShowFlags.SetTemporalAA(false);
		CaptureComponent.ShowFlags.SetMotionBlur(false);
		CaptureComponent.ShowFlags.SetEyeAdaptation(false);
		CaptureComponent.ShowFlags.SetLocalExposure(false);
		CaptureComponent.ShowFlags.SetBloom(false);
		CaptureComponent.ShowFlags.SetScreenSpaceReflections(false);
	}

	bool CaptureColor(
		USceneCaptureComponent2D& CaptureComponent,
		UTextureRenderTarget2D& ColorRenderTarget,
		const FUbstreamCaptureSettings& Settings,
		TArray<uint8>& OutColorJpegBytes,
		uint64& OutColorSampleHash,
		FString& OutErrorMessage)
	{
		CaptureComponent.TextureTarget = &ColorRenderTarget;
		CaptureComponent.CaptureSource = SCS_FinalColorLDR;
		CaptureComponent.CaptureScene();
		FlushRenderingCommands();

		FImage ColorImage;
		if (!FImageUtils::GetRenderTargetImage(&ColorRenderTarget, ColorImage))
		{
			OutErrorMessage = TEXT("Failed to read color render target");
			return false;
		}

		OutColorSampleHash = UbstreamCaptureImageUtils::BuildDeterministicColorSampleHash(ColorImage);

		TArray64<uint8> CompressedColor;
		if (!FImageUtils::CompressImage(CompressedColor, TEXT("jpg"), ColorImage, Settings.JpegQuality))
		{
			OutErrorMessage = TEXT("Failed to encode color image as JPEG");
			return false;
		}

		if (!UbstreamCaptureImageUtils::TakeImageBytes(CompressedColor, OutColorJpegBytes))
		{
			OutErrorMessage = TEXT("Color JPEG is too large for the current frame bundle path");
			return false;
		}

		return true;
	}

	bool BuildEditorWorldGrayDepthImage(
		const TArray<FLinearColor>& DepthPixels,
		int32 Width,
		int32 Height,
		const FEditorWorldDepthRange& ThreeJsDepthRange,
		FImage& OutDepthImage,
		FString& OutErrorMessage)
	{
		if (DepthPixels.Num() != Width * Height)
		{
			OutErrorMessage = TEXT("Depth readback size did not match render target dimensions");
			return false;
		}

		float MinDepth = ThreeJsDepthRange.Near;
		float MaxDepth = ThreeJsDepthRange.Far;
		if (!ThreeJsDepthRange.bValid)
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
		const float DepthRange = FMath::Max(MaxDepth - MinDepth, KINDA_SMALL_NUMBER);

		for (int32 Index = 0; Index < DepthPixels.Num(); ++Index)
		{
			const float Depth = DepthPixels[Index].R;
			float ControlDepth = 0.0f;
			if (UbstreamCaptureImageUtils::IsUsableSceneDepth(Depth))
			{
				const float NormalizedDistance = FMath::Clamp((Depth - MinDepth) / DepthRange, 0.0f, 1.0f);
				ControlDepth = 1.0f - NormalizedDistance;
			}
			const uint8 Gray = static_cast<uint8>(FMath::RoundToInt(ControlDepth * 255.0f));
			GrayPixels[Index] = FColor(Gray, Gray, Gray, 255);
		}

		return true;
	}

	bool CaptureDepth(
		USceneCaptureComponent2D& CaptureComponent,
		UTextureRenderTarget2D& DepthRenderTarget,
		int32 Width,
		int32 Height,
		int32 JpegQuality,
		const FEditorWorldDepthRange& ThreeJsDepthRange,
		TArray<uint8>& OutDepthJpegBytes,
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

		TArray<FLinearColor> DepthPixels;
		if (!DepthResource->ReadLinearColorPixels(DepthPixels, FReadSurfaceDataFlags(RCM_MinMax)))
		{
			OutErrorMessage = TEXT("Failed to read depth render target");
			return false;
		}

		FImage DepthImage;
		if (!BuildEditorWorldGrayDepthImage(DepthPixels, Width, Height, ThreeJsDepthRange, DepthImage, OutErrorMessage))
		{
			return false;
		}

		TArray64<uint8> CompressedDepth;
		if (!FImageUtils::CompressImage(CompressedDepth, TEXT("jpg"), DepthImage, JpegQuality))
		{
			OutErrorMessage = TEXT("Failed to encode depth image as grayscale JPEG");
			return false;
		}

		if (!UbstreamCaptureImageUtils::TakeImageBytes(CompressedDepth, OutDepthJpegBytes))
		{
			OutErrorMessage = TEXT("Depth JPEG is too large for the current frame bundle path");
			return false;
		}

		return true;
	}


}

FUbstreamEditorWorldCaptureResult FUbstreamEditorWorldCaptureService::CaptureFrameBundle(
	uint64 FrameId,
	const FUbstreamCaptureSettings& Settings,
	FUbstreamCaptureTargetPool* TargetPool)
{
	FUbstreamEditorWorldCaptureResult Result;
	if (FUbstreamModelerEditorModule::IsInsideRenderFlush())
	{
		Result.ErrorMessage = TEXT("Capture deferred while rendering commands are being flushed.");
		return Result;
	}
	Result.SessionId = Settings.SessionId;
	Result.FrameId = FrameId;
	Result.Size = FIntPoint(Settings.Width, Settings.Height);
	Result.Camera = ResolveCaptureCamera(Settings);
	const FEditorWorldDepthRange ThreeJsDepthRange = ResolveThreeJsStyleDepthRange(Result.Camera);

	if (Settings.Width <= 0 || Settings.Height <= 0)
	{
		Result.ErrorMessage = TEXT("Capture resolution must be positive");
		return Result;
	}

	UWorld* World = GetEditorCaptureWorld();
	if (!World)
	{
		Result.ErrorMessage = TEXT("No editor world is available for capture");
		return Result;
	}

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

	USceneCaptureComponent2D* CaptureComponent = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
	CaptureComponent->bCaptureEveryFrame = false;
	CaptureComponent->bCaptureOnMovement = false;
	CaptureComponent->FOVAngle = Result.Camera.FovDegrees;
	if (Result.Camera.NearClipPlane > 0.0f)
	{
		CaptureComponent->bOverride_CustomNearClippingPlane = true;
		CaptureComponent->CustomNearClippingPlane = Result.Camera.NearClipPlane;
	}
	CaptureComponent->TextureTarget = ColorRenderTarget;
	CaptureComponent->CaptureSource = SCS_FinalColorLDR;
	CaptureComponent->SetWorldLocationAndRotation(Result.Camera.Location, Result.Camera.Rotation);
	ConfigureStableCapture(*CaptureComponent);
	CaptureComponent->RegisterComponentWithWorld(World);

	if (!CaptureColor(*CaptureComponent, *ColorRenderTarget, Settings, Result.ColorJpegBytes, Result.ColorSampleHash, Result.ErrorMessage))
	{
		CaptureComponent->UnregisterComponent();
		CaptureComponent->DestroyComponent();
		return Result;
	}

	if (Settings.bCaptureDepth && DepthRenderTarget)
	{
		if (!CaptureDepth(*CaptureComponent, *DepthRenderTarget, Settings.Width, Settings.Height, Settings.JpegQuality, ThreeJsDepthRange, Result.DepthJpegBytes, Result.ErrorMessage))
		{
			CaptureComponent->UnregisterComponent();
			CaptureComponent->DestroyComponent();
			return Result;
		}
	}

	CaptureComponent->UnregisterComponent();
	CaptureComponent->DestroyComponent();

	Result.HeaderJson = UbstreamCaptureFrameUtils::BuildFrameHeaderJson(Result);
	if (!FUbstreamFrameEnvelope::BuildFrameBundle(Result.FrameId, Result.HeaderJson, Result.ColorJpegBytes, Result.DepthJpegBytes, Result.FrameBundleBytes))
	{
		Result.ErrorMessage = TEXT("Failed to build UBS1 frame bundle");
		return Result;
	}

	if (Settings.bSaveDebugImage)
	{
		Result.HeaderDebugPath = BuildEditorWorldCapturePath(FrameId, TEXT("header.json"));
		Result.ColorDebugPath = BuildEditorWorldCapturePath(FrameId, TEXT("color.jpg"));
		Result.DepthDebugPath = Result.DepthJpegBytes.IsEmpty() ? FString() : BuildEditorWorldCapturePath(FrameId, TEXT("depth.jpg"));

		if (!FFileHelper::SaveStringToFile(Result.HeaderJson, *Result.HeaderDebugPath))
		{
			UE_LOG(LogUbstreamEditorWorldCapture, Warning, TEXT("Failed to save frame header to %s"), *Result.HeaderDebugPath);
			Result.HeaderDebugPath.Reset();
		}

		if (!FFileHelper::SaveArrayToFile(Result.ColorJpegBytes, *Result.ColorDebugPath))
		{
			UE_LOG(LogUbstreamEditorWorldCapture, Warning, TEXT("Failed to save color capture to %s"), *Result.ColorDebugPath);
			Result.ColorDebugPath.Reset();
		}

		if (!Result.DepthJpegBytes.IsEmpty() && !FFileHelper::SaveArrayToFile(Result.DepthJpegBytes, *Result.DepthDebugPath))
		{
			UE_LOG(LogUbstreamEditorWorldCapture, Warning, TEXT("Failed to save depth capture to %s"), *Result.DepthDebugPath);
			Result.DepthDebugPath.Reset();
		}
	}

	Result.bSuccess = true;
	return Result;
}
