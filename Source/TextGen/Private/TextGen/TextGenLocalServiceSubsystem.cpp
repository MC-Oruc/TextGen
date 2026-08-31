#include "TextGen/TextGenLocalServiceSubsystem.h"

#include "Engine/Engine.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Internationalization/Regex.h"
#include "ProcessRuntimeSubsystem.h"
#include "ProcessRuntimeTypes.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TextGen/TextGenConversationCache.h"
#include "TextGen/TextGenLog.h"
#include "TextGen/TextGenProjectSettings.h"
#include "TextGenLlamacppRuntimePaths.h"

namespace
{
    const FManagedProcessId ManagedTextGenProcessId(TEXT("TextGen.ManagedLocalServer"));
    constexpr int32 FourGiBClassThresholdMiB = 3840;

    FString BackendDevicePrefix(const ETextGenLlamacppBackend Backend)
    {
        return Backend == ETextGenLlamacppBackend::Vulkan ? TEXT("Vulkan") : TEXT("CUDA");
    }

    TArray<ETextGenLlamacppBackend> BuildBackendCandidates(const ETextGenLlamacppBackend Requested)
    {
        TArray<ETextGenLlamacppBackend> Result{Requested};
        for (const ETextGenLlamacppBackend Candidate : {
            ETextGenLlamacppBackend::CUDA13, ETextGenLlamacppBackend::CUDA12, ETextGenLlamacppBackend::Vulkan})
        {
            Result.AddUnique(Candidate);
        }
        return Result;
    }

    bool ProbeBackendExecutable(const FString& Executable, const ETextGenLlamacppBackend Backend,
        FString& OutDeviceId, FString& OutDeviceName, int32& OutMemoryMiB)
    {
        int32 ReturnCode = -1;
        FString StandardOutput;
        FString StandardError;
        if (!FPlatformProcess::ExecProcess(*Executable, TEXT("--list-devices"), &ReturnCode, &StandardOutput, &StandardError)
            || ReturnCode != 0)
        {
            return false;
        }

        const FString Prefix = BackendDevicePrefix(Backend);
        const FRegexPattern Pattern(TEXT("(?m)^\\s*(CUDA[0-9]+|Vulkan[0-9]+):\\s*(.+?)\\s*\\(([0-9]+) MiB,"));
        FRegexMatcher Matcher(Pattern, StandardOutput + TEXT("\n") + StandardError);
        bool bFound = false;
        bool bBestIsIntegratedIntel = true;
        while (Matcher.FindNext())
        {
            const FString DeviceId = Matcher.GetCaptureGroup(1);
            if (!DeviceId.StartsWith(Prefix, ESearchCase::CaseSensitive))
            {
                continue;
            }
            const FString DeviceName = Matcher.GetCaptureGroup(2);
            const int32 MemoryMiB = FCString::Atoi(*Matcher.GetCaptureGroup(3));
            const bool bIntegratedIntel = DeviceName.Contains(TEXT("Intel"), ESearchCase::IgnoreCase);
            if (!bFound || (bBestIsIntegratedIntel && !bIntegratedIntel)
                || bBestIsIntegratedIntel == bIntegratedIntel && MemoryMiB > OutMemoryMiB)
            {
                OutDeviceId = DeviceId;
                OutDeviceName = DeviceName;
                OutMemoryMiB = MemoryMiB;
                bBestIsIntegratedIntel = bIntegratedIntel;
                bFound = true;
            }
        }
        return bFound;
    }

    FString QuoteArgument(const FString& Value)
    {
        FString Escaped = Value;
        Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
        return FString::Printf(TEXT("\"%s\""), *Escaped);
    }

    FString CacheTypeToArgument(const ETextGenLlamacppKVCacheType Type)
    {
        switch (Type)
        {
        case ETextGenLlamacppKVCacheType::F32: return TEXT("f32");
        case ETextGenLlamacppKVCacheType::BF16: return TEXT("bf16");
        case ETextGenLlamacppKVCacheType::Q8_0: return TEXT("q8_0");
        case ETextGenLlamacppKVCacheType::Q4_0: return TEXT("q4_0");
        case ETextGenLlamacppKVCacheType::Q4_1: return TEXT("q4_1");
        case ETextGenLlamacppKVCacheType::IQ4_NL: return TEXT("iq4_nl");
        case ETextGenLlamacppKVCacheType::Q5_0: return TEXT("q5_0");
        case ETextGenLlamacppKVCacheType::Q5_1: return TEXT("q5_1");
        case ETextGenLlamacppKVCacheType::F16:
        default: return TEXT("f16");
        }
    }

    FString CacheTypeToPresetValue(const ETextGenLlamacppKVCacheType Type)
    {
        return CacheTypeToArgument(Type);
    }

    bool HasEquivalentModelConfig(const FTextGenLlamacppConfig& A, const FTextGenLlamacppConfig& B)
    {
        return A.ModelPath.Equals(B.ModelPath, ESearchCase::IgnoreCase)
            && A.Threads == B.Threads
            && A.BatchSize == B.BatchSize
            && A.UBatchSize == B.UBatchSize
            && A.ContextSize == B.ContextSize
            && A.bMMap == B.bMMap
            && A.bFlashAttention == B.bFlashAttention
            && A.bContinuousBatching == B.bContinuousBatching
            && A.bEnableReasoning == B.bEnableReasoning
            && A.ReasoningBudgetTokens == B.ReasoningBudgetTokens
            && A.CacheTypeK == B.CacheTypeK
            && A.CacheTypeV == B.CacheTypeV
            && A.Backend == B.Backend
            && A.OffloadMode == B.OffloadMode;
    }

#if WITH_EDITOR
    bool IsSuccessfulHttpResponse(const bool bSuccess, const FHttpResponsePtr& Response)
    {
        return bSuccess && Response.IsValid() && EHttpResponseCodes::IsOk(Response->GetResponseCode());
    }

    FString SerializeJsonObject(const TSharedRef<FJsonObject>& Object)
    {
        FString Body;
        FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Body));
        return Body;
    }
#endif
}

void UTextGenLocalServiceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Collection.InitializeDependency<UProcessRuntimeSubsystem>();
    Super::Initialize(Collection);

    if (UProcessRuntimeSubsystem* ProcessRuntime = GEngine ? GEngine->GetEngineSubsystem<UProcessRuntimeSubsystem>() : nullptr)
    {
        ProcessStateHandle = ProcessRuntime->OnProcessStateChanged().AddUObject(this, &UTextGenLocalServiceSubsystem::HandleProcessState);
        ProcessOutputHandle = ProcessRuntime->OnProcessOutput().AddUObject(this, &UTextGenLocalServiceSubsystem::HandleProcessOutput);
    }

}

void UTextGenLocalServiceSubsystem::Deinitialize()
{
#if WITH_EDITOR
    CancelDefaultConversationCacheGeneration();
#endif
    bDesiredRunning = false;
    ++LifecycleGeneration;
    StopReconcileTicker();
    CancelHttpRequest();
    if (UProcessRuntimeSubsystem* ProcessRuntime = GEngine ? GEngine->GetEngineSubsystem<UProcessRuntimeSubsystem>() : nullptr)
    {
        ProcessRuntime->OnProcessStateChanged().Remove(ProcessStateHandle);
        ProcessRuntime->OnProcessOutput().Remove(ProcessOutputHandle);
        if (ProcessRuntime->IsOwnedProcess(ManagedTextGenProcessId))
        {
            FString Ignored;
            ProcessRuntime->StopProcess(ManagedTextGenProcessId, Ignored);
        }
    }
    Super::Deinitialize();
}

