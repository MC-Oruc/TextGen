#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "TextGen/TextGenTypes.h"
#include "TextGenProjectSettings.generated.h"

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "TextGen"))
class TEXTGEN_API UTextGenProjectSettings final : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UTextGenProjectSettings();

    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

    UPROPERTY(Config, EditAnywhere, Category = "Development")
    FTextGenLlamacppConfig DevelopmentDefaults;

    UPROPERTY(Config, EditAnywhere, Category = "Packaged Game")
    FTextGenLlamacppConfig PackagedDefaults;

    UPROPERTY(Config, EditAnywhere, Category = "Editor")
    ETextGenManagedLifecycleMode EditorLifecycle = ETextGenManagedLifecycleMode::EditorSession;

    UPROPERTY(Config, EditAnywhere, Category = "Conversation Cache")
    FString DefaultCacheDirectory = TEXT("Content/TextGen/ConversationCaches");
};
