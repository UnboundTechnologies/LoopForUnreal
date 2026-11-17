#include "MeshGen/UbstreamMeshGenService.h"
#include "Auth/UbstreamTransportSecurity.h"
#include "Auth/UbstreamAuthService.h"

#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "HttpModule.h"
#include "Import/UbstreamMeshImportService.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "MeshGen/UbstreamMeshGenError.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogUbstreamMeshGen, Log, All);

namespace
{
	void AppendMultipartUtf8(TArray<uint8>& Out, const FString& Text)
	{
		const FTCHARToUTF8 Converter(*Text);
		Out.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
	}

	FString FormatElapsed(const double Seconds)
	{
		const int32 Total = FMath::Max(0, FMath::FloorToInt(Seconds));
		const int32 Minutes = Total / 60;
		const int32 Secs = Total % 60;
		if (Minutes > 0)
		{
			return FString::Printf(TEXT("%dm %02ds"), Minutes, Secs);
		}
		return FString::Printf(TEXT("%ds"), Secs);
	}
}

FUbstreamMeshGenService::FUbstreamMeshGenService() = default;

FUbstreamMeshGenService::~FUbstreamMeshGenService()
{
	Cancel();
}

bool FUbstreamMeshGenService::IsBusy() const
{
	return bBusy;
}

bool FUbstreamMeshGenService::ClaimCompletedRequest(const FHttpRequestPtr& Request)
{
	// HTTP cancellation completes asynchronously. A previous job's callback
	// must not clear the replacement job's request, even after Start resets the
	// cancellation flag. Each phase creates a distinct request object.
	if (!bBusy || bCancelRequested || !Request.IsValid() || Request != InflightRequest)
	{
		return false;
	}
	InflightRequest.Reset();
	return true;
}

bool FUbstreamMeshGenService::Start(const FUbstreamMeshGenRequest& Request)
{
	if (bBusy)
	{
		UE_LOG(LogUbstreamMeshGen, Warning, TEXT("Start refused: a mesh generation job is already running"));
		return false;
	}

	if (Request.ImageBytes.IsEmpty())
	{
		UE_LOG(LogUbstreamMeshGen, Warning, TEXT("Start refused: image payload is empty"));
		return false;
	}

	if (!UbstreamTransportSecurity::IsAllowedUrl(Request.BaseUrl.TrimStartAndEnd()))
	{
		CompleteWithFailure(TEXT("Mesh generation requires an https:// endpoint. Plain http:// is allowed only for localhost development."));
		return false;
	}

	ActiveRequest = Request;
	ActiveRequest.BaseUrl = UbstreamMeshGenApi::ResolveBaseUrl(Request.BaseUrl);

	bBusy = true;
	bCancelRequested = false;
	bReadRetryPending = false;
	ReadRetryCount = 0;
	++JobGeneration;
	JobStartTimeSeconds = FPlatformTime::Seconds();
	PhaseStartTimeSeconds = JobStartTimeSeconds;
	LastResult = FUbstreamMeshGenResult();
	Progress = FUbstreamMeshGenProgress();
	Progress.TaskId.Reset();
	Progress.Fraction = 0.0f;

	SetPhase(EUbstreamMeshGenPhase::Uploading);
	// Keep the bar alive during the create-task round trip (no poll ticker yet).
	ScheduleNextPoll();
	BeginCreateTask();
	return true;
}

void FUbstreamMeshGenService::Cancel()
{
	if (!bBusy && Progress.Phase != EUbstreamMeshGenPhase::Cancelled)
	{
		CancelPollTicker();
		if (InflightRequest.IsValid())
		{
			InflightRequest->CancelRequest();
			InflightRequest.Reset();
		}
		return;
	}

	bCancelRequested = true;
	++JobGeneration;
	CancelPollTicker();
	if (InflightRequest.IsValid())
	{
		InflightRequest->CancelRequest();
		InflightRequest.Reset();
	}

	BestEffortDeleteTask();

	bBusy = false;
	SetPhase(EUbstreamMeshGenPhase::Cancelled);
	LastResult.bSuccess = false;
	LastResult.TaskId = Progress.TaskId;
	LastResult.UserError = UbstreamMeshGenError::MakeCancelled();
	LastResult.ErrorMessage = TEXT("Cancelled");
	LastResult.FailedPhase = EUbstreamMeshGenPhase::Cancelled;
	OnCompleted.Broadcast(LastResult);
}