bool UTextGenLocalServiceSubsystem::StartManaged(const FTextGenLlamacppConfig& Config, FString& OutError)
{
    check(IsInGameThread());
    OutError.Reset();

#if !PLATFORM_WINDOWS
    OutError = TEXT("Managed CUDA/Vulkan llama.cpp is supported only on Win64 x64 builds.");
    SetState(ETextGenLocalServiceState::Failed, OutError);
    return false;
#else
    if (Config.bManagedDisabled)
    {
        FString IgnoredStopError;
        StopManaged(IgnoredStopError);
        OutError = TEXT("Managed llama.cpp is disabled by the active profile.");
        SetState(ETextGenLocalServiceState::Disabled, OutError);
        return false;
    }

    FString Executable;
    if (!PrepareDesiredConfig(Config, Executable, OutError))
    {
        SetState(ETextGenLocalServiceState::Failed, OutError);
        return false;
    }

    bDesiredRunning = true;
    ++DesiredRevision;
    ReconcileDesiredState();
    return State != ETextGenLocalServiceState::Failed;
#endif
}

bool UTextGenLocalServiceSubsystem::StopManaged(FString& OutError)
{
    check(IsInGameThread());
    ++LifecycleGeneration;
    ++DesiredRevision;
    bDesiredRunning = false;
    bRouterReady = false;
    bModelOperationInFlight = false;
    bWaitingForLoad = false;
    bWaitingForUnload = false;
    ActiveModelId.Reset();
    CancelHttpRequest();
    StopReconcileTicker();
    bStopping = true;
    UProcessRuntimeSubsystem* ProcessRuntime = GEngine ? GEngine->GetEngineSubsystem<UProcessRuntimeSubsystem>() : nullptr;
    if (!ProcessRuntime || !ProcessRuntime->IsOwnedProcess(ManagedTextGenProcessId))
    {
        bStopping = false;
        SetState(ETextGenLocalServiceState::Stopped);
        OutError.Reset();
        return true;
    }
    FManagedProcessStatus Status;
    if (!ProcessRuntime->GetStatus(ManagedTextGenProcessId, Status) || !Status.IsActive())
    {
        bStopping = false;
        SetState(ETextGenLocalServiceState::Stopped);
        return true;
    }
    SetState(ETextGenLocalServiceState::Stopping);
    const bool bStopped = ProcessRuntime->StopProcess(ManagedTextGenProcessId, OutError);
    if (!bStopped)
    {
        bStopping = false;
        SetState(ETextGenLocalServiceState::Failed, OutError);
    }
    return bStopped;
}

