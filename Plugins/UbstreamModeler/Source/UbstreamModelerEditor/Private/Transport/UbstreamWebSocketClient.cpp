#include "Transport/UbstreamWebSocketClient.h"
#include "Auth/UbstreamTransportSecurity.h"

#include "Async/Async.h"
#include "IWebSocket.h"
#include "WebSocketsModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogUbstreamWebSocketClient, Log, All);

FUbstreamWebSocketClient::FUbstreamWebSocketClient() = default;

FUbstreamWebSocketClient::~FUbstreamWebSocketClient()
{
	Disconnect();
}

void FUbstreamWebSocketClient::Connect(const FString& Url)
{
	Disconnect();

	CurrentUrl = Url.TrimStartAndEnd();
	if (!UbstreamTransportSecurity::IsAllowedUrl(CurrentUrl, true))
	{
		UE_LOG(LogUbstreamWebSocketClient, Warning, TEXT("Cannot connect: a secure WebSocket endpoint is required"));
		BroadcastConnectionErrorOnGameThread(TEXT("Use a wss:// endpoint. Plain ws:// is allowed only for localhost development."));
		return;
	}

	FragmentedBinaryMessage.Reset();

	UE_LOG(LogUbstreamWebSocketClient, Log, TEXT("Connecting to %s"), *CurrentUrl);

	FWebSocketsModule& WebSocketsModule = FWebSocketsModule::Get();
	Socket = WebSocketsModule.CreateWebSocket(CurrentUrl);
	BindSocketEvents();
	Socket->Connect();
}

void FUbstreamWebSocketClient::Disconnect()
{
	if (Socket.IsValid())
	{
		Socket->OnConnected().RemoveAll(this);
		Socket->OnConnectionError().RemoveAll(this);
		Socket->OnClosed().RemoveAll(this);
		Socket->OnMessage().RemoveAll(this);
		Socket->OnBinaryMessage().RemoveAll(this);

		// Close also cancels an in-progress handshake. The engine manager
		// owns another reference, so resetting ours alone cannot stop it.
		Socket->Close();
	}

	Socket.Reset();
	FragmentedBinaryMessage.Reset();
}

bool FUbstreamWebSocketClient::IsConnected() const
{
	return Socket.IsValid() && Socket->IsConnected();
}

bool FUbstreamWebSocketClient::SendJson(const FString& Json)
{
	if (!IsConnected())
	{
		UE_LOG(LogUbstreamWebSocketClient, Warning, TEXT("Cannot send JSON while disconnected"));
		return false;
	}

	Socket->Send(Json);
	return true;
}

bool FUbstreamWebSocketClient::SendBinary(const TArray<uint8>& Bytes)
{
	if (!IsConnected())
	{
		UE_LOG(LogUbstreamWebSocketClient, Warning, TEXT("Cannot send binary while disconnected"));
		return false;
	}

	if (Bytes.IsEmpty())
	{
		UE_LOG(LogUbstreamWebSocketClient, Warning, TEXT("Skipping empty binary payload"));
		return false;
	}

	Socket->Send(Bytes.GetData(), Bytes.Num(), true);
	return true;
}

void FUbstreamWebSocketClient::BindSocketEvents()
{
	check(Socket.IsValid());

	Socket->OnConnected().AddRaw(this, &FUbstreamWebSocketClient::HandleConnected);
	Socket->OnConnectionError().AddRaw(this, &FUbstreamWebSocketClient::HandleConnectionError);
	Socket->OnClosed().AddRaw(this, &FUbstreamWebSocketClient::HandleClosed);
	Socket->OnMessage().AddRaw(this, &FUbstreamWebSocketClient::HandleTextMessage);
	Socket->OnBinaryMessage().AddRaw(this, &FUbstreamWebSocketClient::HandleBinaryMessage);
}

void FUbstreamWebSocketClient::HandleConnected()
{
	UE_LOG(LogUbstreamWebSocketClient, Log, TEXT("Connected to %s"), *CurrentUrl);
	BroadcastConnectedOnGameThread();
}

void FUbstreamWebSocketClient::HandleConnectionError(const FString& Error)
{
	UE_LOG(LogUbstreamWebSocketClient, Warning, TEXT("Connection error: %s"), *Error);
	BroadcastConnectionErrorOnGameThread(Error);
}

