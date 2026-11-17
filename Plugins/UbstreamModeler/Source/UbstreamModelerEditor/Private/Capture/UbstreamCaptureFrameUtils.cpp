#include "Capture/UbstreamCaptureFrameUtils.h"

#include "Capture/UbstreamEditorWorldCaptureService.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Protocol/UbstreamProtocolTypes.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	TArray<TSharedPtr<FJsonValue>> MakeVectorArray(const FVector& Vector)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(3);
		Values.Add(MakeShared<FJsonValueNumber>(Vector.X));
		Values.Add(MakeShared<FJsonValueNumber>(Vector.Y));
		Values.Add(MakeShared<FJsonValueNumber>(Vector.Z));
		return Values;
	}

	TArray<TSharedPtr<FJsonValue>> MakeRotatorArray(const FRotator& Rotator)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(3);
		Values.Add(MakeShared<FJsonValueNumber>(Rotator.Pitch));
		Values.Add(MakeShared<FJsonValueNumber>(Rotator.Yaw));
		Values.Add(MakeShared<FJsonValueNumber>(Rotator.Roll));
		return Values;
	}

	int64 GetUnixTimestampMs()
	{
		static const FDateTime UnixEpoch(1970, 1, 1);
		return (FDateTime::UtcNow().GetTicks() - UnixEpoch.GetTicks()) / ETimespan::TicksPerMillisecond;
	}

}

namespace UbstreamCaptureFrameUtils
{
	FString GetCaptureOutputDir()
	{
		const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UbstreamCaptures"));
		IFileManager::Get().MakeDirectory(*OutputDir, true);
		return OutputDir;
	}

	FString BuildFrameHeaderJson(const FUbstreamEditorWorldCaptureResult& Result)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), TEXT("frame_header"));
		Root->SetStringField(TEXT("protocol"), UbstreamProtocol::Name);
		Root->SetStringField(TEXT("sessionId"), Result.SessionId);
		Root->SetNumberField(TEXT("frameId"), static_cast<double>(Result.FrameId));
		Root->SetNumberField(TEXT("timestampMs"), static_cast<double>(GetUnixTimestampMs()));
		Root->SetNumberField(TEXT("width"), Result.Size.X);
		Root->SetNumberField(TEXT("height"), Result.Size.Y);
		Root->SetStringField(TEXT("colorFormat"), TEXT("jpeg"));
		Root->SetStringField(TEXT("depthFormat"), TEXT("gray_jpeg"));
		Root->SetNumberField(TEXT("colorBytes"), Result.ColorJpegBytes.Num());
		Root->SetNumberField(TEXT("depthBytes"), Result.DepthJpegBytes.Num());
		Root->SetStringField(TEXT("captureMode"), Result.AppliedShowOnlyComponentCount > 0 ? TEXT("show_only") : TEXT("scene"));
		Root->SetNumberField(TEXT("showOnlyRequested"), Result.RequestedShowOnlyComponentCount);
		Root->SetNumberField(TEXT("showOnlyApplied"), Result.AppliedShowOnlyComponentCount);
		Root->SetBoolField(TEXT("usedSceneFallback"), Result.bUsedSceneFallback);
		Root->SetBoolField(TEXT("usedFlatColorFallback"), Result.bUsedFlatColorFallback);

		const TSharedRef<FJsonObject> CameraObject = MakeShared<FJsonObject>();
		CameraObject->SetNumberField(TEXT("fov"), Result.Camera.FovDegrees);
		CameraObject->SetNumberField(TEXT("aspect"), Result.Camera.AspectRatio);

		const TSharedRef<FJsonObject> TransformObject = MakeShared<FJsonObject>();
		TransformObject->SetArrayField(TEXT("location"), MakeVectorArray(Result.Camera.Location));
		TransformObject->SetArrayField(TEXT("rotation"), MakeRotatorArray(Result.Camera.Rotation));
		CameraObject->SetObjectField(TEXT("transform"), TransformObject);
		Root->SetObjectField(TEXT("camera"), CameraObject);

		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);
		return Json;
	}
}
