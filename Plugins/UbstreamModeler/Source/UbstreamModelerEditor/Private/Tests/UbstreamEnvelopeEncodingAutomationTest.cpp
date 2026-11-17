#include "Protocol/UbstreamFrameEnvelope.h"

#include "Capture/UbstreamCaptureFrameUtils.h"
#include "Capture/UbstreamEditorWorldCaptureService.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Protocol/UbstreamProtocolTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Previous wire behavior, including converting the header twice. Explicit
	// field serialization keeps the reference independent of the current builder.
	FORCENOINLINE bool ReferenceBundle(uint64 FrameId, const FString& Header,
		const TArray<uint8>& Color, const TArray<uint8>& Depth, TArray<uint8>& Out)
	{
		FTCHARToUTF8 ForLength(*Header);
		const int64 Size = int64(UbstreamProtocol::FrameBundleHeaderSize) + ForLength.Length() + Color.Num() + Depth.Num();
		if (Size > MAX_int32) return false;
		Out.Reset(static_cast<int32>(Size));
		Out.Append({uint8('U'), uint8('B'), uint8('S'), uint8('1')});
		auto WriteLittleEndian = [&Out](uint64 Value, int32 Bytes)
		{
			for (int32 Shift = 0; Shift < Bytes * 8; Shift += 8) Out.Add(static_cast<uint8>(Value >> Shift));
		};
		WriteLittleEndian(UbstreamProtocol::Version, 2);
		WriteLittleEndian(uint16(UbstreamProtocol::EBinaryMessageType::FrameBundle), 2);
		WriteLittleEndian(FrameId, 8);
		WriteLittleEndian(0, 4);
		WriteLittleEndian(ForLength.Length(), 4);
		WriteLittleEndian(Color.Num(), 4);
		WriteLittleEndian(Depth.Num(), 4);
		WriteLittleEndian(0, 4);
		FTCHARToUTF8 ForBytes(*Header);
		Out.Append(reinterpret_cast<const uint8*>(ForBytes.Get()), ForBytes.Length());
		Out.Append(Color);
		Out.Append(Depth);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamEnvelopeEncodingTest,
	"Ubstream.Modeler.Performance.EnvelopeHeaderEncoding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamEnvelopeEncodingTest::RunTest(const FString& Parameters)
{
	FUbstreamEditorWorldCaptureResult Frame;
	Frame.FrameId = 42;
	Frame.SessionId = TEXT("editor_modeling_session");
	Frame.Size = FIntPoint(512, 512);
	Frame.ColorJpegBytes.SetNumZeroed(48000);
	Frame.DepthJpegBytes.SetNumZeroed(4000);
	const FString TypicalHeader = UbstreamCaptureFrameUtils::BuildFrameHeaderJson(Frame);
	const TArray<FString> Headers = {
		FString(), TEXT("{}"), TEXT("{\"name\":\"café Ω 東京 😀\"}"),
		FString::ChrN(2048, TCHAR('x')), TypicalHeader
	};
	const uint64 FrameIds[] = {0, 0x0102030405060708ULL, MAX_uint64};
	const TArray<uint8> Color = {0, 255, 128, 7, 0};
	const TArray<uint8> Depth = {255, 0, 9};
	const TArray<uint8> Empty;
	TArray<uint8> Expected, Actual;
	Actual.Reserve(4096);
	int32 Cases = 0;
	for (const FString& Header : Headers)
	{
		for (const uint64 FrameId : FrameIds)
		{
			for (const bool bColor : {false, true})
			{
				for (const bool bDepth : {false, true})
				{
					Actual.Add(0xcc); // Prior contents/capacity must not leak into the next packet.
					if (!TestTrue(TEXT("Reference packet builds"), ReferenceBundle(FrameId, Header, bColor ? Color : Empty, bDepth ? Depth : Empty, Expected))) return false;
					if (!TestTrue(TEXT("Optimized packet builds"), FUbstreamFrameEnvelope::BuildFrameBundle(FrameId, Header, bColor ? Color : Empty, bDepth ? Depth : Empty, Actual))) return false;
					TestTrue(TEXT("Entire packet, byte lengths and payload remain identical"), Actual == Expected);
					++Cases;
				}
			}
		}
	}
	AddInfo(FString::Printf(TEXT("Envelope equivalence: %d complete packet cases covering ASCII, Unicode, empty/long headers, frame-ID boundaries, optional payloads and reused output buffers."), Cases));

	if (FParse::Param(FCommandLine::Get(), TEXT("UbstreamEnvelopeEncodingProbe")))
	{
		// Warm both code paths. Each measured packet owns a fresh buffer, as in
		// capture; header generation is excluded. No rendering or network calls.
		ReferenceBundle(Frame.FrameId, TypicalHeader, Frame.ColorJpegBytes, Frame.DepthJpegBytes, Expected);
		FUbstreamFrameEnvelope::BuildFrameBundle(Frame.FrameId, TypicalHeader, Frame.ColorJpegBytes, Frame.DepthJpegBytes, Actual);
		double Totals[2] = {0, 0};
		constexpr int32 Batch = 16;
		int32 Pairs = 0;
		for (; Pairs < 8 && Totals[0] + Totals[1] < 0.1; ++Pairs)
		{
			for (int32 Run = 0; Run < 2; ++Run)
			{
				const int32 Variant = (Pairs + Run) % 2;
				for (int32 Index = 0; Index < Batch; ++Index)
				{
					TArray<uint8> Packet;
					const double Start = FPlatformTime::Seconds();
					const bool bSuccess = Variant == 0
						? ReferenceBundle(Frame.FrameId, TypicalHeader, Frame.ColorJpegBytes, Frame.DepthJpegBytes, Packet)
						: FUbstreamFrameEnvelope::BuildFrameBundle(Frame.FrameId, TypicalHeader, Frame.ColorJpegBytes, Frame.DepthJpegBytes, Packet);
					Totals[Variant] += FPlatformTime::Seconds() - Start;
					TestTrue(TEXT("Timed packet preserves every byte"), bSuccess && Packet == Expected);
				}
			}
			FPlatformProcess::Sleep(0.05f);
		}
		FTCHARToUTF8 HeaderBytes(*TypicalHeader);
		AddInfo(FString::Printf(TEXT("Envelope encoding probe: %d pairs of %d packets; previous %.4f ms/frame, reuse %.4f ms/frame; %.2f%% packet-build reduction. One repeated %d-byte UTF-8 conversion avoided; not overall plugin performance."),
			Pairs, Batch, Totals[0] * 1000 / (Pairs * Batch), Totals[1] * 1000 / (Pairs * Batch),
			100 * (Totals[0] - Totals[1]) / Totals[0], HeaderBytes.Length()));
	}
	return true;
}

#endif
