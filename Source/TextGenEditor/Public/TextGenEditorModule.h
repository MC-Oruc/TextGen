#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "TextGen/TextGenEnums.h"
#include "UObject/StrongObjectPtr.h"

class UTextGenContentBrowserDataSource;
class SNotificationItem;

class FTextGenEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void HandlePostEngineInit();
    void InitializeContentBrowserIntegration();
    void StartRuntimeBootstrap();
    void HandleRuntimeInstallProgress(int64 BytesReceived, int64 TotalBytes);
    void HandleRuntimeInstallComplete(bool bSucceeded, const FString& InstalledTag,
        ETextGenLlamacppBackend Backend, ETextGenLlamacppInstallComponent Component, const FText& Message);

    TStrongObjectPtr<UTextGenContentBrowserDataSource> ContentBrowserDataSource;
    FDelegateHandle PostEngineInitHandle;
    FDelegateHandle RuntimeProgressHandle;
    FDelegateHandle RuntimeCompleteHandle;
    TWeakPtr<SNotificationItem> RuntimeInstallNotification;
    FString RuntimeBootstrapTag;
    ETextGenLlamacppBackend RuntimeBootstrapBackend = ETextGenLlamacppBackend::CUDA13;
};
