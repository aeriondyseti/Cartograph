#include "Audio/FGAudioSettings.h"

#include "Templates/SubclassOf.h"

TObjectPtr<UAkSwitchValue> UFGAudioSettings::GetSurfaceMaterialTypeSwitch(TSoftObjectPtr<UPhysicalMaterial> physMaterial) const{ return nullptr; }
TObjectPtr<UAkSwitchValue> UFGAudioSettings::GetDefaultWaterMaterialSwitch() const{ return nullptr; }
TObjectPtr<UAkSwitchValue> UFGAudioSettings::GetFluidTypeSwitch(TSubclassOf<class UFGItemDescriptor> fluidType) const{ return nullptr; }
TObjectPtr<UAkSwitchValue> UFGAudioSettings::GetMovementStateSwitch(bool isSprinting) const{ return nullptr; }