void FUbstreamMeshGenService::SetPhase(const EUbstreamMeshGenPhase Phase, const FString& ErrorMessage)
{
	const bool bPhaseChanged = Progress.Phase != Phase;
	Progress.Phase = Phase;
	Progress.ErrorMessage = ErrorMessage;

	const float Floor = MeshGenPhaseProgressFraction(Phase);
	if (bPhaseChanged)
	{
		PhaseStartTimeSeconds = FPlatformTime::Seconds();
		// Never jump backwards; snap up to the new floor when advancing.
		Progress.Fraction = FMath::Max(Progress.Fraction, Floor);
	}
	else
	{
		Progress.Fraction = FMath::Max(Progress.Fraction, Floor);
	}

	if (Phase == EUbstreamMeshGenPhase::Completed)
	{
		Progress.Fraction = 1.0f;
	}

	RefreshProgressLabel();
	BroadcastProgress();
}

void FUbstreamMeshGenService::RefreshProgressLabel()
{
	Progress.ElapsedSeconds = FMath::Max(0.0, FPlatformTime::Seconds() - JobStartTimeSeconds);

	FString Label = MeshGenPhaseLabel(Progress.Phase);
	if (Progress.Phase == EUbstreamMeshGenPhase::Failed && !Progress.ErrorMessage.IsEmpty())
	{
		Label = FString::Printf(TEXT("Failed: %s"), *Progress.ErrorMessage);
	}
	else if (Progress.Phase != EUbstreamMeshGenPhase::Completed
		&& Progress.Phase != EUbstreamMeshGenPhase::Failed
		&& Progress.Phase != EUbstreamMeshGenPhase::Cancelled
		&& Progress.Phase != EUbstreamMeshGenPhase::Idle)
	{
		const int32 Percent = FMath::Clamp(FMath::RoundToInt(Progress.Fraction * 100.0f), 0, 100);
		Label = FString::Printf(TEXT("%s  ·  %d%%  ·  %s"),
			*Label,
			Percent,
			*FormatElapsed(Progress.ElapsedSeconds));
		if (!Progress.TaskId.IsEmpty())
		{
			Label += FString::Printf(TEXT("  ·  %s"), *Progress.TaskId.Left(8));
		}
	}

	Progress.StatusText = Label;
}

void FUbstreamMeshGenService::AdvanceSmoothedProgress(const float DeltaSeconds)
{
	if (!bBusy)
	{
		return;
	}

	const EUbstreamMeshGenPhase Phase = Progress.Phase;
	if (Phase == EUbstreamMeshGenPhase::Completed
		|| Phase == EUbstreamMeshGenPhase::Failed
		|| Phase == EUbstreamMeshGenPhase::Cancelled
		|| Phase == EUbstreamMeshGenPhase::Idle)
	{
		return;
	}

	const float Floor = MeshGenPhaseProgressFraction(Phase);
	const float Ceiling = MeshGenPhaseProgressCeiling(Phase);
	// Asymptotic creep: faster early in a phase, slows near the ceiling.
	const float Span = FMath::Max(Ceiling - Floor, 0.01f);
	const float Room = FMath::Max(Ceiling - Progress.Fraction, 0.0f);
	const float Rate = (Phase == EUbstreamMeshGenPhase::Reconstruction) ? 0.035f : 0.06f;
	const float Step = Room * (1.0f - FMath::Exp(-Rate * FMath::Max(DeltaSeconds, 0.0f)));
	// Also guarantee a tiny minimum advance so short waits still feel alive.
	const float MinStep = Span * 0.002f * FMath::Max(DeltaSeconds, 0.0f) * 60.0f;
	Progress.Fraction = FMath::Clamp(
		Progress.Fraction + FMath::Max(Step, FMath::Min(MinStep, Room)),
		Floor,
		Ceiling);

	RefreshProgressLabel();
	BroadcastProgress();
}

void FUbstreamMeshGenService::BroadcastProgress()
{
	OnProgress.Broadcast(Progress);
}

