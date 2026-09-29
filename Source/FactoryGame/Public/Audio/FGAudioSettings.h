// Copyright Coffee Stain Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AkSwitchValue.h"
#include "Engine/DeveloperSettings.h"
#include "FGAudioSettings.generated.h"

/**
 * 
 */
UCLASS( config = Game, defaultconfig, meta = ( DisplayName = "Factory Audio Settings" ) )
class FACTORYGAME_API UFGAudioSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Helper to directly get the resource setting without going through UFGGlobalSettings */
	static const UFGAudioSettings* Get() { return GetDefault<UFGAudioSettings>(); }
	
	TObjectPtr<UAkSwitchValue> GetSurfaceMaterialTypeSwitch( TSoftObjectPtr<UPhysicalMaterial> physMaterial ) const;
	TObjectPtr<UAkSwitchValue> GetDefaultWaterMaterialSwitch() const;
	TObjectPtr<UAkSwitchValue> GetFluidTypeSwitch( TSubclassOf< class UFGItemDescriptor > fluidType ) const;
	TObjectPtr<UAkSwitchValue> GetMovementStateSwitch( bool isSprinting ) const;
	
protected:
	//Footstep Material type to AK switch value mapping
	UPROPERTY( Config, EditDefaultsOnly, Category = "Audio | Footstep materials" )
	TMap< TSoftObjectPtr< UPhysicalMaterial >, TSoftObjectPtr<UAkSwitchValue> > mMaterialTypeToSurfaceAkSwitch;

	UPROPERTY( Config, EditDefaultsOnly, Category = "Audio | Footstep materials" )
	TSoftObjectPtr<UAkSwitchValue> mDefaultSurfaceMaterialTypeSwitch = nullptr;

	UPROPERTY( Config, EditDefaultsOnly, Category = "Audio | Footstep materials" )
	TSoftObjectPtr<UAkSwitchValue> mDefaultWaterMaterialTypeSwitch = nullptr;

	//Movement
	UPROPERTY( Config, EditDefaultsOnly, Category = "Audio | Movement" )
	TSoftObjectPtr<UAkSwitchValue> mMovementStateSprinting = nullptr;

	UPROPERTY( Config, EditDefaultsOnly, Category = "Audio | Movement" )
	TSoftObjectPtr<UAkSwitchValue> mMovementStateWalking = nullptr;

	//Fluid to AK switch mapping
	UPROPERTY( Config, EditDefaultsOnly, Category = "Audio | Fluid Types" )
	TMap< TSubclassOf<UFGItemDescriptor >, TSoftObjectPtr<UAkSwitchValue> > mFluidTypeToAkSwitch;
	
	UPROPERTY( Config, EditDefaultsOnly, Category = "Audio | Fluid Types" )
	TSoftObjectPtr<UAkSwitchValue> mNoFluidTypeSwitch = nullptr;
};
