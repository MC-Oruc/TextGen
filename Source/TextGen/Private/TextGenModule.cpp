#include "TextGenModule.h"

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "TextGen/TextGenLocalServiceSubsystem.h"
#include "TextGen/TextGenProjectSettings.h"

namespace
{
    UTextGenLocalServiceSubsystem* GetLocalService()
    {
        return GEngine ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
    }
}

void FTextGenModule::StartupModule()
{
    StartCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("TextGen.ManagedProcess.Start"), TEXT("Start managed llama.cpp."),
        FConsoleCommandDelegate::CreateRaw(this, &FTextGenModule::StartManaged));
    StopCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("TextGen.ManagedProcess.Stop"), TEXT("Stop managed llama.cpp."),
        FConsoleCommandDelegate::CreateRaw(this, &FTextGenModule::StopManaged));
    RestartCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("TextGen.ManagedProcess.Restart"), TEXT("Restart managed llama.cpp."),
        FConsoleCommandDelegate::CreateRaw(this, &FTextGenModule::RestartManaged));
}

void FTextGenModule::ShutdownModule()
{
    StopManaged();
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

IMPLEMENT_MODULE(FTextGenModule, TextGen)