void FUbstreamMeshGenService::CompleteWithFailure(const FString& ErrorMessage)
{
	CompleteWithFailure(UbstreamMeshGenError::MakeGeneric(ErrorMessage, Progress.Phase));
}

void FUbstreamMeshGenService::CompleteWithFailure(const FUbstreamMeshGenUserError& UserError)
{
	CancelPollTicker();
	InflightRequest.Reset();
	bBusy = false;
	LastResult.bSuccess = false;
	LastResult.TaskId = Progress.TaskId;
	LastResult.UserError = UserError;
	LastResult.HttpStatusCode = UserError.HttpStatusCode;
	LastResult.FailedPhase = UserError.FailedPhase != EUbstreamMeshGenPhase::Idle
		? UserError.FailedPhase
		: Progress.Phase;
	LastResult.ErrorMessage = !UserError.TechnicalDetail.IsEmpty()
		? UserError.TechnicalDetail
		: UserError.Message.ToString();

	const FString StatusDetail = UserError.Message.IsEmpty()
		? LastResult.ErrorMessage
		: UserError.Message.ToString();
	UE_LOG(LogUbstreamMeshGen, Error, TEXT("Mesh gen failed [%d]: %s | %s"),
		static_cast<int32>(UserError.Kind),
		*StatusDetail,
		*LastResult.ErrorMessage);
	SetPhase(EUbstreamMeshGenPhase::Failed, StatusDetail);
	OnCompleted.Broadcast(LastResult);
}

void FUbstreamMeshGenService::CompleteWithSuccess(
	const FString& AssetPath,
	UStaticMesh* StaticMesh,
	TArray<uint8>&& GlbBytes)
{
	CancelPollTicker();
	InflightRequest.Reset();
	bBusy = false;
	LastResult.bSuccess = true;
	LastResult.TaskId = Progress.TaskId;
	LastResult.AssetPath = AssetPath;
	LastResult.StaticMesh = StaticMesh;
	LastResult.GlbBytes = MoveTemp(GlbBytes);
	LastResult.ErrorMessage.Reset();
	SetPhase(EUbstreamMeshGenPhase::Completed);
	OnCompleted.Broadcast(LastResult);
}

void FUbstreamMeshGenService::ApplyAuthHeaders(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request) const
{
	const FString Token = FUbstreamAuthService::Get().GetAuthKey();
	if (!Token.IsEmpty() && UbstreamTransportSecurity::IsAllowedUrl(Request->GetURL())) Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Token);
}

FString FUbstreamMeshGenService::MakeTasksUrl() const
{
	return ActiveRequest.BaseUrl / TEXT("tasks");
}

FString FUbstreamMeshGenService::MakeTaskUrl(const FString& TaskId) const
{
	return ActiveRequest.BaseUrl / TEXT("tasks") / TaskId;
}

FString FUbstreamMeshGenService::MakeDownloadUrl(const FString& TaskId) const
{
	return ActiveRequest.BaseUrl / TEXT("tasks") / TaskId / TEXT("download");
}

FString FUbstreamMeshGenService::MakeWaitingDownloadUrl(const FString& TaskId) const
{
	return FString::Printf(TEXT("%s?wait=%d"), *MakeDownloadUrl(TaskId), UbstreamMeshGenApi::DownloadWaitSeconds);
}

float FUbstreamMeshGenService::GetPollIntervalSeconds() const
{
	return FMath::Clamp(ActiveRequest.PollIntervalSeconds,
		UbstreamMeshGenApi::MinPollIntervalSeconds, UbstreamMeshGenApi::MaxPollIntervalSeconds);
}

