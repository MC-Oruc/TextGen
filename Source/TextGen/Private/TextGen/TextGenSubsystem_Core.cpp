// Copyright <--\, Inc. All Rights Reserved.

#include "TextGen/TextGenSubsystem.h"
#include "TextGen/Providers/KoboldAPI.h"
#include "TextGen/Providers/OpenAIAPI.h"
#include "TextGen/Providers/LlamacppAPI.h"
#include "TextGen/TextGenLocalServiceSubsystem.h"
#include "TextGen/Data/TextGenSettingsSave.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "TimerManager.h"
#include "Logging/LogMacros.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Paths.h"
#include "TextGen/TextGenConversationCache.h"
#include "TextGen/TextGenLog.h"

const ITextGenProvider* UTextGenSubsystem::GetActiveProvider() const
{
    switch (ActiveProvider)
    {
    case ELLMProvider::OpenAI: return OpenAIProvider.Get();
    case ELLMProvider::Llamacpp: return LlamacppProvider.Get();
    case ELLMProvider::KoboldCpp:
    default: return KoboldProvider.Get();
    }
}

FTextGenConversationCommitHandle UTextGenSubsystem::CreateConversationCommitHandle()
{
    FTextGenConversationCommitHandle Handle;
    if (ActiveProvider != ELLMProvider::Llamacpp || CurrentConversationContext.ProgressCacheKey.IsEmpty())
    {
        return Handle;
    }

    const UTextGenLocalServiceSubsystem* Service = GEngine
        ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
    const FString ModelId = Service ? Service->GetActiveModelId() : FString();
    if (ModelId.IsEmpty())
    {
        return Handle;
    }

    InvalidateConversationCommits();

    const FString RuntimeTag = FTextGenConversationCachePaths::ResolveRuntimeTag(LlamacppConfig);
    const FString ModelName = FTextGenConversationCachePaths::ResolveModelName(LlamacppConfig);
    const FString ProgressFilename = FTextGenConversationCachePaths::BuildProgressFilename(
        CurrentConversationContext.ProgressCacheKey, RuntimeTag, ModelName, LlamacppConfig.ContextSize,
        LlamacppConfig.CacheTypeK, LlamacppConfig.CacheTypeV);
    const FString ProgressDirectory = FTextGenConversationCachePaths::GetProgressDirectory();
    IFileManager::Get().MakeDirectory(*ProgressDirectory, true);

    Handle.Value = FGuid::NewGuid();
    FPendingConversationCommit Pending;
    Pending.ProgressCacheKey = CurrentConversationContext.ProgressCacheKey;
    Pending.ModelId = ModelId;
    Pending.SaveFilename = ProgressFilename.LeftChop(4) + TEXT(".next.bin");
    Pending.FinalPath = FPaths::Combine(ProgressDirectory, ProgressFilename);
    Pending.BackupPath = Pending.FinalPath + TEXT(".bak");
    Pending.Revision = ConversationCommitRevision;
    PendingConversationCommits.Add(Handle.Value, MoveTemp(Pending));
    return Handle;
}

void UTextGenSubsystem::InvalidateConversationCommits()
{
    ++ConversationCommitRevision;
    TArray<FHttpRequestPtr> Requests;
    Requests.Reserve(PendingConversationCommits.Num());
    for (TPair<FGuid, FPendingConversationCommit>& Entry : PendingConversationCommits)
    {
        if (Entry.Value.Request.IsValid())
        {
            Requests.Add(MoveTemp(Entry.Value.Request));
        }
    }
    PendingConversationCommits.Reset();
    for (const FHttpRequestPtr& Request : Requests)
    {
        Request->CancelRequest();
    }
}

UTextGenSubsystem::UTextGenSubsystem()
{
    bIsGenerating = false;
    bIsAPIAvailable = false;
    LastErrorMessage = TEXT("");
    LastProcessedLength = 0;
    KoboldProvider = MakeUnique<FKoboldCppProvider>();
    OpenAIProvider = MakeUnique<FOpenAIProvider>();
    LlamacppProvider = MakeUnique<FLlamacppProvider>();
    ActiveGenerationSettings = FTextGenGenerationSettings();
}

void UTextGenSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    EnsureSettingsLoaded();
    EnsureProfilesLoaded();
    if (UTextGenLocalServiceSubsystem* Service = GEngine
        ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr)
    {
        Service->OnStateChanged.AddDynamic(this, &UTextGenSubsystem::HandleLocalServiceStateChanged);
    }
#if !WITH_EDITOR
    ReconcileManagedService(true);
#endif
    TEXTGEN_DEBUG_LOG(Log, TEXT("TextGen subsystem initialized. Provider: %d"), static_cast<int32>(ActiveProvider));
}

void UTextGenSubsystem::Deinitialize()
{
    SetInteractiveSessionActive(this, false);
    CancelConversationPreparation(TEXT("TextGen subsystem deinitialized."));
    InvalidateConversationCommits();
    if (UTextGenLocalServiceSubsystem* Service = GEngine
        ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr)
    {
        Service->OnStateChanged.RemoveDynamic(this, &UTextGenSubsystem::HandleLocalServiceStateChanged);
    }
    if (CurrentGenerationRequest.IsValid())
    {
        CurrentGenerationRequest->CancelRequest();
        CurrentGenerationRequest.Reset();
    }
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(StreamProcessingTimer);
    }
    bIsGenerating = false;
    bIsAPIAvailable = false;
    LastErrorMessage.Empty();
    StreamBuffer.Empty();
    UnprocessedStreamBuffer.Empty();
    LastProcessedLength = 0;
    LastStreamActivityTimeSeconds = 0.0;
    PendingStreamFailureReason.Empty();
    CurrentGenerationSettings = FTextGenGenerationSettings();
    ActiveGenerationSettings = FTextGenGenerationSettings();
    SettingsSave = nullptr;
    TEXTGEN_DEBUG_LOG(Log, TEXT("TextGen Subsystem deinitialized"));
    KoboldProvider.Reset();
    OpenAIProvider.Reset();
    LlamacppProvider.Reset();
    Super::Deinitialize();
}

