#include "Capture/UbstreamCaptureImageUtils.h"

#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Previous full-scan algorithm retained as the equivalence/timing reference.
	FORCENOINLINE bool ReferenceNearlyBlack(FImage& Image)
	{
		Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const auto Pixels = Image.AsBGRA8();
		const int64 PixelCount = Pixels.Num();
		if (PixelCount <= 0) return true;
		const int64 Step = FMath::Max<int64>(1, PixelCount / 16384);
		int64 BrightCount = 0, SampleCount = 0;
		uint64 LumaSum = 0;
		for (int64 Index = 0; Index < PixelCount; Index += Step)
		{
			const FColor Pixel = Pixels[Index];
			if (FMath::Max(Pixel.R, FMath::Max(Pixel.G, Pixel.B)) >= 18) ++BrightCount;
			LumaSum += uint64(Pixel.R) * 3 + uint64(Pixel.G) * 6 + Pixel.B;
			++SampleCount;
		}
		if (BrightCount > FMath::Max<int64>(8, SampleCount / 512)) return false;
		return static_cast<double>(LumaSum) / static_cast<double>(SampleCount * 10) < 4.0;
	}

	void FillImage(FImage& Image, FColor Color)
	{
		for (FColor& Pixel : Image.AsBGRA8()) Pixel = Color;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamBlackFrameTest,
	"Ubstream.Modeler.Performance.BlackFrameClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamBlackFrameTest::RunTest(const FString& Parameters)
{
	int32 Cases = 0;
	auto Check = [&](FImage Image)
	{
		FImage ReferenceImage = Image;
		const bool Expected = ReferenceNearlyBlack(ReferenceImage);
		TestEqual(TEXT("Fallback classification matches the full scan"), UbstreamCaptureImageUtils::IsNearlyBlackImage(Image), Expected);
		TestTrue(TEXT("Image normalization remains identical"), Image.RawData == ReferenceImage.RawData);
		++Cases;
	};
	Check(FImage(0, 0, ERawImageFormat::BGRA8, EGammaSpace::sRGB));
	for (const FIntPoint Size : {FIntPoint(1, 1), FIntPoint(9, 1), FIntPoint(64, 1),
		FIntPoint(16384, 1), FIntPoint(32767, 1), FIntPoint(33791, 1), FIntPoint(513, 511), FIntPoint(512, 512)})
	{
		FImage Image(Size.X, Size.Y, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		for (const uint8 Gray : {0, 3, 4, 17, 18, 255})
		{
			FillImage(Image, FColor(Gray, Gray, Gray, 255));
			Check(Image);
		}
		const int64 PixelCount = Image.AsBGRA8().Num();
		const int64 Step = FMath::Max<int64>(1, PixelCount / 16384);
		const int64 Samples = 1 + (PixelCount - 1) / Step;
		const int64 Limit = FMath::Max<int64>(8, Samples / 512);
		for (const int32 Delta : {-1, 0, 1})
		{
			for (const bool bAtEnd : {false, true})
			{
				FillImage(Image, FColor::Black);
				const int64 BrightCount = FMath::Min(Samples, Limit + Delta);
				for (int64 Index = 0; Index < BrightCount; ++Index)
				{
					const int64 Sample = bAtEnd ? Samples - Index - 1 : Index;
					Image.AsBGRA8()[Sample * Step] = FColor(18, 0, 0, 255);
				}
				Check(Image);
			}
		}
		// A bright unsampled pixel must not change the established sampling policy.
		if (Step > 1)
		{
			FillImage(Image, FColor::Black);
			Image.AsBGRA8()[1] = FColor::White;
			Check(Image);
		}
	}
	for (const float Gray : {0.0f, 0.001f, 0.01f, 1.0f})
	{
		FImage Linear(13, 7, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		for (FLinearColor& Pixel : Linear.AsRGBA32F()) Pixel = FLinearColor(Gray, Gray, Gray, 1);
		Check(MoveTemp(Linear));
	}
	AddInfo(FString::Printf(TEXT("Black-frame equivalence: %d cases including thresholds, late bright samples, non-divisible strides, unsampled pixels, empty images and linear-to-sRGB normalization."), Cases));

	if (FParse::Param(FCommandLine::Get(), TEXT("UbstreamBlackFrameProbe")))
	{
		// CPU-only, at most 8 pairs per case, with 50 ms rests. Each case checks
		// a 0.1-second timed-work budget between pairs; no renderer or GPU work.
		for (const bool bBright : {true, false})
		{
			FImage Image(512, 512, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			FillImage(Image, bBright ? FColor(80, 90, 100, 255) : FColor::Black);
			ReferenceNearlyBlack(Image);
			UbstreamCaptureImageUtils::IsNearlyBlackImage(Image);
			double Totals[2] = {0, 0};
			int32 ObservedBlack[2] = {0, 0};
			constexpr int32 Batch = 16;
			int32 Pairs = 0;
			for (; Pairs < 8 && Totals[0] + Totals[1] < 0.1; ++Pairs)
			{
				for (int32 Run = 0; Run < 2; ++Run)
				{
					const int32 Variant = (Pairs + Run) % 2;
					const double Start = FPlatformTime::Seconds();
					for (int32 Frame = 0; Frame < Batch; ++Frame)
					{
						ObservedBlack[Variant] += Variant == 0 ? ReferenceNearlyBlack(Image) : UbstreamCaptureImageUtils::IsNearlyBlackImage(Image);
					}
					Totals[Variant] += FPlatformTime::Seconds() - Start;
				}
				FPlatformProcess::Sleep(0.05f);
			}
			TestEqual(TEXT("Timed checks preserve classification"), ObservedBlack[0], ObservedBlack[1]);
			AddInfo(FString::Printf(TEXT("Black-frame probe (%s): %d pairs of %d checks; full scan %.4f ms/frame, early exit %.4f ms/frame; %.2f%% classification-time reduction. Not overall plugin performance."),
				bBright ? TEXT("bright") : TEXT("black"), Pairs, Batch, Totals[0] * 1000 / (Pairs * Batch),
				Totals[1] * 1000 / (Pairs * Batch), 100 * (Totals[0] - Totals[1]) / Totals[0]));
		}
	}
	return true;
}

#endif