bool FUbstreamMeshGenService::BuildMultipartBody(
	const FUbstreamMeshGenRequest& Request,
	TArray<uint8>& OutBody,
	FString& OutContentType,
	FString& OutError) const
{
	OutBody.Reset();
	const FString Boundary = FString::Printf(TEXT("----UbstreamMeshGen%08X"), FMath::Rand());
	OutContentType = FString::Printf(TEXT("multipart/form-data; boundary=%s"), *Boundary);

	const FString FileName = Request.ImageFileName.IsEmpty() ? TEXT("capture.jpg") : Request.ImageFileName;
	const FString ContentType = Request.ImageContentType.IsEmpty() ? TEXT("image/jpeg") : Request.ImageContentType;

	AppendMultipartUtf8(OutBody, FString::Printf(TEXT("--%s\r\n"), *Boundary));
	AppendMultipartUtf8(OutBody, FString::Printf(
		TEXT("Content-Disposition: form-data; name=\"image\"; filename=\"%s\"\r\n"),
		*FileName));
	AppendMultipartUtf8(OutBody, FString::Printf(TEXT("Content-Type: %s\r\n\r\n"), *ContentType));
	OutBody.Append(Request.ImageBytes);
	AppendMultipartUtf8(OutBody, TEXT("\r\n"));

	auto AppendField = [&OutBody, &Boundary](const TCHAR* Name, const FString& Value)
	{
		AppendMultipartUtf8(OutBody, FString::Printf(TEXT("--%s\r\n"), *Boundary));
		AppendMultipartUtf8(OutBody, FString::Printf(TEXT("Content-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n"), Name, *Value));
	};
	AppendField(TEXT("model"), UbstreamMeshGenApi::ModelId);
	AppendField(TEXT("material"), UbstreamMeshGenApi::Material);
	AppendField(TEXT("quads"), FString::FromInt(UbstreamMeshGenApi::ClampQuads(Request.Quads)));
	AppendField(TEXT("normal_map"), Request.bNormalMap ? TEXT("true") : TEXT("false"));
	AppendField(TEXT("seed"), FString::FromInt(Request.Seed));

	AppendMultipartUtf8(OutBody, FString::Printf(TEXT("--%s--\r\n"), *Boundary));

	if (OutBody.Num() < 64)
	{
		OutError = TEXT("Failed to build multipart body");
		return false;
	}

	return true;
}

void FUbstreamMeshGenService::BeginCreateTask()
{
	const uint64 Generation = JobGeneration;
	TArray<uint8> Body;
	FString ContentType;
	FString BuildError;
	if (!BuildMultipartBody(ActiveRequest, Body, ContentType, BuildError))
	{
		CompleteWithFailure(UbstreamMeshGenError::MakeGeneric(BuildError, EUbstreamMeshGenPhase::Uploading));
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	InflightRequest = Request;
	Request->SetURL(MakeTasksUrl());
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), ContentType);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	ApplyAuthHeaders(Request);
	Request->SetContent(MoveTemp(Body));
	Request->OnProcessRequestComplete().BindSP(
		this,
		&FUbstreamMeshGenService::HandleCreateTaskComplete);

	UE_LOG(LogUbstreamMeshGen, Log, TEXT("Creating Unbound Zero task: %s (bytes=%d seed=%d quads=%d normal_map=%d)"),
		*MakeTasksUrl(), ActiveRequest.ImageBytes.Num(), ActiveRequest.Seed,
		UbstreamMeshGenApi::ClampQuads(ActiveRequest.Quads), ActiveRequest.bNormalMap ? 1 : 0);

	if (!Request->ProcessRequest())
	{
		if (Generation == JobGeneration)
		{
			CompleteWithFailure(UbstreamMeshGenError::MakeNetworkError(
				EUbstreamMeshGenPhase::Uploading,
				TEXT("Failed to start HTTP create-task request")));
		}
	}
}

