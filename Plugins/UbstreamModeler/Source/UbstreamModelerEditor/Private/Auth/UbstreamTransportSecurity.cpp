#include "Auth/UbstreamTransportSecurity.h"

bool UbstreamTransportSecurity::IsAllowedUrl(const FString& Url, bool bWebSocket)
{
	for (const TCHAR C : Url) if (C <= 32 || C == 127 || C == TEXT('\\')) return false;
	const FString Secure = bWebSocket ? TEXT("wss://") : TEXT("https://");
	const FString Local = bWebSocket ? TEXT("ws://") : TEXT("http://");
	const bool bSecure = Url.StartsWith(Secure, ESearchCase::CaseSensitive);
	if (!bSecure && !Url.StartsWith(Local, ESearchCase::CaseSensitive)) return false;
	const FString Rest = Url.Mid(bSecure ? Secure.Len() : Local.Len());
	int32 End = 0;
	while (End < Rest.Len() && Rest[End] != '/' && Rest[End] != '?' && Rest[End] != '#') ++End;
	const FString Authority = Rest.Left(End);
	if (Authority.IsEmpty() || Authority.Contains(TEXT("@"))) return false;
	if (bSecure) return true;
	FString Host = Authority, Port;
	if (Authority.StartsWith(TEXT("[::1]"), ESearchCase::CaseSensitive))
	{
		Host = TEXT("[::1]");
		if (Authority.Len() > 5)
		{
			if (Authority[5] != ':') return false;
			Port = Authority.Mid(6);
			if (Port.IsEmpty()) return false;
		}
	}
	else if (Authority.Split(TEXT(":"), &Host, &Port) && Port.IsEmpty()) return false;
	if (Host != TEXT("localhost") && Host != TEXT("127.0.0.1") && Host != TEXT("[::1]")) return false;
	if (!Port.IsEmpty())
	{
		if (Port.Len() > 5) return false;
		for (const TCHAR C : Port) if (C < '0' || C > '9') return false;
		const int32 Number = FCString::Atoi(*Port);
		if (Number < 1 || Number > 65535) return false;
	}
	return true;
}
