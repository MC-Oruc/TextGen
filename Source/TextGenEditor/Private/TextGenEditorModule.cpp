#include "TextGenEditorModule.h"

#include "ContentBrowserDataSubsystem.h"
#include "ContentBrowserFileDataCore.h"
#include "ContentBrowserFileDataPayload.h"
#include "Engine/Engine.h"
#include "Editor.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "TextGenContentBrowserDataSource.h"
#include "TextGen/TextGenLocalServiceSubsystem.h"
#include "TextGen/TextGenProjectSettings.h"
#include "TextGen/TextGenRuntimeInstallerSubsystem.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "TextGenEditor"

namespace
{
    bool CanRevealFile(const FName, const FString& Filename, FText*)
    {
        return FPaths::FileExists(Filename);
    }

    bool RevealFile(const FName, const FString& Filename)
    {
        FPlatformProcess::ExploreFolder(*Filename);
        return true;
    }

    bool CachePassesFilter(
        const FName FilePath,
        const FString& Filename,
        const FContentBrowserDataFilter& Filter)
    {
        const FString CleanFilename = FPaths::GetCleanFilename(Filename);
        if (CleanFilename.EndsWith(TEXT(".next.bin"), ESearchCase::IgnoreCase)
            || CleanFilename.EndsWith(TEXT(".work.bin"), ESearchCase::IgnoreCase))
        {
            return false;
        }
        return ContentBrowserFileData::FDefaultFileActions::ItemPassesFilter(
            FilePath, Filename, Filter, true);
    }

}

void FTextGenEditorModule::StartupModule()
{
    InitializeContentBrowserIntegration();
    if (GEngine)
    {
        HandlePostEngineInit();
    }
    else
    {
        PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(
            this, &FTextGenEditorModule::HandlePostEngineInit);
    }
}

void FTextGenEditorModule::ShutdownModule()
{
    FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
    if (GEngine)
    {
        if (UTextGenRuntimeInstallerSubsystem* Installer =
            GEngine->GetEngineSubsystem<UTextGenRuntimeInstallerSubsystem>())
        {
            Installer->OnProgressNative().Remove(RuntimeProgressHandle);
            Installer->OnCompleteNative().Remove(RuntimeCompleteHandle);
        }
    }
    if (UObjectInitialized() && GEditor && ContentBrowserDataSource.IsValid())
    {
        GEditor->GetEditorSubsystem<UContentBrowserDataSubsystem>()->DeactivateDataSource(
            ContentBrowserDataSource->GetFName());
    }
    ContentBrowserDataSource.Reset();
    RuntimeInstallNotification.Reset();
}

void FTextGenEditorModule::HandlePostEngineInit()
{
    FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
    PostEngineInitHandle.Reset();
    StartRuntimeBootstrap();
}

void FTextGenEditorModule::StartRuntimeBootstrap()
{
    if (!GEngine || IsRunningCommandlet())
    {
        return;
    }
    UTextGenRuntimeInstallerSubsystem* Installer =
        GEngine->GetEngineSubsystem<UTextGenRuntimeInstallerSubsystem>();
    const FTextGenLlamacppConfig& Config = GetDefault<UTextGenProjectSettings>()->DevelopmentDefaults;
    if (!Installer || Config.RuntimeTag.IsEmpty())
    {
        return;
    }

    RuntimeBootstrapTag = Config.RuntimeTag;
    RuntimeBootstrapBackends = {
        ETextGenLlamacppBackend::CUDA13,
        ETextGenLlamacppBackend::CUDA12,
        ETextGenLlamacppBackend::Vulkan
    };
    RuntimeBootstrapBackendIndex = 0;
    const bool bRequiresPreparation = RuntimeBootstrapBackends.ContainsByPredicate(
        [Installer, this](const ETextGenLlamacppBackend Backend)
        {
            return !Installer->IsRuntimeInstalled(RuntimeBootstrapTag, Backend);
        });
    if (!bRequiresPreparation)
    {
        return;
    }

    RuntimeProgressHandle = Installer->OnProgressNative().AddRaw(
        this, &FTextGenEditorModule::HandleRuntimeInstallProgress);
    RuntimeCompleteHandle = Installer->OnCompleteNative().AddRaw(
        this, &FTextGenEditorModule::HandleRuntimeInstallComplete);

    FNotificationInfo NotificationInfo(LOCTEXT(
        "RuntimeBootstrapStarted", "Preparing distributable llama.cpp runtimes..."));
    NotificationInfo.bFireAndForget = false;
    NotificationInfo.bUseThrobber = true;
    RuntimeInstallNotification = FSlateNotificationManager::Get().AddNotification(NotificationInfo);
    ContinueRuntimeBootstrap();
}