void FUbstreamMeshGenService::HandleCreateTaskComplete(
	FHttpRequestPtr Request,
	FHttpResponsePtr Response,
	const bool bSucceeded)
{
	if (!ClaimCompletedRequest(Request))
	{
		return;
	}

	if (!bSucceeded || !Response.IsValid())
	{
		CompleteWithFailure(UbstreamMeshGenError::MakeNetworkError(
			EUbstreamMeshGenPhase::Uploading,
			TEXT("Create task request failed (network)")));
		return;
	}

	const int32 Code = Response->GetResponseCode();
	const FString Body = Response->GetContentAsString();
	if (Code < 200 || Code >= 300)
	{
		UE_LOG(LogUbstreamMeshGen, Error, TEXT("Create task HTTP %d: %s"), Code, *Body.Left(500));
		CompleteWithFailure(UbstreamMeshGenError::MakeHttpError(
			Code, Body, EUbstreamMeshGenPhase::Uploading, FString::Printf(TEXT("Create task failed (HTTP %d)"), Code)));
		return;
	}

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		CompleteWithFailure(UbstreamMeshGenError::MakeGeneric(
			TEXT("Create task returned invalid JSON"),
			EUbstreamMeshGenPhase::Uploading));
		return;
	}

	FString TaskId;
	if (!JsonObject->TryGetStringField(TEXT("task_id"), TaskId) || TaskId.IsEmpty())
	{
		CompleteWithFailure(UbstreamMeshGenError::MakeGeneric(
			TEXT("Create task response missing task_id"),
			EUbstreamMeshGenPhase::Uploading));
		return;
	}

	Progress.TaskId = TaskId;
	FString StatusString;
	JsonObject->TryGetStringField(TEXT("status"), StatusString);
	const EUbstreamMeshGenApiStatus ApiStatus = ParseUbstreamMeshGenApiStatus(StatusString);
	SetPhase(MeshGenPhaseFromApiStatus(ApiStatus == EUbstreamMeshGenApiStatus::Unknown
		? EUbstreamMeshGenApiStatus::Queued
		: ApiStatus));

	UE_LOG(LogUbstreamMeshGen, Log, TEXT("Mesh gen task created: %s status=%s"), *TaskId, *StatusString);

	if (ApiStatus == EUbstreamMeshGenApiStatus::Completed)
	{
		SetPhase(EUbstreamMeshGenPhase::Downloading);
		// Smooth progress while downloading.
		ScheduleNextPoll();
		BeginDownload();
		return;
	}
	if (ApiStatus == EUbstreamMeshGenApiStatus::Failed)
	{
		FString Error;
		JsonObject->TryGetStringField(TEXT("error"), Error);
		FUbstreamMeshGenUserError UserError = UbstreamMeshGenError::MakeGeneric(
			Error.IsEmpty() ? TEXT("Task failed on the server") : Error,
			EUbstreamMeshGenPhase::Reconstruction);
		UserError.Title = NSLOCTEXT("UbstreamMeshGen", "TaskFailedTitle", "Generation failed");
		UserError.Hint = NSLOCTEXT(
			"UbstreamMeshGen",
			"TaskFailedHint",
			"Try a different seed or source image. Some images fail reconstruction.");
		CompleteWithFailure(UserError);
		return;
	}

	// The ticker drives smooth progress and the stall deadline (Start already
	// scheduled it). The download itself waits for the mesh: generation usually
	// finishes within seconds, so no status poll is needed on the happy path.
	if (!PollTickerHandle.IsValid())
	{
		ScheduleNextPoll();
	}
	BeginDownload();
}

bool FUbstreamMeshGenService::RetryReadRequest(int32 Code, const FString& RetryAfter)
{
	if (ReadRetryCount >= 5 || !(Code == 0 || Code == 408 || Code == 429 || Code == 500 || Code == 502 || Code == 503 || Code == 504)) return false;
	const double Delay = FMath::Clamp(FMath::Max(double(FCString::Atof(*RetryAfter)), 5.0 * (1 << ReadRetryCount)), 5.0, 60.0);
	++ReadRetryCount;
	bReadRetryPending = true;
	if (!PollTickerHandle.IsValid()) ScheduleNextPoll();
	NextPollTimeSeconds = FPlatformTime::Seconds() + Delay;
	SetPhase(Progress.Phase, TEXT("Connection interrupted. Retrying automatically..."));
	return true;
}

void FUbstreamMeshGenService::ScheduleNextPoll()
{
	CancelPollTicker();
	NextPollTimeSeconds = FPlatformTime::Seconds() + GetPollIntervalSeconds();
	PollTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateSP(this, &FUbstreamMeshGenService::HandlePollTicker),
		0.05f);
}

void FUbstreamMeshGenService::CancelPollTicker()
{
	if (PollTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollTickerHandle);
		PollTickerHandle.Reset();
	}
}

