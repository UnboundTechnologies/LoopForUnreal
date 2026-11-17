#include "Misc/AutomationTest.h"
#include "MeshGen/UbstreamMeshGenService.h"
#include "MeshGen/UbstreamMeshGenError.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamMeshGenApiTest,
	"Ubstream.Modeler.MeshGeneration.ApiV2", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUbstreamMeshGenApiTest::RunTest(const FString& Parameters)
{
	// Contract: the Unbound Zero /api/v2 task API. No network or credits used.
	FUbstreamMeshGenService Service;
	FUbstreamMeshGenRequest Request;
	Request.ImageBytes = {0x50, 0x00, 0x7f, 0x42};
	Request.Seed = 12345;
	TestEqual(TEXT("New requests default to 5,000 quads"), Request.Quads, 5000);
	TestTrue(TEXT("New requests include a normal map"), Request.bNormalMap);
	TestTrue(TEXT("Requests never poll faster than the API's 5 s floor"), Request.PollIntervalSeconds >= 5.0f);

	struct FCase { int32 Quads; bool bNormalMap; const TCHAR* SentQuads; const TCHAR* SentNormalMap; };
	const FCase Cases[] = {
		{5000, true, TEXT("5000"), TEXT("true")},
		{20000, false, TEXT("20000"), TEXT("false")},
		{50, true, TEXT("1000"), TEXT("true")},         // below the server's range
		{999999, true, TEXT("20000"), TEXT("true")},    // above it
	};
	for (const FCase& Case : Cases)
	{
		Request.Quads = Case.Quads;
		Request.bNormalMap = Case.bNormalMap;
		TArray<uint8> Body;
		FString ContentType, Error;
		if (!TestTrue(TEXT("Multipart request builds"), Service.BuildMultipartBody(Request, Body, ContentType, Error))) return false;
		// Keep binary bytes in Body for the byte comparison; avoid a NUL ending
		// string searches before the form fields that follow the image.
		TArray<uint8> ReadableBody = Body;
		for (uint8& Byte : ReadableBody) if (Byte == 0) Byte = '?';
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(ReadableBody.GetData()), ReadableBody.Num());
		const FString Multipart(Text.Length(), Text.Get());
		const FString Boundary = ContentType.RightChop(FString(TEXT("multipart/form-data; boundary=")).Len());
		TestTrue(TEXT("Opening boundary matches Content-Type"), Multipart.StartsWith(TEXT("--") + Boundary + TEXT("\r\n")));
		TestTrue(TEXT("Closing boundary matches Content-Type"), Multipart.EndsWith(TEXT("--") + Boundary + TEXT("--\r\n")));
		TestTrue(TEXT("Explicit Unbound Zero model"), Multipart.Contains(TEXT("name=\"model\"\r\n\r\nunbound-zero\r\n")));
		TestTrue(TEXT("PBR material requested"), Multipart.Contains(TEXT("name=\"material\"\r\n\r\npbr\r\n")));
		TestTrue(TEXT("Quads clamped to the server range"),
			Multipart.Contains(FString::Printf(TEXT("name=\"quads\"\r\n\r\n%s\r\n"), Case.SentQuads)));
		TestTrue(TEXT("Normal map flag sent"),
			Multipart.Contains(FString::Printf(TEXT("name=\"normal_map\"\r\n\r\n%s\r\n"), Case.SentNormalMap)));
		TestTrue(TEXT("Seed preserved"), Multipart.Contains(TEXT("name=\"seed\"\r\n\r\n12345\r\n")));
		TestFalse(TEXT("Retired fields are gone"),
			Multipart.Contains(TEXT("name=\"resolution\"")));
		const int32 HeaderEnd = Multipart.Find(TEXT("\r\n\r\n"));
		if (TestTrue(TEXT("Image has multipart headers"), HeaderEnd != INDEX_NONE))
		{
			TestTrue(TEXT("Binary image including NUL is unchanged"),
				FMemory::Memcmp(Body.GetData() + HeaderEnd + 4, Request.ImageBytes.GetData(), Request.ImageBytes.Num()) == 0);
		}
	}

	TestEqual(TEXT("Saved legacy production URL migrates"),
		UbstreamMeshGenApi::ResolveBaseUrl(TEXT(" https://unbound-3dgen-server-618775028388.us-central1.run.app/api/v1/ ")),
		FString(UbstreamMeshGenApi::DefaultBaseUrl));
	TestEqual(TEXT("Current production URL is stable"),
		UbstreamMeshGenApi::ResolveBaseUrl(UbstreamMeshGenApi::DefaultBaseUrl), FString(UbstreamMeshGenApi::DefaultBaseUrl));
	TestEqual(TEXT("Custom v1 server is not silently retargeted"),
		UbstreamMeshGenApi::ResolveBaseUrl(TEXT("https://custom.example/api/v1/")), FString(TEXT("https://custom.example/api/v1")));
	TestEqual(TEXT("Local development URL is preserved"),
		UbstreamMeshGenApi::ResolveBaseUrl(TEXT("http://localhost:8000/api/v2")), FString(TEXT("http://localhost:8000/api/v2")));
	Service.ActiveRequest.BaseUrl = UbstreamMeshGenApi::ResolveBaseUrl(TEXT("https://unbound-3dgen-server-618775028388.us-central1.run.app/api/v1"));
	TestEqual(TEXT("Submission targets v2 tasks"), Service.MakeTasksUrl(), FString(UbstreamMeshGenApi::DefaultBaseUrl) + TEXT("/tasks"));
	TestEqual(TEXT("Download remains a direct GLB endpoint"), Service.MakeDownloadUrl(TEXT("test-task")),
		FString(UbstreamMeshGenApi::DefaultBaseUrl) + TEXT("/tasks/test-task/download"));
	TestEqual(TEXT("Waiting download holds open for the documented 60 s"), Service.MakeWaitingDownloadUrl(TEXT("test-task")),
		FString(UbstreamMeshGenApi::DefaultBaseUrl) + TEXT("/tasks/test-task/download?wait=60"));
	Service.ActiveRequest.PollIntervalSeconds = 1.5f; // a value saved by earlier plugin versions
	TestEqual(TEXT("Saved fast poll intervals are raised to the API floor"), Service.GetPollIntervalSeconds(), 5.0f);

	TestTrue(TEXT("Zero's running status reads as generating"),
		MeshGenPhaseFromApiStatus(ParseUbstreamMeshGenApiStatus(TEXT("running"))) == EUbstreamMeshGenPhase::Reconstruction);
	TestTrue(TEXT("Unknown in-progress statuses do not read as queued"),
		MeshGenPhaseFromApiStatus(ParseUbstreamMeshGenApiStatus(TEXT("texturing"))) == EUbstreamMeshGenPhase::Reconstruction);
	TestTrue(TEXT("Queued still reads as queued"),
		MeshGenPhaseFromApiStatus(ParseUbstreamMeshGenApiStatus(TEXT("queued"))) == EUbstreamMeshGenPhase::Queued);

	const auto Credits = UbstreamMeshGenError::MakeHttpError(402, TEXT(""), EUbstreamMeshGenPhase::Uploading);
	TestTrue(TEXT("402 explains insufficient credits"), Credits.Kind == EUbstreamMeshGenErrorKind::InsufficientCredits);
	const auto CreditsDetail = UbstreamMeshGenError::MakeHttpError(402, TEXT("{\"detail\":\"Not enough credits: need 1, have 0.\"}"), EUbstreamMeshGenPhase::Uploading);
	TestEqual(TEXT("402 shows the server's balance detail"), CreditsDetail.Message.ToString(), FString(TEXT("Not enough credits: need 1, have 0.")));
	const auto MissingEndpoint = UbstreamMeshGenError::MakeHttpError(404, TEXT(""), EUbstreamMeshGenPhase::Uploading);
	const auto ExpiredTask = UbstreamMeshGenError::MakeHttpError(404, TEXT(""), EUbstreamMeshGenPhase::Reconstruction);
	TestFalse(TEXT("Endpoint failures are distinguished from expired tasks"), MissingEndpoint.Title.EqualTo(ExpiredTask.Title));
	return true;
}
#endif
