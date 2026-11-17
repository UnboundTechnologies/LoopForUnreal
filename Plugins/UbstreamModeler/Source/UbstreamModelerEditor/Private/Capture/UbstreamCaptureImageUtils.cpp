#include "Capture/UbstreamCaptureImageUtils.h"

#include "Math/Float16Color.h"

namespace
{
	uint64 MixSampleHash(uint64 Hash, uint64 Value)
	{
		Hash ^= Value + 0x9e3779b97f4a7c15ull + (Hash << 6) + (Hash >> 2);
		return Hash;
	}
}

namespace UbstreamCaptureImageUtils
{
	void ConvertDepthRow(const FFloat16Color* Source, FLinearColor* Destination, int32 Width)
	{
		for (int32 Column = 0; Column < Width; ++Column)
		{
			// Same conversion as FLinearColor(FFloat16Color), exposed inline so
			// the compiler can optimize the row without a Core call per pixel.
			Destination[Column] = Source[Column].GetFloats();
		}
	}

	bool CopyCompressedBytes(const TArray64<uint8>& Source, TArray<uint8>& Target)
	{
		if (Source.Num() > MAX_int32)
		{
			return false;
		}

		Target.SetNumUninitialized(static_cast<int32>(Source.Num()));
		if (!Target.IsEmpty())
		{
			FMemory::Memcpy(Target.GetData(), Source.GetData(), Target.Num());
		}
		return true;
	}

	bool TakeImageBytes(TArray64<uint8>& Source, TArray<uint8>& Target)
	{
		// TArray's cross-allocator move checks capacity as well as byte count.
		// Preserve valid payloads even if their old allocation has excess slack.
		if (Source.Max() > MAX_int32)
		{
			if (!CopyCompressedBytes(Source, Target)) return false;
			Source.Empty();
		}
		else
		{
			Target = TArray<uint8>(MoveTemp(Source));
		}
		return true;
	}

	uint64 BuildDeterministicColorSampleHash(FImage& ColorImage)
	{
		ColorImage.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);

		FImageView ColorImageView = ColorImage;
		const TArrayView64<FColor> Pixels = ColorImageView.AsBGRA8();
		const int64 PixelCount = Pixels.Num();
		if (PixelCount <= 0)
		{
			return 0;
		}

		uint64 Hash = MixSampleHash(0xcbf29ce484222325ull, static_cast<uint64>(ColorImage.SizeX));
		Hash = MixSampleHash(Hash, static_cast<uint64>(ColorImage.SizeY));

		uint32 State = 2166136261u
			^ static_cast<uint32>(ColorImage.SizeX * 73856093)
			^ static_cast<uint32>(ColorImage.SizeY * 19349663);
		const int32 SampleCount = FMath::Min<int32>(128, static_cast<int32>(PixelCount));
		for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
		{
			State = State * 1664525u + 1013904223u;
			const int64 PixelIndex = static_cast<int64>(State % static_cast<uint32>(PixelCount));
			const FColor Pixel = Pixels[PixelIndex];
			const uint64 PackedColor =
				static_cast<uint64>(Pixel.B)
				| (static_cast<uint64>(Pixel.G) << 8)
				| (static_cast<uint64>(Pixel.R) << 16)
				| (static_cast<uint64>(Pixel.A) << 24);
			Hash = MixSampleHash(Hash, PackedColor | (static_cast<uint64>(PixelIndex) << 32));
		}

		return Hash;
	}

	bool IsNearlyBlackImage(FImage& ColorImage)
	{
		ColorImage.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);

		FImageView ColorImageView = ColorImage;
		const TArrayView64<FColor> Pixels = ColorImageView.AsBGRA8();
		const int64 PixelCount = Pixels.Num();
		if (PixelCount <= 0)
		{
			return true;
		}

		const int64 Step = FMath::Max<int64>(1, PixelCount / 16384);
		const int64 TotalSampleCount = 1 + (PixelCount - 1) / Step;
		// A few specular sparkles must not suppress the black-frame fallback.
		const int64 BrightSampleLimit = FMath::Max<int64>(8, TotalSampleCount / 512);
		int64 BrightSampleCount = 0;
		uint64 LumaSum = 0;
		int64 SampleCount = 0;
		for (int64 PixelIndex = 0; PixelIndex < PixelCount; PixelIndex += Step)
		{
			const FColor Pixel = Pixels[PixelIndex];
			if (FMath::Max(Pixel.R, FMath::Max(Pixel.G, Pixel.B)) >= 18)
			{
				// Once the final bright-sample threshold is exceeded, later
				// samples cannot make the image nearly black again.
				if (++BrightSampleCount > BrightSampleLimit)
				{
					return false;
				}
			}
			LumaSum += static_cast<uint64>(Pixel.R) * 3 + static_cast<uint64>(Pixel.G) * 6 + static_cast<uint64>(Pixel.B);
			++SampleCount;
		}

		const double MeanLuma = SampleCount > 0
			? static_cast<double>(LumaSum) / static_cast<double>(SampleCount * 10)
			: 0.0;
		return MeanLuma < 4.0;
	}
}