void FTextGenEditorModule::ContinueRuntimeBootstrap()
{
    UTextGenRuntimeInstallerSubsystem* Installer = GEngine
        ? GEngine->GetEngineSubsystem<UTextGenRuntimeInstallerSubsystem>() : nullptr;
    if (!Installer)
    {
        FinishRuntimeBootstrap(false,
            LOCTEXT("RuntimeBootstrapUnavailable", "The llama.cpp runtime installer is unavailable."));
        return;
    }
    while (RuntimeBootstrapBackends.IsValidIndex(RuntimeBootstrapBackendIndex)
        && Installer->IsRuntimeInstalled(
            RuntimeBootstrapTag, RuntimeBootstrapBackends[RuntimeBootstrapBackendIndex]))
    {
        ++RuntimeBootstrapBackendIndex;
    }
    if (!RuntimeBootstrapBackends.IsValidIndex(RuntimeBootstrapBackendIndex))
    {
        FinishRuntimeBootstrap(true,
            LOCTEXT("RuntimeBootstrapComplete", "Distributable llama.cpp runtimes are ready."));
        return;
    }

    RuntimeBootstrapBackend = RuntimeBootstrapBackends[RuntimeBootstrapBackendIndex];
    const ETextGenLlamacppInstallComponent Component =
        RuntimeBootstrapBackend != ETextGenLlamacppBackend::Vulkan
        && !Installer->AreCudaDependenciesInstalled(RuntimeBootstrapBackend)
            ? ETextGenLlamacppInstallComponent::CudaDependencies
            : ETextGenLlamacppInstallComponent::Runtime;
    if (Installer->IsInstalling()
        || !Installer->InstallExact(RuntimeBootstrapTag, RuntimeBootstrapBackend, Component))
    {
        FinishRuntimeBootstrap(false,
            LOCTEXT("RuntimeBootstrapStartFailed", "The llama.cpp runtime download could not start."));
    }
}

void FTextGenEditorModule::HandleRuntimeInstallProgress(const int64 BytesReceived, const int64 TotalBytes)
{
    if (const TSharedPtr<SNotificationItem> Notification = RuntimeInstallNotification.Pin())
    {
        const double Fraction = TotalBytes > 0
            ? FMath::Clamp(static_cast<double>(BytesReceived) / static_cast<double>(TotalBytes), 0.0, 1.0)
            : 0.0;
        Notification->SetText(FText::Format(
            LOCTEXT("RuntimeBootstrapProgress", "Preparing distributable llama.cpp runtimes: {0}"),
            FText::AsPercent(Fraction)));
    }
}

void FTextGenEditorModule::HandleRuntimeInstallComplete(const bool bSucceeded, const FString&,
    const ETextGenLlamacppBackend, const ETextGenLlamacppInstallComponent Component, const FText& Message)
{
    UTextGenRuntimeInstallerSubsystem* Installer = GEngine
        ? GEngine->GetEngineSubsystem<UTextGenRuntimeInstallerSubsystem>() : nullptr;
    if (!bSucceeded || !Installer)
    {
        FinishRuntimeBootstrap(false, Message);
        return;
    }
    if (Component == ETextGenLlamacppInstallComponent::CudaDependencies)
    {
        if (const TSharedPtr<SNotificationItem> Notification = RuntimeInstallNotification.Pin())
        {
            Notification->SetText(LOCTEXT(
                "RuntimeBootstrapCore", "CUDA dependencies installed. Installing llama.cpp runtime..."));
        }
        if (Installer->InstallExact(RuntimeBootstrapTag, RuntimeBootstrapBackend,
            ETextGenLlamacppInstallComponent::Runtime))
        {
            return;
        }
        HandleRuntimeInstallComplete(false, FString(), RuntimeBootstrapBackend,
            ETextGenLlamacppInstallComponent::Runtime,
            LOCTEXT("RuntimeBootstrapCoreFailed", "The llama.cpp runtime download could not start."));
        return;
    }
    ++RuntimeBootstrapBackendIndex;
    ContinueRuntimeBootstrap();
}