bool UTextGenSubsystem::GenerateTextStreamInternal(const FTextGenGenerationParams& GenerationParams)
{
    if (ActiveProvider == ELLMProvider::Llamacpp)
    {
        const UTextGenLocalServiceSubsystem* Service = GEngine ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
        if (!Service || Service->GetState() != ETextGenLocalServiceState::Ready)
        {
            LastErrorMessage = TEXT("Managed llama.cpp is not ready. Start it before sending a request.");
            return false;
        }
    }
    if (!bIsAPIAvailable)
    {
        UE_LOG(LogTextGenAPI, Warning, TEXT("API not marked available; triggering health check and proceeding with request"));
        CheckAPIHealthInternal();
    }
    if (bIsGenerating)
    {
        LastErrorMessage = TEXT("Generation already in progress. Call AbortGeneration() first.");
        UE_LOG(LogTextGenAPI, Warning, TEXT("%s"), *LastErrorMessage);
        return false;
    }
    if (GenerationParams.Prompt.IsEmpty())
    {
        LastErrorMessage = TEXT("Prompt cannot be empty");
        UE_LOG(LogTextGenAPI, Warning, TEXT("%s"), *LastErrorMessage);
        return false;
    }
    // Make a mutable copy so we can apply usage config (disable certain params)
    FTextGenGenerationParams EffectiveParams = GenerationParams;
    FTextGenGenerationSettings EffectiveSettings = ActiveGenerationSettings;
    // Apply ParamUsageConfig rules
    if (ParamUsageConfig.SystemPromptMode == ESystemPromptMode::EmbedInFirstUser && !EffectiveParams.SystemPrompt.IsEmpty())
    {
        // Embed system prompt at start of user prompt (simple prefix with two newlines for separation)
        EffectiveParams.Prompt = EffectiveParams.SystemPrompt + TEXT("\n\n") + EffectiveParams.Prompt;
        EffectiveParams.SystemPrompt.Empty();
    }
    else if (ParamUsageConfig.SystemPromptMode == ESystemPromptMode::Ignore)
    {
        // Completely ignore system prompt
        EffectiveParams.SystemPrompt.Empty();
    }
    // For SendAsSystem, do nothing (send as system message normally)
    if (!ParamUsageConfig.bUseConversationMessages)
    {
        EffectiveParams.ConversationMessages.Reset();
    }
    ApplyUsageConfigToSettings(EffectiveSettings);
    CurrentConversationContext = EffectiveParams.ConversationContext;
    CurrentGenerationParams = EffectiveParams;
    CurrentGenerationSettings = EffectiveSettings;
    if (ActiveProvider == ELLMProvider::Llamacpp
        && EffectiveParams.ConversationContext.HasCacheRequest())
    {
        const UTextGenLocalServiceSubsystem* Service = GEngine
            ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
        const FString ActiveModelId = Service ? Service->GetActiveModelId() : FString();
        if (!PreparedCacheKey.Equals(
                EffectiveParams.ConversationContext.GetRequestedCacheKey(), ESearchCase::CaseSensitive)
            || ActiveModelId.IsEmpty()
            || !PreparedModelId.Equals(ActiveModelId, ESearchCase::CaseSensitive)
            || PreparedSlotPreference != EffectiveParams.ConversationContext.SlotPreference)
        {
            LastErrorMessage = TEXT("Conversation cache preparation must complete before generation.");
            return false;
        }
    }
    return StartPreparedGenerationRequest(EffectiveParams, EffectiveSettings);
}

bool UTextGenSubsystem::StartPreparedGenerationRequest(const FTextGenGenerationParams& Params, const FTextGenGenerationSettings& Settings)
{
    constexpr float StreamRequestHardTimeoutSeconds = 3600.0f;
    FString JsonString;
    FString Endpoint;
    FString Verb;
    const ITextGenProvider* Provider = GetActiveProvider();
    Provider->BuildGenerateRequestData(this, Params, Settings, Endpoint, Verb, JsonString);
    TSharedRef<IHttpRequest> Request = CreateRequest(Endpoint, Verb, StreamRequestHardTimeoutSeconds);
    Request->SetContentAsString(JsonString);
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->OnProcessRequestComplete().BindUObject(this, &UTextGenSubsystem::OnGenerationStreamResponse);
    bIsGenerating = true;
    StreamBuffer.Empty();
    LastProcessedLength = 0;
    UnprocessedStreamBuffer.Empty();
    CurrentGenerationRequest = Request;
    // Stream stop-sequence handling must match what we actually send to the provider (EffectiveSettings).
    // Otherwise disabling stop sequences via ParamUsageConfig may still truncate output locally.
    CurrentGenerationSettings = Settings;
    CurrentConversationContext = Params.ConversationContext;
    CurrentGenerationParams = Params;
    const double NowSeconds = FPlatformTime::Seconds();
    LastStreamActivityTimeSeconds = NowSeconds;
    PendingStreamFailureReason.Empty();

    bool bRequestStarted = Request->ProcessRequest();
    if (bRequestStarted)
    {
        TEXTGEN_DEBUG_LOG(Log, TEXT("Text generation started"));
        if (UWorld* World = GetWorld())
        {
            World->GetTimerManager().SetTimer(StreamProcessingTimer, this, &UTextGenSubsystem::ProcessStreamChunks, 0.05f, true);
        }
    }
    else
    {
        bIsGenerating = false;
        CurrentGenerationRequest.Reset();
        StreamBuffer.Empty();
        UnprocessedStreamBuffer.Empty();
        LastProcessedLength = 0;
        LastStreamActivityTimeSeconds = 0.0;
        PendingStreamFailureReason.Empty();
        LastErrorMessage = TEXT("Failed to start HTTP request - Network error");
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *LastErrorMessage);
    }
    return bRequestStarted;
}

