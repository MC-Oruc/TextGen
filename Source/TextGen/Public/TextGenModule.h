#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class IConsoleObject;

class FTextGenModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void StartManaged();
    void StopManaged();
    void RestartManaged();

    IConsoleObject* StartCommand = nullptr;
    IConsoleObject* StopCommand = nullptr;
    IConsoleObject* RestartCommand = nullptr;
};