bool UTextGenLocalServiceSubsystem::RestartManaged(const FTextGenLlamacppConfig& Config, FString& OutError)
{
    FString Executable;
    if (Config.bManagedDisabled || !PrepareDesiredConfig(Config, Executable, OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("Managed llama.cpp is disabled by the active profile.");
        SetState(Config.bManagedDisabled ? ETextGenLocalServiceState::Disabled : ETextGenLocalServiceState::Failed, OutError);
        return false;
    }
    if (!PublishModelPreset(DesiredConfig, DesiredModelPath, DesiredModelId, EffectivePlacement, OutError))
    {
        SetState(ETextGenLocalServiceState::Failed, OutError);
        return false;
    }

    bDesiredRunning = true;
    ++DesiredRevision;
    ++LifecycleGeneration;
    CancelHttpRequest();
    bRouterReady = false;
    bModelOperationInFlight = false;
    ActiveModelId.Reset();

    FManagedProcessSpec Spec;
    Spec.ExecutablePath = Executable;
    Spec.WorkingDirectory = FPaths::GetPath(Executable);
    Spec.Arguments = BuildRouterArguments(PresetPath, SlotPath, DesiredConfig.Port);
    Spec.bHidden = true;
    Spec.bCaptureOutput = true;
    Spec.Priority = bInteractiveWorkloadActive
        ? EManagedProcessPriority::AboveNormal
        : EManagedProcessPriority::BelowNormal;
    Spec.MaxRecentOutputLines = 200;

    StartupBeganSeconds = FPlatformTime::Seconds();
    bStopping = false;
    SetState(ETextGenLocalServiceState::Starting);

    UProcessRuntimeSubsystem* ProcessRuntime = GEngine ? GEngine->GetEngineSubsystem<UProcessRuntimeSubsystem>() : nullptr;
    if (!ProcessRuntime || !ProcessRuntime->RestartProcess(ManagedTextGenProcessId, Spec, OutError))
    {
        SetState(ETextGenLocalServiceState::Failed, OutError);
        return false;
    }
    StartReconcileTicker();
    return true;
}

void UTextGenLocalServiceSubsystem::SetInteractiveWorkloadActive(const bool bActive)
{
    check(IsInGameThread());
    if (bInteractiveWorkloadActive == bActive)
    {
        return;
    }

    bInteractiveWorkloadActive = bActive;
    UProcessRuntimeSubsystem* ProcessRuntime = GEngine ? GEngine->GetEngineSubsystem<UProcessRuntimeSubsystem>() : nullptr;
    if (!ProcessRuntime || !ProcessRuntime->IsOwnedProcess(ManagedTextGenProcessId))
    {
        return;
    }

    FManagedProcessStatus Status;
    if (!ProcessRuntime->GetStatus(ManagedTextGenProcessId, Status) || !Status.IsActive())
    {
        return;
    }

    FString Error;
    const EManagedProcessPriority Priority = bActive
        ? EManagedProcessPriority::AboveNormal
        : EManagedProcessPriority::BelowNormal;
    if (!ProcessRuntime->SetPriority(ManagedTextGenProcessId, Priority, Error))
    {
        UE_LOG(LogTextGenAPI, Warning, TEXT("Failed to change managed llama.cpp priority: %s"), *Error);
    }
}

#if WITH_EDITOR
bool UTextGenLocalServiceSubsystem::GenerateDefaultConversationCache(
    const FTextGenLlamacppConfig& Config,
    const FString& CacheKey,
    const FString& SystemPrompt,
    FTextGenDefaultCacheGenerationCallback&& Callback)
{
    check(IsInGameThread());

    if (DefaultCacheGenerationCallback)
    {
        Callback(false, TEXT("A default conversation cache generation is already active."), FString());
        return false;
    }
    if (CacheKey.IsEmpty() || SystemPrompt.IsEmpty() || Config.ModelPath.IsEmpty())
    {
        Callback(false, TEXT("Default cache generation requires a cache key, system prompt, and model."), FString());
        return false;
    }

    ++DefaultCacheGenerationRevision;
    const uint64 Revision = DefaultCacheGenerationRevision;
    DefaultCacheGenerationConfig = Config;
    DefaultCacheGenerationKey = CacheKey;
    DefaultCacheGenerationPrompt = SystemPrompt;
    DefaultCacheGenerationCallback = MoveTemp(Callback);
    DefaultCacheGenerationPhase = EDefaultCacheGenerationPhase::WaitingForService;

    FString Error;
    if (!StartManaged(Config, Error))
    {
        CompleteDefaultCacheGeneration(Revision, false, Error);
        return false;
    }
    if (State == ETextGenLocalServiceState::Ready)
    {
        BeginDefaultCacheApplyTemplate(Revision);
    }
    return true;
}

void UTextGenLocalServiceSubsystem::CancelDefaultConversationCacheGeneration()
{
    check(IsInGameThread());
    if (!DefaultCacheGenerationCallback)
    {
        return;
    }

    ++DefaultCacheGenerationRevision;
    FTextGenDefaultCacheGenerationCallback Callback = MoveTemp(DefaultCacheGenerationCallback);
    FHttpRequestPtr Request = MoveTemp(DefaultCacheGenerationRequest);
    DefaultCacheGenerationConfig = FTextGenLlamacppConfig();
    DefaultCacheGenerationKey.Reset();
    DefaultCacheGenerationPrompt.Reset();
    DefaultCacheGenerationModelId.Reset();
    DefaultCacheGenerationPhase = EDefaultCacheGenerationPhase::None;
    if (Request.IsValid())
    {
        Request->CancelRequest();
    }
    Callback(false, TEXT("Default conversation cache generation cancelled."), FString());
}

void UTextGenLocalServiceSubsystem::HandleDefaultCacheServiceState(const ETextGenLocalServiceState NewState)
{
    if (!DefaultCacheGenerationCallback)
    {
        return;
    }

    if (NewState == ETextGenLocalServiceState::Ready
        && DefaultCacheGenerationPhase == EDefaultCacheGenerationPhase::WaitingForService)
    {
        BeginDefaultCacheApplyTemplate(DefaultCacheGenerationRevision);
        return;
    }

    const bool bTerminal = NewState == ETextGenLocalServiceState::Failed
        || NewState == ETextGenLocalServiceState::Disabled
        || NewState == ETextGenLocalServiceState::Unhealthy
        || NewState == ETextGenLocalServiceState::Stopped;
    if (bTerminal)
    {
        CompleteDefaultCacheGeneration(
            DefaultCacheGenerationRevision,
            false,
            LastError.IsEmpty() ? TEXT("Managed llama.cpp service stopped during cache generation.") : LastError);
    }
    else if (DefaultCacheGenerationPhase != EDefaultCacheGenerationPhase::WaitingForService
        && NewState != ETextGenLocalServiceState::Ready)
    {
        CompleteDefaultCacheGeneration(
            DefaultCacheGenerationRevision,
            false,
            TEXT("Managed llama.cpp model changed during cache generation."));
    }
}

void UTextGenLocalServiceSubsystem::BeginDefaultCacheApplyTemplate(const uint64 Revision)
{
    if (Revision != DefaultCacheGenerationRevision || !DefaultCacheGenerationCallback
        || DefaultCacheGenerationRequest.IsValid())
    {
        return;
    }
    if (State != ETextGenLocalServiceState::Ready || ActiveModelId.IsEmpty())
    {
        CompleteDefaultCacheGeneration(Revision, false, TEXT("Managed llama.cpp model is not ready."));
        return;
    }

    DefaultCacheGenerationModelId = ActiveModelId;
    const TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
    Message->SetStringField(TEXT("role"), TEXT("system"));
    Message->SetStringField(TEXT("content"), DefaultCacheGenerationPrompt);
    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetArrayField(TEXT("messages"), {MakeShared<FJsonValueObject>(Message)});
    Root->SetStringField(TEXT("model"), DefaultCacheGenerationModelId);
    Root->SetBoolField(TEXT("add_generation_prompt"), false);

    DefaultCacheGenerationPhase = EDefaultCacheGenerationPhase::ApplyingTemplate;
    DefaultCacheGenerationRequest = FHttpModule::Get().CreateRequest();
    DefaultCacheGenerationRequest->SetURL(EffectiveBaseURL + TEXT("/apply-template"));
    DefaultCacheGenerationRequest->SetVerb(TEXT("POST"));
    DefaultCacheGenerationRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    DefaultCacheGenerationRequest->SetContentAsString(SerializeJsonObject(Root));
    DefaultCacheGenerationRequest->OnProcessRequestComplete().BindWeakLambda(this,
        [this, Revision](FHttpRequestPtr, FHttpResponsePtr Response, const bool bSuccess)
        {
            DefaultCacheGenerationRequest.Reset();
            if (Revision != DefaultCacheGenerationRevision || !DefaultCacheGenerationCallback)
            {
                return;
            }

            TSharedPtr<FJsonObject> RootObject;
            FString Prompt;
            if (!IsSuccessfulHttpResponse(bSuccess, Response)
                || !FJsonSerializer::Deserialize(
                    TJsonReaderFactory<>::Create(Response->GetContentAsString()), RootObject)
                || !RootObject.IsValid()
                || !RootObject->TryGetStringField(TEXT("prompt"), Prompt))
            {
                CompleteDefaultCacheGeneration(Revision, false, TEXT("llama.cpp chat template application failed."));
                return;
            }
            BeginDefaultCachePrefill(Revision, Prompt);
        });
    if (!DefaultCacheGenerationRequest->ProcessRequest())
    {
        DefaultCacheGenerationRequest.Reset();
        CompleteDefaultCacheGeneration(Revision, false, TEXT("llama.cpp chat template request could not start."));
    }
}

void UTextGenLocalServiceSubsystem::BeginDefaultCachePrefill(const uint64 Revision, const FString& Prompt)
{
    if (Revision != DefaultCacheGenerationRevision || !DefaultCacheGenerationCallback
        || DefaultCacheGenerationRequest.IsValid())
    {
        return;
    }

    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("prompt"), Prompt);
    Root->SetNumberField(TEXT("n_predict"), 0);
    Root->SetNumberField(TEXT("id_slot"), 0);
    Root->SetBoolField(TEXT("cache_prompt"), true);
    Root->SetStringField(TEXT("model"), DefaultCacheGenerationModelId);

    DefaultCacheGenerationPhase = EDefaultCacheGenerationPhase::Prefilling;
    DefaultCacheGenerationRequest = FHttpModule::Get().CreateRequest();
    DefaultCacheGenerationRequest->SetURL(EffectiveBaseURL + TEXT("/completion"));
    DefaultCacheGenerationRequest->SetVerb(TEXT("POST"));
    DefaultCacheGenerationRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    DefaultCacheGenerationRequest->SetContentAsString(SerializeJsonObject(Root));
    DefaultCacheGenerationRequest->OnProcessRequestComplete().BindWeakLambda(this,
        [this, Revision](FHttpRequestPtr, FHttpResponsePtr Response, const bool bSuccess)
        {
            DefaultCacheGenerationRequest.Reset();
            if (Revision != DefaultCacheGenerationRevision || !DefaultCacheGenerationCallback)
            {
                return;
            }
            if (!IsSuccessfulHttpResponse(bSuccess, Response))
            {
                CompleteDefaultCacheGeneration(Revision, false, TEXT("llama.cpp prompt prefill failed."));
                return;
            }
            BeginDefaultCacheSave(Revision);
        });
    if (!DefaultCacheGenerationRequest->ProcessRequest())
    {
        DefaultCacheGenerationRequest.Reset();
        CompleteDefaultCacheGeneration(Revision, false, TEXT("llama.cpp prompt prefill request could not start."));
    }
}