bool FUbstreamMeshGenService::HandlePollTicker(const float DeltaTime)
{
	if (bCancelRequested || !bBusy)
	{
		return false;
	}

	if (ActiveRequest.StallTimeoutSeconds > 0.0
		&& FPlatformTime::Seconds() - PhaseStartTimeSeconds >= ActiveRequest.StallTimeoutSeconds)
	{
		// Invalidate first: CancelRequest may synchronously deliver a callback.
		FHttpRequestPtr TimedOutRequest = MoveTemp(InflightRequest);
		if (TimedOutRequest.IsValid())
		{
			TimedOutRequest->OnProcessRequestComplete().Unbind();
			TimedOutRequest->CancelRequest();
		}
		FUbstreamMeshGenUserError Error = UbstreamMeshGenError::MakeGeneric(TEXT("No generation phase change before the waiting deadline."), Progress.Phase);
		Error.Title = NSLOCTEXT("UbstreamMeshGen", "StalledTitle", "Generation timed out");
		Error.Message = NSLOCTEXT("UbstreamMeshGen", "StalledMessage", "The server stayed in the same generation phase too long. Local waiting has stopped.");
		Error.Hint = NSLOCTEXT("UbstreamMeshGen", "StalledHint", "Check the server before retrying; the remote task may still be running. For long jobs, increase or disable Stalled Job Timeout in settings.");
		CompleteWithFailure(Error);
		return false;
	}

	AdvanceSmoothedProgress(DeltaTime);
	if (bReadRetryPending)
	{
		if (!InflightRequest.IsValid() && FPlatformTime::Seconds() >= NextPollTimeSeconds)
		{
			bReadRetryPending = false;
			if (Progress.Phase == EUbstreamMeshGenPhase::Downloading) BeginDownload();
			else BeginPollTask();
		}
		return bBusy && !bCancelRequested;
	}

	// During upload/download/import we still want smooth progress, but only wait
	// on the server when we actually have a task id and it is still working.
	const bool bShouldPollServer =
		!Progress.TaskId.IsEmpty()
		&& (Progress.Phase == EUbstreamMeshGenPhase::Queued
			|| Progress.Phase == EUbstreamMeshGenPhase::MaskGeneration
			|| Progress.Phase == EUbstreamMeshGenPhase::Reconstruction
			|| Progress.Phase == EUbstreamMeshGenPhase::Exporting);

	if (!bShouldPollServer)
	{
		return true;
	}

	if (FPlatformTime::Seconds() < NextPollTimeSeconds)
	{
		return true;
	}

	if (!InflightRequest.IsValid())
	{
		BeginDownload();
	}
	return bBusy && !bCancelRequested;
}

void FUbstreamMeshGenService::BeginPollTask()
{
	if (Progress.TaskId.IsEmpty())
	{
		CompleteWithFailure(UbstreamMeshGenError::MakeGeneric(
			TEXT("Cannot poll without task_id"),
			Progress.Phase));
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	InflightRequest = Request;
	Request->SetURL(MakeTaskUrl(Progress.TaskId));
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	ApplyAuthHeaders(Request);
	Request->OnProcessRequestComplete().BindSP(
		this,
		&FUbstreamMeshGenService::HandlePollTaskComplete);

	if (!Request->ProcessRequest())
	{
		CompleteWithFailure(UbstreamMeshGenError::MakeNetworkError(
			Progress.Phase,
			TEXT("Failed to start HTTP poll request")));
	}
}

void FUbstreamMeshGenService::HandlePollTaskComplete(
	FHttpRequestPtr Request,
	FHttpResponsePtr Response,
	const bool bSucceeded)
{
	if (!ClaimCompletedRequest(Request))
	{
		return;
	}

	if (!bSucceeded || !Response.IsValid())
	{
		if (RetryReadRequest(0)) return;
		CompleteWithFailure(UbstreamMeshGenError::MakeNetworkError(
			Progress.Phase,
			TEXT("Poll request failed (network)")));
		return;
	}

	const int32 Code = Response->GetResponseCode();
	if (RetryReadRequest(Code, Response->GetHeader(TEXT("Retry-After")))) return;
	if (Code >= 200 && Code < 300) ReadRetryCount = 0;
	const FString Body = Response->GetContentAsString();
	if (Code < 200 || Code >= 300)
	{
		UE_LOG(LogUbstreamMeshGen, Error, TEXT("Poll HTTP %d: %s"), Code, *Body.Left(500));
		CompleteWithFailure(UbstreamMeshGenError::MakeHttpError(
			Code, Body, Progress.Phase, FString::Printf(TEXT("Poll failed (HTTP %d)"), Code)));
		return;
	}

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		CompleteWithFailure(UbstreamMeshGenError::MakeGeneric(
			TEXT("Poll returned invalid JSON"),
			Progress.Phase));
		return;
	}

	FString StatusString;
	JsonObject->TryGetStringField(TEXT("status"), StatusString);
	const EUbstreamMeshGenApiStatus ApiStatus = ParseUbstreamMeshGenApiStatus(StatusString);

	if (ApiStatus == EUbstreamMeshGenApiStatus::Failed)
	{
		FString Error;
		if (!JsonObject->TryGetStringField(TEXT("error"), Error) || Error.IsEmpty() || Error.Equals(TEXT("null")))
		{
			Error = TEXT("Task failed on the server");
		}
		FUbstreamMeshGenUserError UserError = UbstreamMeshGenError::MakeGeneric(Error, Progress.Phase);
		UserError.Title = NSLOCTEXT("UbstreamMeshGen", "TaskFailedTitle2", "Generation failed");
		UserError.Hint = NSLOCTEXT(
			"UbstreamMeshGen",
			"TaskFailedHint2",
			"Try a different seed or source image. Some images fail reconstruction.");
		CompleteWithFailure(UserError);
		return;
	}

	if (ApiStatus == EUbstreamMeshGenApiStatus::Completed)
	{
		SetPhase(EUbstreamMeshGenPhase::Downloading);
		ScheduleNextPoll();
		BeginDownload();
		return;
	}

	// Still working: the ticker starts the next waiting download. A server that
	// held the last one for its full wait is asked again at once; one that
	// answered early is not asked more often than the poll interval allows.
	SetPhase(MeshGenPhaseFromApiStatus(ApiStatus));
	NextPollTimeSeconds = FMath::Max(FPlatformTime::Seconds(), WaitStartedAtSeconds + GetPollIntervalSeconds());
}

