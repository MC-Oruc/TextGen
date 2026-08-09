// Copyright <--\, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Http.h"
#include "Engine/DataTable.h"
#include "TimerManager.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "TextGen/TextGenProvider.h"
// Moved type & delegate definitions to a separate header to keep this file smaller.
#include "TextGen/TextGenTypes.h"
#include "TextGenSubsystem.generated.h"

//////////////////////////////////////////////////////////////////////////
// Subsystem (Common)

class ITextGenProvider;
class UTextGenSettingsSaveGame;

using FTextGenConversationPreparationCallback = TFunction<void(bool, const FString&)>;

UCLASS(BlueprintType)
class TEXTGEN_API UTextGenSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    UTextGenSubsystem();

    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // Events
    UPROPERTY(BlueprintAssignable, Category = "TextGen|LLM|Events")
    FOnTextGenGenerationComplete OnGenerationComplete;

    UPROPERTY(BlueprintAssignable, Category = "TextGen|LLM|Events")
    FOnTextGenGenerationChunk OnGenerationChunk;

    UPROPERTY(BlueprintAssignable, Category = "TextGen|LLM|Events")
    FOnTextGenGenerationReasoningChunk OnGenerationReasoningChunk;

    UPROPERTY(BlueprintAssignable, Category = "TextGen|LLM|Events")
    FOnTextGenTokenCount OnTokenCountComplete;

    UPROPERTY(BlueprintAssignable, Category = "TextGen|LLM|Events")
    FOnTextGenVersion OnVersionComplete;

    UPROPERTY(BlueprintAssignable, Category = "TextGen|LLM|Events")
    FOnTextGenMaxContext OnMaxContextComplete;

    UPROPERTY(BlueprintAssignable, Category = "TextGen|LLM|Events")
    FOnTextGenAbortComplete OnAbortComplete;

    UPROPERTY(BlueprintAssignable, Category = "TextGen|LLM|Events")
    FOnTextGenHealthCheck OnHealthCheckComplete;

    // Models list event
    UPROPERTY(BlueprintAssignable, Category = "TextGen|LLM|Events")
    FOnTextGenModels OnModelsListed;

    // Config
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Settings|Local")
    FString BaseURL = TEXT("http://localhost:5001");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Settings|Local", meta = (ClampMin = "1.0", ClampMax = "300.0"))
    float RequestTimeout = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Settings|Local")
    bool bEnableDebugLogging = false;

    // Provider selection + OpenAI config
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Provider")
    ELLMProvider ActiveProvider = ELLMProvider::KoboldCpp;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Provider|OpenAI")
    FString OpenAIBaseURL = TEXT("https://api.openai.com");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Provider|OpenAI")
    FString OpenAIAPIKey;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Provider|OpenAI")
    FString OpenAIChatModel = TEXT("gpt-5.6-luna");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Provider|OpenAI", meta = (ClampMin = "1", ClampMax = "1000000"))
    int32 OpenAIContextWindow = 128000;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TextGen|LLM|Provider|Llamacpp")
    FTextGenLlamacppConfig LlamacppConfig;

    // API
    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|API", meta = (CallInEditor = "true", WorldContext = "WorldContext"))
    static bool GenerateTextStream(const UObject* WorldContext, const FTextGenGenerationParams& GenerationParams, FString& OutErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|API", meta = (CallInEditor = "true", WorldContext = "WorldContext"))
    static bool AbortGeneration(const UObject* WorldContext, FString& OutErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|API", meta = (CallInEditor = "true", WorldContext = "WorldContext"))
    static bool GetTokenCount(const UObject* WorldContext, const FString& Text, FString& OutErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|API", meta = (CallInEditor = "true", WorldContext = "WorldContext"))
    static bool GetVersion(const UObject* WorldContext, FString& OutErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|API", meta = (CallInEditor = "true", WorldContext = "WorldContext"))
    static bool GetMaxContextLength(const UObject* WorldContext, FString& OutErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|API", meta = (CallInEditor = "true", WorldContext = "WorldContext"))
    static bool CheckAPIHealth(const UObject* WorldContext, FString& OutErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|API", meta = (CallInEditor = "true", WorldContext = "WorldContext"))
    static bool ListModels(const UObject* WorldContext, ELLMProvider Provider, FString& OutErrorMessage);

    // Formatting helpers
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="TextGen|LLM|Models")
    static FString BuildModelsDebugSummary(const TArray<FTextGenModelInfo>& Models);

    UFUNCTION(BlueprintCallable, BlueprintPure, Category="TextGen|LLM|Models")
    static TArray<FString> BuildModelsDebugLines(const TArray<FTextGenModelInfo>& Models);

    // URL Normalization and Endpoint Combination Utility
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="TextGen|LLM|Utilities")
    static FString CombineBaseAndEndpoint(const FString& InBaseURL, const FString& InEndpoint);

    // Parameter usage configuration (which generation params are applied)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TextGen|LLM|Settings|Params")
    FTextGenParamUsageConfig ParamUsageConfig;

    UFUNCTION(BlueprintCallable, Category="TextGen|LLM|Settings|Params", meta=(WorldContext="WorldContext"))
    static void SetGenerationParamUsage(const UObject* WorldContext, const FTextGenParamUsageConfig& Config);

    UFUNCTION(BlueprintCallable, BlueprintPure, Category="TextGen|LLM|Settings|Params", meta=(WorldContext="WorldContext"))
    static FTextGenParamUsageConfig GetGenerationParamUsage(const UObject* WorldContext);

    // Generation settings (single source of truth)
    UFUNCTION(BlueprintCallable, Category="TextGen|LLM|Settings|Generation", meta=(WorldContext="WorldContext"))
    static void SetGenerationSettings(const UObject* WorldContext, const FTextGenGenerationSettings& Settings);

    UFUNCTION(BlueprintCallable, BlueprintPure, Category="TextGen|LLM|Settings|Generation", meta=(WorldContext="WorldContext"))
    static FTextGenGenerationSettings GetGenerationSettings(const UObject* WorldContext);

    // Provider profiles (UI)
    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Profiles", meta = (WorldContext = "WorldContext"))
    static TArray<FTextGenProviderProfileSummary> GetProviderProfiles(const UObject* WorldContext);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Profiles", meta = (WorldContext = "WorldContext"))
    static FString CreateProviderProfile(const UObject* WorldContext, const FString& DisplayName, ELLMProvider Provider);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Profiles", meta = (WorldContext = "WorldContext"))
    static bool RenameProviderProfile(const UObject* WorldContext, const FString& ProfileId, const FString& NewDisplayName);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Profiles", meta = (WorldContext = "WorldContext"))
    static bool DeleteProviderProfile(const UObject* WorldContext, const FString& ProfileId);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Profiles", meta = (WorldContext = "WorldContext"))
    static bool SetActiveProfile(const UObject* WorldContext, const FString& ProfileId);

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "TextGen|LLM|Profiles", meta = (WorldContext = "WorldContext"))
    static FString GetActiveProfileId(const UObject* WorldContext);

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "TextGen|LLM|Profiles", meta = (WorldContext = "WorldContext"))
    static FTextGenOpenAIConfig GetOpenAIProfileConfig(const UObject* WorldContext, const FString& ProfileId, bool& bFound);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Profiles", meta = (WorldContext = "WorldContext"))
    static bool SetOpenAIProfileConfig(const UObject* WorldContext, const FString& ProfileId, const FTextGenOpenAIConfig& Config);

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "TextGen|LLM|Profiles", meta = (WorldContext = "WorldContext"))
    static FTextGenKoboldConfig GetKoboldProfileConfig(const UObject* WorldContext, const FString& ProfileId, bool& bFound);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Profiles", meta = (WorldContext = "WorldContext"))
    static bool SetKoboldProfileConfig(const UObject* WorldContext, const FString& ProfileId, const FTextGenKoboldConfig& Config);

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "TextGen|LLM|Profiles", meta = (WorldContext = "WorldContext"))
    static FTextGenLlamacppConfig GetLlamacppProfileConfig(const UObject* WorldContext, const FString& ProfileId, bool& bFound);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Profiles", meta = (WorldContext = "WorldContext"))
    static bool SetLlamacppProfileConfig(const UObject* WorldContext, const FString& ProfileId, const FTextGenLlamacppConfig& Config);

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "TextGen|LLM|Provider", meta = (WorldContext = "WorldContext"))
    static ELLMProvider GetActiveProviderType(const UObject* WorldContext);

    /** Raise managed llama.cpp responsiveness while an interactive conversation UI is active. */
    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Conversation", meta = (WorldContext = "WorldContext"))
    static void SetInteractiveSessionActive(const UObject* WorldContext, bool bActive);

    void PrepareConversation(
        const FTextGenConversationContext& ConversationContext,
        FTextGenConversationPreparationCallback&& Callback);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Conversation", meta = (WorldContext = "WorldContext"))
    static bool CommitConversationTurn(const UObject* WorldContext, const FTextGenConversationCommitHandle& Handle, FString& OutErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Conversation", meta = (WorldContext = "WorldContext"))
    static void AbortConversationTurn(const UObject* WorldContext, const FTextGenConversationCommitHandle& Handle);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Conversation", meta = (WorldContext = "WorldContext"))
    static bool ArchiveConversationProgress(const UObject* WorldContext, const FString& ProgressCacheKey);

    // Status
    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Status", BlueprintPure, meta = (CallInEditor = "true", WorldContext = "WorldContext"))
    static bool IsGenerating(const UObject* WorldContext);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Status", BlueprintPure, meta = (CallInEditor = "true", WorldContext = "WorldContext"))
    static bool IsAPIAvailable(const UObject* WorldContext);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Status", BlueprintPure, meta = (CallInEditor = "true", WorldContext = "WorldContext"))
    static FString GetLastErrorMessage(const UObject* WorldContext);

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM|Status", BlueprintPure, meta = (CallInEditor = "true", WorldContext = "WorldContext", BlueprintInternalUseOnly = "true"))
    static UTextGenSubsystem* GetTextGenSubsystem(const UObject* WorldContext);

private:
    // Internal instance methods
    bool GenerateTextStreamInternal(const FTextGenGenerationParams& GenerationParams);
    bool AbortGenerationInternal();
    bool GetTokenCountInternal(const FString& Text);
    bool GetVersionInternal();
    bool GetMaxContextLengthInternal();
    bool CheckAPIHealthInternal();
    bool ListModelsInternal(ELLMProvider Provider);
    TSharedRef<IHttpRequest> CreateRequestForProvider(ELLMProvider Provider, const FString& Endpoint, const FString& Verb = TEXT("GET"), float TimeoutOverrideSeconds = -1.0f);

    // State
    bool bIsGenerating = false;
    bool bIsAPIAvailable = false;
    bool bInteractiveSessionActive = false;
    FString LastErrorMessage;
    TSharedPtr<IHttpRequest> CurrentGenerationRequest;
    FString StreamBuffer;
    FTimerHandle StreamProcessingTimer;
    int32 LastProcessedLength = 0;
    FString UnprocessedStreamBuffer;
    FTextGenGenerationSettings CurrentGenerationSettings;
    double LastStreamActivityTimeSeconds = 0.0;
    FString PendingStreamFailureReason;
    FTextGenConversationContext CurrentConversationContext;
    FTextGenGenerationParams CurrentGenerationParams;
    FTextGenConversationContext PendingPreparationContext;
    FTextGenConversationPreparationCallback PendingPreparationCallback;
    FHttpRequestPtr ConversationPreparationRequest;
    FString PreparedCacheKey;
    FString PreparedModelId;
    ETextGenConversationSlotPreference PreparedSlotPreference = ETextGenConversationSlotPreference::None;
    uint64 ConversationPreparationRevision = 0;

    struct FPendingConversationCommit
    {
        FString ProgressCacheKey;
        FString ModelId;
        FString SaveFilename;
        FString FinalPath;
        FString BackupPath;
        FHttpRequestPtr Request;
        uint64 Revision = 0;
    };
    TMap<FGuid, FPendingConversationCommit> PendingConversationCommits;
    uint64 ConversationCommitRevision = 0;

    // HTTP helpers
    TSharedRef<IHttpRequest> CreateRequest(const FString& Endpoint, const FString& Verb = TEXT("GET"), float TimeoutOverrideSeconds = -1.0f);
    void ProcessStreamData(const FString& NewData);
    FTextGenOperationResult ParseJSONResponse(const FString& ResponseString, int32 ResponseCode);
    FTextGenOperationResult CreateErrorResult(ETextGenResult ErrorType, const FString& ErrorMessage, int32 ResponseCode = 0);

    // Handlers
    void OnGenerationStreamResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
    void OnAbortResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
    void OnTokenCountResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
    void OnVersionResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
    void OnMaxContextResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
    void OnHealthCheckResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
    void OnListModelsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

    // Provider accessor
    const ITextGenProvider* GetActiveProvider() const;
    FTextGenConversationCommitHandle CreateConversationCommitHandle();
    void InvalidateConversationCommits();
    bool StartPreparedGenerationRequest(const FTextGenGenerationParams& Params, const FTextGenGenerationSettings& Settings);

    // Stream processing
    UFUNCTION()
    void ProcessStreamChunks();

    UFUNCTION()
    void HandleLocalServiceStateChanged(ETextGenLocalServiceState NewState);

    void BeginPendingConversationRestore(uint64 Revision);
    void BeginConversationSlotErase(uint64 Revision, const FString& ModelId);
    void CompleteConversationPreparation(uint64 Revision, bool bSuccess, const FString& ErrorMessage);
    void CancelConversationPreparation(const FString& Reason);
    void CancelActiveStreamDueToTimeout(const FString& Reason);
    void ProcessFinalStreamChunk();
    TArray<FString> ExtractJSONObjects(FString& Buffer);
    void ProcessJSONChunk(const FString& JSONString);
    bool CheckForStopSequences(FString& Text, const TArray<FString>& StopSequences);

    // Settings persistence
    void EnsureSettingsLoaded();
    void LoadSettingsForActiveProfile();
    void SaveSettings();
    FTextGenProviderProfile* GetActiveProfileMutable();
    const FTextGenProviderProfile* GetActiveProfile() const;
    FTextGenGenerationSettings GetDefaultGenerationSettings() const;
    FTextGenParamUsageConfig GetDefaultUsageConfig() const;
    void ApplyUsageConfigToSettings(FTextGenGenerationSettings& InOutSettings) const;
    void SetGenerationSettingsInternal(const FTextGenGenerationSettings& Settings);
    void EnsureProfilesLoaded();
    void SaveProfiles();
    bool ApplyProfileById(const FString& ProfileId);
    FString CreateProfileInternal(const FString& DisplayName, ELLMProvider Provider);
    bool RenameProfileInternal(const FString& ProfileId, const FString& NewDisplayName);
    bool DeleteProfileInternal(const FString& ProfileId);
    bool SetActiveProfileInternal(const FString& ProfileId);
    void HandleActiveProfileChanged();
    void ReconcileManagedService(bool bAllowStartWhenStopped);
    FString FindFirstProfileIdForProvider(ELLMProvider Provider) const;

    TUniquePtr<ITextGenProvider> KoboldProvider;
    TUniquePtr<ITextGenProvider> OpenAIProvider;
    TUniquePtr<ITextGenProvider> LlamacppProvider;

    // Settings cache
    UPROPERTY(Transient)
    UTextGenSettingsSaveGame* SettingsSave = nullptr;

    FTextGenGenerationSettings ActiveGenerationSettings;
    FString ActiveProfileId;
};