void UTextGenLocalServiceSubsystem::BeginDefaultCacheSave(const uint64 Revision)
{
    if (Revision != DefaultCacheGenerationRevision || !DefaultCacheGenerationCallback
        || DefaultCacheGenerationRequest.IsValid())
    {
        return;
    }

    constexpr const TCHAR* TemporaryFilename = TEXT("DefaultGenerate.next.bin");
    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("filename"), TemporaryFilename);
    Root->SetStringField(TEXT("model"), DefaultCacheGenerationModelId);

    DefaultCacheGenerationPhase = EDefaultCacheGenerationPhase::Saving;
    DefaultCacheGenerationRequest = FHttpModule::Get().CreateRequest();
    DefaultCacheGenerationRequest->SetURL(EffectiveBaseURL + TEXT("/slots/0?action=save"));
    DefaultCacheGenerationRequest->SetVerb(TEXT("POST"));
    DefaultCacheGenerationRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    DefaultCacheGenerationRequest->SetContentAsString(SerializeJsonObject(Root));
    DefaultCacheGenerationRequest->OnProcessRequestComplete().BindWeakLambda(this,
        [this, Revision](FHttpRequestPtr, FHttpResponsePtr Response, const bool bSuccess)
        {
            DefaultCacheGenerationRequest.Reset();
            if (Revision != DefaultCacheGenerationRevision || !DefaultCacheGenerationCallback)
            {
                return;
            }
            if (!IsSuccessfulHttpResponse(bSuccess, Response))
            {
                CompleteDefaultCacheGeneration(Revision, false, TEXT("llama.cpp slot save failed."));
                return;
            }

            constexpr const TCHAR* TemporaryFilename = TEXT("DefaultGenerate.next.bin");
            const FString Source = FPaths::Combine(
                FTextGenConversationCachePaths::GetProgressDirectory(), TemporaryFilename);
            const FString DestinationFilename = FTextGenConversationCachePaths::BuildDefaultFilename(
                DefaultCacheGenerationKey,
                FTextGenConversationCachePaths::ResolveRuntimeTag(DefaultCacheGenerationConfig),
                FTextGenConversationCachePaths::ResolveModelName(DefaultCacheGenerationConfig),
                DefaultCacheGenerationConfig.ContextSize,
                DefaultCacheGenerationConfig.CacheTypeK,
                DefaultCacheGenerationConfig.CacheTypeV);
            const FString Destination = FPaths::Combine(
                FTextGenConversationCachePaths::GetDefaultDirectory(), DestinationFilename);
            const FString Backup = Destination + TEXT(".bak");

            IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true);
            if (!FPaths::FileExists(Source))
            {
                CompleteDefaultCacheGeneration(Revision, false, TEXT("llama.cpp did not create the saved slot file."));
                return;
            }
            if (FPaths::FileExists(Destination)
                && !IFileManager::Get().Move(*Backup, *Destination, true, true, false, true))
            {
                CompleteDefaultCacheGeneration(Revision, false, TEXT("Existing Default cache could not be archived."));
                return;
            }
            if (!IFileManager::Get().Move(*Destination, *Source, true, true, false, true))
            {
                if (FPaths::FileExists(Backup) && !FPaths::FileExists(Destination))
                {
                    IFileManager::Get().Move(*Destination, *Backup, true, true, false, true);
                }
                CompleteDefaultCacheGeneration(Revision, false, TEXT("Generated Default cache could not be published."));
                return;
            }
            CompleteDefaultCacheGeneration(Revision, true, FString(), Destination);
        });
    if (!DefaultCacheGenerationRequest->ProcessRequest())
    {
        DefaultCacheGenerationRequest.Reset();
        CompleteDefaultCacheGeneration(Revision, false, TEXT("llama.cpp slot save request could not start."));
    }
}

void UTextGenLocalServiceSubsystem::CompleteDefaultCacheGeneration(
    const uint64 Revision,
    const bool bSuccess,
    const FString& Error,
    const FString& OutputPath)
{
    if (Revision != DefaultCacheGenerationRevision || !DefaultCacheGenerationCallback)
    {
        return;
    }

    ++DefaultCacheGenerationRevision;
    FTextGenDefaultCacheGenerationCallback Callback = MoveTemp(DefaultCacheGenerationCallback);
    FHttpRequestPtr Request = MoveTemp(DefaultCacheGenerationRequest);
    DefaultCacheGenerationConfig = FTextGenLlamacppConfig();
    DefaultCacheGenerationKey.Reset();
    DefaultCacheGenerationPrompt.Reset();
    DefaultCacheGenerationModelId.Reset();
    DefaultCacheGenerationPhase = EDefaultCacheGenerationPhase::None;
    if (Request.IsValid())
    {
        Request->CancelRequest();
    }
    Callback(bSuccess, Error, OutputPath);
}
#endif

void UTextGenLocalServiceSubsystem::SetState(const ETextGenLocalServiceState NewState, const FString& Error)
{
    if (!Error.IsEmpty())
    {
        LastError = Error;
    }
    else if (NewState != ETextGenLocalServiceState::Failed && NewState != ETextGenLocalServiceState::Unhealthy)
    {
        LastError.Reset();
    }
    if (State != NewState)
    {
        State = NewState;
        OnStateChanged.Broadcast(State);
    }
#if WITH_EDITOR
    HandleDefaultCacheServiceState(NewState);
#endif
}

bool UTextGenLocalServiceSubsystem::ResolveExecutable(const FTextGenLlamacppConfig& Config, FString& OutExecutable,
    FString& OutTag, ETextGenLlamacppBackend& OutBackend, FString& OutDevice, FString& OutDeviceName,
    int32& OutDeviceMemoryMiB, FString& OutError) const
{
    OutTag = Config.RuntimeTag;
    if (OutTag.IsEmpty())
    {
        OutError = TEXT("No exact llama.cpp runtime tag is configured.");
        return false;
    }
    FString FirstRunnableExecutable;
    ETextGenLlamacppBackend FirstRunnableBackend = Config.Backend;
    for (const ETextGenLlamacppBackend Backend : BuildBackendCandidates(Config.Backend))
    {
        FString RuntimeDirectory;
        if (!TextGenLlamacppRuntimePaths::ResolveRuntimeDirectory(OutTag, Backend, RuntimeDirectory))
        {
            continue;
        }
        const FString Candidate = FPaths::Combine(RuntimeDirectory, TEXT("llama-server.exe"));
        if (FirstRunnableExecutable.IsEmpty())
        {
            FirstRunnableExecutable = Candidate;
            FirstRunnableBackend = Backend;
        }

        FString DeviceId;
        FString DeviceName;
        int32 DeviceMemoryMiB = 0;
        if (ProbeBackendExecutable(Candidate, Backend, DeviceId, DeviceName, DeviceMemoryMiB))
        {
            OutExecutable = Candidate;
            OutBackend = Backend;
            OutDevice = DeviceId;
            OutDeviceName = DeviceName;
            OutDeviceMemoryMiB = DeviceMemoryMiB;
            return true;
        }
    }

    if (!FirstRunnableExecutable.IsEmpty())
    {
        OutExecutable = FirstRunnableExecutable;
        OutBackend = FirstRunnableBackend;
        OutDevice.Reset();
        OutDeviceName = TEXT("CPU");
        OutDeviceMemoryMiB = 0;
        return true;
    }

    OutError = FString::Printf(TEXT("No runnable CUDA 13, CUDA 12, or Vulkan llama.cpp runtime is installed for tag '%s'."), *OutTag);
    return false;
}

bool UTextGenLocalServiceSubsystem::ResolveModelPath(const FString& ConfiguredPath, FString& OutModelPath, FString& OutError) const
{
    if (ConfiguredPath.IsEmpty())
    {
        OutError = TEXT("Managed llama.cpp model path is empty.");
        return false;
    }
    OutModelPath = FPaths::IsRelative(ConfiguredPath)
        ? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), ConfiguredPath))
        : FPaths::ConvertRelativePathToFull(ConfiguredPath);
    FPaths::NormalizeFilename(OutModelPath);
    if (!FPaths::FileExists(OutModelPath))
    {
        OutError = FString::Printf(TEXT("GGUF model does not exist: %s"), *OutModelPath);
        return false;
    }
    if (!FPaths::GetExtension(OutModelPath).Equals(TEXT("gguf"), ESearchCase::IgnoreCase))
    {
        OutError = FString::Printf(TEXT("Managed model is not a GGUF file: %s"), *OutModelPath);
        return false;
    }
    return true;
}

