#pragma once

#include "CoreMinimal.h"
#include "Transport/IUbstreamTransport.h"

class IWebSocket;

DECLARE_MULTICAST_DELEGATE(FUbstreamWebSocketConnected);
DECLARE_MULTICAST_DELEGATE_OneParam(FUbstreamWebSocketStringEvent, const FString&);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FUbstreamWebSocketClosedEvent, int32, const FString&, bool);
DECLARE_MULTICAST_DELEGATE_OneParam(FUbstreamWebSocketBinaryEvent, const TArray<uint8>&);

class FUbstreamWebSocketClient final : public IUbstreamTransport, public TSharedFromThis<FUbstreamWebSocketClient>
{
public:
	FUbstreamWebSocketClient();
	virtual ~FUbstreamWebSocketClient() override;

	virtual void Connect(const FString& Url) override;
	virtual void Disconnect() override;
	virtual bool IsConnected() const override;
	virtual bool SendJson(const FString& Json) override;
	virtual bool SendBinary(const TArray<uint8>& Bytes) override;

	FUbstreamWebSocketConnected OnConnected;
	FUbstreamWebSocketStringEvent OnConnectionError;
	FUbstreamWebSocketClosedEvent OnClosed;
	FUbstreamWebSocketStringEvent OnJsonMessage;
	FUbstreamWebSocketBinaryEvent OnBinaryMessage;

private:
	friend class FUbstreamSocketLifecycleTest;
	void BindSocketEvents();
	void HandleConnected();
	void HandleConnectionError(const FString& Error);
	void HandleClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void HandleTextMessage(const FString& Message);
	void HandleBinaryMessage(const void* Data, SIZE_T Size, bool bIsLastFragment);
	void BroadcastConnectedOnGameThread();
	void BroadcastConnectionErrorOnGameThread(const FString& Error);
	void BroadcastClosedOnGameThread(int32 StatusCode, const FString& Reason, bool bWasClean);
	void BroadcastTextMessageOnGameThread(const FString& Message);
	void BroadcastBinaryMessageOnGameThread(TArray<uint8>&& Message);

	TSharedPtr<IWebSocket> Socket;
	TArray<uint8> FragmentedBinaryMessage;
	FString CurrentUrl;
};
