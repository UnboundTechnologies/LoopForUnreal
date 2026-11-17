#include "Protocol/UbstreamFrameEnvelope.h"

#include "Protocol/UbstreamProtocolTypes.h"

bool FUbstreamFrameEnvelope::BuildFrameBundle(
	uint64 FrameId,
	const FString& HeaderJson,
	const TArray<uint8>& ColorBytes,
	const TArray<uint8>& DepthBytes,
	TArray<uint8>& OutBytes)
{
	FTCHARToUTF8 HeaderUtf8(*HeaderJson);
	const int32 HeaderBytes = HeaderUtf8.Length();

	if (HeaderBytes < 0)
	{
		return false;
	}

	const int64 BundleSize =
		static_cast<int64>(UbstreamProtocol::FrameBundleHeaderSize)
		+ static_cast<int64>(HeaderBytes)
		+ static_cast<int64>(ColorBytes.Num())
		+ static_cast<int64>(DepthBytes.Num());
	if (BundleSize > MAX_int32)
	{
		return false;
	}

	OutBytes.Reset(static_cast<int32>(BundleSize));

	OutBytes.Add(static_cast<uint8>('U'));
	OutBytes.Add(static_cast<uint8>('B'));
	OutBytes.Add(static_cast<uint8>('S'));
	OutBytes.Add(static_cast<uint8>('1'));
	WriteUInt16LE(OutBytes, UbstreamProtocol::Version);
	WriteUInt16LE(OutBytes, static_cast<uint16>(UbstreamProtocol::EBinaryMessageType::FrameBundle));
	WriteUInt64LE(OutBytes, FrameId);
	WriteUInt32LE(OutBytes, 0);
	WriteUInt32LE(OutBytes, static_cast<uint32>(HeaderBytes));
	WriteUInt32LE(OutBytes, static_cast<uint32>(ColorBytes.Num()));
	WriteUInt32LE(OutBytes, static_cast<uint32>(DepthBytes.Num()));
	WriteUInt32LE(OutBytes, 0);

	OutBytes.Append(reinterpret_cast<const uint8*>(HeaderUtf8.Get()), HeaderBytes);
	OutBytes.Append(ColorBytes);
	OutBytes.Append(DepthBytes);

	return true;
}

void FUbstreamFrameEnvelope::WriteUInt16LE(TArray<uint8>& OutBytes, uint16 Value)
{
	OutBytes.Add(static_cast<uint8>(Value & 0xff));
	OutBytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
}

void FUbstreamFrameEnvelope::WriteUInt32LE(TArray<uint8>& OutBytes, uint32 Value)
{
	OutBytes.Add(static_cast<uint8>(Value & 0xff));
	OutBytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
	OutBytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
	OutBytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
}

void FUbstreamFrameEnvelope::WriteUInt64LE(TArray<uint8>& OutBytes, uint64 Value)
{
	for (int32 Index = 0; Index < 8; ++Index)
	{
		OutBytes.Add(static_cast<uint8>((Value >> (Index * 8)) & 0xff));
	}
}
