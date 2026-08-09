#pragma once

#include "CoreMinimal.h"
#include "TextGen/TextGenEnums.h"

struct FTextGenLlamacppConfig;

struct TEXTGEN_API FTextGenConversationCachePaths
{
    static FString SanitizeComponent(const FString& Value);
    static FString BuildKey(const FString& CacheKey, const FString& RuntimeTag, const FString& ModelName,
        int32 ContextSize, ETextGenLlamacppKVCacheType CacheTypeK,
        ETextGenLlamacppKVCacheType CacheTypeV);
    static FString BuildDefaultFilename(const FString& CacheKey, const FString& RuntimeTag,
        const FString& ModelName, int32 ContextSize, ETextGenLlamacppKVCacheType CacheTypeK,
        ETextGenLlamacppKVCacheType CacheTypeV);
    static FString BuildProgressFilename(const FString& CacheKey, const FString& RuntimeTag,
        const FString& ModelName, int32 ContextSize, ETextGenLlamacppKVCacheType CacheTypeK,
        ETextGenLlamacppKVCacheType CacheTypeV);
    static FString GetDefaultDirectory();
    static FString GetProgressDirectory();
    static FString ResolveRuntimeTag(const FTextGenLlamacppConfig& Config);
    static FString ResolveModelName(const FTextGenLlamacppConfig& Config);
};