void FTextGenEditorModule::FinishRuntimeBootstrap(const bool bSucceeded, const FText& Message)
{
    UTextGenRuntimeInstallerSubsystem* Installer = GEngine
        ? GEngine->GetEngineSubsystem<UTextGenRuntimeInstallerSubsystem>() : nullptr;
    if (Installer)
    {
        Installer->OnProgressNative().Remove(RuntimeProgressHandle);
        Installer->OnCompleteNative().Remove(RuntimeCompleteHandle);
    }
    RuntimeProgressHandle.Reset();
    RuntimeCompleteHandle.Reset();
    if (const TSharedPtr<SNotificationItem> Notification = RuntimeInstallNotification.Pin())
    {
        Notification->SetText(Message);
        Notification->SetCompletionState(bSucceeded
            ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
        Notification->ExpireAndFadeout();
    }
    if (bSucceeded && GEngine)
    {
        const UTextGenProjectSettings* Settings = GetDefault<UTextGenProjectSettings>();
        if (Settings->EditorLifecycle != ETextGenManagedLifecycleMode::Manual
            && !Settings->DevelopmentDefaults.bManagedDisabled)
        {
            if (UTextGenLocalServiceSubsystem* Service =
                GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>())
            {
                FString StartError;
                Service->StartManaged(Settings->DevelopmentDefaults, StartError);
            }
        }
    }
    RuntimeInstallNotification.Reset();
}

void FTextGenEditorModule::InitializeContentBrowserIntegration()
{
    if (ContentBrowserDataSource.IsValid() || !GEditor || IsRunningCommandlet())
    {
        return;
    }

    ContentBrowserFileData::FFileConfigData Config;
    ContentBrowserFileData::FDirectoryActions DirectoryActions;
    DirectoryActions.PassesFilter.BindStatic(
        &ContentBrowserFileData::FDefaultFileActions::ItemPassesFilter, false);
    DirectoryActions.GetAttribute.BindStatic(
        &ContentBrowserFileData::FDefaultFileActions::GetItemAttribute);
    Config.SetDirectoryActions(DirectoryActions);

    ContentBrowserFileData::FFileActions ModelActions;
    ModelActions.TypeExtension = TEXT("gguf");
    ModelActions.TypeName = FTopLevelAssetPath(TEXT("/Script/TextGenEditor.GGUFModel"));
    ModelActions.TypeDisplayName = LOCTEXT("GGUFTypeName", "GGUF Model");
    ModelActions.TypeShortDescription = LOCTEXT("GGUFTypeShortDescription", "GGUF language model");
    ModelActions.TypeFullDescription = LOCTEXT("GGUFTypeFullDescription", "A GGUF language model file");
    ModelActions.TypeColor = FColor(66, 165, 245);
    ModelActions.DefaultEditVerb = ELaunchVerb::Edit;
    ModelActions.PassesFilter.BindStatic(
        &ContentBrowserFileData::FDefaultFileActions::ItemPassesFilter, true);
    ModelActions.GetAttribute.BindStatic(&ContentBrowserFileData::FDefaultFileActions::GetItemAttribute);
    ModelActions.CanEdit.BindStatic(&CanRevealFile);
    ModelActions.Edit.BindStatic(&RevealFile);
    ModelActions.CanPreview.BindStatic(&CanRevealFile);
    ModelActions.Preview.BindStatic(&RevealFile);
    Config.RegisterFileActions(ModelActions);

    ContentBrowserFileData::FFileActions CacheActions;
    CacheActions.TypeExtension = TEXT("bin");
    CacheActions.TypeName = FTopLevelAssetPath(TEXT("/Script/TextGenEditor.LlamaConversationCache"));
    CacheActions.TypeDisplayName = LOCTEXT("CacheTypeName", "llama.cpp Conversation Cache");
    CacheActions.TypeShortDescription = LOCTEXT("CacheTypeShortDescription", "llama.cpp conversation cache");
    CacheActions.TypeFullDescription = LOCTEXT("CacheTypeFullDescription", "A llama.cpp conversation slot cache");
    CacheActions.TypeColor = FColor(102, 187, 106);
    CacheActions.DefaultEditVerb = ELaunchVerb::Edit;
    CacheActions.PassesFilter.BindStatic(&CachePassesFilter);
    CacheActions.GetAttribute.BindStatic(&ContentBrowserFileData::FDefaultFileActions::GetItemAttribute);
    CacheActions.CanEdit.BindStatic(&CanRevealFile);
    CacheActions.Edit.BindStatic(&RevealFile);
    CacheActions.CanPreview.BindStatic(&CanRevealFile);
    CacheActions.Preview.BindStatic(&RevealFile);
    Config.RegisterFileActions(CacheActions);

    ContentBrowserDataSource.Reset(NewObject<UTextGenContentBrowserDataSource>(
        GetTransientPackage(), TEXT("TextGenFileDataSource")));
    ContentBrowserDataSource->Initialize(Config);
    FString ProjectContentPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
    FPaths::NormalizeDirectoryName(ProjectContentPath);
    ContentBrowserDataSource->AddFileMount(TEXT("/Game"), ProjectContentPath);
    GEditor->GetEditorSubsystem<UContentBrowserDataSubsystem>()->ActivateDataSource(
        ContentBrowserDataSource->GetFName());
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FTextGenEditorModule, TextGenEditor)