void FUbstreamWebSocketClient::HandleClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	UE_LOG(LogUbstreamWebSocketClient, Log, TEXT("Closed: code=%d clean=%s reason=%s"), StatusCode, bWasClean ? TEXT("true") : TEXT("false"), *Reason);
	BroadcastClosedOnGameThread(StatusCode, Reason, bWasClean);
}

void FUbstreamWebSocketClient::HandleTextMessage(const FString& Message)
{
	UE_LOG(LogUbstreamWebSocketClient, Log, TEXT("Text message: %s"), *Message);
	BroadcastTextMessageOnGameThread(Message);
}

void FUbstreamWebSocketClient::HandleBinaryMessage(const void* Data, SIZE_T Size, bool bIsLastFragment)
{
	if (Size == 0 || Data == nullptr)
	{
		return;
	}

	const uint8* ByteData = static_cast<const uint8*>(Data);
	if (Size > static_cast<SIZE_T>(MAX_int32) || FragmentedBinaryMessage.Num() > MAX_int32 - static_cast<int32>(Size))
	{
		UE_LOG(LogUbstreamWebSocketClient, Warning, TEXT("Dropping oversized binary message fragment"));
		FragmentedBinaryMessage.Reset();
		return;
	}

	FragmentedBinaryMessage.Append(ByteData, static_cast<int32>(Size));

	if (bIsLastFragment)
	{
		TArray<uint8> CompletedMessage = MoveTemp(FragmentedBinaryMessage);
		FragmentedBinaryMessage.Reset();
		BroadcastBinaryMessageOnGameThread(MoveTemp(CompletedMessage));
	}
}

void FUbstreamWebSocketClient::BroadcastConnectedOnGameThread()
{
	if (IsInGameThread())
	{
		OnConnected.Broadcast();
		return;
	}

	const TWeakPtr<FUbstreamWebSocketClient> WeakThis = AsShared();
	AsyncTask(ENamedThreads::GameThread, [WeakThis]()
	{
		if (const TSharedPtr<FUbstreamWebSocketClient> Pinned = WeakThis.Pin())
		{
			Pinned->OnConnected.Broadcast();
		}
	});
}

void FUbstreamWebSocketClient::BroadcastConnectionErrorOnGameThread(const FString& Error)
{
	if (IsInGameThread())
	{
		OnConnectionError.Broadcast(Error);
		return;
	}

	const TWeakPtr<FUbstreamWebSocketClient> WeakThis = AsShared();
	AsyncTask(ENamedThreads::GameThread, [WeakThis, Error]()
	{
		if (const TSharedPtr<FUbstreamWebSocketClient> Pinned = WeakThis.Pin())
		{
			Pinned->OnConnectionError.Broadcast(Error);
		}
	});
}

void FUbstreamWebSocketClient::BroadcastClosedOnGameThread(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	if (IsInGameThread())
	{
		OnClosed.Broadcast(StatusCode, Reason, bWasClean);
		return;
	}

	const TWeakPtr<FUbstreamWebSocketClient> WeakThis = AsShared();
	AsyncTask(ENamedThreads::GameThread, [WeakThis, StatusCode, Reason, bWasClean]()
	{
		if (const TSharedPtr<FUbstreamWebSocketClient> Pinned = WeakThis.Pin())
		{
			Pinned->OnClosed.Broadcast(StatusCode, Reason, bWasClean);
		}
	});
}

void FUbstreamWebSocketClient::BroadcastTextMessageOnGameThread(const FString& Message)
{
	if (IsInGameThread())
	{
		OnJsonMessage.Broadcast(Message);
		return;
	}

	const TWeakPtr<FUbstreamWebSocketClient> WeakThis = AsShared();
	AsyncTask(ENamedThreads::GameThread, [WeakThis, Message]()
	{
		if (const TSharedPtr<FUbstreamWebSocketClient> Pinned = WeakThis.Pin())
		{
			Pinned->OnJsonMessage.Broadcast(Message);
		}
	});
}

void FUbstreamWebSocketClient::BroadcastBinaryMessageOnGameThread(TArray<uint8>&& Message)
{
	if (IsInGameThread())
	{
		OnBinaryMessage.Broadcast(Message);
		return;
	}

	const TWeakPtr<FUbstreamWebSocketClient> WeakThis = AsShared();
	AsyncTask(ENamedThreads::GameThread, [WeakThis, Message = MoveTemp(Message)]() mutable
	{
		if (const TSharedPtr<FUbstreamWebSocketClient> Pinned = WeakThis.Pin())
		{
			Pinned->OnBinaryMessage.Broadcast(Message);
		}
	});
}
