#include "TextGenLlamacppRuntimePaths.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

namespace TextGenLlamacppRuntimePaths
{
    FString GetBackendDirectoryName(const ETextGenLlamacppBackend Backend)
    {
        switch (Backend)
        {
        case ETextGenLlamacppBackend::CUDA12: return TEXT("cuda-12.4");
        case ETextGenLlamacppBackend::Vulkan: return TEXT("vulkan");
        case ETextGenLlamacppBackend::CUDA13:
        default: return TEXT("cuda-13.3");
        }
    }

    FString GetWritableRoot()
    {
        return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TextGen/Runtimes/Llamacpp/Win64"));
    }

    FString GetWritableRuntimeDirectory(const FString& Tag, const ETextGenLlamacppBackend Backend)
    {
        return FPaths::Combine(GetWritableRoot(), GetBackendDirectoryName(Backend), Tag);
    }

    FString GetShippedRuntimeDirectory(const FString& Tag, const ETextGenLlamacppBackend Backend)
    {
        return FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("TextGen/Runtimes/Llamacpp/Win64"),
            GetBackendDirectoryName(Backend), Tag);
    }

    bool HasRuntimeInventory(const FString& Directory, const ETextGenLlamacppBackend Backend)
    {
        if (!FPaths::FileExists(FPaths::Combine(Directory, TEXT("llama-server.exe"))))
        {
            return false;
        }
        if (Backend == ETextGenLlamacppBackend::Vulkan)
        {
            return FPaths::FileExists(FPaths::Combine(Directory, TEXT("ggml-vulkan.dll")));
        }
        TArray<FString> Files;
        IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, TEXT("*.dll")), true, false);
        return Files.ContainsByPredicate([](const FString& Name)
            { return Name.StartsWith(TEXT("cudart"), ESearchCase::IgnoreCase); })
            && Files.ContainsByPredicate([](const FString& Name)
            { return Name.StartsWith(TEXT("cublas"), ESearchCase::IgnoreCase); });
    }

    bool ResolveRuntimeDirectory(const FString& Tag, const ETextGenLlamacppBackend Backend,
        FString& OutDirectory)
    {
        if (Tag.IsEmpty())
        {
            return false;
        }
        const FString Writable = GetWritableRuntimeDirectory(Tag, Backend);
        if (HasRuntimeInventory(Writable, Backend))
        {
            OutDirectory = Writable;
            return true;
        }
        const FString Shipped = GetShippedRuntimeDirectory(Tag, Backend);
        if (HasRuntimeInventory(Shipped, Backend))
        {
            OutDirectory = Shipped;
            return true;
        }
        return false;
    }
}
