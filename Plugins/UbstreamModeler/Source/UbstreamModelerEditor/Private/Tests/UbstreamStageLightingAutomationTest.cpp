#include "AdvancedPreviewScene.h"
#include "AssetViewerSettings.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Slate/SceneViewport.h"
#include "Tests/UbstreamWorkspaceTestUtils.h"
#include "Viewport/UbstreamViewportClient.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/SUbstreamModelerViewport.h"
#include "Widgets/SWindow.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUbstreamStageLightingTest, "Ubstream.Modeler.StageLighting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FUbstreamStageLightingTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("UWorld::DestroyActor: World has no context!"), EAutomationExpectedErrorFlags::Contains, -1);
	const auto Panel = SNew(SUbstreamModelerWidget);
	if (!TestTrue(TEXT("Offline workspace visible"), UbstreamWorkspaceTestUtils::ShowOfflineWorkspace(Panel.Get()))) return false;
	const auto Window = SNew(SWindow).ClientSize(FVector2D(1440, 900))[Panel];
	FSlateApplication::Get().AddWindow(Window);
	const auto Stage = Panel->GetSoloViewport();
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Cube available"), Cube) || !Panel->LoadSoloMesh(Cube))
	{
		Window->RequestDestroyWindow();
		return false;
	}
	Stage->SetStageLighting(FUbstreamStageLighting());
	TestTrue(TEXT("Default lighting follows the camera"), Stage->GetStageLighting().bFollowCamera);
	const uint64 SourceSignature = Stage->BuildSourceSignature();
	const uint64 DefaultSignature = Stage->BuildSceneSignature();
	const FPreviewSceneProfile ProfileBefore = *Stage->GetPreviewScene()->GetCurrentProfile();

	auto Pump = [&]()
	{
		for (int32 Frame = 0; Frame < 12; ++Frame)
		{
			FSlateApplication::Get().PumpMessages();
			FSlateApplication::Get().Tick();
			Stage->GetUbstreamViewportClient()->Tick(1.0f / 60.0f);
			Stage->GetSceneViewport()->Draw();
		}
	};
	Pump();
	FUbstreamCaptureSettings Capture;
	Capture.Width = 128;
	Capture.Height = 128;
	Capture.bCaptureDepth = false;
	const auto DefaultCapture = Stage->CaptureFrameBundle(701, Capture);
	TestTrue(TEXT("Default camera light captures successfully"), DefaultCapture.bSuccess);
	TestFalse(TEXT("Default camera light is lit, without flat fallback"), DefaultCapture.bUsedFlatColorFallback);

	const FString VisualDirectory = FPaths::ProjectSavedDir() / TEXT("LightingCheck");
	IFileManager::Get().MakeDirectory(*VisualDirectory, true);
	FFileHelper::SaveArrayToFile(DefaultCapture.ColorJpegBytes, *(VisualDirectory / TEXT("default-lighting.jpg")));

	FUbstreamStageLighting Dim = Stage->GetStageLighting();
	Dim.AmbientFill = 0.15f;
	Dim.MainStrength = 0.2f;
	Stage->SetStageLighting(Dim);
	TestTrue(TEXT("Lighting wakes unchanged-scene streaming"), DefaultSignature != Stage->BuildSceneSignature());
	Pump();
	const auto DimCapture = Stage->CaptureFrameBundle(702, Capture);
	TestTrue(TEXT("Dim lighting captures successfully"), DimCapture.bSuccess);
	TestFalse(TEXT("Dim lighting does not invoke flat fallback"), DimCapture.bUsedFlatColorFallback);
	TestTrue(TEXT("Light intensity changes captured pixels"), DefaultCapture.ColorSampleHash != DimCapture.ColorSampleHash);
	TestEqual(TEXT("Lighting never changes source geometry signature"), Stage->BuildSourceSignature(), SourceSignature);
	TestFalse(TEXT("Lighting does not create pending mesh edits"), Stage->HasUncommittedStageEdits());

	// The flush-free light setters must render exactly like the engine's
	// Pre/PostEditChange path (which re-registers the light).
	{
		FUbstreamStageLighting Fixed = Stage->GetStageLighting();
		Fixed.bFollowCamera = false;
		Fixed.Rotation = 70.0f;
		Fixed.Elevation = -35.0f;
		Fixed.MainStrength = 0.8f;
		Stage->SetStageLighting(Fixed);
		Pump();
		const auto Lightweight = Stage->CaptureFrameBundle(703, Capture);
		const FRotator Direction = Stage->GetPreviewScene()->GetLightDirection();
		Stage->SyncCaptureMirrorScene();
		for (FAdvancedPreviewScene* Scene : { &Stage->GetPreviewScene().Get(), Stage->CaptureMirrorScene.Get() })
		{
			if (!Scene) continue;
			Scene->SetLightDirection(Direction + FRotator(0.0f, 40.0f, 0.0f));
			Scene->SetLightDirection(Direction);
			Scene->SetLightBrightness(Scene->DirectionalLight->Intensity);
		}
		Pump();
		const auto EditChange = Stage->CaptureFrameBundle(704, Capture);
		Pump();
		const auto EditChangeAgain = Stage->CaptureFrameBundle(705, Capture);
		TestTrue(TEXT("Light equivalence captures succeed"), Lightweight.bSuccess && EditChange.bSuccess && EditChangeAgain.bSuccess);
		if (TestTrue(TEXT("Repeated capture of an unchanged scene is deterministic"), EditChangeAgain.ColorJpegBytes == EditChange.ColorJpegBytes))
		{
			TestEqual(TEXT("Flush-free light update renders identically to the edit-change path"), Lightweight.ColorSampleHash, EditChange.ColorSampleHash);
			TestTrue(TEXT("Flush-free light update produces byte-identical color output"), Lightweight.ColorJpegBytes == EditChange.ColorJpegBytes);
		}
		TestTrue(TEXT("Flush-free update sets the same direction"), Direction.Vector().Equals(FRotator(Fixed.Elevation, Fixed.Rotation, 0.0f).Vector(), 0.0001));
	}

	Stage->SetLightFollowsCamera(true);
	const auto Client = Stage->GetUbstreamViewportClient();
	Client->SetViewRotation(Client->GetViewRotation() + FRotator(12, 55, 0));
	Stage->UpdateStageLighting();
	const auto Settings = Stage->GetStageLighting();
	const FVector Expected = (Stage->BuildCaptureCamera(1, 1).Rotation.Quaternion()
		* FRotator(Settings.Elevation, Settings.Rotation, 0).Quaternion()).GetForwardVector();
	TestTrue(TEXT("Follow camera uses the rendered orbit pose"), Expected.Equals(Stage->GetPreviewScene()->GetLightDirection().Vector(), 0.0001));
	const uint64 FollowSignature = Stage->BuildSceneSignature();
	Stage->UpdateStageLighting();
	TestEqual(TEXT("Idle light update keeps scene signature stable"), Stage->BuildSceneSignature(), FollowSignature);
	const FVector FollowDirection = Stage->GetPreviewScene()->GetLightDirection().Vector();
	Stage->SetLightFollowsCamera(false);
	TestTrue(TEXT("Disabling follow freezes the current direction"), FollowDirection.Equals(Stage->GetPreviewScene()->GetLightDirection().Vector(), 0.0001));
	Client->SetViewRotation(Client->GetViewRotation() + FRotator(0, 35, 0));
	Stage->UpdateStageLighting();
	TestTrue(TEXT("Fixed light stays put when camera moves"), FollowDirection.Equals(Stage->GetPreviewScene()->GetLightDirection().Vector(), 0.0001));

	Stage->SyncCaptureMirrorScene();
	TestEqual(TEXT("Mirror gets ambient fill"), Stage->CaptureMirrorScene->SkyLight->Intensity, Stage->GetPreviewScene()->SkyLight->Intensity);
	TestEqual(TEXT("Mirror gets main strength"), Stage->CaptureMirrorScene->DirectionalLight->Intensity, Stage->GetPreviewScene()->DirectionalLight->Intensity);
	TestTrue(TEXT("Mirror gets light direction"), Stage->CaptureMirrorScene->GetLightDirection().Vector().Equals(FollowDirection, 0.0001));
	Dim.MainStrength = 0.9f;
	Dim.AmbientFill = 1.2f;
	Dim.Rotation = 135.0f;
	Stage->SetStageLighting(Dim);
	TestEqual(TEXT("Existing mirror follows subsequent adjustments"), Stage->CaptureMirrorScene->SkyLight->Intensity, Stage->GetPreviewScene()->SkyLight->Intensity);
	TestEqual(TEXT("Mirror gets main color"), Stage->CaptureMirrorScene->DirectionalLight->LightColor, Stage->GetPreviewScene()->DirectionalLight->LightColor);
	const uint64 NeutralSignature = Stage->BuildSceneSignature();
	Dim.MainColor = FLinearColor(1.0f, 0.5f, 0.2f);
	Dim.AmbientColor = FLinearColor(0.3f, 0.5f, 1.0f, 0.25f);
	Stage->SetStageLighting(Dim);
	TestTrue(TEXT("Light color wakes unchanged-scene streaming"), NeutralSignature != Stage->BuildSceneSignature());
	TestEqual(TEXT("Main color reaches the stage light"), Stage->GetPreviewScene()->DirectionalLight->LightColor, Dim.MainColor.ToFColor(true));
	TestEqual(TEXT("Ambient color reaches the stage sky light"), Stage->GetPreviewScene()->SkyLight->LightColor, FLinearColor(0.3f, 0.5f, 1.0f).ToFColor(true));
	TestEqual(TEXT("Mirror follows ambient color"), Stage->CaptureMirrorScene->SkyLight->LightColor, Stage->GetPreviewScene()->SkyLight->LightColor);
	TestEqual(TEXT("Tint alpha is dropped"), Stage->GetStageLighting().AmbientColor.A, 1.0f);
	Dim.MainColor = FLinearColor(4.0f, 1.0f, 1.0f);
	Stage->SetStageLighting(Dim);
	TestEqual(TEXT("HDR tints clamp to the unit range"), Stage->GetStageLighting().MainColor.R, 1.0f);
	const auto* ProfileAfter = Stage->GetPreviewScene()->GetCurrentProfile();
	TestEqual(TEXT("Asset viewer brightness is untouched"), ProfileAfter->DirectionalLightIntensity, ProfileBefore.DirectionalLightIntensity);
	TestTrue(TEXT("Asset viewer rotation is untouched"), ProfileAfter->DirectionalLightRotation.Equals(ProfileBefore.DirectionalLightRotation));

	// Recent picker colors: most recent first, deduplicated, capped at five.
	const TArray<FLinearColor> SavedRecents = Panel->RecentMainLightColors;
	Panel->RecentMainLightColors.Reset();
	for (int32 Index = 0; Index < 7; ++Index)
	{
		Panel->RememberLightColor(&SUbstreamModelerWidget::RecentMainLightColors, TEXT("RecentMainColors"), FLinearColor(Index / 10.0f, 0.5f, 0.5f));
	}
	TestEqual(TEXT("Recent colors are capped at five"), Panel->RecentMainLightColors.Num(), 5);
	Panel->RememberLightColor(&SUbstreamModelerWidget::RecentMainLightColors, TEXT("RecentMainColors"), FLinearColor(0.4f, 0.5f, 0.5f));
	TestEqual(TEXT("Re-picking a color does not duplicate it"), Panel->RecentMainLightColors.Num(), 5);
	TestEqual(TEXT("Re-picked color moves to the front"), Panel->RecentMainLightColors[0].ToFColor(true), FLinearColor(0.4f, 0.5f, 0.5f).ToFColor(true));
	Panel->RecentMainLightColors.Reset();
	for (int32 Index = SavedRecents.Num() - 1; Index >= 0; --Index)
	{
		Panel->RememberLightColor(&SUbstreamModelerWidget::RecentMainLightColors, TEXT("RecentMainColors"), SavedRecents[Index]);
	}
	if (SavedRecents.IsEmpty())
	{
		GConfig->RemoveKey(TEXT("UnboundLoop.StageLighting"), TEXT("RecentMainColors"), GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	// Render the actual header popup to catch layout/overflow regressions.
	Stage->SetStageLighting(FUbstreamStageLighting());
	Stage->FocusSoloMesh();
	Pump();
	TSharedPtr<SComboButton> LightsButton;
	TFunction<void(const TSharedRef<SWidget>&)> FindButton = [&](const TSharedRef<SWidget>& Widget)
	{
		if (Widget->GetType() == FName(TEXT("SComboButton")) && Widget->GetTag() == FName(TEXT("StageLights")))
		{
			LightsButton = StaticCastSharedRef<SComboButton>(Widget);
		}
		if (auto* Children = Widget->GetChildren())
			for (int32 Index = 0; Index < Children->Num(); ++Index) FindButton(Children->GetChildAt(Index));
	};
	FindButton(Panel);
	if (TestTrue(TEXT("Lights button is present in existing header"), LightsButton.IsValid()))
	{
		const auto Menu = Panel->BuildLightingMenu();
		const auto MenuWindow = SNew(SWindow).ClientSize(FVector2D(300, 560))[Menu];
		FSlateApplication::Get().AddWindow(MenuWindow);
		Pump();
		TArray<FColor> Pixels;
		FIntVector Size;
		if (TestTrue(TEXT("Lighting popup renders"), FSlateApplication::Get().TakeScreenshot(Menu, Pixels, Size)))
		{
			TArray64<uint8> Png;
			FImageUtils::CompressImage(Png, TEXT("png"), FImageView(Pixels.GetData(), Size.X, Size.Y));
			const FString Directory = FPaths::ProjectSavedDir() / TEXT("LightingCheck");
			IFileManager::Get().MakeDirectory(*Directory, true);
			FFileHelper::SaveArrayToFile(Png, *(Directory / TEXT("lights-popup.png")));
		}
		MenuWindow->RequestDestroyWindow();
	}
	Stage->ClearObjects();
	Window->RequestDestroyWindow();
	return true;
}
#endif
