#include "Bridge/UbstreamModelingModeBridge.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "DynamicMeshActor.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/Actor.h"
#include "ModelingToolsEditorMode.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "Selection.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "UbstreamModelingModeBridge"

DEFINE_LOG_CATEGORY_STATIC(LogUbstreamModelingModeBridge, Log, All);

const FName FUbstreamModelingModeBridge::SandboxTag(TEXT("UbstreamSandbox"));
const FName FUbstreamModelingModeBridge::DoNotCommitTag(TEXT("UbstreamDoNotCommit"));

namespace
{
	FString VectorToJsonLabel(const FVector& Vector)
	{
		return FString::Printf(TEXT("[%.3f, %.3f, %.3f]"), Vector.X, Vector.Y, Vector.Z);
	}

	TArray<TSharedPtr<FJsonValue>> BridgeVectorToJsonArray(const FVector& Vector)
	{
		return {
			MakeShared<FJsonValueNumber>(Vector.X),
			MakeShared<FJsonValueNumber>(Vector.Y),
			MakeShared<FJsonValueNumber>(Vector.Z)
		};
	}

	TArray<TSharedPtr<FJsonValue>> BridgeRotatorToJsonArray(const FRotator& Rotator)
	{
		// Protocol-wide convention: rotation arrays are [pitch, yaw, roll],
		// matching the frame-header camera serialization.
		return {
			MakeShared<FJsonValueNumber>(Rotator.Pitch),
			MakeShared<FJsonValueNumber>(Rotator.Yaw),
			MakeShared<FJsonValueNumber>(Rotator.Roll)
		};
	}

	UStaticMeshComponent* FindStaticMeshComponent(AActor* Actor, UActorComponent* PreferredComponent = nullptr)
	{
		if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(PreferredComponent))
		{
			return StaticMeshComponent;
		}

		return Actor ? Actor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
	}

	UDynamicMeshComponent* FindDynamicMeshComponent(AActor* Actor, UActorComponent* PreferredComponent = nullptr)
	{
		if (UDynamicMeshComponent* DynamicMeshComponent = Cast<UDynamicMeshComponent>(PreferredComponent))
		{
			return DynamicMeshComponent;
		}

		return Actor ? Actor->FindComponentByClass<UDynamicMeshComponent>() : nullptr;
	}

	FString DetermineObjectType(AActor* Actor, UActorComponent* PreferredComponent = nullptr)
	{
		if (Cast<ADynamicMeshActor>(Actor) || FindDynamicMeshComponent(Actor, PreferredComponent))
		{
			return TEXT("DynamicMeshActor");
		}

		if (Cast<AStaticMeshActor>(Actor) || FindStaticMeshComponent(Actor, PreferredComponent))
		{
			return TEXT("StaticMeshActor");
		}

		return TEXT("Actor");
	}

	FString FindAssetPath(AActor* Actor, UActorComponent* PreferredComponent = nullptr)
	{
		if (UStaticMeshComponent* StaticMeshComponent = FindStaticMeshComponent(Actor, PreferredComponent))
		{
			if (const UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh())
			{
				return StaticMesh->GetPathName();
			}
		}

		return FString();
	}

	FUbstreamSelectedModelingObject BuildSelectionObject(AActor* Actor, UActorComponent* PreferredComponent = nullptr)
	{
		FUbstreamSelectedModelingObject Object;
		if (!Actor)
		{
			return Object;
		}

		Object.ActorName = Actor->GetActorLabel();
		Object.ActorPath = Actor->GetPathName();
		Object.ClassName = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();
		Object.Type = DetermineObjectType(Actor, PreferredComponent);
		Object.AssetPath = FindAssetPath(Actor, PreferredComponent);
		Object.Transform = Actor->GetActorTransform();

		for (const FName& Tag : Actor->Tags)
		{
			Object.Tags.Add(Tag.ToString());
		}

		Actor->GetActorBounds(false, Object.BoundsOrigin, Object.BoundsExtent);
		return Object;
	}

	TSharedRef<FJsonObject> BuildTransformJson(const FTransform& Transform)
	{
		TSharedRef<FJsonObject> TransformJson = MakeShared<FJsonObject>();
		TransformJson->SetArrayField(TEXT("location"), BridgeVectorToJsonArray(Transform.GetLocation()));
		TransformJson->SetArrayField(TEXT("rotation"), BridgeRotatorToJsonArray(Transform.GetRotation().Rotator()));
		TransformJson->SetArrayField(TEXT("scale"), BridgeVectorToJsonArray(Transform.GetScale3D()));
		return TransformJson;
	}

	TSharedRef<FJsonObject> BuildBoundsJson(const FUbstreamSelectedModelingObject& Object)
	{
		TSharedRef<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
		BoundsJson->SetArrayField(TEXT("origin"), BridgeVectorToJsonArray(Object.BoundsOrigin));
		BoundsJson->SetArrayField(TEXT("extent"), BridgeVectorToJsonArray(Object.BoundsExtent));
		return BoundsJson;
	}
}

bool FUbstreamModelingModeBridge::IsModelingModeAvailable()
{
	return GEditor != nullptr && FModuleManager::Get().ModuleExists(TEXT("ModelingToolsEditorMode"));
}

bool FUbstreamModelingModeBridge::IsModelingModeActive()
{
	return IsModelingModeAvailable()
		&& GLevelEditorModeTools().IsModeActive(UModelingToolsEditorMode::EM_ModelingToolsEditorModeId);
}

