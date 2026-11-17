#pragma once

#include "CoreMinimal.h"
#include "MeshGen/UbstreamMeshGenOptions.h"

enum class EUbstreamMeshGenPhase : uint8
{
	Idle,
	Uploading,
	Queued,
	MaskGeneration,
	Reconstruction,
	Exporting,
	Downloading,
	Importing,
	Completed,
	Failed,
	Cancelled
};

enum class EUbstreamMeshGenApiStatus : uint8
{
	Unknown,
	Queued,
	MaskGeneration,
	Reconstruction,
	Exporting,
	Completed,
	Failed
};

enum class EUbstreamMeshGenErrorKind : uint8
{
	None,
	Cancelled,
	NoSource,
	Network,
	Auth,
	InsufficientCredits,
	Forbidden,
	NotFound,
	Validation,
	PayloadTooLarge,
	Server,
	Busy,
	Import,
	Unknown
};

struct FUbstreamMeshGenUserError
{
	EUbstreamMeshGenErrorKind Kind = EUbstreamMeshGenErrorKind::None;
	FText Title;
	FText Message;
	FText Hint;
	FString TechnicalDetail;
	int32 HttpStatusCode = 0;
	EUbstreamMeshGenPhase FailedPhase = EUbstreamMeshGenPhase::Idle;

	bool IsSet() const { return Kind != EUbstreamMeshGenErrorKind::None; }
};

struct FUbstreamMeshGenRequest
{
	TArray<uint8> ImageBytes;
	FString ImageFileName = TEXT("capture.jpg");
	FString ImageContentType = TEXT("image/jpeg");
	int32 Seed = 42;
	/** Target quad count, clamped to the server's 1,000-20,000 range. */
	int32 Quads = UbstreamMeshGenApi::DefaultQuads;
	/** Normal map of the fine surface detail; off makes the GLB about 1.4 MB smaller. */
	bool bNormalMap = true;
	FString BaseUrl;
	FString UserId = TEXT("ue-editor");
	/** Minimum time between status checks; the held-open download does the waiting. */
	float PollIntervalSeconds = UbstreamMeshGenApi::MinPollIntervalSeconds;
	/** No server phase change for this long ends local waiting. Zero disables. */
	double StallTimeoutSeconds = 1800.0;
	FString ImportDirectory = TEXT("/Game/Ubstream/Generated");
	/** Optional explicit name; empty uses UB_01, UB_02, etc. in the import folder. */
	FString AssetStem;
};

struct FUbstreamMeshGenProgress
{
	EUbstreamMeshGenPhase Phase = EUbstreamMeshGenPhase::Idle;
	float Fraction = 0.0f;
	FString StatusText;
	FString TaskId;
	FString ErrorMessage;
	double ElapsedSeconds = 0.0;
};

struct FUbstreamMeshGenResult
{
	bool bSuccess = false;
	FString TaskId;
	FString AssetPath;
	/** Raw / technical error string (logs / fallbacks). */
	FString ErrorMessage;
	/** Structured, user-facing error for styled UI. */
	FUbstreamMeshGenUserError UserError;
	int32 HttpStatusCode = 0;
	EUbstreamMeshGenPhase FailedPhase = EUbstreamMeshGenPhase::Idle;
	TArray<uint8> GlbBytes;
	TWeakObjectPtr<class UStaticMesh> StaticMesh;
};

inline EUbstreamMeshGenApiStatus ParseUbstreamMeshGenApiStatus(const FString& Status)
{
	if (Status.Equals(TEXT("queued"), ESearchCase::IgnoreCase))
	{
		return EUbstreamMeshGenApiStatus::Queued;
	}
	if (Status.Equals(TEXT("mask_generation"), ESearchCase::IgnoreCase))
	{
		return EUbstreamMeshGenApiStatus::MaskGeneration;
	}
	// Unbound Zero reports `running` while it generates.
	if (Status.Equals(TEXT("reconstruction"), ESearchCase::IgnoreCase)
		|| Status.Equals(TEXT("running"), ESearchCase::IgnoreCase))
	{
		return EUbstreamMeshGenApiStatus::Reconstruction;
	}
	if (Status.Equals(TEXT("exporting"), ESearchCase::IgnoreCase))
	{
		return EUbstreamMeshGenApiStatus::Exporting;
	}
	if (Status.Equals(TEXT("completed"), ESearchCase::IgnoreCase))
	{
		return EUbstreamMeshGenApiStatus::Completed;
	}
	if (Status.Equals(TEXT("failed"), ESearchCase::IgnoreCase))
	{
		return EUbstreamMeshGenApiStatus::Failed;
	}
	return EUbstreamMeshGenApiStatus::Unknown;
}

