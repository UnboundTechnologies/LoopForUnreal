#include "MeshGen/UbstreamMeshGenError.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "UbstreamMeshGenError"

namespace
{
	FText PhaseContext(const EUbstreamMeshGenPhase Phase)
	{
		switch (Phase)
		{
		case EUbstreamMeshGenPhase::Uploading:
			return LOCTEXT("PhaseUpload", "while uploading the image");
		case EUbstreamMeshGenPhase::Queued:
		case EUbstreamMeshGenPhase::MaskGeneration:
		case EUbstreamMeshGenPhase::Reconstruction:
		case EUbstreamMeshGenPhase::Exporting:
			return LOCTEXT("PhaseServer", "while the server was processing");
		case EUbstreamMeshGenPhase::Downloading:
			return LOCTEXT("PhaseDownload", "while downloading the mesh");
		case EUbstreamMeshGenPhase::Importing:
			return LOCTEXT("PhaseImport", "while importing into the project");
		default:
			return LOCTEXT("PhaseGeneric", "during 3D generation");
		}
	}

	FString TrimDetail(FString Detail)
	{
		Detail.TrimStartAndEndInline();
		// Strip JSON-ish quotes if the whole body is a quoted string.
		if (Detail.Len() >= 2 && Detail.StartsWith(TEXT("\"")) && Detail.EndsWith(TEXT("\"")))
		{
			Detail = Detail.Mid(1, Detail.Len() - 2);
		}
		// Keep UI readable.
		if (Detail.Len() > 280)
		{
			Detail = Detail.Left(277) + TEXT("…");
		}
		return Detail;
	}
}

namespace UbstreamMeshGenError
{
	FString ExtractServerDetail(const FString& ResponseBody)
	{
		if (ResponseBody.IsEmpty())
		{
			return FString();
		}

		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
		if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
		{
			FString Detail;
			if (JsonObject->TryGetStringField(TEXT("detail"), Detail) && !Detail.IsEmpty())
			{
				return TrimDetail(Detail);
			}
			if (JsonObject->TryGetStringField(TEXT("error"), Detail) && !Detail.IsEmpty()
				&& !Detail.Equals(TEXT("null"), ESearchCase::IgnoreCase))
			{
				return TrimDetail(Detail);
			}
			if (JsonObject->TryGetStringField(TEXT("message"), Detail) && !Detail.IsEmpty())
			{
				return TrimDetail(Detail);
			}

			// FastAPI validation errors: detail is often an array of objects.
			const TArray<TSharedPtr<FJsonValue>>* DetailArray = nullptr;
			if (JsonObject->TryGetArrayField(TEXT("detail"), DetailArray) && DetailArray && DetailArray->Num() > 0)
			{
				TArray<FString> Parts;
				for (const TSharedPtr<FJsonValue>& Value : *DetailArray)
				{
					if (!Value.IsValid())
					{
						continue;
					}
					if (Value->Type == EJson::String)
					{
						Parts.Add(Value->AsString());
					}
					else if (Value->Type == EJson::Object)
					{
						const TSharedPtr<FJsonObject> Obj = Value->AsObject();
						FString Msg;
						if (Obj.IsValid() && Obj->TryGetStringField(TEXT("msg"), Msg) && !Msg.IsEmpty())
						{
							Parts.Add(Msg);
						}
					}
				}
				if (Parts.Num() > 0)
				{
					return TrimDetail(FString::Join(Parts, TEXT("; ")));
				}
			}
		}

		// Non-JSON body: use a short slice if it looks textual.
		const FString Trimmed = TrimDetail(ResponseBody);
		if (Trimmed.StartsWith(TEXT("<")) || Trimmed.StartsWith(TEXT("<!DOCTYPE")))
		{
			return FString();
		}
		return Trimmed;
	}

