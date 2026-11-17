#pragma once

#include "CoreMinimal.h"

class IUbstreamTransport
{
public:
	virtual ~IUbstreamTransport() = default;

	virtual void Connect(const FString& Url) = 0;
	virtual void Disconnect() = 0;
	virtual bool IsConnected() const = 0;
	virtual bool SendJson(const FString& Json) = 0;
	virtual bool SendBinary(const TArray<uint8>& Bytes) = 0;
};
