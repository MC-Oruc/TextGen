#include "TextGen/TextGenProjectSettings.h"

UTextGenProjectSettings::UTextGenProjectSettings()
{
    DevelopmentDefaults.RuntimeTag = TEXT("v0.3.0");
    PackagedDefaults.RuntimeTag = TEXT("v0.3.0");
    DevelopmentDefaults.ContextSize = 12288;
    PackagedDefaults.ContextSize = 12288;
}
