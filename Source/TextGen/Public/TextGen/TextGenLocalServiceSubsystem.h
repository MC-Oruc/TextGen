#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HttpFwd.h"
#include "Subsystems/EngineSubsystem.h"
#include "TextGen/TextGenTypes.h"
#include "TextGenLocalServiceSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTextGenLocalServiceStateChanged, ETextGenLocalServiceState, State);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTextGenLocalServiceOutput, const FString&, OutputLine);

#if WITH_EDITOR
using FTextGenDefaultCacheGenerationCallback = TFunction<void(bool, const FString&, const FString&)>;
#endif

UCLASS(BlueprintType)
class TEXTGEN_API UTextGenLocalServiceSubsystem final : public UEngineSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category = "TextGen|Managed Process")
    bool StartManaged(const FTextGenLlamacppConfig& Config, FString& OutError);

    UFUNCTION(BlueprintCallable, Category = "TextGen|Managed Process")
    bool StopManaged(FString& OutError);

    UFUNCTION(BlueprintCallable, Category = "TextGen|Managed Process")
    bool RestartManaged(const FTextGenLlamacppConfig& Config, FString& OutError);

    UFUNCTION(BlueprintPure, Category = "TextGen|Managed Process")
    ETextGenLocalServiceState GetState() const { return State; }

    UFUNCTION(BlueprintPure, Category = "TextGen|Managed Process")
    FString GetEffectiveBaseURL() const { return EffectiveBaseURL; }

    UFUNCTION(BlueprintPure, Category = "TextGen|Managed Process")
    FString GetEffectiveCommandLine() const { return EffectiveCommandLine; }

    UFUNCTION(BlueprintPure, Category = "TextGen|Managed Process")
    FString GetLastError() const { return LastError; }

    UFUNCTION(BlueprintPure, Category = "TextGen|Managed Process")
    TArray<FString> GetRecentOutput() const { return RecentOutput; }

    UFUNCTION(BlueprintPure, Category = "TextGen|Managed Process")
    FString GetActiveModelId() const { return ActiveModelId; }

    UFUNCTION(BlueprintPure, Category = "TextGen|Managed Process")
    FString GetActiveRuntimeTag() const { return ActiveRuntimeTag; }

    void SetInteractiveWorkloadActive(bool bActive);

    const FTextGenLlamacppConfig& GetActiveConfig() const { return ActiveConfig; }
    const FTextGenLlamacppPlacement& GetEffectivePlacement() const { return EffectivePlacement; }

#if WITH_EDITOR
    bool GenerateDefaultConversationCache(
        const FTextGenLlamacppConfig& Config,
        const FString& CacheKey,
        const FString& SystemPrompt,
        FTextGenDefaultCacheGenerationCallback&& Callback);
    void CancelDefaultConversationCacheGeneration();
    bool IsDefaultConversationCacheGenerationActive() const { return static_cast<bool>(DefaultCacheGenerationCallback); }
#endif

    UPROPERTY(BlueprintAssignable, Category = "TextGen|Managed Process")
    FOnTextGenLocalServiceStateChanged OnStateChanged;

    UPROPERTY(BlueprintAssignable, Category = "TextGen|Managed Process")
    FOnTextGenLocalServiceOutput OnOutput;

