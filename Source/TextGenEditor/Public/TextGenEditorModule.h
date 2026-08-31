#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "Modules/ModuleManager.h"
#include "TextGen/TextGenEnums.h"
#include "UObject/StrongObjectPtr.h"

class UTextGenContentBrowserDataSource;
class SNotificationItem;
struct FPropertyChangedEvent;

class FTextGenEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    bool PrepareRuntimesForTag(const FString& Tag);

private:
    void HandlePostEngineInit();
    void InitializeManagedLifecycle();
    void HandleWorldInitialized(UWorld* World, const UWorld::InitializationValues InitializationValues);
    void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
    void HandleEditorSettingsChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);
    void HandleProjectSettingsChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);
    void ReconcileManagedLifecycle();
    void StartManaged();
    void StopManaged();
    void InitializeContentBrowserIntegration();
    void RegisterProjectSettingsCustomization();
    void UnregisterProjectSettingsCustomization();
    void StartRuntimeBootstrap();
    void ContinueRuntimeBootstrap();
    void FinishRuntimeBootstrap(bool bSucceeded, const FText& Message);
    void HandleRuntimeInstallProgress(int64 BytesReceived, int64 TotalBytes);
    void HandleRuntimeInstallComplete(bool bSucceeded, const FString& InstalledTag,
        ETextGenLlamacppBackend Backend, ETextGenLlamacppInstallComponent Component, const FText& Message);

    TStrongObjectPtr<UTextGenContentBrowserDataSource> ContentBrowserDataSource;
    FDelegateHandle PostEngineInitHandle;
    FDelegateHandle WorldInitializedHandle;
    FDelegateHandle WorldCleanupHandle;
    FDelegateHandle EditorSettingsChangedHandle;
    FDelegateHandle ProjectSettingsChangedHandle;
    FDelegateHandle RuntimeProgressHandle;
    FDelegateHandle RuntimeCompleteHandle;
    TWeakPtr<SNotificationItem> RuntimeInstallNotification;
    FString RuntimeBootstrapTag;
    TArray<ETextGenLlamacppBackend> RuntimeBootstrapBackends;
    int32 RuntimeBootstrapBackendIndex = 0;
    ETextGenLlamacppBackend RuntimeBootstrapBackend = ETextGenLlamacppBackend::CUDA13;
    int32 ActivePIEWorldCount = 0;
};