void UTextGenSubsystem::PrepareConversation(
    const FTextGenConversationContext& ConversationContext,
    FTextGenConversationPreparationCallback&& Callback)
{
    CancelConversationPreparation(TEXT("Conversation preparation superseded."));
    InvalidateConversationCommits();
    PreparedCacheKey.Reset();
    PreparedModelId.Reset();
    PreparedSlotPreference = ETextGenConversationSlotPreference::None;

    if (ActiveProvider != ELLMProvider::Llamacpp || !ConversationContext.HasCacheRequest())
    {
        Callback(true, FString());
        return;
    }

    PendingPreparationContext = ConversationContext;
    PendingPreparationCallback = MoveTemp(Callback);
    ReconcileManagedService(true);

    UTextGenLocalServiceSubsystem* Service = GEngine
        ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
    if (!Service)
    {
        CompleteConversationPreparation(
            ConversationPreparationRevision, false, TEXT("Managed llama.cpp service is unavailable."));
        return;
    }

    switch (Service->GetState())
    {
    case ETextGenLocalServiceState::Ready:
        BeginPendingConversationRestore(ConversationPreparationRevision);
        break;
    case ETextGenLocalServiceState::Failed:
    case ETextGenLocalServiceState::Disabled:
    case ETextGenLocalServiceState::Unhealthy:
        CompleteConversationPreparation(
            ConversationPreparationRevision, false, Service->GetLastError());
        break;
    default:
        break;
    }
}

void UTextGenSubsystem::HandleLocalServiceStateChanged(const ETextGenLocalServiceState NewState)
{
    if (ActiveProvider == ELLMProvider::Llamacpp)
    {
        bIsAPIAvailable = false;
        if (NewState == ETextGenLocalServiceState::Ready)
        {
            CheckAPIHealthInternal();
        }
    }

    if (NewState != ETextGenLocalServiceState::Ready)
    {
        InvalidateConversationCommits();
        PreparedCacheKey.Reset();
        PreparedModelId.Reset();
        PreparedSlotPreference = ETextGenConversationSlotPreference::None;
    }
    if (!PendingPreparationCallback)
    {
        return;
    }

    if (NewState == ETextGenLocalServiceState::Ready)
    {
        BeginPendingConversationRestore(ConversationPreparationRevision);
    }
    else if (NewState == ETextGenLocalServiceState::Failed
        || NewState == ETextGenLocalServiceState::Disabled
        || NewState == ETextGenLocalServiceState::Unhealthy)
    {
        const UTextGenLocalServiceSubsystem* Service = GEngine
            ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
        CompleteConversationPreparation(
            ConversationPreparationRevision, false,
            Service ? Service->GetLastError() : TEXT("Managed llama.cpp service failed."));
    }
}

