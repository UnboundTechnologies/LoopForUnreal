#pragma once

#include "CoreMinimal.h"
#include "MeshGen/UbstreamMeshGenTypes.h"

namespace UbstreamMeshGenError
{
	/** Extract a human-readable detail string from a JSON error body if present. */
	FString ExtractServerDetail(const FString& ResponseBody);

	/**
	 * Build a user-facing error from HTTP/network/import failure context.
	 * Prefer this over raw status strings in the UI.
	 */
	FUbstreamMeshGenUserError MakeHttpError(
		int32 HttpStatusCode,
		const FString& ResponseBody,
		EUbstreamMeshGenPhase Phase,
		const FString& FallbackMessage = FString());

	FUbstreamMeshGenUserError MakeNetworkError(EUbstreamMeshGenPhase Phase, const FString& Detail = FString());
	FUbstreamMeshGenUserError MakeImportError(const FString& Detail);
	FUbstreamMeshGenUserError MakeNoSourceError();
	FUbstreamMeshGenUserError MakeCancelled();
	FUbstreamMeshGenUserError MakeGeneric(const FString& Detail, EUbstreamMeshGenPhase Phase = EUbstreamMeshGenPhase::Failed);

	/** Map a completed failure result into display fields. */
	FUbstreamMeshGenUserError FromResult(const FUbstreamMeshGenResult& Result);
}
