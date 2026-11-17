#include "Widgets/SUbstreamModelerViewport.h"

#include "AdvancedPreviewScene.h"
#include "Misc/MessageDialog.h"
#include "AssetViewerSettings.h"
#include "AssetEditorModeManager.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "BaseGizmos/AxisAngleGizmo.h"
#include "BaseGizmos/AxisPositionGizmo.h"
#include "BaseGizmos/CombinedTransformGizmo.h"
#include "BaseGizmos/GizmoActor.h"
#include "BaseGizmos/GizmoArrowComponent.h"
#include "BaseGizmos/GizmoBaseComponent.h"
#include "BaseGizmos/GizmoBoxComponent.h"
#include "BaseGizmos/GizmoCircleComponent.h"
#include "BaseGizmos/GizmoLineHandleComponent.h"
#include "BaseGizmos/GizmoRectangleComponent.h"
#include "BaseGizmos/HitTargets.h"
#include "BaseGizmos/PlanePositionGizmo.h"
#include "BaseGizmos/TransformGizmoUtil.h"
#include "BaseGizmos/TransformProxy.h"
#include "InteractiveGizmoManager.h"
#include "ToolContextInterfaces.h"
#include "HAL/IConsoleManager.h"
#include "Capture/UbstreamCaptureService.h"
#include "Changes/MeshReplacementChange.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/ActorComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Misc/ConfigCacheIni.h"
#include "Engine/TextureCube.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DragAndDrop/ActorDragDropOp.h"
#include "Editing/UbstreamStageEditorMode.h"
#include "Editing/UbstreamStageToolkitHost.h"
#include "MeshVertexSculptTool.h"
#include "Editing/UbstreamVertexSculptTool.h"
#include "Sculpting/MeshSculptToolBase.h"
#include "Sculpting/MeshBrushOpBase.h"
#include "Geometry/UbstreamRemeshJob.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Misc/ITransaction.h"
#include "UObject/UnrealType.h"
#include "UbstreamModelerEditorModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Geometry/UbstreamCSGSession.h"
#include "Geometry/UbstreamMeshOrientation.h"
#include "Toolkits/BaseToolkit.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "Tools/DefaultEdMode.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "DragAndDrop/CompositeDragDropOp.h"
#include "DragAndDrop/DecoratedDragDropOp.h"
#include "Dom/JsonObject.h"
#include "Drawing/LineSetComponent.h"
#include "Drawing/PointSetComponent.h"
#include "Drawing/TriangleSetComponent.h"
#include "Editor.h"
#include "Engine/EngineTypes.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Generators/MinimalBoxMeshGenerator.h"
#include "Generators/SphereGenerator.h"
#include "Generators/SweepGenerator.h"
#include "HAL/PlatformTime.h"
#include "Input/DragAndDrop.h"
#include "InteractiveTool.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "ConversionUtils/SceneComponentToDynamicMesh.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "DynamicMeshToMeshDescription.h"
#include "PreviewMesh.h"
#include "RenderingThread.h"
#include "ScopedTransaction.h"
#include "Selection.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Selections/MeshConnectedComponents.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "ToolSetupUtil.h"
#include "UDynamicMesh.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Utils/UbstreamMeshComponentUtils.h"
#include "Viewport/UbstreamViewportClient.h"

namespace
{
	const FName CSGPrimitiveTag(TEXT("ubstream_csg_primitive"));
	// The session brush renders as a translucent glass ghost. Neutral on
	// purpose: a strong tint over a large cutter reads as a solid blob, and
	// the yellow wireframe edges already carry the "this is the cutter" cue.
	const FLinearColor CSGSessionGhostColor(0.5f, 0.62f, 0.78f);
	constexpr float CSGSessionGhostOpacity = 0.13f;
	const FColor CSGPrimitiveColor(255, 126, 28, 255);
	const FColor SelectedCSGPrimitiveColor(255, 214, 64, 255);
	const FColor HoveredCSGPrimitiveColor(255, 168, 92, 255);
	const FLinearColor CSGPrimitiveWireColor(0.18f, 0.78f, 1.0f, 1.0f);
	const FLinearColor SelectedCSGPrimitiveWireColor(1.0f, 0.76f, 0.05f, 1.0f);
	const FLinearColor HoveredCSGPrimitiveWireColor(0.55f, 0.88f, 1.0f, 1.0f);

	// FAdvancedPreviewScene subscribes itself to the editor-wide asset-viewer
	// settings broadcast. Our stage/mirror scenes never change viewer profiles
	// at runtime, but the broadcast re-enters EVERY live scene - e.g. opening
	// the Static Mesh editor fires SetProfileIndex, which ran UpdateScene on
	// our scenes and SIGSEGV'd when one of them held GC'd internals (crash in
	// FAdvancedPreviewScene::UpdateScene via OnAssetViewerSettingsRefresh).
	// Opting out pins the constructed look and removes the re-entry entirely.
	void DetachSceneFromAssetViewerBroadcast(FAdvancedPreviewScene& Scene)
	{
		if (UAssetViewerSettings* ViewerSettings = UAssetViewerSettings::Get())
		{
			ViewerSettings->OnAssetViewerSettingsChanged().RemoveAll(&Scene);
		}
	}

	template <typename CallbackType>
	void RunWithPreviewWorldInactive(const TSharedPtr<FAdvancedPreviewScene>& PreviewScene, CallbackType&& Callback)
	{
		UWorld* PreviewWorld = PreviewScene.IsValid() ? PreviewScene->GetWorld() : nullptr;
		if (!PreviewWorld)
		{
			Callback();
			return;
		}

		const EWorldType::Type PreviousWorldType = PreviewWorld->WorldType;
		PreviewWorld->WorldType = EWorldType::Inactive;
		Callback();
		PreviewWorld->WorldType = PreviousWorldType;
	}

	bool IsFullyTranslucentMeshComponent(const UMeshComponent& Component);

	bool IsCSGPrimitiveComponent(const UMeshComponent* Component)
	{
		return IsValid(Component) && Component->ComponentTags.Contains(CSGPrimitiveTag);
	}

	bool IsEditorGizmoComponent(const UActorComponent* Component)
	{
		const AActor* Owner = IsValid(Component) ? Component->GetOwner() : nullptr;
		return Owner && Owner->IsA<AGizmoActor>();
	}

	// The classic line-drawn gizmo handles have a single highlight channel
	// (bHovering); the ITF input router ends hover the instant a drag capture
	// begins, and UCombinedTransformGizmo never wires the hit targets'
	// interacting callback for them. Stock result: the grabbed axis LOSES its
	// highlight for the whole drag and re-lights on release - reads as
	// blinking. Track hover and interaction separately and keep the handle lit
	// while either is set, so a dragged axis stays visibly active.
	void KeepGizmoHandleLitWhileDragged(UGizmoComponentHitTarget& HitTarget)
	{
		struct FHighlightFlags
		{
			bool bHovered = false;
			bool bInteracting = false;
		};
		const TSharedRef<FHighlightFlags> Flags = MakeShared<FHighlightFlags>();
		const auto ApplyHighlight =
			[Flags, WeakTarget = TWeakObjectPtr<UGizmoComponentHitTarget>(&HitTarget)]()
		{
			// Resolve the component at call time: entering Scale mode swaps the
			// hit target's component between the full-axis and combined handles.
			const UGizmoComponentHitTarget* Target = WeakTarget.Get();
			if (IGizmoBaseComponentInterface* Handle =
				Target ? Cast<IGizmoBaseComponentInterface>(Target->Component) : nullptr)
			{
				Handle->UpdateHoverState(Flags->bHovered || Flags->bInteracting);
			}
		};
		HitTarget.UpdateHoverFunction = [Flags, ApplyHighlight](const bool bHovering)
		{
			Flags->bHovered = bHovering;
			ApplyHighlight();
		};
		HitTarget.UpdateInteractingFunction = [Flags, ApplyHighlight](const bool bInteracting)
		{
			Flags->bInteracting = bInteracting;
			ApplyHighlight();
		};
	}

	// Content fingerprint from vertex/triangle counts plus a strided sample of
	// vertex positions. It runs every streamed frame per mirrored component,
	// so it must be O(samples), not O(V) - the full scan measurably heated
	// large-mesh sessions. Mesh edits here come from boolean commits and
	// restores, which virtually always change the counts; the sampled
	// positions cover the equal-count edge cases.
	uint64 ComputeMeshFingerprint(const UE::Geometry::FDynamicMesh3& Mesh)
	{
		uint64 Hash = (static_cast<uint64>(Mesh.VertexCount()) << 32) ^ static_cast<uint64>(Mesh.TriangleCount());
		constexpr int32 MaxSampledVertices = 64;
		const int32 Stride = FMath::Max(1, Mesh.MaxVertexID() / MaxSampledVertices);
		for (int32 VertexID = 0; VertexID < Mesh.MaxVertexID(); VertexID += Stride)
		{
			if (!Mesh.IsVertex(VertexID))
			{
				continue;
			}
			const FVector3d Position = Mesh.GetVertex(VertexID);
			Hash ^= static_cast<uint64>(FMath::RoundToInt64(Position.X * 256.0)) + 0x9e3779b97f4a7c15ull + (Hash << 6) + (Hash >> 2);
			Hash ^= static_cast<uint64>(FMath::RoundToInt64(Position.Y * 256.0)) + 0x9e3779b97f4a7c15ull + (Hash << 6) + (Hash >> 2);
			Hash ^= static_cast<uint64>(FMath::RoundToInt64(Position.Z * 256.0)) + 0x9e3779b97f4a7c15ull + (Hash << 6) + (Hash >> 2);
		}
		return Hash;
	}

	uint64 ComputeDynamicMeshFingerprint(const UBaseDynamicMeshComponent& Component)
	{
		uint64 Fingerprint = 0;
		Component.ProcessMesh([&Fingerprint](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			Fingerprint = ComputeMeshFingerprint(Mesh);
		});
		return Fingerprint;
	}

	EUbstreamCSGOp ToKernelCSGOp(const EUbstreamCSGOperation Operation)
	{
		switch (Operation)
		{
		case EUbstreamCSGOperation::Union:
			return EUbstreamCSGOp::Union;
		case EUbstreamCSGOperation::Intersect:
			return EUbstreamCSGOp::Intersect;
		case EUbstreamCSGOperation::Subtract:
		default:
			return EUbstreamCSGOp::Subtract;
		}
	}

	bool HidePreviewMeshObjectProperty(UObject& Owner, const FName PropertyName)
	{
		FObjectPropertyBase* ObjectProperty = FindFProperty<FObjectPropertyBase>(Owner.GetClass(), PropertyName);
		if (!ObjectProperty)
		{
			return false;
		}

		UObject* PreviewObject = ObjectProperty->GetObjectPropertyValue_InContainer(&Owner);
		UPreviewMesh* PreviewMesh = Cast<UPreviewMesh>(PreviewObject);
		if (!PreviewMesh)
		{
			return false;
		}

		PreviewMesh->SetVisible(false);
		if (UPrimitiveComponent* RootComponent = PreviewMesh->GetRootComponent())
		{
			RootComponent->SetVisibility(false, true);
		}
		return true;
	}

	FRotator GetStageCameraViewRotation(EUbstreamStageCameraView CameraView)
	{
		switch (CameraView)
		{
		case EUbstreamStageCameraView::Front:
			return FRotator(0.0f, -90.0f, 0.0f);
		case EUbstreamStageCameraView::Right:
			return FRotator(0.0f, 0.0f, 0.0f);
		case EUbstreamStageCameraView::Top:
			return FRotator(-89.0f, 0.0f, 0.0f);
		case EUbstreamStageCameraView::Iso:
		default:
			return FRotator(-24.0f, 45.0f, 0.0f);
		}
	}

	uint64 MixSceneSignature(uint64 Hash, uint64 Value)
	{
		Hash ^= Value + 0x9e3779b97f4a7c15ull + (Hash << 6) + (Hash >> 2);
		return Hash;
	}

	uint64 QuantizeSceneFloat(double Value)
	{
		return static_cast<uint64>(static_cast<int64>(FMath::RoundToDouble(Value * 100.0)));
	}

	uint64 MixSceneString(uint64 Hash, const FString& Value)
	{
		return MixSceneSignature(Hash, static_cast<uint64>(GetTypeHash(Value)));
	}

	uint64 MixSceneVector(uint64 Hash, const FVector& Value)
	{
		Hash = MixSceneSignature(Hash, QuantizeSceneFloat(Value.X));
		Hash = MixSceneSignature(Hash, QuantizeSceneFloat(Value.Y));
		Hash = MixSceneSignature(Hash, QuantizeSceneFloat(Value.Z));
		return Hash;
	}

	uint64 MixSceneTransform(uint64 Hash, const FTransform& Value)
	{
		Hash = MixSceneVector(Hash, Value.GetLocation());
		Hash = MixSceneVector(Hash, Value.GetRotation().Rotator().Euler());
		Hash = MixSceneVector(Hash, Value.GetScale3D());
		return Hash;
	}

	uint64 MixStaticMeshSource(uint64 Hash, const UStaticMesh* StaticMesh)
	{
		if (!IsValid(StaticMesh))
		{
			return MixSceneSignature(Hash, 0);
		}

		Hash = MixSceneString(Hash, StaticMesh->GetPathName());
		// Render-readiness: a mesh that is still async-compiling produces black
		// captures; once it finishes, the signature changes and the frame is
		// recaptured automatically.
		Hash = MixSceneSignature(Hash, StaticMesh->IsCompiling() ? 1 : 0);
		Hash = MixSceneSignature(Hash, StaticMesh->GetOutermost() && StaticMesh->GetOutermost()->IsDirty() ? 1 : 0);
		const FBoxSphereBounds Bounds = StaticMesh->GetBounds();
		Hash = MixSceneVector(Hash, Bounds.Origin);
		Hash = MixSceneVector(Hash, Bounds.BoxExtent);
		Hash = MixSceneSignature(Hash, QuantizeSceneFloat(Bounds.SphereRadius));
		Hash = MixSceneSignature(Hash, static_cast<uint64>(StaticMesh->GetNumLODs()));

		const TArray<FStaticMaterial>& StaticMaterials = StaticMesh->GetStaticMaterials();
		Hash = MixSceneSignature(Hash, static_cast<uint64>(StaticMaterials.Num()));
		for (const FStaticMaterial& StaticMaterial : StaticMaterials)
		{
			Hash = MixSceneString(Hash, StaticMaterial.MaterialInterface ? StaticMaterial.MaterialInterface->GetPathName() : FString());
			Hash = MixSceneString(Hash, StaticMaterial.MaterialSlotName.ToString());
		}

		return Hash;
	}

	uint64 MixMeshComponentSource(uint64 Hash, const UMeshComponent* Component)
	{
		if (!UbstreamMeshComponents::HasRenderableGeometry(Component))
		{
			return MixSceneSignature(Hash, 0);
		}

		Hash = MixSceneString(Hash, Component->GetPathName());
		Hash = MixSceneSignature(Hash, Component->SceneProxy != nullptr ? 1 : 0);
		Hash = MixSceneTransform(Hash, Component->GetComponentTransform());
		Hash = MixSceneVector(Hash, Component->Bounds.Origin);
		Hash = MixSceneVector(Hash, Component->Bounds.BoxExtent);
		Hash = MixSceneSignature(Hash, QuantizeSceneFloat(Component->Bounds.SphereRadius));

		const int32 MaterialCount = Component->GetNumMaterials();
		Hash = MixSceneSignature(Hash, static_cast<uint64>(MaterialCount));
		for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
		{
			const UMaterialInterface* Material = Component->GetMaterial(MaterialIndex);
			Hash = MixSceneString(Hash, Material ? Material->GetPathName() : FString());
		}

		if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
		{
			Hash = MixStaticMeshSource(Hash, StaticMeshComponent->GetStaticMesh());

			if (const UInstancedStaticMeshComponent* InstancedComponent = Cast<UInstancedStaticMeshComponent>(StaticMeshComponent))
			{
				const int32 InstanceCount = InstancedComponent->GetInstanceCount();
				Hash = MixSceneSignature(Hash, static_cast<uint64>(InstanceCount));
				for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; ++InstanceIndex)
				{
					FTransform InstanceTransform;
					if (InstancedComponent->GetInstanceTransform(InstanceIndex, InstanceTransform, true))
					{
						Hash = MixSceneTransform(Hash, InstanceTransform);
					}
				}
			}
		}
		else if (const UDynamicMeshComponent* DynamicMeshComponent = Cast<UDynamicMeshComponent>(Component))
		{
			if (const UE::Geometry::FDynamicMesh3* Mesh = DynamicMeshComponent->GetMesh())
			{
				Hash = MixSceneSignature(Hash, static_cast<uint64>(Mesh->TriangleCount()));
				Hash = MixSceneSignature(Hash, static_cast<uint64>(Mesh->VertexCount()));

				// Sample a deterministic subset of vertex positions so
				// same-topology deformations (sculpt strokes that leave the
				// bounds and counts unchanged) still change the signature.
				const int32 MaxVertexID = Mesh->MaxVertexID();
				if (MaxVertexID > 0)
				{
					uint32 State = 2166136261u ^ static_cast<uint32>(MaxVertexID * 73856093);
					const int32 SampleCount = FMath::Min(32, MaxVertexID);
					for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
					{
						State = State * 1664525u + 1013904223u;
						const int32 VertexID = static_cast<int32>(State % static_cast<uint32>(MaxVertexID));
						if (Mesh->IsVertex(VertexID))
						{
							Hash = MixSceneVector(Hash, FVector(Mesh->GetVertex(VertexID)));
						}
					}
				}
			}
		}

		return Hash;
	}

	TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& Vector)
	{
		return {
			MakeShared<FJsonValueNumber>(Vector.X),
			MakeShared<FJsonValueNumber>(Vector.Y),
			MakeShared<FJsonValueNumber>(Vector.Z)
		};
	}

	TArray<TSharedPtr<FJsonValue>> RotatorToJsonArray(const FRotator& Rotator)
	{
		// Protocol-wide convention: rotation arrays are [pitch, yaw, roll],
		// matching the frame-header camera serialization.
		return {
			MakeShared<FJsonValueNumber>(Rotator.Pitch),
			MakeShared<FJsonValueNumber>(Rotator.Yaw),
			MakeShared<FJsonValueNumber>(Rotator.Roll)
		};
	}

	FBoxSphereBounds GetReliableComponentBounds(const UMeshComponent& Component)
	{
		const FBoxSphereBounds CalculatedBounds = Component.CalcBounds(Component.GetComponentTransform());
		if (!CalculatedBounds.ContainsNaN() && CalculatedBounds.GetBox().IsValid)
		{
			return CalculatedBounds;
		}

		return Component.Bounds;
	}

	// Tight world-space geometry bounds. Imported assets often carry inflated
	// render bounds, which is exactly what cutter sizing must not inherit.
	FBox ComputeTightComponentBounds(const UMeshComponent& Component)
	{
		if (const UDynamicMeshComponent* DynamicComponent = Cast<UDynamicMeshComponent>(&Component))
		{
			bool bHasGeometry = false;
			UE::Geometry::FAxisAlignedBox3d MeshBounds;
			DynamicComponent->ProcessMesh([&bHasGeometry, &MeshBounds](const UE::Geometry::FDynamicMesh3& Mesh)
			{
				bHasGeometry = Mesh.VertexCount() > 0;
				MeshBounds = Mesh.GetBounds();
			});
			if (bHasGeometry)
			{
				return FBox(FVector(MeshBounds.Min), FVector(MeshBounds.Max))
					.TransformBy(Component.GetComponentTransform());
			}
		}
		else if (const UStaticMeshComponent* StaticComponent = Cast<UStaticMeshComponent>(&Component))
		{
			if (const UStaticMesh* StaticMesh = StaticComponent->GetStaticMesh())
			{
				return StaticMesh->GetBoundingBox().TransformBy(Component.GetComponentTransform());
			}
		}
		return Component.CalcBounds(Component.GetComponentTransform()).GetBox();
	}

	/**
	 * Bounds of the subject's main body: generated and imported meshes
	 * routinely carry stray fragments far from the visible geometry, and a raw
	 * bounding box over those makes anything sized from it (cutters!) absurdly
	 * large. The dominant connected component is what the user sees.
	 */
	FBox ComputeDominantGeometryBounds(const TArray<UMeshComponent*>& Components)
	{
		FBox DominantBounds(ForceInit);
		FBox FallbackBounds(ForceInit);
		for (const UMeshComponent* Component : Components)
		{
			if (!UbstreamMeshComponents::HasRenderableGeometry(Component))
			{
				continue;
			}
			FallbackBounds += ComputeTightComponentBounds(*Component);

			const UDynamicMeshComponent* DynamicComponent = Cast<UDynamicMeshComponent>(Component);
			if (!DynamicComponent)
			{
				continue;
			}
			const FTransform& ComponentTransform = Component->GetComponentTransform();
			DynamicComponent->ProcessMesh([&DominantBounds, &ComponentTransform](const UE::Geometry::FDynamicMesh3& Mesh)
			{
				if (Mesh.TriangleCount() == 0)
				{
					return;
				}
				UE::Geometry::FMeshConnectedComponents ConnectedComponents(&Mesh);
				ConnectedComponents.FindConnectedTriangles();
				const int32 LargestIndex = ConnectedComponents.GetLargestIndexByCount();
				if (LargestIndex < 0)
				{
					return;
				}
				for (const int32 TriangleId : ConnectedComponents.Components[LargestIndex].Indices)
				{
					const UE::Geometry::FIndex3i Triangle = Mesh.GetTriangle(TriangleId);
					for (int32 Corner = 0; Corner < 3; ++Corner)
					{
						DominantBounds += ComponentTransform.TransformPosition(FVector(Mesh.GetVertex(Triangle[Corner])));
					}
				}
			});
		}
		return DominantBounds.IsValid ? DominantBounds : FallbackBounds;
	}

	FBox BuildComponentsBounds(const TArray<UMeshComponent*>& Components)
	{
		FBox Bounds(ForceInit);
		for (const UMeshComponent* Component : Components)
		{
			if (UbstreamMeshComponents::HasRenderableGeometry(Component))
			{
				Bounds += GetReliableComponentBounds(*Component).GetBox();
			}
		}
		return Bounds;
	}

	FBox BuildPreviewBounds(const TArray<TObjectPtr<UMeshComponent>>& Components)
	{
		FBox Bounds(ForceInit);
		for (const UMeshComponent* Component : Components)
		{
			if (UbstreamMeshComponents::HasRenderableGeometry(Component))
			{
				Bounds += GetReliableComponentBounds(*Component).GetBox();
			}
		}
		return Bounds;
	}

	FString BuildActorSubjectLabel(const TArray<TWeakObjectPtr<AActor>>& Actors)
	{
		TArray<FString> Labels;
		for (const TWeakObjectPtr<AActor>& ActorPtr : Actors)
		{
			if (const AActor* Actor = ActorPtr.Get())
			{
				Labels.Add(Actor->GetActorLabel());
			}
		}

		if (Labels.Num() == 1)
		{
			return Labels[0];
		}

		return FString::Printf(TEXT("%d selected actors"), Labels.Num());
	}

	FString BuildActorSubjectPath(const TArray<TWeakObjectPtr<AActor>>& Actors)
	{
		TArray<FString> Paths;
		for (const TWeakObjectPtr<AActor>& ActorPtr : Actors)
		{
			if (const AActor* Actor = ActorPtr.Get())
			{
				Paths.Add(Actor->GetPathName());
			}
		}
		return FString::Join(Paths, TEXT(";"));
	}

	void AppendActorDragDropPayload(const FActorDragDropOp& ActorOp, TArray<TWeakObjectPtr<AActor>>& OutActors)
	{
		TSet<AActor*> AddedActors;
		for (const TWeakObjectPtr<AActor>& ActorPtr : OutActors)
		{
			if (AActor* Actor = ActorPtr.Get())
			{
				AddedActors.Add(Actor);
			}
		}

		for (const TWeakObjectPtr<AActor>& ActorPtr : ActorOp.Actors)
		{
			AActor* Actor = ActorPtr.Get();
			if (UbstreamMeshComponents::ActorHasRenderableMeshComponent(Actor) && !AddedActors.Contains(Actor))
			{
				OutActors.Add(Actor);
				AddedActors.Add(Actor);
			}
		}
	}

	void CopyCommonMeshComponentSettings(const UMeshComponent& SourceComponent, UMeshComponent& PreviewComponent)
	{
		// Modeling Mode's target input filter rejects Native-created components
		// without an owning property; Instance is the truthful classification
		// for stage clones and passes FComponentEditorUtils::CanEditComponentInstance.
		PreviewComponent.SetFlags(RF_Transactional);
		PreviewComponent.CreationMethod = EComponentCreationMethod::Instance;
		PreviewComponent.SetMobility(EComponentMobility::Movable);
		PreviewComponent.SetCollisionEnabled(ECollisionEnabled::NoCollision);
		PreviewComponent.SetCastShadow(SourceComponent.CastShadow);

		if (UDynamicMeshComponent* DynamicPreview = Cast<UDynamicMeshComponent>(&PreviewComponent))
		{
			if (UDynamicMesh* DynamicMesh = DynamicPreview->GetDynamicMesh())
			{
				DynamicMesh->SetFlags(RF_Transactional);
			}
		}

		for (int32 MaterialIndex = 0; MaterialIndex < SourceComponent.GetNumMaterials(); ++MaterialIndex)
		{
			PreviewComponent.SetMaterial(MaterialIndex, SourceComponent.GetMaterial(MaterialIndex));
		}
	}

	void CopyStaticMeshComponentVisuals(const UStaticMeshComponent& SourceComponent, UStaticMeshComponent& PreviewComponent)
	{
		PreviewComponent.SetStaticMesh(SourceComponent.GetStaticMesh());
		CopyCommonMeshComponentSettings(SourceComponent, PreviewComponent);
	}

	bool IsEngineStaticMeshAsset(const UStaticMesh* StaticMesh)
	{
		return IsValid(StaticMesh) && StaticMesh->GetPathName().StartsWith(TEXT("/Engine/"));
	}

	TArray<UMaterialInterface*> GetStaticMeshMaterials(const UStaticMesh& StaticMesh)
	{
		TArray<UMaterialInterface*> Materials;
		for (const FStaticMaterial& StaticMaterial : StaticMesh.GetStaticMaterials())
		{
			Materials.Add(StaticMaterial.MaterialInterface);
		}
		return Materials;
	}

	TArray<UMaterialInterface*> GetComponentMaterials(const UMeshComponent& Component)
	{
		TArray<UMaterialInterface*> Materials;
		for (int32 MaterialIndex = 0; MaterialIndex < Component.GetNumMaterials(); ++MaterialIndex)
		{
			Materials.Add(Component.GetMaterial(MaterialIndex));
		}
		return Materials;
	}

	// Mirrors the essential commit sequence from UStaticMeshToolTarget::CommitMeshDescription
	// (which is protected): transact the asset, rewrite its LOD0 source mesh,
	// and rebuild without auto-reduction replacing the result.
	bool CommitDynamicMeshToStaticMeshAsset(const UE::Geometry::FDynamicMesh3& EditedMesh, UStaticMesh* StaticMesh)
	{
		if (!IsValid(StaticMesh))
		{
			return false;
		}

		FlushRenderingCommands();
		StaticMesh->SetFlags(RF_Transactional);
		StaticMesh->Modify();

		if (StaticMesh->GetNumSourceModels() < 1)
		{
			StaticMesh->SetNumSourceModels(1);
		}

		FMeshDescription* MeshDescription = StaticMesh->GetMeshDescription(0);
		if (!MeshDescription)
		{
			MeshDescription = StaticMesh->CreateMeshDescription(0);
		}
		if (!MeshDescription)
		{
			return false;
		}

		StaticMesh->ModifyMeshDescription(0);

		FDynamicMeshToMeshDescription Converter;
		Converter.Convert(&EditedMesh, *MeshDescription, true);

		StaticMesh->CommitMeshDescription(0);
		StaticMesh->GetSourceModel(0).ResetReductionSetting();
		StaticMesh->PostEditChange();
		return true;
	}

	// Creates a new project Static Mesh asset from the edited stage mesh. Used
	// when the source asset cannot be modified (built-in engine content).
	UStaticMesh* CreateProjectAssetFromDynamicMesh(
		const UE::Geometry::FDynamicMesh3& EditedMesh,
		const FString& BaseName,
		const TArray<UMaterialInterface*>& Materials,
		const FString& Suffix = TEXT("Edited"))
	{
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		FString PackageName;
		FString AssetName;
		AssetToolsModule.Get().CreateUniqueAssetName(
			FString::Printf(TEXT("/Game/Ubstream/Meshes/%s_%s"), *BaseName, *Suffix), TEXT(""), PackageName, AssetName);

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return nullptr;
		}

		UStaticMesh* NewStaticMesh = NewObject<UStaticMesh>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		if (!NewStaticMesh)
		{
			return nullptr;
		}

		TArray<FStaticMaterial>& StaticMaterials = NewStaticMesh->GetStaticMaterials();
		for (UMaterialInterface* Material : Materials)
		{
			StaticMaterials.Add(FStaticMaterial(Material));
		}
		if (StaticMaterials.IsEmpty())
		{
			StaticMaterials.Add(FStaticMaterial());
		}

		if (!CommitDynamicMeshToStaticMeshAsset(EditedMesh, NewStaticMesh))
		{
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(NewStaticMesh);
		Package->MarkPackageDirty();
		return NewStaticMesh;
	}

	UStaticMesh* CreateProjectAssetCopyOfStaticMesh(
		UStaticMesh* SourceMesh,
		const TArray<UMaterialInterface*>& Materials)
	{
		if (!IsValid(SourceMesh))
		{
			return nullptr;
		}

		const FMeshDescription* MeshDescription = SourceMesh->GetMeshDescription(0);
		if (!MeshDescription)
		{
			return nullptr;
		}

		UE::Geometry::FDynamicMesh3 ConvertedMesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(MeshDescription, ConvertedMesh);

		TArray<UMaterialInterface*> CopyMaterials = Materials;
		if (CopyMaterials.IsEmpty())
		{
			CopyMaterials = GetStaticMeshMaterials(*SourceMesh);
		}
		return CreateProjectAssetFromDynamicMesh(ConvertedMesh, SourceMesh->GetName(), CopyMaterials, TEXT("Editable"));
	}

	UMeshComponent* CreatePreviewComponentClone(const UMeshComponent& SourceComponent)
	{
		if (const UStaticMeshComponent* StaticSource = Cast<UStaticMeshComponent>(&SourceComponent))
		{
			UStaticMeshComponent* PreviewComponent = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			CopyStaticMeshComponentVisuals(*StaticSource, *PreviewComponent);
			return PreviewComponent;
		}

		if (const UDynamicMeshComponent* DynamicSource = Cast<UDynamicMeshComponent>(&SourceComponent))
		{
			UDynamicMeshComponent* PreviewComponent = NewObject<UDynamicMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			if (const UE::Geometry::FDynamicMesh3* SourceMesh = DynamicSource->GetMesh())
			{
				PreviewComponent->SetMesh(UE::Geometry::FDynamicMesh3(*SourceMesh));
			}
			CopyCommonMeshComponentSettings(SourceComponent, *PreviewComponent);
			return PreviewComponent;
		}

		return nullptr;
	}
}