void FUbstreamMeshGenService::BeginDownload()
{
	if (Progress.TaskId.IsEmpty())
	{
		CompleteWithFailure(UbstreamMeshGenError::MakeGeneric(
			TEXT("Cannot download without task_id"),
			EUbstreamMeshGenPhase::Downloading));
		return;
	}

	// The server holds this request open until the mesh is ready (up to
	// DownloadWaitSeconds), then answers 409. The phase stays on the server's
	// step meanwhile. Unreal's default 30 s activity timeout would drop the
	// silent wait, so both limits are set for this request.
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	InflightRequest = Request;
	Request->SetURL(MakeWaitingDownloadUrl(Progress.TaskId));
	Request->SetVerb(TEXT("GET"));
	Request->SetActivityTimeout(UbstreamMeshGenApi::DownloadWaitSeconds + 30.0f);
	Request->SetTimeout(UbstreamMeshGenApi::DownloadWaitSeconds + 120.0f);
	ApplyAuthHeaders(Request);
	Request->OnProcessRequestComplete().BindSP(
		this,
		&FUbstreamMeshGenService::HandleDownloadComplete);
	WaitStartedAtSeconds = FPlatformTime::Seconds();

	UE_LOG(LogUbstreamMeshGen, Log, TEXT("Waiting for mesh: %s"), *MakeWaitingDownloadUrl(Progress.TaskId));

	if (!Request->ProcessRequest())
	{
		CompleteWithFailure(UbstreamMeshGenError::MakeNetworkError(
			EUbstreamMeshGenPhase::Downloading,
			TEXT("Failed to start HTTP download request")));
	}
}

