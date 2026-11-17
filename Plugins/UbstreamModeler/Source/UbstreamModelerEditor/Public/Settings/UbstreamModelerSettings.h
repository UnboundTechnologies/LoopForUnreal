#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MeshGen/UbstreamMeshGenOptions.h"

#include "UbstreamModelerSettings.generated.h"

UENUM()
enum class EUnboundLoopWebSocketProtocol : uint8
{
	FastStream UMETA(DisplayName="FastStream server"),
	UbstreamEnvelope UMETA(DisplayName="UBS1 simulator")
};

UCLASS(config=EditorPerProjectUserSettings, defaultconfig, meta=(DisplayName="Unbound Loop"))
class UBSTREAMMODELEREDITOR_API UUbstreamModelerSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UUbstreamModelerSettings();
	virtual void PostInitProperties() override;

	virtual FName GetCategoryName() const override;

#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif

	UPROPERTY(Config, EditAnywhere, Category="Workflow", meta=(DisplayName="WebSocket Protocol", ToolTip="Select the wire protocol used by the stream endpoint. FastStream is the production stream backend; UBS1 is the local simulator."))
	EUnboundLoopWebSocketProtocol WebSocketProtocol = EUnboundLoopWebSocketProtocol::FastStream;

	UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(DisplayName="Default WebSocket URL", ToolTip="Default endpoint used by the panel. Must start with ws:// or wss://."))
	FString DefaultWebSocketUrl = TEXT("wss://unbound-loop-2dgen-sdxl-server-618775028388.us-central1.run.app/api/ws/ue-editor");

	UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(DisplayName="Session ID", ToolTip="Stable session identifier included in legacy JSON messages and capture metadata."))
	FString SessionId = TEXT("editor_modeling_session");

	UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(DisplayName="Project ID", ToolTip="Optional project identifier sent to backends that require project scoping."))
	FString ProjectId;

	UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(DisplayName="Account Website", ToolTip="Unbound Loop account origin. Restart the editor after changing this. HTTPS required, except localhost:3000 for development."))
	FString AccountBaseUrl = TEXT("https://loop.unbound.io");

	UPROPERTY(Config, EditAnywhere, Category="Workflow", meta=(DisplayName="Auto Activate Modeling Mode", ToolTip="When enabled, opening the panel can move the editor into Unreal Modeling Mode automatically."))
	bool bAutoActivateModelingModeOnOpen = false;

	UPROPERTY(Config, EditAnywhere, Category="Workflow", meta=(DisplayName="Send Selection on Connect", ToolTip="In UBS1 simulator mode, send selected actor metadata when the WebSocket connects."))
	bool bSendSelectionStateOnConnect = true;

	UPROPERTY(Config, EditAnywhere, Category="Workflow", meta=(DisplayName="Auto Sync Isolated Subject", ToolTip="Automatically refresh the isolated subject clone when its source asset, component, or actor changes. The camera is preserved during automatic syncs."))
	bool bAutoResyncIsolatedSubject = true;

	UPROPERTY(Config, EditAnywhere, Category="Capture", meta=(DisplayName="Capture Width", ClampMin=64, ClampMax=4096, UIMin=256, UIMax=2048, ToolTip="Fallback capture width. FastStream Ready messages can override this with the server's required resolution."))
	int32 CaptureWidth = 512;

	UPROPERTY(Config, EditAnywhere, Category="Capture", meta=(DisplayName="Capture Height", ClampMin=64, ClampMax=4096, UIMin=256, UIMax=2048, ToolTip="Fallback capture height. FastStream Ready messages can override this with the server's required resolution."))
	int32 CaptureHeight = 512;

	UPROPERTY(Config, EditAnywhere, Category="Capture", meta=(DisplayName="JPEG Quality", ClampMin=1, ClampMax=100, UIMin=50, UIMax=95, ToolTip="Compression quality for RGB and depth JPEG frames. Lower values reduce latency and bandwidth."))
	int32 JpegQuality = 88;

	UPROPERTY(Config, EditAnywhere, Category="Capture", meta=(DisplayName="Save Debug Captures", ToolTip="Write captured and returned frames to Saved/ for inspection. Disable for fastest streaming."))
	bool bSaveDebugCaptures = false;

	UPROPERTY(Config, EditAnywhere, Category="Streaming", meta=(DisplayName="Stream Rate (fps)", ClampMin=0.2, ClampMax=15.0, UIMin=1.0, UIMax=15.0, ToolTip="Target live streaming rate. Server streaming is capped at 15 fps and still drops captures when backpressure is high."))
	float StreamFps = 15.0f;

	UPROPERTY(Config, EditAnywhere, Category="Streaming", meta=(DisplayName="Max In-Flight Frames", ClampMin=1, ClampMax=8, UIMin=1, UIMax=4, ToolTip="Maximum outstanding UBS1 frames. FastStream uses latest-wins delivery and does not wait for acknowledgements."))
	int32 MaxInFlightFrames = 1;

	UPROPERTY(Config, EditAnywhere, Category="Streaming", meta=(DisplayName="Max Frame Bundle (bytes)", ClampMin=1048576, ClampMax=134217728, UIMin=1048576, UIMax=67108864, ToolTip="Maximum frame payload size. The current FastStream backend cap is 10 MB."))
	int32 MaxFrameBundleBytes = 10 * 1024 * 1024;

	UPROPERTY(Config, EditAnywhere, Category="Streaming", meta=(DisplayName="Auto Start Stream on Connect", ToolTip="Start live streaming automatically after Modeling Mode and server Ready are active and the prompt is non-empty."))
	bool bAutoStartStreamOnConnect = true;

	UPROPERTY(Config, EditAnywhere, Category="Generation", meta=(DisplayName="Prompt Strength", ClampMin=0.0, ClampMax=1.0, UIMin=0.0, UIMax=1.0, ToolTip="How strongly the prompt steers the generated image. Sent to FastStream as strength."))
	float PromptStrength = 0.5f;

	UPROPERTY(Config, EditAnywhere, Category="Generation", meta=(DisplayName="Shape Strength", ClampMin=0.0, ClampMax=1.0, UIMin=0.0, UIMax=1.0, ToolTip="How strongly the generated image keeps the modeled shape. Sent to FastStream as the depth control strength."))
	float DepthStrength = 0.5f;

	UPROPERTY(Config, EditAnywhere, Category="Generation", meta=(DisplayName="Variation Seed", ClampMin=0, ClampMax=999999999, ToolTip="Seed used by the generation backend. Keep it fixed for a stable look while modeling; reroll in the panel to explore variations."))
	int32 GenerationSeed = 2;

	UPROPERTY(Config, EditAnywhere, Category="Mesh Generation", meta=(DisplayName="3D Gen Base URL", ToolTip="REST base URL for the Unbound Zero image-to-mesh task API, including /api/v2."))
	FString MeshGenBaseUrl = UbstreamMeshGenApi::DefaultBaseUrl;

	UPROPERTY(Config, EditAnywhere, Category="Mesh Generation", meta=(DisplayName="Target Quads", ClampMin=1000, ClampMax=20000, UIMin=1000, UIMax=20000, Delta=500, ToolTip="Target quad count of the generated mesh. The result lands near the target. The price does not depend on it."))
	int32 MeshGenQuads = UbstreamMeshGenApi::DefaultQuads;

	UPROPERTY(Config, EditAnywhere, Category="Mesh Generation", meta=(DisplayName="Normal Map", ToolTip="Include a normal map of the fine surface detail. Off makes the downloaded GLB about 1.4 MB smaller."))
	bool bMeshGenNormalMap = true;

	UPROPERTY(Config, EditAnywhere, Category="Mesh Generation", meta=(DisplayName="Status Check Interval (seconds)", ClampMin=5.0, ClampMax=60.0, UIMin=5.0, UIMax=30.0, ToolTip="Minimum time between task status checks. The plugin waits on the download itself, so this only matters for jobs longer than a minute. The service asks for at least 5 seconds."))
	float MeshGenPollIntervalSeconds = 5.0f;

	UPROPERTY(Config, EditAnywhere, Category="Mesh Generation", meta=(DisplayName="Stalled Job Timeout (minutes)", ClampMin=0, ToolTip="Stop waiting when the server stays in one phase this long. Phase changes reset the timer. Zero disables the timeout for very long jobs."))
	float MeshGenStallTimeoutMinutes = 30.0f;

	UPROPERTY(Config, EditAnywhere, Category="Mesh Generation", meta=(DisplayName="Import Directory", ToolTip="Content path where generated Static Mesh assets are created."))
	FString MeshGenImportDirectory = TEXT("/Game/Ubstream/Generated");

	UPROPERTY(Config, EditAnywhere, Category="Mesh Generation", meta=(DisplayName="Open Mesh Editor Preview", ToolTip="After a successful import, open the Static Mesh Editor so you can inspect the mesh without replacing the Unbound CSG stage."))
	bool bMeshGenOpenPreviewEditor = true;

	UPROPERTY(Config, EditAnywhere, Category="Mesh Generation", meta=(DisplayName="Auto Load Into Stage", ToolTip="After a successful import, also load the generated mesh into the Unbound CSG stage. Off by default so generation is preview-only."))
	bool bMeshGenAutoLoadIntoStage = false;
};
