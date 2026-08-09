// Copyright <--\, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "TextGen/TextGenTypes.h"
#include "TextGenSettingsSave.generated.h"

/** SaveGame container for profile-centric TextGen settings */
UCLASS()
class TEXTGEN_API UTextGenSettingsSaveGame : public USaveGame
{
    GENERATED_BODY()
public:
    /** Active provider (persisted across sessions) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles")
    ELLMProvider ActiveProvider = ELLMProvider::Llamacpp;

    /** Active profile id (persisted across sessions) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles")
    FString ActiveProfileId;

    /** ProfileId -> profile data */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles")
    TMap<FString, FTextGenProviderProfile> ProfilesById;

    UPROPERTY()
    int32 ProfileSchemaVersion = 0;

};
