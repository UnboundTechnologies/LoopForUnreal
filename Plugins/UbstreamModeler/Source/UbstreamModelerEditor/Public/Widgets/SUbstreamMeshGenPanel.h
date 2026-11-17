#pragma once

#include "CoreMinimal.h"
#include "MeshGen/UbstreamMeshGenTypes.h"
#include "Widgets/SCompoundWidget.h"

class FUbstreamMeshGenService;
class SProgressBar;
class STextBlock;
class SUbstreamModelerViewport;
class UStaticMesh;

/**
 * Minimal image → 3D mesh generation card. Parent supplies preview/snapshot
 * image bytes and the stage viewport for optional auto-load.
 */
class SUbstreamMeshGenPanel final : public SCompoundWidget
{
public:
	DECLARE_DELEGATE_RetVal(TArray<uint8>, FGetImageBytes);
	DECLARE_DELEGATE_RetVal(bool, FHasSourceImage);
	DECLARE_DELEGATE_RetVal(FString, FGetImageLabel);
	DECLARE_DELEGATE_RetVal(FString, FGetBlockedReason);

	SLATE_BEGIN_ARGS(SUbstreamMeshGenPanel) {}
		SLATE_ARGUMENT(TWeakPtr<SUbstreamModelerViewport>, SoloViewport)
		SLATE_EVENT(FGetImageBytes, OnGetSourceImageBytes)
		SLATE_EVENT(FHasSourceImage, OnHasSourceImage)
		SLATE_EVENT(FGetImageLabel, OnGetSourceImageLabel)
		SLATE_EVENT(FGetBlockedReason, OnGetBlockedReason)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SUbstreamMeshGenPanel() override;

	/** Kept for parent call sites; source chrome no longer renders a thumbnail. */
	void RefreshSourcePreview();

	/** Start generation from the current source (used by the Live Preview header). */
	bool RequestGenerate();
	bool CanRequestGenerate() const;
	bool IsGenerationBusy() const;

private:
	TSharedRef<SWidget> BuildContent();
	TSharedRef<SWidget> BuildErrorBanner();
	TSharedRef<SWidget> BuildSuccessBanner();
	FReply GenerateClicked();
	FReply CancelClicked();
	FReply ShowInContentBrowserClicked();
	FReply LoadInStageClicked();
	FReply RetryClicked();
	FReply DismissResultClicked();
	FReply OpenPreviewClicked();

	bool IsGenerateEnabled() const;
	bool IsCancelEnabled() const;
	bool IsBusy() const;
	bool HasSourceImage() const;
	bool HasResultMesh() const;
	bool HasDisplayError() const;
	EVisibility GetProgressVisibility() const;
	EVisibility GetResultVisibility() const;
	EVisibility GetErrorVisibility() const;
	EVisibility GetIdleHintVisibility() const;
	TOptional<float> GetProgressFraction() const;
	FText GetStatusText() const;
	FText GetIdleHintText() const;
	FText GetGenerateTooltipText() const;
	FText GetResultText() const;
	FText GetErrorTitleText() const;
	FText GetErrorMessageText() const;
	FText GetErrorHintText() const;
	EVisibility GetErrorHintVisibility() const;
	FText GetGenerateButtonText() const;

	void HandleProgress(const FUbstreamMeshGenProgress& InProgress);
	void HandleCompleted(const FUbstreamMeshGenResult& Result);
	void SetDisplayError(const FUbstreamMeshGenUserError& Error);
	void ClearDisplayError();
	void NotifyUser(const FText& Message, bool bIsError = false) const;
	void NotifyUserError(const FUbstreamMeshGenUserError& Error) const;
	bool ResolveSourceImage(TArray<uint8>& OutBytes, FString& OutFileName, FString& OutContentType) const;
	FUbstreamMeshGenRequest BuildRequest(const TArray<uint8>& ImageBytes, const FString& FileName, const FString& ContentType) const;
	UStaticMesh* ResolveResultMesh() const;
	bool OpenGeneratedMeshPreview(UStaticMesh* StaticMesh);
	bool LoadGeneratedMeshIntoStage(UStaticMesh* StaticMesh);

	TWeakPtr<SUbstreamModelerViewport> SoloViewport;
	FGetImageBytes OnGetSourceImageBytes;
	FHasSourceImage OnHasSourceImage;
	FGetImageLabel OnGetSourceImageLabel;
	FGetBlockedReason OnGetBlockedReason;

	TSharedPtr<FUbstreamMeshGenService> MeshGenService;
	TSharedPtr<SProgressBar> ProgressBar;
	TSharedPtr<STextBlock> StatusTextBlock;

	FUbstreamMeshGenProgress LastProgress;
	FUbstreamMeshGenResult LastResult;
	FUbstreamMeshGenUserError DisplayError;
	bool bShowResult = false;
};