void UTextGenSubsystem::BeginPendingConversationRestore(const uint64 Revision)
{
    if (Revision != ConversationPreparationRevision || !PendingPreparationCallback
        || ConversationPreparationRequest.IsValid())
    {
        return;
    }

    const UTextGenLocalServiceSubsystem* Service = GEngine
        ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
    const FString ModelId = Service ? Service->GetActiveModelId() : FString();
    if (ModelId.IsEmpty())
    {
        CompleteConversationPreparation(Revision, false, TEXT("Managed llama.cpp model is not ready."));
        return;
    }

    const FString RuntimeTag = FTextGenConversationCachePaths::ResolveRuntimeTag(LlamacppConfig);
    const FString ModelName = FTextGenConversationCachePaths::ResolveModelName(LlamacppConfig);
    FString CheckpointFilename;
	FString RestoreKind = TEXT("Default");
	FString RestoreCacheKey = PendingPreparationContext.DefaultCacheKey;
    const auto PrepareDefaultWorkingCheckpoint = [&]()
    {
        const FString DefaultFilename = FTextGenConversationCachePaths::BuildDefaultFilename(
            PendingPreparationContext.DefaultCacheKey, RuntimeTag, ModelName, LlamacppConfig.ContextSize,
            LlamacppConfig.CacheTypeK, LlamacppConfig.CacheTypeV);
        const FString DefaultPath = FPaths::Combine(
            FTextGenConversationCachePaths::GetDefaultDirectory(), DefaultFilename);
        if (!FPaths::FileExists(DefaultPath))
        {
            return false;
        }

        CheckpointFilename = TEXT("DefaultRestore.work");
        const FString WorkingPath = FPaths::Combine(
            FTextGenConversationCachePaths::GetProgressDirectory(), CheckpointFilename);
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(WorkingPath), true);
        return IFileManager::Get().Copy(*WorkingPath, *DefaultPath, true, true) == COPY_OK;
    };
    if (PendingPreparationContext.SlotPreference == ETextGenConversationSlotPreference::Progress)
    {
		RestoreKind = TEXT("Progress");
		RestoreCacheKey = PendingPreparationContext.ProgressCacheKey;
        CheckpointFilename = FTextGenConversationCachePaths::BuildProgressFilename(
            PendingPreparationContext.ProgressCacheKey, RuntimeTag, ModelName, LlamacppConfig.ContextSize,
            LlamacppConfig.CacheTypeK, LlamacppConfig.CacheTypeV);
        if (!FPaths::FileExists(FPaths::Combine(
                FTextGenConversationCachePaths::GetProgressDirectory(), CheckpointFilename)))
        {
            if (!PrepareDefaultWorkingCheckpoint())
            {
				UE_LOG(LogTextGenAPI, Warning,
					TEXT("Neither Progress cache '%s' nor Default cache '%s' is available; erasing the conversation slot."),
					*PendingPreparationContext.ProgressCacheKey,
					*PendingPreparationContext.DefaultCacheKey);
                BeginConversationSlotErase(Revision, ModelId);
                return;
            }
			RestoreKind = TEXT("Default");
			RestoreCacheKey = PendingPreparationContext.DefaultCacheKey;
            UE_LOG(LogTextGenAPI, Log,
                TEXT("Progress cache '%s' is unavailable; restoring Default cache '%s' before replaying history."),
                *PendingPreparationContext.ProgressCacheKey,
                *PendingPreparationContext.DefaultCacheKey);
        }
    }
    else if (!PrepareDefaultWorkingCheckpoint())
    {
		UE_LOG(LogTextGenAPI, Warning,
			TEXT("Default conversation cache '%s' is unavailable; erasing the conversation slot."),
			*PendingPreparationContext.DefaultCacheKey);
        BeginConversationSlotErase(Revision, ModelId);
        return;
    }

	UE_LOG(LogTextGenAPI, Log,
		TEXT("Restoring %s conversation cache key='%s' checkpoint='%s'."),
		*RestoreKind, *RestoreCacheKey, *CheckpointFilename);

    const TSharedRef<IHttpRequest> Restore = CreateRequest(TEXT("/slots/0?action=restore"), TEXT("POST"));
    Restore->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Restore->SetContentAsString(FString::Printf(TEXT("{\"filename\":\"%s\",\"model\":\"%s\"}"),
        *CheckpointFilename.ReplaceCharWithEscapedChar(), *ModelId.ReplaceCharWithEscapedChar()));
    ConversationPreparationRequest = Restore;
    Restore->OnProcessRequestComplete().BindWeakLambda(this,
        [this, Revision, ModelId, RestoreKind, RestoreCacheKey, CheckpointFilename](
			FHttpRequestPtr, FHttpResponsePtr Response, const bool bSuccess)
        {
            ConversationPreparationRequest.Reset();
            if (Revision != ConversationPreparationRevision || !PendingPreparationCallback)
            {
                return;
            }
            if (!bSuccess || !Response.IsValid()
                || Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
            {
				UE_LOG(LogTextGenAPI, Warning,
					TEXT("Failed to restore %s conversation cache key='%s' checkpoint='%s' (HTTP %d); erasing the slot."),
					*RestoreKind,
					*RestoreCacheKey,
					*CheckpointFilename,
					Response.IsValid() ? Response->GetResponseCode() : 0);
                BeginConversationSlotErase(Revision, ModelId);
                return;
            }
			UE_LOG(LogTextGenAPI, Log,
				TEXT("Restored %s conversation cache key='%s' checkpoint='%s'."),
				*RestoreKind, *RestoreCacheKey, *CheckpointFilename);
            CompleteConversationPreparation(Revision, true, FString());
        });
    if (!Restore->ProcessRequest())
    {
        ConversationPreparationRequest.Reset();
        BeginConversationSlotErase(Revision, ModelId);
    }
}

void UTextGenSubsystem::BeginConversationSlotErase(const uint64 Revision, const FString& ModelId)
{
    if (Revision != ConversationPreparationRevision || !PendingPreparationCallback
        || ConversationPreparationRequest.IsValid())
    {
        return;
    }

    const TSharedRef<IHttpRequest> Erase = CreateRequest(TEXT("/slots/0?action=erase"), TEXT("POST"));
    Erase->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Erase->SetContentAsString(FString::Printf(
        TEXT("{\"model\":\"%s\"}"), *ModelId.ReplaceCharWithEscapedChar()));
    ConversationPreparationRequest = Erase;
    Erase->OnProcessRequestComplete().BindWeakLambda(this,
        [this, Revision](FHttpRequestPtr, FHttpResponsePtr Response, const bool bSuccess)
        {
            ConversationPreparationRequest.Reset();
            const bool bErased = bSuccess && Response.IsValid()
                && Response->GetResponseCode() >= 200 && Response->GetResponseCode() < 300;
            CompleteConversationPreparation(
                Revision, bErased,
                bErased ? FString() : TEXT("llama.cpp conversation slot could not be reset."));
        });
    if (!Erase->ProcessRequest())
    {
        ConversationPreparationRequest.Reset();
        CompleteConversationPreparation(
            Revision, false, TEXT("llama.cpp conversation slot reset request could not start."));
    }
}

void UTextGenSubsystem::CompleteConversationPreparation(
    const uint64 Revision,
    const bool bSuccess,
    const FString& ErrorMessage)
{
    if (Revision != ConversationPreparationRevision || !PendingPreparationCallback)
    {
        return;
    }

    if (bSuccess)
    {
        const UTextGenLocalServiceSubsystem* Service = GEngine
            ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
        PreparedCacheKey = PendingPreparationContext.GetRequestedCacheKey();
        PreparedModelId = Service ? Service->GetActiveModelId() : FString();
        PreparedSlotPreference = PendingPreparationContext.SlotPreference;
    }
    FTextGenConversationPreparationCallback Callback = MoveTemp(PendingPreparationCallback);
    PendingPreparationContext = FTextGenConversationContext();
    Callback(bSuccess, ErrorMessage);
}

