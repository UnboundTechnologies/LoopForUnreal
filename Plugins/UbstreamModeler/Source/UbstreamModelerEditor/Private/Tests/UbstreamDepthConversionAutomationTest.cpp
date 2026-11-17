#include "Capture/UbstreamCaptureImageUtils.h"

#include "HAL/PlatformProcess.h"
#include "Math/Float16Color.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamDepthConversionTest,
	"Ubstream.Modeler.Performance.DepthConversion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamDepthConversionTest::RunTest(const FString& Parameters)
{
	constexpr int32 PatternCount = 65536;
	TArray<FFloat16Color> Source;
	TArray<FLinearColor> Reference, Converted;
	Source.SetNumUninitialized(PatternCount);
	Reference.SetNumUninitialized(PatternCount);
	Converted.SetNumUninitialized(PatternCount);
	for (int32 Index = 0; Index < PatternCount; ++Index)
	{
		// Each channel visits every bit pattern, in different orders. Comparing
		// bytes also verifies signed zero and NaN payloads (float == cannot).
		Source[Index].R.Encoded = static_cast<uint16>(Index);
		Source[Index].G.Encoded = static_cast<uint16>(65535 - Index);
		Source[Index].B.Encoded = static_cast<uint16>(Index * 17);
		Source[Index].A.Encoded = static_cast<uint16>(Index * 257);
		Reference[Index] = FLinearColor(Source[Index]);
	}
	UbstreamCaptureImageUtils::ConvertDepthRow(Source.GetData(), Converted.GetData(), PatternCount);
	TestTrue(TEXT("All half-float patterns in all four channels match the previous conversion bit-for-bit"),
		FMemory::Memcmp(Reference.GetData(), Converted.GetData(), PatternCount * sizeof(FLinearColor)) == 0);

	constexpr int32 Width = 7, Height = 3, SourcePitch = 11, DestinationPitch = 9;
	Reference.Init(FLinearColor(2, 3, 4, 5), Height * DestinationPitch);
	Converted = Reference;
	for (int32 Row = 0; Row < Height; ++Row)
	{
		for (int32 Column = 0; Column < Width; ++Column)
		{
			Reference[Row * DestinationPitch + Column] = FLinearColor(Source[Row * SourcePitch + Column]);
		}
		UbstreamCaptureImageUtils::ConvertDepthRow(Source.GetData() + Row * SourcePitch,
			Converted.GetData() + Row * DestinationPitch, Width);
	}
	TestTrue(TEXT("Odd-width rows preserve pixel order and leave row padding untouched"),
		FMemory::Memcmp(Reference.GetData(), Converted.GetData(), Reference.Num() * sizeof(FLinearColor)) == 0);
	UbstreamCaptureImageUtils::ConvertDepthRow(nullptr, nullptr, 0);

	// Explicitly opt in to timing. No renderer/GPU work; alternate ordering and
	// pause between pairs. Stop after 16 pairs or 0.25 seconds of timed work.
	if (FParse::Param(FCommandLine::Get(), TEXT("UbstreamDepthConversionProbe")))
	{
		constexpr int32 FrameSize = 512;
		Source.SetNumUninitialized(FrameSize * FrameSize);
		Reference.SetNumUninitialized(Source.Num());
		Converted.SetNumUninitialized(Source.Num());
		for (int32 Index = 0; Index < Source.Num(); ++Index)
		{
			Source[Index] = FFloat16Color(FLinearColor(1.0f + Index % 60000, 0, 0, 1));
		}
		auto ConvertFrame = [&](bool bInline)
		{
			for (int32 Row = 0; Row < FrameSize; ++Row)
			{
				const FFloat16Color* SourceRow = Source.GetData() + Row * FrameSize;
				if (bInline)
				{
					UbstreamCaptureImageUtils::ConvertDepthRow(SourceRow, Converted.GetData() + Row * FrameSize, FrameSize);
				}
				else
				{
					FLinearColor* DestinationRow = Reference.GetData() + Row * FrameSize;
					for (int32 Column = 0; Column < FrameSize; ++Column)
					{
						DestinationRow[Column] = FLinearColor(SourceRow[Column]);
					}
				}
			}
		};
		ConvertFrame(false);
		ConvertFrame(true);
		double Totals[2] = {0.0, 0.0};
		int32 Pairs = 0;
		for (; Pairs < 16 && Totals[0] + Totals[1] < 0.25; ++Pairs)
		{
			for (int32 Run = 0; Run < 2; ++Run)
			{
				const int32 Variant = (Pairs + Run) % 2;
				const double Start = FPlatformTime::Seconds();
				ConvertFrame(Variant == 1);
				Totals[Variant] += FPlatformTime::Seconds() - Start;
			}
			TestTrue(TEXT("Timed conversion preserves every output byte"),
				FMemory::Memcmp(Reference.GetData(), Converted.GetData(), Reference.Num() * sizeof(FLinearColor)) == 0);
			FPlatformProcess::Sleep(0.05f);
		}
		AddInfo(FString::Printf(TEXT("Depth conversion probe: %d paired 512x512 frames; previous %.4f ms/frame, inline %.4f ms/frame; %.2f%% conversion-time reduction. CPU conversion only, not overall plugin performance."),
			Pairs, Totals[0] * 1000.0 / Pairs, Totals[1] * 1000.0 / Pairs,
			100.0 * (Totals[0] - Totals[1]) / Totals[0]));
	}
	return true;
}

#endif