//////////////////////////////////////////////////////////////////////////
// Async Blueprint Node

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FTextGenAsyncEvent,
    UPARAM(DisplayName = "Chunk Text") FString, ChunkText,
    UPARAM(DisplayName = "Error Text") FString, ErrorText
);


UCLASS()
class TEXTGEN_API UTextGenGenerateTextStreamAsync : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UTextGenGenerateTextStreamAsync();

    UPROPERTY(BlueprintAssignable)
    FTextGenAsyncEvent OnStarted;

    UPROPERTY(BlueprintAssignable)
    FTextGenAsyncEvent OnChunk;

    UPROPERTY(BlueprintAssignable)
    FTextGenAsyncEvent OnReasoning;

    UPROPERTY(BlueprintAssignable)
    FTextGenAsyncEvent OnCompleted;

    UPROPERTY(BlueprintAssignable)
    FTextGenAsyncEvent OnFailed;

    UPROPERTY(BlueprintAssignable)
    FTextGenAsyncEvent OnStreamFailed;

    UFUNCTION(BlueprintCallable, Category = "TextGen|LLM", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
    static UTextGenGenerateTextStreamAsync* GenerateTextStreamAsync(UObject* WorldContextObject, const FTextGenGenerationParams& GenerationParams);

    virtual void Activate() override;

private:
    UPROPERTY()
    UObject* WorldContextObject = nullptr;

    FTextGenGenerationParams GenerationParams;

    UPROPERTY()
    UTextGenSubsystem* APISubsystem = nullptr;

    FString AccumulatedText;
    bool bHasStarted = false;

    UFUNCTION()
    void OnGenerationComplete(const FTextGenOperationResult& Result, const FTextGenGenerationResponse& Response);

    UFUNCTION()
    void OnGenerationChunk(const FString& ChunkText, bool bIsComplete);

    UFUNCTION()
    void OnGenerationReasoningChunk(const FString& ChunkText, bool bIsComplete);

    void CleanupBindings();
};

