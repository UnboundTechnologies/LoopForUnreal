#pragma once

#include "CoreMinimal.h"

namespace UbstreamProtocol
{
	inline constexpr const TCHAR* Name = TEXT("ubstream.websocket.v1");
	inline constexpr uint16 Version = 1;
	inline constexpr uint32 FrameBundleHeaderSize = 36;

	enum class EBinaryMessageType : uint16
	{
		FrameBundle = 1
	};
}

struct FUbstreamCameraState
{
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	float Fov = 45.0f;
	float Aspect = 1.0f;
};

struct FUbstreamFrameHeader
{
	FString SessionId;
	uint64 FrameId = 0;
	int32 Width = 0;
	int32 Height = 0;
	FString ColorFormat = TEXT("jpeg");
	FString DepthFormat = TEXT("gray_jpeg");
	FUbstreamCameraState Camera;
};