	FUbstreamMeshGenUserError MakeHttpError(
		const int32 HttpStatusCode,
		const FString& ResponseBody,
		const EUbstreamMeshGenPhase Phase,
		const FString& FallbackMessage)
	{
		FUbstreamMeshGenUserError Error;
		Error.HttpStatusCode = HttpStatusCode;
		Error.FailedPhase = Phase;
		Error.TechnicalDetail = FallbackMessage.IsEmpty()
			? FString::Printf(TEXT("HTTP %d: %s"), HttpStatusCode, *ResponseBody.Left(500))
			: FallbackMessage;

		const FString ServerDetail = ExtractServerDetail(ResponseBody);
		const FText Context = PhaseContext(Phase);

		if (HttpStatusCode == 401 || HttpStatusCode == 403)
		{
			Error.Kind = HttpStatusCode == 401 ? EUbstreamMeshGenErrorKind::Auth : EUbstreamMeshGenErrorKind::Forbidden;
			Error.Title = LOCTEXT("AuthTitle", "Authentication failed");
			Error.Message = ServerDetail.IsEmpty()
				? FText::Format(
					LOCTEXT("AuthBodyFmt", "The 3D generation server rejected the request {0}."),
					Context)
				: FText::FromString(ServerDetail);
			Error.Hint = LOCTEXT(
				"AuthHint",
				"Reconnect your Unbound account from the Settings menu and check that your account key is still valid.");
			return Error;
		}

		if (HttpStatusCode == 402)
		{
			Error.Kind = EUbstreamMeshGenErrorKind::InsufficientCredits;
			Error.Title = LOCTEXT("CreditsTitle", "Not enough credits");
			// The server's detail carries the balance and the price, when it sends one.
			Error.Message = ServerDetail.IsEmpty()
				? LOCTEXT("CreditsBody", "Your account does not have enough credits for this generation.")
				: FText::FromString(ServerDetail);
			Error.Hint = LOCTEXT("CreditsHint", "Add credits under Account > Plan & credits on the Unbound website, then try again.");
			return Error;
		}

		if (HttpStatusCode == 404)
		{
			Error.Kind = EUbstreamMeshGenErrorKind::NotFound;
			if (Phase == EUbstreamMeshGenPhase::Uploading)
			{
				Error.Title = LOCTEXT("EndpointNotFoundTitle", "3D service not found");
				Error.Message = LOCTEXT("EndpointNotFoundBody", "The configured server does not expose the 3D generation endpoint.");
				Error.Hint = LOCTEXT("EndpointNotFoundHint", "Check the 3D Gen Base URL in Project Settings → Plugins → Unbound Loop. The current service uses /api/v2.");
				return Error;
			}
			Error.Title = LOCTEXT("NotFoundTitle", "Task not found");
			Error.Message = ServerDetail.IsEmpty()
				? LOCTEXT("NotFoundBody", "The generation task is no longer available on the server.")
				: FText::FromString(ServerDetail);
			Error.Hint = LOCTEXT("NotFoundHint", "Start a new generation. Tasks may expire after the server TTL.");
			return Error;
		}

		if (HttpStatusCode == 409)
		{
			Error.Kind = EUbstreamMeshGenErrorKind::Busy;
			Error.Title = LOCTEXT("ConflictTitle", "Not ready yet");
			Error.Message = ServerDetail.IsEmpty()
				? LOCTEXT("ConflictBody", "The mesh is not ready to download yet.")
				: FText::FromString(ServerDetail);
			Error.Hint = LOCTEXT("ConflictHint", "Wait a moment and try again, or start a new generation.");
			return Error;
		}

		if (HttpStatusCode == 413)
		{
			Error.Kind = EUbstreamMeshGenErrorKind::PayloadTooLarge;
			Error.Title = LOCTEXT("TooLargeTitle", "Image too large");
			Error.Message = ServerDetail.IsEmpty()
				? LOCTEXT("TooLargeBody", "The source image exceeds the server upload limit.")
				: FText::FromString(ServerDetail);
			Error.Hint = LOCTEXT("TooLargeHint", "Use a smaller image (under 20 MB) or a lower-resolution preview frame.");
			return Error;
		}

		if (HttpStatusCode == 422)
		{
			Error.Kind = EUbstreamMeshGenErrorKind::Validation;
			Error.Title = LOCTEXT("ValidationTitle", "Invalid request");
			Error.Message = ServerDetail.IsEmpty()
				? LOCTEXT("ValidationBody", "The server could not accept the image or parameters.")
				: FText::FromString(ServerDetail);
			Error.Hint = LOCTEXT("ValidationHint", "Try a PNG, JPEG or WebP under 20 MB, and confirm the base URL includes /api/v2.");
			return Error;
		}

		if (HttpStatusCode >= 500 || HttpStatusCode == 0)
		{
			Error.Kind = EUbstreamMeshGenErrorKind::Server;
			Error.Title = LOCTEXT("ServerTitle", "Server error");
			Error.Message = ServerDetail.IsEmpty()
				? FText::Format(
					LOCTEXT("ServerBodyFmt", "The 3D generation service failed {0} (HTTP {1})."),
					Context,
					FText::AsNumber(HttpStatusCode))
				: FText::FromString(ServerDetail);
			Error.Hint = LOCTEXT("ServerHint", "Retry in a moment. If it keeps failing, check the mesh-gen service status.");
			return Error;
		}

		if (HttpStatusCode >= 400)
		{
			Error.Kind = EUbstreamMeshGenErrorKind::Unknown;
			Error.Title = LOCTEXT("HttpTitle", "Request failed");
			Error.Message = ServerDetail.IsEmpty()
				? FText::Format(
					LOCTEXT("HttpBodyFmt", "The server returned HTTP {0} {1}."),
					FText::AsNumber(HttpStatusCode),
					Context)
				: FText::FromString(ServerDetail);
			Error.Hint = LOCTEXT("HttpHint", "Retry, or review Mesh Generation settings and the server logs.");
			return Error;
		}

		return MakeGeneric(
			ServerDetail.IsEmpty() ? FallbackMessage : ServerDetail,
			Phase);
	}

