#include "Capture/UbstreamAsyncFrameReadback.h"
#include "UbstreamModelerEditorModule.h"
#include "Capture/UbstreamCaptureService.h"

#include "AdvancedPreviewScene.h"
#include "Async/Async.h"
#include "Capture/UbstreamCaptureImageUtils.h"
#include "Capture/UbstreamCaptureSceneUtils.h"
#include "Capture/UbstreamCaptureTargetPool.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"

TSharedPtr<FUbstreamAsyncFrameReadback, ESPMode::ThreadSafe> FUbstreamAsyncFrameReadback::Begin(
	FAdvancedPreviewScene& PreviewScene,
	const FUbstreamCaptureCamera& Camera,
	const uint64 FrameId,
	const FUbstreamCaptureSettings& Settings,
	const TArray<UPrimitiveComponent*>& ShowOnlyComponents,
	FUbstreamCaptureTargetPool* TargetPool,
	USceneCaptureComponent2D* ReusedComponent,
	USceneCaptureComponent2D* ReusedDepthComponent)
{
	if (FUbstreamModelerEditorModule::IsInsideRenderFlush())
	{
		return nullptr;
	}

	// The streaming path owns persistent targets and persistent capture
	// components; the async design depends on them outliving the readback.
	// Depth needs its OWN component: two same-frame CaptureScene calls on one
	// component race each other's target/source state (the second call's
	// state won the race and the color target stayed black).
	if (!TargetPool || !ReusedComponent || Settings.Width <= 0 || Settings.Height <= 0
		|| !PreviewScene.GetWorld()
		|| (Settings.bCaptureDepth && !ReusedDepthComponent))
	{
		return nullptr;
	}

	UbstreamCaptureSceneUtils::FinishMeshCompilationForCapture(ShowOnlyComponents);

	UTextureRenderTarget2D* ColorRenderTarget = TargetPool->AcquireColorTarget(Settings.Width, Settings.Height);
	UTextureRenderTarget2D* DepthRenderTarget = Settings.bCaptureDepth
		? TargetPool->AcquireDepthTarget(Settings.Width, Settings.Height)
		: nullptr;
	if (!ColorRenderTarget)
	{
		return nullptr;
	}

	const TSharedPtr<FUbstreamAsyncFrameReadback, ESPMode::ThreadSafe> Frame =
		MakeShared<FUbstreamAsyncFrameReadback, ESPMode::ThreadSafe>();
	Frame->BeginTimeSeconds = FPlatformTime::Seconds();
	Frame->Pixels.FrameId = FrameId;
	Frame->Pixels.Size = FIntPoint(Settings.Width, Settings.Height);
	Frame->Pixels.Camera = Camera;
	Frame->Pixels.SessionId = Settings.SessionId;
	Frame->Pixels.JpegQuality = Settings.JpegQuality;
	Frame->Pixels.EncodingCache = TargetPool->GetEncodingCache();
	Frame->Pixels.bCaptureDepth = Settings.bCaptureDepth && DepthRenderTarget != nullptr;
	Frame->Pixels.bSaveDebugImage = Settings.bSaveDebugImage;
	Frame->Pixels.RequestedShowOnlyComponentCount = ShowOnlyComponents.Num();

	USceneCaptureComponent2D* CaptureComponent = ReusedComponent;
	CaptureComponent->TextureTarget = ColorRenderTarget;
	CaptureComponent->CaptureSource = SCS_FinalColorLDR;
	UbstreamCaptureSceneUtils::ConfigureCamera(*CaptureComponent, Camera);
	UbstreamCaptureSceneUtils::ConfigureCleanCapture(*CaptureComponent);
	CaptureComponent->ShowFlags.SetLighting(true);
	CaptureComponent->ShowFlags.SetMaterials(true);
	CaptureComponent->ShowFlags.SetPostProcessing(true);
	UbstreamCaptureSceneUtils::ClearShowOnlyComponents(*CaptureComponent);
	Frame->Pixels.AppliedShowOnlyComponentCount = UbstreamCaptureSceneUtils::ApplyShowOnlyComponents(*CaptureComponent, ShowOnlyComponents);

	CaptureComponent->SetWorldTransform(FTransform(Camera.Rotation, Camera.Location, FVector::OneVector));

	CaptureComponent->CaptureScene();

	if (DepthRenderTarget && ReusedDepthComponent)
	{
		ReusedDepthComponent->TextureTarget = DepthRenderTarget;
		ReusedDepthComponent->CaptureSource = SCS_SceneDepth;
		UbstreamCaptureSceneUtils::ConfigureCamera(*ReusedDepthComponent, Camera);
		UbstreamCaptureSceneUtils::ConfigureCleanCapture(*ReusedDepthComponent);
		UbstreamCaptureSceneUtils::ClearShowOnlyComponents(*ReusedDepthComponent);
		UbstreamCaptureSceneUtils::ApplyShowOnlyComponents(*ReusedDepthComponent, ShowOnlyComponents);
		ReusedDepthComponent->SetWorldTransform(FTransform(Camera.Rotation, Camera.Location, FVector::OneVector));
		ReusedDepthComponent->CaptureScene();
	}

	Frame->ColorReadback = TargetPool->AcquireReadback(Frame->Pixels.Size, PF_B8G8R8A8);
	if (DepthRenderTarget)
	{
		Frame->DepthReadback = TargetPool->AcquireReadback(Frame->Pixels.Size, PF_FloatRGBA);
	}

	FTextureRenderTargetResource* ColorResource = ColorRenderTarget->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* DepthResource = DepthRenderTarget
		? DepthRenderTarget->GameThread_GetRenderTargetResource()
		: nullptr;
	ENQUEUE_RENDER_COMMAND(UbstreamEnqueueFrameReadback)(
		[Frame, ColorResource, DepthResource](FRHICommandListImmediate& RHICmdList)
	{
		if (!ColorResource || !ColorResource->GetRenderTargetTexture()
			|| (Frame->DepthReadback.IsValid() && (!DepthResource || !DepthResource->GetRenderTargetTexture())))
		{
			return; // Leave readiness false so the existing timeout fallback runs.
		}
		const auto EnqueueReadback = [&RHICmdList](FRHIGPUTextureReadback& Readback, FRHITexture* Texture)
		{
			// EnqueueCopy transitions its staging destination, not this source.
			// Restore the scene capture's shader-readable final state afterwards
			// so subsequent captures and sampling can reuse the same target.
			RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
			Readback.EnqueueCopy(RHICmdList, Texture);
			RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		};
		EnqueueReadback(*Frame->ColorReadback, ColorResource->GetRenderTargetTexture());
		if (Frame->DepthReadback.IsValid())
		{
			EnqueueReadback(*Frame->DepthReadback, DepthResource->GetRenderTargetTexture());
		}
		Frame->bCopyCommandsEnqueued.Store(true);
	});

	return Frame;
}

