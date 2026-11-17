#include "Settings/UbstreamModelerSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(UbstreamModelerSettings)

UUbstreamModelerSettings::UUbstreamModelerSettings() = default;

void UUbstreamModelerSettings::PostInitProperties()
{
	Super::PostInitProperties();
	MeshGenBaseUrl = UbstreamMeshGenApi::ResolveBaseUrl(MeshGenBaseUrl);
}

FName UUbstreamModelerSettings::GetCategoryName() const
{
	return FName(TEXT("Plugins"));
}

#if WITH_EDITOR
FText UUbstreamModelerSettings::GetSectionText() const
{
	return NSLOCTEXT("UbstreamModelerSettings", "SectionText", "Unbound Loop");
}

FText UUbstreamModelerSettings::GetSectionDescription() const
{
	return NSLOCTEXT("UbstreamModelerSettings", "SectionDescription", "Configure Unbound Loop transport, capture, generation, and streaming defaults.");
}
#endif
