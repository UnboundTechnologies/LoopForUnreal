#include "Capture/UbstreamCaptureService.h"
#include "AdvancedPreviewScene.h"
#include "Async/TaskGraphInterfaces.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Capture/UbstreamCaptureEncodingCache.h"
#include "ClearQuad.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Capture/UbstreamCaptureTargetPool.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "Widgets/SUbstreamControlPanel.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamReadbackReuseTest,
	"Ubstream.Modeler.Performance.ReadbackReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FUbstreamReadbackReuseTest::RunTest(const FString& Parameters)
{
	using FReadbackPtr = TSharedPtr<FRHIGPUTextureReadback, ESPMode::ThreadSafe>;
	FUbstreamCaptureTargetPool Pool;
	FReadbackPtr Readback;
	auto Capture = [this, &Pool, &Readback](FIntPoint Size, EPixelFormat Format, FLinearColor Color)
	{
		UTextureRenderTarget2D* Target = Format == PF_B8G8R8A8
			? Pool.AcquireColorTarget(Size.X, Size.Y) : Pool.AcquireDepthTarget(Size.X, Size.Y);

		Readback = Pool.AcquireReadback(Size, Format);
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		ENQUEUE_RENDER_COMMAND(UbstreamTestReadbackCopy)(
			[Lease = Readback, Resource, Size, Color](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* Texture = Resource->GetRenderTargetTexture();
			RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::RTV));
			RHICmdList.BeginRenderPass(FRHIRenderPassInfo(Texture, ERenderTargetActions::DontLoad_Store), TEXT("UbstreamTestClear"));
			RHICmdList.SetViewport(0, 0, 0, Size.X, Size.Y, 1);
			DrawClearQuad(RHICmdList, Color);
			RHICmdList.EndRenderPass();
			RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::RTV, ERHIAccess::CopySrc));
			Lease->EnqueueCopy(RHICmdList, Texture);
		});
		FlushRenderingCommands();
		const double Deadline = FPlatformTime::Seconds() + 5.0;
		while (!Readback->IsReady() && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.001f);
		}
		if (!TestTrue(TEXT("Readback completes on GPU"), Readback->IsReady()))
		{
			return false;
		}
		FLinearColor First, Last;
		bool bMapped = false;
		ENQUEUE_RENDER_COMMAND(UbstreamTestReadbackPixels)(
			[Lease = Readback, Size, Format, &First, &Last, &bMapped](FRHICommandListImmediate& RHICmdList)
		{
			int32 Pitch = 0, BufferHeight = 0;
			if (const void* Data = Lease->Lock(Pitch, &BufferHeight))
			{
				bMapped = Pitch >= Size.X && BufferHeight >= Size.Y;
				if (bMapped)
				{
					const int64 End = static_cast<int64>(Size.Y - 1) * Pitch + Size.X - 1;
					if (Format == PF_B8G8R8A8)
					{
						First = FLinearColor(static_cast<const FColor*>(Data)[0]);
						Last = FLinearColor(static_cast<const FColor*>(Data)[End]);
					}
					else
					{
						First = FLinearColor(static_cast<const FFloat16Color*>(Data)[0]);
						Last = FLinearColor(static_cast<const FFloat16Color*>(Data)[End]);
					}
				}
				Lease->Unlock();
			}
		});
		FlushRenderingCommands();
		TestTrue(TEXT("Staging extent covers every requested pixel"), bMapped);
		TestTrue(TEXT("First pixel belongs to current frame"), First.Equals(Color, 0.01f));
		TestTrue(TEXT("Last pixel belongs to current frame"), Last.Equals(Color, 0.01f));
		return bMapped;
	};

	if (!Capture(FIntPoint(32, 24), PF_B8G8R8A8, FLinearColor::Red)) return false;
	FRHIGPUTextureReadback* OriginalColor = Readback.Get();
	// A completed fence must not make a new frame ready before its own copy.
	{
		FUbstreamAsyncFrameReadback NewFrame;
		NewFrame.ColorReadback = Readback;
		TestFalse(TEXT("Previous frame's signalled fence cannot complete a new frame"), NewFrame.IsReady());
		const auto Concurrent = Pool.AcquireReadback(FIntPoint(32, 24), PF_B8G8R8A8);
		TestTrue(TEXT("A held lease cannot be reused even after GPU completion"), Concurrent.Get() != OriginalColor);
	}
	Readback.Reset();
	// Warm the other format: acquiring color must not evict reusable depth.
	if (!Capture(FIntPoint(32, 24), PF_FloatRGBA, FLinearColor::Green)) return false;
	FRHIGPUTextureReadback* OriginalDepth = Readback.Get();
	Readback.Reset();
	bool bReusedEveryFrame = true;
	for (int32 Frame = 0; Frame < 64; ++Frame)
	{
		const bool bDepth = Frame % 2 != 0;
		if (!Capture(FIntPoint(32, 24), bDepth ? PF_FloatRGBA : PF_B8G8R8A8,
			Frame % 4 < 2 ? FLinearColor::Blue : FLinearColor::Red)) return false;
		bReusedEveryFrame &= Readback.Get() == (bDepth ? OriginalDepth : OriginalColor);
		Readback.Reset();
	}
	TestTrue(TEXT("64 color/depth captures reuse their original readback allocations"), bReusedEveryFrame);
	// A larger copy must get a suitably sized staging texture, not the old one.
	if (!Capture(FIntPoint(63, 47), PF_B8G8R8A8, FLinearColor::Green)) return false;
	Pool.Reset();
	TestTrue(TEXT("Pool reset preserves outstanding frame leases"), Readback->IsReady());
	const auto AfterReset = Pool.AcquireReadback(FIntPoint(63, 47), PF_B8G8R8A8);
	TestTrue(TEXT("Reset starts a new pool without aliasing the old lease"), AfterReset != Readback);
	Readback.Reset();
	// Unique ownership is not proof of readiness (cancelled/unsubmitted work).
	FUbstreamCaptureTargetPool PendingPool;
	auto Pending = PendingPool.AcquireReadback(FIntPoint(32, 24), PF_B8G8R8A8);
	FRHIGPUTextureReadback* PendingIdentity = Pending.Get();
	Pending.Reset();
	TestTrue(TEXT("An unready lease is never recycled"),
		PendingPool.AcquireReadback(FIntPoint(32, 24), PF_B8G8R8A8).Get() != PendingIdentity);
	AddInfo(TEXT("Steady-state test: 64 captures, 2 existing readbacks, 0 additional readback allocations."));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamPreviewReuseTest,
	"Ubstream.Modeler.Performance.PreviewReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FUbstreamPreviewReuseTest::RunTest(const FString& Parameters)
{
	const auto Panel = MakeShared<SUbstreamControlPanel>();
	FImage Pixels(8, 8, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	for (FColor& Pixel : Pixels.AsBGRA8()) Pixel = FColor::Red;
	TArray64<uint8> Encoded;
	if (!TestTrue(TEXT("Test PNG encodes"), FImageUtils::CompressImage(Encoded, TEXT("png"), Pixels))) return false;
	TArray<uint8> Bytes(Encoded);
	FString Error;
	Panel->PendingReturnedFramePayloadType = TEXT("local_passthrough");
	if (!TestTrue(TEXT("Initial preview decodes"), Panel->UpdateReturnedFramePreviewFromBytes(Bytes, Error))) return false;
	const auto SnapshotBrush = Panel->ReturnedFrameBrush;
	const FName SnapshotResource = SnapshotBrush->GetResourceName();
	bool bReusedEveryFrame = true;
	for (int32 Frame = 1; Frame <= 100; ++Frame)
	{
		Panel->PendingReturnedFramePayloadType = TEXT("server");
		Panel->PendingReturnedFrameId = Frame;
		if (!Panel->UpdateReturnedFramePreviewFromBytes(Bytes, Error)) return false;
		bReusedEveryFrame &= Panel->ReturnedFrameBrush == SnapshotBrush;
	}
	TestTrue(TEXT("100 identical frames retain the decoded texture"), bReusedEveryFrame);
	TestTrue(TEXT("Identical server pixels still update preview provenance"), Panel->bCurrentPreviewIsServerFrame);
	TestEqual(TEXT("Identical frames still advance debug metadata"), Panel->LastDebugReceivedFrameId, uint64(100));
	// Change exactly one pixel. A sampled signature must never suppress it.
	Pixels.AsBGRA8()[1] = FColor::Blue;
	FImageUtils::CompressImage(Encoded, TEXT("png"), Pixels);
	Bytes = TArray<uint8>(Encoded);
	TestTrue(TEXT("A one-pixel change decodes"), Panel->UpdateReturnedFramePreviewFromBytes(Bytes, Error));
	TestTrue(TEXT("Changed image receives a new immutable resource"), Panel->ReturnedFrameBrush != SnapshotBrush);
	TestEqual(TEXT("A retained snapshot keeps its original resource"), SnapshotBrush->GetResourceName(), SnapshotResource);
	Panel->bHasPendingReturnedFrame = true;
	Panel->PendingReturnedFrameBytes = Bytes.Num() + 1;
	TestFalse(TEXT("Duplicate cache does not bypass payload validation"), Panel->UpdateReturnedFramePreviewFromBytes(Bytes, Error));
	Panel->ClearCaptureWorkspaceState();
	TestTrue(TEXT("Workspace clear releases cached image bytes"), Panel->CurrentPreviewImageBytes.IsEmpty());
	TestFalse(TEXT("Workspace clear releases the displayed brush"), Panel->ReturnedFrameBrush.IsValid());
	AddInfo(TEXT("Steady-state test: 100 identical previews, 1 retained brush, 0 additional decodes/textures."));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamEncodingReuseTest,
	"Ubstream.Modeler.Performance.EncodingReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamEncodingReuseTest::RunTest(const FString& Parameters)
{
	const auto Cache = MakeShared<FUbstreamCaptureEncodingCache, ESPMode::ThreadSafe>();
	auto MakePixels = [&Cache](int32 Width, int32 Height, int32 Quality, uint64 FrameId)
	{
		FUbstreamCapturePixels Pixels;
		Pixels.bSuccess = true;
		Pixels.FrameId = FrameId;
		Pixels.Size = FIntPoint(Width, Height);
		Pixels.JpegQuality = Quality;
		Pixels.EncodingCache = Cache;
		Pixels.ColorImage = FImage(Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		for (int32 Index = 0; Index < Width * Height; ++Index)
		{
			Pixels.ColorImage.AsBGRA8()[Index] = FColor(Index % 256, (Index * 7) % 256, 100, 255);
			Pixels.DepthPixels.Add(FLinearColor(10.0f + Index * 0.1f, 0, 0, 1));
		}
		return Pixels;
	};
	auto CompareWithUncached = [this](FUbstreamCapturePixels Pixels)
	{
		FUbstreamCapturePixels ReferencePixels = Pixels;
		ReferencePixels.EncodingCache.Reset();
		const auto Cached = FUbstreamCaptureService::EncodeCapturedFrame(MoveTemp(Pixels));
		const auto Reference = FUbstreamCaptureService::EncodeCapturedFrame(MoveTemp(ReferencePixels));
		TestTrue(TEXT("Cached encoding succeeds"), Cached.bSuccess && Reference.bSuccess);
		TestTrue(TEXT("Cached color is byte-identical to a fresh JPEG"), Cached.ColorJpegBytes == Reference.ColorJpegBytes);
		TestTrue(TEXT("Cached depth is byte-identical to a fresh JPEG"), Cached.DepthJpegBytes == Reference.DepthJpegBytes);
		TestEqual(TEXT("Pixel signature is preserved"), Cached.ColorSampleHash, Reference.ColorSampleHash);
		TSharedPtr<FJsonObject> Header;
		if (TestTrue(TEXT("Frame header still generated"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Cached.HeaderJson), Header)))
		{
			TestEqual(TEXT("Cache does not reuse old frame metadata"), uint64(Header->GetNumberField(TEXT("frameId"))), Cached.FrameId);
		}
		TestFalse(TEXT("Frame bundle still generated"), Cached.FrameBundleBytes.IsEmpty());
	};
	for (uint64 Frame = 1; Frame <= 40; ++Frame)
	{
		CompareWithUncached(MakePixels(32, 24, 88, Frame));
	}
	TestEqual(TEXT("40 identical RGB/depth frames compress just once each"), Cache->CompressionCount, uint64(2));
	FUbstreamCapturePixels SmallEdit = MakePixels(32, 24, 88, 41);
	SmallEdit.ColorImage.AsBGRA8()[1] = FColor::White;
	CompareWithUncached(MoveTemp(SmallEdit));
	TestEqual(TEXT("One-pixel edit re-encodes color only"), Cache->CompressionCount, uint64(3));
	CompareWithUncached(MakePixels(32, 24, 70, 42));
	TestEqual(TEXT("Quality changes re-encode both images"), Cache->CompressionCount, uint64(5));
	CompareWithUncached(MakePixels(24, 32, 70, 43));
	TestEqual(TEXT("Same byte count with different dimensions cannot hit the cache"), Cache->CompressionCount, uint64(7));
	FUbstreamCapturePixels DepthOff = MakePixels(24, 32, 70, 44);
	DepthOff.bCaptureDepth = false;
	CompareWithUncached(MoveTemp(DepthOff));
	TestEqual(TEXT("Disabling depth preserves cached color"), Cache->CompressionCount, uint64(7));
	FUbstreamCapturePixels DepthEdit = MakePixels(24, 32, 70, 45);
	DepthEdit.DepthPixels[1].R = 100.0f;
	CompareWithUncached(MoveTemp(DepthEdit));
	TestEqual(TEXT("Changed depth is re-encoded independently"), Cache->CompressionCount, uint64(8));

	FUbstreamCaptureTargetPool Pool;
	const auto OutstandingCache = Pool.GetEncodingCache();
	Pool.Reset();
	TestTrue(TEXT("Reset creates a new cache while old jobs retain theirs"), Pool.GetEncodingCache() != OutstandingCache);
	AddInfo(TEXT("Steady-state test: 40 RGB/depth frames, 2 JPEG compressions instead of 80; output matches uncached encoding."));
	const FUbstreamCapturePixels BenchmarkImage = MakePixels(512, 512, 88, 1);
	auto Benchmark = [&BenchmarkImage](bool bCached, bool bChanging)
	{
		const TSharedPtr<FUbstreamCaptureEncodingCache, ESPMode::ThreadSafe> BenchmarkCache = MakeShared<FUbstreamCaptureEncodingCache, ESPMode::ThreadSafe>();
		const double Start = FPlatformTime::Seconds();
		for (int32 Frame = 0; Frame < 24; ++Frame)
		{
			FUbstreamCapturePixels Pixels = BenchmarkImage;
			Pixels.EncodingCache = bCached ? BenchmarkCache : nullptr;
			if (bChanging)
			{
				Pixels.ColorImage.AsBGRA8()[1] = FColor(Frame, 0, 0, 255);
				Pixels.DepthPixels[1].R += Frame;
			}
			FUbstreamCaptureService::EncodeCapturedFrame(MoveTemp(Pixels));
		}
		return (FPlatformTime::Seconds() - Start) * 1000.0 / 24.0;
	};
	const double UncachedIdle = Benchmark(false, false);
	const double CachedIdle = Benchmark(true, false);
	const double UncachedChanging = Benchmark(false, true);
	const double CachedChanging = Benchmark(true, true);
	AddInfo(FString::Printf(TEXT("512x512 RGB/depth encode benchmark (24 frames, includes pixel copy/header/bundle): idle %.3f -> %.3f ms/frame; changing %.3f -> %.3f ms/frame (uncached -> cached). Timing is diagnostic, not a pass threshold."),
		UncachedIdle, CachedIdle, UncachedChanging, CachedChanging));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamAsyncCaptureReuseTest,
	"Ubstream.Modeler.Performance.AsyncCaptureReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FUbstreamAsyncCaptureReuseTest::RunTest(const FString& Parameters)
{
	FAdvancedPreviewScene Scene{FPreviewScene::ConstructionValues()};
	Scene.SetFloorVisibility(false, true);
	Scene.SetEnvironmentVisibility(false, true);
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Cube available"), Cube)) return false;
	UStaticMeshComponent* Subject = NewObject<UStaticMeshComponent>();
	Subject->SetStaticMesh(Cube);
	Scene.AddComponent(Subject, FTransform::Identity);
	USceneCaptureComponent2D* Color = NewObject<USceneCaptureComponent2D>();
	USceneCaptureComponent2D* Depth = NewObject<USceneCaptureComponent2D>();
	for (USceneCaptureComponent2D* Component : {Color, Depth})
	{
		Component->bCaptureEveryFrame = false;
		Component->bCaptureOnMovement = false;
		Scene.AddComponent(Component, FTransform::Identity);
	}
	FUbstreamCaptureTargetPool Pool;
	FUbstreamCaptureSettings Settings;
	Settings.Width = 64;
	Settings.Height = 48;
	Settings.bSaveDebugImage = false;
	FUbstreamCaptureCamera Camera;
	Camera.Location = FVector(-300, 0, 0);
	Camera.Rotation = FRotator::ZeroRotator;
	Camera.FovDegrees = 60;
	Camera.AspectRatio = 4.0f / 3.0f;
	for (uint64 FrameId = 1; FrameId <= 6; ++FrameId)
	{
		Settings.bCaptureDepth = FrameId != 4;
		if (FrameId >= 5) { Settings.Width = 96; Settings.Height = 72; }
		Camera.Location.Y = FrameId * 5.0;
		auto Frame = FUbstreamAsyncFrameReadback::Begin(Scene, Camera, FrameId, Settings, {}, &Pool, Color, Depth);
		if (!TestTrue(TEXT("Async frame starts"), Frame.IsValid())) return false;
		if (FrameId == 2)
		{
			// Simulate pause/cancel before render commands have necessarily run.
			Frame.Reset();
			continue;
		}
		if (FrameId == 3) Pool.Reset(); // Old leases must survive workspace reset.
		FlushRenderingCommands();
		const double Deadline = FPlatformTime::Seconds() + 10.0;
		while (!Frame->IsReady() && FPlatformTime::Seconds() < Deadline) FPlatformProcess::Sleep(0.001f);
		if (!TestTrue(TEXT("Current async frame becomes ready"), Frame->IsReady())) return false;
		if (FrameId == 6)
		{
			// Use a real completed GPU readback through the production panel poll.
			// Advancing its age simulates an editor hitch without a slow sleep.
			Frame->BeginTimeSeconds = FPlatformTime::Seconds() - 3.0;
			TestTrue(TEXT("Completed readback is older than the deadline"), Frame->HasTimedOut());
			const auto Panel = MakeShared<SUbstreamControlPanel>();
			const auto Ticket = Panel->CaptureState.BeginAsync(600);
			Panel->PendingFrameReadback = Frame;
			Panel->TryCompletePendingFrameReadback();
			TestFalse(TEXT("Ready readback moves out of pending state"), Panel->PendingFrameReadback.IsValid());
			TestTrue(TEXT("Ready readback proceeds to encoding instead of timing out"), Panel->CaptureState.IsBusy());
			TestTrue(TEXT("Ready readback retains the original capture ticket"), Panel->CaptureState.IsCurrent(Ticket));
			// Keep this test network-free and retire the callback before it can send.
			Panel->CancelPendingCapture();
			FlushRenderingCommands();
			continue;
		}
		struct FCompletion { bool bDone = false; FUbstreamEditorWorldCaptureResult Result; };
		const auto Completion = MakeShared<FCompletion>();
		Frame->CompleteAndEncodeAsync([Completion](FUbstreamEditorWorldCaptureResult&& Result)
		{
			Completion->Result = MoveTemp(Result);
			Completion->bDone = true;
		});
		Frame.Reset();
		while (!Completion->bDone && FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.001f);
		}
		if (!TestTrue(TEXT("Async encoding completes"), Completion->bDone)) return false;
		const auto& Result = Completion->Result;
		TestTrue(TEXT("Async result succeeds"), Result.bSuccess);
		TestEqual(TEXT("Async result belongs to the requested frame"), Result.FrameId, FrameId);
		TestEqual(TEXT("Async result follows resize"), Result.Size, FIntPoint(Settings.Width, Settings.Height));
		TestEqual(TEXT("Async depth toggle is preserved"), !Result.DepthJpegBytes.IsEmpty(), Settings.bCaptureDepth);
		FImage Decoded;
		TestTrue(TEXT("Async color JPEG decodes"), FImageUtils::DecompressImage(Result.ColorJpegBytes.GetData(), Result.ColorJpegBytes.Num(), Decoded));
		TestEqual(TEXT("Decoded width follows resize"), Decoded.SizeX, Settings.Width);
		TestEqual(TEXT("Decoded height follows resize"), Decoded.SizeY, Settings.Height);
	}
	Scene.RemoveComponent(Color);
	Scene.RemoveComponent(Depth);
	Scene.RemoveComponent(Subject);
	return true;
}

#endif