inline float MeshGenPhaseProgressFraction(const EUbstreamMeshGenPhase Phase)
{
	switch (Phase)
	{
	case EUbstreamMeshGenPhase::Uploading: return 0.05f;
	case EUbstreamMeshGenPhase::Queued: return 0.12f;
	case EUbstreamMeshGenPhase::MaskGeneration: return 0.30f;
	case EUbstreamMeshGenPhase::Reconstruction: return 0.55f;
	case EUbstreamMeshGenPhase::Exporting: return 0.82f;
	case EUbstreamMeshGenPhase::Downloading: return 0.90f;
	case EUbstreamMeshGenPhase::Importing: return 0.96f;
	case EUbstreamMeshGenPhase::Completed: return 1.0f;
	case EUbstreamMeshGenPhase::Failed:
	case EUbstreamMeshGenPhase::Cancelled:
	case EUbstreamMeshGenPhase::Idle:
	default:
		return 0.0f;
	}
}

/** Soft ceiling for time-based creep within a phase (just under the next phase floor). */
inline float MeshGenPhaseProgressCeiling(const EUbstreamMeshGenPhase Phase)
{
	switch (Phase)
	{
	case EUbstreamMeshGenPhase::Uploading: return 0.11f;
	case EUbstreamMeshGenPhase::Queued: return 0.28f;
	case EUbstreamMeshGenPhase::MaskGeneration: return 0.53f;
	case EUbstreamMeshGenPhase::Reconstruction: return 0.80f;
	case EUbstreamMeshGenPhase::Exporting: return 0.89f;
	case EUbstreamMeshGenPhase::Downloading: return 0.95f;
	case EUbstreamMeshGenPhase::Importing: return 0.99f;
	case EUbstreamMeshGenPhase::Completed: return 1.0f;
	case EUbstreamMeshGenPhase::Failed:
	case EUbstreamMeshGenPhase::Cancelled:
	case EUbstreamMeshGenPhase::Idle:
	default:
		return MeshGenPhaseProgressFraction(Phase);
	}
}

inline FString MeshGenPhaseLabel(const EUbstreamMeshGenPhase Phase)
{
	switch (Phase)
	{
	case EUbstreamMeshGenPhase::Uploading: return TEXT("Uploading image…");
	case EUbstreamMeshGenPhase::Queued: return TEXT("Queued…");
	case EUbstreamMeshGenPhase::MaskGeneration: return TEXT("Generating mask…");
	case EUbstreamMeshGenPhase::Reconstruction: return TEXT("Generating mesh…");
	case EUbstreamMeshGenPhase::Exporting: return TEXT("Exporting GLB…");
	case EUbstreamMeshGenPhase::Downloading: return TEXT("Downloading mesh…");
	case EUbstreamMeshGenPhase::Importing: return TEXT("Importing into project…");
	case EUbstreamMeshGenPhase::Completed: return TEXT("Ready");
	case EUbstreamMeshGenPhase::Failed: return TEXT("Failed");
	case EUbstreamMeshGenPhase::Cancelled: return TEXT("Cancelled");
	case EUbstreamMeshGenPhase::Idle:
	default:
		return TEXT("Idle");
	}
}

inline EUbstreamMeshGenPhase MeshGenPhaseFromApiStatus(const EUbstreamMeshGenApiStatus Status)
{
	switch (Status)
	{
	case EUbstreamMeshGenApiStatus::Queued: return EUbstreamMeshGenPhase::Queued;
	case EUbstreamMeshGenApiStatus::MaskGeneration: return EUbstreamMeshGenPhase::MaskGeneration;
	case EUbstreamMeshGenApiStatus::Reconstruction: return EUbstreamMeshGenPhase::Reconstruction;
	case EUbstreamMeshGenApiStatus::Exporting: return EUbstreamMeshGenPhase::Exporting;
	case EUbstreamMeshGenApiStatus::Completed: return EUbstreamMeshGenPhase::Downloading;
	case EUbstreamMeshGenApiStatus::Failed: return EUbstreamMeshGenPhase::Failed;
	case EUbstreamMeshGenApiStatus::Unknown:
	default:
		// The API documents any status other than completed/failed as still
		// working; the server's own step names should not read as "Queued".
		return EUbstreamMeshGenPhase::Reconstruction;
	}
}