void SUbstreamModelerViewport::Construct(const FArguments& InArgs)
{
	OnConfirmDiscardStageEdits = InArgs._OnConfirmDiscardStageEdits;
	OnSoloMeshChanged = InArgs._OnSoloMeshChanged;
	OnEditingStateChanged = InArgs._OnEditingStateChanged;
	PreviewScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
	DetachSceneFromAssetViewerBroadcast(*PreviewScene);
	PreviewScene->SetFloorVisibility(false, true);
	// Keep the studio backdrop in the stage only. Ambient illumination is
	// configured separately and shared with the clean capture mirror.
	PreviewScene->SetEnvironmentVisibility(true, true);
	GConfig->GetFloat(TEXT("UnboundLoop.StageLighting"), TEXT("AmbientFill"), StageLighting.AmbientFill, GEditorPerProjectIni);
	GConfig->GetFloat(TEXT("UnboundLoop.StageLighting"), TEXT("MainStrength"), StageLighting.MainStrength, GEditorPerProjectIni);
	GConfig->GetFloat(TEXT("UnboundLoop.StageLighting"), TEXT("Rotation"), StageLighting.Rotation, GEditorPerProjectIni);
	GConfig->GetFloat(TEXT("UnboundLoop.StageLighting"), TEXT("Elevation"), StageLighting.Elevation, GEditorPerProjectIni);
	GConfig->GetBool(TEXT("UnboundLoop.StageLighting"), TEXT("FollowCamera"), StageLighting.bFollowCamera, GEditorPerProjectIni);
	const auto LoadLightColor = [](const TCHAR* Key, FLinearColor& OutColor)
	{
		FString ColorString;
		FLinearColor Parsed;
		if (GConfig->GetString(TEXT("UnboundLoop.StageLighting"), Key, ColorString, GEditorPerProjectIni) && Parsed.InitFromString(ColorString))
		{
			OutColor = Parsed;
		}
	};
	LoadLightColor(TEXT("MainColor"), StageLighting.MainColor);
	LoadLightColor(TEXT("AmbientColor"), StageLighting.AmbientColor);
	InitializeSceneLighting(*PreviewScene);
	SetStageLighting(StageLighting);

	StageModeManager = MakeShared<FAssetEditorModeManager>();
	StageModeManager->SetPreviewScene(PreviewScene.Get());
	StageModeManager->SetDefaultMode(FAssetEdModes::EM_AssetDefault);
	// Start with the translate gizmo like the Level Editor, so the first
	// element selection immediately shows a usable widget.
	// The mode manager must NOT spawn its own selection gizmo: the stage
	// creates exactly one transform gizmo itself, and the editor's default
	// selection gizmo was rendering as a second set of (arrow) axes.
	StageModeManager->SetShowWidget(false);
	StageModeManager->SetWidgetMode(UE::Widget::WM_Translate);

	SEditorViewport::Construct(SEditorViewport::FArguments());

	if (ViewportClient.IsValid())
	{
		ViewportClient->SetStageViewport(SharedThis(this));
	}

	StageToolkitHost = MakeShared<FUbstreamStageToolkitHost>();
	StageToolkitHost->Initialize(StageModeManager.Get(), PreviewScene.Get(), SharedThis(this));
	StageToolkitHost->OnHostedToolkitChanged.AddSP(this, &SUbstreamModelerViewport::HandleHostedToolkitChanged);
	StageToolkitHost->OnOverlayWidgetAdded.AddSP(this, &SUbstreamModelerViewport::HandleHostOverlayAdded);
	StageToolkitHost->OnOverlayWidgetRemoved.AddSP(this, &SUbstreamModelerViewport::HandleHostOverlayRemoved);
	StageModeManager->SetToolkitHost(StageToolkitHost.ToSharedRef());
	StageModeManager->ActivateDefaultMode();

	// Undo/redo can restore attribute-only state (UVs, normals, colors) that
	// the position fingerprint cannot see; force a full mirror resync after
	// every transaction so the stream always matches the model.
	PostUndoRedoHandle = FEditorDelegates::PostUndoRedo.AddSP(
		this, &SUbstreamModelerViewport::HandlePostUndoRedo);
}

void SUbstreamModelerViewport::InitializeSceneLighting(FAdvancedPreviewScene& Scene)
{
	// Reuse the scene's skylight: one ambient source, with identical fill in
	// the visible stage and the clean capture mirror. Backdrop stays independent.
	if (UTextureCube* Cubemap = LoadObject<UTextureCube>(nullptr, TEXT("/Engine/MapTemplates/Sky/DaylightAmbientCubemap.DaylightAmbientCubemap")))
	{
		Scene.SetSkyCubemap(Cubemap);
	}
	Scene.SkyLight->bLowerHemisphereIsBlack = false;
	Scene.SkyLight->SetCastShadows(false);
	Scene.SkyLight->SetVisibility(true);
	Scene.SkyLight->SetCaptureIsDirty();
	Scene.SetLightColor(FColor::White);
}

void SUbstreamModelerViewport::SetStageLighting(const FUbstreamStageLighting& Settings)
{
	const FUbstreamStageLighting Defaults;
	StageLighting = Settings;
	StageLighting.AmbientFill = FMath::IsFinite(Settings.AmbientFill) ? FMath::Clamp(Settings.AmbientFill, 0.0f, 2.0f) : Defaults.AmbientFill;
	StageLighting.MainStrength = FMath::IsFinite(Settings.MainStrength) ? FMath::Clamp(Settings.MainStrength, 0.0f, 2.0f) : Defaults.MainStrength;
	StageLighting.Rotation = FMath::IsFinite(Settings.Rotation) ? FRotator::ClampAxis(Settings.Rotation) : Defaults.Rotation;
	StageLighting.Elevation = FMath::IsFinite(Settings.Elevation) ? FMath::Clamp(Settings.Elevation, -89.0f, 89.0f) : Defaults.Elevation;
	// Tints only: intensity stays on the strength sliders, so drop alpha and
	// clamp to [0,1] (a picker in HDR mode could otherwise double-brighten).
	const auto SanitizeTint = [](const FLinearColor& Color, const FLinearColor& Fallback)
	{
		if (!FMath::IsFinite(Color.R) || !FMath::IsFinite(Color.G) || !FMath::IsFinite(Color.B)) return Fallback;
		FLinearColor Tint = Color.GetClamped(0.0f, 1.0f);
		Tint.A = 1.0f;
		return Tint;
	};
	StageLighting.MainColor = SanitizeTint(Settings.MainColor, Defaults.MainColor);
	StageLighting.AmbientColor = SanitizeTint(Settings.AmbientColor, Defaults.AmbientColor);
	UpdateStageLighting();
	if (ViewportClient.IsValid()) ViewportClient->Invalidate();
}

void SUbstreamModelerViewport::SetLightFollowsCamera(bool bFollow)
{
	if (StageLighting.bFollowCamera == bFollow) return;
	FUbstreamStageLighting Settings = StageLighting;
	if (bFollow)
	{
		// A useful headlight starts slightly above and beside the view axis.
		Settings.Rotation = 25.0f;
		Settings.Elevation = -20.0f;
	}
	else
	{
		// Switching to a fixed light freezes its current world direction.
		const FRotator Direction = (BuildCaptureCamera(1, 1).Rotation.Quaternion()
			* FRotator(Settings.Elevation, Settings.Rotation, 0.0f).Quaternion()).Rotator();
		Settings.Rotation = Direction.Yaw;
		Settings.Elevation = Direction.Pitch;
	}
	Settings.bFollowCamera = bFollow;
	SetStageLighting(Settings);
}

