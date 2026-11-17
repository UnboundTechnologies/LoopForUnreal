#pragma once

#include "CoreMinimal.h"

/**
 * Wire values for the Unbound Zero /api/v2 task API.
 */
namespace UbstreamMeshGenApi
{
	inline constexpr const TCHAR* DefaultBaseUrl = TEXT("https://unbound-3dgen-server-618775028388.us-central1.run.app/api/v2");

	/** Model id sent as `model`. */
	inline constexpr const TCHAR* ModelId = TEXT("unbound-zero");
	/** Base color, metallic and roughness maps. The server would otherwise bake vertex-lit colors. */
	inline constexpr const TCHAR* Material = TEXT("pbr");

	/** Target quad count of the remeshed result; it lands near the target, not on it. */
	inline constexpr int32 MinQuads = 1000;
	inline constexpr int32 MaxQuads = 20000;
	inline constexpr int32 DefaultQuads = 5000;

	/** The download holds open until the mesh is ready, for up to this long, then answers 409. */
	inline constexpr int32 DownloadWaitSeconds = 60;
	/** Floor between status checks the API asks clients to keep. */
	inline constexpr float MinPollIntervalSeconds = 5.0f;
	inline constexpr float MaxPollIntervalSeconds = 60.0f;

	inline int32 ClampQuads(const int32 Quads)
	{
		return FMath::Clamp(Quads, MinQuads, MaxQuads);
	}

	inline FString ResolveBaseUrl(FString BaseUrl)
	{
		BaseUrl.TrimStartAndEndInline();
		while (BaseUrl.EndsWith(TEXT("/"))) BaseUrl.LeftChopInline(1);
		// Migrate the retired shipped default, including saved user settings.
		// Custom hosts and paths remain under the user's control.
		if (BaseUrl == TEXT("https://unbound-3dgen-server-618775028388.us-central1.run.app/api/v1"))
		{
			return DefaultBaseUrl;
		}
		return BaseUrl;
	}
}
