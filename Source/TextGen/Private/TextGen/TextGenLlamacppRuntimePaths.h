#pragma once

#include "CoreMinimal.h"
#include "TextGen/TextGenEnums.h"

namespace TextGenLlamacppRuntimePaths
{
    FString GetBackendDirectoryName(ETextGenLlamacppBackend Backend);
    FString GetWritableRoot();
    FString GetWritableRuntimeDirectory(const FString& Tag, ETextGenLlamacppBackend Backend);
    FString GetShippedRuntimeDirectory(const FString& Tag, ETextGenLlamacppBackend Backend);
    bool HasRuntimeInventory(const FString& Directory, ETextGenLlamacppBackend Backend);
    bool ResolveRuntimeDirectory(const FString& Tag, ETextGenLlamacppBackend Backend, FString& OutDirectory);
}