void SUbstreamModelerViewport::SaveStageLighting() const
{
	const TCHAR* Section = TEXT("UnboundLoop.StageLighting");
	GConfig->SetFloat(Section, TEXT("AmbientFill"), StageLighting.AmbientFill, GEditorPerProjectIni);
	GConfig->SetFloat(Section, TEXT("MainStrength"), StageLighting.MainStrength, GEditorPerProjectIni);
	GConfig->SetFloat(Section, TEXT("Rotation"), StageLighting.Rotation, GEditorPerProjectIni);
	GConfig->SetFloat(Section, TEXT("Elevation"), StageLighting.Elevation, GEditorPerProjectIni);
	GConfig->SetBool(Section, TEXT("FollowCamera"), StageLighting.bFollowCamera, GEditorPerProjectIni);
	GConfig->SetString(Section, TEXT("MainColor"), *StageLighting.MainColor.ToString(), GEditorPerProjectIni);
	GConfig->SetString(Section, TEXT("AmbientColor"), *StageLighting.AmbientColor.ToString(), GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

void SUbstreamModelerViewport::ApplySceneLighting(FAdvancedPreviewScene& Scene, const FRotator& Direction)
{
	// FPreviewScene's SetLightDirection/SetLightBrightness/SetLightColor wrap
	// each change in Pre/PostEditChange, which flushes rendering commands (a
	// full GPU sync) and re-registers the light. Follow-camera lighting changes
	// direction every tick while orbiting, so drive the component's lightweight
	// runtime setters instead: identical light state, no flush.
	UDirectionalLightComponent* Light = Scene.DirectionalLight;
	if (Light && !Scene.GetLightDirection().Vector().Equals(Direction.Vector(), 0.00001f))
	{
		if (!Light->IsUsingAbsoluteLocation() || !Light->IsUsingAbsoluteRotation() || !Light->IsUsingAbsoluteScale())
		{
			Light->SetAbsolute(true, true, true);
		}
		Light->SetRelativeRotation(Direction);
	}
	const float MainIntensity = StageLighting.MainStrength * 5.5f;
	const float AmbientIntensity = StageLighting.AmbientFill * 1.4f;
	if (Light && !FMath::IsNearlyEqual(Light->Intensity, MainIntensity)) Light->SetIntensity(MainIntensity);
	if (!FMath::IsNearlyEqual(Scene.SkyLight->Intensity, AmbientIntensity)) Scene.SetSkyBrightness(AmbientIntensity);
	// Compare quantized colors so the per-tick camera-follow path does not
	// dirty light render state when nothing changed.
	const FColor MainColor = StageLighting.MainColor.ToFColor(true);
	const FColor AmbientColor = StageLighting.AmbientColor.ToFColor(true);
	if (Light && Light->LightColor != MainColor) Light->SetLightFColor(MainColor);
	if (Scene.SkyLight->LightColor != AmbientColor) Scene.SkyLight->SetLightColor(StageLighting.AmbientColor);
}

void SUbstreamModelerViewport::UpdateStageLighting()
{
	FRotator Direction(StageLighting.Elevation, StageLighting.Rotation, 0.0f);
	if (StageLighting.bFollowCamera && ViewportClient.IsValid())
	{
		Direction = (BuildCaptureCamera(1, 1).Rotation.Quaternion() * Direction.Quaternion()).Rotator();
	}
	if (PreviewScene.IsValid()) ApplySceneLighting(*PreviewScene, Direction);
	if (CaptureMirrorScene.IsValid()) ApplySceneLighting(*CaptureMirrorScene, Direction);
}

SUbstreamModelerViewport::~SUbstreamModelerViewport()
{
	AbandonRemesh();
	UWorld* TeardownWorld = PreviewScene.IsValid() ? PreviewScene->GetWorld() : nullptr;
	const EWorldType::Type PreviousWorldType = TeardownWorld
		? TeardownWorld->WorldType.GetValue()
		: EWorldType::None;
	if (TeardownWorld)
	{
		TeardownWorld->WorldType = EWorldType::Inactive;
	}

	if (PostUndoRedoHandle.IsValid())
	{
		FEditorDelegates::PostUndoRedo.Remove(PostUndoRedoHandle);
		PostUndoRedoHandle.Reset();
	}
	OnEditingStateChanged.Unbind();
	OnSoloMeshChanged.Unbind();
	RunWithPreviewWorldInactive(PreviewScene, [this]()
	{
		ExitEditMode();
	});
	ResetCaptureMirrorScene();
	ReleaseCaptureComponent();

	// The viewport client co-owns the mode manager and is destroyed after our
	// members; clear the manager's raw preview-scene pointer so nothing can
	// touch it during late teardown.
	if (StageModeManager.IsValid())
	{
		StageModeManager->SetPreviewScene(nullptr);
	}
	StageModeManager.Reset();

	if (TeardownWorld)
	{
		TeardownWorld->WorldType = PreviousWorldType;
	}
}

bool SUbstreamModelerViewport::EnterEditMode()
{
	if (bEditModeActive)
	{
		return true;
	}

	if (!StageModeManager.IsValid() || !HasSoloMesh())
	{
		return false;
	}

	FString EditableSourceError;
	if (!EnsureEngineAssetSourcesAreEditable(EditableSourceError))
	{
		return false;
	}

	if (!ConvertStaticPreviewsToEditable())
	{
		return false;
	}

	if (!SelectPreviewComponentsForEditing())
	{
		return false;
	}
	// Entry validated the editable subject via selection; now start clean.
	// The gizmo appears when a cutter (or an explicitly clicked component)
	// is selected - never as ambient chrome.
	DeselectAllStageComponents();

	// Editing runs entirely on the lean stage mode: it supplies the selection
	// and tools context the CSG workflow needs without pulling the Modeling
	// Mode palette into the stage panel.
	StageModeManager->ActivateMode(UUbstreamStageEditorMode::EM_UbstreamStageEditorModeId);
	if (!StageModeManager->IsModeActive(UUbstreamStageEditorMode::EM_UbstreamStageEditorModeId))
	{
		StageModeManager->GetSelectedComponents()->DeselectAll();
		return false;
	}

	if (UUbstreamStageEditorMode* Mode = GetStageEditorMode())
	{
		ToolStateChangedHandle = Mode->OnToolStateChanged.AddSP(
			this, &SUbstreamModelerViewport::HandleModeToolStateChanged);
	}

	bEditModeActive = true;
	FocusStageViewport();
	UpdateStageTransformGizmo();
	UpdateStageRealtime();
	RefreshToolPreviewVisibility();

	OnEditingStateChanged.ExecuteIfBound();
	return true;
}

void SUbstreamModelerViewport::ExitEditMode()
{
	AbandonRemesh();
	if (!bEditModeActive)
	{
		return;
	}

	// Ensure cutters are visible when leaving the stage.
	SetCSGPrimitiveComponentsHidden(false);

	CancelCSGSession();
	DestroyStageTransformGizmo();

	if (UUbstreamStageEditorMode* Mode = GetStageEditorMode())
	{
		RunWithPreviewWorldInactive(PreviewScene, [Mode]()
		{
			Mode->EndActiveTool(/*bAccept*/ false);
		});
		if (ToolStateChangedHandle.IsValid())
		{
			Mode->OnToolStateChanged.Remove(ToolStateChangedHandle);
		}
	}
	ToolStateChangedHandle.Reset();

	if (StageModeManager.IsValid())
	{
		RunWithPreviewWorldInactive(PreviewScene, [this]()
		{
			StageModeManager->DeactivateMode(UUbstreamStageEditorMode::EM_UbstreamStageEditorModeId);
		});
		if (USelection* SelectedComponents = StageModeManager->GetSelectedComponents())
		{
			SelectedComponents->DeselectAll();
		}
	}

	SetStagePreviewComponentsSuppressed(false);
	bEditModeActive = false;
	ResetCaptureMirrorScene();
	// Keep realtime while a subject is still loaded so skylight / first-frame
	// composites settle (non-realtime left a ghost overlay until mouse move).
	UpdateStageRealtime();

	OnEditingStateChanged.ExecuteIfBound();
}

bool SUbstreamModelerViewport::HasActiveEditTool() const
{
	UUbstreamStageEditorMode* Mode = GetStageEditorMode();
	return Mode && Mode->HasActiveTool();
}

bool SUbstreamModelerViewport::HasAnyActiveStageTool() const
{
	return HasActiveEditTool();
}

UInteractiveTool* SUbstreamModelerViewport::GetAnyActiveStageToolInstance() const
{
	UUbstreamStageEditorMode* Mode = GetStageEditorMode();
	return Mode ? Mode->GetActiveToolInstance() : nullptr;
}

UInteractiveTool* SUbstreamModelerViewport::GetActiveEditToolInstance() const
{
	return GetAnyActiveStageToolInstance();
}

void SUbstreamModelerViewport::ApplyActiveToolDisplayPolicy()
{
	UInteractiveTool* ActiveTool = GetAnyActiveStageToolInstance();
	if (!IsValid(ActiveTool))
	{
		return;
	}

	// Space Warp keeps a translucent copy of the undeformed input visible by
	// default. In Ubstream's isolated stage that reads as a duplicate mesh, so
	// hide the tool-owned reference preview without changing the user's saved
	// Modeling Tools preference.
	HidePreviewMeshObjectProperty(*ActiveTool, TEXT("OriginalMeshPreview"));
}

void SUbstreamModelerViewport::RefreshToolPreviewVisibility()
{
	// During a CSG session the subject carries the live result directly; leave
	// its visibility to the session.
	if (HasActiveCSGSession())
	{
		return;
	}

	if (!bEditModeActive || !HasAnyActiveStageTool())
	{
		SetStagePreviewComponentsSuppressed(false);
		return;
	}

	ApplyActiveToolDisplayPolicy();

	TSet<const UMeshComponent*> BaseComponents;
	for (const UMeshComponent* Component : PreviewComponents)
	{
		if (IsValid(Component))
		{
			BaseComponents.Add(Component);
		}
	}

	TArray<UPrimitiveComponent*> StageComponents;
	AppendEditSessionMeshComponents(StageComponents);

	bool bToolHasOpaqueWorkingMesh = false;
	for (UPrimitiveComponent* Primitive : StageComponents)
	{
		UMeshComponent* MeshComponent = Cast<UMeshComponent>(Primitive);
		if (!IsValid(MeshComponent)
			|| BaseComponents.Contains(MeshComponent)
			|| !MeshComponent->IsVisible()
			|| !UbstreamMeshComponents::HasRenderableGeometry(MeshComponent)
			|| IsFullyTranslucentMeshComponent(*MeshComponent))
		{
			continue;
		}

		bToolHasOpaqueWorkingMesh = true;
		break;
	}

	SetStagePreviewComponentsSuppressed(bToolHasOpaqueWorkingMesh);
}

bool SUbstreamModelerViewport::CanStartEditTool(const FString& ToolIdentifier) const
{
	if (IsRemeshing()) return false;
	UUbstreamStageEditorMode* Mode = GetStageEditorMode();
	return Mode && Mode->CanStartTool(ToolIdentifier);
}

bool SUbstreamModelerViewport::StartEditTool(const FString& ToolIdentifier)
{
	if (IsRemeshing()) return false;
	// Tools only tick for the mode manager's focused viewport; make sure that
	// is us before the deferred start resolves.
	FocusStageViewport();

	if (!SelectPreviewComponentsForTool(ToolIdentifier))
	{
		return false;
	}

	UUbstreamStageEditorMode* Mode = GetStageEditorMode();
	const bool bStarted = Mode && Mode->StartTool(ToolIdentifier);
	if (bStarted)
	{
		bHasUncommittedStageEdits = true;
		OnEditingStateChanged.ExecuteIfBound();
	}
	return bStarted;
}

bool SUbstreamModelerViewport::CanStartCSGOperation() const
{
	if (IsRemeshing()) return false;
	if (!HasEditableSource() || HasAnyActiveStageTool())
	{
		return false;
	}

	TArray<UMeshComponent*> SourceComponentsForEditing;
	AppendSourcePreviewComponents(SourceComponentsForEditing);
	if (SourceComponentsForEditing.Num() != 1)
	{
		return false;
	}

	TArray<UMeshComponent*> CSGPrimitives;
	AppendCSGPrimitiveComponents(CSGPrimitives);
	return !CSGPrimitives.IsEmpty();
}

bool SUbstreamModelerViewport::StartCSGOperation(const EUbstreamCSGOperation Operation)
{
	if (!CanStartCSGOperation())
	{
		UE_LOG(LogUbstreamModeler, Warning,
			TEXT("StartCSGOperation refused: editable=%d activeTool=%d sources=%d cutters=%d"),
			HasEditableSource() ? 1 : 0,
			HasAnyActiveStageTool() ? 1 : 0,
			GetSourcePreviewComponentCount(),
			GetCSGPrimitiveCount());
		return false;
	}

	FString EditableSourceError;
	if (!EnsureEngineAssetSourcesAreEditable(EditableSourceError))
	{
		UE_LOG(LogUbstreamModeler, Warning, TEXT("StartCSGOperation refused: %s"), *EditableSourceError);
		return false;
	}

	if (!bEditModeActive && !EnterEditMode())
	{
		UE_LOG(LogUbstreamModeler, Warning, TEXT("StartCSGOperation refused: stage edit mode failed to activate"));
		return false;
	}

	UDynamicMeshComponent* BrushComponent = Cast<UDynamicMeshComponent>(ResolveCSGPrimitiveForTool());
	if (!BrushComponent)
	{
		UE_LOG(LogUbstreamModeler, Warning, TEXT("StartCSGOperation refused: no dynamic-mesh cutter available"));
		return false;
	}

	PendingCSGOperation = Operation;
	// The operation applies to every SELECTED cutter (falling back to the
	// resolved target); unselected cutters keep their own. The live session
	// composites the whole stack. Tag writes are transactional so op changes
	// undo cleanly.
	TArray<UMeshComponent*> OpTargets;
	GetSelectedCSGPrimitiveComponents(OpTargets);
	if (!OpTargets.Contains(BrushComponent))
	{
		OpTargets.Add(BrushComponent);
	}
	{
		const bool bAnyOpChange = OpTargets.ContainsByPredicate([this, Operation](const UMeshComponent* OpTarget)
		{
			return GetCSGPrimitiveOperation(OpTarget) != Operation;
		});
		TUniquePtr<FScopedTransaction> OpTransaction;
		if (bAnyOpChange && !bHandlingUndoRedo)
		{
			OpTransaction = MakeUnique<FScopedTransaction>(
				NSLOCTEXT("SUbstreamModelerViewport", "ChangeCutterOpTransaction", "Change Cutter Operation"));
		}
		if (!bHandlingUndoRedo)
		{
			for (UMeshComponent* OpTarget : OpTargets)
			{
				SetCSGPrimitiveOperation(OpTarget, Operation);
			}
		}
	}

	// Collect the stack in stable stage order.
	TArray<UMeshComponent*> CutterComponents;
	AppendCSGPrimitiveComponents(CutterComponents);
	TArray<UDynamicMeshComponent*> StackCutters;
	for (UMeshComponent* Cutter : CutterComponents)
	{
		if (UDynamicMeshComponent* DynamicCutter = Cast<UDynamicMeshComponent>(Cutter))
		{
			StackCutters.Add(DynamicCutter);
		}
	}
	if (StackCutters.IsEmpty())
	{
		return false;
	}

	// Same stack, new op on one cutter: retarget the running session instead
	// of tearing the preview down and back up.
	if (HasActiveCSGSession())
	{
		const bool bStackUnchanged = CSGSessionBrushes.Num() == StackCutters.Num()
			&& [this, &StackCutters]()
			{
				for (int32 Index = 0; Index < StackCutters.Num(); ++Index)
				{
					if (CSGSessionBrushes[Index].Get() != StackCutters[Index])
					{
						return false;
					}
				}
				return true;
			}();
		if (bStackUnchanged)
		{
			for (const UMeshComponent* OpTarget : OpTargets)
			{
				const int32 BrushIndex = FindCSGSessionBrushIndex(OpTarget);
				if (BrushIndex != INDEX_NONE)
				{
					CSGSession->SetBrushOperation(BrushIndex, ToKernelCSGOp(Operation));
				}
			}
			ApplySessionGhostMaterials(StackCutters);
			SelectCSGPrimitiveForEditing(BrushComponent);
			RefreshCSGPrimitiveVisuals();
			OnEditingStateChanged.ExecuteIfBound();
			return true;
		}
		CancelCSGSession();
	}

	UDynamicMeshComponent* Subject = ResolveCSGSessionSubject();
	if (!Subject)
	{
		return false;
	}

	// CancelCSGSession above restored the pre-session mesh, so reading the
	// subject here always snapshots clean geometry.
	UE::Geometry::FDynamicMesh3 SubjectMesh;
	Subject->ProcessMesh([&SubjectMesh](const UE::Geometry::FDynamicMesh3& Mesh)
	{
		SubjectMesh = Mesh;
	});
	// Imported subjects are often open by construction; remember their own
	// boundary count so the session only warns about edges the CUT introduces.
	CSGSessionSubjectOpenBoundaryEdges = 0;
	for (const int32 EdgeId : SubjectMesh.BoundaryEdgeIndicesItr())
	{
		++CSGSessionSubjectOpenBoundaryEdges;
	}

	TArray<FUbstreamCSGSessionBrush> SessionBrushes;
	SessionBrushes.Reserve(StackCutters.Num());
	CSGSessionBrushes.Reset(StackCutters.Num());
	for (UDynamicMeshComponent* Cutter : StackCutters)
	{
		FUbstreamCSGSessionBrush& SessionBrush = SessionBrushes.AddDefaulted_GetRef();
		Cutter->ProcessMesh([&SessionBrush](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			SessionBrush.Mesh = Mesh;
		});
		// Geometry a union contributes must render with the MODEL's primary
		// material, whatever the subject's slot layout: pin the brush copy's
		// material IDs to slot 0.
		if (!SessionBrush.Mesh.HasAttributes())
		{
			SessionBrush.Mesh.EnableAttributes();
		}
		if (!SessionBrush.Mesh.Attributes()->HasMaterialID())
		{
			SessionBrush.Mesh.Attributes()->EnableMaterialID();
		}
		if (UE::Geometry::FDynamicMeshMaterialAttribute* BrushMaterialIDs = SessionBrush.Mesh.Attributes()->GetMaterialID())
		{
			for (const int32 TriangleId : SessionBrush.Mesh.TriangleIndicesItr())
			{
				BrushMaterialIDs->SetValue(TriangleId, 0);
			}
		}
		SessionBrush.Transform = Cutter->GetComponentTransform();
		SessionBrush.Operation = ToKernelCSGOp(GetCSGPrimitiveOperation(Cutter));
		CSGSessionBrushes.Add(Cutter);
	}

	if (!CSGSession.IsValid())
	{
		CSGSession = MakeShared<FUbstreamCSGSession, ESPMode::ThreadSafe>();
	}
	CSGSession->OnPreviewReady.BindSP(this, &SUbstreamModelerViewport::HandleCSGPreviewReady);

	// The preview carves the subject itself: no stand-in component, no
	// visibility juggling, no copied materials - the mesh the user sees is
	// always the real subject with its real look. The original is kept for
	// cancel/undo, and the commit records original -> result explicitly.
	CSGSessionSubject = Subject;
	CSGSessionOriginalMesh = MakeShared<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>(SubjectMesh);
	CSGSessionSubjectFingerprint = ComputeDynamicMeshFingerprint(*Subject);
	CSGSessionMessage.Reset();

	// Ghost every cutter so the live result stays readable behind them; a
	// solid cutter would hide the very hole a subtract carves. Epic's
	// translucent tool-overlay material gives the see-through body, and the
	// wireframe render pass keeps edges crisp (colored per operation).
	ApplySessionGhostMaterials(StackCutters);
	UMaterialInterface* GhostMaterial = LastSessionGhostMaterial.Get();
	if (!GhostMaterial)
	{
		UE_LOG(LogUbstreamModeler, Error,
			TEXT("CSG session brush ghost materials unavailable; falling back to dim solid"));
	}

	// Open-shell subjects keep open cuts: filling would cap the very hole a
	// subtract carves. Watertight subjects keep the fill for solid results.
	CSGSession->Begin(
		MoveTemp(SubjectMesh),
		Subject->GetComponentTransform(),
		MoveTemp(SessionBrushes),
		/*bFillCreatedHoles*/ CSGSessionSubjectOpenBoundaryEdges == 0);

	SelectCSGPrimitiveForEditing(BrushComponent);
	UE_LOG(LogUbstreamModeler, Log,
		TEXT("CSG session started: %d cutter(s) live (ghost=%d)"),
		CSGSessionBrushes.Num(),
		GhostMaterial ? 1 : 0);
	bCaptureMirrorRefreshPending = true;
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnSoloMeshChanged.ExecuteIfBound();
	OnEditingStateChanged.ExecuteIfBound();
	return true;
}

bool SUbstreamModelerViewport::HasActiveCSGSession() const
{
	return CSGSession.IsValid() && CSGSession->IsActive();
}

bool SUbstreamModelerViewport::IsCSGSessionComputing() const
{
	return HasActiveCSGSession() && CSGSession->HasPendingWork();
}

bool SUbstreamModelerViewport::CanCommitCSGSession() const
{
	if (!HasActiveCSGSession() || CSGSession->HasPendingWork() || !CSGSessionSubject.IsValid())
	{
		return false;
	}

	const FUbstreamCSGResult* Latest = CSGSession->GetLatestResult();
	return Latest && Latest->bSuccess && Latest->Mesh.TriangleCount() > 0;
}

void SUbstreamModelerViewport::TickCSGSession()
{
	if (CSGSession.IsValid())
	{
		CSGSession->Tick();
	}
}

UEditorInteractiveToolsContext* SUbstreamModelerViewport::GetStageToolsContextForTesting() const
{
	UUbstreamStageEditorMode* Mode = GetStageEditorMode();
	return Mode ? Mode->GetStageToolsContext() : nullptr;
}

int32 SUbstreamModelerViewport::FindCSGSessionBrushIndex(const UMeshComponent* Component) const
{
	for (int32 Index = 0; Index < CSGSessionBrushes.Num(); ++Index)
	{
		if (CSGSessionBrushes[Index].Get() == Component)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

EUbstreamCSGOperation SUbstreamModelerViewport::GetCSGPrimitiveOperation(const UMeshComponent* Component) const
{
	// Stored as a component tag: ComponentTags is a UPROPERTY, so the
	// operation serializes through undo/redo with the component itself.
	if (Component)
	{
		if (Component->ComponentTags.Contains(TEXT("csg_op_union")))
		{
			return EUbstreamCSGOperation::Union;
		}
		if (Component->ComponentTags.Contains(TEXT("csg_op_intersect")))
		{
			return EUbstreamCSGOperation::Intersect;
		}
	}
	return EUbstreamCSGOperation::Subtract;
}

void SUbstreamModelerViewport::SetCSGPrimitiveOperation(UMeshComponent* Component, const EUbstreamCSGOperation Operation)
{
	if (!IsValid(Component) || GetCSGPrimitiveOperation(Component) == Operation)
	{
		return;
	}
	Component->Modify();
	Component->ComponentTags.Remove(TEXT("csg_op_union"));
	Component->ComponentTags.Remove(TEXT("csg_op_intersect"));
	if (Operation == EUbstreamCSGOperation::Union)
	{
		Component->ComponentTags.Add(TEXT("csg_op_union"));
	}
	else if (Operation == EUbstreamCSGOperation::Intersect)
	{
		Component->ComponentTags.Add(TEXT("csg_op_intersect"));
	}
}

EUbstreamCSGOperation SUbstreamModelerViewport::GetSelectedCSGOperation() const
{
	TArray<UMeshComponent*> SelectedCutters;
	GetSelectedCSGPrimitiveComponents(SelectedCutters);
	return SelectedCutters.Num() > 0
		? GetCSGPrimitiveOperation(SelectedCutters.Last())
		: EUbstreamCSGOperation::Subtract;
}

namespace
{
	UMeshComponent* GetLastSelectedCutter(const SUbstreamModelerViewport& Viewport)
	{
		TArray<UMeshComponent*> SelectedCutters;
		Viewport.GetSelectedCSGPrimitiveComponents(SelectedCutters);
		return SelectedCutters.Num() > 0 ? SelectedCutters.Last() : nullptr;
	}
}

bool SUbstreamModelerViewport::GetSelectedCSGPrimitiveTransform(FVector& OutLocation, FRotator& OutRotation, double& OutSize) const
{
	UMeshComponent* Cutter = GetLastSelectedCutter(*this);
	if (!IsValid(Cutter))
	{
		return false;
	}
	const FTransform& Transform = Cutter->GetComponentTransform();
	OutLocation = Transform.GetLocation();
	OutRotation = Transform.Rotator();
	Cutter->UpdateBounds();
	OutSize = Cutter->Bounds.GetBox().GetSize().GetMax();
	return true;
}

bool SUbstreamModelerViewport::SetSelectedCSGPrimitiveLocation(const FVector& NewLocation)
{
	UMeshComponent* Cutter = GetLastSelectedCutter(*this);
	if (!IsValid(Cutter))
	{
		return false;
	}
	FScopedTransaction Transaction(NSLOCTEXT("SUbstreamModelerViewport", "MoveCutterTransaction", "Move Cutter"));
	Cutter->Modify();
	Cutter->SetWorldLocation(NewLocation);
	Cutter->UpdateBounds();
	PushSessionBrushTransforms();
	UpdateStageTransformGizmo();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnSoloMeshChanged.ExecuteIfBound();
	return true;
}

bool SUbstreamModelerViewport::SetSelectedCSGPrimitiveRotation(const FRotator& NewRotation)
{
	UMeshComponent* Cutter = GetLastSelectedCutter(*this);
	if (!IsValid(Cutter))
	{
		return false;
	}
	FScopedTransaction Transaction(NSLOCTEXT("SUbstreamModelerViewport", "RotateCutterTransaction", "Rotate Cutter"));
	Cutter->Modify();
	Cutter->SetWorldRotation(NewRotation);
	Cutter->UpdateBounds();
	PushSessionBrushTransforms();
	UpdateStageTransformGizmo();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnSoloMeshChanged.ExecuteIfBound();
	return true;
}

bool SUbstreamModelerViewport::SetSelectedCSGPrimitiveSize(const double NewSize)
{
	UMeshComponent* Cutter = GetLastSelectedCutter(*this);
	if (!IsValid(Cutter) || NewSize <= UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return false;
	}
	Cutter->UpdateBounds();
	const double CurrentSize = Cutter->Bounds.GetBox().GetSize().GetMax();
	if (CurrentSize <= UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return false;
	}
	FScopedTransaction Transaction(NSLOCTEXT("SUbstreamModelerViewport", "ScaleCutterTransaction", "Scale Cutter"));
	Cutter->Modify();
	Cutter->SetWorldScale3D(Cutter->GetComponentTransform().GetScale3D() * (NewSize / CurrentSize));
	Cutter->UpdateBounds();
	PushSessionBrushTransforms();
	UpdateStageTransformGizmo();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnSoloMeshChanged.ExecuteIfBound();
	return true;
}

bool SUbstreamModelerViewport::GetSelectedCSGPrimitiveUniformScale(double& OutScale) const
{
	UMeshComponent* Cutter = GetLastSelectedCutter(*this);
	if (!IsValid(Cutter))
	{
		return false;
	}
	const FVector Scale3D = Cutter->GetComponentTransform().GetScale3D().GetAbs();
	OutScale = FMath::Max3(Scale3D.X, Scale3D.Y, Scale3D.Z);
	return OutScale > UE_DOUBLE_KINDA_SMALL_NUMBER;
}

bool SUbstreamModelerViewport::SetSelectedCSGPrimitiveUniformScale(const double NewScale)
{
	UMeshComponent* Cutter = GetLastSelectedCutter(*this);
	if (!IsValid(Cutter) || NewScale <= UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	double CurrentScale = 1.0;
	if (!GetSelectedCSGPrimitiveUniformScale(CurrentScale) || CurrentScale <= UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("SUbstreamModelerViewport", "UniformScaleCutterTransaction", "Scale Cutter"));
	Cutter->Modify();
	Cutter->SetWorldScale3D(Cutter->GetComponentTransform().GetScale3D() * (NewScale / CurrentScale));
	Cutter->UpdateBounds();
	PushSessionBrushTransforms();
	UpdateStageTransformGizmo();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnSoloMeshChanged.ExecuteIfBound();
	return true;
}

bool SUbstreamModelerViewport::GetSelectedCSGPrimitiveDimensions(FVector& OutDimensions) const
{
	UDynamicMeshComponent* Cutter = Cast<UDynamicMeshComponent>(GetLastSelectedCutter(*this));
	if (!IsValid(Cutter))
	{
		return false;
	}
	// Local mesh bounds x per-axis scale = the cutter's ORIENTED dimensions
	// (its own width/depth/height, independent of rotation).
	UE::Geometry::FAxisAlignedBox3d LocalBounds;
	Cutter->ProcessMesh([&LocalBounds](const UE::Geometry::FDynamicMesh3& Mesh)
	{
		LocalBounds = Mesh.GetBounds();
	});
	OutDimensions = FVector(LocalBounds.Max.X - LocalBounds.Min.X,
		LocalBounds.Max.Y - LocalBounds.Min.Y,
		LocalBounds.Max.Z - LocalBounds.Min.Z) * Cutter->GetComponentTransform().GetScale3D();
	OutDimensions = OutDimensions.GetAbs();
	return true;
}

bool SUbstreamModelerViewport::SetSelectedCSGPrimitiveDimension(const int32 Axis, const double NewSize)
{
	UDynamicMeshComponent* Cutter = Cast<UDynamicMeshComponent>(GetLastSelectedCutter(*this));
	if (!IsValid(Cutter) || NewSize <= UE_DOUBLE_KINDA_SMALL_NUMBER || Axis < 0 || Axis > 2)
	{
		return false;
	}
	FVector CurrentDimensions;
	if (!GetSelectedCSGPrimitiveDimensions(CurrentDimensions) || CurrentDimensions[Axis] <= UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("SUbstreamModelerViewport", "ResizeCutterTransaction", "Resize Cutter"));
	Cutter->Modify();
	FVector NewScale = Cutter->GetComponentTransform().GetScale3D();
	NewScale[Axis] *= NewSize / CurrentDimensions[Axis];
	Cutter->SetWorldScale3D(NewScale);
	Cutter->UpdateBounds();
	PushSessionBrushTransforms();
	UpdateStageTransformGizmo();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnSoloMeshChanged.ExecuteIfBound();
	return true;
}

void SUbstreamModelerViewport::GetCSGPrimitiveDisplayList(TArray<TPair<UMeshComponent*, FString>>& OutCutters) const
{
	TArray<UMeshComponent*> Cutters;
	AppendCSGPrimitiveComponents(Cutters);
	int32 FallbackIndex = 1;
	for (UMeshComponent* Cutter : Cutters)
	{
		// Cutter tags look like "csg_box_003"; render as "Box 3".
		FString DisplayName;
		for (const FName& ComponentTag : Cutter->ComponentTags)
		{
			FString TagString = ComponentTag.ToString();
			if (TagString.RemoveFromStart(TEXT("csg_")))
			{
				FString TypePart;
				FString IndexPart;
				if (TagString.Split(TEXT("_"), &TypePart, &IndexPart, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
				{
					TypePart = TypePart.Left(1).ToUpper() + TypePart.Mid(1);
					DisplayName = FString::Printf(TEXT("%s %d"), *TypePart, FCString::Atoi(*IndexPart));
					break;
				}
			}
		}
		if (DisplayName.IsEmpty())
		{
			DisplayName = FString::Printf(TEXT("Cutter %d"), FallbackIndex);
		}
		++FallbackIndex;
		OutCutters.Emplace(Cutter, MoveTemp(DisplayName));
	}
}

void SUbstreamModelerViewport::SetCSGSnapIncrement(const double Increment)
{
	CSGSnapIncrement = FMath::Max(0.0, Increment);
	UpdateStageTransformGizmo();
	OnEditingStateChanged.ExecuteIfBound();
}

void SUbstreamModelerViewport::ApplyCSGSnapToGizmo(UCombinedTransformGizmo& Gizmo) const
{
	const bool bSnapEnabled = CSGSnapIncrement > 0.0;
	Gizmo.bSnapToWorldGrid = bSnapEnabled;
	Gizmo.bGridSizeIsExplicit = true;
	Gizmo.ExplicitGridSize = FVector(FMath::Max(CSGSnapIncrement, 0.001));
	// Rotation stays continuous (floating) unless the user enables snap.
	Gizmo.bSnapToWorldRotGrid = bSnapEnabled;
	Gizmo.bRotationGridSizeIsExplicit = true;
	Gizmo.ExplicitRotationGridSize = FRotator(15.0, 15.0, 15.0);
	// Always free continuous scale on the ITF path (legacy scale widget has its own gain).
	Gizmo.bSnapToScaleGrid = false;
}

bool SUbstreamModelerViewport::CanAcceptSelectedCutter() const
{
	// Any single selected cutter can be baked on its own (keeping the others
	// live). Note: baking a Subtract/Intersect that sits above a Union bakes
	// against the raw base, so it won't cut that union in the result - the live
	// preview stays correct, but the baked prefix ordering is approximate.
	TArray<UMeshComponent*> SelectedCutters;
	GetSelectedCSGPrimitiveComponents(SelectedCutters);
	return SelectedCutters.Num() == 1 && !HasAnyActiveStageTool();
}

bool SUbstreamModelerViewport::AcceptSelectedCutter()
{
	if (!CanAcceptSelectedCutter())
	{
		return false;
	}
	TArray<UMeshComponent*> SelectedCutters;
	GetSelectedCSGPrimitiveComponents(SelectedCutters);
	UDynamicMeshComponent* Cutter = Cast<UDynamicMeshComponent>(SelectedCutters[0]);
	UDynamicMeshComponent* Subject = ResolveCSGSessionSubject();
	if (!IsValid(Cutter) || !IsValid(Subject))
	{
		return false;
	}

	// Bake ONLY the selected cutter into the base; every OTHER cutter stays
	// live and re-carves on top of the new base. Baking a Union leaves its
	// geometry in the base, so a later Subtract still cuts it - the ordered
	// stack is preserved without collapsing the rest of the list.
	UE::Geometry::FDynamicMesh3 BaseMesh;
	if (HasActiveCSGSession() && CSGSessionOriginalMesh.IsValid())
	{
		BaseMesh = *CSGSessionOriginalMesh;
	}
	else
	{
		Subject->ProcessMesh([&BaseMesh](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			BaseMesh = Mesh;
		});
	}

	int32 BaseOpenBoundaryEdges = 0;
	for (const int32 EdgeId : BaseMesh.BoundaryEdgeIndicesItr())
	{
		++BaseOpenBoundaryEdges;
	}

	UE::Geometry::FDynamicMesh3 CutterMesh;
	Cutter->ProcessMesh([&CutterMesh](const UE::Geometry::FDynamicMesh3& Mesh)
	{
		CutterMesh = Mesh;
	});
	// Baked geometry (a union contributes surface) must use the model's primary
	// material slot, whatever the cutter's own slot layout.
	if (!CutterMesh.HasAttributes())
	{
		CutterMesh.EnableAttributes();
	}
	if (!CutterMesh.Attributes()->HasMaterialID())
	{
		CutterMesh.Attributes()->EnableMaterialID();
	}
	if (UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs = CutterMesh.Attributes()->GetMaterialID())
	{
		for (const int32 TriangleId : CutterMesh.TriangleIndicesItr())
		{
			MaterialIDs->SetValue(TriangleId, 0);
		}
	}

	// One-shot synchronous boolean: a deliberate button press, not a per-frame
	// path. The cutter's own operation applies (Subtract, Union or Intersect).
	FUbstreamCSGResult Result = FUbstreamCSGKernel::Apply(
		BaseMesh, Subject->GetComponentTransform(),
		CutterMesh, Cutter->GetComponentTransform(),
		ToKernelCSGOp(GetCSGPrimitiveOperation(Cutter)),
		/*bFillCreatedHoles*/ BaseOpenBoundaryEdges == 0);
	if (!Result.bSuccess)
	{
		CSGSessionMessage = TEXT("Could not apply this cut; adjust the cutter and try again.");
		OnEditingStateChanged.ExecuteIfBound();
		return false;
	}
	if (Result.Mesh.TriangleCount() == 0)
	{
		// A non-overlapping intersect (or an all-consuming subtract) would
		// erase the entire mesh - refuse instead of silently destroying it.
		CSGSessionMessage = TEXT("This cut would remove the entire mesh; move the cutter so it overlaps the model.");
		OnEditingStateChanged.ExecuteIfBound();
		return false;
	}

	// The live session's snapshot is about to go stale; end it (no restore -
	// the transaction below writes the new base) and rebuild afterwards.
	if (HasActiveCSGSession())
	{
		CSGSession->End();
	}

	{
		const TSharedPtr<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> BeforeMesh =
			MakeShared<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>(MoveTemp(BaseMesh));
		const TSharedPtr<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> AfterMesh =
			MakeShared<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>(MoveTemp(Result.Mesh));

		FScopedTransaction Transaction(NSLOCTEXT("SUbstreamModelerViewport", "AcceptCutTransaction", "Accept Cut"));
		Subject->SetFlags(RF_Transactional);
		Subject->Modify();
		if (UDynamicMesh* SubjectDynamicMesh = Subject->GetDynamicMesh())
		{
			SubjectDynamicMesh->SetMesh(*AfterMesh);
		}
		if (GUndo)
		{
			GUndo->StoreUndo(Subject, MakeUnique<FMeshReplacementChange>(BeforeMesh, AfterMesh));
		}
		Subject->UpdateBounds();

		// Consume ONLY the accepted cutter; the rest of the stack stays live.
		if (USelection* StageSelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr)
		{
			StageSelectedComponents->Deselect(Cutter);
			StageSelectedComponents->NoteSelectionChanged();
		}
		if (AActor* Stage = StageActor.Get())
		{
			Stage->Modify();
		}
		Cutter->Modify();
		StoreCutterMeshForUndo(Cutter);
		Cutter->DestroyComponent();
		PreviewComponents.Remove(Cutter);
	}
	EndCSGSessionInternal();
	bHasUncommittedStageEdits = true;

	// Survivors go live again on the new base; otherwise clean chrome.
	TArray<UMeshComponent*> RemainingCutters;
	AppendCSGPrimitiveComponents(RemainingCutters);
	if (!RemainingCutters.IsEmpty())
	{
		SelectLatestCSGPrimitiveForEditing();
		StartCSGOperation(GetSelectedCSGOperation());
	}
	else
	{
		DeselectAllStageComponents();
	}
	RefreshCSGPrimitiveVisuals();
	UpdateStageTransformGizmo();
	bCaptureMirrorRefreshPending = true;
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnSoloMeshChanged.ExecuteIfBound();
	OnEditingStateChanged.ExecuteIfBound();
	return true;
}

void SUbstreamModelerViewport::ApplySessionGhostMaterials(const TArray<UDynamicMeshComponent*>& StackCutters)
{
	UUbstreamStageEditorMode* StageMode = GetStageEditorMode();
	UEditorInteractiveToolsContext* ToolsContext = StageMode ? StageMode->GetStageToolsContext() : nullptr;
	UInteractiveToolManager* ToolManager = ToolsContext ? ToolsContext->ToolManager : nullptr;
	if (!ToolManager)
	{
		return;
	}

	// Every live cutter is OUTLINE-ONLY: the model's live result surface is
	// the preview (a union's surface coincides with the cutter and Z-fights
	// any visible body; subtract/intersect bodies obscure the very cavity or
	// overlap the user needs to see). The op-colored wireframe edges remain
	// as the visible, selectable handle.
	// Opacity 0.0: even a 1.5% body shimmers against a coincident union
	// surface. Fully invisible body; the op-colored wireframe is the handle.
	UMaterialInterface* OutlineGhost = ToolSetupUtil::GetSimpleCustomMaterial(
		ToolManager, CSGSessionGhostColor, 0.0f);
	if (!OutlineGhost)
	{
		OutlineGhost = ToolSetupUtil::GetDefaultWorkingMaterial(ToolManager);
	}

	LastSessionGhostMaterial = OutlineGhost;
	for (UDynamicMeshComponent* Cutter : StackCutters)
	{
		if (OutlineGhost)
		{
			Cutter->SetOverrideRenderMaterial(OutlineGhost);
		}
	}
}

void SUbstreamModelerViewport::PushSessionBrushTransforms()
{
	if (!HasActiveCSGSession())
	{
		return;
	}
	// The session dedupes unchanged transforms, so pushing the whole stack is
	// cheap and removes all per-call-site index bookkeeping.
	for (int32 Index = 0; Index < CSGSessionBrushes.Num(); ++Index)
	{
		if (const UMeshComponent* Brush = CSGSessionBrushes[Index].Get())
		{
			CSGSession->SetBrushTransform(Index, Brush->GetComponentTransform());
		}
	}
}

UDynamicMeshComponent* SUbstreamModelerViewport::ResolveCSGSessionSubject() const
{
	TArray<UMeshComponent*> SourceComponentsForEditing;
	AppendSourcePreviewComponents(SourceComponentsForEditing);
	return SourceComponentsForEditing.Num() == 1
		? Cast<UDynamicMeshComponent>(SourceComponentsForEditing[0])
		: nullptr;
}

void SUbstreamModelerViewport::HandleCSGPreviewReady(const FUbstreamCSGResult& Result)
{
	if (!HasActiveCSGSession())
	{
		return;
	}

	if (!Result.bSuccess)
	{
		// Keep the last good preview visible; the user just moves the cutter on.
		CSGSessionMessage = Result.ErrorMessage.IsEmpty()
			? TEXT("The boolean failed for this cutter placement.")
			: Result.ErrorMessage;
		OnEditingStateChanged.ExecuteIfBound();
		return;
	}

	if (Result.Mesh.TriangleCount() == 0)
	{
		CSGSessionMessage = TEXT("This cut would remove the entire mesh; adjust the cutter to keep part of the model.");
		OnEditingStateChanged.ExecuteIfBound();
		return;
	}

	const int32 NewOpenBoundaryEdges = Result.BoundaryEdgesAfterRepair - CSGSessionSubjectOpenBoundaryEdges;
	CSGSessionMessage = NewOpenBoundaryEdges > 0
		? FString::Printf(TEXT("The cut left %d open edges; adjust the cutter for a clean result."), NewOpenBoundaryEdges)
		: FString();

	if (UDynamicMeshComponent* Subject = CSGSessionSubject.Get())
	{
		// The kernel returns the result in subject-local space, so it drops
		// straight into the subject. Track what we wrote: undo/redo touching
		// the subject from outside is detected by fingerprint mismatch.
		if (UDynamicMesh* SubjectDynamicMesh = Subject->GetDynamicMesh())
		{
			SubjectDynamicMesh->SetMesh(Result.Mesh);
		}
		Subject->UpdateBounds();
		CSGSessionSubjectFingerprint = ComputeDynamicMeshFingerprint(*Subject);
	}

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	// Light capture dirty only: full OnSoloMeshChanged during live drag flooded
	// the control panel and contributed to gizmo visual instability.
	OnSoloMeshChanged.ExecuteIfBound();
	// Skip editing-state rebuilds while a mouse button is down (gizmo drag).
	const bool bUserDragging =
		ViewportClient.IsValid()
		&& ViewportClient->Viewport
		&& (ViewportClient->Viewport->KeyState(EKeys::LeftMouseButton)
			|| ViewportClient->Viewport->KeyState(EKeys::MiddleMouseButton)
			|| ViewportClient->Viewport->KeyState(EKeys::RightMouseButton));
	if (!bUserDragging)
	{
		OnEditingStateChanged.ExecuteIfBound();
	}
}

void SUbstreamModelerViewport::EndCSGSessionInternal()
{
	if (CSGSession.IsValid())
	{
		CSGSession->End();
	}

	for (const TWeakObjectPtr<UMeshComponent>& SessionBrush : CSGSessionBrushes)
	{
		if (UBaseDynamicMeshComponent* DynamicBrush = Cast<UBaseDynamicMeshComponent>(SessionBrush.Get()))
		{
			DynamicBrush->ClearOverrideRenderMaterial();
		}
	}

	CSGSessionSubject.Reset();
	CSGSessionBrushes.Reset();
	CSGSessionOriginalMesh.Reset();
	CSGSessionSubjectFingerprint = 0;
	CSGSessionSubjectOpenBoundaryEdges = 0;
	CSGSessionMessage.Reset();
	bCaptureMirrorRefreshPending = true;
}

void SUbstreamModelerViewport::CommitCSGSession()
{
	if (!CanCommitCSGSession())
	{
		return;
	}

	const FUbstreamCSGResult* Latest = CSGSession->GetLatestResult();
	const int32 OpenBoundaryEdges = Latest
		? FMath::Max(0, Latest->BoundaryEdgesAfterRepair - CSGSessionSubjectOpenBoundaryEdges)
		: 0;
	TOptional<UE::Geometry::FDynamicMesh3> ResultMesh = CSGSession->TakeResultMeshAndEnd();
	UDynamicMeshComponent* Subject = CSGSessionSubject.Get();
	TArray<UMeshComponent*> ConsumedBrushes;
	for (const TWeakObjectPtr<UMeshComponent>& SessionBrush : CSGSessionBrushes)
	{
		if (UMeshComponent* Brush = SessionBrush.Get())
		{
			ConsumedBrushes.Add(Brush);
		}
	}

	if (ResultMesh.IsSet() && IsValid(Subject))
	{
		// One transaction captures exactly pre-session -> result: the Before
		// mesh is the snapshot taken when the session began (the subject holds
		// preview data by now). UDynamicMesh deliberately skips its mesh data
		// when serializing for transactions, so the swap must be recorded as
		// an explicit replacement change; Modify() alone would be a silent
		// no-op for undo.
		TSharedPtr<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> BeforeMesh = CSGSessionOriginalMesh;
		if (!BeforeMesh.IsValid())
		{
			Subject->ProcessMesh([&BeforeMesh](const UE::Geometry::FDynamicMesh3& Mesh)
			{
				BeforeMesh = MakeShared<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>(Mesh);
			});
		}
		const TSharedPtr<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> AfterMesh =
			MakeShared<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>(MoveTemp(ResultMesh.GetValue()));

		FScopedTransaction Transaction(NSLOCTEXT("SUbstreamModelerViewport", "CommitCSGTransaction", "CSG Boolean"));
		Subject->SetFlags(RF_Transactional);
		Subject->Modify();
		if (UDynamicMesh* SubjectDynamicMesh = Subject->GetDynamicMesh())
		{
			SubjectDynamicMesh->SetMesh(*AfterMesh);
		}
		if (GUndo)
		{
			GUndo->StoreUndo(Subject, MakeUnique<FMeshReplacementChange>(BeforeMesh, AfterMesh));
		}
		Subject->UpdateBounds();

		// Accept bakes the whole stack, so every participating cutter is
		// consumed - no spent cutters piling up in the stage.
		USelection* StageSelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
		for (UMeshComponent* Brush : ConsumedBrushes)
		{
			if (!IsValid(Brush))
			{
				continue;
			}
			if (StageSelectedComponents)
			{
				StageSelectedComponents->Deselect(Brush);
			}
			Brush->Modify();
			// UDynamicMesh skips its mesh data in transactions: record an
			// explicit restoration so an undone bake resurrects the cutter
			// WITH its shape.
			if (UDynamicMeshComponent* DynamicBrush = Cast<UDynamicMeshComponent>(Brush))
			{
				StoreCutterMeshForUndo(DynamicBrush);
			}
			Brush->DestroyComponent();
			PreviewComponents.Remove(Brush);
		}
		if (StageSelectedComponents)
		{
			StageSelectedComponents->NoteSelectionChanged();
		}

		bHasUncommittedStageEdits = true;
	}

	EndCSGSessionInternal();
	CSGSessionMessage = OpenBoundaryEdges > 0
		? FString::Printf(TEXT("Applied; the cut left %d open edges."), OpenBoundaryEdges)
		: FString();

	TArray<UMeshComponent*> RemainingCSGPrimitives;
	AppendCSGPrimitiveComponents(RemainingCSGPrimitives);
	if (!RemainingCSGPrimitives.IsEmpty())
	{
		SelectLatestCSGPrimitiveForEditing();
	}
	else
	{
		// Accept/undo with no cutters left: clear the selection so no gizmo
		// lingers - chrome exists only while there is something to drag.
		DeselectAllStageComponents();
	}
	RefreshCSGPrimitiveVisuals();
	UpdateStageTransformGizmo();

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnSoloMeshChanged.ExecuteIfBound();
	OnEditingStateChanged.ExecuteIfBound();
}

void SUbstreamModelerViewport::CancelCSGSession()
{
	if (!HasActiveCSGSession())
	{
		return;
	}

	// Put the subject back exactly as it was before the session; the preview
	// wrote into it directly.
	UDynamicMeshComponent* Subject = CSGSessionSubject.Get();
	if (IsValid(Subject) && CSGSessionOriginalMesh.IsValid())
	{
		if (UDynamicMesh* SubjectDynamicMesh = Subject->GetDynamicMesh())
		{
			SubjectDynamicMesh->SetMesh(*CSGSessionOriginalMesh);
		}
		Subject->UpdateBounds();
	}

	EndCSGSessionInternal();
	RefreshCSGPrimitiveVisuals();
	UpdateStageTransformGizmo();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnSoloMeshChanged.ExecuteIfBound();
	OnEditingStateChanged.ExecuteIfBound();
}

void SUbstreamModelerViewport::UpdateStageTransformGizmo()
{
	UUbstreamStageEditorMode* Mode = GetStageEditorMode();
	UEditorInteractiveToolsContext* ToolsContext = Mode ? Mode->GetStageToolsContext() : nullptr;
	if (!ToolsContext || !ToolsContext->GizmoManager)
	{
		if (StageTransformGizmo.IsValid())
		{
			UE_LOG(LogUbstreamModeler, Warning,
				TEXT("Stage transform gizmo dropped: tools context unavailable (mode active=%d)"),
				Mode ? 1 : 0);
		}
		StageTransformGizmo.Reset();
		StageTransformGizmoProxy.Reset();
		StageTransformGizmoTargets.Reset();
		return;
	}

	// Only cutters get a transform gizmo. Interactive tools own the
	// viewport while they run; the subject remains a tool selection target.
	TArray<UMeshComponent*> Targets;
	if (!HasAnyActiveStageTool())
	{
		GetSelectedCSGPrimitiveComponents(Targets);
	}

	const bool bTargetsUnchanged = StageTransformGizmo.IsValid()
		&& Targets.Num() == StageTransformGizmoTargets.Num()
		&& Targets.Num() > 0
		&& [this, &Targets]()
		{
			for (int32 Index = 0; Index < Targets.Num(); ++Index)
			{
				if (StageTransformGizmoTargets[Index].Get() != Targets[Index])
				{
					return false;
				}
			}
			return true;
		}();
	if (bTargetsUnchanged)
	{
		// Targets can move without the gizmo knowing (programmatic transforms,
		// undo, session paths) - the proxy does not watch its components. Left
		// alone, the gizmo orphans at the old spot: the "double axes" bug.
		// Never reinitialize while the user is mid-drag: fighting the live proxy
		// made rotation rings blink/stutter.
		const bool bUserDragging =
			ViewportClient.IsValid()
			&& ViewportClient->Viewport
			&& (ViewportClient->Viewport->KeyState(EKeys::LeftMouseButton)
				|| ViewportClient->Viewport->KeyState(EKeys::MiddleMouseButton)
				|| ViewportClient->Viewport->KeyState(EKeys::RightMouseButton));
		if (UCombinedTransformGizmo* ExistingGizmo = StageTransformGizmo.Get())
		{
			ApplyCSGSnapToGizmo(*ExistingGizmo);
			if (Targets.Num() != 1 || bUserDragging)
			{
				return;
			}
			const FTransform AnchorTransform = GetStageGizmoAnchorTransform(Targets[0]);
			// Scale must be compared too: a numeric/slider scale change moves the
			// component without the proxy noticing, so if we only re-sync on
			// location/rotation the gizmo keeps its stale scale and re-asserts it
			// on the next viewport interaction - reverting the slider edit.
			if (!ExistingGizmo->GetGizmoTransform().GetLocation().Equals(AnchorTransform.GetLocation(), 0.01)
				|| !ExistingGizmo->GetGizmoTransform().GetRotation().Equals(AnchorTransform.GetRotation(), 0.001)
				|| !ExistingGizmo->GetGizmoTransform().GetScale3D().Equals(AnchorTransform.GetScale3D(), 0.001))
			{
				ExistingGizmo->ReinitializeGizmoTransform(AnchorTransform);
			}
		}
		return;
	}

	DestroyStageTransformGizmo();
	if (Targets.IsEmpty())
	{
		return;
	}

	// WM_None maps to NoGizmo in the tools context; make sure a mode is live.
	if (StageModeManager.IsValid() && StageModeManager->GetWidgetMode() == UE::Widget::WM_None)
	{
		StageModeManager->SetWidgetMode(UE::Widget::WM_Translate);
	}

	UTransformProxy* Proxy = NewObject<UTransformProxy>(GetTransientPackage(), NAME_None, RF_Transient);
	for (UMeshComponent* Target : Targets)
	{
		Proxy->AddComponent(Target);
		StageTransformGizmoTargets.Add(Target);
	}
	Proxy->OnTransformChanged.AddSP(this, &SUbstreamModelerViewport::HandleGizmoTransformChanged);

	// Epic's Modeling Mode gizmo: hover/drag/snap behavior comes with it, it
	// follows the mode manager's W/E/R widget mode each tick, and drags emit
	// undoable transactions through the stage tool manager. Build it from the
	// classic line components rather than 5.7's mesh handles: the meshes have
	// no usable size control (their adjuster re-asserts scale every tick) and
	// render as a dithered speck in the compact stage panel.
	IConsoleVariable* GizmoDrawModeCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("modeling.Gizmo.DrawMode"));
	const int32 PreviousGizmoDrawMode = GizmoDrawModeCVar ? GizmoDrawModeCVar->GetInt() : 1;
	if (GizmoDrawModeCVar)
	{
		GizmoDrawModeCVar->Set(0, ECVF_SetByCode);
	}
	// Unique identifier per creation: a stale registration (ours or a foreign
	// gizmo system's) must never be able to block re-creation - that was the
	// "gizmo doesn't come back when I re-select" bug.
	static int32 StageGizmoInstanceCounter = 0;
	UCombinedTransformGizmo* Gizmo = UE::TransformGizmoUtil::Create3AxisTransformGizmo(
		ToolsContext->GizmoManager,
		/*Owner*/ this,
		FString::Printf(TEXT("UbstreamStageTransformGizmo_%d"), ++StageGizmoInstanceCounter));
	if (GizmoDrawModeCVar)
	{
		GizmoDrawModeCVar->Set(PreviousGizmoDrawMode, ECVF_SetByCode);
	}
	if (!Gizmo)
	{
		UE_LOG(LogUbstreamModeler, Error,
			TEXT("Stage transform gizmo creation FAILED (targets=%d) - the selection has no gizmo; check TransformGizmoUtil context registration"),
			Targets.Num());
		StageTransformGizmoTargets.Reset();
		return;
	}
	// The Level Editor's world grid rounds small drags to ZERO on tiny
	// subjects, so the editor grid is never used. Snapping here is the
	// stage's own explicit increment, chosen in the CSG bar (0 = off).
	ApplyCSGSnapToGizmo(*Gizmo);
	// The transaction provider must be the GIZMO manager: the tool manager
	// only accepts change records while a tool is active, and this gizmo runs
	// standalone - routing through it drops the undo record with an ensure.
	Gizmo->SetActiveTarget(Proxy, ToolsContext->GizmoManager);
	// SetActiveTarget spawned the per-handle sub-gizmos through the manager;
	// find ours and keep each handle highlighted through its own drag.
	{
		TArray<UInteractiveGizmo*> SubGizmos;
		SubGizmos.Append(ToolsContext->GizmoManager->FindAllGizmosOfType(
			UCombinedTransformGizmoContextObject::DefaultAxisPositionBuilderIdentifier));
		SubGizmos.Append(ToolsContext->GizmoManager->FindAllGizmosOfType(
			UCombinedTransformGizmoContextObject::DefaultPlanePositionBuilderIdentifier));
		SubGizmos.Append(ToolsContext->GizmoManager->FindAllGizmosOfType(
			UCombinedTransformGizmoContextObject::DefaultAxisAngleBuilderIdentifier));
		for (const UInteractiveGizmo* SubGizmo : SubGizmos)
		{
			UObject* HitTargetObject = nullptr;
			if (const UAxisPositionGizmo* AxisGizmo = Cast<UAxisPositionGizmo>(SubGizmo))
			{
				HitTargetObject = AxisGizmo->HitTarget.GetObject();
			}
			else if (const UPlanePositionGizmo* PlaneGizmo = Cast<UPlanePositionGizmo>(SubGizmo))
			{
				HitTargetObject = PlaneGizmo->HitTarget.GetObject();
			}
			else if (const UAxisAngleGizmo* AngleGizmo = Cast<UAxisAngleGizmo>(SubGizmo))
			{
				HitTargetObject = AngleGizmo->HitTarget.GetObject();
			}

			UGizmoComponentHitTarget* HitTarget = Cast<UGizmoComponentHitTarget>(HitTargetObject);
			if (HitTarget && HitTarget->Component
				&& HitTarget->Component->GetOwner() == Gizmo->GetGizmoActor())
			{
				KeepGizmoHandleLitWhileDragged(*HitTarget);
			}
		}
	}
	// Never let a coordinate-system quirk hide the scale handles again.
	Gizmo->SetIsNonUniformScaleAllowedFunction([]() { return true; });
	// Editor scale-grid snapping often rounds small cutter scale drags to zero
	// (same class of bug as world-grid translation on tiny subjects). Free scale.
	Gizmo->bSnapToScaleGrid = false;
	// Start the gizmo in the mode the toolbar already selected (Move/Rotate/Scale)
	// rather than always Combined until the next context tick.
	Gizmo->bUseContextGizmoMode = true;
	switch (CSGTransformMode)
	{
	case EUbstreamCSGTransformMode::Rotate:
		Gizmo->ActiveGizmoMode = EToolContextTransformGizmoMode::Rotation;
		break;
	case EUbstreamCSGTransformMode::Scale:
		Gizmo->ActiveGizmoMode = EToolContextTransformGizmoMode::Scale;
		break;
	case EUbstreamCSGTransformMode::Move:
	default:
		Gizmo->ActiveGizmoMode = EToolContextTransformGizmoMode::Translation;
		break;
	}
	if (Targets.Num() == 1)
	{
		Gizmo->ReinitializeGizmoTransform(GetStageGizmoAnchorTransform(Targets[0]));
	}
	UE_LOG(LogUbstreamModeler, Verbose,
		TEXT("Stage transform gizmo created for %d target(s), first=%s"),
		Targets.Num(),
		Targets.Num() > 0 ? *GetNameSafe(Targets[0]) : TEXT("none"));

	// Unreal's original gizmo look (per user preference) with generous hit
	// zones, but doubled line thickness: the stock 2px axes read too faint
	// over the stage, especially against the op-colored cutter wireframes.
	if (ACombinedTransformGizmoActor* GizmoActor = Gizmo->GetGizmoActor())
	{
		constexpr float GizmoLineThickness = 4.0f;
		GizmoActor->ForEachComponent(/*bIncludeFromChildActors*/ false, [](UActorComponent* ActorComponent)
		{
			// The gizmo is interaction chrome: on-screen only, never streamed.
			if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(ActorComponent))
			{
				Primitive->SetHiddenInSceneCapture(true);
			}
			if (UGizmoBaseComponent* GizmoComponent = Cast<UGizmoBaseComponent>(ActorComponent))
			{
				GizmoComponent->PixelHitDistanceThreshold = 14.0f;
				if (UGizmoArrowComponent* Arrow = Cast<UGizmoArrowComponent>(GizmoComponent))
				{
					Arrow->Thickness = GizmoLineThickness;
				}
				else if (UGizmoCircleComponent* Circle = Cast<UGizmoCircleComponent>(GizmoComponent))
				{
					Circle->Thickness = GizmoLineThickness;
				}
				else if (UGizmoRectangleComponent* Rectangle = Cast<UGizmoRectangleComponent>(GizmoComponent))
				{
					Rectangle->Thickness = GizmoLineThickness;
				}
				else if (UGizmoLineHandleComponent* LineHandle = Cast<UGizmoLineHandleComponent>(GizmoComponent))
				{
					LineHandle->Thickness = GizmoLineThickness;
				}
				else if (UGizmoBoxComponent* Box = Cast<UGizmoBoxComponent>(GizmoComponent))
				{
					Box->LineThickness = GizmoLineThickness;
				}
				GizmoComponent->MarkRenderStateDirty();
			}
		});
	}

	StageTransformGizmo = Gizmo;
	StageTransformGizmoProxy = Proxy;

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SUbstreamModelerViewport::SetStageTransformGizmoVisible(const bool bVisible)
{
	if (UCombinedTransformGizmo* Gizmo = StageTransformGizmo.Get())
	{
		Gizmo->SetVisibility(bVisible);
	}
}

void SUbstreamModelerViewport::DestroyStageTransformGizmo()
{
	if (UTransformProxy* Proxy = StageTransformGizmoProxy.Get())
	{
		Proxy->OnTransformChanged.RemoveAll(this);
	}

	if (StageTransformGizmo.IsValid())
	{
		UUbstreamStageEditorMode* Mode = GetStageEditorMode();
		UEditorInteractiveToolsContext* ToolsContext = Mode ? Mode->GetStageToolsContext() : nullptr;
		if (ToolsContext && ToolsContext->GizmoManager)
		{
			ToolsContext->GizmoManager->DestroyAllGizmosByOwner(this);
		}
	}

	StageTransformGizmo.Reset();
	StageTransformGizmoProxy.Reset();
	StageTransformGizmoTargets.Reset();
}

void SUbstreamModelerViewport::HandleGizmoTransformChanged(UTransformProxy* Proxy, FTransform Transform)
{
	++GizmoTransformChangeCount;

	// Drag-time bisection telemetry (cheap; logs once per second while hot).
	static double HandlerWindowStart = 0.0;
	static double HandlerTotalMs = 0.0, PushMs = 0.0, NotifyMs = 0.0;
	static int32 HandlerCalls = 0;
	const double HandlerStart = FPlatformTime::Seconds();

	// This fires for EVERY mouse delta of a drag, so it must stay minimal.
	// Deliberately NOT done here: forcing a capture-mirror recopy (cutters are
	// never mirrored, and subject mesh changes are fingerprint-detected) and
	// panel-wide editing-state refreshes (nothing a drag changes needs them).
	// Both used to run per delta and made large-mesh drags crawl.

	// Dragging a session brush is what drives the live preview: hand the new
	// transforms to the session and let latest-wins pacing do the rest.
	const double PushStart = FPlatformTime::Seconds();
	PushSessionBrushTransforms();
	PushMs += (FPlatformTime::Seconds() - PushStart) * 1000.0;

	// Moving a stage component (not a cutter) is a real edit to the model.
	for (const TWeakObjectPtr<UMeshComponent>& Target : StageTransformGizmoTargets)
	{
		if (Target.IsValid() && !IsCSGPrimitiveComponent(Target.Get()))
		{
			bHasUncommittedStageEdits = true;
			break;
		}
	}

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	// Do not call OnSoloMeshChanged here: it rebuilds capture-workspace UI and
	// can force heavy work every mouse delta, which made rotate/scale gizmo
	// chrome feel unstable. Live CSG already dirties via PushSessionBrushTransforms;
	// stream capture picks up mesh changes on the next CSG preview ready path.
	const double NotifyStart = FPlatformTime::Seconds();
	NotifyMs += (FPlatformTime::Seconds() - NotifyStart) * 1000.0;

	++HandlerCalls;
	HandlerTotalMs += (FPlatformTime::Seconds() - HandlerStart) * 1000.0;
	if (HandlerWindowStart <= 0.0)
	{
		HandlerWindowStart = FPlatformTime::Seconds();
	}
	else if (FPlatformTime::Seconds() - HandlerWindowStart > 1.0)
	{
		UE_LOG(LogUbstreamModeler, Log,
			TEXT("UbstreamGizmoHandlerProfile: %d calls | handler total avg %.2fms | push avg %.3fms | notify avg %.3fms"),
			HandlerCalls,
			HandlerCalls > 0 ? HandlerTotalMs / HandlerCalls : 0.0,
			HandlerCalls > 0 ? PushMs / HandlerCalls : 0.0,
			HandlerCalls > 0 ? NotifyMs / HandlerCalls : 0.0);
		HandlerWindowStart = FPlatformTime::Seconds();
		HandlerTotalMs = PushMs = NotifyMs = 0.0;
		HandlerCalls = 0;
	}
}

bool SUbstreamModelerViewport::CanAcceptActiveStageTool() const
{
	if (UUbstreamStageEditorMode* Mode = GetStageEditorMode())
	{
		return Mode->CanAcceptActiveTool();
	}

	return false;
}

void SUbstreamModelerViewport::EndActiveEditTool(bool bAccept)
{
	if (bAccept && !CanAcceptActiveStageTool())
	{
		return;
	}

	if (UUbstreamStageEditorMode* Mode = GetStageEditorMode())
	{
		RunWithPreviewWorldInactive(PreviewScene, [Mode, bAccept]()
		{
			Mode->EndActiveTool(bAccept);
		});
		RefreshCSGPrimitiveVisuals();
		RefreshToolPreviewVisibility();
		OnEditingStateChanged.ExecuteIfBound();
	}
}

FString SUbstreamModelerViewport::GetActiveEditToolIdentifier() const
{
	UUbstreamStageEditorMode* Mode = GetStageEditorMode();
	return Mode ? Mode->GetActiveToolIdentifier() : FString();
}

namespace
{
	EMeshVertexSculptBrushType ToVertexSculptBrushType(EUbstreamSculptBrush Brush)
	{
		switch (Brush)
		{
		case EUbstreamSculptBrush::Move:    return EMeshVertexSculptBrushType::Move;
		case EUbstreamSculptBrush::Draw:    return EMeshVertexSculptBrushType::Offset;
		case EUbstreamSculptBrush::Smooth:  return EMeshVertexSculptBrushType::Smooth;
		case EUbstreamSculptBrush::Inflate: return EMeshVertexSculptBrushType::Inflate;
		case EUbstreamSculptBrush::Flatten: return EMeshVertexSculptBrushType::Flatten;
		case EUbstreamSculptBrush::Pinch:   return EMeshVertexSculptBrushType::Pinch;
		case EUbstreamSculptBrush::Clay:    return EMeshVertexSculptBrushType::SculptMax;
		case EUbstreamSculptBrush::Scrape:  return EMeshVertexSculptBrushType::Plane;
		case EUbstreamSculptBrush::Grab:    return EMeshVertexSculptBrushType::PullKelvin;
		}
		return EMeshVertexSculptBrushType::Offset;
	}

	/** Reverse of ToVertexSculptBrushType, for the brushes we expose; unset when
	    the live tool is on a brush our panel does not surface (e.g. a keyboard
	    cycle landed on one we hide). */
	TOptional<EUbstreamSculptBrush> FromVertexSculptBrushType(EMeshVertexSculptBrushType Brush)
	{
		switch (Brush)
		{
		case EMeshVertexSculptBrushType::Move:       return EUbstreamSculptBrush::Move;
		case EMeshVertexSculptBrushType::Offset:     return EUbstreamSculptBrush::Draw;
		case EMeshVertexSculptBrushType::Smooth:     return EUbstreamSculptBrush::Smooth;
		case EMeshVertexSculptBrushType::Inflate:    return EUbstreamSculptBrush::Inflate;
		case EMeshVertexSculptBrushType::Flatten:    return EUbstreamSculptBrush::Flatten;
		case EMeshVertexSculptBrushType::Pinch:      return EUbstreamSculptBrush::Pinch;
		case EMeshVertexSculptBrushType::SculptMax:  return EUbstreamSculptBrush::Clay;
		case EMeshVertexSculptBrushType::Plane:      return EUbstreamSculptBrush::Scrape;
		case EMeshVertexSculptBrushType::PullKelvin: return EUbstreamSculptBrush::Grab;
		default: return TOptional<EUbstreamSculptBrush>();
		}
	}
}

bool SUbstreamModelerViewport::CanSculpt() const
{
	if (IsRemeshing()) return false;
	if (!HasEditableSource())
	{
		return false;
	}

	// Sculpting deforms one base surface; it needs exactly one source mesh, the
	// same single-subject rule the CSG workflow uses.
	TArray<UMeshComponent*> SourceComponentsForEditing;
	AppendSourcePreviewComponents(SourceComponentsForEditing);
	return SourceComponentsForEditing.Num() == 1;
}

bool SUbstreamModelerViewport::IsSculptActive() const
{
	return GetActiveEditToolIdentifier() == UUbstreamStageEditorMode::VertexSculptToolIdentifier;
}

UMeshVertexSculptTool* SUbstreamModelerViewport::GetActiveVertexSculptTool() const
{
	if (GetActiveEditToolIdentifier() != UUbstreamStageEditorMode::VertexSculptToolIdentifier)
	{
		return nullptr;
	}
	return Cast<UMeshVertexSculptTool>(GetActiveEditToolInstance());
}

UMeshSculptBrushOpProps* SUbstreamModelerViewport::GetActiveSculptBrushOpProps() const
{
	UMeshVertexSculptTool* Tool = GetActiveVertexSculptTool();
	if (!Tool)
	{
		return nullptr;
	}

	// The active brush's op property set is added to the tool's property
	// sources when the brush becomes active; that is where Strength lives.
	for (UObject* PropObject : Tool->GetToolProperties(/*bEnabledOnly*/ true))
	{
		if (UMeshSculptBrushOpProps* Props = Cast<UMeshSculptBrushOpProps>(PropObject))
		{
			return Props;
		}
	}
	return nullptr;
}

void SUbstreamModelerViewport::ApplySculptSettingsToActiveTool()
{
	UMeshVertexSculptTool* Tool = GetActiveVertexSculptTool();
	if (!Tool)
	{
		return;
	}

	// Brush type first: it swaps the visible op prop set that Strength targets.
	const int32 BrushId = static_cast<int32>(ToVertexSculptBrushType(SculptBrush));
	// The setter compares against PrimaryBrushID; assigning that field first
	// skips the operation swap and applies our settings to the previous brush.
	Tool->SetActiveBrushType(BrushId);

	if (Tool->BrushProperties)
	{
		Tool->BrushProperties->BrushSize.SizeType = EBrushToolSizeType::Adaptive;
		Tool->BrushProperties->BrushSize.AdaptiveSize = SculptBrushSize;
		Tool->BrushProperties->BrushFalloffAmount = SculptBrushFalloff;
	}

	SetSculptBrushStrength(SculptBrushStrength);
	SetSculptBrushFalloff(SculptBrushFalloff);

	if (Tool->SymmetryProperties && Tool->SymmetryProperties->bSymmetryCanBeEnabled)
	{
		Tool->SymmetryProperties->bEnableSymmetry = bSculptSymmetryEnabled;
	}
}

bool SUbstreamModelerViewport::StartSculpt(EUbstreamSculptBrush Brush)
{
	if (!CanSculpt())
	{
		return false;
	}

	SculptBrush = Brush;

	// Already sculpting: just switch the live brush and re-push settings.
	if (IsSculptActive())
	{
		ApplySculptSettingsToActiveTool();
		OnEditingStateChanged.ExecuteIfBound();
		return true;
	}

	FString EditableSourceError;
	if (!EnsureEngineAssetSourcesAreEditable(EditableSourceError))
	{
		UE_LOG(LogUbstreamModeler, Warning, TEXT("StartSculpt refused: %s"), *EditableSourceError);
		return false;
	}

	if (!bEditModeActive && !EnterEditMode())
	{
		UE_LOG(LogUbstreamModeler, Warning, TEXT("StartSculpt refused: stage edit mode failed to activate"));
		return false;
	}

	// Bake any live cut into the base first, so you sculpt exactly the shape you
	// see on screen (holes/notches stay). A true live-cut-while-sculpting preview
	// proved too fragile to drive the display; this "hard apply" is the reliable
	// WYSIWYG behaviour, and the bake is undoable. Cutters are consumed - add new
	// ones afterwards on the sculpted result.
	if (GetCSGPrimitiveCount() > 0 && HasActiveCSGSession())
	{
		if (!CanCommitCSGSession())
		{
			// The cut is still computing; try again in a moment.
			return false;
		}
		CommitCSGSession();
	}

	if (!StartEditTool(UUbstreamStageEditorMode::VertexSculptToolIdentifier))
	{
		return false;
	}

	// The tool start is deferred a tick; settings get pushed when it resolves
	// (HandleModeToolStateChanged). Try now too in case it went live at once.
	ApplySculptSettingsToActiveTool();
	return true;
}

void SUbstreamModelerViewport::SetCSGPrimitiveComponentsHidden(bool bHidden)
{
	TArray<UMeshComponent*> Cutters;
	GetCSGPrimitiveComponents(Cutters);
	for (UMeshComponent* Cutter : Cutters)
	{
		if (IsValid(Cutter))
		{
			Cutter->SetVisibility(!bHidden, /*bPropagateToChildren*/ true);
		}
	}
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SUbstreamModelerViewport::StopSculpt(bool bAccept)
{
	if (!IsSculptActive())
	{
		return;
	}
	EndActiveEditTool(bAccept);
}

void SUbstreamModelerViewport::SetSculptBrushSize(float Normalized)
{
	SculptBrushSize = FMath::Clamp(Normalized, 0.0f, 1.0f);
	if (UMeshVertexSculptTool* Tool = GetActiveVertexSculptTool())
	{
		if (Tool->BrushProperties)
		{
			Tool->BrushProperties->BrushSize.SizeType = EBrushToolSizeType::Adaptive;
			Tool->BrushProperties->BrushSize.AdaptiveSize = SculptBrushSize;
		}
	}
}

void SUbstreamModelerViewport::SetSculptBrushStrength(float Normalized)
{
	SculptBrushStrength = FMath::Clamp(Normalized, 0.0f, 1.0f);
	if (UMeshSculptBrushOpProps* OpProps = GetActiveSculptBrushOpProps())
	{
		OpProps->SetStrength(SculptBrushStrength);
	}
}

void SUbstreamModelerViewport::SetSculptBrushFalloff(float Normalized)
{
	SculptBrushFalloff = FMath::Clamp(Normalized, 0.0f, 1.0f);
	// Vertex sculpt reads falloff from the active operation, not the legacy
	// BrushProperties field. Keep both in sync for the panel and engine tools.
	if (UMeshSculptBrushOpProps* OpProps = GetActiveSculptBrushOpProps())
	{
		if (FFloatProperty* FalloffProp = FindFProperty<FFloatProperty>(OpProps->GetClass(), TEXT("Falloff")))
		{
			FalloffProp->SetPropertyValue_InContainer(OpProps, SculptBrushFalloff);
		}
	}
	if (UMeshVertexSculptTool* Tool = GetActiveVertexSculptTool())
	{
		if (Tool->BrushProperties)
		{
			Tool->BrushProperties->BrushFalloffAmount = SculptBrushFalloff;
		}
	}
}

bool SUbstreamModelerViewport::CanEnableSculptSymmetry() const
{
	UMeshVertexSculptTool* Tool = GetActiveVertexSculptTool();
	return Tool && Tool->SymmetryProperties && Tool->SymmetryProperties->bSymmetryCanBeEnabled;
}

void SUbstreamModelerViewport::SetSculptSymmetryEnabled(bool bEnabled)
{
	bSculptSymmetryEnabled = bEnabled;
	if (UMeshVertexSculptTool* Tool = GetActiveVertexSculptTool())
	{
		if (Tool->SymmetryProperties && Tool->SymmetryProperties->bSymmetryCanBeEnabled)
		{
			Tool->SymmetryProperties->bEnableSymmetry = bEnabled;
		}
	}
}

EUbstreamSculptBrush SUbstreamModelerViewport::GetSculptBrush() const
{
	if (const UMeshVertexSculptTool* Tool = GetActiveVertexSculptTool())
	{
		if (Tool->SculptProperties)
		{
			const TOptional<EUbstreamSculptBrush> LiveBrush = FromVertexSculptBrushType(
				static_cast<EMeshVertexSculptBrushType>(Tool->SculptProperties->PrimaryBrushID));
			if (LiveBrush.IsSet())
			{
				return LiveBrush.GetValue();
			}
		}
	}
	return SculptBrush;
}

float SUbstreamModelerViewport::GetSculptBrushSize() const
{
	if (const UMeshVertexSculptTool* Tool = GetActiveVertexSculptTool())
	{
		if (Tool->BrushProperties)
		{
			return Tool->BrushProperties->BrushSize.AdaptiveSize;
		}
	}
	return SculptBrushSize;
}

float SUbstreamModelerViewport::GetSculptBrushStrength() const
{
	if (UMeshSculptBrushOpProps* OpProps = GetActiveSculptBrushOpProps())
	{
		return OpProps->GetStrength();
	}
	return SculptBrushStrength;
}

float SUbstreamModelerViewport::GetSculptBrushFalloff() const
{
	if (const UMeshVertexSculptTool* Tool = GetActiveVertexSculptTool())
	{
		if (Tool->BrushProperties)
		{
			return Tool->BrushProperties->BrushFalloffAmount;
		}
	}
	return SculptBrushFalloff;
}

bool SUbstreamModelerViewport::CanRemeshSubject() const
{
	if (IsRemeshing()) return false;
	if (HasAnyActiveStageTool() && !IsSculptActive()) return false;
	if (!HasEditableSource())
	{
		return false;
	}
	TArray<UMeshComponent*> Sources;
	AppendSourcePreviewComponents(Sources);
	return Sources.Num() == 1;
}

int32 SUbstreamModelerViewport::GetSubjectTriangleCount() const
{
	TArray<UMeshComponent*> Sources;
	AppendSourcePreviewComponents(Sources);
	if (Sources.Num() != 1)
	{
		return 0;
	}

	int32 TriangleCount = 0;
	if (const UDynamicMeshComponent* DynamicSource = Cast<UDynamicMeshComponent>(Sources[0]))
	{
		DynamicSource->ProcessMesh([&TriangleCount](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			TriangleCount = Mesh.TriangleCount();
		});
	}
	else if (const UStaticMeshComponent* StaticSource = Cast<UStaticMeshComponent>(Sources[0]))
	{
		if (const UStaticMesh* StaticMesh = StaticSource->GetStaticMesh())
		{
			TriangleCount = StaticMesh->GetNumTriangles(0);
		}
	}
	return TriangleCount;
}

void SUbstreamModelerViewport::SetSculptTargetTriangleCount(int32 TargetTriangleCount)
{
	SculptTargetTriangleCount = FMath::Clamp(
		TargetTriangleCount, GetSculptTargetTriangleMin(), GetSculptTargetTriangleMax());
}

bool SUbstreamModelerViewport::RemeshSubjectToTargetDetail(FString& OutErrorMessage)
{
	OutErrorMessage.Reset();
	if (IsRemeshing())
	{
		OutErrorMessage = TEXT("A remesh is already running. Wait for it or cancel it first.");
		return false;
	}
	if (HasAnyActiveStageTool() && !IsSculptActive())
	{
		OutErrorMessage = TEXT("Finish or cancel the current editing tool before remeshing.");
		return false;
	}
	if (!CanRemeshSubject())
	{
		OutErrorMessage = TEXT("Load a single mesh first.");
		return false;
	}

	FString EditableSourceError;
	if (!EnsureEngineAssetSourcesAreEditable(EditableSourceError))
	{
		OutErrorMessage = EditableSourceError;
		return false;
	}

	if (!bEditModeActive && !EnterEditMode())
	{
		OutErrorMessage = TEXT("The stage edit mode failed to activate.");
		return false;
	}

	// Finish sculpting before remeshing. Preserve any cutter stack and
	// re-apply it to the denser result below.
	const bool bHadCutters = GetCSGPrimitiveCount() > 0;
	if (IsSculptActive())
	{
		if (UUbstreamStageEditorMode* Mode = GetStageEditorMode())
		{
			RunWithPreviewWorldInactive(PreviewScene, [Mode]()
			{
				Mode->EndActiveToolImmediate(/*bAccept*/ true);
			});
		}

	}

	TArray<UMeshComponent*> Sources;
	AppendSourcePreviewComponents(Sources);
	UDynamicMeshComponent* Subject = (Sources.Num() == 1) ? Cast<UDynamicMeshComponent>(Sources[0]) : nullptr;
	if (!Subject)
	{
		OutErrorMessage = TEXT("The stage holds no editable mesh to remesh.");
		return false;
	}

	const int32 TargetTriangles = FMath::Clamp(
		SculptTargetTriangleCount, GetSculptTargetTriangleMin(), GetSculptTargetTriangleMax());

	TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> InputMesh =
		MakeShared<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>();
	Subject->ProcessMesh([&InputMesh](const UE::Geometry::FDynamicMesh3& Mesh)
	{
		*InputMesh = Mesh;
	});
	if (InputMesh->TriangleCount() == 0)
	{
		OutErrorMessage = TEXT("The mesh is empty.");
		return false;
	}

	// The engine's CalculateTargetEdgeLength rounds to 0.01 world units.
	// On small meshes that becomes zero, causing runaway subdivision. Keep
	// the full precision and reject geometry with no finite surface area.
	double SurfaceArea = 0.0;
	for (int32 TriangleID : InputMesh->TriangleIndicesItr())
	{
		SurfaceArea += InputMesh->GetTriArea(TriangleID);
	}
	const double TargetEdgeLength = FMath::Sqrt(4.0 * (SurfaceArea / TargetTriangles) / UE_DOUBLE_SQRT_3);
	if (!FMath::IsFinite(TargetEdgeLength) || TargetEdgeLength <= 0.0)
	{
		OutErrorMessage = TEXT("The mesh has no finite surface area to remesh.");
		return false;
	}

	const auto Job = MakeShared<FUbstreamRemeshJob, ESPMode::ThreadSafe>();
	Job->Input = InputMesh;
	RemeshJob = Job;
	RemeshSubject = Subject;
	RemeshStatus = TEXT("Remeshing… You can keep navigating the view.");
	// Any external geometry change invalidates the snapshot, including edits
	// that don't pass through the stage UI. Remove this delegate before apply.
	RemeshMeshChangedHandle = Subject->OnMeshChanged.AddSP(this, &SUbstreamModelerViewport::CancelRemesh);
	const TWeakPtr<SUbstreamModelerViewport> WeakThis = SharedThis(this);
	Job->Start(TargetEdgeLength, [WeakThis, Job, bHadCutters]()
	{
		if (const auto Viewport = WeakThis.Pin()) Viewport->FinishRemesh(Job, bHadCutters);
	});
	OnRemeshStateChanged.Broadcast();
	OnEditingStateChanged.ExecuteIfBound();
	return true;
}

void SUbstreamModelerViewport::CancelRemesh()
{
	if (RemeshJob)
	{
		RemeshJob->Cancel();
		RemeshStatus = TEXT("Cancelling remesh…");
	}
}

void SUbstreamModelerViewport::AbandonRemesh()
{
	const bool bWasRemeshing = IsRemeshing();
	CancelRemesh();
	if (UDynamicMeshComponent* Subject = RemeshSubject.Get())
	{
		Subject->OnMeshChanged.Remove(RemeshMeshChangedHandle);
	}
	RemeshMeshChangedHandle.Reset();
	RemeshSubject.Reset();
	RemeshJob.Reset();
	RemeshStatus.Reset();
	if (bWasRemeshing) OnRemeshStateChanged.Broadcast();
}

void SUbstreamModelerViewport::FinishRemesh(
	const TSharedRef<FUbstreamRemeshJob, ESPMode::ThreadSafe>& Job, bool bHadCutters)
{
	// A closed/replaced stage may already own a newer job. Never let this
	// completion clear it or apply geometry to a different subject.
	if (RemeshJob != Job) return;
	UDynamicMeshComponent* Subject = RemeshSubject.Get();
	const bool bCancelled = Job->IsCancelled();
	AbandonRemesh();
	if (!IsValid(Subject) || !PreviewComponents.Contains(Subject)) return;
	if (bCancelled)
	{
		RemeshStatus = TEXT("Remesh cancelled. Your mesh is unchanged.");
	}
	else if (!Job->Error.IsEmpty())
	{
		RemeshStatus = Job->Error;
	}
	else
	{
		// Keep the live cut visible throughout compute. Replace its base only
		// once a complete result is ready, then rebuild the editable cutter stack.
		if (HasActiveCSGSession()) CancelCSGSession();
		const auto BeforeMesh = Job->Input;
		const TSharedPtr<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> AfterMesh =
			MakeShared<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>(MoveTemp(*Job->Result));

		{
			// UDynamicMesh skips its mesh data in transactions, so record the swap as
			// an explicit replacement change (same pattern as the CSG bake).
			FScopedTransaction Transaction(
				NSLOCTEXT("SUbstreamModelerViewport", "RemeshSubjectTransaction", "Change Surface Detail"));
			Subject->SetFlags(RF_Transactional);
			Subject->Modify();
			if (UDynamicMesh* SubjectDynamicMesh = Subject->GetDynamicMesh())
			{
				SubjectDynamicMesh->SetMesh(*AfterMesh);
			}
			if (GUndo)
			{
				GUndo->StoreUndo(Subject, MakeUnique<FMeshReplacementChange>(BeforeMesh, AfterMesh));
			}
			Subject->NotifyMeshUpdated();
			Subject->UpdateBounds();
			bHasUncommittedStageEdits = true;
		}

		RemeshStatus = TEXT("Surface detail updated. You can undo this change.");
	}

	// Re-apply the editable cutter stack to the remeshed base.
	if (bHadCutters && GetCSGPrimitiveCount() > 0 && !HasActiveCSGSession())
	{
		SelectLatestCSGPrimitiveForEditing();
		StartCSGOperation(GetSelectedCSGOperation());
	}

	bCaptureMirrorRefreshPending = true;
	RefreshToolPreviewVisibility();
	OnSoloMeshChanged.ExecuteIfBound();
	OnEditingStateChanged.ExecuteIfBound();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SUbstreamModelerViewport::HandlePostUndoRedo()
{
	CancelRemesh();
	TGuardValue<bool> UndoGuard(bHandlingUndoRedo, true);
	bCaptureMirrorRefreshPending = true;
	for (UMeshComponent* Component : PreviewComponents)
	{
		if (!IsValid(Component))
		{
			continue;
		}

		if (UDynamicMeshComponent* DynamicComponent = Cast<UDynamicMeshComponent>(Component))
		{
			DynamicComponent->NotifyMeshUpdated();
		}
		Component->UpdateBounds();
	}

	// Undo/redo may have created or destroyed cutters: resync bookkeeping
	// before deciding what the session should look like.
	ReconcileCuttersAfterUndoRedo();

	if (HasActiveCSGSession())
	{
		// A cutter-transform undo just moves brushes: chase them. Anything
		// that rewrote the subject mesh from outside (undoing past the
		// session), or changed the cutter SET, invalidates the snapshot: end
		// the session so the restart below rebuilds from reality.
		UDynamicMeshComponent* Subject = CSGSessionSubject.Get();
		TArray<UMeshComponent*> CurrentCutters;
		AppendCSGPrimitiveComponents(CurrentCutters);
		const bool bStackUnchanged = CurrentCutters.Num() == CSGSessionBrushes.Num();
		const bool bSubjectUntouched = Subject
			&& ComputeDynamicMeshFingerprint(*Subject) == CSGSessionSubjectFingerprint;
		if (bSubjectUntouched && bStackUnchanged)
		{
			PushSessionBrushTransforms();
			// Ops may have been undone too (tag changes).
			for (int32 Index = 0; Index < CSGSessionBrushes.Num(); ++Index)
			{
				if (const UMeshComponent* Brush = CSGSessionBrushes[Index].Get())
				{
					CSGSession->SetBrushOperation(Index, ToKernelCSGOp(GetCSGPrimitiveOperation(Brush)));
				}
			}
		}
		else
		{
			EndCSGSessionInternal();
			RefreshCSGPrimitiveVisuals();
			OnSoloMeshChanged.ExecuteIfBound();
		}
	}

	// Non-destructive model: whenever cutters exist, they should be live.
	// Undoing a bake resurrects the cutters; restarting here returns the
	// stage to its editable state with the same visible shape.
	if (!HasActiveCSGSession() && bEditModeActive)
	{
		TArray<UMeshComponent*> RemainingCutters;
		AppendCSGPrimitiveComponents(RemainingCutters);
		if (!RemainingCutters.IsEmpty())
		{
			SelectLatestCSGPrimitiveForEditing();
			StartCSGOperation(GetSelectedCSGOperation());
		}
	}

	if (!HasAnyActiveStageTool() && !HasActiveCSGSession())
	{
		SetStagePreviewComponentsSuppressed(false);
		FString EditableSourceError;
		EnsureEngineAssetSourcesAreEditable(EditableSourceError);
		if (bEditModeActive)
		{
			TArray<UMeshComponent*> CSGPrimitives;
			AppendCSGPrimitiveComponents(CSGPrimitives);
			if (!CSGPrimitives.IsEmpty())
			{
				SelectLatestCSGPrimitiveForEditing();
			}
			else
			{
				DeselectAllStageComponents();
			}
		}
	}

	RefreshToolPreviewVisibility();
	RefreshCSGPrimitiveVisuals();
	UpdateStageTransformGizmo();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnEditingStateChanged.ExecuteIfBound();
}

void SUbstreamModelerViewport::HandleModeToolStateChanged()
{
	// Any tool start/end may have mutated meshes without a topology change
	// (e.g. an instant apply); the next mirror sync must do a full recopy.
	bCaptureMirrorRefreshPending = true;
	if (!HasAnyActiveStageTool())
	{
		FString EditableSourceError;
		EnsureEngineAssetSourcesAreEditable(EditableSourceError);
	}
	// A freshly-resolved vertex sculpt tool starts on its own cached palette
	// state; push our panel's brush + size/strength/falloff/symmetry onto it.
	if (GetActiveVertexSculptTool())
	{
		ApplySculptSettingsToActiveTool();
		MakeActiveSculptToolTransactional();
	}
	// The sculpt tool just ended (from Done, Cancel, Esc or Enter) and it had
	// suspended a live cut: re-apply that cut to the sculpted base now.
	RefreshToolPreviewVisibility();
	UpdateStageTransformGizmo();
	OnEditingStateChanged.ExecuteIfBound();
}

void SUbstreamModelerViewport::HandleHostedToolkitChanged()
{
	OnEditingStateChanged.ExecuteIfBound();
}

void SUbstreamModelerViewport::HandleHostOverlayAdded(const TSharedRef<SWidget>& OverlayWidget)
{
	if (ViewportOverlay.IsValid())
	{
		ViewportOverlay->AddSlot()
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Fill)
			[
				OverlayWidget
			];
	}
}

void SUbstreamModelerViewport::HandleHostOverlayRemoved(const TSharedRef<SWidget>& OverlayWidget)
{
	if (ViewportOverlay.IsValid())
	{
		ViewportOverlay->RemoveSlot(OverlayWidget);
	}
}

bool SUbstreamModelerViewport::ShouldCaptureWholeScene() const
{
	return HasActiveEditTool();
}

bool SUbstreamModelerViewport::ApplyEditsToSource(FString& OutErrorMessage)
{
	if (!HasSoloMesh())
	{
		OutErrorMessage = TEXT("No isolated subject is loaded.");
		return false;
	}

	// Non-destructive stack: cuts stay live until finality is needed. Writing
	// back to the source asset is that moment - bake implicitly.
	if (HasActiveCSGSession())
	{
		if (!CanCommitCSGSession())
		{
			OutErrorMessage = IsCSGSessionComputing()
				? TEXT("The live cut is still computing; try again in a moment.")
				: !CSGSessionMessage.IsEmpty() ? CSGSessionMessage
				: TEXT("The live cut has no valid result. Adjust the cutter before applying to the source.");
			return false;
		}
		CommitCSGSession();
	}

	if (SourceKind == ESourceKind::None || SourceKind == ESourceKind::GeneratedPreview)
	{
		OutErrorMessage = TEXT("This subject has no source to apply to.");
		return false;
	}

	// A live CSG preview must resolve before the previews are read: a ready
	// result commits (it is what the user is looking at), anything else cancels.
	if (HasActiveCSGSession())
	{
		if (CanCommitCSGSession())
		{
			CommitCSGSession();
		}
		else
		{
			CancelCSGSession();
		}
	}

	// Close any active tool synchronously so the preview meshes hold the final
	// result before we read them.
	if (UUbstreamStageEditorMode* Mode = GetStageEditorMode())
	{
		RunWithPreviewWorldInactive(PreviewScene, [Mode]()
		{
			Mode->EndActiveToolImmediate(/*bAccept*/ true);
		});
		OnEditingStateChanged.ExecuteIfBound();
	}

	struct FApplyPair
	{
		const UE::Geometry::FDynamicMesh3* EditedMesh = nullptr;
		const UMeshComponent* PreviewComponent = nullptr;
		UStaticMesh* TargetAsset = nullptr;
		UDynamicMeshComponent* TargetDynamicComponent = nullptr;
		UStaticMeshComponent* RetargetComponent = nullptr;
		bool bCreateNewAsset = false;
	};
	TArray<FApplyPair> ApplyPairs;

	auto GetEditedMesh = [](const UMeshComponent* PreviewComponent) -> const UE::Geometry::FDynamicMesh3*
	{
		const UDynamicMeshComponent* DynamicPreview = Cast<UDynamicMeshComponent>(PreviewComponent);
		return DynamicPreview ? DynamicPreview->GetMesh() : nullptr;
	};

	TArray<UMeshComponent*> SourcePreviewComponents;
	AppendSourcePreviewComponents(SourcePreviewComponents);

	if (SourceKind == ESourceKind::StaticMeshAsset)
	{
		if (SourcePreviewComponents.Num() != 1 || !IsValid(SoloMesh.Get()))
		{
			OutErrorMessage = TEXT("The source asset is no longer available.");
			return false;
		}

		FApplyPair Pair;
		Pair.EditedMesh = GetEditedMesh(SourcePreviewComponents[0]);
		Pair.PreviewComponent = SourcePreviewComponents[0];
		Pair.TargetAsset = SoloMesh.Get();
		if (!Pair.EditedMesh)
		{
			OutErrorMessage = TEXT("The stage holds no editable working copy. Enter edit mode first.");
			return false;
		}
		ApplyPairs.Add(Pair);
	}
	else
	{
		if (SourceComponents.Num() != SourcePreviewComponents.Num())
		{
			OutErrorMessage = TEXT("Apply is not supported for instanced subjects yet.");
			return false;
		}

		for (int32 Index = 0; Index < SourcePreviewComponents.Num(); ++Index)
		{
			UMeshComponent* SourceComponent = SourceComponents[Index].Get();
			if (!IsValid(SourceComponent))
			{
				OutErrorMessage = TEXT("A source component was deleted; cannot apply.");
				return false;
			}

			FApplyPair Pair;
			Pair.EditedMesh = GetEditedMesh(SourcePreviewComponents[Index]);
			Pair.PreviewComponent = SourcePreviewComponents[Index];
			if (!Pair.EditedMesh)
			{
				OutErrorMessage = TEXT("The stage holds no editable working copy. Enter edit mode first.");
				return false;
			}

			if (UDynamicMeshComponent* DynamicSource = Cast<UDynamicMeshComponent>(SourceComponent))
			{
				Pair.TargetDynamicComponent = DynamicSource;
			}
			else if (UStaticMeshComponent* StaticSource = Cast<UStaticMeshComponent>(SourceComponent))
			{
				Pair.TargetAsset = StaticSource->GetStaticMesh();
				Pair.RetargetComponent = StaticSource;
			}

			if (!Pair.TargetAsset && !Pair.TargetDynamicComponent)
			{
				OutErrorMessage = TEXT("A source component has no editable mesh.");
				return false;
			}
			ApplyPairs.Add(Pair);
		}
	}

	// Built-in engine assets cannot be modified; redirect those edits into a
	// new project asset instead and rewire the source to it.
	for (FApplyPair& Pair : ApplyPairs)
	{
		if (Pair.TargetAsset && Pair.TargetAsset->GetPathName().StartsWith(TEXT("/Engine/")))
		{
			Pair.bCreateNewAsset = true;
		}
	}

	const FScopedTransaction Transaction(NSLOCTEXT("SUbstreamModelerViewport", "ApplyStageEdits", "Apply Unbound Loop Stage Edits"));

	TArray<FString> CreatedAssetNames;
	for (const FApplyPair& Pair : ApplyPairs)
	{
		if (Pair.bCreateNewAsset)
		{
			TArray<UMaterialInterface*> Materials;
			if (Pair.PreviewComponent)
			{
				for (int32 MaterialIndex = 0; MaterialIndex < Pair.PreviewComponent->GetNumMaterials(); ++MaterialIndex)
				{
					Materials.Add(Pair.PreviewComponent->GetMaterial(MaterialIndex));
				}
			}

			UStaticMesh* NewAsset = CreateProjectAssetFromDynamicMesh(*Pair.EditedMesh, Pair.TargetAsset->GetName(), Materials);
			if (!NewAsset)
			{
				OutErrorMessage = FString::Printf(
					TEXT("%s is a built-in engine asset and saving a project copy failed."),
					*Pair.TargetAsset->GetName());
				return false;
			}

			CreatedAssetNames.Add(NewAsset->GetPathName());

			if (Pair.RetargetComponent)
			{
				Pair.RetargetComponent->Modify();
				Pair.RetargetComponent->SetStaticMesh(NewAsset);
			}

			if (SourceKind == ESourceKind::StaticMeshAsset)
			{
				SoloMesh = NewAsset;
				SoloMeshLabel = NewAsset->GetName();
				SoloMeshPath = NewAsset->GetPathName();
			}
		}
		else if (Pair.TargetDynamicComponent)
		{
			Pair.TargetDynamicComponent->Modify();
			if (UDynamicMesh* TargetMesh = Pair.TargetDynamicComponent->GetDynamicMesh())
			{
				TargetMesh->Modify();
				TargetMesh->SetMesh(UE::Geometry::FDynamicMesh3(*Pair.EditedMesh));
			}
		}
		else if (Pair.TargetAsset)
		{
			if (!CommitDynamicMeshToStaticMeshAsset(*Pair.EditedMesh, Pair.TargetAsset))
			{
				OutErrorMessage = FString::Printf(TEXT("Failed to write the edited mesh into %s."), *Pair.TargetAsset->GetName());
				return false;
			}
		}
	}

	if (!CreatedAssetNames.IsEmpty())
	{
		OutErrorMessage = FString::Printf(
			TEXT("The source was a built-in engine asset, so the edit was saved as %s%s."),
			*FString::Join(CreatedAssetNames, TEXT(", ")),
			SourceKind != ESourceKind::StaticMeshAsset ? TEXT(" and the source component now uses it") : TEXT(""));
	}

	// The source now matches the stage; re-baseline so auto-resync does not
	// immediately re-clone over the live edit session.
	SourceSignatureAtLoad = BuildSourceSignature();
	LastSourceDirtyCheckSeconds = -1.0;
	bHasUncommittedStageEdits = false;
	OnEditingStateChanged.ExecuteIfBound();
	return true;
}

UUbstreamStageEditorMode* SUbstreamModelerViewport::GetStageEditorMode() const
{
	if (!StageModeManager.IsValid())
	{
		return nullptr;
	}

	return Cast<UUbstreamStageEditorMode>(
		StageModeManager->GetActiveScriptableMode(UUbstreamStageEditorMode::EM_UbstreamStageEditorModeId));
}

bool SUbstreamModelerViewport::ConvertStaticPreviewsToEditable()
{
	if (!PreviewScene.IsValid())
	{
		return false;
	}

	for (int32 Index = 0; Index < PreviewComponents.Num(); ++Index)
	{
		UStaticMeshComponent* StaticComponent = Cast<UStaticMeshComponent>(PreviewComponents[Index]);
		if (!IsValid(StaticComponent) || !StaticComponent->GetStaticMesh())
		{
			continue;
		}

		const FMeshDescription* MeshDescription = StaticComponent->GetStaticMesh()->GetMeshDescription(0);
		if (!MeshDescription)
		{
			continue;
		}

		UE::Geometry::FDynamicMesh3 ConvertedMesh;
		FMeshDescriptionToDynamicMesh Converter;
		// Per-triangle MaterialIDs must index the component's material SLOTS,
		// the way the engine's tool targets convert. Raw polygon-group IDs can
		// exceed the slot count (common for imported OBJs), and the dynamic
		// mesh renderer silently drops triangles whose MaterialID is out of
		// range instead of falling back to a default material.
		Converter.bUseCompactedPolygonGroupIDValues = true;
		Converter.SetPolygonGroupToMaterialIndexMap(UE::Conversion::GetPolygonGroupToMaterialIndexMap(
			StaticComponent->GetStaticMesh(), UE::Conversion::EMeshLODType::SourceModel, 0));
		Converter.Convert(MeshDescription, ConvertedMesh);
		// Generated / imported meshes sometimes land inside-out; without this
		// the stage only shows backfaces after enter-edit conversion. Ray
		// probe: open shells (tubs) defeat the centroid score, and any cut
		// then inherits the inverted orientation into its cavity walls.
		UbstreamMeshOrientation::EnsureOutwardOrientation(ConvertedMesh, /*bRayProbe*/ true);

		UDynamicMeshComponent* EditableComponent = NewObject<UDynamicMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		EditableComponent->SetMesh(MoveTemp(ConvertedMesh));
		CopyCommonMeshComponentSettings(*StaticComponent, *EditableComponent);
		EditableComponent->ComponentTags = StaticComponent->ComponentTags;
		// Safety net: if any MaterialID still exceeds the slot count, grow the
		// slot list rather than letting those triangles disappear.
		EditableComponent->ValidateMaterialSlots(/*bCreateIfMissing*/ true, /*bDeleteExtraSlots*/ false);

		const FTransform PreviewTransform = StaticComponent->GetComponentTransform();
		StaticComponent->DestroyComponent();

		AttachPreviewComponentToStage(EditableComponent, PreviewTransform);
		PreviewComponents[Index] = EditableComponent;
	}

	return HasSoloMesh();
}

AActor* SUbstreamModelerViewport::EnsureStageActor()
{
	if (AActor* ExistingActor = StageActor.Get())
	{
		// Older stages predate transactional cutters; upgrade in place.
		ExistingActor->SetFlags(RF_Transactional);
		return ExistingActor;
	}

	UWorld* PreviewWorld = PreviewScene.IsValid() ? PreviewScene->GetWorld() : nullptr;
	if (!PreviewWorld)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// Transactional: cutter add/delete undo depends on the actor's component
	// list being recorded by Modify() - RF_Transient alone records nothing.
	SpawnParameters.ObjectFlags = RF_Transient | RF_Transactional;
	AActor* NewStageActor = PreviewWorld->SpawnActor<AActor>(SpawnParameters);
	if (!NewStageActor)
	{
		return nullptr;
	}

	USceneComponent* RootComponent = NewObject<USceneComponent>(NewStageActor, TEXT("StageRoot"), RF_Transient);
	NewStageActor->SetRootComponent(RootComponent);
	NewStageActor->AddInstanceComponent(RootComponent);
	RootComponent->RegisterComponent();

	StageActor = NewStageActor;
	return NewStageActor;
}

void SUbstreamModelerViewport::AttachPreviewComponentToStage(UMeshComponent* Component, const FTransform& WorldTransform)
{
	AActor* Actor = EnsureStageActor();
	if (!Actor || !Component)
	{
		return;
	}

	if (Component->GetOuter() != Actor)
	{
		Component->Rename(nullptr, Actor, REN_DontCreateRedirectors | REN_NonTransactional);
	}

	Actor->AddInstanceComponent(Component);
	Component->AttachToComponent(Actor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
	// Editor-style selection outline: preview-world components never receive
	// the editor's implicit selection state, so route the proxy's selected
	// flag through the stage's own selection set (the SCS-editor pattern).
	Component->SelectionOverrideDelegate = UPrimitiveComponent::FSelectionOverride::CreateSP(
		this, &SUbstreamModelerViewport::IsStageComponentSelected);
	Component->RegisterComponent();
	Component->SetWorldTransform(WorldTransform);
	Component->UpdateBounds();
}

bool SUbstreamModelerViewport::IsStageComponentSelected(const UPrimitiveComponent* Component) const
{
	USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	return SelectedComponents && SelectedComponents->IsSelected(const_cast<UPrimitiveComponent*>(Component));
}

void SUbstreamModelerViewport::StoreCutterMeshForUndo(UDynamicMeshComponent* Cutter)
{
	if (!GUndo || !IsValid(Cutter))
	{
		return;
	}
	TSharedPtr<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> MeshCopy;
	Cutter->ProcessMesh([&MeshCopy](const UE::Geometry::FDynamicMesh3& Mesh)
	{
		MeshCopy = MakeShared<const UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>(Mesh);
	});
	// Before == After: the change is a no-op forward, but reverting it puts
	// the geometry back on the resurrected component (whose mesh data the
	// transaction system deliberately skips).
	GUndo->StoreUndo(Cutter, MakeUnique<FMeshReplacementChange>(MeshCopy, MeshCopy));
}

void SUbstreamModelerViewport::ReconcileCuttersAfterUndoRedo()
{
	// Undo/redo mutates the stage actor's component list and object states
	// behind our back; the preview world gets no editor-level fixup, so the
	// stage repairs itself: prune un-added cutters (undone adds), register
	// resurrected ones (undone deletes), and resync PreviewComponents.
	AActor* Stage = StageActor.Get();

	for (int32 Index = PreviewComponents.Num() - 1; Index >= 0; --Index)
	{
		UMeshComponent* Component = PreviewComponents[Index];
		const AActor* Owner = IsValid(Component) ? Component->GetOwner() : nullptr;
		const bool bStillOwned = Owner && Owner->GetInstanceComponents().Contains(Component);
		if (!IsValid(Component) || !bStillOwned)
		{
			// An undone add: the actor no longer lists it. Fully retire the
			// zombie so it stops rendering.
			if (IsValid(Component) && Component->IsRegistered())
			{
				Component->UnregisterComponent();
			}
			PreviewComponents.RemoveAt(Index);
		}
	}

	if (Stage)
	{
		for (UActorComponent* ActorComponent : Stage->GetInstanceComponents())
		{
			UMeshComponent* MeshComponent = Cast<UMeshComponent>(ActorComponent);
			if (!IsCSGPrimitiveComponent(MeshComponent) || !IsValid(MeshComponent))
			{
				continue;
			}
			// An undone delete resurrects the component unregistered in
			// preview worlds; bring it back to life ourselves.
			if (!MeshComponent->IsRegistered())
			{
				MeshComponent->RegisterComponent();
				MeshComponent->UpdateBounds();
			}
			if (!PreviewComponents.Contains(MeshComponent))
			{
				PreviewComponents.Add(MeshComponent);
			}
		}
	}
}

FTransform SUbstreamModelerViewport::GetStageGizmoAnchorTransform(UMeshComponent* Target) const
{
	FTransform Anchor = Target->GetComponentTransform();
	if (!IsCSGPrimitiveComponent(Target))
	{
		// Imported subjects often carry far off-center pivots; anchoring at
		// the visible geometry keeps the gizmo ON the model instead of
		// floating in empty space (drag deltas apply through the proxy
		// regardless of where the gizmo itself stands).
		Target->UpdateBounds();
		Anchor.SetLocation(Target->Bounds.GetBox().GetCenter());
	}
	return Anchor;
}

void SUbstreamModelerViewport::DestroyForeignGizmoActors()
{
	// Active (or starting) ITF tools own their own handles in this world.
	if (HasAnyActiveStageTool())
	{
		return;
	}

	// The stage owns exactly ONE gizmo. Editor subsystems (the new TRS
	// selection gizmo, the legacy mode-manager widget's actor forms) can
	// spawn their own into the preview world depending on per-user editor
	// settings; those render as dead duplicate axes that Z-fight ours and can
	// block our gizmo from re-registering. Anything gizmo-shaped we did not
	// create is destroyed on sight.
	UWorld* StageWorld = PreviewScene.IsValid() ? PreviewScene->GetWorld() : nullptr;
	if (!StageWorld)
	{
		return;
	}

	const AActor* OwnGizmoActor = StageTransformGizmo.IsValid()
		? StageTransformGizmo->GetGizmoActor()
		: nullptr;
	for (TActorIterator<AGizmoActor> It(StageWorld); It; ++It)
	{
		AGizmoActor* GizmoActor = *It;
		if (GizmoActor != OwnGizmoActor && IsValid(GizmoActor))
		{
			UE_LOG(LogUbstreamModeler, Log,
				TEXT("Destroying foreign gizmo actor %s (%s) in the stage world"),
				*GetNameSafe(GizmoActor), *GetNameSafe(GizmoActor->GetClass()));
			GizmoActor->Destroy();
		}
	}
}

void SUbstreamModelerViewport::PushStageSelectionToProxies()
{
	for (UMeshComponent* Component : PreviewComponents)
	{
		if (IsValid(Component))
		{
			Component->PushSelectionToProxy();
		}
	}
}

bool SUbstreamModelerViewport::SelectPreviewComponentsForEditing()
{
	return SelectPreviewComponentsForTool(FString());
}

void SUbstreamModelerViewport::AppendSourcePreviewComponents(TArray<UMeshComponent*>& OutComponents) const
{
	for (UMeshComponent* Component : PreviewComponents)
	{
		if (UbstreamMeshComponents::HasRenderableGeometry(Component) && !IsCSGPrimitiveComponent(Component))
		{
			OutComponents.Add(Component);
		}
	}
}

void SUbstreamModelerViewport::AppendCSGPrimitiveComponents(TArray<UMeshComponent*>& OutComponents) const
{
	for (UMeshComponent* Component : PreviewComponents)
	{
		if (UbstreamMeshComponents::HasRenderableGeometry(Component) && IsCSGPrimitiveComponent(Component))
		{
			OutComponents.Add(Component);
		}
	}
}

int32 SUbstreamModelerViewport::AdoptUntrackedPreviewWorldMeshComponents()
{
	if (!PreviewScene.IsValid())
	{
		return 0;
	}

	UWorld* PreviewWorld = PreviewScene->GetWorld();
	if (!PreviewWorld)
	{
		return 0;
	}

	TSet<UMeshComponent*> KnownComponents;
	for (UMeshComponent* Component : PreviewComponents)
	{
		if (IsValid(Component))
		{
			KnownComponents.Add(Component);
		}
	}

	int32 AdoptedComponentCount = 0;
	TArray<UMeshComponent*> AdoptedComponents;
	for (TActorIterator<AActor> ActorIt(PreviewWorld); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (!IsValid(Actor) || Actor->IsA<AGizmoActor>())
		{
			continue;
		}

		TInlineComponentArray<UMeshComponent*> MeshComponents(Actor);
		for (UMeshComponent* Component : MeshComponents)
		{
			if (!IsValid(Component)
				|| KnownComponents.Contains(Component)
				|| !Component->IsRegistered()
				|| IsEditorGizmoComponent(Component)
				|| IsCSGPrimitiveComponent(Component)
				|| Component->IsA<ULineSetComponent>()
				|| Component->IsA<UPointSetComponent>()
				|| Component->IsA<UTriangleSetComponent>()
				|| IsFullyTranslucentMeshComponent(*Component)
				|| !UbstreamMeshComponents::HasRenderableGeometry(Component))
			{
				continue;
			}

			Component->SetFlags(RF_Transactional);
			Component->SetMobility(EComponentMobility::Movable);
			Component->ComponentTags.Add(FName(*FString::Printf(TEXT("solo_created_component_%03d"), PreviewComponents.Num() + 1)));
			Component->UpdateBounds();
			PreviewComponents.Add(Component);
			KnownComponents.Add(Component);
			AdoptedComponents.Add(Component);
			++AdoptedComponentCount;
		}
	}

	if (AdoptedComponentCount > 0)
	{
		bHasUncommittedStageEdits = true;
		bCaptureMirrorRefreshPending = true;
		if (USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr)
		{
			SelectedComponents->DeselectAll();
			for (UMeshComponent* AdoptedComponent : AdoptedComponents)
			{
				if (IsValid(AdoptedComponent))
				{
					SelectedComponents->Select(AdoptedComponent);
				}
			}
			SelectedComponents->NoteSelectionChanged();
			StageModeManager->SetWidgetMode(UE::Widget::WM_Translate);
		}
		UpdateStageTransformGizmo();
		if (ViewportClient.IsValid())
		{
			ViewportClient->Invalidate();
		}
		OnSoloMeshChanged.ExecuteIfBound();
	}

	return AdoptedComponentCount;
}

void SUbstreamModelerViewport::DeselectAllStageComponents()
{
	USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	if (SelectedComponents)
	{
		SelectedComponents->DeselectAll();
	}
	PushStageSelectionToProxies();
}

bool SUbstreamModelerViewport::SelectPreviewComponentsForTool(const FString& ToolIdentifier)
{
	USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	if (!SelectedComponents)
	{
		return false;
	}

	TArray<UMeshComponent*> ComponentsToSelect;
	AppendSourcePreviewComponents(ComponentsToSelect);

	SelectedComponents->DeselectAll();
	for (UMeshComponent* Component : ComponentsToSelect)
	{
		SelectedComponents->Select(Component);
	}

	return SelectedComponents->Num() > 0;
}

void SUbstreamModelerViewport::OnFocusViewportToSelection()
{
	if (!ViewportClient.IsValid())
	{
		return;
	}

	// Frame the whole subject, preserving the view direction (Level Editor F).
	const FBox PreviewBounds = BuildPreviewBounds(PreviewComponents);
	if (!PreviewBounds.IsValid)
	{
		return;
	}

	ViewportClient->FocusOnBounds(FBoxSphereBounds(PreviewBounds), ViewportClient->GetViewRotation());
}

void SUbstreamModelerViewport::FocusStageViewport()
{
	if (ViewportWidget.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(ViewportWidget, EFocusCause::SetDirectly);
	}
}

void SUbstreamModelerViewport::SetStagePreviewComponentsSuppressed(bool bSuppressed)
{
	if (bStagePreviewComponentsSuppressed == bSuppressed)
	{
		return;
	}

	bStagePreviewComponentsSuppressed = bSuppressed;
	for (UMeshComponent* Component : PreviewComponents)
	{
		if (IsValid(Component))
		{
			Component->SetVisibility(!bSuppressed, true);
		}
	}

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

bool SUbstreamModelerViewport::EnsureEngineAssetSourcesAreEditable(FString& OutErrorMessage)
{
	OutErrorMessage.Reset();
	// Undo rebuilds transient previews only. A later edit can create a copy;
	// doing so here would record a new transaction while restoring the old one.
	if (bHandlingUndoRedo)
	{
		return true;
	}

	TArray<UMeshComponent*> StageMeshComponents;
	TSet<UMeshComponent*> SeenStageComponents;
	auto AddStageMeshComponent = [&StageMeshComponents, &SeenStageComponents](UMeshComponent* Component)
	{
		if (IsValid(Component) && !SeenStageComponents.Contains(Component))
		{
			StageMeshComponents.Add(Component);
			SeenStageComponents.Add(Component);
		}
	};

	for (UMeshComponent* PreviewComponent : PreviewComponents)
	{
		AddStageMeshComponent(PreviewComponent);
	}

	TArray<UPrimitiveComponent*> EditSessionComponents;
	AppendEditSessionMeshComponents(EditSessionComponents);
	for (UPrimitiveComponent* Primitive : EditSessionComponents)
	{
		AddStageMeshComponent(Cast<UMeshComponent>(Primitive));
	}

	bool bNeedsEditableCopy = false;
	if (SourceKind == ESourceKind::StaticMeshAsset)
	{
		bNeedsEditableCopy = IsEngineStaticMeshAsset(SoloMesh.Get());
	}
	else if (SourceKind == ESourceKind::ActorSelection || SourceKind == ESourceKind::ComponentSelection)
	{
		for (const TWeakObjectPtr<UMeshComponent>& ComponentPtr : SourceComponents)
		{
			const UStaticMeshComponent* StaticComponent = Cast<UStaticMeshComponent>(ComponentPtr.Get());
			if (StaticComponent && IsEngineStaticMeshAsset(StaticComponent->GetStaticMesh()))
			{
				bNeedsEditableCopy = true;
				break;
			}
		}
	}
	for (const UMeshComponent* StageMeshComponent : StageMeshComponents)
	{
		const UStaticMeshComponent* StaticStageComponent = Cast<UStaticMeshComponent>(StageMeshComponent);
		if (StaticStageComponent && IsEngineStaticMeshAsset(StaticStageComponent->GetStaticMesh()))
		{
			bNeedsEditableCopy = true;
			break;
		}
	}

	if (!bNeedsEditableCopy)
	{
		return true;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("SUbstreamModelerViewport", "MakeEngineAssetsEditable", "Make Unbound Loop Engine Assets Editable"));
	TMap<UStaticMesh*, UStaticMesh*> EditableCopies;

	auto GetOrCreateEditableCopy = [&EditableCopies, &OutErrorMessage](UStaticMesh* SourceMesh, const TArray<UMaterialInterface*>& Materials) -> UStaticMesh*
	{
		if (!IsEngineStaticMeshAsset(SourceMesh))
		{
			return SourceMesh;
		}

		if (UStaticMesh** ExistingCopy = EditableCopies.Find(SourceMesh))
		{
			return *ExistingCopy;
		}

		UStaticMesh* EditableCopy = CreateProjectAssetCopyOfStaticMesh(SourceMesh, Materials);
		if (!EditableCopy)
		{
			OutErrorMessage = FString::Printf(
				TEXT("%s is a built-in engine asset and creating an editable project copy failed."),
				*SourceMesh->GetName());
			return nullptr;
		}

		EditableCopies.Add(SourceMesh, EditableCopy);
		return EditableCopy;
	};

	if (SourceKind == ESourceKind::StaticMeshAsset)
	{
		UStaticMesh* SourceMesh = SoloMesh.Get();
		TArray<UMaterialInterface*> Materials = IsValid(SourceMesh) ? GetStaticMeshMaterials(*SourceMesh) : TArray<UMaterialInterface*>();
		if (PreviewComponents.Num() > 0)
		{
			if (const UMeshComponent* PreviewComponent = PreviewComponents[0])
			{
				Materials = GetComponentMaterials(*PreviewComponent);
			}
		}

		UStaticMesh* EditableCopy = GetOrCreateEditableCopy(SourceMesh, Materials);
		if (!EditableCopy)
		{
			return false;
		}

		if (EditableCopy != SourceMesh)
		{
			SoloMesh = EditableCopy;
			SoloMeshLabel = EditableCopy->GetName();
			SoloMeshPath = EditableCopy->GetPathName();
		}
	}
	else if (SourceKind == ESourceKind::ActorSelection || SourceKind == ESourceKind::ComponentSelection)
	{
		for (const TWeakObjectPtr<UMeshComponent>& ComponentPtr : SourceComponents)
		{
			UStaticMeshComponent* StaticComponent = Cast<UStaticMeshComponent>(ComponentPtr.Get());
			if (!StaticComponent || !IsEngineStaticMeshAsset(StaticComponent->GetStaticMesh()))
			{
				continue;
			}

			UStaticMesh* SourceMesh = StaticComponent->GetStaticMesh();
			UStaticMesh* EditableCopy = GetOrCreateEditableCopy(SourceMesh, GetComponentMaterials(*StaticComponent));
			if (!EditableCopy)
			{
				return false;
			}

			if (EditableCopy != SourceMesh)
			{
				StaticComponent->Modify();
				StaticComponent->SetStaticMesh(EditableCopy);
				if (SoloMesh.Get() == SourceMesh)
				{
					SoloMesh = EditableCopy;
				}
			}
		}
	}

	for (UMeshComponent* StageMeshComponent : StageMeshComponents)
	{
		UStaticMeshComponent* StaticStageComponent = Cast<UStaticMeshComponent>(StageMeshComponent);
		if (!StaticStageComponent)
		{
			continue;
		}

		UStaticMesh* StageMesh = StaticStageComponent->GetStaticMesh();
		if (!IsEngineStaticMeshAsset(StageMesh))
		{
			continue;
		}

		UStaticMesh* EditableCopy = GetOrCreateEditableCopy(StageMesh, GetComponentMaterials(*StaticStageComponent));
		if (!EditableCopy)
		{
			return false;
		}

		if (EditableCopy != StageMesh)
		{
			StaticStageComponent->Modify();
			StaticStageComponent->SetStaticMesh(EditableCopy);
			StaticStageComponent->UpdateBounds();
			if (SoloMesh.Get() == StageMesh)
			{
				SoloMesh = EditableCopy;
			}
		}
	}

	if (EditableCopies.IsEmpty())
	{
		return true;
	}

	if (SoloMesh.IsValid())
	{
		SoloMeshLabel = SoloMesh->GetName();
		SoloMeshPath = SoloMesh->GetPathName();
	}

	SourceSignatureAtLoad = BuildSourceSignature();

	LastSourceDirtyCheckSeconds = -1.0;
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnSoloMeshChanged.ExecuteIfBound();
	return true;
}

TSharedRef<FAdvancedPreviewScene> SUbstreamModelerViewport::GetPreviewScene() const
{
	check(PreviewScene.IsValid());
	return PreviewScene.ToSharedRef();
}

TSharedPtr<FUbstreamViewportClient> SUbstreamModelerViewport::GetUbstreamViewportClient() const
{
	return ViewportClient;
}

bool SUbstreamModelerViewport::ConfirmDiscardStageEdits(const FText& Prompt) const
{
	if (!HasUncommittedStageEdits())
	{
		return true;
	}
	return OnConfirmDiscardStageEdits.IsBound()
		? OnConfirmDiscardStageEdits.Execute(Prompt)
		: FMessageDialog::Open(EAppMsgType::YesNo, Prompt) == EAppReturnType::Yes;
}

bool SUbstreamModelerViewport::ConfirmReplaceSubject() const
{
	return ConfirmDiscardStageEdits(NSLOCTEXT("SUbstreamModelerViewport", "DiscardOnReplace",
		"The stage has edits that were not applied to the source. Loading another mesh will discard them.\n\nDiscard the stage edits and load the new mesh?"));
}

bool SUbstreamModelerViewport::SetStaticMesh(UStaticMesh* StaticMesh, bool bFocusCamera)
{
	if (!PreviewScene.IsValid() || !IsValid(StaticMesh))
	{
		return false;
	}

	ClearObjectsInternal(false);

	const FBoxSphereBounds MeshBounds = StaticMesh->GetBounds();
	const FTransform Transform(FRotator::ZeroRotator, -MeshBounds.Origin, FVector::OneVector);

	// Prefer a static-mesh preview. If the asset is clearly inside-out (common
	// with generated GLBs), stage a corrected dynamic mesh instead so the
	// first paint shows front faces without mutating the source asset here.
	UMeshComponent* MeshComponent = nullptr;
	if (const FMeshDescription* MeshDescription = StaticMesh->GetMeshDescription(0))
	{
		UE::Geometry::FDynamicMesh3 ConvertedMesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.bUseCompactedPolygonGroupIDValues = true;
		Converter.SetPolygonGroupToMaterialIndexMap(UE::Conversion::GetPolygonGroupToMaterialIndexMap(
			StaticMesh, UE::Conversion::EMeshLODType::SourceModel, 0));
		Converter.Convert(MeshDescription, ConvertedMesh);

		if (ConvertedMesh.TriangleCount() > 0
			&& UbstreamMeshOrientation::EnsureOutwardOrientation(ConvertedMesh, /*bRayProbe*/ true))
		{
			UDynamicMeshComponent* DynamicComponent = NewObject<UDynamicMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			DynamicComponent->SetFlags(RF_Transactional);
			DynamicComponent->CreationMethod = EComponentCreationMethod::Instance;
			DynamicComponent->SetMesh(MoveTemp(ConvertedMesh));
			DynamicComponent->SetMobility(EComponentMobility::Movable);
			DynamicComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			DynamicComponent->SetCastShadow(true);
			for (int32 MaterialIndex = 0; MaterialIndex < StaticMesh->GetStaticMaterials().Num(); ++MaterialIndex)
			{
				DynamicComponent->SetMaterial(MaterialIndex, StaticMesh->GetMaterial(MaterialIndex));
			}
			DynamicComponent->ValidateMaterialSlots(/*bCreateIfMissing*/ true, /*bDeleteExtraSlots*/ false);
			DynamicComponent->ComponentTags.Add(TEXT("solo_mesh"));
			MeshComponent = DynamicComponent;
		}
	}

	if (!MeshComponent)
	{
		UStaticMeshComponent* StaticComponent = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		StaticComponent->SetFlags(RF_Transactional);
		StaticComponent->CreationMethod = EComponentCreationMethod::Instance;
		StaticComponent->SetStaticMesh(StaticMesh);
		StaticComponent->SetMobility(EComponentMobility::Movable);
		StaticComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		StaticComponent->SetCastShadow(true);
		StaticComponent->ComponentTags.Add(TEXT("solo_mesh"));
		MeshComponent = StaticComponent;
	}

	AttachPreviewComponentToStage(MeshComponent, Transform);
	PreviewComponents.Add(MeshComponent);
	SoloMesh = StaticMesh;
	SourceActors.Reset();
	SourceComponents.Reset();
	SoloMeshLabel = StaticMesh->GetName();
	SoloMeshPath = StaticMesh->GetPathName();
	SourceKind = ESourceKind::StaticMeshAsset;
	NextBoxIndex = 1;
	SourceSignatureAtLoad = BuildSourceSignature();
	LastSourceDirtyCheckSeconds = -1.0;

	if (bFocusCamera)
	{
		FocusCameraOnComponent(*MeshComponent);
	}

	// Realtime while content is present: non-realtime first paint left a
	// double/ghost composite until the next mouse-driven Invalidate.
	UpdateStageRealtime();

	OnSoloMeshChanged.ExecuteIfBound();
	return true;
}

bool SUbstreamModelerViewport::SetMeshComponents(
	const TArray<UMeshComponent*>& Components,
	FString SubjectLabel,
	FString SubjectPath,
	TArray<TWeakObjectPtr<AActor>> InSourceActors,
	bool bFocusCamera)
{
	if (!PreviewScene.IsValid())
	{
		return false;
	}

	TArray<UMeshComponent*> ValidComponents;
	ValidComponents.Reserve(Components.Num());
	for (UMeshComponent* Component : Components)
	{
		if (UbstreamMeshComponents::HasRenderableGeometry(Component))
		{
			ValidComponents.Add(Component);
		}
	}

	if (ValidComponents.IsEmpty())
	{
		return false;
	}

	const FBox SourceBounds = BuildComponentsBounds(ValidComponents);
	if (!SourceBounds.IsValid)
	{
		return false;
	}

	ClearObjectsInternal(false);

	const FVector SourceCenter = SourceBounds.GetCenter();
	SourceComponents.Reset();
	SourceComponents.Reserve(ValidComponents.Num());
	int32 PreviewIndex = 0;
	for (int32 Index = 0; Index < ValidComponents.Num(); ++Index)
	{
		const UMeshComponent* SourceComponent = ValidComponents[Index];
		SourceComponents.Add(ValidComponents[Index]);
		const UInstancedStaticMeshComponent* InstancedSourceComponent = Cast<UInstancedStaticMeshComponent>(SourceComponent);
		if (InstancedSourceComponent && InstancedSourceComponent->GetInstanceCount() > 0)
		{
			for (int32 InstanceIndex = 0; InstanceIndex < InstancedSourceComponent->GetInstanceCount(); ++InstanceIndex)
			{
				FTransform InstanceTransform;
				if (!InstancedSourceComponent->GetInstanceTransform(InstanceIndex, InstanceTransform, true))
				{
					continue;
				}

				UStaticMeshComponent* PreviewComponent = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
				CopyStaticMeshComponentVisuals(*InstancedSourceComponent, *PreviewComponent);
				PreviewComponent->ComponentTags.Add(FName(*FString::Printf(TEXT("solo_component_%03d_instance_%03d"), Index + 1, InstanceIndex + 1)));

				FTransform PreviewTransform = InstanceTransform;
				PreviewTransform.SetLocation(PreviewTransform.GetLocation() - SourceCenter);
				AttachPreviewComponentToStage(PreviewComponent, PreviewTransform);
				PreviewComponents.Add(PreviewComponent);
				++PreviewIndex;
			}
			continue;
		}

		UMeshComponent* PreviewComponent = CreatePreviewComponentClone(*SourceComponent);
		if (!PreviewComponent)
		{
			continue;
		}
		PreviewComponent->ComponentTags.Add(FName(*FString::Printf(TEXT("solo_component_%03d"), ++PreviewIndex)));

		FTransform PreviewTransform = SourceComponent->GetComponentTransform();
		PreviewTransform.SetLocation(PreviewTransform.GetLocation() - SourceCenter);
		AttachPreviewComponentToStage(PreviewComponent, PreviewTransform);
		PreviewComponents.Add(PreviewComponent);
	}

	if (PreviewComponents.IsEmpty())
	{
		ClearObjectsInternal(false);
		return false;
	}

	SoloMesh = nullptr;
	for (UMeshComponent* ValidComponent : ValidComponents)
	{
		if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(ValidComponent))
		{
			SoloMesh = StaticMeshComponent->GetStaticMesh();
			break;
		}
	}
	SourceActors = InSourceActors;
	SoloMeshLabel = SubjectLabel.IsEmpty() ? ValidComponents[0]->GetName() : SubjectLabel;
	SoloMeshPath = SubjectPath.IsEmpty() ? ValidComponents[0]->GetPathName() : SubjectPath;
	SourceKind = ESourceKind::ComponentSelection;
	NextBoxIndex = 1;
	SourceSignatureAtLoad = BuildSourceSignature();
	LastSourceDirtyCheckSeconds = -1.0;

	if (bFocusCamera && PreviewComponents.Num() > 0 && ViewportClient.IsValid())
	{
		const FBox PreviewBounds = BuildPreviewBounds(PreviewComponents);
		if (PreviewBounds.IsValid)
		{
			ViewportClient->FocusOnBounds(FBoxSphereBounds(PreviewBounds));
		}
	}

	UpdateStageRealtime();

	OnSoloMeshChanged.ExecuteIfBound();
	return true;
}

bool SUbstreamModelerViewport::SetActors(const TArray<TWeakObjectPtr<AActor>>& Actors, bool bFocusCamera)
{
	TArray<UMeshComponent*> Components;
	TArray<TWeakObjectPtr<AActor>> ValidActors;
	for (const TWeakObjectPtr<AActor>& ActorPtr : Actors)
	{
		AActor* Actor = ActorPtr.Get();
		if (!IsValid(Actor))
		{
			continue;
		}

		TArray<UMeshComponent*> ActorComponents;
		Actor->GetComponents<UMeshComponent>(ActorComponents);
		bool bActorHasRenderableMesh = false;
		for (UMeshComponent* Component : ActorComponents)
		{
			if (UbstreamMeshComponents::HasRenderableGeometry(Component))
			{
				Components.Add(Component);
				bActorHasRenderableMesh = true;
			}
		}

		if (bActorHasRenderableMesh)
		{
			ValidActors.Add(Actor);
		}
	}

	if (ValidActors.IsEmpty())
	{
		return false;
	}

	const bool bLoaded = SetMeshComponents(Components, BuildActorSubjectLabel(ValidActors), BuildActorSubjectPath(ValidActors), ValidActors, bFocusCamera);
	if (!bLoaded)
	{
		SourceActors.Reset();
	}
	else
	{
		SourceKind = ESourceKind::ActorSelection;
		SourceSignatureAtLoad = BuildSourceSignature();
		LastSourceDirtyCheckSeconds = -1.0;
	}
	return bLoaded;
}

bool SUbstreamModelerViewport::FocusSoloMesh()
{
	return SetCameraView(EUbstreamStageCameraView::Iso);
}

bool SUbstreamModelerViewport::SetCameraView(EUbstreamStageCameraView CameraView)
{
	if (!HasSoloMesh())
	{
		return false;
	}

	const FBox PreviewBounds = BuildPreviewBounds(PreviewComponents);
	if (!PreviewBounds.IsValid || !ViewportClient.IsValid())
	{
		return false;
	}

	ViewportClient->FocusOnBounds(FBoxSphereBounds(PreviewBounds), GetStageCameraViewRotation(CameraView));
	return true;
}

bool SUbstreamModelerViewport::ResyncFromSource(bool bPreserveCamera)
{
	if (!CanResyncFromSource())
	{
		return false;
	}

	const bool bFocusCamera = !bPreserveCamera;

	if (SourceKind == ESourceKind::StaticMeshAsset)
	{
		return SetStaticMesh(SoloMesh.Get(), bFocusCamera);
	}

	if (SourceKind == ESourceKind::ComponentSelection)
	{
		TArray<UMeshComponent*> Components;
		Components.Reserve(SourceComponents.Num());
		for (const TWeakObjectPtr<UMeshComponent>& ComponentPtr : SourceComponents)
		{
			UMeshComponent* Component = ComponentPtr.Get();
			if (UbstreamMeshComponents::HasRenderableGeometry(Component))
			{
				Components.Add(Component);
			}
		}

		return SetMeshComponents(Components, SoloMeshLabel, SoloMeshPath, SourceActors, bFocusCamera);
	}

	if (SourceKind == ESourceKind::ActorSelection)
	{
		return SetActors(SourceActors, bFocusCamera);
	}

	return false;
}

bool SUbstreamModelerViewport::CanResyncFromSource() const
{
	if (!HasSoloMesh())
	{
		return false;
	}

	if (SourceKind == ESourceKind::StaticMeshAsset)
	{
		return IsValid(SoloMesh.Get());
	}

	if (SourceKind == ESourceKind::ComponentSelection)
	{
		return SourceComponents.ContainsByPredicate([](const TWeakObjectPtr<UMeshComponent>& ComponentPtr)
		{
			return UbstreamMeshComponents::HasRenderableGeometry(ComponentPtr.Get());
		});
	}

	if (SourceKind == ESourceKind::ActorSelection)
	{
		return SourceActors.ContainsByPredicate([](const TWeakObjectPtr<AActor>& ActorPtr)
		{
			return IsValid(ActorPtr.Get());
		});
	}

	return false;
}

bool SUbstreamModelerViewport::HasEditableSource() const
{
	return CanResyncFromSource();
}

bool SUbstreamModelerViewport::HasSoloMesh() const
{
	for (const UMeshComponent* Component : PreviewComponents)
	{
		if (UbstreamMeshComponents::HasRenderableGeometry(Component))
		{
			return true;
		}
	}
	return false;
}

bool SUbstreamModelerViewport::IsSourceDirty() const
{
	if (!CanResyncFromSource() || SourceSignatureAtLoad == 0)
	{
		return false;
	}
	// Multiple Slate attributes and the source-status timer ask this question.
	// Share a 0.5s sample; a new load/resync baseline invalidates it immediately.
	const double Now = FPlatformTime::Seconds();
	if (LastSourceDirtyCheckSeconds < 0.0
		|| Now - LastSourceDirtyCheckSeconds >= 0.5
		|| LastSourceDirtyCheckBaseline != SourceSignatureAtLoad)
	{
		bCachedSourceDirty = BuildSourceSignature() != SourceSignatureAtLoad;
		LastSourceDirtyCheckSeconds = Now;
		LastSourceDirtyCheckBaseline = SourceSignatureAtLoad;
	}
	return bCachedSourceDirty;
}

UStaticMesh* SUbstreamModelerViewport::GetPrimaryStaticMesh() const
{
	return SoloMesh.Get();
}

TArray<TWeakObjectPtr<AActor>> SUbstreamModelerViewport::GetSourceActors() const
{
	return SourceActors;
}

TArray<TWeakObjectPtr<UMeshComponent>> SUbstreamModelerViewport::GetSourceComponents() const
{
	return SourceComponents;
}

FString SUbstreamModelerViewport::GetSoloMeshLabel() const
{
	return SoloMeshLabel.IsEmpty() ? FString(TEXT("No isolated subject")) : SoloMeshLabel;
}

FString SUbstreamModelerViewport::GetSoloMeshPath() const
{
	return SoloMeshPath;
}

FString SUbstreamModelerViewport::GetSourceModeLabel() const
{
	switch (SourceKind)
	{
	case ESourceKind::StaticMeshAsset:
		return TEXT("Static Mesh asset clone");
	case ESourceKind::ActorSelection:
		return TEXT("Actor selection clone");
	case ESourceKind::ComponentSelection:
		return TEXT("Component selection clone");
	case ESourceKind::GeneratedPreview:
		return TEXT("Generated preview");
	default:
		return TEXT("No source");
	}
}

int32 SUbstreamModelerViewport::GetPreviewComponentCount() const
{
	int32 Count = 0;
	for (const UMeshComponent* Component : PreviewComponents)
	{
		if (UbstreamMeshComponents::HasRenderableGeometry(Component))
		{
			++Count;
		}
	}
	return Count;
}

int32 SUbstreamModelerViewport::GetSourcePreviewComponentCount() const
{
	TArray<UMeshComponent*> SourcePreviewComponents;
	AppendSourcePreviewComponents(SourcePreviewComponents);
	return SourcePreviewComponents.Num();
}

int32 SUbstreamModelerViewport::GetSourceActorCount() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<AActor>& ActorPtr : SourceActors)
	{
		if (IsValid(ActorPtr.Get()))
		{
			++Count;
		}
	}
	return Count;
}

int32 SUbstreamModelerViewport::GetSourceComponentCount() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<UMeshComponent>& ComponentPtr : SourceComponents)
	{
		if (UbstreamMeshComponents::HasRenderableGeometry(ComponentPtr.Get()))
		{
			++Count;
		}
	}
	return Count;
}

uint64 SUbstreamModelerViewport::BuildSceneSignature() const
{
	uint64 Hash = MixSceneSignature(0x7b58d5a8f0d54d1full, static_cast<uint64>(PreviewComponents.Num()));
	Hash = MixSceneString(Hash, SoloMeshPath);
	Hash = MixSceneSignature(Hash, QuantizeSceneFloat(StageLighting.AmbientFill));
	Hash = MixSceneSignature(Hash, QuantizeSceneFloat(StageLighting.MainStrength));
	Hash = MixSceneSignature(Hash, QuantizeSceneFloat(StageLighting.Rotation));
	Hash = MixSceneSignature(Hash, QuantizeSceneFloat(StageLighting.Elevation));
	Hash = MixSceneSignature(Hash, StageLighting.bFollowCamera ? 1 : 0);
	Hash = MixSceneSignature(Hash, StageLighting.MainColor.ToFColor(true).DWColor());
	Hash = MixSceneSignature(Hash, StageLighting.AmbientColor.ToFColor(true).DWColor());

	// An active edit session mutates geometry continuously without necessarily
	// changing bounds or counts; make every tick look dirty so strokes stream.
	if (ShouldCaptureWholeScene())
	{
		Hash = MixSceneSignature(Hash, FPlatformTime::Cycles64());
	}
	if (ViewportClient.IsValid())
	{
		const FUbstreamCaptureCamera Camera = BuildCaptureCamera(/*Width*/ 1, /*Height*/ 1);
		Hash = MixSceneVector(Hash, Camera.Location);
		Hash = MixSceneVector(Hash, Camera.Rotation.Euler());
		Hash = MixSceneSignature(Hash, QuantizeSceneFloat(Camera.FovDegrees));
		Hash = MixSceneSignature(Hash, QuantizeSceneFloat(Camera.NearClipPlane));
	}
	for (const UMeshComponent* Component : PreviewComponents)
	{
		if (!IsValid(Component))
		{
			continue;
		}

		Hash = MixMeshComponentSource(Hash, Component);
	}
	return Hash;
}

uint64 SUbstreamModelerViewport::BuildSourceSignature() const
{
	uint64 Hash = MixSceneSignature(0x0f68cf3cc2f351b7ull, static_cast<uint64>(SourceKind));
	Hash = MixSceneString(Hash, SoloMeshPath);

	if (SourceKind == ESourceKind::StaticMeshAsset)
	{
		return MixStaticMeshSource(Hash, SoloMesh.Get());
	}

	if (SourceKind == ESourceKind::ComponentSelection)
	{
		Hash = MixSceneSignature(Hash, static_cast<uint64>(SourceComponents.Num()));
		for (const TWeakObjectPtr<UMeshComponent>& ComponentPtr : SourceComponents)
		{
			Hash = MixMeshComponentSource(Hash, ComponentPtr.Get());
		}
		return Hash;
	}

	if (SourceKind == ESourceKind::ActorSelection)
	{
		Hash = MixSceneSignature(Hash, static_cast<uint64>(SourceActors.Num()));
		for (const TWeakObjectPtr<AActor>& ActorPtr : SourceActors)
		{
			AActor* Actor = ActorPtr.Get();
			if (!IsValid(Actor))
			{
				Hash = MixSceneSignature(Hash, 0);
				continue;
			}

			Hash = MixSceneString(Hash, Actor->GetPathName());
			Hash = MixSceneString(Hash, Actor->GetActorLabel());
			Hash = MixSceneTransform(Hash, Actor->GetActorTransform());

			TArray<UMeshComponent*> MeshComponents;
			Actor->GetComponents<UMeshComponent>(MeshComponents);
			Hash = MixSceneSignature(Hash, static_cast<uint64>(MeshComponents.Num()));
			for (const UMeshComponent* Component : MeshComponents)
			{
				Hash = MixMeshComponentSource(Hash, Component);
			}
		}
		return Hash;
	}

	return Hash;
}

void SUbstreamModelerViewport::AddBox()
{
	AddCSGPrimitive(EUbstreamCSGPrimitiveType::Box);
}

void SUbstreamModelerViewport::MakeActiveSculptToolTransactional()
{
	if (!GetActiveVertexSculptTool() || !PreviewScene.IsValid())
	{
		return;
	}
	UWorld* PreviewWorld = PreviewScene->GetWorld();
	if (!PreviewWorld)
	{
		return;
	}

	TSet<const UMeshComponent*> KnownComponents;
	for (const UMeshComponent* Component : PreviewComponents)
	{
		if (IsValid(Component))
		{
			KnownComponents.Add(Component);
		}
	}

	// The tool's working mesh is the one opaque, renderable mesh in the preview
	// world that is neither ours nor a cutter/gizmo/overlay.
	for (TActorIterator<AActor> ActorIt(PreviewWorld); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (!IsValid(Actor) || Actor->IsA<AGizmoActor>())
		{
			continue;
		}
		TInlineComponentArray<UMeshComponent*> MeshComponents(Actor);
		for (UMeshComponent* Component : MeshComponents)
		{
			if (!IsValid(Component)
				|| KnownComponents.Contains(Component)
				|| !Component->IsRegistered()
				|| IsEditorGizmoComponent(Component)
				|| IsCSGPrimitiveComponent(Component)
				|| Component->IsA<ULineSetComponent>()
				|| Component->IsA<UPointSetComponent>()
				|| Component->IsA<UTriangleSetComponent>()
				|| !UbstreamMeshComponents::HasRenderableGeometry(Component))
			{
				continue;
			}

			Component->SetFlags(RF_Transactional);
			if (AActor* Owner = Component->GetOwner())
			{
				Owner->SetFlags(RF_Transactional);
			}
			if (UDynamicMeshComponent* DynamicComponent = Cast<UDynamicMeshComponent>(Component))
			{
				if (UDynamicMesh* DynamicMesh = DynamicComponent->GetDynamicMesh())
				{
					DynamicMesh->SetFlags(RF_Transactional);
				}
			}
		}
	}
}

void SUbstreamModelerViewport::CommitActiveSculptImmediate()
{
	if (!IsSculptActive())
	{
		return;
	}
	// Synchronous bake so the CSG add that follows sees no active tool (the
	// deferred end would leave the tool live for a tick and refuse the add).
	if (UUbstreamStageEditorMode* Mode = GetStageEditorMode())
	{
		RunWithPreviewWorldInactive(PreviewScene, [Mode]()
		{
			Mode->EndActiveToolImmediate(/*bAccept*/ true);
		});
	}
	RefreshCSGPrimitiveVisuals();
	RefreshToolPreviewVisibility();
	OnEditingStateChanged.ExecuteIfBound();
}

bool SUbstreamModelerViewport::CanAddCSGPrimitive() const
{
	if (IsRemeshing()) return false;
	// Adding during a live session is part of the stack workflow: the session
	// restarts with the new cutter included. A live SCULPT is also fine to add
	// over - placing a shape bakes the sculpt in and hands back to the cutter
	// workflow (CommitActiveSculptImmediate). Other tools still block.
	if (!CanResyncFromSource() || (HasAnyActiveStageTool() && !IsSculptActive()))
	{
		return false;
	}

	TArray<UMeshComponent*> SourcePreviewComponents;
	AppendSourcePreviewComponents(SourcePreviewComponents);
	return SourcePreviewComponents.Num() == 1;
}

bool SUbstreamModelerViewport::AddCSGPrimitive(
	EUbstreamCSGPrimitiveType PrimitiveType,
	const TOptional<FVector>& WorldLocation,
	const TOptional<double>& HalfSizeOverride)
{
	check(PreviewScene.IsValid());

	// Adding a shape mid-sculpt bakes the sculpt in first, then the cutter is
	// added to the (now sculpted) base.
	CommitActiveSculptImmediate();

	if (!PreviewScene.IsValid() || !CanAddCSGPrimitive())
	{
		return false;
	}

	FString EditableSourceError;
	if (!EnsureEngineAssetSourcesAreEditable(EditableSourceError))
	{
		return false;
	}

	// Enter the editing session first: it converts static clones to dynamic
	// meshes, which is what lets sizing see the actual geometry (and its
	// dominant connected component) instead of import-inflated render bounds.
	if (!bEditModeActive && !EnterEditMode())
	{
		return false;
	}

	TArray<UMeshComponent*> SourcePreviewComponents;
	AppendSourcePreviewComponents(SourcePreviewComponents);
	const FBox SourceBounds = ComputeDominantGeometryBounds(SourcePreviewComponents);
	if (!SourceBounds.IsValid)
	{
		return false;
	}

	const FVector SourceExtent = SourceBounds.GetExtent();
	const double MinExtent = FMath::Min3(SourceExtent.X, SourceExtent.Y, SourceExtent.Z);
	const double MaxExtent = FMath::Max3(SourceExtent.X, SourceExtent.Y, SourceExtent.Z);
	const double MedianExtent = SourceExtent.X + SourceExtent.Y + SourceExtent.Z - MinExtent - MaxExtent;
	auto GetAxisValue = [](const FVector& Vector, int32 Axis)
	{
		return Axis == 0 ? Vector.X : (Axis == 1 ? Vector.Y : Vector.Z);
	};

	// Roughly a quarter of the model reads as "a tool", not "a second model";
	// the median clamp keeps it proportionate on elongated subjects.
	const double CutterHalfSize = HalfSizeOverride.IsSet()
		? FMath::Max(0.001, HalfSizeOverride.GetValue())
		: FMath::Max(0.001, FMath::Min(MaxExtent * 0.25, MedianExtent * 0.5));
	UE_LOG(LogUbstreamModeler, Log,
		TEXT("Cutter sized %.3f for subject bounds %.3f x %.3f x %.3f (robust, outlier-trimmed)"),
		CutterHalfSize * 2.0,
		SourceExtent.X * 2.0, SourceExtent.Y * 2.0, SourceExtent.Z * 2.0);

	// Half-embedded on the model's largest face: a visible bite and a
	// grabbable ghost, with no camera-dependent placement. The one camera rule
	// is a safety net: if the view sits so close that the cutter would
	// surround the camera (and flood the screen with its edges), fall back to
	// the model's center instead.
	const int32 LargestAxis = MaxExtent == SourceExtent.Y ? 1 : (MaxExtent == SourceExtent.Z ? 2 : 0);
	FVector LargestAxisVector = FVector::XAxisVector;
	if (LargestAxis == 1)
	{
		LargestAxisVector = FVector::YAxisVector;
	}
	else if (LargestAxis == 2)
	{
		LargestAxisVector = FVector::ZAxisVector;
	}
	FVector PrimitiveLocation = SourceBounds.GetCenter()
		+ LargestAxisVector * GetAxisValue(SourceExtent, LargestAxis);
	if (WorldLocation.IsSet())
	{
		// Click-to-place: the cutter's center sits exactly on the clicked
		// surface point, so half of it bites into the model.
		PrimitiveLocation = WorldLocation.GetValue();
	}
	else if (ViewportClient.IsValid())
	{
		const double CutterBoundingRadius = CutterHalfSize * UE_DOUBLE_SQRT_3;
		if (FVector::Distance(ViewportClient->GetViewLocation(), PrimitiveLocation) < CutterBoundingRadius * 1.6)
		{
			PrimitiveLocation = SourceBounds.GetCenter();
		}
	}

	const FVector BoxExtent(CutterHalfSize, CutterHalfSize, CutterHalfSize);
	const double CutterRadius = CutterHalfSize;

	TUniquePtr<UE::Geometry::FMeshShapeGenerator> Generator;
	FString PrimitiveName;
	switch (PrimitiveType)
	{
	case EUbstreamCSGPrimitiveType::Cylinder:
		{
			TUniquePtr<UE::Geometry::FCylinderGenerator> CylinderGenerator = MakeUnique<UE::Geometry::FCylinderGenerator>();
			CylinderGenerator->AngleSamples = 32;
			CylinderGenerator->bCapped = true;
			CylinderGenerator->Radius[0] = static_cast<float>(CutterRadius);
			CylinderGenerator->Radius[1] = static_cast<float>(CutterRadius);
			CylinderGenerator->Height = static_cast<float>(CutterHalfSize * 2.0);
			Generator = MoveTemp(CylinderGenerator);
			PrimitiveName = TEXT("csg_cylinder");
			break;
		}
	case EUbstreamCSGPrimitiveType::Sphere:
		{
			TUniquePtr<UE::Geometry::FSphereGenerator> SphereGenerator = MakeUnique<UE::Geometry::FSphereGenerator>();
			SphereGenerator->Radius = CutterRadius;
			SphereGenerator->NumPhi = 16;
			SphereGenerator->NumTheta = 32;
			Generator = MoveTemp(SphereGenerator);
			PrimitiveName = TEXT("csg_sphere");
			break;
		}
	case EUbstreamCSGPrimitiveType::Box:
	default:
		{
			TUniquePtr<UE::Geometry::FMinimalBoxMeshGenerator> BoxGenerator = MakeUnique<UE::Geometry::FMinimalBoxMeshGenerator>();
			BoxGenerator->Box = UE::Geometry::FOrientedBox3d(FVector3d::Zero(), FVector3d(BoxExtent));
			Generator = MoveTemp(BoxGenerator);
			PrimitiveName = TEXT("csg_box");
			break;
		}
	}

	if (!Generator)
	{
		return false;
	}

	Generator->Generate();
	UE::Geometry::FDynamicMesh3 PrimitiveMesh(Generator.Get());
	if (PrimitiveType == EUbstreamCSGPrimitiveType::Cylinder)
	{
		// FCylinderGenerator spans Z=0..Height. Placement and dimensions use
		// a centered primitive, just like the box and sphere cutters.
		for (int32 VertexID : PrimitiveMesh.VertexIndicesItr())
		{
			PrimitiveMesh.SetVertex(VertexID, PrimitiveMesh.GetVertex(VertexID) - FVector3d(0, 0, CutterHalfSize));
		}
	}

	UDynamicMeshComponent* PrimitiveComponent = NewObject<UDynamicMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
	PrimitiveComponent->SetFlags(RF_Transactional);
	PrimitiveComponent->SetMesh(MoveTemp(PrimitiveMesh));
	if (UDynamicMesh* DynamicMesh = PrimitiveComponent->GetDynamicMesh())
	{
		DynamicMesh->SetFlags(RF_Transactional);
	}
	PrimitiveComponent->CreationMethod = EComponentCreationMethod::Instance;
	PrimitiveComponent->SetMobility(EComponentMobility::Movable);
	PrimitiveComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PrimitiveComponent->SetCastShadow(true);
	PrimitiveComponent->ComponentTags.Add(CSGPrimitiveTag);
	PrimitiveComponent->ComponentTags.Add(FName(*FString::Printf(TEXT("%s_%03d"), *PrimitiveName, NextBoxIndex++)));
	PrimitiveComponent->SetColorOverrideMode(EDynamicMeshComponentColorOverrideMode::Constant);
	PrimitiveComponent->SetConstantOverrideColor(CSGPrimitiveColor);
	// Interaction chrome must never reach the stream, whichever capture path
	// (mirror or whole-scene) is active.
	PrimitiveComponent->SetHiddenInSceneCapture(true);
	PrimitiveComponent->SetEnableWireframeRenderPass(true);
	PrimitiveComponent->WireframeColor = CSGPrimitiveWireColor;
	PrimitiveComponent->ValidateMaterialSlots(/*bCreateIfMissing*/ true, /*bDeleteExtraSlots*/ false);

	{
		// Component creation records through the owning actor, making the add
		// itself undoable (the mesh stays on the living object across undo).
		FScopedTransaction AddTransaction(NSLOCTEXT("SUbstreamModelerViewport", "AddCutterTransaction", "Add Cutter"));
		if (AActor* Stage = EnsureStageActor())
		{
			Stage->Modify();
		}
		PrimitiveComponent->Modify();
		AttachPreviewComponentToStage(PrimitiveComponent, FTransform(FRotator::ZeroRotator, PrimitiveLocation, FVector::OneVector));
	}
	PreviewComponents.Add(PrimitiveComponent);

	LastCSGPrimitiveType = PrimitiveType;
	LastCSGPrimitiveHalfSize = CutterHalfSize;

	if (bEditModeActive)
	{
		CSGTransformMode = EUbstreamCSGTransformMode::Move;
		SelectLatestCSGPrimitiveForEditing();
	}
	RefreshCSGPrimitiveVisuals();

	// Deliberately no camera refocus: the cutter spawns on the face the user
	// is already looking at, and a view jump on every add reads as glitchy.
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}

	OnSoloMeshChanged.ExecuteIfBound();
	OnEditingStateChanged.ExecuteIfBound();
	return true;
}

void SUbstreamModelerViewport::ArmCSGPlacement(const EUbstreamCSGPrimitiveType PrimitiveType)
{
	// Arming a shape mid-sculpt bakes the sculpt in and returns to the cutter
	// workflow.
	CommitActiveSculptImmediate();

	// The placement raycast only sees dynamic meshes: convert the static
	// clone up front, or the first click on a fresh subject silently misses.
	FString EditableSourceError;
	if (!EnsureEngineAssetSourcesAreEditable(EditableSourceError)
		|| (!bEditModeActive && !EnterEditMode()))
	{
		return;
	}
	PendingCSGPlacementType = PrimitiveType;
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnEditingStateChanged.ExecuteIfBound();
}

void SUbstreamModelerViewport::CancelCSGPlacement()
{
	if (!PendingCSGPlacementType.IsSet())
	{
		return;
	}
	PendingCSGPlacementType.Reset();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnEditingStateChanged.ExecuteIfBound();
}

bool SUbstreamModelerViewport::HasPendingCSGPlacement() const
{
	return PendingCSGPlacementType.IsSet();
}

bool SUbstreamModelerViewport::RaycastSourcePreview(const FVector& RayOrigin, const FVector& RayDirection, FVector& OutHitPoint) const
{
	TArray<UMeshComponent*> RaycastComponents;
	AppendSourcePreviewComponents(RaycastComponents);

	double BestRayParameter = TNumericLimits<double>::Max();
	bool bHit = false;
	for (const UMeshComponent* Component : RaycastComponents)
	{
		const UDynamicMeshComponent* DynamicComponent = Cast<UDynamicMeshComponent>(Component);
		if (!DynamicComponent)
		{
			continue;
		}
		const FTransform& ComponentTransform = DynamicComponent->GetComponentTransform();
		// Ray to local space; direction kept unit-length by scaling the hit
		// parameter back to world units afterward.
		const FVector LocalOrigin = ComponentTransform.InverseTransformPosition(RayOrigin);
		const FVector LocalDirectionScaled = ComponentTransform.InverseTransformVector(RayDirection);
		const double LocalScale = LocalDirectionScaled.Length();
		if (LocalScale <= UE_DOUBLE_SMALL_NUMBER)
		{
			continue;
		}
		const FVector LocalDirection = LocalDirectionScaled / LocalScale;

		DynamicComponent->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			UE::Geometry::FDynamicMeshAABBTree3 Tree(&Mesh, /*bAutoBuild*/ true);
			const FRay3d LocalRay((FVector3d)LocalOrigin, (FVector3d)LocalDirection);
			double LocalRayParameter = TNumericLimits<double>::Max();
			int32 HitTriangleId = IndexConstants::InvalidID;
			if (Tree.FindNearestHitTriangle(LocalRay, LocalRayParameter, HitTriangleId))
			{
				const double WorldRayParameter = LocalRayParameter / LocalScale;
				if (WorldRayParameter < BestRayParameter)
				{
					BestRayParameter = WorldRayParameter;
					bHit = true;
				}
			}
		});
	}

	if (bHit)
	{
		OutHitPoint = RayOrigin + RayDirection * BestRayParameter;
	}
	return bHit;
}

UMeshComponent* SUbstreamModelerViewport::FindCSGPrimitiveUnderRay(const FVector& RayOrigin, const FVector& RayDirection, double& OutRayParameter) const
{
	OutRayParameter = TNumericLimits<double>::Max();
	UMeshComponent* BestCutter = nullptr;
	for (UMeshComponent* Component : PreviewComponents)
	{
		UDynamicMeshComponent* DynamicComponent = Cast<UDynamicMeshComponent>(Component);
		if (!IsCSGPrimitiveComponent(DynamicComponent))
		{
			continue;
		}
		const FTransform& ComponentTransform = DynamicComponent->GetComponentTransform();
		const FVector LocalOrigin = ComponentTransform.InverseTransformPosition(RayOrigin);
		const FVector LocalDirectionScaled = ComponentTransform.InverseTransformVector(RayDirection);
		const double LocalScale = LocalDirectionScaled.Length();
		if (LocalScale <= UE_DOUBLE_SMALL_NUMBER)
		{
			continue;
		}
		const FVector LocalDirection = LocalDirectionScaled / LocalScale;

		DynamicComponent->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			// Cutter meshes are primitive-sized (a box is 12 triangles), so a
			// per-query tree build is nothing.
			UE::Geometry::FDynamicMeshAABBTree3 Tree(&Mesh, /*bAutoBuild*/ true);
			const FRay3d LocalRay((FVector3d)LocalOrigin, (FVector3d)LocalDirection);
			double LocalRayParameter = TNumericLimits<double>::Max();
			int32 HitTriangleId = IndexConstants::InvalidID;
			if (Tree.FindNearestHitTriangle(LocalRay, LocalRayParameter, HitTriangleId))
			{
				const double WorldRayParameter = LocalRayParameter / LocalScale;
				if (WorldRayParameter < OutRayParameter)
				{
					OutRayParameter = WorldRayParameter;
					BestCutter = DynamicComponent;
				}
			}
		});
	}
	return BestCutter;
}

