#include "Capture/UbstreamCaptureSceneUtils.h"

#include "Capture/UbstreamCaptureTypes.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "StaticMeshCompiler.h"
#include "Viewport/UbstreamCleanViewportFlags.h"

namespace UbstreamCaptureSceneUtils
{
	void ConfigureCamera(USceneCaptureComponent2D& CaptureComponent, const FUbstreamCaptureCamera& Camera)
	{
		CaptureComponent.FOVAngle = Camera.FovDegrees;
		CaptureComponent.bOverride_CustomNearClippingPlane = Camera.NearClipPlane > 0.0f;
		if (Camera.NearClipPlane > 0.0f)
		{
			CaptureComponent.CustomNearClippingPlane = Camera.NearClipPlane;
		}
	}

	UTextureRenderTarget2D* CreateRenderTarget(int32 Width, int32 Height, EPixelFormat PixelFormat)
	{
		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
		RenderTarget->ClearColor = FLinearColor::Black;
		RenderTarget->InitCustomFormat(Width, Height, PixelFormat, false);
		RenderTarget->UpdateResourceImmediate(true);
		return RenderTarget;
	}

	void ConfigureCleanCapture(USceneCaptureComponent2D& CaptureComponent)
	{
		UbstreamCleanViewportFlags::Apply(CaptureComponent.ShowFlags);
	}

	void ConfigureFlatColorFallbackCapture(USceneCaptureComponent2D& CaptureComponent)
	{
		ConfigureCleanCapture(CaptureComponent);
		CaptureComponent.ShowFlags.SetLighting(false);
		CaptureComponent.ShowFlags.SetMaterials(false);
		CaptureComponent.ShowFlags.SetPostProcessing(false);
	}

	int32 ApplyShowOnlyComponents(USceneCaptureComponent2D& CaptureComponent, const TArray<UPrimitiveComponent*>& ShowOnlyComponents)
	{
		if (ShowOnlyComponents.IsEmpty())
		{
			return 0;
		}

		CaptureComponent.PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		int32 AppliedCount = 0;
		for (UPrimitiveComponent* Component : ShowOnlyComponents)
		{
			if (IsValid(Component))
			{
				CaptureComponent.ShowOnlyComponent(Component);
				++AppliedCount;
			}
		}
		return AppliedCount;
	}

	void ClearShowOnlyComponents(USceneCaptureComponent2D& CaptureComponent)
	{
		CaptureComponent.PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
		CaptureComponent.ClearShowOnlyComponents();
	}

	void FinishMeshCompilationForCapture(const TArray<UPrimitiveComponent*>& ShowOnlyComponents)
	{
		// While a static mesh is async-compiling (e.g. right after an OBJ
		// import), its component is refused a render state, so the subject has
		// no scene proxy and every capture pass - including the flat-color
		// fallback - renders black even though the editor viewport shows the
		// mesh moments later. Finish compilation before capturing; this also
		// recreates the component render states synchronously.
		TArray<UStaticMesh*> CompilingMeshes;
		for (UPrimitiveComponent* Component : ShowOnlyComponents)
		{
			if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
			{
				UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh();
				if (StaticMesh && StaticMesh->IsCompiling())
				{
					CompilingMeshes.AddUnique(StaticMesh);
				}
			}
		}
		if (!CompilingMeshes.IsEmpty())
		{
			FStaticMeshCompilingManager::Get().FinishCompilation(CompilingMeshes);
		}
	}
}