private:
    void SetState(ETextGenLocalServiceState NewState, const FString& Error = FString());
    bool ResolveExecutable(const FTextGenLlamacppConfig& Config, FString& OutExecutable, FString& OutTag,
        ETextGenLlamacppBackend& OutBackend, FString& OutDevice, FString& OutDeviceName,
        int32& OutDeviceMemoryMiB, FString& OutError) const;
    bool ResolveModelPath(const FString& ConfiguredPath, FString& OutModelPath, FString& OutError) const;
    bool PrepareDesiredConfig(const FTextGenLlamacppConfig& Config, FString& OutExecutable, FString& OutError);
    bool PublishModelPreset(const FTextGenLlamacppConfig& Config, const FString& ModelPath, const FString& ModelId,
        const FTextGenLlamacppPlacement& Placement, FString& OutError) const;
    FString BuildFallbackKey(const FTextGenLlamacppConfig& Config, const FString& ModelPath,
        ETextGenLlamacppBackend Backend, const FString& DeviceName) const;
    ETextGenLlamacppOffloadMode ResolveInitialOffloadMode(const FTextGenLlamacppConfig& Config,
        const FString& FallbackKey, int32 DeviceMemoryMiB) const;
    bool TryApplyMandatoryFallback(const FString& Failure);
    static bool IsAcceleratorMemoryFailure(const FString& Failure);
    FString BuildRouterArguments(const FString& PresetPath, const FString& SlotPath, int32 Port) const;
    FString BuildModelId(const FString& ModelPath) const;
    bool TickReconcile(float DeltaSeconds);
    void StartReconcileTicker();
    void StopReconcileTicker();
    void ReconcileDesiredState();
    void RequestRouterHealth(uint64 Generation);
    void RequestModelUnload(uint64 Generation, uint64 Revision);
    void RequestModelCatalogReload(uint64 Generation, uint64 Revision);
    void RequestModelLoad(uint64 Generation, uint64 Revision);
    void RequestModelStatus(uint64 Generation, uint64 Revision);
    void FinishModelOperation(bool bSuccess, const FString& Error = FString());
    bool ParseModelStatus(const FString& ResponseBody, const FString& ModelId, FString& OutStatus, bool& bOutFailed) const;
    void CancelHttpRequest();
    void HandleProcessState(const struct FManagedProcessId& ProcessId, const struct FManagedProcessStatus& Status);
    void HandleProcessOutput(const struct FManagedProcessId& ProcessId, const FString& OutputLine);

#if WITH_EDITOR
    enum class EDefaultCacheGenerationPhase : uint8
    {
        None,
        WaitingForService,
        ApplyingTemplate,
        Prefilling,
        Saving
    };

    void HandleDefaultCacheServiceState(ETextGenLocalServiceState NewState);
    void BeginDefaultCacheApplyTemplate(uint64 Revision);
    void BeginDefaultCachePrefill(uint64 Revision, const FString& Prompt);
    void BeginDefaultCacheSave(uint64 Revision);
    void CompleteDefaultCacheGeneration(uint64 Revision, bool bSuccess, const FString& Error, const FString& OutputPath = FString());

    FTextGenLlamacppConfig DefaultCacheGenerationConfig;
    FString DefaultCacheGenerationKey;
    FString DefaultCacheGenerationPrompt;
    FString DefaultCacheGenerationModelId;
    FTextGenDefaultCacheGenerationCallback DefaultCacheGenerationCallback;
    FHttpRequestPtr DefaultCacheGenerationRequest;
    EDefaultCacheGenerationPhase DefaultCacheGenerationPhase = EDefaultCacheGenerationPhase::None;
    uint64 DefaultCacheGenerationRevision = 0;
#endif

    UPROPERTY(Transient)
    ETextGenLocalServiceState State = ETextGenLocalServiceState::Stopped;

    UPROPERTY(Transient)
    FTextGenLlamacppConfig ActiveConfig;

    UPROPERTY(Transient)
    FTextGenLlamacppPlacement EffectivePlacement;

    FTextGenLlamacppConfig DesiredConfig;
    FTextGenLlamacppConfig OperationConfig;

    FString EffectiveBaseURL;
    FString EffectiveCommandLine;
    FString LastError;
    FString ActiveRuntimeTag;
    FString DesiredRuntimeTag;
    ETextGenLlamacppBackend ActiveBackend = ETextGenLlamacppBackend::CUDA13;
    ETextGenLlamacppBackend DesiredBackend = ETextGenLlamacppBackend::CUDA13;
    FString DesiredDevice;
    int32 DesiredDeviceMemoryMiB = 0;
    FString DesiredFallbackKey;
    FString DesiredExecutable;
    FString DesiredModelPath;
    FString DesiredModelId;
    FString ActiveModelId;
    FString OperationModelId;
    FString OperationModelPath;
    FString OperationRuntimeTag;
    FString PresetPath;
    FString SlotPath;
    TArray<FString> RecentOutput;
    double StartupBeganSeconds = 0.0;
    uint64 LifecycleGeneration = 0;
    uint64 DesiredRevision = 0;
    uint64 OperationRevision = 0;
    bool bDesiredRunning = false;
    bool bRouterReady = false;
    bool bModelOperationInFlight = false;
    bool bWaitingForUnload = false;
    bool bWaitingForLoad = false;
    bool bStopping = false;
    bool bInteractiveWorkloadActive = false;
    FDelegateHandle ProcessStateHandle;
    FDelegateHandle ProcessOutputHandle;
    FTSTicker::FDelegateHandle ReconcileTickerHandle;
    FHttpRequestPtr ActiveHttpRequest;
};
