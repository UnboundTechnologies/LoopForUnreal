#include "Capture/UbstreamCaptureImageUtils.h"

#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Textures/SlateTextureData.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamPreviewBufferTest,
	"Ubstream.Modeler.Performance.PreviewBufferTransfer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamPreviewBufferTest::RunTest(const FString& Parameters)
{
	for (const int32 Count : {0, 1, 257, 4096})
	{
		TArray64<uint8> Source;
		Source.Reserve(Count + 37);
		for (int32 Index = 0; Index < Count; ++Index) Source.Add(static_cast<uint8>(Index * 17));
		const TArray<uint8> Expected(Source);
		const uint8* Allocation = Source.GetData();
		TArray<uint8> Transferred;
		Transferred.Init(0xee, 64);
		TestTrue(TEXT("Transfer succeeds with empty, odd-sized and slack buffers"), UbstreamCaptureImageUtils::TakeImageBytes(Source, Transferred));
		TestTrue(TEXT("Ownership transfer reuses the allocation rather than copying it"), Transferred.GetData() == Allocation);
		TestTrue(TEXT("Transferred bytes replace the previous destination exactly"), Transferred == Expected);
		TestTrue(TEXT("The old owner releases its allocation"), Source.IsEmpty() && Source.GetData() == nullptr);
		Source.Add(0xff);
		TestTrue(TEXT("Reusing the moved-from source does not alter the payload"), Transferred == Expected);
	}

	FImage Fixture(13, 7, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	for (int32 Index = 0; Index < Fixture.AsBGRA8().Num(); ++Index)
	{
		Fixture.AsBGRA8()[Index] = FColor(Index * 7, Index * 11, Index * 13, 255);
	}
	for (const TCHAR* Format : {TEXT("png"), TEXT("jpg")})
	{
		TArray64<uint8> Compressed;
		if (!TestTrue(TEXT("Fixture encodes"), FImageUtils::CompressImage(Compressed, Format, Fixture, 88))) return false;
		FImage Decoded;
		if (!TestTrue(TEXT("Fixture decodes"), FImageUtils::DecompressImage(Compressed.GetData(), Compressed.Num(), Decoded))) return false;
		Decoded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const TArray<uint8> Expected(Decoded.RawData);
		TArray<uint8> Payload;
		TestTrue(TEXT("Decoded preview pixels transfer"), UbstreamCaptureImageUtils::TakeImageBytes(Decoded.RawData, Payload));
		// This is the CPU storage constructor used by Slate's actual resource
		// creation path. No renderer or GPU is required to verify its ownership.
		FSlateTextureData Storage(Decoded.SizeX, Decoded.SizeY, 4, Payload);
		Decoded.Reset();
		Payload.Empty();
		TestTrue(TEXT("Slate retains identical pixels after both temporary owners are destroyed"), Storage.GetRawBytes() == Expected);
		TestEqual(TEXT("Preview width is retained"), Storage.GetWidth(), uint32(13));
		TestEqual(TEXT("Preview height is retained"), Storage.GetHeight(), uint32(7));
		TestEqual(TEXT("Preview pixel format is retained"), Storage.GetBytesPerPixel(), uint32(4));
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("UbstreamPreviewBufferProbe")))
	{
		FImage Frame(512, 512, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		for (FColor& Pixel : Frame.AsBGRA8()) Pixel = FColor(80, 90, 100, 255);
		const TArray<uint8> Expected(Frame.RawData);
		auto PrepareStorage = [](FImage& Image, bool bTransfer)
		{
			TArray<uint8> Payload;
			if (bTransfer) UbstreamCaptureImageUtils::TakeImageBytes(Image.RawData, Payload);
			else Payload = TArray<uint8>(Image.RawData);
			return MakeUnique<FSlateTextureData>(Image.SizeX, Image.SizeY, 4, Payload);
		};
		for (const bool bTransfer : {false, true})
		{
			FImage Warmup = Frame;
			PrepareStorage(Warmup, bTransfer);
		}
		double Totals[2] = {0, 0};
		int32 Pairs = 0;
		// CPU buffer handoff only; no decoding or GPU upload in the timings.
		// At most 8 pairs, a 0.1-second budget checked between pairs, and rests.
		for (; Pairs < 8 && Totals[0] + Totals[1] < 0.1; ++Pairs)
		{
			for (int32 Run = 0; Run < 2; ++Run)
			{
				FImage Input = Frame; // Fixture preparation is outside the timer.
				const int32 Variant = (Pairs + Run) % 2;
				const double Start = FPlatformTime::Seconds();
				const auto Storage = PrepareStorage(Input, Variant == 1);
				Totals[Variant] += FPlatformTime::Seconds() - Start;
				TestTrue(TEXT("Timed handoff preserves every byte"), Storage->GetRawBytes() == Expected);
			}
			FPlatformProcess::Sleep(0.05f);
		}
		AddInfo(FString::Printf(TEXT("Preview buffer probe: %d paired 512x512 handoffs including Slate CPU storage; copy %.4f ms/frame, transfer %.4f ms/frame; %.2f%% handoff-time reduction. One 1048576-byte allocation/copy avoided per changed preview. Not overall plugin performance."),
			Pairs, Totals[0] * 1000 / Pairs, Totals[1] * 1000 / Pairs, 100 * (Totals[0] - Totals[1]) / Totals[0]));
	}
	return true;
}

#endif
