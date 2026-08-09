#include "TextGen/TextGenProjectSettings.h"

UTextGenProjectSettings::UTextGenProjectSettings()
{
    DevelopmentDefaults.RuntimeTag = TEXT("b10333");
    PackagedDefaults.RuntimeTag = TEXT("b10333");
    DevelopmentDefaults.ContextSize = 12288;
    PackagedDefaults.ContextSize = 12288;
}