bool SUbstreamModelerViewport::PlacePendingCSGPrimitiveAtRay(const FVector& RayOrigin, const FVector& RayDirection)
{
	if (!PendingCSGPlacementType.IsSet())
	{
		return false;
	}

	FVector HitPoint = FVector::ZeroVector;
	if (!RaycastSourcePreview(RayOrigin, RayDirection, HitPoint))
	{
		// Clicking off the model cancels placement instead of guessing.
		CancelCSGPlacement();
		return false;
	}

	const EUbstreamCSGPrimitiveType PrimitiveType = PendingCSGPlacementType.GetValue();
	PendingCSGPlacementType.Reset();
	// (The raycast above only sees converted dynamic meshes; AddCSGPrimitive
	// converts on entry, so a fresh static clone reaches here only after the
	// caller ensured edit mode - see ArmCSGPlacement.)
	if (!AddCSGPrimitive(PrimitiveType, HitPoint))
	{
		OnEditingStateChanged.ExecuteIfBound();
		return false;
	}
	StartCSGOperation(EUbstreamCSGOperation::Subtract);
	OnEditingStateChanged.ExecuteIfBound();
	return true;
}

bool SUbstreamModelerViewport::RepositionSessionCutterAtRay(const FVector& RayOrigin, const FVector& RayDirection)
{
	if (!HasActiveCSGSession())
	{
		return false;
	}
	// The selected cutter follows the click; with none selected, the last
	// one in the stack does.
	TArray<UMeshComponent*> SelectedCutters;
	GetSelectedCSGPrimitiveComponents(SelectedCutters);
	UMeshComponent* Brush = SelectedCutters.Num() > 0 ? SelectedCutters.Last() : nullptr;
	if (!Brush || FindCSGSessionBrushIndex(Brush) == INDEX_NONE)
	{
		Brush = CSGSessionBrushes.Num() > 0 ? CSGSessionBrushes.Last().Get() : nullptr;
	}
	if (!IsValid(Brush))
	{
		return false;
	}

	FVector HitPoint = FVector::ZeroVector;
	if (!RaycastSourcePreview(RayOrigin, RayDirection, HitPoint))
	{
		return false;
	}

	{
		FScopedTransaction Transaction(NSLOCTEXT("SUbstreamModelerViewport", "RepositionCutterTransaction", "Move Cutter"));
		Brush->Modify();
		Brush->SetWorldLocation(HitPoint);
	}
	PushSessionBrushTransforms();
	UpdateStageTransformGizmo();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnSoloMeshChanged.ExecuteIfBound();
	return true;
}

