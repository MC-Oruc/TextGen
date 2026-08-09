#include "TextGen/TextGenConversationCache.h"

#include "Misc/Paths.h"
#include "TextGen/TextGenProjectSettings.h"
#include "TextGen/TextGenTypes.h"

namespace
{
    FString CacheTypeName(const ETextGenLlamacppKVCacheType Type)
    {
        switch (Type)
        {
        case ETextGenLlamacppKVCacheType::F32: return TEXT("F32");
        case ETextGenLlamacppKVCacheType::BF16: return TEXT("BF16");
        case ETextGenLlamacppKVCacheType::Q8_0: return TEXT("Q8_0");
        case ETextGenLlamacppKVCacheType::Q4_0: return TEXT("Q4_0");
        case ETextGenLlamacppKVCacheType::Q4_1: return TEXT("Q4_1");
        case ETextGenLlamacppKVCacheType::IQ4_NL: return TEXT("IQ4_NL");
        case ETextGenLlamacppKVCacheType::Q5_0: return TEXT("Q5_0");
        case ETextGenLlamacppKVCacheType::Q5_1: return TEXT("Q5_1");
        case ETextGenLlamacppKVCacheType::F16:
        default: return TEXT("F16");
        }
    }
}

FString FTextGenConversationCachePaths::SanitizeComponent(const FString& Value)
{
    const FString Trimmed = Value.TrimStartAndEnd();
    FString Result;
    Result.Reserve(Trimmed.Len());
    static const FString Invalid(TEXT("\\/:*?\"<>|[]"));
    for (const TCHAR Character : Trimmed)
    {
        const bool bSeparator = Character == TEXT('_') || Character == TEXT('-')
            || Invalid.Contains(FString::Chr(Character)) || FChar::IsWhitespace(Character);
        if (bSeparator)
        {
            if (!Result.IsEmpty() && Result[Result.Len() - 1] != TEXT('-'))
            {
                Result.AppendChar(TEXT('-'));
            }
            continue;
        }
        Result.AppendChar(Character);
    }
    while (Result.EndsWith(TEXT("-")))
    {
        Result.LeftChopInline(1, EAllowShrinking::No);
    }
    return Result.IsEmpty() ? TEXT("None") : Result;
}

FString FTextGenConversationCachePaths::BuildKey(const FString& CacheKey, const FString& RuntimeTag,
    const FString& ModelName, const int32 ContextSize,
    const ETextGenLlamacppKVCacheType CacheTypeK,
    const ETextGenLlamacppKVCacheType CacheTypeV)
{
    return FString::Printf(TEXT("%s_%s_%s_Ctx%d_K%s_V%s"),
        *SanitizeComponent(CacheKey),
        *SanitizeComponent(RuntimeTag),
        *SanitizeComponent(ModelName),
        ContextSize,
        *SanitizeComponent(CacheTypeName(CacheTypeK)),
        *SanitizeComponent(CacheTypeName(CacheTypeV)));
}

FString FTextGenConversationCachePaths::BuildDefaultFilename(const FString& CacheKey, const FString& RuntimeTag,
    const FString& ModelName, const int32 ContextSize,
    const ETextGenLlamacppKVCacheType CacheTypeK,
    const ETextGenLlamacppKVCacheType CacheTypeV)
{
    return BuildKey(CacheKey, RuntimeTag, ModelName, ContextSize, CacheTypeK, CacheTypeV) + TEXT("_Default.bin");
}

FString FTextGenConversationCachePaths::BuildProgressFilename(const FString& CacheKey, const FString& RuntimeTag,
    const FString& ModelName, const int32 ContextSize,
    const ETextGenLlamacppKVCacheType CacheTypeK,
    const ETextGenLlamacppKVCacheType CacheTypeV)
{
    return BuildKey(CacheKey, RuntimeTag, ModelName, ContextSize, CacheTypeK, CacheTypeV) + TEXT("_Progress.bin");
}

FString FTextGenConversationCachePaths::GetDefaultDirectory()
{
    const FString Configured = GetDefault<UTextGenProjectSettings>()->DefaultCacheDirectory;
    return FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Configured)
        ? FPaths::Combine(FPaths::ProjectDir(), Configured)
        : Configured);
}

FString FTextGenConversationCachePaths::GetProgressDirectory()
{
    return FPaths::ConvertRelativePathToFull(FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("TextGen"), TEXT("ConversationCaches"), TEXT("Slots")));
}

FString FTextGenConversationCachePaths::ResolveRuntimeTag(const FTextGenLlamacppConfig& Config)
{
    return Config.RuntimeTag;
}

FString FTextGenConversationCachePaths::ResolveModelName(const FTextGenLlamacppConfig& Config)
{
    return FPaths::GetBaseFilename(Config.ModelPath);
}