bool UTextGenLocalServiceSubsystem::PrepareDesiredConfig(const FTextGenLlamacppConfig& Config, FString& OutExecutable, FString& OutError)
{
    FString RuntimeTag;
    FString ModelPath;
    FString DeviceName;
    if (!ResolveExecutable(Config, OutExecutable, RuntimeTag, DesiredBackend, DesiredDevice, DeviceName,
            DesiredDeviceMemoryMiB, OutError)
        || !ResolveModelPath(Config.ModelPath, ModelPath, OutError))
    {
        return false;
    }

    DesiredConfig = Config;
    DesiredConfig.RuntimeTag = RuntimeTag;
    DesiredConfig.Port = FMath::Clamp(Config.Port, 1, 65535);
    DesiredConfig.Threads = Config.Threads > 0
        ? Config.Threads
        : FMath::Max(1, FPlatformMisc::NumberOfCores());
    DesiredConfig.BatchSize = FMath::Clamp(Config.BatchSize, 1, 8192);
    DesiredConfig.UBatchSize = FMath::Clamp(Config.UBatchSize, 1, DesiredConfig.BatchSize);
    DesiredExecutable = OutExecutable;
    DesiredRuntimeTag = RuntimeTag;
    DesiredModelPath = ModelPath;
    DesiredModelId = BuildModelId(ModelPath);
    DesiredFallbackKey = BuildFallbackKey(DesiredConfig, ModelPath, DesiredBackend, DeviceName);
    EffectivePlacement.RequestedBackend = Config.Backend;
    EffectivePlacement.EffectiveBackend = DesiredBackend;
    EffectivePlacement.DeviceName = DeviceName;
    EffectivePlacement.DeviceMemoryMiB = DesiredDeviceMemoryMiB;
    EffectivePlacement.EffectiveOffloadMode = ResolveInitialOffloadMode(
        DesiredConfig, DesiredFallbackKey, DesiredDeviceMemoryMiB);
    EffectivePlacement.OffloadedLayerCount = -1;
    EffectivePlacement.TotalLayerCount = -1;
    EffectivePlacement.FallbackReason.Reset();
    EffectivePlacement.bFallbackApplied = EffectivePlacement.EffectiveOffloadMode != ETextGenLlamacppOffloadMode::DenseSharedGPU
        && DesiredConfig.OffloadMode == ETextGenLlamacppOffloadMode::Auto
        && DesiredDeviceMemoryMiB >= FourGiBClassThresholdMiB;
    SlotPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("TextGen"), TEXT("ConversationCaches"), TEXT("Slots")));
    PresetPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("TextGen"), TEXT("Router"), TEXT("models.ini")));
    IFileManager::Get().MakeDirectory(*SlotPath, true);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(PresetPath), true);
    EffectiveBaseURL = FString::Printf(TEXT("http://127.0.0.1:%d"), DesiredConfig.Port);
    return true;
}

