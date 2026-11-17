#pragma once

#include "CoreMinimal.h"

enum class EUbstreamTransportStatus : uint8
{
	Disconnected,
	Connecting,
	Connected,
	Error
};