bool SUbstreamModelerViewport::DuplicateSelectedCSGPrimitive()
{
	// Mid-session duplication is fine: StartCSGOperation below rebuilds the
	// live stack with the copy included.
	TArray<UMeshComponent*> SelectedCutters;
	GetSelectedCSGPrimitiveComponents(SelectedCutters);
	UDynamicMeshComponent* Source = SelectedCutters.Num() > 0
		? Cast<UDynamicMeshComponent>(SelectedCutters.Last())
		: nullptr;
	if (!IsValid(Source))
	{
		return false;
	}

	UE::Geometry::FDynamicMesh3 CopiedMesh;
	Source->ProcessMesh([&CopiedMesh](const UE::Geometry::FDynamicMesh3& Mesh)
	{
		CopiedMesh = Mesh;
	});

	// Offset sideways relative to the camera so the copy is obviously a new
	// object, snapped to the dominant world axis for predictable dragging.
	Source->UpdateBounds();
	const double Offset = FMath::Max(1e-3, Source->Bounds.GetBox().GetSize().GetMax() * 0.6);
	FVector OffsetDirection = FVector::XAxisVector;
	if (ViewportClient.IsValid())
	{
		const FVector CameraRight = ViewportClient->GetViewRotation().RotateVector(FVector::RightVector);
		OffsetDirection = FMath::Abs(CameraRight.X) >= FMath::Abs(CameraRight.Y)
			? FVector(FMath::Sign(CameraRight.X), 0.0, 0.0)
			: FVector(0.0, FMath::Sign(CameraRight.Y), 0.0);
	}
	FTransform DuplicateTransform = Source->GetComponentTransform();
	DuplicateTransform.AddToTranslation(OffsetDirection * Offset);

	UDynamicMeshComponent* Duplicate = NewObject<UDynamicMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
	Duplicate->SetFlags(RF_Transactional);
	Duplicate->SetMesh(MoveTemp(CopiedMesh));
	if (UDynamicMesh* DynamicMesh = Duplicate->GetDynamicMesh())
	{
		DynamicMesh->SetFlags(RF_Transactional);
	}
	Duplicate->CreationMethod = EComponentCreationMethod::Instance;
	Duplicate->SetMobility(EComponentMobility::Movable);
	Duplicate->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Duplicate->SetCastShadow(true);
	Duplicate->ComponentTags.Add(CSGPrimitiveTag);
	Duplicate->ComponentTags.Add(FName(*FString::Printf(TEXT("csg_duplicate_%03d"), NextBoxIndex++)));
	Duplicate->SetColorOverrideMode(EDynamicMeshComponentColorOverrideMode::Constant);
	Duplicate->SetConstantOverrideColor(CSGPrimitiveColor);
	Duplicate->SetHiddenInSceneCapture(true);
	Duplicate->SetEnableWireframeRenderPass(true);
	Duplicate->WireframeColor = CSGPrimitiveWireColor;
	Duplicate->ValidateMaterialSlots(/*bCreateIfMissing*/ true, /*bDeleteExtraSlots*/ false);

	{
		FScopedTransaction DuplicateTransaction(NSLOCTEXT("SUbstreamModelerViewport", "DuplicateCutterTransaction", "Duplicate Cutter"));
		if (AActor* Stage = EnsureStageActor())
		{
			Stage->Modify();
		}
		Duplicate->Modify();
		AttachPreviewComponentToStage(Duplicate, DuplicateTransform);
	}
	PreviewComponents.Add(Duplicate);

	CSGTransformMode = EUbstreamCSGTransformMode::Move;
	SelectLatestCSGPrimitiveForEditing();
	RefreshCSGPrimitiveVisuals();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnSoloMeshChanged.ExecuteIfBound();
	OnEditingStateChanged.ExecuteIfBound();

	StartCSGOperation(EUbstreamCSGOperation::Subtract);
	return true;
}

