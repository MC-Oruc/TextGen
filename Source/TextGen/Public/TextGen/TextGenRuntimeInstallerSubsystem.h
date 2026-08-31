// Copyright <--\, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Subsystems/EngineSubsystem.h"
#include "TextGen/TextGenEnums.h"
#include "TextGenRuntimeInstallerSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTextGenRuntimeInstallProgress, int64, BytesReceived, int64, TotalBytes);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FOnTextGenRuntimeInstallComplete, bool, bSucceeded,
    const FString&, InstalledTag, ETextGenLlamacppBackend, Backend,
    ETextGenLlamacppInstallComponent, Component, const FText&, Message);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnTextGenRuntimeInstallProgressNative, int64, int64);
DECLARE_MULTICAST_DELEGATE_FiveParams(FOnTextGenRuntimeInstallCompleteNative, bool, const FString&,
    ETextGenLlamacppBackend, ETextGenLlamacppInstallComponent, const FText&);

UCLASS(BlueprintType)
class TEXTGEN_API UTextGenRuntimeInstallerSubsystem final : public UEngineSubsystem
{
    GENERATED_BODY()

public:
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category = "TextGen|Runtime Installer")
    bool InstallLatest(ETextGenLlamacppBackend Backend, ETextGenLlamacppInstallComponent Component);

    UFUNCTION(BlueprintCallable, Category = "TextGen|Runtime Installer")
    bool InstallExact(const FString& Tag, ETextGenLlamacppBackend Backend,
        ETextGenLlamacppInstallComponent Component);

    UFUNCTION(BlueprintPure, Category = "TextGen|Runtime Installer")
    bool IsInstalling() const { return bInstalling; }

    UFUNCTION(BlueprintPure, Category = "TextGen|Runtime Installer")
    FString GetPendingTag() const { return PendingTag; }

    UFUNCTION(BlueprintPure, Category = "TextGen|Runtime Installer")
    bool IsRuntimeInstalled(const FString& Tag, ETextGenLlamacppBackend Backend) const;

    UFUNCTION(BlueprintPure, Category = "TextGen|Runtime Installer")
    bool AreCudaDependenciesInstalled(ETextGenLlamacppBackend Backend) const;

    FOnTextGenRuntimeInstallProgressNative& OnProgressNative() { return ProgressNative; }
    FOnTextGenRuntimeInstallCompleteNative& OnCompleteNative() { return CompleteNative; }

    UPROPERTY(BlueprintAssignable, Category = "TextGen|Runtime Installer")
    FOnTextGenRuntimeInstallProgress OnProgress;

    UPROPERTY(BlueprintAssignable, Category = "TextGen|Runtime Installer")
    FOnTextGenRuntimeInstallComplete OnComplete;

private:
    struct FPendingAsset
    {
        FString Name;
        FString URL;
        int64 Size = 0;
        bool bCudaDependency = false;
        FString DownloadPath;
    };

    bool BeginReleaseRequest(const FString& Endpoint);
    bool RequestRelease(const FString& Endpoint);
    void ReconcileInstallerArtifacts(ETextGenLlamacppBackend Backend);
    void CleanupPendingDownloads();
    void HandleReleaseResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
    void HandleStableAssetTagResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
    bool BeginNextDownload();
    void HandleDownloadProgress(FHttpRequestPtr Request, uint64 BytesSent, uint64 BytesReceived);
    void HandleDownloadResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
    bool ExtractAndPublish(FString& OutError);
    void Finish(bool bSucceeded, const FString& Error = FString());

    bool bInstalling = false;
    FString RequestedTag;
    FString PendingTag;
    FString PendingAssetTag;
    bool bResolvingStableAssets = false;
    ETextGenLlamacppBackend PendingBackend = ETextGenLlamacppBackend::CUDA13;
    ETextGenLlamacppInstallComponent PendingComponent = ETextGenLlamacppInstallComponent::Runtime;
    TArray<FPendingAsset> PendingAssets;
    FString PendingDownloadDirectory;
    int32 ActiveAssetIndex = INDEX_NONE;
    FHttpRequestPtr ActiveRequest;
    TSharedPtr<FArchive> ActiveDownloadStream;
    FOnTextGenRuntimeInstallProgressNative ProgressNative;
    FOnTextGenRuntimeInstallCompleteNative CompleteNative;
};
