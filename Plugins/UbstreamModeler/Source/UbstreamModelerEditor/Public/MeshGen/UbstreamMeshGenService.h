#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "MeshGen/UbstreamMeshGenTypes.h"

class IHttpRequest;
class IHttpResponse;
class UStaticMesh;
// Match Unreal HTTP module typedefs used by OnProcessRequestComplete.
using FHttpRequestPtr = TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>;
using FHttpResponsePtr = TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>;

DECLARE_MULTICAST_DELEGATE_OneParam(FUbstreamMeshGenProgressEvent, const FUbstreamMeshGenProgress&);
DECLARE_MULTICAST_DELEGATE_OneParam(FUbstreamMeshGenCompletedEvent, const FUbstreamMeshGenResult&);

/**
 * Owns one Unbound Zero image-to-mesh job: multipart create, a GLB download
 * that waits for the result (with a status check whenever that wait runs out),
 * and handoff to the mesh import service. All callbacks fire on the game thread.
 */
class UBSTREAMMODELEREDITOR_API FUbstreamMeshGenService
	: public TSharedFromThis<FUbstreamMeshGenService, ESPMode::ThreadSafe>
{
public:
	FUbstreamMeshGenService();
	~FUbstreamMeshGenService();

	bool IsBusy() const;
	const FUbstreamMeshGenProgress& GetProgress() const { return Progress; }
	const FUbstreamMeshGenResult& GetLastResult() const { return LastResult; }

	/** Starts a new job. Returns false if already busy or the request is invalid. */
	bool Start(const FUbstreamMeshGenRequest& Request);
	void Cancel();

	FUbstreamMeshGenProgressEvent OnProgress;
	FUbstreamMeshGenCompletedEvent OnCompleted;

private:
	friend class FUbstreamMeshGenLifecycleTest;
	friend class FUbstreamMeshGenApiTest;
	/** Claim only the active request before a callback changes job state. */
	bool ClaimCompletedRequest(const FHttpRequestPtr& Request);
	void SetPhase(EUbstreamMeshGenPhase Phase, const FString& ErrorMessage = FString());
	/** Rebuild status text + elapsed without changing phase floor. */
	void RefreshProgressLabel();
	/** Creep fraction toward the current phase ceiling (smooth bar while waiting). */
	void AdvanceSmoothedProgress(float DeltaSeconds);
	void BroadcastProgress();
	void CompleteWithFailure(const FString& ErrorMessage);
	void CompleteWithFailure(const FUbstreamMeshGenUserError& UserError);
	void CompleteWithSuccess(const FString& AssetPath, UStaticMesh* StaticMesh, TArray<uint8>&& GlbBytes);
	void ApplyAuthHeaders(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request) const;
	FString MakeTasksUrl() const;
	FString MakeTaskUrl(const FString& TaskId) const;
	FString MakeDownloadUrl(const FString& TaskId) const;
	/** The download URL with ?wait=, which holds open until the mesh is ready. */
	FString MakeWaitingDownloadUrl(const FString& TaskId) const;
	float GetPollIntervalSeconds() const;
	bool BuildMultipartBody(
		const FUbstreamMeshGenRequest& Request,
		TArray<uint8>& OutBody,
		FString& OutContentType,
		FString& OutError) const;
	void BeginCreateTask();
	void HandleCreateTaskComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
	void ScheduleNextPoll();
	bool RetryReadRequest(int32 Code, const FString& RetryAfter = FString());
	void CancelPollTicker();
	bool HandlePollTicker(float DeltaTime);
	void BeginPollTask();
	void HandlePollTaskComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
	void BeginDownload();
	void HandleDownloadComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
	void BeginImport(TArray<uint8>&& GlbBytes);
	void BestEffortDeleteTask();

	FUbstreamMeshGenRequest ActiveRequest;
	FUbstreamMeshGenProgress Progress;
	FUbstreamMeshGenResult LastResult;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> InflightRequest;
	FTSTicker::FDelegateHandle PollTickerHandle;
	double JobStartTimeSeconds = 0.0;
	double PhaseStartTimeSeconds = 0.0;
	double NextPollTimeSeconds = 0.0;
	/** When the latest waiting download was sent; paces the next one. */
	double WaitStartedAtSeconds = 0.0;
	uint64 JobGeneration = 0;
	bool bCancelRequested = false;
	bool bBusy = false;
	bool bReadRetryPending = false;
	int32 ReadRetryCount = 0;
};