//============================================================
// Async Blueprint Node: List Models
//============================================================

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FTextGenListModelsAsyncEvent,
    UPARAM(DisplayName="Models") const TArray<FTextGenModelInfo>&, Models,
    UPARAM(DisplayName="Models Debug Lines") const TArray<FString>&, ModelsDebugLines,
    UPARAM(DisplayName="Error Text") FString, ErrorText);

UCLASS()
class TEXTGEN_API UTextGenListModelsAsync : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()
public:
    UTextGenListModelsAsync();

    UPROPERTY(BlueprintAssignable)
    FTextGenListModelsAsyncEvent OnSuccess;

    UPROPERTY(BlueprintAssignable)
    FTextGenListModelsAsyncEvent OnFailure;

    UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"), Category="TextGen|LLM|Models")
    static UTextGenListModelsAsync* ListModelsAsync(UObject* WorldContextObject, ELLMProvider Provider = ELLMProvider::KoboldCpp);

    virtual void Activate() override;

private:
    UPROPERTY()
    UObject* WorldContextObject = nullptr;

    UPROPERTY()
    UTextGenSubsystem* APISubsystem = nullptr;

    UFUNCTION()
    void OnModelsListedInternal(const FTextGenOperationResult& Result, const FTextGenModelsResponse& Response);

    ELLMProvider RequestedProvider = ELLMProvider::KoboldCpp;

    void Cleanup();
};
