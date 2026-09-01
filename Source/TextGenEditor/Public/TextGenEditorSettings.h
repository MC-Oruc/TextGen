#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "TextGen/TextGenEnums.h"
#include "TextGenEditorSettings.generated.h"

UCLASS(Config = EditorPerProjectUserSettings, meta = (DisplayName = "TextGen"))
class TEXTGENEDITOR_API UTextGenEditorSettings final : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	UPROPERTY(Config, EditAnywhere, Category = "Runtime Lifecycle")
	bool bOverrideEditorLifecycle = false;

	UPROPERTY(Config, EditAnywhere, Category = "Runtime Lifecycle",
		meta = (EditCondition = "bOverrideEditorLifecycle"))
	ETextGenManagedLifecycleMode EditorLifecycle = ETextGenManagedLifecycleMode::Manual;
};
