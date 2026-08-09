#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "Modules/ModuleManager.h"

class IConsoleObject;
struct FPropertyChangedEvent;

class FTextGenModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void HandlePostEngineInit();
    void HandleWorldInitialized(UWorld* World, const UWorld::InitializationValues InitializationValues);
    void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
    void StartManaged();
    void StopManaged();
    void RestartManaged();
#if WITH_EDITOR
    void HandleProjectSettingsChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);
#endif

    FDelegateHandle PostEngineInitHandle;
    FDelegateHandle WorldInitializedHandle;
    FDelegateHandle WorldCleanupHandle;
    FDelegateHandle ProjectSettingsChangedHandle;
    IConsoleObject* StartCommand = nullptr;
    IConsoleObject* StopCommand = nullptr;
    IConsoleObject* RestartCommand = nullptr;
    int32 ActivePIEWorldCount = 0;
};
