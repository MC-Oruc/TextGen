// Copyright <--\, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "TextGen/TextGenEnums.h"
#include "TextGenTypes.generated.h"

//////////////////////////////////////////////////////////////////////////
// Types (Common)

/** Single conversation message entry (shared domain type: used both by generation requests and persistence). */
USTRUCT(BlueprintType)
struct TEXTGEN_API FConversationMessage
{
    GENERATED_BODY()

    /** Role (User = player, Assistant = NPC, Tool = tool response, ToolCall = LLM requesting tool execution). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Conversation")
    EPromptRole Role = EPromptRole::User;

    /** Raw message text */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Conversation")
    FString Message;

    /** Tool call ID (used when Role == Tool to reference which tool_call this responds to) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Conversation")
    FString ToolCallId;

    /** Function/tool name (used when Role == Tool or ToolCall) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Conversation")
    FString Name;
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenConversationContext
{
    GENERATED_BODY()

    /** Stable baseline identity supplied by the conversation owner. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Conversation")
    FString DefaultCacheKey;

    /** Runtime progress identity, including the owner's history partition. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Conversation")
    FString ProgressCacheKey;

    /** Exact cache kind requested by the conversation owner. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Conversation")
    ETextGenConversationSlotPreference SlotPreference = ETextGenConversationSlotPreference::None;

    const FString& GetRequestedCacheKey() const
    {
        return SlotPreference == ETextGenConversationSlotPreference::Default
            ? DefaultCacheKey
            : ProgressCacheKey;
    }

    bool HasCacheRequest() const
    {
        return SlotPreference != ETextGenConversationSlotPreference::None
            && !GetRequestedCacheKey().IsEmpty();
    }
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenConversationCommitHandle
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Conversation")
    FGuid Value;

    bool IsValid() const { return Value.IsValid(); }
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenGenerationParams
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Generation")
    FString Prompt;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Generation", meta = (DisplayName = "SystemPrompt", ToolTip = "System prompt. Provider-specific formatting may apply (e.g., KoboldCpp uses '{{[SYSTEM]}}' prefix)."))
    FString SystemPrompt;

    // Manual conversation history provided directly from Blueprint (chronological oldest->newest, excluding current Prompt).
    // Used directly by providers.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|Generation|Conversation")
    TArray<FConversationMessage> ConversationMessages;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|Generation|Conversation")
    FTextGenConversationContext ConversationContext;


    FTextGenGenerationParams()
    {
        Prompt = TEXT("");
        SystemPrompt = TEXT("");
    }
};

/**
 * Persistent generation settings (single source of truth).
 * Applied automatically at send time, regardless of per-request params.
 */
USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenGenerationSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Generation", meta = (ClampMin = "1", ClampMax = "2048"))
    int32 MaxLength = 256;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Generation", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float Temperature = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Generation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TopP = 0.95f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Generation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MinP = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Generation", meta = (ClampMin = "0", ClampMax = "200"))
    int32 TopK = 64;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Generation", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float RepetitionPenalty = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Generation", meta = (ClampMin = "-2.0", ClampMax = "2.0"))
    float FrequencyPenalty = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Generation", meta = (ClampMin = "-2.0", ClampMax = "2.0"))
    float PresencePenalty = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Generation")
    TArray<FString> StopSequences;

    FTextGenGenerationSettings()
    {
        StopSequences = { TEXT("{{[INPUT]}}"), TEXT("{{[OUTPUT]}}") };
    }
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenOpenAIConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Provider|OpenAI")
    FString BaseURL = TEXT("https://api.openai.com");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Provider|OpenAI")
    FString APIKey;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Provider|OpenAI")
    FString ChatModel = TEXT("gpt-5.6-luna");
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenKoboldConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Provider|KoboldCpp")
    FString BaseURL = TEXT("http://localhost:5001");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Provider|KoboldCpp", meta = (ClampMin = "1.0", ClampMax = "300.0"))
    float RequestTimeout = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Provider|KoboldCpp")
    bool bEnableDebugLogging = false;
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenLlamacppConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp")
    FString RuntimeTag;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp")
    ETextGenLlamacppBackend Backend = ETextGenLlamacppBackend::CUDA13;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp")
    ETextGenLlamacppOffloadMode OffloadMode = ETextGenLlamacppOffloadMode::Auto;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp")
    FString ModelPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp", meta = (ClampMin = "1", ClampMax = "65535"))
    int32 Port = 8780;

    /** Zero selects the current machine's physical core count. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp", meta = (ClampMin = "0"))
    int32 Threads = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp", meta = (ClampMin = "1", ClampMax = "8192"))
    int32 BatchSize = 2048;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp", meta = (ClampMin = "1", ClampMax = "8192"))
    int32 UBatchSize = 512;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp", meta = (ClampMin = "0"))
    int32 ContextSize = 12288;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp", meta = (ClampMin = "1.0", ClampMax = "3600.0"))
    float RequestTimeout = 300.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp", meta = (ClampMin = "1.0", ClampMax = "3600.0"))
    float StartupTimeout = 300.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp")
    bool bMMap = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp")
    bool bFlashAttention = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp")
    bool bContinuousBatching = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp")
    bool bEnableReasoning = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp", meta = (ClampMin = "128", ClampMax = "4096"))
    int32 ReasoningBudgetTokens = 256;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp")
    ETextGenLlamacppKVCacheType CacheTypeK = ETextGenLlamacppKVCacheType::F16;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp")
    ETextGenLlamacppKVCacheType CacheTypeV = ETextGenLlamacppKVCacheType::F16;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|Provider|llama.cpp")
    bool bManagedDisabled = false;
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenLlamacppPlacement
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|llama.cpp|Placement")
    ETextGenLlamacppBackend RequestedBackend = ETextGenLlamacppBackend::CUDA13;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|llama.cpp|Placement")
    ETextGenLlamacppBackend EffectiveBackend = ETextGenLlamacppBackend::CUDA13;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|llama.cpp|Placement")
    ETextGenLlamacppOffloadMode EffectiveOffloadMode = ETextGenLlamacppOffloadMode::CPUOnly;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|llama.cpp|Placement")
    FString DeviceName;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|llama.cpp|Placement")
    int32 DeviceMemoryMiB = 0;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|llama.cpp|Placement")
    int32 OffloadedLayerCount = -1;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|llama.cpp|Placement")
    int32 TotalLayerCount = -1;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|llama.cpp|Placement")
    FString FallbackReason;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|llama.cpp|Placement")
    bool bFallbackApplied = false;

    bool operator==(const FTextGenLlamacppPlacement& Other) const
    {
        return RequestedBackend == Other.RequestedBackend
            && EffectiveBackend == Other.EffectiveBackend
            && EffectiveOffloadMode == Other.EffectiveOffloadMode
            && DeviceName == Other.DeviceName
            && DeviceMemoryMiB == Other.DeviceMemoryMiB
            && OffloadedLayerCount == Other.OffloadedLayerCount
            && TotalLayerCount == Other.TotalLayerCount
            && FallbackReason == Other.FallbackReason
            && bFallbackApplied == Other.bFallbackApplied;
    }

    bool operator!=(const FTextGenLlamacppPlacement& Other) const { return !(*this == Other); }
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenProviderCapabilities
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Capabilities")
    bool bStreamingChat = true;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Capabilities")
    bool bConversationCheckpoint = false;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Capabilities")
    bool bManagedProcess = false;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Capabilities")
    bool bModelRouter = false;
};

/**
 * Configuration for enabling/disabling which generation parameters are actually applied.
 * Disabled params are omitted from provider JSON (or in the case of SystemPrompt, handled based on mode).
 * All default to enabled.
 */
USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenParamUsageConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|Generation|Usage")
    bool bUseMaxLength = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|Generation|Usage")
    bool bUseTemperature = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|Generation|Usage")
    bool bUseTopP = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|Generation|Usage")
    bool bUseMinP = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|Generation|Usage")
    bool bUseTopK = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|Generation|Usage")
    bool bUseRepetitionPenalty = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|Generation|Usage")
    bool bUseFrequencyPenalty = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|Generation|Usage")
    bool bUsePresencePenalty = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|Generation|Usage")
    bool bUseStopSequences = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|Generation|Usage", meta=(ToolTip="How to handle the SystemPrompt: Send as system message, embed in first user message, or ignore entirely."))
    ESystemPromptMode SystemPromptMode = ESystemPromptMode::SendAsSystem;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|Generation|Usage", meta=(ToolTip="If disabled, conversation history entries are ignored."))
    bool bUseConversationMessages = true;
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenProviderProfile
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles")
    FString Id;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles")
    FString DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles")
    ELLMProvider Provider = ELLMProvider::KoboldCpp;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TextGen|LLM|Profiles")
    bool bIsProjectDefault = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles|ProviderSettings")
    FTextGenKoboldConfig Kobold;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles")
    FTextGenOpenAIConfig OpenAI;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles")
    FTextGenLlamacppConfig Llamacpp;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles|GenerationSettings")
    FTextGenGenerationSettings GenerationSettings;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles|GenerationSettings")
    FTextGenParamUsageConfig ParamUsageConfig;
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenProviderProfileSummary
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles")
    FString Id;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles")
    FString DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles")
    ELLMProvider Provider = ELLMProvider::KoboldCpp;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Profiles")
    bool bIsProjectDefault = false;
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenGenerationResponse
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Response")
    FString GeneratedText;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Response")
    bool bIsComplete = false;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Response")
    int32 TokensGenerated = 0;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Response")
    FTextGenConversationCommitHandle CommitHandle;

    FTextGenGenerationResponse()
    {
        GeneratedText = TEXT("");
        bIsComplete = false;
        TokensGenerated = 0;
    }
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenOperationResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Result")
    ETextGenResult Result = ETextGenResult::Unknown;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Result")
    FString ErrorMessage;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Result")
    int32 ResponseCode = 0;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Result")
    FString ResponseData;

    FTextGenOperationResult()
    {
        Result = ETextGenResult::Unknown;
        ErrorMessage = TEXT("");
        ResponseCode = 0;
        ResponseData = TEXT("");
    }
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenTokenCountResponse
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Response")
    int32 TokenCount = 0;

    FTextGenTokenCountResponse()
    {
        TokenCount = 0;
    }
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenVersionResponse
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Response")
    FString Version;

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Response")
    FString BuildInfo;

    FTextGenVersionResponse()
    {
        Version = TEXT("");
        BuildInfo = TEXT("");
    }
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenMaxContextResponse
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "TextGen|Response")
    int32 MaxContextLength = 0;

    FTextGenMaxContextResponse()
    {
        MaxContextLength = 0;
    }
};