bool UTextGenLocalServiceSubsystem::PublishModelPreset(const FTextGenLlamacppConfig& Config, const FString& ModelPath,
    const FString& ModelId, const FTextGenLlamacppPlacement& Placement, FString& OutError) const
{
    TArray<FString> Lines;
    Lines.Add(TEXT("version = 1"));
    Lines.Add(FString());
    Lines.Add(FString::Printf(TEXT("[%s]"), *ModelId));
    Lines.Add(FString::Printf(TEXT("model = %s"), *ModelPath));
    Lines.Add(FString::Printf(TEXT("ctx-size = %d"), Config.ContextSize));
    Lines.Add(FString::Printf(TEXT("threads = %d"), FMath::Max(1, Config.Threads)));
    Lines.Add(FString::Printf(TEXT("batch-size = %d"), Config.BatchSize));
    Lines.Add(FString::Printf(TEXT("ubatch-size = %d"), Config.UBatchSize));
    switch (Placement.EffectiveOffloadMode)
    {
    case ETextGenLlamacppOffloadMode::DenseSharedGPU:
        Lines.Add(FString::Printf(TEXT("device = %s"), *DesiredDevice));
        Lines.Add(TEXT("n-gpu-layers = all"));
        Lines.Add(TEXT("override-tensor = .*exps.*=CPU"));
        break;
    case ETextGenLlamacppOffloadMode::DenseOnlyGPU:
        Lines.Add(FString::Printf(TEXT("device = %s"), *DesiredDevice));
        Lines.Add(TEXT("n-gpu-layers = all"));
        Lines.Add(TEXT("override-tensor = .*ffn.*=CPU"));
        break;
    case ETextGenLlamacppOffloadMode::CPUOnly:
    case ETextGenLlamacppOffloadMode::Auto:
    default:
        Lines.Add(TEXT("device = none"));
        Lines.Add(TEXT("n-gpu-layers = 0"));
        Lines.Add(TEXT("no-kv-offload = true"));
        break;
    }
    Lines.Add(Config.bMMap ? TEXT("load-mode = mmap") : TEXT("load-mode = none"));
    Lines.Add(Config.bFlashAttention ? TEXT("flash-attn = on") : TEXT("flash-attn = off"));
    Lines.Add(FString::Printf(TEXT("cache-type-k = %s"), *CacheTypeToPresetValue(Config.CacheTypeK)));
    Lines.Add(FString::Printf(TEXT("cache-type-v = %s"), *CacheTypeToPresetValue(Config.CacheTypeV)));
    Lines.Add(TEXT("swa-full = true"));
    Lines.Add(Config.bEnableReasoning ? TEXT("reasoning = on") : TEXT("reasoning = off"));
    Lines.Add(FString::Printf(TEXT("reasoning-budget = %d"),
        Config.bEnableReasoning ? FMath::Clamp(Config.ReasoningBudgetTokens, 128, 4096) : 0));
    Lines.Add(TEXT("parallel = 1"));
    Lines.Add(TEXT("poll = 0"));
    Lines.Add(Config.bContinuousBatching ? TEXT("cont-batching = true") : TEXT("no-cont-batching = true"));
    Lines.Add(TEXT("load-on-startup = false"));
    Lines.Add(TEXT("stop-timeout = 30"));

    const FString TempPath = PresetPath + TEXT(".next");
    if (!FFileHelper::SaveStringArrayToFile(Lines, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
        || !IFileManager::Get().Move(*PresetPath, *TempPath, true, true, false, true))
    {
        OutError = FString::Printf(TEXT("Failed to publish llama.cpp model preset: %s"), *PresetPath);
        return false;
    }
    return true;
}

FString UTextGenLocalServiceSubsystem::BuildFallbackKey(const FTextGenLlamacppConfig& Config,
    const FString& ModelPath, const ETextGenLlamacppBackend Backend, const FString& DeviceName) const
{
    const int64 FileSize = IFileManager::Get().FileSize(*ModelPath);
    const int64 Timestamp = IFileManager::Get().GetTimeStamp(*ModelPath).ToUnixTimestamp();
    const FString Identity = FString::Printf(TEXT("%s|%lld|%lld|%s|%d|%s|%d|%d|%d"),
        *ModelPath, FileSize, Timestamp, *Config.RuntimeTag, static_cast<int32>(Backend), *DeviceName,
        Config.ContextSize, static_cast<int32>(Config.CacheTypeK), static_cast<int32>(Config.CacheTypeV));
    return FMD5::HashAnsiString(*Identity);
}

ETextGenLlamacppOffloadMode UTextGenLocalServiceSubsystem::ResolveInitialOffloadMode(
    const FTextGenLlamacppConfig& Config, const FString& FallbackKey, const int32 DeviceMemoryMiB) const
{
    if (Config.OffloadMode != ETextGenLlamacppOffloadMode::Auto)
    {
        return Config.OffloadMode;
    }
    if (DeviceMemoryMiB < FourGiBClassThresholdMiB)
    {
        return ETextGenLlamacppOffloadMode::CPUOnly;
    }

    int32 StoredMode = 0;
    const FString StatePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TextGen"), TEXT("FallbackState.ini"));
    if (GConfig && GConfig->GetInt(TEXT("LlamacppOffload"), *FallbackKey, StoredMode, StatePath)
        && StoredMode >= static_cast<int32>(ETextGenLlamacppOffloadMode::DenseSharedGPU)
        && StoredMode <= static_cast<int32>(ETextGenLlamacppOffloadMode::CPUOnly))
    {
        return static_cast<ETextGenLlamacppOffloadMode>(StoredMode);
    }
    return ETextGenLlamacppOffloadMode::DenseSharedGPU;
}

bool UTextGenLocalServiceSubsystem::IsAcceleratorMemoryFailure(const FString& Failure)
{
    const FString Lower = Failure.ToLower();
    return Lower.Contains(TEXT("out of memory"))
        || Lower.Contains(TEXT("failed to allocate"))
        || Lower.Contains(TEXT("allocation failed"))
        || Lower.Contains(TEXT("cuda error out of memory"))
        || Lower.Contains(TEXT("vk_error_out_of_device_memory"))
        || Lower.Contains(TEXT("device memory allocation"));
}

bool UTextGenLocalServiceSubsystem::TryApplyMandatoryFallback(const FString& Failure)
{
    if (!IsAcceleratorMemoryFailure(Failure))
    {
        return false;
    }

    ETextGenLlamacppOffloadMode NextMode = ETextGenLlamacppOffloadMode::Auto;
    switch (EffectivePlacement.EffectiveOffloadMode)
    {
    case ETextGenLlamacppOffloadMode::DenseSharedGPU:
        NextMode = ETextGenLlamacppOffloadMode::DenseOnlyGPU;
        break;
    case ETextGenLlamacppOffloadMode::DenseOnlyGPU:
        NextMode = ETextGenLlamacppOffloadMode::CPUOnly;
        break;
    case ETextGenLlamacppOffloadMode::CPUOnly:
    case ETextGenLlamacppOffloadMode::Auto:
    default:
        return false;
    }

    // Mandatory safety invariant: a previous success must never disable a later memory fallback.
    EffectivePlacement.EffectiveOffloadMode = NextMode;
    EffectivePlacement.bFallbackApplied = true;
    EffectivePlacement.FallbackReason = Failure;
    EffectivePlacement.OffloadedLayerCount = -1;
    EffectivePlacement.TotalLayerCount = -1;

    const FString StatePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TextGen"), TEXT("FallbackState.ini"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(StatePath), true);
    if (GConfig)
    {
        GConfig->SetInt(TEXT("LlamacppOffload"), *DesiredFallbackKey, static_cast<int32>(NextMode), StatePath);
        GConfig->Flush(false, StatePath);
    }

    CancelHttpRequest();
    bModelOperationInFlight = false;
    bWaitingForLoad = false;
    bWaitingForUnload = false;
    ActiveModelId.Reset();
    ++DesiredRevision;
    SetState(ETextGenLocalServiceState::Unhealthy, Failure);
    StartReconcileTicker();
    return true;
}

FString UTextGenLocalServiceSubsystem::BuildRouterArguments(const FString& InPresetPath, const FString& InSlotPath, const int32 Port) const
{
    TArray<FString> Args;
    Args.Add(TEXT("--host 127.0.0.1"));
    Args.Add(FString::Printf(TEXT("--port %d"), Port));
    Args.Add(FString::Printf(TEXT("--models-preset %s"), *QuoteArgument(InPresetPath)));
    Args.Add(TEXT("--models-max 1"));
    Args.Add(TEXT("--no-models-autoload"));
    Args.Add(TEXT("--slots"));
    Args.Add(FString::Printf(TEXT("--slot-save-path %s"), *QuoteArgument(InSlotPath)));
    Args.Add(TEXT("--no-webui"));
    Args.Add(TEXT("--offline"));
    return FString::Join(Args, TEXT(" "));
}

FString UTextGenLocalServiceSubsystem::BuildModelId(const FString& ModelPath) const
{
    FString Id = FPaths::GetBaseFilename(ModelPath);
    static const FString Invalid(TEXT("\\/:*?\"<>|[]"));
    for (TCHAR& Character : Id)
    {
        if (Invalid.Contains(FString::Chr(Character)) || FChar::IsWhitespace(Character))
        {
            Character = TEXT('_');
        }
    }
    return Id.IsEmpty() ? TEXT("local-model") : Id;
}

void UTextGenLocalServiceSubsystem::ReconcileDesiredState()
{
    if (!bDesiredRunning || bModelOperationInFlight || bStopping)
    {
        return;
    }

    UProcessRuntimeSubsystem* ProcessRuntime = GEngine ? GEngine->GetEngineSubsystem<UProcessRuntimeSubsystem>() : nullptr;
    if (!ProcessRuntime)
    {
        SetState(ETextGenLocalServiceState::Failed, TEXT("ProcessRuntime subsystem is unavailable."));
        return;
    }

    FManagedProcessStatus ProcessStatus;
    const bool bProcessActive = ProcessRuntime->GetStatus(ManagedTextGenProcessId, ProcessStatus) && ProcessStatus.IsActive();
    const bool bParentConfigChanged = bProcessActive
        && (!ActiveRuntimeTag.Equals(DesiredRuntimeTag, ESearchCase::CaseSensitive)
            || ActiveBackend != DesiredBackend || ActiveConfig.Port != DesiredConfig.Port);
    if (!bProcessActive || bParentConfigChanged)
    {
        FString Error;
        if (!PublishModelPreset(DesiredConfig, DesiredModelPath, DesiredModelId, EffectivePlacement, Error))
        {
            SetState(ETextGenLocalServiceState::Failed, Error);
            return;
        }

        const FString Arguments = BuildRouterArguments(PresetPath, SlotPath, DesiredConfig.Port);
        FManagedProcessSpec Spec;
        Spec.ExecutablePath = DesiredExecutable;
        Spec.WorkingDirectory = FPaths::GetPath(DesiredExecutable);
        Spec.Arguments = Arguments;
        Spec.bHidden = true;
        Spec.bCaptureOutput = true;
        Spec.Priority = bInteractiveWorkloadActive
            ? EManagedProcessPriority::AboveNormal
            : EManagedProcessPriority::BelowNormal;
        Spec.MaxRecentOutputLines = 200;

        ++LifecycleGeneration;
        CancelHttpRequest();
        bRouterReady = false;
        ActiveModelId.Reset();
        ActiveRuntimeTag = DesiredRuntimeTag;
        ActiveBackend = DesiredBackend;
        ActiveConfig = DesiredConfig;
        EffectiveCommandLine = FString::Printf(TEXT("%s %s"), *DesiredExecutable, *Arguments);
        StartupBeganSeconds = FPlatformTime::Seconds();
        SetState(ETextGenLocalServiceState::Starting);
        const bool bStarted = bProcessActive
            ? ProcessRuntime->RestartProcess(ManagedTextGenProcessId, Spec, Error)
            : ProcessRuntime->StartProcess(ManagedTextGenProcessId, Spec, Error);
        if (!bStarted)
        {
            SetState(ETextGenLocalServiceState::Failed, Error);
            return;
        }
        StartReconcileTicker();
        return;
    }

    if (!bRouterReady)
    {
        StartReconcileTicker();
        return;
    }

    if (ActiveModelId.Equals(DesiredModelId, ESearchCase::CaseSensitive)
        && HasEquivalentModelConfig(ActiveConfig, DesiredConfig))
    {
        OperationRevision = DesiredRevision;
        SetState(ETextGenLocalServiceState::Ready);
        StopReconcileTicker();
        return;
    }

    OperationRevision = DesiredRevision;
    OperationConfig = DesiredConfig;
    OperationModelId = DesiredModelId;
    OperationModelPath = DesiredModelPath;
    OperationRuntimeTag = DesiredRuntimeTag;
    bModelOperationInFlight = true;
    StartupBeganSeconds = FPlatformTime::Seconds();
    SetState(ETextGenLocalServiceState::LoadingModel);
    if (!ActiveModelId.IsEmpty())
    {
        RequestModelUnload(LifecycleGeneration, OperationRevision);
    }
    else
    {
        RequestModelCatalogReload(LifecycleGeneration, OperationRevision);
    }
}

bool UTextGenLocalServiceSubsystem::TickReconcile(float)
{
    if (!bDesiredRunning)
    {
        return false;
    }
    if (FPlatformTime::Seconds() - StartupBeganSeconds >= DesiredConfig.StartupTimeout)
    {
        CancelHttpRequest();
        const FString Failure = RecentOutput.Num() > 0
            ? FString::Join(RecentOutput, TEXT("\n"))
            : TEXT("llama.cpp router/model startup timeout expired.");
        if (TryApplyMandatoryFallback(Failure))
        {
            StartupBeganSeconds = FPlatformTime::Seconds();
            return true;
        }
        SetState(ETextGenLocalServiceState::Failed, TEXT("llama.cpp router/model startup timeout expired."));
        ReconcileTickerHandle.Reset();
        return false;
    }
    if (!ActiveHttpRequest.IsValid())
    {
        if (!bRouterReady)
        {
            RequestRouterHealth(LifecycleGeneration);
        }
        else if (bWaitingForUnload || bWaitingForLoad)
        {
            RequestModelStatus(LifecycleGeneration, OperationRevision);
        }
        else
        {
            ReconcileDesiredState();
        }
    }
    return true;
}

void UTextGenLocalServiceSubsystem::StartReconcileTicker()
{
    if (!ReconcileTickerHandle.IsValid())
    {
        ReconcileTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateUObject(this, &UTextGenLocalServiceSubsystem::TickReconcile), 0.25f);
    }
}

