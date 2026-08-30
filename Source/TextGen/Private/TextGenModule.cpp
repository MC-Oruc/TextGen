#include "TextGenModule.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CoreDelegates.h"
#include "TextGen/TextGenLocalServiceSubsystem.h"
#include "TextGen/TextGenProjectSettings.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
    UTextGenLocalServiceSubsystem* GetLocalService()
    {
        return GEngine ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
    }
}

void FTextGenModule::StartupModule()
{
    PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FTextGenModule::HandlePostEngineInit);
    WorldInitializedHandle = FWorldDelegates::OnPostWorldInitialization.AddRaw(
        this, &FTextGenModule::HandleWorldInitialized);
    WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(this, &FTextGenModule::HandleWorldCleanup);

    StartCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("TextGen.ManagedProcess.Start"), TEXT("Start managed llama.cpp."),
        FConsoleCommandDelegate::CreateRaw(this, &FTextGenModule::StartManaged));
    StopCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("TextGen.ManagedProcess.Stop"), TEXT("Stop managed llama.cpp."),
        FConsoleCommandDelegate::CreateRaw(this, &FTextGenModule::StopManaged));
    RestartCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("TextGen.ManagedProcess.Restart"), TEXT("Restart managed llama.cpp."),
        FConsoleCommandDelegate::CreateRaw(this, &FTextGenModule::RestartManaged));

    if (GEngine)
    {
        HandlePostEngineInit();
    }
}

void FTextGenModule::ShutdownModule()
{
    StopManaged();
    FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
    FWorldDelegates::OnPostWorldInitialization.Remove(WorldInitializedHandle);
    FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
#if WITH_EDITOR
    if (UObjectInitialized())
    {
        GetMutableDefault<UTextGenProjectSettings>()->OnSettingChanged().Remove(ProjectSettingsChangedHandle);
    }
#endif
    if (StartCommand)
    {
        IConsoleManager::Get().UnregisterConsoleObject(StartCommand);
        StartCommand = nullptr;
    }
    if (StopCommand)
    {
        IConsoleManager::Get().UnregisterConsoleObject(StopCommand);
        StopCommand = nullptr;
    }
    if (RestartCommand)
    {
        IConsoleManager::Get().UnregisterConsoleObject(RestartCommand);
        RestartCommand = nullptr;
    }
}

void FTextGenModule::HandlePostEngineInit()
{
    if (!GIsEditor || IsRunningCommandlet())
    {
        return;
    }
#if WITH_EDITOR
    UTextGenProjectSettings* Settings = GetMutableDefault<UTextGenProjectSettings>();
    if (!ProjectSettingsChangedHandle.IsValid())
    {
        ProjectSettingsChangedHandle = Settings->OnSettingChanged().AddRaw(
            this, &FTextGenModule::HandleProjectSettingsChanged);
    }
#endif
    if (GetDefault<UTextGenProjectSettings>()->EditorLifecycle == ETextGenManagedLifecycleMode::EditorSession)
    {
        StartManaged();
    }
}

void FTextGenModule::HandleWorldInitialized(
    UWorld* World,
    const UWorld::InitializationValues)
{
    if (!World || World->WorldType != EWorldType::PIE)
    {
        return;
    }
    ++ActivePIEWorldCount;
    if (ActivePIEWorldCount == 1
        && GetDefault<UTextGenProjectSettings>()->EditorLifecycle == ETextGenManagedLifecycleMode::PIESession)
    {
        StartManaged();
    }
}

void FTextGenModule::HandleWorldCleanup(UWorld* World, const bool, const bool)
{
    if (!World || World->WorldType != EWorldType::PIE)
    {
        return;
    }
    ActivePIEWorldCount = FMath::Max(0, ActivePIEWorldCount - 1);
    if (ActivePIEWorldCount == 0
        && GetDefault<UTextGenProjectSettings>()->EditorLifecycle == ETextGenManagedLifecycleMode::PIESession)
    {
        StopManaged();
    }
}

void FTextGenModule::StartManaged()
{
    if (UTextGenLocalServiceSubsystem* Service = GetLocalService())
    {
        FString Error;
        Service->StartManaged(GetDefault<UTextGenProjectSettings>()->DevelopmentDefaults, Error);
    }
}

void FTextGenModule::StopManaged()
{
    if (UTextGenLocalServiceSubsystem* Service = GetLocalService())
    {
        FString Error;
        Service->StopManaged(Error);
    }
}

void FTextGenModule::RestartManaged()
{
    if (UTextGenLocalServiceSubsystem* Service = GetLocalService())
    {
        FString Error;
        Service->RestartManaged(GetDefault<UTextGenProjectSettings>()->DevelopmentDefaults, Error);
    }
}

#if WITH_EDITOR
void FTextGenModule::HandleProjectSettingsChanged(UObject*, FPropertyChangedEvent& PropertyChangedEvent)
{
    if (PropertyChangedEvent.GetPropertyName()
        != GET_MEMBER_NAME_CHECKED(UTextGenProjectSettings, EditorLifecycle))
    {
        return;
    }

    switch (GetDefault<UTextGenProjectSettings>()->EditorLifecycle)
    {
    case ETextGenManagedLifecycleMode::EditorSession:
        StartManaged();
        break;
    case ETextGenManagedLifecycleMode::PIESession:
        if (ActivePIEWorldCount > 0)
        {
            StartManaged();
        }
        else
        {
            StopManaged();
        }
        break;
    case ETextGenManagedLifecycleMode::Manual:
        StopManaged();
        break;
    }
}
#endif

IMPLEMENT_MODULE(FTextGenModule, TextGen)