bool SUbstreamModelerViewport::RepeatLastCSGPrimitive()
{
	if (!LastCSGPrimitiveType.IsSet() || !CanAddCSGPrimitive())
	{
		return false;
	}
	if (!AddCSGPrimitive(LastCSGPrimitiveType.GetValue(), TOptional<FVector>(), LastCSGPrimitiveHalfSize))
	{
		return false;
	}
	StartCSGOperation(EUbstreamCSGOperation::Subtract);
	return true;
}

void SUbstreamModelerViewport::DeselectAllStageComponentsForUI()
{
	DeselectAllStageComponents();
	RefreshCSGPrimitiveVisuals();
	UpdateStageTransformGizmo();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnEditingStateChanged.ExecuteIfBound();
}

void SUbstreamModelerViewport::SetHoveredCSGPrimitiveComponent(UMeshComponent* Component)
{
	UMeshComponent* NewHovered = IsCSGPrimitiveComponent(Component) ? Component : nullptr;
	if (HoveredCSGPrimitive.Get() == NewHovered)
	{
		return;
	}
	HoveredCSGPrimitive = NewHovered;
	RefreshCSGPrimitiveVisuals();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

int32 SUbstreamModelerViewport::GetCSGPrimitiveCount() const
{
	TArray<UMeshComponent*> CSGPrimitives;
	AppendCSGPrimitiveComponents(CSGPrimitives);
	return CSGPrimitives.Num();
}

bool SUbstreamModelerViewport::HasSelectedCSGPrimitive() const
{
	USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	if (!SelectedComponents)
	{
		return false;
	}

	TArray<UActorComponent*> SelectedActorComponents;
	SelectedComponents->GetSelectedObjects<UActorComponent>(SelectedActorComponents);
	for (UActorComponent* ActorComponent : SelectedActorComponents)
	{
		if (IsCSGPrimitiveComponent(Cast<UMeshComponent>(ActorComponent)))
		{
			return true;
		}
	}

	return false;
}

int32 SUbstreamModelerViewport::GetSelectedCSGPrimitiveIndex() const
{
	TArray<UMeshComponent*> CSGPrimitives;
	AppendCSGPrimitiveComponents(CSGPrimitives);
	if (CSGPrimitives.IsEmpty())
	{
		return INDEX_NONE;
	}

	USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	if (!SelectedComponents)
	{
		return INDEX_NONE;
	}

	for (int32 Index = 0; Index < CSGPrimitives.Num(); ++Index)
	{
		if (SelectedComponents->IsSelected(CSGPrimitives[Index]))
		{
			return Index;
		}
	}

	return INDEX_NONE;
}

void SUbstreamModelerViewport::GetCSGPrimitiveComponents(TArray<UMeshComponent*>& OutComponents) const
{
	AppendCSGPrimitiveComponents(OutComponents);
}

void SUbstreamModelerViewport::GetSelectedCSGPrimitiveComponents(TArray<UMeshComponent*>& OutComponents) const
{
	USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	if (!SelectedComponents)
	{
		return;
	}

	TArray<UActorComponent*> SelectedActorComponents;
	SelectedComponents->GetSelectedObjects<UActorComponent>(SelectedActorComponents);
	for (UActorComponent* ActorComponent : SelectedActorComponents)
	{
		UMeshComponent* MeshComponent = Cast<UMeshComponent>(ActorComponent);
		if (IsCSGPrimitiveComponent(MeshComponent))
		{
			OutComponents.Add(MeshComponent);
		}
	}
}

UMeshComponent* SUbstreamModelerViewport::ResolveCSGPrimitiveForTool() const
{
	TArray<UMeshComponent*> CSGPrimitives;
	AppendCSGPrimitiveComponents(CSGPrimitives);
	if (CSGPrimitives.IsEmpty())
	{
		return nullptr;
	}

	USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	if (SelectedComponents)
	{
		TArray<UActorComponent*> SelectedActorComponents;
		SelectedComponents->GetSelectedObjects<UActorComponent>(SelectedActorComponents);
		TSet<UMeshComponent*> SelectedCSGPrimitives;
		for (UActorComponent* ActorComponent : SelectedActorComponents)
		{
			UMeshComponent* MeshComponent = Cast<UMeshComponent>(ActorComponent);
			if (IsCSGPrimitiveComponent(MeshComponent))
			{
				SelectedCSGPrimitives.Add(MeshComponent);
			}
		}

		for (int32 Index = CSGPrimitives.Num() - 1; Index >= 0; --Index)
		{
			if (SelectedCSGPrimitives.Contains(CSGPrimitives[Index]))
			{
				return CSGPrimitives[Index];
			}
		}
	}

	return CSGPrimitives.Last();
}

bool SUbstreamModelerViewport::SelectCSGPrimitiveForEditing(
	const UPrimitiveComponent* PrimitiveComponent,
	const bool bAddToSelection)
{
	if (HasAnyActiveStageTool())
	{
		return false;
	}

	UMeshComponent* MeshComponent = Cast<UMeshComponent>(const_cast<UPrimitiveComponent*>(PrimitiveComponent));
	if (!IsCSGPrimitiveComponent(MeshComponent))
	{
		return false;
	}

	// Every cutter in the live stack stays selectable mid-session - selecting
	// one just retargets the gizmo to it.
	if (HasActiveCSGSession() && FindCSGSessionBrushIndex(MeshComponent) == INDEX_NONE)
	{
		return false;
	}

	USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	if (!SelectedComponents)
	{
		return false;
	}

	if (!bAddToSelection)
	{
		SelectedComponents->DeselectAll();
		SelectedComponents->Select(MeshComponent);
	}
	else if (SelectedComponents->IsSelected(MeshComponent))
	{
		SelectedComponents->Deselect(MeshComponent);
	}
	else
	{
		SelectedComponents->Select(MeshComponent);
	}
	SelectedComponents->NoteSelectionChanged();

	RefreshCSGPrimitiveVisuals();
	if (HasSelectedCSGPrimitive())
	{
		SetCSGTransformMode(CSGTransformMode);
	}
	UpdateStageTransformGizmo();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnEditingStateChanged.ExecuteIfBound();
	return true;
}

bool SUbstreamModelerViewport::SelectLatestCSGPrimitiveForEditing()
{
	USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	if (!SelectedComponents)
	{
		return false;
	}

	TArray<UMeshComponent*> CSGPrimitives;
	AppendCSGPrimitiveComponents(CSGPrimitives);
	if (CSGPrimitives.IsEmpty())
	{
		return false;
	}

	SelectedComponents->DeselectAll();
	SelectedComponents->Select(CSGPrimitives.Last());
	SelectedComponents->NoteSelectionChanged();
	RefreshCSGPrimitiveVisuals();

	SetCSGTransformMode(CSGTransformMode);
	FocusStageViewport();
	UpdateStageTransformGizmo();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	return true;
}

bool SUbstreamModelerViewport::SelectNextCSGPrimitiveForEditing(const int32 Direction)
{
	if (HasAnyActiveStageTool())
	{
		return false;
	}

	TArray<UMeshComponent*> CSGPrimitives;
	AppendCSGPrimitiveComponents(CSGPrimitives);
	if (CSGPrimitives.IsEmpty())
	{
		return false;
	}

	const int32 Step = Direction < 0 ? -1 : 1;
	int32 SelectedIndex = GetSelectedCSGPrimitiveIndex();
	if (SelectedIndex == INDEX_NONE)
	{
		SelectedIndex = Step > 0 ? -1 : 0;
	}

	const int32 NextIndex = (SelectedIndex + Step + CSGPrimitives.Num()) % CSGPrimitives.Num();
	return SelectCSGPrimitiveForEditing(CSGPrimitives[NextIndex], /*bAddToSelection*/ false);
}

bool SUbstreamModelerViewport::SetCSGTransformMode(const EUbstreamCSGTransformMode TransformMode)
{
	if (!StageModeManager.IsValid())
	{
		return false;
	}

	FVector WidgetLocation = FVector::ZeroVector;
	if (!GetCSGTransformWidgetLocation(WidgetLocation))
	{
		return false;
	}

	const EUbstreamCSGTransformMode PreviousMode = CSGTransformMode;
	CSGTransformMode = TransformMode;
	switch (TransformMode)
	{
	case EUbstreamCSGTransformMode::Rotate:
		StageModeManager->SetWidgetMode(UE::Widget::WM_Rotate);
		break;
	case EUbstreamCSGTransformMode::Scale:
		// Non-uniform scale handles require Local space (engine CombinedTransformGizmo).
		StageModeManager->SetCoordSystem(COORD_Local);
		StageModeManager->SetWidgetMode(UE::Widget::WM_Scale);
		break;
	case EUbstreamCSGTransformMode::Move:
	default:
		StageModeManager->SetWidgetMode(UE::Widget::WM_Translate);
		break;
	}

	// Rebuild the live ITF gizmo so Scale-only handles appear immediately instead
	// of waiting on a tick edge case while the UI already shows Scale selected.
	if (PreviousMode != TransformMode && StageTransformGizmo.IsValid())
	{
		StageTransformGizmoTargets.Reset();
		UpdateStageTransformGizmo();
	}

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	return true;
}

bool SUbstreamModelerViewport::GetCSGTransformWidgetLocation(FVector& OutLocation) const
{
	USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	if (!SelectedComponents)
	{
		return false;
	}

	TArray<UActorComponent*> SelectedActorComponents;
	SelectedComponents->GetSelectedObjects<UActorComponent>(SelectedActorComponents);
	FVector AccumulatedLocation = FVector::ZeroVector;
	int32 ValidCount = 0;
	for (UActorComponent* ActorComponent : SelectedActorComponents)
	{
		UMeshComponent* MeshComponent = Cast<UMeshComponent>(ActorComponent);
		if (!IsCSGPrimitiveComponent(MeshComponent))
		{
			continue;
		}

		AccumulatedLocation += MeshComponent->GetComponentLocation();
		++ValidCount;
	}

	if (ValidCount <= 0)
	{
		return false;
	}

	OutLocation = AccumulatedLocation / static_cast<double>(ValidCount);
	return true;
}

bool SUbstreamModelerViewport::ApplyCSGTransformDelta(
	const FVector& Drag,
	const FRotator& Rot,
	const FVector& Scale)
{
	if (HasAnyActiveStageTool())
	{
		return false;
	}

	USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	if (!SelectedComponents)
	{
		return false;
	}

	TArray<UActorComponent*> SelectedActorComponents;
	SelectedComponents->GetSelectedObjects<UActorComponent>(SelectedActorComponents);
	TArray<UMeshComponent*> CSGComponents;
	for (UActorComponent* ActorComponent : SelectedActorComponents)
	{
		UMeshComponent* MeshComponent = Cast<UMeshComponent>(ActorComponent);
		if (IsCSGPrimitiveComponent(MeshComponent))
		{
			CSGComponents.Add(MeshComponent);
		}
	}

	if (CSGComponents.IsEmpty())
	{
		return false;
	}

	const bool bHasTranslation = !Drag.IsNearlyZero();
	const bool bHasRotation = !Rot.IsNearlyZero();
	const bool bHasScale = !Scale.IsNearlyZero();
	if (!bHasTranslation && !bHasRotation && !bHasScale)
	{
		return false;
	}

	FVector Pivot = FVector::ZeroVector;
	GetCSGTransformWidgetLocation(Pivot);
	const FQuat DeltaRotation = Rot.Quaternion();
	constexpr double MinCutterScale = 0.01;

	for (UMeshComponent* Component : CSGComponents)
	{
		if (!IsValid(Component))
		{
			continue;
		}

		Component->Modify();
		FTransform Transform = Component->GetComponentTransform();
		FVector Location = Transform.GetLocation();
		FQuat Rotation = Transform.GetRotation();
		FVector LocalScale = Transform.GetScale3D();

		if (bHasTranslation)
		{
			Location += Drag;
		}
		if (bHasRotation)
		{
			Location = Pivot + DeltaRotation.RotateVector(Location - Pivot);
			Rotation = (DeltaRotation * Rotation).GetNormalized();
		}
		if (bHasScale)
		{
			// Continuous (floating) proportional scale. Legacy widget deltas are
			// tiny; a modest gain keeps motion smooth without the 12x jumpiness.
			// Sidebar Scale slider remains the precision control.
			constexpr double CutterScaleDragGain = 3.25;
			LocalScale.X = FMath::Max(MinCutterScale, LocalScale.X * (1.0 + Scale.X * CutterScaleDragGain));
			LocalScale.Y = FMath::Max(MinCutterScale, LocalScale.Y * (1.0 + Scale.Y * CutterScaleDragGain));
			LocalScale.Z = FMath::Max(MinCutterScale, LocalScale.Z * (1.0 + Scale.Z * CutterScaleDragGain));
		}

		Transform.SetLocation(Location);
		Transform.SetRotation(Rotation);
		Transform.SetScale3D(LocalScale);
		Component->SetWorldTransform(Transform);
		Component->UpdateBounds();
	}

	// Dragging a session brush is what drives the live preview: hand the new
	// transforms to the session and let latest-wins pacing do the rest.
	PushSessionBrushTransforms();

	// Do not rebuild the ITF gizmo every scale delta (Scale uses the legacy widget).
	if (CSGTransformMode != EUbstreamCSGTransformMode::Scale)
	{
		UpdateStageTransformGizmo();
	}
	bCaptureMirrorRefreshPending = true;
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnSoloMeshChanged.ExecuteIfBound();
	OnEditingStateChanged.ExecuteIfBound();
	return true;
}

bool SUbstreamModelerViewport::SelectStageComponentForEditing(
	const UPrimitiveComponent* PrimitiveComponent,
	const bool bAddToSelection)
{
	// The subject is hidden behind the live preview during a CSG session, so
	// stage-component selection would grab an invisible mesh.
	if (HasAnyActiveStageTool() || HasActiveCSGSession())
	{
		return false;
	}

	UMeshComponent* MeshComponent = const_cast<UMeshComponent*>(Cast<UMeshComponent>(PrimitiveComponent));
	if (!UbstreamMeshComponents::HasRenderableGeometry(MeshComponent) || IsCSGPrimitiveComponent(MeshComponent))
	{
		return false;
	}

	bool bIsStageComponent = false;
	for (UMeshComponent* Component : PreviewComponents)
	{
		if (Component == MeshComponent)
		{
			bIsStageComponent = true;
			break;
		}
	}
	if (!bIsStageComponent)
	{
		return false;
	}

	USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	if (!SelectedComponents)
	{
		return false;
	}

	if (!bAddToSelection)
	{
		SelectedComponents->DeselectAll();
	}
	SelectedComponents->Select(MeshComponent);
	SelectedComponents->NoteSelectionChanged();
	StageModeManager->SetWidgetMode(UE::Widget::WM_Translate);
	UpdateStageTransformGizmo();

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
	OnEditingStateChanged.ExecuteIfBound();
	return true;
}

bool SUbstreamModelerViewport::ClearCSGPrimitives()
{
	if (HasAnyActiveStageTool())
	{
		return false;
	}

	const int32 PrimitiveCount = GetCSGPrimitiveCount();
	if (PrimitiveCount <= 0)
	{
		return false;
	}

	// Live CSG is the normal state while cutters exist (adding a cutter starts a
	// session). Clear must cancel first so the subject mesh is restored, then
	// remove every cutter — same pattern as DeleteSelectedCSGPrimitives.
	if (HasActiveCSGSession())
	{
		CancelCSGSession();
	}

	FScopedTransaction ClearTransaction(NSLOCTEXT("SUbstreamModelerViewport", "ClearCuttersTransaction", "Clear Cutters"));
	if (AActor* Stage = StageActor.Get())
	{
		Stage->Modify();
	}

	RemoveCSGPrimitiveComponents(/*bNotify*/ true);
	return true;
}

bool SUbstreamModelerViewport::DeleteSelectedCSGPrimitives()
{
	if (HasAnyActiveStageTool())
	{
		return false;
	}

	USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	if (!SelectedComponents)
	{
		return false;
	}

	TArray<UActorComponent*> SelectedActorComponents;
	SelectedComponents->GetSelectedObjects<UActorComponent>(SelectedActorComponents);
	TSet<UMeshComponent*> ComponentsToDelete;
	for (UActorComponent* ActorComponent : SelectedActorComponents)
	{
		UMeshComponent* MeshComponent = Cast<UMeshComponent>(ActorComponent);
		if (IsCSGPrimitiveComponent(MeshComponent))
		{
			ComponentsToDelete.Add(MeshComponent);
		}
	}

	if (ComponentsToDelete.IsEmpty())
	{
		return false;
	}

	// Deleting a cutter out of a live stack: restore the pre-session mesh,
	// remove the cutter, and rebuild the session with the survivors.
	const bool bSessionWasActive = HasActiveCSGSession();
	if (bSessionWasActive)
	{
		CancelCSGSession();
	}

	FScopedTransaction DeleteTransaction(NSLOCTEXT("SUbstreamModelerViewport", "DeleteCutterTransaction", "Delete Cutter"));
	if (AActor* Stage = StageActor.Get())
	{
		Stage->Modify();
	}

	USelection* EditorSelectedComponents = GEditor ? GEditor->GetSelectedComponents() : nullptr;
	for (int32 Index = PreviewComponents.Num() - 1; Index >= 0; --Index)
	{
		UMeshComponent* Component = PreviewComponents[Index];
		if (!ComponentsToDelete.Contains(Component))
		{
			continue;
		}

		SelectedComponents->Deselect(Component);
		if (EditorSelectedComponents)
		{
			EditorSelectedComponents->Deselect(Component);
		}
		if (IsValid(Component))
		{
			Component->Modify();
			if (UDynamicMeshComponent* DynamicCutter = Cast<UDynamicMeshComponent>(Component))
			{
				StoreCutterMeshForUndo(DynamicCutter);
			}
			Component->DestroyComponent();
		}
		PreviewComponents.RemoveAt(Index);
	}

	TArray<UMeshComponent*> RemainingCSGPrimitives;
	AppendCSGPrimitiveComponents(RemainingCSGPrimitives);
	if (!RemainingCSGPrimitives.IsEmpty())
	{
		SelectLatestCSGPrimitiveForEditing();
		if (bSessionWasActive)
		{
			// The stack lives on without the deleted cutter.
			StartCSGOperation(GetSelectedCSGOperation());
		}
	}
	else
	{
		// Accept/undo with no cutters left: clear the selection so no gizmo
		// lingers - chrome exists only while there is something to drag.
		DeselectAllStageComponents();
	}
	RefreshCSGPrimitiveVisuals();
	UpdateStageTransformGizmo();

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}

	OnSoloMeshChanged.ExecuteIfBound();
	OnEditingStateChanged.ExecuteIfBound();
	return true;
}

