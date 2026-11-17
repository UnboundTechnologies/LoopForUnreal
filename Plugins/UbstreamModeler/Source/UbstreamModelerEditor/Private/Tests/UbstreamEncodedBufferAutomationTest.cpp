#include "Capture/UbstreamCaptureService.h"

#include "Capture/UbstreamCaptureEncodingCache.h"
#include "Capture/UbstreamCaptureImageUtils.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Protocol/UbstreamFrameEnvelope.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamEncodedBufferTest,
	"Ubstream.Modeler.Performance.EncodedBufferTransfer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamEncodedBufferTest::RunTest(const FString& Parameters)
{
	FImage Color(32, 24, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FImage GrayDepth(32, 24, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	for (int32 Index = 0; Index < Color.AsBGRA8().Num(); ++Index)
	{
		Color.AsBGRA8()[Index] = FColor(Index * 7, Index * 13, Index * 17, 255);
		// Constant visible depth normalizes to white; empty background to black.
		GrayDepth.AsBGRA8()[Index] = Index % 2 ? FColor::White : FColor::Black;
	}
	TArray64<uint8> ColorJpeg, DepthJpeg;
	if (!TestTrue(TEXT("Reference color JPEG encodes"), FImageUtils::CompressImage(ColorJpeg, TEXT("jpg"), Color, 88))) return false;
	if (!TestTrue(TEXT("Reference depth JPEG encodes"), FImageUtils::CompressImage(DepthJpeg, TEXT("jpg"), GrayDepth, 88))) return false;
	const TArray<uint8> ExpectedColor(ColorJpeg), ExpectedDepth(DepthJpeg);
	for (const bool bCache : {false, true})
	{
		for (const bool bDepth : {false, true})
		{
			FUbstreamCapturePixels Pixels;
			Pixels.bSuccess = true;
			Pixels.Size = FIntPoint(32, 24);
			Pixels.ColorImage = Color;
			Pixels.bCaptureDepth = bDepth;
			if (bCache) Pixels.EncodingCache = MakeShared<FUbstreamCaptureEncodingCache, ESPMode::ThreadSafe>();
			if (bDepth)
			{
				for (int32 Index = 0; Index < 32 * 24; ++Index)
				{
					Pixels.DepthPixels.Add(FLinearColor(Index % 2 ? 10.0f : 0.0f, 0, 0, 1));
				}
			}
			for (uint64 Frame = 1; Frame <= 3; ++Frame)
			{
				Pixels.FrameId = Frame;
				auto Result = FUbstreamCaptureService::EncodeCapturedFrame(Pixels);
				if (!TestTrue(TEXT("Cold and repeated frame encoding succeed"), Result.bSuccess)) return false;
				TestTrue(TEXT("Transferred color matches independently encoded JPEG"), Result.ColorJpegBytes == ExpectedColor);
				TestTrue(TEXT("Transferred depth matches independently encoded JPEG"), bDepth ? Result.DepthJpegBytes == ExpectedDepth : Result.DepthJpegBytes.IsEmpty());
				TArray<uint8> ExpectedBundle;
				TestTrue(TEXT("Reference envelope builds"), FUbstreamFrameEnvelope::BuildFrameBundle(Frame, Result.HeaderJson,
					ExpectedColor, bDepth ? ExpectedDepth : TArray<uint8>(), ExpectedBundle));
				TestTrue(TEXT("Wire envelope preserves the complete payload"), Result.FrameBundleBytes == ExpectedBundle);
				// Results own their buffers: editing or releasing one must not steal
				// or corrupt the cache's bytes for the next frame.
				Result.ColorJpegBytes[0] ^= 0xff;
				if (bDepth) Result.DepthJpegBytes[0] ^= 0xff;
			}
		}
	}
	FUbstreamCapturePixels Failed;
	Failed.ErrorMessage = TEXT("capture failed");
	const auto Failure = FUbstreamCaptureService::EncodeCapturedFrame(MoveTemp(Failed));
	TestFalse(TEXT("Capture failures remain failures"), Failure.bSuccess);
	TestEqual(TEXT("Capture failure text is preserved"), Failure.ErrorMessage, FString(TEXT("capture failed")));

	if (FParse::Param(FCommandLine::Get(), TEXT("UbstreamEncodedBufferProbe")))
	{
		FImage Frame(512, 512, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		for (int32 Index = 0; Index < Frame.AsBGRA8().Num(); ++Index)
		{
			Frame.AsBGRA8()[Index] = FColor(Index % 256, (Index * 7) % 256, 100, 255);
		}
		if (!TestTrue(TEXT("Probe JPEG encodes"), FImageUtils::CompressImage(ColorJpeg, TEXT("jpg"), Frame, 88))) return false;
		for (int32 Index = 0; Index < Frame.AsBGRA8().Num(); ++Index)
		{
			const uint8 Gray = static_cast<uint8>(Index / 512 / 2);
			Frame.AsBGRA8()[Index] = FColor(Gray, Gray, Gray, 255);
		}
		if (!TestTrue(TEXT("Probe depth JPEG encodes"), FImageUtils::CompressImage(DepthJpeg, TEXT("jpg"), Frame, 88))) return false;
		const TArray<uint8> ReferenceColor(ColorJpeg), ReferenceDepth(DepthJpeg);
		auto Export = [](TArray64<uint8>& ColorBytes, TArray64<uint8>& DepthBytes, TArray<uint8>& OutColor, TArray<uint8>& OutDepth, bool bTransfer)
		{
			return bTransfer
				? UbstreamCaptureImageUtils::TakeImageBytes(ColorBytes, OutColor) && UbstreamCaptureImageUtils::TakeImageBytes(DepthBytes, OutDepth)
				: UbstreamCaptureImageUtils::CopyCompressedBytes(ColorBytes, OutColor) && UbstreamCaptureImageUtils::CopyCompressedBytes(DepthBytes, OutDepth);
		};
		for (const bool bTransfer : {false, true})
		{
			TArray64<uint8> InputColor = ColorJpeg, InputDepth = DepthJpeg;
			TArray<uint8> OutColor, OutDepth;
			Export(InputColor, InputDepth, OutColor, OutDepth, bTransfer);
		}
		double Totals[2] = {0, 0};
		int32 Pairs = 0;
		constexpr int32 Batch = 16;
		// CPU export only; prepare inputs outside timing, alternate order, pause
		// between pairs, and stop after 8 pairs or 0.1 seconds of timed work.
		for (; Pairs < 8 && Totals[0] + Totals[1] < 0.1; ++Pairs)
		{
			for (int32 Run = 0; Run < 2; ++Run)
			{
				const int32 Variant = (Pairs + Run) % 2;
				for (int32 Index = 0; Index < Batch; ++Index)
				{
					TArray64<uint8> InputColor = ColorJpeg, InputDepth = DepthJpeg;
					TArray<uint8> OutColor, OutDepth;
					const double Start = FPlatformTime::Seconds();
					const bool bSuccess = Export(InputColor, InputDepth, OutColor, OutDepth, Variant == 1);
					Totals[Variant] += FPlatformTime::Seconds() - Start;
					TestTrue(TEXT("Timed export preserves JPEG bytes"), bSuccess && OutColor == ReferenceColor && OutDepth == ReferenceDepth);
				}
			}
			FPlatformProcess::Sleep(0.05f);
		}
		AddInfo(FString::Printf(TEXT("Encoded buffer probe: %d pairs of %d RGB/depth JPEG exports; copy %.4f ms/frame, transfer %.4f ms/frame; %.2f%% export-time reduction; %d copied bytes and 2 allocations avoided/frame. Encoding, rendering and networking are excluded."),
			Pairs, Batch, Totals[0] * 1000 / (Pairs * Batch), Totals[1] * 1000 / (Pairs * Batch),
			100 * (Totals[0] - Totals[1]) / Totals[0], ReferenceColor.Num() + ReferenceDepth.Num()));
	}
	return true;
}

#endif