void UTextGenSubsystem::CancelConversationPreparation(const FString& Reason)
{
    ++ConversationPreparationRevision;
    if (ConversationPreparationRequest.IsValid())
    {
        ConversationPreparationRequest->CancelRequest();
        ConversationPreparationRequest.Reset();
    }
    if (PendingPreparationCallback)
    {
        FTextGenConversationPreparationCallback Callback = MoveTemp(PendingPreparationCallback);
        PendingPreparationContext = FTextGenConversationContext();
        Callback(false, Reason);
    }
}

void UTextGenSubsystem::SetGenerationParamUsage(const UObject* WorldContext, const FTextGenParamUsageConfig& Config)
{
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        Subsystem->EnsureProfilesLoaded();
        if (FTextGenProviderProfile* Profile = Subsystem->GetActiveProfileMutable())
        {
            if (Profile->bIsProjectDefault)
            {
                return;
            }
            Subsystem->ParamUsageConfig = Config;
            Profile->ParamUsageConfig = Subsystem->ParamUsageConfig;
            Subsystem->SaveProfiles();
        }
    }
}

FTextGenParamUsageConfig UTextGenSubsystem::GetGenerationParamUsage(const UObject* WorldContext)
{
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        return Subsystem->ParamUsageConfig;
    }
    return FTextGenParamUsageConfig();
}

bool UTextGenSubsystem::AbortGenerationInternal()
{
    if (!bIsGenerating)
    {
        LastErrorMessage = TEXT("No generation in progress");
        UE_LOG(LogTextGenAPI, Warning, TEXT("%s"), *LastErrorMessage);
        return false;
    }
    if (CurrentGenerationRequest.IsValid())
    {
        CurrentGenerationRequest->CancelRequest();
        CurrentGenerationRequest.Reset();
    }
    bIsGenerating = false;
    LastProcessedLength = 0;
    UnprocessedStreamBuffer.Empty();
    StreamBuffer.Empty();
    LastStreamActivityTimeSeconds = 0.0;
    PendingStreamFailureReason.Empty();
    CurrentGenerationSettings = FTextGenGenerationSettings();
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(StreamProcessingTimer);
    }
    bool bHasServerAbort = false;
    FString AbortEndpoint;
    FString AbortVerb;
    FString AbortBody;
    GetActiveProvider()->BuildAbortRequestData(this, bHasServerAbort, AbortEndpoint, AbortVerb, AbortBody);
    if (!bHasServerAbort)
    {
        FTextGenOperationResult Result;
        Result.Result = ETextGenResult::Success;
        Result.ErrorMessage = TEXT("");
        Result.ResponseCode = 200;
        OnAbortComplete.Broadcast(Result);
        TEXTGEN_DEBUG_LOG(Log, TEXT("Abort completed locally (No server abort)"));
        return true;
    }
    TSharedRef<IHttpRequest> AbortRequest = CreateRequest(AbortEndpoint, AbortVerb);
    if (!AbortBody.IsEmpty())
    {
        AbortRequest->SetContentAsString(AbortBody);
        AbortRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    }
    AbortRequest->OnProcessRequestComplete().BindUObject(this, &UTextGenSubsystem::OnAbortResponse);
    bool bRequestStarted = AbortRequest->ProcessRequest();
    if (bRequestStarted)
    {
        TEXTGEN_DEBUG_LOG(Log, TEXT("Sent abort request"));
    }
    else
    {
        LastErrorMessage = TEXT("Failed to send abort request");
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *LastErrorMessage);
    }
    return bRequestStarted;
}

bool UTextGenSubsystem::GetTokenCountInternal(const FString& Text)
{
    if (!bIsAPIAvailable)
    {
        LastErrorMessage = ActiveProvider == ELLMProvider::KoboldCpp
            ? TEXT("API not available. Please check KoboldCpp server.")
            : TEXT("API not available. Please check OpenAI settings and connectivity.");
        UE_LOG(LogTextGenAPI, Warning, TEXT("%s"), *LastErrorMessage);
        return false;
    }
    if (Text.IsEmpty())
    {
        LastErrorMessage = TEXT("Text cannot be empty for token count");
        UE_LOG(LogTextGenAPI, Warning, TEXT("%s"), *LastErrorMessage);
        return false;
    }
    FString Endpoint;
    FString Verb;
    FString Body;
    GetActiveProvider()->BuildTokenCountRequestData(this, Text, Endpoint, Verb, Body);
    TSharedRef<IHttpRequest> Request = CreateRequest(Endpoint, Verb);
    Request->SetContentAsString(Body);
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->OnProcessRequestComplete().BindUObject(this, &UTextGenSubsystem::OnTokenCountResponse);
    bool bRequestStarted = Request->ProcessRequest();
    if (!bRequestStarted)
    {
        LastErrorMessage = TEXT("Failed to start token count request - Network error");
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *LastErrorMessage);
    }
    return bRequestStarted;
}

bool UTextGenSubsystem::GetVersionInternal()
{
    if (!bIsAPIAvailable)
    {
        LastErrorMessage = ActiveProvider == ELLMProvider::KoboldCpp
            ? TEXT("API not available. Please check KoboldCpp server.")
            : TEXT("API not available. Please check OpenAI settings and connectivity.");
        UE_LOG(LogTextGenAPI, Warning, TEXT("%s"), *LastErrorMessage);
        return false;
    }
    FString Endpoint;
    FString Verb;
    GetActiveProvider()->BuildVersionRequestData(this, Endpoint, Verb);
    TSharedRef<IHttpRequest> Request = CreateRequest(Endpoint, Verb);
    Request->OnProcessRequestComplete().BindUObject(this, &UTextGenSubsystem::OnVersionResponse);
    bool bRequestStarted = Request->ProcessRequest();
    if (!bRequestStarted)
    {
        LastErrorMessage = TEXT("Failed to start version request - Network error");
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *LastErrorMessage);
    }
    return bRequestStarted;
}