void UTextGenLocalServiceSubsystem::StopReconcileTicker()
{
    if (ReconcileTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(ReconcileTickerHandle);
        ReconcileTickerHandle.Reset();
    }
}

void UTextGenLocalServiceSubsystem::CancelHttpRequest()
{
    if (ActiveHttpRequest.IsValid())
    {
        ActiveHttpRequest->CancelRequest();
        ActiveHttpRequest.Reset();
    }
}

void UTextGenLocalServiceSubsystem::RequestRouterHealth(const uint64 Generation)
{
    ActiveHttpRequest = FHttpModule::Get().CreateRequest();
    ActiveHttpRequest->SetURL(EffectiveBaseURL + TEXT("/health"));
    ActiveHttpRequest->SetVerb(TEXT("GET"));
    ActiveHttpRequest->SetTimeout(5.0f);
    ActiveHttpRequest->OnProcessRequestComplete().BindWeakLambda(this,
        [this, Generation](FHttpRequestPtr, FHttpResponsePtr Response, const bool bSuccess)
        {
            ActiveHttpRequest.Reset();
            if (Generation != LifecycleGeneration || !bDesiredRunning) return;
            if (bSuccess && Response.IsValid() && Response->GetResponseCode() == 200)
            {
                bRouterReady = true;
                SetState(ETextGenLocalServiceState::LoadingModel);
                ReconcileDesiredState();
            }
        });
    if (!ActiveHttpRequest->ProcessRequest()) ActiveHttpRequest.Reset();
}

void UTextGenLocalServiceSubsystem::RequestModelUnload(const uint64 Generation, const uint64 Revision)
{
    ActiveHttpRequest = FHttpModule::Get().CreateRequest();
    ActiveHttpRequest->SetURL(EffectiveBaseURL + TEXT("/models/unload"));
    ActiveHttpRequest->SetVerb(TEXT("POST"));
    ActiveHttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    ActiveHttpRequest->SetContentAsString(FString::Printf(TEXT("{\"model\":\"%s\"}"), *ActiveModelId.ReplaceCharWithEscapedChar()));
    ActiveHttpRequest->OnProcessRequestComplete().BindWeakLambda(this,
        [this, Generation, Revision](FHttpRequestPtr, FHttpResponsePtr Response, const bool bSuccess)
        {
            ActiveHttpRequest.Reset();
            if (Generation != LifecycleGeneration || Revision != OperationRevision) return;
            if (!bSuccess || !Response.IsValid() || !EHttpResponseCodes::IsOk(Response->GetResponseCode()))
            {
                FinishModelOperation(false, TEXT("llama.cpp model unload request failed."));
                return;
            }
            bWaitingForUnload = true;
            StartReconcileTicker();
        });
    if (!ActiveHttpRequest->ProcessRequest())
    {
        ActiveHttpRequest.Reset();
        FinishModelOperation(false, TEXT("llama.cpp model unload request could not start."));
    }
}

void UTextGenLocalServiceSubsystem::RequestModelCatalogReload(const uint64 Generation, const uint64 Revision)
{
    FString Error;
    if (!PublishModelPreset(OperationConfig, OperationModelPath, OperationModelId, EffectivePlacement, Error))
    {
        FinishModelOperation(false, Error);
        return;
    }
    ActiveHttpRequest = FHttpModule::Get().CreateRequest();
    ActiveHttpRequest->SetURL(EffectiveBaseURL + TEXT("/models?reload=1"));
    ActiveHttpRequest->SetVerb(TEXT("GET"));
    ActiveHttpRequest->OnProcessRequestComplete().BindWeakLambda(this,
        [this, Generation, Revision](FHttpRequestPtr, FHttpResponsePtr Response, const bool bSuccess)
        {
            ActiveHttpRequest.Reset();
            if (Generation != LifecycleGeneration || Revision != OperationRevision) return;
            if (Revision != DesiredRevision)
            {
                FinishModelOperation(true);
                return;
            }
            if (!bSuccess || !Response.IsValid() || !EHttpResponseCodes::IsOk(Response->GetResponseCode()))
            {
                FinishModelOperation(false, TEXT("llama.cpp model catalog reload failed."));
                return;
            }
            RequestModelLoad(Generation, Revision);
        });
    if (!ActiveHttpRequest->ProcessRequest())
    {
        ActiveHttpRequest.Reset();
        FinishModelOperation(false, TEXT("llama.cpp model catalog reload could not start."));
    }
}