void SUbstreamModelerViewport::ClearObjects()
{
	ClearObjectsInternal(true);
}

void SUbstreamModelerViewport::UpdateStageRealtime()
{
	if (!ViewportClient.IsValid())
	{
		return;
	}

	// Non-realtime preview viewports only redraw on Invalidate. The first paint
	// after load often composites a stale skylight/environment frame (the
	// "double overlay" that vanishes on mouse move). Keep realtime on whenever
	// there is a subject so captures settle without requiring interaction.
	const bool bWantRealtime = bEditModeActive || HasSoloMesh();
	ViewportClient->SetRealtime(bWantRealtime);
	ViewportClient->Invalidate();
}

void SUbstreamModelerViewport::ClearObjectsInternal(bool bNotify)
{
	AbandonRemesh();
	if (!PreviewScene.IsValid())
	{
		return;
	}

	UWorld* PreviewWorld = PreviewScene->GetWorld();

	// Active tools hold references to the preview components; close the edit
	// session before destroying them. Combined transform gizmos destroy actor
	// handles during shutdown; preview worlds have no engine world context, so
	// temporarily mark the world inactive to keep that teardown warning-free.
	RunWithPreviewWorldInactive(PreviewScene, [this]()
	{
		ExitEditMode();
	});
	SetStagePreviewComponentsSuppressed(false);

	TSet<UMeshComponent*> RemovedComponents;
	for (UMeshComponent* Component : PreviewComponents)
	{
		if (IsValid(Component))
		{
			RemovedComponents.Add(Component);
			Component->DestroyComponent();
		}
	}

	// The stage actor is intentionally kept alive and reused across subject
	// loads: explicitly destroying actors in the preview world trips typed
	// element teardown assertions; the world owns its lifetime instead.

	if (PreviewWorld)
	{
		for (TObjectIterator<UMeshComponent> It; It; ++It)
		{
			UMeshComponent* Component = *It;
			if (!IsValid(Component)
				|| RemovedComponents.Contains(Component)
				|| Component->GetWorld() != PreviewWorld
				|| !Component->IsRegistered()
				|| !Component->IsVisible())
			{
				continue;
			}

			PreviewScene->RemoveComponent(Component);
			Component->DestroyComponent();
		}
	}

	PreviewComponents.Reset();
	SoloMesh.Reset();
	SourceActors.Reset();
	SourceComponents.Reset();
	SoloMeshLabel.Reset();
	SoloMeshPath.Reset();
	SourceKind = ESourceKind::None;
	SourceSignatureAtLoad = 0;
	LastSourceDirtyCheckSeconds = -1.0;
	bHasLastCaptureCamera = false;
	LastCaptureCameraChangeTimeSeconds = -1.0;
	NextBoxIndex = 1;
	bHasUncommittedStageEdits = false;

	UpdateStageRealtime();

	if (bNotify)
	{
		OnSoloMeshChanged.ExecuteIfBound();
	}
}

void SUbstreamModelerViewport::RefreshCSGPrimitiveVisuals()
{
	TSet<const UMeshComponent*> SelectedCSGPrimitives;
	if (USelection* SelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr)
	{
		TArray<UActorComponent*> SelectedActorComponents;
		SelectedComponents->GetSelectedObjects<UActorComponent>(SelectedActorComponents);
		for (UActorComponent* ActorComponent : SelectedActorComponents)
		{
			const UMeshComponent* MeshComponent = Cast<UMeshComponent>(ActorComponent);
			if (IsCSGPrimitiveComponent(MeshComponent))
			{
				SelectedCSGPrimitives.Add(MeshComponent);
			}
		}
	}

	for (UMeshComponent* Component : PreviewComponents)
	{
		UDynamicMeshComponent* DynamicMeshComponent = Cast<UDynamicMeshComponent>(Component);
		if (!IsCSGPrimitiveComponent(DynamicMeshComponent))
		{
			continue;
		}

		const bool bSelected = SelectedCSGPrimitives.Contains(DynamicMeshComponent);
		const bool bHovered = HoveredCSGPrimitive.Get() == DynamicMeshComponent;
		const bool bIsSessionBrush = HasActiveCSGSession()
			&& FindCSGSessionBrushIndex(DynamicMeshComponent) != INDEX_NONE;

		// Professional op color language: what a cutter DOES is readable at a
		// glance. Subtract carves (warm red), Union adds (green), Intersect
		// keeps the overlap (blue). Selection brightens, hover nudges,
		// unselected cutters sit dimmed.
		FLinearColor OpWireColor(1.0f, 0.42f, 0.30f);
		FColor OpBodyColor(214, 88, 60, 255);
		switch (GetCSGPrimitiveOperation(DynamicMeshComponent))
		{
		case EUbstreamCSGOperation::Union:
			OpWireColor = FLinearColor(0.35f, 0.95f, 0.50f);
			OpBodyColor = FColor(74, 190, 110, 255);
			break;
		case EUbstreamCSGOperation::Intersect:
			OpWireColor = FLinearColor(0.30f, 0.62f, 1.0f);
			OpBodyColor = FColor(62, 138, 232, 255);
			break;
		default:
			break;
		}
		const float Emphasis = bSelected ? 1.0f : (bHovered ? 0.8f : 0.5f);
		const FLinearColor WireColor = OpWireColor * Emphasis;
		const FColor BodyColor(
			static_cast<uint8>(OpBodyColor.R * Emphasis),
			static_cast<uint8>(OpBodyColor.G * Emphasis),
			static_cast<uint8>(OpBodyColor.B * Emphasis),
			255);

		// Session brushes must NOT use Constant color mode: the proxy force-
		// swaps in the opaque vertex-color material whenever ColorMode != None
		// (BaseDynamicMeshSceneProxy.cpp), which beats the invisible ghost
		// override material and paints a solid body that hides the very cut
		// the user is steering. ColorMode None lets the ghost render.
		if (bIsSessionBrush)
		{
			DynamicMeshComponent->SetColorOverrideMode(EDynamicMeshComponentColorOverrideMode::None);
		}
		else
		{
			DynamicMeshComponent->SetColorOverrideMode(EDynamicMeshComponentColorOverrideMode::Constant);
			DynamicMeshComponent->SetConstantOverrideColor(BodyColor);
		}
		DynamicMeshComponent->SetEnableWireframeRenderPass(true);
		DynamicMeshComponent->WireframeColor = WireColor;
		DynamicMeshComponent->MarkRenderStateDirty();
	}

	PushStageSelectionToProxies();
}

void SUbstreamModelerViewport::RemoveCSGPrimitiveComponents(bool bNotify)
{
	USelection* StageSelectedComponents = StageModeManager.IsValid() ? StageModeManager->GetSelectedComponents() : nullptr;
	USelection* EditorSelectedComponents = GEditor ? GEditor->GetSelectedComponents() : nullptr;

	for (int32 Index = PreviewComponents.Num() - 1; Index >= 0; --Index)
	{
		UMeshComponent* Component = PreviewComponents[Index];
		if (!IsCSGPrimitiveComponent(Component))
		{
			continue;
		}

		if (StageSelectedComponents)
		{
			StageSelectedComponents->Deselect(Component);
		}
		if (EditorSelectedComponents)
		{
			EditorSelectedComponents->Deselect(Component);
		}

		if (IsValid(Component))
		{
			Component->Modify();
			if (UDynamicMeshComponent* DynamicCutter = Cast<UDynamicMeshComponent>(Component))
			{
				StoreCutterMeshForUndo(DynamicCutter);
			}
			Component->DestroyComponent();
		}
		PreviewComponents.RemoveAt(Index);
	}

	DeselectAllStageComponents();
	RefreshCSGPrimitiveVisuals();
	UpdateStageTransformGizmo();

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}

	if (bNotify)
	{
		OnSoloMeshChanged.ExecuteIfBound();
		OnEditingStateChanged.ExecuteIfBound();
	}
}