bool UTextGenSubsystem::GetMaxContextLengthInternal()
{
    if (!bIsAPIAvailable)
    {
        LastErrorMessage = ActiveProvider == ELLMProvider::KoboldCpp
            ? TEXT("API not available. Please check KoboldCpp server.")
            : TEXT("API not available. Please check OpenAI settings and connectivity.");
        UE_LOG(LogTextGenAPI, Warning, TEXT("%s"), *LastErrorMessage);
        return false;
    }
    bool bImmediate = false;
    int32 ImmediateValue = 0;
    FString Endpoint;
    FString Verb;
    GetActiveProvider()->BuildMaxContextRequestData(this, bImmediate, ImmediateValue, Endpoint, Verb);
    if (bImmediate)
    {
        FTextGenOperationResult Result;
        Result.Result = ETextGenResult::Success;
        Result.ErrorMessage = TEXT("");
        Result.ResponseCode = 200;
        FTextGenMaxContextResponse MaxContextResponse;
        MaxContextResponse.MaxContextLength = ImmediateValue;
        OnMaxContextComplete.Broadcast(Result, MaxContextResponse);
        return true;
    }
    TSharedRef<IHttpRequest> Request = CreateRequest(Endpoint, Verb);
    Request->OnProcessRequestComplete().BindUObject(this, &UTextGenSubsystem::OnMaxContextResponse);
    bool bRequestStarted = Request->ProcessRequest();
    if (!bRequestStarted)
    {
        LastErrorMessage = TEXT("Failed to start max context request - Network error");
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *LastErrorMessage);
    }
    return bRequestStarted;
}

bool UTextGenSubsystem::CheckAPIHealthInternal()
{
    if (ActiveProvider == ELLMProvider::Llamacpp)
    {
        const UTextGenLocalServiceSubsystem* Service = GEngine
            ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
        if (!Service || Service->GetState() != ETextGenLocalServiceState::Ready
            || Service->GetEffectiveBaseURL().IsEmpty())
        {
            LastErrorMessage = TEXT("Managed llama.cpp service is not ready.");
            bIsAPIAvailable = false;
            OnHealthCheckComplete.Broadcast(
                CreateErrorResult(ETextGenResult::NetworkError, LastErrorMessage), false);
            return false;
        }
    }

    FString Endpoint;
    FString Verb;
    GetActiveProvider()->BuildHealthRequestData(this, Endpoint, Verb);
    TSharedRef<IHttpRequest> Request = CreateRequest(Endpoint, Verb);
    Request->OnProcessRequestComplete().BindUObject(this, &UTextGenSubsystem::OnHealthCheckResponse);
    bool bRequestStarted = Request->ProcessRequest();
    if (!bRequestStarted)
    {
        LastErrorMessage = TEXT("Failed to start API health check");
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *LastErrorMessage);
        FTextGenOperationResult Result = CreateErrorResult(ETextGenResult::NetworkError, LastErrorMessage);
        OnHealthCheckComplete.Broadcast(Result, false);
    }
    return bRequestStarted;
}

TArray<FTextGenProviderProfileSummary> UTextGenSubsystem::GetProviderProfiles(const UObject* WorldContext)
{
    TArray<FTextGenProviderProfileSummary> Out;
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        Subsystem->EnsureProfilesLoaded();
        if (Subsystem->SettingsSave)
        {
            for (const auto& Pair : Subsystem->SettingsSave->ProfilesById)
            {
                FTextGenProviderProfileSummary Summary;
                Summary.Id = Pair.Value.Id;
                Summary.DisplayName = Pair.Value.DisplayName;
                Summary.Provider = Pair.Value.Provider;
                Summary.bIsProjectDefault = Pair.Value.bIsProjectDefault;
                Out.Add(MoveTemp(Summary));
            }
        }
    }
    return Out;
}

FString UTextGenSubsystem::CreateProviderProfile(const UObject* WorldContext, const FString& DisplayName, ELLMProvider Provider)
{
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        return Subsystem->CreateProfileInternal(DisplayName, Provider);
    }
    return TEXT("");
}

bool UTextGenSubsystem::RenameProviderProfile(const UObject* WorldContext, const FString& ProfileId, const FString& NewDisplayName)
{
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        Subsystem->EnsureProfilesLoaded();
        return Subsystem->RenameProfileInternal(ProfileId, NewDisplayName);
    }
    return false;
}

bool UTextGenSubsystem::DeleteProviderProfile(const UObject* WorldContext, const FString& ProfileId)
{
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        Subsystem->EnsureProfilesLoaded();
        return Subsystem->DeleteProfileInternal(ProfileId);
    }
    return false;
}

bool UTextGenSubsystem::SetActiveProfile(const UObject* WorldContext, const FString& ProfileId)
{
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        return Subsystem->SetActiveProfileInternal(ProfileId);
    }
    return false;
}

FString UTextGenSubsystem::GetActiveProfileId(const UObject* WorldContext)
{
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        return Subsystem->ActiveProfileId;
    }
    return TEXT("");
}

FTextGenOpenAIConfig UTextGenSubsystem::GetOpenAIProfileConfig(const UObject* WorldContext, const FString& ProfileId, bool& bFound)
{
    bFound = false;
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        Subsystem->EnsureProfilesLoaded();
        if (Subsystem->SettingsSave)
        {
            if (const FTextGenProviderProfile* Profile = Subsystem->SettingsSave->ProfilesById.Find(ProfileId))
            {
                if (Profile->Provider == ELLMProvider::OpenAI)
                {
                    bFound = true;
                    return Profile->OpenAI;
                }
            }
        }
    }
    return FTextGenOpenAIConfig();
}

