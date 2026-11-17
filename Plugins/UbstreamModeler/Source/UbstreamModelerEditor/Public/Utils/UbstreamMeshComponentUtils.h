#pragma once

#include "CoreMinimal.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"

namespace UbstreamMeshComponents
{
	/**
	 * A mesh component the isolated stage can clone and render: a Static Mesh
	 * component with an assigned asset, or a Modeling Mode Dynamic Mesh
	 * component with at least one triangle.
	 */
	inline bool HasRenderableGeometry(const UMeshComponent* Component)
	{
		if (!IsValid(Component))
		{
			return false;
		}

		if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
		{
			return StaticMeshComponent->GetStaticMesh() != nullptr;
		}

		if (const UDynamicMeshComponent* DynamicMeshComponent = Cast<UDynamicMeshComponent>(Component))
		{
			const UE::Geometry::FDynamicMesh3* Mesh = DynamicMeshComponent->GetMesh();
			return Mesh && Mesh->TriangleCount() > 0;
		}

		return false;
	}

	inline bool ActorHasRenderableMeshComponent(const AActor* Actor)
	{
		if (!IsValid(Actor))
		{
			return false;
		}

		TArray<UMeshComponent*> MeshComponents;
		Actor->GetComponents<UMeshComponent>(MeshComponents);
		return MeshComponents.ContainsByPredicate([](const UMeshComponent* Component)
		{
			return HasRenderableGeometry(Component);
		});
	}
}
