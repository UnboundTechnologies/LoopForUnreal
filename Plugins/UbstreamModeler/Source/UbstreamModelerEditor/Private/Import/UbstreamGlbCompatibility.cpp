#include "Import/UbstreamGlbCompatibility.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if USE_DRACO_LIBRARY
THIRD_PARTY_INCLUDES_START
#include "draco/compression/decode.h"
THIRD_PARTY_INCLUDES_END
#endif

namespace
{
	uint32 ReadU32(const uint8* Bytes)
	{
		return uint32(Bytes[0]) | (uint32(Bytes[1]) << 8) | (uint32(Bytes[2]) << 16) | (uint32(Bytes[3]) << 24);
	}

	void AppendU32(TArray<uint8>& Bytes, uint32 Value)
	{
		for (int32 Shift = 0; Shift < 32; Shift += 8) Bytes.Add(uint8(Value >> Shift));
	}
}

bool UbstreamGlbCompatibility::PrepareForImport(const TArray<uint8>& Source, TArray<uint8>& OutGlb, FString& OutError)
{
	OutGlb.Reset();
	OutError.Reset();
	auto Fail = [&OutError](const TCHAR* Message) { OutError = Message; return false; };
	if (Source.Num() < 20 || ReadU32(Source.GetData()) != 0x46546c67
		|| ReadU32(Source.GetData() + 4) != 2 || ReadU32(Source.GetData() + 8) != uint32(Source.Num()))
	{
		return Fail(TEXT("The downloaded file is not a complete GLB 2.0 mesh."));
	}
	const uint32 JsonLength = ReadU32(Source.GetData() + 12);
	if (ReadU32(Source.GetData() + 16) != 0x4e4f534a || JsonLength > uint32(Source.Num() - 20) || JsonLength % 4 != 0)
	{
		return Fail(TEXT("The GLB metadata chunk is invalid."));
	}
	const FUTF8ToTCHAR JsonText(reinterpret_cast<const ANSICHAR*>(Source.GetData() + 20), JsonLength);
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(JsonText.Length(), JsonText.Get())), Root) || !Root)
	{
		return Fail(TEXT("The GLB metadata could not be read."));
	}
	int32 BinaryOffset = 0, BinaryLength = 0;
	for (int64 Offset = 20 + int64(JsonLength); Offset < Source.Num();)
	{
		if (Offset + 8 > Source.Num()) return Fail(TEXT("The GLB contains an incomplete chunk."));
		const uint32 Length = ReadU32(Source.GetData() + Offset);
		if (Offset + 8 + Length > Source.Num() || Length % 4 != 0) return Fail(TEXT("The GLB contains an invalid chunk length."));
		if (ReadU32(Source.GetData() + Offset + 4) == 0x004e4942)
		{
			BinaryOffset = int32(Offset + 8);
			BinaryLength = int32(Length);
		}
		Offset += 8 + Length;
	}
	const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Accessors = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Views = nullptr;
	Root->TryGetArrayField(TEXT("accessors"), Accessors);
	Root->TryGetArrayField(TEXT("bufferViews"), Views);
	bool bChanged = false;
	if (Root->TryGetArrayField(TEXT("meshes"), Meshes))
	for (const auto& MeshValue : *Meshes)
	{
		const TSharedPtr<FJsonObject> MeshObject = MeshValue->Type == EJson::Object ? MeshValue->AsObject() : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Primitives = nullptr;
		if (!MeshObject || !MeshObject->TryGetArrayField(TEXT("primitives"), Primitives)) continue;
		for (const auto& PrimitiveValue : *Primitives)
		{
			const TSharedPtr<FJsonObject> Primitive = PrimitiveValue->Type == EJson::Object ? PrimitiveValue->AsObject() : nullptr;
			const TSharedPtr<FJsonObject>* Extensions = nullptr;
			const TSharedPtr<FJsonObject>* DracoExtension = nullptr;
			if (!Primitive || !Primitive->TryGetObjectField(TEXT("extensions"), Extensions)
				|| !(*Extensions)->TryGetObjectField(TEXT("KHR_draco_mesh_compression"), DracoExtension)) continue;
#if USE_DRACO_LIBRARY
			int32 ViewIndex = -1;
			if (!Views || !Accessors || !(*DracoExtension)->TryGetNumberField(TEXT("bufferView"), ViewIndex)
				|| !Views->IsValidIndex(ViewIndex) || (*Views)[ViewIndex]->Type != EJson::Object)
			{
				return Fail(TEXT("The compressed mesh references an invalid buffer."));
			}
			const TSharedPtr<FJsonObject> View = (*Views)[ViewIndex]->AsObject();
			int32 BufferIndex = -1, Offset = 0, Length = 0;
			View->TryGetNumberField(TEXT("byteOffset"), Offset);
			if (!View->TryGetNumberField(TEXT("buffer"), BufferIndex) || BufferIndex != 0
				|| !View->TryGetNumberField(TEXT("byteLength"), Length)
				|| BinaryOffset == 0 || Offset < 0 || Length <= 0 || int64(Offset) + Length > BinaryLength)
			{
				return Fail(TEXT("The compressed mesh data is missing or truncated."));
			}
			draco::DecoderBuffer Buffer;
			Buffer.Init(reinterpret_cast<const char*>(Source.GetData() + BinaryOffset + Offset), Length);
			draco::Decoder Decoder;
			auto Decoded = Decoder.DecodeMeshFromBuffer(&Buffer);
			if (!Decoded.ok() || !Decoded.value() || Decoded.value()->num_points() == 0 || Decoded.value()->num_faces() == 0)
			{
				return Fail(TEXT("The server returned an empty or invalid compressed mesh."));
			}
			const auto& Mesh = *Decoded.value();
			auto SetCount = [&](int32 Index, int64 Count)
			{
				if (!Accessors->IsValidIndex(Index) || (*Accessors)[Index]->Type != EJson::Object) return false;
				const auto& Accessor = (*Accessors)[Index]->AsObject();
				double Previous = 0;
				if (!Accessor->TryGetNumberField(TEXT("count"), Previous) || Previous != double(Count))
				{
					Accessor->SetNumberField(TEXT("count"), double(Count));
					bChanged = true;
				}
				return true;
			};
			const TSharedPtr<FJsonObject>* Attributes = nullptr;
			const TSharedPtr<FJsonObject>* DracoAttributes = nullptr;
			if (!Primitive->TryGetObjectField(TEXT("attributes"), Attributes)
				|| !(*DracoExtension)->TryGetObjectField(TEXT("attributes"), DracoAttributes))
			{
				return Fail(TEXT("The compressed mesh has no attribute mapping."));
			}
			for (const auto& Attribute : (*DracoAttributes)->Values)
			{
				int32 Index = -1;
				if (!(*Attributes)->TryGetNumberField(Attribute.Key, Index) || !SetCount(Index, Mesh.num_points()))
				{
					return Fail(TEXT("The compressed mesh references an invalid attribute."));
				}
			}
			int32 Index = -1;
			if (Primitive->TryGetNumberField(TEXT("indices"), Index) && !SetCount(Index, int64(Mesh.num_faces()) * 3))
			{
				return Fail(TEXT("The compressed mesh references invalid triangle indices."));
			}
#else
			return Fail(TEXT("This Unreal platform does not include a Draco mesh decoder."));
#endif
		}
	}
	if (!bChanged)
	{
		OutGlb = Source;
		return true;
	}
	// Only JSON metadata changes. Preserve compressed geometry and all following chunks byte-for-byte.
	FString UpdatedJson;
	FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&UpdatedJson));
	const FTCHARToUTF8 Utf8(*UpdatedJson);
	TArray<uint8> JsonBytes;
	JsonBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	while (JsonBytes.Num() % 4) JsonBytes.Add(' ');
	const int32 TailOffset = 20 + int32(JsonLength);
	const int64 NewLength = 20LL + JsonBytes.Num() + Source.Num() - TailOffset;
	if (NewLength > MAX_int32) return Fail(TEXT("The mesh is too large to import."));
	AppendU32(OutGlb, 0x46546c67);
	AppendU32(OutGlb, 2);
	AppendU32(OutGlb, uint32(NewLength));
	AppendU32(OutGlb, JsonBytes.Num());
	AppendU32(OutGlb, 0x4e4f534a);
	OutGlb.Append(JsonBytes);
	OutGlb.Append(Source.GetData() + TailOffset, Source.Num() - TailOffset);
	return true;
}