bool UTextGenSubsystem::SetOpenAIProfileConfig(const UObject* WorldContext, const FString& ProfileId, const FTextGenOpenAIConfig& Config)
{
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        Subsystem->EnsureProfilesLoaded();
        if (Subsystem->SettingsSave)
        {
            if (FTextGenProviderProfile* Profile = Subsystem->SettingsSave->ProfilesById.Find(ProfileId))
            {
                if (Profile->Provider != ELLMProvider::OpenAI || Profile->bIsProjectDefault)
                {
                    return false;
                }
                Profile->OpenAI = Config;

                if (Subsystem->ActiveProfileId == ProfileId)
                {
                    Subsystem->OpenAIBaseURL = Config.BaseURL;
                    Subsystem->OpenAIAPIKey = Config.APIKey;
                    Subsystem->OpenAIChatModel = Config.ChatModel;
                    Subsystem->CheckAPIHealthInternal();
                }

                Subsystem->SaveProfiles();
                return true;
            }
        }
    }
    return false;
}

FTextGenKoboldConfig UTextGenSubsystem::GetKoboldProfileConfig(const UObject* WorldContext, const FString& ProfileId, bool& bFound)
{
    bFound = false;
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        Subsystem->EnsureProfilesLoaded();
        if (Subsystem->SettingsSave)
        {
            if (const FTextGenProviderProfile* Profile = Subsystem->SettingsSave->ProfilesById.Find(ProfileId))
            {
                if (Profile->Provider == ELLMProvider::KoboldCpp)
                {
                    bFound = true;
                    return Profile->Kobold;
                }
            }
        }
    }
    return FTextGenKoboldConfig();
}

bool UTextGenSubsystem::SetKoboldProfileConfig(const UObject* WorldContext, const FString& ProfileId, const FTextGenKoboldConfig& Config)
{
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        Subsystem->EnsureProfilesLoaded();
        if (Subsystem->SettingsSave)
        {
            if (FTextGenProviderProfile* Profile = Subsystem->SettingsSave->ProfilesById.Find(ProfileId))
            {
                if (Profile->Provider != ELLMProvider::KoboldCpp || Profile->bIsProjectDefault)
                {
                    return false;
                }
                Profile->Kobold = Config;

                if (Subsystem->ActiveProfileId == ProfileId)
                {
                    Subsystem->BaseURL = Config.BaseURL;
                    Subsystem->RequestTimeout = Config.RequestTimeout;
                    Subsystem->bEnableDebugLogging = Config.bEnableDebugLogging;
                    Subsystem->CheckAPIHealthInternal();
                }

                Subsystem->SaveProfiles();
                return true;
            }
        }
    }
    return false;
}

FTextGenLlamacppConfig UTextGenSubsystem::GetLlamacppProfileConfig(const UObject* WorldContext, const FString& ProfileId, bool& bFound)
{
    bFound = false;
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        Subsystem->EnsureProfilesLoaded();
        if (Subsystem->SettingsSave)
        {
            if (const FTextGenProviderProfile* Profile = Subsystem->SettingsSave->ProfilesById.Find(ProfileId))
            {
                if (Profile->Provider == ELLMProvider::Llamacpp)
                {
                    bFound = true;
                    return Profile->Llamacpp;
                }
            }
        }
    }
    return FTextGenLlamacppConfig();
}

bool UTextGenSubsystem::SetLlamacppProfileConfig(const UObject* WorldContext, const FString& ProfileId, const FTextGenLlamacppConfig& Config)
{
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem)
    {
        return false;
    }
    Subsystem->EnsureProfilesLoaded();
    FTextGenProviderProfile* Profile = Subsystem->SettingsSave ? Subsystem->SettingsSave->ProfilesById.Find(ProfileId) : nullptr;
    if (!Profile || Profile->Provider != ELLMProvider::Llamacpp || Profile->bIsProjectDefault)
    {
        return false;
    }
    Profile->Llamacpp = Config;
    if (Subsystem->ActiveProfileId == ProfileId)
    {
        Subsystem->CancelConversationPreparation(TEXT("Active llama.cpp profile changed."));
        Subsystem->InvalidateConversationCommits();
        Subsystem->PreparedCacheKey.Reset();
        Subsystem->PreparedModelId.Reset();
        Subsystem->PreparedSlotPreference = ETextGenConversationSlotPreference::None;
        Subsystem->LlamacppConfig = Config;
        Subsystem->ReconcileManagedService(false);
    }
    Subsystem->SaveProfiles();
    return true;
}