FString SUbstreamModelerViewport::BuildSceneStateJson(const FString& SessionId) const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("type"), TEXT("modeling_selection_state"));
	Root->SetStringField(TEXT("protocol"), TEXT("ubstream.websocket.v1"));
	Root->SetStringField(TEXT("sessionId"), SessionId);

	TArray<TSharedPtr<FJsonValue>> ObjectValues;
	for (int32 Index = 0; Index < PreviewComponents.Num(); ++Index)
	{
		const UMeshComponent* Component = PreviewComponents[Index];
		if (!IsValid(Component))
		{
			continue;
		}

		const FTransform Transform = Component->GetComponentTransform();
		const FVector Location = Transform.GetLocation();
		const FRotator Rotation = Transform.GetRotation().Rotator();
		const FVector Scale = Transform.GetScale3D();

		TSharedRef<FJsonObject> TransformJson = MakeShared<FJsonObject>();
		TransformJson->SetArrayField(TEXT("location"), VectorToJsonArray(Location));
		TransformJson->SetArrayField(TEXT("rotation"), RotatorToJsonArray(Rotation));
		TransformJson->SetArrayField(TEXT("scale"), VectorToJsonArray(Scale));

		const FBoxSphereBounds ComponentBounds = GetReliableComponentBounds(*Component);
		TSharedRef<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
		BoundsJson->SetArrayField(TEXT("origin"), VectorToJsonArray(ComponentBounds.Origin));
		BoundsJson->SetArrayField(TEXT("extent"), VectorToJsonArray(ComponentBounds.BoxExtent));

		const FString Id = Component->ComponentTags.Num() > 0
			? Component->ComponentTags[0].ToString()
			: FString::Printf(TEXT("box_%03d"), Index + 1);

		const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component);
		const UStaticMesh* ComponentStaticMesh = StaticMeshComponent ? StaticMeshComponent->GetStaticMesh() : nullptr;

		TSharedRef<FJsonObject> ObjectJson = MakeShared<FJsonObject>();
		ObjectJson->SetStringField(TEXT("id"), Id);
		ObjectJson->SetStringField(TEXT("type"), HasSoloMesh() ? TEXT("StaticMeshActor") : TEXT("PreviewMesh"));
		ObjectJson->SetStringField(TEXT("assetPath"), ComponentStaticMesh ? ComponentStaticMesh->GetPathName() : SoloMeshPath);
		ObjectJson->SetObjectField(TEXT("transform"), TransformJson);
		ObjectJson->SetObjectField(TEXT("bounds"), BoundsJson);
		ObjectValues.Add(MakeShared<FJsonValueObject>(ObjectJson));
	}

	Root->SetArrayField(TEXT("objects"), ObjectValues);

	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root, Writer);
	return Output;
}

FUbstreamCaptureCamera SUbstreamModelerViewport::BuildCaptureCamera(int32 Width, int32 Height) const
{
	FUbstreamCaptureCamera Camera;
	if (!ViewportClient.IsValid())
	{
		return Camera;
	}

	if (ViewportClient->bUsingOrbitCamera)
	{
		// In orbit mode the stored location/rotation are in orbit convention,
		// not the rendered camera pose; recover it the way the engine's tools
		// context does, from the inverse orbit matrix.
		const FMatrix OrbitToWorld = ViewportClient->GetViewTransform().ComputeOrbitMatrix().InverseFast();
		Camera.Location = OrbitToWorld.GetOrigin();
		Camera.Rotation = OrbitToWorld.Rotator();
	}
	else
	{
		Camera.Location = ViewportClient->GetViewLocation();
		Camera.Rotation = ViewportClient->GetViewRotation();
	}

	Camera.FovDegrees = ViewportClient->ViewFOV;
	Camera.AspectRatio = Height > 0 ? static_cast<float>(Width) / static_cast<float>(Height) : 1.0f;
	// FocusOnBounds shrinks the viewport's near plane for small subjects; the
	// capture must use the same plane or close-framed geometry clips to black.
	Camera.NearClipPlane = ViewportClient->GetNearClipPlane();
	return Camera;
}

FString SUbstreamModelerViewport::CaptureRgbFrame()
{
	if (!PreviewScene.IsValid() || !ViewportClient.IsValid())
	{
		return TEXT("Capture failed: viewport is not ready");
	}

	FUbstreamCaptureSettings Settings;
	Settings.Width = 1024;
	Settings.Height = 1024;

	UpdateStageLighting();
	const FUbstreamCaptureCamera Camera = BuildCaptureCamera(Settings.Width, Settings.Height);

	const FUbstreamCaptureResult Result = FUbstreamCaptureService::CaptureRgb(*PreviewScene, Camera, Settings);
	if (!Result.bSuccess)
	{
		return FString::Printf(TEXT("Capture failed: %s"), *Result.ErrorMessage);
	}

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}

	return FString::Printf(
		TEXT("Captured RGB %dx%d (%lld bytes)%s%s"),
		Result.Size.X,
		Result.Size.Y,
		static_cast<long long>(Result.ColorPngBytes.Num()),
		Result.ColorDebugPath.IsEmpty() ? TEXT("") : TEXT(" -> "),
		*Result.ColorDebugPath);
}

FUbstreamEditorWorldCaptureResult SUbstreamModelerViewport::CaptureFrameBundle(uint64 FrameId, const FUbstreamCaptureSettings& Settings)
{
	// Synchronous one-shot path: capture + encode inline.
	return FUbstreamCaptureService::EncodeCapturedFrame(CaptureFramePixels(FrameId, Settings));
}

TSharedPtr<FUbstreamAsyncFrameReadback, ESPMode::ThreadSafe> SUbstreamModelerViewport::BeginAsyncCaptureFramePixels(
	const uint64 FrameId, const FUbstreamCaptureSettings& Settings)
{
	if (FUbstreamModelerEditorModule::IsInsideRenderFlush())
	{
		return nullptr;
	}
	if (!PreviewScene.IsValid() || !ViewportClient.IsValid() || !HasSoloMesh())
	{
		return nullptr;
	}

	UpdateStageLighting();
	const FUbstreamCaptureCamera Camera = BuildCaptureCamera(Settings.Width, Settings.Height);

	if (ShouldCaptureWholeScene())
	{
		SyncCaptureMirrorScene();
		if (CaptureMirrorScene.IsValid())
		{
			USceneCaptureComponent2D* CaptureComp = EnsureCaptureComponent(CaptureMirrorScene);
			USceneCaptureComponent2D* DepthComp = Settings.bCaptureDepth ? EnsureDepthCaptureComponent(CaptureMirrorScene) : nullptr;
			return FUbstreamAsyncFrameReadback::Begin(
				*CaptureMirrorScene, Camera, FrameId, Settings, TArray<UPrimitiveComponent*>(), &CaptureTargetPool, CaptureComp, DepthComp);
		}
		return nullptr;
	}

	TArray<UPrimitiveComponent*> ShowOnlyComponents;
	for (UMeshComponent* Component : PreviewComponents)
	{
		if (IsValid(Component))
		{
			ShowOnlyComponents.Add(Component);
		}
	}
	USceneCaptureComponent2D* CaptureComp = EnsureCaptureComponent(PreviewScene);
	USceneCaptureComponent2D* DepthComp = Settings.bCaptureDepth ? EnsureDepthCaptureComponent(PreviewScene) : nullptr;
	return FUbstreamAsyncFrameReadback::Begin(
		*PreviewScene, Camera, FrameId, Settings, ShowOnlyComponents, &CaptureTargetPool, CaptureComp, DepthComp);
}

FUbstreamCapturePixels SUbstreamModelerViewport::CaptureFramePixels(uint64 FrameId, const FUbstreamCaptureSettings& Settings)
{
	FUbstreamCapturePixels Pixels;
	// Guard before mirror synchronization or capture-component registration,
	// which can themselves submit rendering work during a nested ticker call.
	if (FUbstreamModelerEditorModule::IsInsideRenderFlush())
	{
		Pixels.ErrorMessage = TEXT("Capture deferred while rendering commands are being flushed.");
		return Pixels;
	}
	if (!PreviewScene.IsValid() || !ViewportClient.IsValid())
	{
		Pixels.ErrorMessage = TEXT("Solo viewport is not ready");
		return Pixels;
	}

	if (!HasSoloMesh())
	{
		Pixels.ErrorMessage = TEXT("No isolated subject is loaded");
		return Pixels;
	}

	UpdateStageLighting();
	const FUbstreamCaptureCamera Camera = BuildCaptureCamera(Settings.Width, Settings.Height);

	// While editing, the stage world is full of things that must never reach
	// the stream (gizmo handles, selection wireframes, brush indicators, tool
	// highlight materials baked into the preview components themselves). No
	// show-only list or show flag can separate those from the real geometry,
	// so the stream renders a private mirror world that contains nothing but
	// copies of the actual meshes.
	if (ShouldCaptureWholeScene())
	{
		SyncCaptureMirrorScene();
		if (CaptureMirrorScene.IsValid())
		{
			USceneCaptureComponent2D* CaptureComp = EnsureCaptureComponent(CaptureMirrorScene);
			return FUbstreamCaptureService::CaptureFramePixels(
				*CaptureMirrorScene, Camera, FrameId, Settings, TArray<UPrimitiveComponent*>(), &CaptureTargetPool, CaptureComp);
		}
	}

	TArray<UPrimitiveComponent*> ShowOnlyComponents;
	for (UMeshComponent* Component : PreviewComponents)
	{
		if (IsValid(Component))
		{
			ShowOnlyComponents.Add(Component);
		}
	}

	USceneCaptureComponent2D* CaptureComp = EnsureCaptureComponent(PreviewScene);
	return FUbstreamCaptureService::CaptureFramePixels(
		*PreviewScene, Camera, FrameId, Settings, ShowOnlyComponents, &CaptureTargetPool, CaptureComp);
}

USceneCaptureComponent2D* SUbstreamModelerViewport::EnsureCaptureComponent(const TSharedPtr<FAdvancedPreviewScene>& Scene)
{
	if (!Scene.IsValid())
	{
		return nullptr;
	}

	USceneCaptureComponent2D* Component = CaptureComponent.Get();
	const TSharedPtr<FAdvancedPreviewScene> CurrentScene = CaptureComponentScene.Pin();
	if (Component && CurrentScene == Scene)
	{
		return Component;
	}

	// Moving between scenes (preview <-> mirror on edit-mode transitions): drop
	// the old registration before re-adding to the new world.
	if (Component && CurrentScene.IsValid())
	{
		CurrentScene->RemoveComponent(Component);
	}

	if (!Component)
	{
		Component = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Component->bCaptureEveryFrame = false;
		Component->bCaptureOnMovement = false;
		CaptureComponent.Reset(Component);
	}

	Scene->AddComponent(Component, FTransform::Identity);
	CaptureComponentScene = Scene;
	return Component;
}

USceneCaptureComponent2D* SUbstreamModelerViewport::EnsureDepthCaptureComponent(const TSharedPtr<FAdvancedPreviewScene>& Scene)
{
	if (!Scene.IsValid())
	{
		return nullptr;
	}

	USceneCaptureComponent2D* Component = DepthCaptureComponent.Get();
	const TSharedPtr<FAdvancedPreviewScene> CurrentScene = DepthCaptureComponentScene.Pin();
	if (Component && CurrentScene == Scene)
	{
		return Component;
	}

	if (Component && CurrentScene.IsValid())
	{
		CurrentScene->RemoveComponent(Component);
	}

	if (!Component)
	{
		Component = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Component->bCaptureEveryFrame = false;
		Component->bCaptureOnMovement = false;
		DepthCaptureComponent.Reset(Component);
	}

	Scene->AddComponent(Component, FTransform::Identity);
	DepthCaptureComponentScene = Scene;
	return Component;
}

void SUbstreamModelerViewport::ReleaseCaptureComponent()
{
	// Unregister from the scene if it is still alive, then drop our strong
	// reference and let GC / world teardown collect the component. No explicit
	// DestroyComponent: like ResetCaptureMirrorScene, we avoid forcing destruction
	// during world teardown.
	if (USceneCaptureComponent2D* Component = CaptureComponent.Get())
	{
		if (const TSharedPtr<FAdvancedPreviewScene> Scene = CaptureComponentScene.Pin())
		{
			Scene->RemoveComponent(Component);
		}
	}
	if (USceneCaptureComponent2D* DepthComponent = DepthCaptureComponent.Get())
	{
		if (const TSharedPtr<FAdvancedPreviewScene> Scene = DepthCaptureComponentScene.Pin())
		{
			Scene->RemoveComponent(DepthComponent);
		}
	}
	DepthCaptureComponentScene.Reset();
	DepthCaptureComponent.Reset();
	CaptureComponentScene.Reset();
	CaptureComponent.Reset();
}

namespace
{
	bool IsFullyTranslucentMeshComponent(const UMeshComponent& Component)
	{
		const int32 MaterialCount = Component.GetNumMaterials();
		if (MaterialCount == 0)
		{
			return false;
		}

		for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
		{
			const UMaterialInterface* Material = Component.GetMaterial(MaterialIndex);
			if (!Material)
			{
				return false;
			}
			const EBlendMode BlendMode = Material->GetBlendMode();
			if (BlendMode == BLEND_Opaque || BlendMode == BLEND_Masked)
			{
				return false;
			}
		}
		return true;
	}
}

AActor* SUbstreamModelerViewport::EnsureCaptureMirrorActor()
{
	if (AActor* ExistingActor = CaptureMirrorActor.Get())
	{
		return ExistingActor;
	}

	UWorld* MirrorWorld = CaptureMirrorScene.IsValid() ? CaptureMirrorScene->GetWorld() : nullptr;
	if (!MirrorWorld)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParameters.ObjectFlags = RF_Transient;
	AActor* NewActor = MirrorWorld->SpawnActor<AActor>(SpawnParameters);
	if (!NewActor)
	{
		return nullptr;
	}

	USceneComponent* RootComponent = NewObject<USceneComponent>(NewActor, TEXT("CaptureMirrorRoot"), RF_Transient);
	NewActor->SetRootComponent(RootComponent);
	NewActor->AddInstanceComponent(RootComponent);
	RootComponent->RegisterComponent();
	CaptureMirrorActor = NewActor;
	return NewActor;
}

void SUbstreamModelerViewport::SyncCaptureMirrorScene()
{
	if (!CaptureMirrorScene.IsValid())
	{
		CaptureMirrorScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
		DetachSceneFromAssetViewerBroadcast(*CaptureMirrorScene);
		CaptureMirrorScene->SetFloorVisibility(false, true);
		CaptureMirrorScene->SetEnvironmentVisibility(false, true);
		InitializeSceneLighting(*CaptureMirrorScene);
	}
	UpdateStageLighting();

	AActor* MirrorActor = EnsureCaptureMirrorActor();
	if (!MirrorActor)
	{
		return;
	}

	TArray<UPrimitiveComponent*> SourcePrimitives;
	AppendEditSessionMeshComponents(SourcePrimitives);

	TArray<UMeshComponent*> Sources;
	for (UPrimitiveComponent* Primitive : SourcePrimitives)
	{
		UMeshComponent* Source = Cast<UMeshComponent>(Primitive);
		if (!Source)
		{
			continue;
		}
		// Mesh-based tool indicators (brush spheres, plane fills) are fully
		// translucent; real subjects always carry an opaque or masked slot.
		if (IsFullyTranslucentMeshComponent(*Source))
		{
			continue;
		}
		// CSG cutters are interaction chrome: the stream must only ever show
		// the model (the live boolean preview already reflects their effect).
		if (IsCSGPrimitiveComponent(Source))
		{
			continue;
		}
		Sources.Add(Source);
	}

	for (int32 Index = CaptureMirrors.Num() - 1; Index >= 0; --Index)
	{
		if (!CaptureMirrors[Index].Source.IsValid())
		{
			if (UMeshComponent* StaleMirror = CaptureMirrors[Index].Mirror)
			{
				StaleMirror->DestroyComponent();
			}
			CaptureMirrors.RemoveAt(Index);
		}
	}

	const bool bForceRefresh = bCaptureMirrorRefreshPending;
	const bool bToolActive = HasAnyActiveStageTool();
	UUbstreamVertexSculptTool* RevisionSculpt = Cast<UUbstreamVertexSculptTool>(GetActiveVertexSculptTool());
	bCaptureMirrorRefreshPending = false;
	for (UMeshComponent* Source : Sources)
	{
		FUbstreamCaptureMirror* Entry = CaptureMirrors.FindByPredicate(
			[Source](const FUbstreamCaptureMirror& Candidate) { return Candidate.Source.Get() == Source; });
		UMeshComponent* Mirror = Entry ? Entry->Mirror.Get() : nullptr;

		if (!Mirror)
		{
			if (Source->IsA<UBaseDynamicMeshComponent>())
			{
				Mirror = NewObject<UDynamicMeshComponent>(MirrorActor, NAME_None, RF_Transient);
			}
			else if (Source->IsA<UStaticMeshComponent>())
			{
				Mirror = NewObject<UStaticMeshComponent>(MirrorActor, NAME_None, RF_Transient);
			}
			else
			{
				continue;
			}

			MirrorActor->AddInstanceComponent(Mirror);
			Mirror->AttachToComponent(MirrorActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
			Mirror->RegisterComponent();

			FUbstreamCaptureMirror& NewEntry = CaptureMirrors.AddDefaulted_GetRef();
			NewEntry.Source = Source;
			NewEntry.Mirror = Mirror;
			Entry = &NewEntry;
		}

		// Sources hidden by a tool (which replaced them with a working mesh)
		// must hide their mirrors too, or the stream would show stale ghosts.
		// While hidden the source's mesh can change underneath (tool accept
		// writes the result back), so force a recopy on the next visible sync.
		const bool bSourceVisible = Source->IsVisible();
		Mirror->SetVisibility(bSourceVisible);
		if (!bSourceVisible)
		{
			Entry->MeshFingerprint = MAX_uint64;
			Entry->RevisionTool.Reset();
			continue;
		}

		if (const UBaseDynamicMeshComponent* DynamicSource = Cast<UBaseDynamicMeshComponent>(Source))
		{
			if (UDynamicMeshComponent* DynamicMirror = Cast<UDynamicMeshComponent>(Mirror))
			{
				DynamicMirror->SetColorOverrideMode(DynamicSource->GetColorOverrideMode());
				DynamicMirror->SetConstantOverrideColor(DynamicSource->GetConstantOverrideColor());
				DynamicMirror->SetEnableWireframeRenderPass(DynamicSource->GetEnableWireframeRenderPass());
				DynamicMirror->WireframeColor = DynamicSource->WireframeColor;

				uint64 SculptRevision = 0;
				const bool bStableSculpt = RevisionSculpt
					&& RevisionSculpt->TryGetStableCaptureRevision(DynamicSource, SculptRevision);
				const bool bForceCopy = bForceRefresh || bToolActive;
				// Unknown tools and in-progress strokes still copy every capture.
				// Do not also scan a mesh whose copy is already required.
				const uint64 Fingerprint = bStableSculpt || bForceCopy
					? MAX_uint64 : ComputeDynamicMeshFingerprint(*DynamicSource);
				const bool bNeedsCopy = bStableSculpt
					? bForceRefresh || Entry->RevisionTool.Get() != RevisionSculpt || Entry->SculptRevision != SculptRevision
					: bForceCopy || Entry->MeshFingerprint != Fingerprint;

				if (bNeedsCopy)
				{
					DynamicSource->ProcessMesh([DynamicMirror](const UE::Geometry::FDynamicMesh3& Mesh)
					{
						DynamicMirror->GetDynamicMesh()->SetMesh(Mesh);
					});
					Entry->MeshFingerprint = Fingerprint;
				}
				Entry->RevisionTool = bStableSculpt ? RevisionSculpt : nullptr;
				Entry->SculptRevision = SculptRevision;
			}
		}
		else if (const UStaticMeshComponent* StaticSource = Cast<UStaticMeshComponent>(Source))
		{
			if (UStaticMeshComponent* StaticMirror = Cast<UStaticMeshComponent>(Mirror))
			{
				if (StaticMirror->GetStaticMesh() != StaticSource->GetStaticMesh())
				{
					StaticMirror->SetStaticMesh(StaticSource->GetStaticMesh());
				}
			}
		}

		Mirror->SetWorldTransform(Source->GetComponentTransform());
		const int32 MaterialCount = Source->GetNumMaterials();
		for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
		{
			UMaterialInterface* Material = Source->GetMaterial(MaterialIndex);
			const EBlendMode BlendMode = Material ? Material->GetBlendMode() : BLEND_Opaque;
			// Skip transient translucent overrides (tool "working" materials)
			// so the stream keeps the last real surface look.
			if (Material && (BlendMode == BLEND_Opaque || BlendMode == BLEND_Masked))
			{
				Mirror->SetMaterial(MaterialIndex, Material);
			}
		}
		Mirror->UpdateBounds();
	}
}

void SUbstreamModelerViewport::ResetCaptureMirrorScene()
{
	// If the persistent capture components are registered in the mirror scene,
	// unregister them before the scene (and its world) are torn down so they do
	// not dangle. Both color and depth are reused across scenes; leaving either
	// attached while CaptureMirrorScene is dropped skips RemoveComponent on the
	// next Ensure* (the scene weak ptr goes stale) and can leave a half-torn
	// registration. They re-register into the next capture scene on demand.
	if (CaptureMirrorScene.IsValid())
	{
		if (USceneCaptureComponent2D* Component = CaptureComponent.Get())
		{
			if (CaptureComponentScene.Pin() == CaptureMirrorScene)
			{
				CaptureMirrorScene->RemoveComponent(Component);
				CaptureComponentScene.Reset();
			}
		}
		if (USceneCaptureComponent2D* DepthComponent = DepthCaptureComponent.Get())
		{
			if (DepthCaptureComponentScene.Pin() == CaptureMirrorScene)
			{
				CaptureMirrorScene->RemoveComponent(DepthComponent);
				DepthCaptureComponentScene.Reset();
			}
		}
	}

	// Dropping the scene tears down the mirror world, its actor, and all
	// mirror components; explicit DestroyActor in preview worlds trips
	// typed-element shutdown assertions, so the world owns the teardown.
	CaptureMirrors.Reset();
	CaptureMirrorActor = nullptr;
	CaptureMirrorScene.Reset();
}

void SUbstreamModelerViewport::AppendEditSessionMeshComponents(TArray<UPrimitiveComponent*>& OutComponents) const
{
	UWorld* World = PreviewScene.IsValid() ? PreviewScene->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		TInlineComponentArray<UMeshComponent*> MeshComponents(*ActorIt);
		for (UMeshComponent* Component : MeshComponents)
		{
			if (!IsValid(Component) || !Component->IsRegistered())
			{
				continue;
			}

			// Native TRS gizmos can render handle meshes in the same preview
			// world as the model. They are interaction chrome, not user
			// geometry, and must never be captured or copied as editable
			// sources. Tool working meshes are intentionally still included.
			if (IsEditorGizmoComponent(Component))
			{
				continue;
			}

			// Tool visualization renders through line/point/triangle set
			// components. The stream only sees real geometry, including the
			// tools' temporary working meshes.
			if (Component->IsA<ULineSetComponent>()
				|| Component->IsA<UPointSetComponent>()
				|| Component->IsA<UTriangleSetComponent>())
			{
				continue;
			}

			OutComponents.Add(Component);
		}
	}
}

bool SUbstreamModelerViewport::IsCaptureCameraSettling(double SettleSeconds) const
{
	if (!ViewportClient.IsValid())
	{
		return false;
	}

	const FVector CameraLocation = ViewportClient->GetViewLocation();
	const FRotator CameraRotation = ViewportClient->GetViewRotation();
	const float CameraFov = ViewportClient->ViewFOV;
	const double CurrentTimeSeconds = FPlatformTime::Seconds();

	if (!bHasLastCaptureCamera)
	{
		LastCaptureCameraLocation = CameraLocation;
		LastCaptureCameraRotation = CameraRotation;
		LastCaptureCameraFov = CameraFov;
		LastCaptureCameraChangeTimeSeconds = CurrentTimeSeconds;
		bHasLastCaptureCamera = true;
		return false;
	}

	const bool bCameraChanged =
		!CameraLocation.Equals(LastCaptureCameraLocation, 0.01)
		|| !CameraRotation.Equals(LastCaptureCameraRotation, 0.01f)
		|| !FMath::IsNearlyEqual(CameraFov, LastCaptureCameraFov, 0.01f);

	if (bCameraChanged)
	{
		LastCaptureCameraLocation = CameraLocation;
		LastCaptureCameraRotation = CameraRotation;
		LastCaptureCameraFov = CameraFov;
		LastCaptureCameraChangeTimeSeconds = CurrentTimeSeconds;
		return true;
	}

	return LastCaptureCameraChangeTimeSeconds >= 0.0
		&& CurrentTimeSeconds - LastCaptureCameraChangeTimeSeconds < SettleSeconds;
}

FReply SUbstreamModelerViewport::OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	if (!ResolveDraggedExternalMeshFiles(DragDropEvent).IsEmpty())
	{
		return FReply::Handled();
	}

	if (ResolveDraggedStaticMesh(DragDropEvent) || !ResolveDraggedActors(DragDropEvent).IsEmpty())
	{
		SetAcceptedDropFeedback(DragDropEvent);
		return FReply::Handled();
	}

	if (!ResolveDraggedMeshComponents(DragDropEvent).IsEmpty())
	{
		SetAcceptedDropFeedback(DragDropEvent);
		return FReply::Handled();
	}

	return SEditorViewport::OnDragOver(MyGeometry, DragDropEvent);
}

FReply SUbstreamModelerViewport::OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	const TArray<FString> ExternalMeshFiles = ResolveDraggedExternalMeshFiles(DragDropEvent);
	if (!ExternalMeshFiles.IsEmpty())
	{
		if (!ConfirmReplaceSubject())
		{
			return FReply::Handled();
		}
		return ImportExternalMeshFiles(ExternalMeshFiles) ? FReply::Handled() : FReply::Unhandled();
	}

	if (UStaticMesh* StaticMesh = ResolveDraggedStaticMesh(DragDropEvent))
	{
		if (!ConfirmReplaceSubject())
		{
			return FReply::Handled();
		}
		return SetStaticMesh(StaticMesh) ? FReply::Handled() : FReply::Unhandled();
	}

	TArray<TWeakObjectPtr<AActor>> DraggedActors = ResolveDraggedActors(DragDropEvent);
	if (!DraggedActors.IsEmpty())
	{
		if (!ConfirmReplaceSubject())
		{
			return FReply::Handled();
		}
		return SetActors(DraggedActors) ? FReply::Handled() : FReply::Unhandled();
	}

	TArray<UMeshComponent*> DraggedComponents = ResolveDraggedMeshComponents(DragDropEvent);
	if (!DraggedComponents.IsEmpty())
	{
		TArray<TWeakObjectPtr<AActor>> DraggedOwners;
		TSet<AActor*> AddedActors;
		for (const UMeshComponent* Component : DraggedComponents)
		{
			AActor* Owner = Component ? Component->GetOwner() : nullptr;
			if (IsValid(Owner) && !AddedActors.Contains(Owner))
			{
				DraggedOwners.Add(Owner);
				AddedActors.Add(Owner);
			}
		}

		if (!ConfirmReplaceSubject())
		{
			return FReply::Handled();
		}
		return SetMeshComponents(
			DraggedComponents,
			DraggedComponents.Num() == 1 ? DraggedComponents[0]->GetName() : FString::Printf(TEXT("%d dragged components"), DraggedComponents.Num()),
			TEXT("dragged_mesh_components"),
			DraggedOwners)
			? FReply::Handled()
			: FReply::Unhandled();
	}

	return SEditorViewport::OnDrop(MyGeometry, DragDropEvent);
}

TSharedRef<FEditorViewportClient> SUbstreamModelerViewport::MakeEditorViewportClient()
{
	ViewportClient = MakeShared<FUbstreamViewportClient>(StageModeManager.Get(), GetPreviewScene(), SharedThis(this));
	return ViewportClient.ToSharedRef();
}

UStaticMesh* SUbstreamModelerViewport::ResolveDraggedStaticMesh(const FDragDropEvent& DragDropEvent) const
{
	const TSharedPtr<FDragDropOperation> Operation = DragDropEvent.GetOperation();
	if (!Operation.IsValid() || !Operation->IsOfType<FAssetDragDropOp>())
	{
		return nullptr;
	}

	const TSharedPtr<FAssetDragDropOp> AssetDragDropOp = StaticCastSharedPtr<FAssetDragDropOp>(Operation);
	for (const FAssetData& AssetData : AssetDragDropOp->GetAssets())
	{
		if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(AssetData.GetAsset()))
		{
			return StaticMesh;
		}
	}

	return nullptr;
}

TArray<TWeakObjectPtr<AActor>> SUbstreamModelerViewport::ResolveDraggedActors(const FDragDropEvent& DragDropEvent) const
{
	TArray<TWeakObjectPtr<AActor>> Actors;
	const TSharedPtr<FDragDropOperation> Operation = DragDropEvent.GetOperation();
	if (!Operation.IsValid())
	{
		return Actors;
	}

	if (Operation->IsOfType<FActorDragDropOp>())
	{
		AppendActorDragDropPayload(static_cast<const FActorDragDropOp&>(*Operation), Actors);
		return Actors;
	}

	if (Operation->IsOfType<FCompositeDragDropOp>())
	{
		const TSharedPtr<FCompositeDragDropOp> CompositeOp = StaticCastSharedPtr<FCompositeDragDropOp>(Operation);
		if (const TSharedPtr<const FActorDragDropOp> ActorOp = CompositeOp->GetSubOp<FActorDragDropOp>())
		{
			AppendActorDragDropPayload(*ActorOp, Actors);
		}
	}

	return Actors;
}

TArray<FString> SUbstreamModelerViewport::ResolveDraggedExternalMeshFiles(const FDragDropEvent& DragDropEvent) const
{
	TArray<FString> MeshFiles;
	const TSharedPtr<FDragDropOperation> Operation = DragDropEvent.GetOperation();
	if (!Operation.IsValid() || !Operation->IsOfType<FExternalDragOperation>())
	{
		return MeshFiles;
	}

	const TSharedPtr<FExternalDragOperation> ExternalOp = StaticCastSharedPtr<FExternalDragOperation>(Operation);
	if (!ExternalOp->HasFiles())
	{
		return MeshFiles;
	}

	static const TArray<FString> SupportedExtensions = { TEXT("obj"), TEXT("fbx"), TEXT("gltf"), TEXT("glb"), TEXT("stl"), TEXT("ply") };
	for (const FString& File : ExternalOp->GetFiles())
	{
		const FString Extension = FPaths::GetExtension(File).ToLower();
		if (SupportedExtensions.Contains(Extension))
		{
			MeshFiles.Add(File);
		}
	}

	return MeshFiles;
}

bool SUbstreamModelerViewport::ImportExternalMeshFiles(const TArray<FString>& Files)
{
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	const TArray<UObject*> ImportedObjects = AssetToolsModule.Get().ImportAssets(Files, TEXT("/Game/Ubstream/Meshes"));

	for (UObject* ImportedObject : ImportedObjects)
	{
		if (UStaticMesh* ImportedMesh = Cast<UStaticMesh>(ImportedObject))
		{
			return SetStaticMesh(ImportedMesh);
		}
	}

	return false;
}

TArray<UMeshComponent*> SUbstreamModelerViewport::ResolveDraggedMeshComponents(const FDragDropEvent& DragDropEvent) const
{
	TArray<UMeshComponent*> Components;
	const TSharedPtr<FDragDropOperation> Operation = DragDropEvent.GetOperation();
	if (!Operation.IsValid() || !Operation->IsOfType<FCompositeDragDropOp>() || !GEditor)
	{
		return Components;
	}

	TSet<UMeshComponent*> AddedComponents;
	for (FSelectionIterator It(GEditor->GetSelectedComponentIterator()); It; ++It)
	{
		UMeshComponent* MeshComponent = Cast<UMeshComponent>(*It);
		if (UbstreamMeshComponents::HasRenderableGeometry(MeshComponent)
			&& !AddedComponents.Contains(MeshComponent))
		{
			Components.Add(MeshComponent);
			AddedComponents.Add(MeshComponent);
		}
	}

	return Components;
}

void SUbstreamModelerViewport::SetAcceptedDropFeedback(const FDragDropEvent& DragDropEvent) const
{
	const TSharedPtr<FDragDropOperation> Operation = DragDropEvent.GetOperation();
	if (!Operation.IsValid() || !Operation->IsOfType<FDecoratedDragDropOp>())
	{
		return;
	}

	const TSharedPtr<FDecoratedDragDropOp> DecoratedOp = StaticCastSharedPtr<FDecoratedDragDropOp>(Operation);
	DecoratedOp->SetToolTip(
		NSLOCTEXT("SUbstreamModelerViewport", "DropToIsolateInUnboundLoop", "Drop to isolate in Unbound Loop"),
		FAppStyle::GetBrush(TEXT("Graph.ConnectorFeedback.OK")));
}

void SUbstreamModelerViewport::FocusCameraOnComponent(const UMeshComponent& Component)
{
	if (!ViewportClient.IsValid())
	{
		return;
	}

	ViewportClient->FocusOnBounds(GetReliableComponentBounds(Component));
}