bool FUbstreamModelingModeBridge::ActivateModelingMode()
{
	if (!IsModelingModeAvailable())
	{
		return false;
	}

	FEditorModeTools& ModeTools = GLevelEditorModeTools();
	if (!ModeTools.HasToolkitHost())
	{
		UE_LOG(LogUbstreamModelingModeBridge, Warning, TEXT("Skipping Modeling Mode activation because the Level Editor toolkit host is not ready."));
		return false;
	}

	FModuleManager::Get().LoadModule(FName(TEXT("ModelingToolsEditorMode")));

	ModeTools.ActivateMode(UModelingToolsEditorMode::EM_ModelingToolsEditorModeId);
	return ModeTools.IsModeActive(UModelingToolsEditorMode::EM_ModelingToolsEditorModeId);
}

TArray<FUbstreamSelectedModelingObject> FUbstreamModelingModeBridge::GetSelectedModelingActors()
{
	TArray<FUbstreamSelectedModelingObject> Objects;
	TSet<AActor*> AddedActors;

	if (!GEditor)
	{
		return Objects;
	}

	for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
	{
		AActor* Actor = Cast<AActor>(*It);
		if (!IsValid(Actor) || AddedActors.Contains(Actor))
		{
			continue;
		}

		Objects.Add(BuildSelectionObject(Actor));
		AddedActors.Add(Actor);
	}

	for (FSelectionIterator It(GEditor->GetSelectedComponentIterator()); It; ++It)
	{
		UActorComponent* Component = Cast<UActorComponent>(*It);
		AActor* Owner = Component ? Component->GetOwner() : nullptr;
		if (!IsValid(Owner) || AddedActors.Contains(Owner))
		{
			continue;
		}

		Objects.Add(BuildSelectionObject(Owner, Component));
		AddedActors.Add(Owner);
	}

	return Objects;
}

int32 FUbstreamModelingModeBridge::TagSelectedModelingActors()
{
	TSet<AActor*> TaggedActors;

	if (!GEditor)
	{
		return 0;
	}

	const FScopedTransaction Transaction(LOCTEXT("TagSelectedModelingActors", "Mark Ubstream Sandbox Selection"));

	auto TagActor = [&TaggedActors](AActor* Actor)
	{
		if (!IsValid(Actor) || TaggedActors.Contains(Actor))
		{
			return;
		}

		Actor->Modify();
		Actor->Tags.AddUnique(FUbstreamModelingModeBridge::SandboxTag);
		Actor->Tags.AddUnique(FUbstreamModelingModeBridge::DoNotCommitTag);
		TaggedActors.Add(Actor);
	};

	for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
	{
		TagActor(Cast<AActor>(*It));
	}

	for (FSelectionIterator It(GEditor->GetSelectedComponentIterator()); It; ++It)
	{
		if (const UActorComponent* Component = Cast<UActorComponent>(*It))
		{
			TagActor(Component->GetOwner());
		}
	}

	return TaggedActors.Num();
}

FString FUbstreamModelingModeBridge::BuildSelectedModelingStateJson(const FString& SessionId)
{
	const TArray<FUbstreamSelectedModelingObject> Objects = GetSelectedModelingActors();

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("type"), TEXT("modeling_selection_state"));
	Root->SetStringField(TEXT("protocol"), TEXT("ubstream.websocket.v1"));
	Root->SetStringField(TEXT("sessionId"), SessionId);

	TArray<TSharedPtr<FJsonValue>> ObjectValues;
	for (const FUbstreamSelectedModelingObject& Object : Objects)
	{
		TSharedRef<FJsonObject> ObjectJson = MakeShared<FJsonObject>();
		ObjectJson->SetStringField(TEXT("actorName"), Object.ActorName);
		ObjectJson->SetStringField(TEXT("actorPath"), Object.ActorPath);
		ObjectJson->SetStringField(TEXT("className"), Object.ClassName);
		ObjectJson->SetStringField(TEXT("type"), Object.Type);
		ObjectJson->SetStringField(TEXT("assetPath"), Object.AssetPath);
		ObjectJson->SetObjectField(TEXT("bounds"), BuildBoundsJson(Object));
		ObjectJson->SetObjectField(TEXT("transform"), BuildTransformJson(Object.Transform));

		TArray<TSharedPtr<FJsonValue>> TagValues;
		for (const FString& Tag : Object.Tags)
		{
			TagValues.Add(MakeShared<FJsonValueString>(Tag));
		}
		ObjectJson->SetArrayField(TEXT("tags"), TagValues);

		ObjectValues.Add(MakeShared<FJsonValueObject>(ObjectJson));
	}

	Root->SetArrayField(TEXT("objects"), ObjectValues);

	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root, Writer);
	return Output;
}

FString FUbstreamModelingModeBridge::BuildSelectionSummary(const TArray<FUbstreamSelectedModelingObject>& Objects)
{
	if (Objects.IsEmpty())
	{
		return TEXT("No selected modeling actors");
	}

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("%d selected modeling actor(s):"), Objects.Num()));

	for (const FUbstreamSelectedModelingObject& Object : Objects)
	{
		const FString AssetSuffix = Object.AssetPath.IsEmpty()
			? FString()
			: FString::Printf(TEXT(" asset=%s"), *Object.AssetPath);
		Lines.Add(FString::Printf(
			TEXT("- %s [%s]%s bounds origin=%s extent=%s"),
			*Object.ActorName,
			*Object.Type,
			*AssetSuffix,
			*VectorToJsonLabel(Object.BoundsOrigin),
			*VectorToJsonLabel(Object.BoundsExtent)));
	}

	return FString::Join(Lines, TEXT("\n"));
}

#undef LOCTEXT_NAMESPACE