	FUbstreamMeshGenUserError MakeNetworkError(const EUbstreamMeshGenPhase Phase, const FString& Detail)
	{
		FUbstreamMeshGenUserError Error;
		Error.Kind = EUbstreamMeshGenErrorKind::Network;
		Error.FailedPhase = Phase;
		Error.TechnicalDetail = Detail;
		Error.Title = LOCTEXT("NetworkTitle", "Connection problem");
		Error.Message = Detail.IsEmpty()
			? FText::Format(
				LOCTEXT("NetworkBodyFmt", "Could not reach the 3D generation server {0}."),
				PhaseContext(Phase))
			: FText::FromString(TrimDetail(Detail));
		Error.Hint = LOCTEXT(
			"NetworkHint",
			"Check your network and the Mesh Generation base URL in Project Settings.");
		return Error;
	}

	FUbstreamMeshGenUserError MakeImportError(const FString& Detail)
	{
		FUbstreamMeshGenUserError Error;
		Error.Kind = EUbstreamMeshGenErrorKind::Import;
		Error.FailedPhase = EUbstreamMeshGenPhase::Importing;
		Error.TechnicalDetail = Detail;
		Error.Title = LOCTEXT("ImportTitle", "Import failed");
		Error.Message = Detail.IsEmpty()
			? LOCTEXT("ImportBody", "The mesh downloaded but could not be imported into this project.")
			: FText::FromString(TrimDetail(Detail));
		Error.Hint = LOCTEXT(
			"ImportHint",
			"Confirm /Game/Ubstream/Generated is writable and the GLB is valid. Try again or import the file manually.");
		return Error;
	}

	FUbstreamMeshGenUserError MakeNoSourceError()
	{
		FUbstreamMeshGenUserError Error;
		Error.Kind = EUbstreamMeshGenErrorKind::NoSource;
		Error.Title = LOCTEXT("NoSourceTitle", "No source image");
		Error.Message = LOCTEXT(
			"NoSourceBody",
			"There is no live preview or snapshot image to send.");
		Error.Hint = LOCTEXT(
			"NoSourceHint",
			"Stream a frame or select a snapshot, then try Generate again.");
		return Error;
	}

	FUbstreamMeshGenUserError MakeCancelled()
	{
		FUbstreamMeshGenUserError Error;
		Error.Kind = EUbstreamMeshGenErrorKind::Cancelled;
		Error.Title = LOCTEXT("CancelledTitle", "Cancelled");
		Error.Message = LOCTEXT("CancelledBody", "3D generation was cancelled.");
		return Error;
	}

	FUbstreamMeshGenUserError MakeGeneric(const FString& Detail, const EUbstreamMeshGenPhase Phase)
	{
		FUbstreamMeshGenUserError Error;
		Error.Kind = EUbstreamMeshGenErrorKind::Unknown;
		Error.FailedPhase = Phase;
		Error.TechnicalDetail = Detail;
		Error.Title = LOCTEXT("GenericTitle", "3D generation failed");
		Error.Message = Detail.IsEmpty()
			? FText::Format(LOCTEXT("GenericBodyFmt", "Something went wrong {0}."), PhaseContext(Phase))
			: FText::FromString(TrimDetail(Detail));
		Error.Hint = LOCTEXT("GenericHint", "Retry the generation. If it persists, check the Output Log for details.");
		return Error;
	}

	FUbstreamMeshGenUserError FromResult(const FUbstreamMeshGenResult& Result)
	{
		if (Result.UserError.IsSet())
		{
			FUbstreamMeshGenUserError Error = Result.UserError;
			if (Error.Title.IsEmpty())
			{
				Error.Title = LOCTEXT("FallbackTitle", "3D generation failed");
			}
			if (Error.Message.IsEmpty())
			{
				Error.Message = Result.ErrorMessage.IsEmpty()
					? LOCTEXT("FallbackMessage", "Something went wrong.")
					: FText::FromString(Result.ErrorMessage);
			}
			return Error;
		}
		if (Result.ErrorMessage.Equals(TEXT("Cancelled"), ESearchCase::IgnoreCase))
		{
			return MakeCancelled();
		}
		if (Result.HttpStatusCode > 0)
		{
			return MakeHttpError(Result.HttpStatusCode, Result.ErrorMessage, Result.FailedPhase, Result.ErrorMessage);
		}
		if (Result.ErrorMessage.Contains(TEXT("network"), ESearchCase::IgnoreCase)
			|| Result.ErrorMessage.Contains(TEXT("HTTP create"), ESearchCase::IgnoreCase)
			|| Result.ErrorMessage.Contains(TEXT("Poll request failed"), ESearchCase::IgnoreCase)
			|| Result.ErrorMessage.Contains(TEXT("Download request failed"), ESearchCase::IgnoreCase))
		{
			return MakeNetworkError(Result.FailedPhase, Result.ErrorMessage);
		}
		if (Result.FailedPhase == EUbstreamMeshGenPhase::Importing
			|| Result.ErrorMessage.Contains(TEXT("import"), ESearchCase::IgnoreCase)
			|| Result.ErrorMessage.Contains(TEXT("Static Mesh"), ESearchCase::IgnoreCase))
		{
			return MakeImportError(Result.ErrorMessage);
		}
		return MakeGeneric(Result.ErrorMessage, Result.FailedPhase);
	}
}

#undef LOCTEXT_NAMESPACE