void FUbstreamMeshGenService::HandleDownloadComplete(
	FHttpRequestPtr Request,
	FHttpResponsePtr Response,
	const bool bSucceeded)
{
	if (!ClaimCompletedRequest(Request))
	{
		return;
	}

	if (!bSucceeded || !Response.IsValid())
	{
		if (RetryReadRequest(0)) return;
		CompleteWithFailure(UbstreamMeshGenError::MakeNetworkError(
			EUbstreamMeshGenPhase::Downloading,
			TEXT("Download request failed (network)")));
		return;
	}

	const int32 Code = Response->GetResponseCode();
	if (RetryReadRequest(Code, Response->GetHeader(TEXT("Retry-After")))) return;
	if (Code >= 200 && Code < 300) ReadRetryCount = 0;
	if (Code == 409)
	{
		// Not ready within the wait, or the task failed: the status says which.
		UE_LOG(LogUbstreamMeshGen, Log, TEXT("Download returned 409; checking task status"));
		BeginPollTask();
		return;
	}

	if (Code < 200 || Code >= 300)
	{
		const FString Body = Response->GetContentAsString();
		UE_LOG(LogUbstreamMeshGen, Error, TEXT("Download HTTP %d"), Code);
		CompleteWithFailure(UbstreamMeshGenError::MakeHttpError(
			Code, Body, EUbstreamMeshGenPhase::Downloading,
			FString::Printf(TEXT("Download failed (HTTP %d)"), Code)));
		return;
	}

	TArray<uint8> GlbBytes = Response->GetContent();
	if (GlbBytes.Num() < 12)
	{
		CompleteWithFailure(UbstreamMeshGenError::MakeGeneric(
			TEXT("Downloaded GLB payload is too small or empty"),
			EUbstreamMeshGenPhase::Downloading));
		return;
	}

	// glTF binary magic "glTF"
	if (!(GlbBytes[0] == 'g' && GlbBytes[1] == 'l' && GlbBytes[2] == 'T' && GlbBytes[3] == 'F'))
	{
		UE_LOG(LogUbstreamMeshGen, Warning, TEXT("Download payload missing glTF magic; importing anyway (bytes=%d)"), GlbBytes.Num());
	}

	// Do NOT run Interchange import + CompleteWithSuccess on the HTTP manager
	// callback stack. That re-enters Slate/asset editors mid-tick and has crashed
	// when opening the Static Mesh Editor (AdvancedPreviewScene::SetProfileIndex).
	// Schedule import for the next core-ticker tick so the HTTP request fully unwinds.
	CancelPollTicker();
	SetPhase(EUbstreamMeshGenPhase::Importing);

	const uint64 ImportGeneration = JobGeneration;
	TWeakPtr<FUbstreamMeshGenService, ESPMode::ThreadSafe> WeakThis = AsShared();
	TArray<uint8> GlbCopy = MoveTemp(GlbBytes);
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda(
			[WeakThis, ImportGeneration, GlbCopy = MoveTemp(GlbCopy)](float) mutable
			{
				if (const TSharedPtr<FUbstreamMeshGenService, ESPMode::ThreadSafe> Pinned = WeakThis.Pin())
				{
					if (!Pinned->bCancelRequested && Pinned->JobGeneration == ImportGeneration && Pinned->bBusy)
					{
						Pinned->BeginImport(MoveTemp(GlbCopy));
					}
				}
				return false; // one-shot
			}),
		0.0f);
}

void FUbstreamMeshGenService::BeginImport(TArray<uint8>&& GlbBytes)
{
	if (bCancelRequested || !bBusy)
	{
		return;
	}

	CancelPollTicker();
	SetPhase(EUbstreamMeshGenPhase::Importing);

	const FUbstreamMeshImportResult ImportResult = FUbstreamMeshImportService::ImportGlbBytes(
		GlbBytes,
		ActiveRequest.ImportDirectory,
		ActiveRequest.AssetStem);

	if (!ImportResult.bSuccess)
	{
		CompleteWithFailure(UbstreamMeshGenError::MakeImportError(
			ImportResult.ErrorMessage.IsEmpty() ? TEXT("Mesh import failed") : ImportResult.ErrorMessage));
		return;
	}

	CompleteWithSuccess(
		ImportResult.AssetPath,
		ImportResult.StaticMesh.Get(),
		MoveTemp(GlbBytes));
}

void FUbstreamMeshGenService::BestEffortDeleteTask()
{
	if (Progress.TaskId.IsEmpty() || ActiveRequest.BaseUrl.IsEmpty())
	{
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(MakeTaskUrl(Progress.TaskId));
	Request->SetVerb(TEXT("DELETE"));
	ApplyAuthHeaders(Request);
	Request->OnProcessRequestComplete().BindLambda(
		[](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			const int32 Code = (bSucceeded && Response.IsValid()) ? Response->GetResponseCode() : -1;
			UE_LOG(LogUbstreamMeshGen, Log, TEXT("Best-effort task delete finished (HTTP %d)"), Code);
		});
	Request->ProcessRequest();
}