//============================================================
// Model Listing Types
//============================================================

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenModelInfo
{
    GENERATED_BODY()

    // Raw identifier (id / result)
    UPROPERTY(BlueprintReadOnly, Category="TextGen|Model")
    FString Id;

    // Human Friendly Name (if supplied by provider, else mirrors Id)
    UPROPERTY(BlueprintReadOnly, Category="TextGen|Model")
    FString Name;

    // Maximum context window tokens (if known; 0 if unknown)
    UPROPERTY(BlueprintReadOnly, Category="TextGen|Model")
    int32 ContextLength;

    // Maximum completion tokens (if known; 0 if unknown)
    UPROPERTY(BlueprintReadOnly, Category="TextGen|Model")
    int32 MaxCompletionTokens;

    // Model-supported parameter names (if provided by endpoint)
    UPROPERTY(BlueprintReadOnly, Category="TextGen|Model")
    TArray<FString> SupportedParameters;

    // Pricing entries (key -> value as string). Empty for free providers (KoboldCpp -> all 0 entries prefilled)
    UPROPERTY(BlueprintReadOnly, Category="TextGen|Model")
    TMap<FString, FString> Pricing;

    // True if valid pricing information (per 1M tokens) was parsed from provider metadata
    UPROPERTY(BlueprintReadOnly, Category="TextGen|Model")
    bool bHasPricing = false;

    // Prompt cost per 1M tokens ($)
    UPROPERTY(BlueprintReadOnly, Category="TextGen|Model")
    double PromptCostPerMToken = 0.0;

    // Completion cost per 1M tokens ($)
    UPROPERTY(BlueprintReadOnly, Category="TextGen|Model")
    double CompletionCostPerMToken = 0.0;

    FTextGenModelInfo()
    {
        Id = TEXT("");
        Name = TEXT("");
        ContextLength = 0;
        MaxCompletionTokens = 0;
        bHasPricing = false;
        PromptCostPerMToken = 0.0;
        CompletionCostPerMToken = 0.0;
    }
};

USTRUCT(BlueprintType)
struct TEXTGEN_API FTextGenModelsResponse
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="TextGen|Models")
    TArray<FTextGenModelInfo> Models;

    // Human-readable multi-line summary for logging / debug (per 1M tokens pricing already scaled)
    UPROPERTY(BlueprintReadOnly, Category="TextGen|Models")
    FString DebugSummary;

    // Per-model formatted debug lines (one entry per model for easier iteration / logging)
    UPROPERTY(BlueprintReadOnly, Category="TextGen|Models")
    TArray<FString> DebugLines;
};

//////////////////////////////////////////////////////////////////////////
// Delegates (Common)

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTextGenGenerationComplete, const FTextGenOperationResult&, Result, const FTextGenGenerationResponse&, Response);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTextGenGenerationChunk, const FString&, ChunkText, bool, bIsComplete);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTextGenGenerationReasoningChunk, const FString&, ChunkText, bool, bIsComplete);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTextGenTokenCount, const FTextGenOperationResult&, Result, const FTextGenTokenCountResponse&, Response);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTextGenVersion, const FTextGenOperationResult&, Result, const FTextGenVersionResponse&, Response);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTextGenMaxContext, const FTextGenOperationResult&, Result, const FTextGenMaxContextResponse&, Response);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTextGenAbortComplete, const FTextGenOperationResult&, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTextGenHealthCheck, const FTextGenOperationResult&, Result, bool, bIsAPIHealthy);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTextGenModels, const FTextGenOperationResult&, Result, const FTextGenModelsResponse&, Response);
