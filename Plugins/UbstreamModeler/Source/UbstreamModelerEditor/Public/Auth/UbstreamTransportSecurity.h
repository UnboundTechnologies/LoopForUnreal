#pragma once
#include "CoreMinimal.h"

namespace UbstreamTransportSecurity
{
// TLS is required outside exact loopback hosts used by local development tools.
bool IsAllowedUrl(const FString& Url, bool bWebSocket = false);
}