bool FUbstreamAsyncFrameReadback::IsReady() const
{
	if (!bCopyCommandsEnqueued.Load())
	{
		return false;
	}
	const bool bColorReady = ColorReadback.IsValid() && ColorReadback->IsReady();
	const bool bDepthReady = !DepthReadback.IsValid() || DepthReadback->IsReady();
	return bColorReady && bDepthReady;
}

bool FUbstreamAsyncFrameReadback::HasTimedOut() const
{
	return FPlatformTime::Seconds() - BeginTimeSeconds > 2.0;
}

void FUbstreamAsyncFrameReadback::CompleteAndEncodeAsync(TUniqueFunction<void(FUbstreamEditorWorldCaptureResult&&)> OnDone)
{
	TSharedRef<FUbstreamAsyncFrameReadback, ESPMode::ThreadSafe> Self = AsShared();
	ENQUEUE_RENDER_COMMAND(UbstreamCompleteFrameReadback)(
		[Self, OnDone = MoveTemp(OnDone)](FRHICommandListImmediate& RHICmdList) mutable
	{
		const int32 Width = Self->Pixels.Size.X;
		const int32 Height = Self->Pixels.Size.Y;

		bool bLockedOk = false;
		if (Self->ColorReadback.IsValid())
		{
			int32 RowPitchInPixels = 0;
			if (const void* ColorData = Self->ColorReadback->Lock(RowPitchInPixels))
			{
				Self->Pixels.ColorImage = FImage(Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
				FImageView ColorView = Self->Pixels.ColorImage;
				TArrayView64<FColor> Destination = ColorView.AsBGRA8();
				const FColor* Source = static_cast<const FColor*>(ColorData);
				for (int32 Row = 0; Row < Height; ++Row)
				{
					FMemory::Memcpy(
						Destination.GetData() + static_cast<int64>(Row) * Width,
						Source + static_cast<int64>(Row) * RowPitchInPixels,
						sizeof(FColor) * Width);
				}
				Self->ColorReadback->Unlock();
				bLockedOk = true;
			}
		}

		if (bLockedOk && Self->DepthReadback.IsValid())
		{
			int32 RowPitchInPixels = 0;
			if (const void* DepthData = Self->DepthReadback->Lock(RowPitchInPixels))
			{
				Self->Pixels.DepthPixels.SetNumUninitialized(static_cast<int64>(Width) * Height);
				const FFloat16Color* Source = static_cast<const FFloat16Color*>(DepthData);
				for (int32 Row = 0; Row < Height; ++Row)
				{
					const FFloat16Color* SourceRow = Source + static_cast<int64>(Row) * RowPitchInPixels;
					FLinearColor* DestinationRow = Self->Pixels.DepthPixels.GetData() + static_cast<int64>(Row) * Width;
					UbstreamCaptureImageUtils::ConvertDepthRow(SourceRow, DestinationRow, Width);
				}
				Self->DepthReadback->Unlock();
			}
			else
			{
				bLockedOk = false;
			}
		}

		Async(EAsyncExecution::ThreadPool, [Self, OnDone = MoveTemp(OnDone), bLockedOk]() mutable
		{
			FUbstreamEditorWorldCaptureResult Result;
			if (!bLockedOk)
			{
				Result.FrameId = Self->Pixels.FrameId;
				Result.ErrorMessage = TEXT("GPU readback lock failed");
			}
			else if (UbstreamCaptureImageUtils::IsNearlyBlackImage(Self->Pixels.ColorImage) && Self->Pixels.AppliedShowOnlyComponentCount > 0)
			{
				Result.FrameId = Self->Pixels.FrameId;
				Result.bNearlyBlackWithShowOnly = true;
				Result.ErrorMessage = TEXT("Show-only capture came back black");
			}
			else
			{
				Self->Pixels.bSuccess = true;
				Result = FUbstreamCaptureService::EncodeCapturedFrame(MoveTemp(Self->Pixels));
			}
			AsyncTask(ENamedThreads::GameThread, [OnDone = MoveTemp(OnDone), Result = MoveTemp(Result)]() mutable
			{
				OnDone(MoveTemp(Result));
			});
		});
	});
}