void UTextGenLocalServiceSubsystem::RequestModelLoad(const uint64 Generation, const uint64 Revision)
{
    ActiveHttpRequest = FHttpModule::Get().CreateRequest();
    ActiveHttpRequest->SetURL(EffectiveBaseURL + TEXT("/models/load"));
    ActiveHttpRequest->SetVerb(TEXT("POST"));
    ActiveHttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    ActiveHttpRequest->SetContentAsString(FString::Printf(TEXT("{\"model\":\"%s\"}"), *OperationModelId.ReplaceCharWithEscapedChar()));
    ActiveHttpRequest->OnProcessRequestComplete().BindWeakLambda(this,
        [this, Generation, Revision](FHttpRequestPtr, FHttpResponsePtr Response, const bool bSuccess)
        {
            ActiveHttpRequest.Reset();
            if (Generation != LifecycleGeneration || Revision != OperationRevision) return;
            if (!bSuccess || !Response.IsValid() || !EHttpResponseCodes::IsOk(Response->GetResponseCode()))
            {
                const FString Detail = Response.IsValid() ? Response->GetContentAsString() : FString();
                FinishModelOperation(false, Detail.IsEmpty() ? TEXT("llama.cpp model load request failed.") : Detail);
                return;
            }
            bWaitingForLoad = true;
            StartReconcileTicker();
        });
    if (!ActiveHttpRequest->ProcessRequest())
    {
        ActiveHttpRequest.Reset();
        FinishModelOperation(false, TEXT("llama.cpp model load request could not start."));
    }
}

void UTextGenLocalServiceSubsystem::RequestModelStatus(const uint64 Generation, const uint64 Revision)
{
    const FString WatchedModel = bWaitingForUnload ? ActiveModelId : OperationModelId;
    ActiveHttpRequest = FHttpModule::Get().CreateRequest();
    ActiveHttpRequest->SetURL(EffectiveBaseURL + TEXT("/models"));
    ActiveHttpRequest->SetVerb(TEXT("GET"));
    ActiveHttpRequest->OnProcessRequestComplete().BindWeakLambda(this,
        [this, Generation, Revision, WatchedModel](FHttpRequestPtr, FHttpResponsePtr Response, const bool bSuccess)
        {
            ActiveHttpRequest.Reset();
            if (Generation != LifecycleGeneration || Revision != OperationRevision) return;
            if (!bSuccess || !Response.IsValid() || !EHttpResponseCodes::IsOk(Response->GetResponseCode())) return;
            FString Status;
            bool bFailed = false;
            if (!ParseModelStatus(Response->GetContentAsString(), WatchedModel, Status, bFailed)) return;
            if (bFailed)
            {
                FinishModelOperation(false, FString::Printf(TEXT("llama.cpp model '%s' failed to load."), *WatchedModel));
                return;
            }
            if (bWaitingForUnload && Status.Equals(TEXT("unloaded"), ESearchCase::IgnoreCase))
            {
                bWaitingForUnload = false;
                ActiveModelId.Reset();
                if (Revision != DesiredRevision)
                {
                    FinishModelOperation(true);
                    return;
                }
                RequestModelCatalogReload(Generation, Revision);
            }
            else if (bWaitingForLoad && Status.Equals(TEXT("loaded"), ESearchCase::IgnoreCase))
            {
                bWaitingForLoad = false;
                ActiveModelId = OperationModelId;
                ActiveConfig = OperationConfig;
                ActiveRuntimeTag = OperationRuntimeTag;
                FinishModelOperation(true);
            }
        });
    if (!ActiveHttpRequest->ProcessRequest()) ActiveHttpRequest.Reset();
}

bool UTextGenLocalServiceSubsystem::ParseModelStatus(const FString& ResponseBody, const FString& ModelId,
    FString& OutStatus, bool& bOutFailed) const
{
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
    const TArray<TSharedPtr<FJsonValue>>* Models = nullptr;
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() || !Root->TryGetArrayField(TEXT("data"), Models) || !Models)
    {
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Models)
    {
        const TSharedPtr<FJsonObject> Model = Value.IsValid() ? Value->AsObject() : nullptr;
        FString Id;
        if (!Model.IsValid() || !Model->TryGetStringField(TEXT("id"), Id) || !Id.Equals(ModelId, ESearchCase::CaseSensitive)) continue;
        const TSharedPtr<FJsonObject>* Status = nullptr;
        if (!Model->TryGetObjectField(TEXT("status"), Status) || !Status || !Status->IsValid()) return false;
        (*Status)->TryGetStringField(TEXT("value"), OutStatus);
        (*Status)->TryGetBoolField(TEXT("failed"), bOutFailed);
        return !OutStatus.IsEmpty();
    }
    OutStatus = TEXT("unloaded");
    return true;
}

void UTextGenLocalServiceSubsystem::FinishModelOperation(const bool bSuccess, const FString& Error)
{
    bModelOperationInFlight = false;
    bWaitingForLoad = false;
    bWaitingForUnload = false;
    if (!bSuccess)
    {
        const FString FailureContext = RecentOutput.Num() > 0
            ? Error + TEXT("\n") + FString::Join(RecentOutput, TEXT("\n"))
            : Error;
        if (TryApplyMandatoryFallback(FailureContext))
        {
            return;
        }
        SetState(ETextGenLocalServiceState::Failed, Error);
        StopReconcileTicker();
        return;
    }
    if (OperationRevision == DesiredRevision)
    {
        SetState(ETextGenLocalServiceState::Ready);
        StopReconcileTicker();
    }
    else
    {
        SetState(ETextGenLocalServiceState::LoadingModel);
        ReconcileDesiredState();
    }
}

void UTextGenLocalServiceSubsystem::HandleProcessState(const FManagedProcessId& ProcessId, const FManagedProcessStatus& Status)
{
    if (ProcessId != ManagedTextGenProcessId)
    {
        return;
    }
    if (Status.State == EManagedProcessState::Failed || (Status.State == EManagedProcessState::Exited && !bStopping))
    {
        CancelHttpRequest();
        StopReconcileTicker();
        bRouterReady = false;
        bModelOperationInFlight = false;
        FString Failure = Status.LastError;
        if (Failure.IsEmpty() && Status.RecentOutput.Num() > 0)
        {
            Failure = Status.RecentOutput.Last();
        }
        if (Failure.IsEmpty())
        {
            Failure = TEXT("llama.cpp exited unexpectedly.");
        }
        const FString FailureContext = Status.RecentOutput.Num() > 0
            ? Failure + TEXT("\n") + FString::Join(Status.RecentOutput, TEXT("\n"))
            : Failure;
        if (TryApplyMandatoryFallback(FailureContext))
        {
            return;
        }
        SetState(ETextGenLocalServiceState::Failed, Failure);
    }
    else if (Status.State == EManagedProcessState::Stopped && bStopping)
    {
        bStopping = false;
        if (bDesiredRunning)
        {
            SetState(ETextGenLocalServiceState::Starting);
            ReconcileDesiredState();
        }
        else
        {
            SetState(ETextGenLocalServiceState::Stopped);
        }
    }
}

void UTextGenLocalServiceSubsystem::HandleProcessOutput(const FManagedProcessId& ProcessId, const FString& OutputLine)
{
    if (ProcessId != ManagedTextGenProcessId)
    {
        return;
    }
    RecentOutput.Add(OutputLine);
    if (RecentOutput.Num() > 200)
    {
        RecentOutput.RemoveAt(0, RecentOutput.Num() - 200, EAllowShrinking::No);
    }
    const FRegexPattern OffloadPattern(TEXT("offloaded ([0-9]+)\\/([0-9]+) layers to GPU"));
    FRegexMatcher OffloadMatcher(OffloadPattern, OutputLine);
    if (OffloadMatcher.FindNext())
    {
        EffectivePlacement.OffloadedLayerCount = FCString::Atoi(*OffloadMatcher.GetCaptureGroup(1));
        EffectivePlacement.TotalLayerCount = FCString::Atoi(*OffloadMatcher.GetCaptureGroup(2));
    }
    OnOutput.Broadcast(OutputLine);
}