bool UTextGenSubsystem::CommitConversationTurn(const UObject* WorldContext, const FTextGenConversationCommitHandle& Handle, FString& OutErrorMessage)
{
    OutErrorMessage.Reset();
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem || !Handle.Value.IsValid())
    {
        OutErrorMessage = TEXT("Conversation commit handle is invalid.");
        return false;
    }

    FPendingConversationCommit* Pending = Subsystem->PendingConversationCommits.Find(Handle.Value);
    if (!Pending)
    {
        OutErrorMessage = TEXT("Conversation commit handle is unknown or already consumed.");
        return false;
    }

    const FPendingConversationCommit Commit = *Pending;
    const UTextGenLocalServiceSubsystem* Service = GEngine ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
    const FString ModelId = Service ? Service->GetActiveModelId() : FString();
    if (Commit.Revision != Subsystem->ConversationCommitRevision
        || Subsystem->ActiveProvider != ELLMProvider::Llamacpp
        || !Subsystem->CurrentConversationContext.ProgressCacheKey.Equals(
            Commit.ProgressCacheKey, ESearchCase::CaseSensitive)
        || ModelId.IsEmpty()
        || !ModelId.Equals(Commit.ModelId, ESearchCase::CaseSensitive))
    {
        Subsystem->PendingConversationCommits.Remove(Handle.Value);
        OutErrorMessage = TEXT("Conversation changed before its KV checkpoint could be saved.");
        return false;
    }
    const TSharedRef<IHttpRequest> Request = Subsystem->CreateRequest(TEXT("/slots/0?action=save"), TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(FString::Printf(TEXT("{\"filename\":\"%s\",\"model\":\"%s\"}"),
        *Commit.SaveFilename.ReplaceCharWithEscapedChar(), *Commit.ModelId.ReplaceCharWithEscapedChar()));
    Request->OnProcessRequestComplete().BindWeakLambda(Subsystem,
        [WeakSubsystem = TWeakObjectPtr<UTextGenSubsystem>(Subsystem), Handle, Commit](FHttpRequestPtr, FHttpResponsePtr Response, const bool bSuccess)
        {
            UTextGenSubsystem* Pinned = WeakSubsystem.Get();
            if (!Pinned)
            {
                return;
            }
            const FPendingConversationCommit* ActiveCommit = Pinned->PendingConversationCommits.Find(Handle.Value);
            if (!ActiveCommit || ActiveCommit->Revision != Commit.Revision)
            {
                return;
            }
            Pinned->PendingConversationCommits.Remove(Handle.Value);
            if (!bSuccess || !Response.IsValid() || Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
            {
                UE_LOG(LogTextGenAPI, Warning, TEXT("llama.cpp KV checkpoint save failed for %s."), *Commit.SaveFilename);
                return;
            }

            const UTextGenLocalServiceSubsystem* CurrentService = GEngine
                ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
            if (Pinned->ConversationCommitRevision != Commit.Revision
                || Pinned->ActiveProvider != ELLMProvider::Llamacpp
                || !Pinned->CurrentConversationContext.ProgressCacheKey.Equals(
                    Commit.ProgressCacheKey, ESearchCase::CaseSensitive)
                || !CurrentService
                || !CurrentService->GetActiveModelId().Equals(Commit.ModelId, ESearchCase::CaseSensitive))
            {
                return;
            }

            const FString NextPath = FPaths::Combine(FPaths::GetPath(Commit.FinalPath), Commit.SaveFilename);
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(Commit.FinalPath), true);
            if (FPaths::FileExists(Commit.FinalPath)
                && !IFileManager::Get().Move(*Commit.BackupPath, *Commit.FinalPath, true, true, false, true))
            {
                UE_LOG(LogTextGenAPI, Warning, TEXT("llama.cpp KV checkpoint backup failed: %s"), *Commit.FinalPath);
                return;
            }
            if (!IFileManager::Get().Move(*Commit.FinalPath, *NextPath, true, true, false, true))
            {
                UE_LOG(LogTextGenAPI, Warning, TEXT("llama.cpp KV checkpoint publish failed: %s"), *Commit.FinalPath);
            }
        });
    Pending->Request = Request;
    if (!Request->ProcessRequest())
    {
        Subsystem->PendingConversationCommits.Remove(Handle.Value);
        OutErrorMessage = TEXT("KV checkpoint request could not be started.");
        return false;
    }
    return true;
}

bool UTextGenSubsystem::ArchiveConversationProgress(const UObject* WorldContext, const FString& ProgressCacheKey)
{
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem || ProgressCacheKey.IsEmpty())
    {
        return false;
    }
    Subsystem->EnsureProfilesLoaded();
    Subsystem->InvalidateConversationCommits();
    const FString Filename = FTextGenConversationCachePaths::BuildProgressFilename(
        ProgressCacheKey,
        FTextGenConversationCachePaths::ResolveRuntimeTag(Subsystem->LlamacppConfig),
        FTextGenConversationCachePaths::ResolveModelName(Subsystem->LlamacppConfig),
        Subsystem->LlamacppConfig.ContextSize,
        Subsystem->LlamacppConfig.CacheTypeK,
        Subsystem->LlamacppConfig.CacheTypeV);
    const FString ProgressPath = FPaths::Combine(FTextGenConversationCachePaths::GetProgressDirectory(), Filename);
    if (!FPaths::FileExists(ProgressPath))
    {
        return true;
    }
    return IFileManager::Get().Move(*(ProgressPath + TEXT(".bak")), *ProgressPath, true, true, false, true);
}

void UTextGenSubsystem::AbortConversationTurn(const UObject* WorldContext, const FTextGenConversationCommitHandle& Handle)
{
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        if (FPendingConversationCommit* Pending = Subsystem->PendingConversationCommits.Find(Handle.Value))
        {
            FHttpRequestPtr Request = MoveTemp(Pending->Request);
            Subsystem->PendingConversationCommits.Remove(Handle.Value);
            if (Request.IsValid())
            {
                Request->CancelRequest();
            }
        }
    }
}

ELLMProvider UTextGenSubsystem::GetActiveProviderType(const UObject* WorldContext)
{
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        return Subsystem->ActiveProvider;
    }
    return ELLMProvider::KoboldCpp;
}

void UTextGenSubsystem::SetInteractiveSessionActive(const UObject* WorldContext, const bool bActive)
{
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem || Subsystem->bInteractiveSessionActive == bActive)
    {
        return;
    }

    Subsystem->bInteractiveSessionActive = bActive;
    if (UTextGenLocalServiceSubsystem* Service = GEngine
        ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr)
    {
        const bool bUseInteractivePriority = bActive
            && Subsystem->ActiveProvider == ELLMProvider::Llamacpp
            && !Subsystem->LlamacppConfig.bManagedDisabled;
        Service->SetInteractiveWorkloadActive(bUseInteractivePriority);
    }
}
