#include "TextGen/TextGenRuntimeInstallerSubsystem.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TextGen/TextGenLog.h"
#include "TextGenLlamacppRuntimePaths.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

THIRD_PARTY_INCLUDES_START
#include <libzip/zip.h>
THIRD_PARTY_INCLUDES_END

namespace
{
    const TCHAR* ReleasesAPI = TEXT("https://api.github.com/repos/ggml-org/llama.cpp/releases");

    bool HashFileSha256(const FString& Path, FString& OutHash)
    {
#if PLATFORM_WINDOWS
        BCRYPT_ALG_HANDLE Algorithm = nullptr;
        BCRYPT_HASH_HANDLE Hash = nullptr;
        DWORD ObjectSize = 0;
        DWORD ResultSize = 0;
        TArray<uint8> HashObject;
        uint8 Digest[32];
        bool bSucceeded = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        if (bSucceeded)
        {
            bSucceeded = BCRYPT_SUCCESS(BCryptGetProperty(Algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&ObjectSize), sizeof(ObjectSize), &ResultSize, 0));
        }
        if (bSucceeded)
        {
            HashObject.SetNumUninitialized(ObjectSize);
            bSucceeded = BCRYPT_SUCCESS(BCryptCreateHash(Algorithm, &Hash,
                HashObject.GetData(), HashObject.Num(), nullptr, 0, 0));
        }
        TUniquePtr<FArchive> Input(IFileManager::Get().CreateFileReader(*Path));
        bSucceeded = bSucceeded && Input.IsValid();
        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(1024 * 1024);
        while (bSucceeded && !Input->AtEnd())
        {
            const int64 Remaining = Input->TotalSize() - Input->Tell();
            const int32 ReadSize = static_cast<int32>(FMath::Min<int64>(Buffer.Num(), Remaining));
            Input->Serialize(Buffer.GetData(), ReadSize);
            bSucceeded = !Input->IsError()
                && BCRYPT_SUCCESS(BCryptHashData(Hash, Buffer.GetData(), ReadSize, 0));
        }
        if (bSucceeded)
        {
            bSucceeded = BCRYPT_SUCCESS(BCryptFinishHash(Hash, Digest, sizeof(Digest), 0));
        }
        if (Hash) BCryptDestroyHash(Hash);
        if (Algorithm) BCryptCloseAlgorithmProvider(Algorithm, 0);
        if (!bSucceeded)
        {
            return false;
        }
        OutHash = BytesToHex(Digest, sizeof(Digest)).ToLower();
        return true;
#else
        return false;
#endif
    }

    bool IsSafeArchivePath(const FString& Path)
    {
        FString Normalized = Path;
        Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
        return !Normalized.IsEmpty()
            && !Normalized.StartsWith(TEXT("/"))
            && !Normalized.Contains(TEXT(":"))
            && !Normalized.Equals(TEXT(".."))
            && !Normalized.StartsWith(TEXT("../"))
            && !Normalized.Contains(TEXT("/../"));
    }

    FString RuntimeAssetName(const FString& Tag, const ETextGenLlamacppBackend Backend)
    {
        if (Backend == ETextGenLlamacppBackend::Vulkan)
        {
            return FString::Printf(TEXT("llama-%s-bin-win-vulkan-x64.zip"), *Tag);
        }
        return FString::Printf(TEXT("llama-%s-bin-win-%s-x64.zip"), *Tag,
            *TextGenLlamacppRuntimePaths::GetBackendDirectoryName(Backend));
    }

    FString CudaDependencyAssetName(const ETextGenLlamacppBackend Backend)
    {
        return FString::Printf(TEXT("cudart-llama-bin-win-%s-x64.zip"),
            *TextGenLlamacppRuntimePaths::GetBackendDirectoryName(Backend));
    }
}

void UTextGenRuntimeInstallerSubsystem::Deinitialize()
{
    if (ActiveRequest.IsValid())
    {
        ActiveRequest->CancelRequest();
    }
    ActiveRequest.Reset();
    ActiveDownloadStream.Reset();
    Super::Deinitialize();
}

bool UTextGenRuntimeInstallerSubsystem::IsRuntimeInstalled(
    const FString& Tag, const ETextGenLlamacppBackend Backend) const
{
    FString RuntimeDirectory;
    return TextGenLlamacppRuntimePaths::ResolveRuntimeDirectory(Tag, Backend, RuntimeDirectory);
}

bool UTextGenRuntimeInstallerSubsystem::AreCudaDependenciesInstalled(
    const ETextGenLlamacppBackend Backend) const
{
    if (Backend == ETextGenLlamacppBackend::Vulkan)
    {
        return true;
    }
    const FString Cache = FPaths::Combine(
        TextGenLlamacppRuntimePaths::GetWritableRoot(), TEXT("_Dependencies"),
        TextGenLlamacppRuntimePaths::GetBackendDirectoryName(Backend));
    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *FPaths::Combine(Cache, TEXT("*.dll")), true, false);
    return Files.ContainsByPredicate([](const FString& Name)
        { return Name.StartsWith(TEXT("cudart"), ESearchCase::IgnoreCase); })
        && Files.ContainsByPredicate([](const FString& Name)
        { return Name.StartsWith(TEXT("cublas"), ESearchCase::IgnoreCase); });
}

bool UTextGenRuntimeInstallerSubsystem::InstallLatest(const ETextGenLlamacppBackend Backend,
    const ETextGenLlamacppInstallComponent Component)
{
    if (Backend == ETextGenLlamacppBackend::Vulkan && Component == ETextGenLlamacppInstallComponent::CudaDependencies)
    {
        Finish(false, TEXT("Vulkan has no CUDA dependency component."));
        return false;
    }
    PendingBackend = Backend;
    PendingComponent = Component;
    RequestedTag.Reset();
    return BeginReleaseRequest(FString(ReleasesAPI) + TEXT("/latest"));
}

bool UTextGenRuntimeInstallerSubsystem::InstallExact(const FString& Tag,
    const ETextGenLlamacppBackend Backend, const ETextGenLlamacppInstallComponent Component)
{
    FString CleanTag = Tag;
    CleanTag.TrimStartAndEndInline();
    if (CleanTag.IsEmpty() || CleanTag.Contains(TEXT("/")) || CleanTag.Contains(TEXT("\\")))
    {
        Finish(false, TEXT("Invalid llama.cpp release tag."));
        return false;
    }
    if (Backend == ETextGenLlamacppBackend::Vulkan && Component == ETextGenLlamacppInstallComponent::CudaDependencies)
    {
        Finish(false, TEXT("Vulkan has no CUDA dependency component."));
        return false;
    }
    PendingBackend = Backend;
    PendingComponent = Component;
    RequestedTag = CleanTag;
    return BeginReleaseRequest(FString::Printf(TEXT("%s/tags/%s"), ReleasesAPI, *FGenericPlatformHttp::UrlEncode(CleanTag)));
}

bool UTextGenRuntimeInstallerSubsystem::BeginReleaseRequest(const FString& Endpoint)
{
    if (bInstalling)
    {
        return false;
    }
    bInstalling = true;
    PendingTag.Reset();
    PendingAssets.Reset();
    ActiveAssetIndex = INDEX_NONE;
    ActiveRequest = FHttpModule::Get().CreateRequest();
    ActiveRequest->SetURL(Endpoint);
    ActiveRequest->SetVerb(TEXT("GET"));
    ActiveRequest->SetHeader(TEXT("Accept"), TEXT("application/vnd.github+json"));
    ActiveRequest->SetHeader(TEXT("User-Agent"), TEXT("SoC-TextGen"));
    ActiveRequest->OnProcessRequestComplete().BindUObject(this, &UTextGenRuntimeInstallerSubsystem::HandleReleaseResponse);
    if (!ActiveRequest->ProcessRequest())
    {
        Finish(false, TEXT("Could not start the llama.cpp release request."));
        return false;
    }
    return true;
}

void UTextGenRuntimeInstallerSubsystem::HandleReleaseResponse(FHttpRequestPtr, FHttpResponsePtr Response, const bool bSucceeded)
{
    if (!bSucceeded || !Response.IsValid() || Response->GetResponseCode() != 200)
    {
        Finish(false, TEXT("The official llama.cpp release could not be resolved."));
        return;
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() || !Root->TryGetStringField(TEXT("tag_name"), PendingTag))
    {
        Finish(false, TEXT("The official llama.cpp release response is invalid."));
        return;
    }
    if (!RequestedTag.IsEmpty() && PendingTag != RequestedTag)
    {
        Finish(false, TEXT("The resolved llama.cpp tag does not match the requested tag."));
        return;
    }
    if (PendingComponent == ETextGenLlamacppInstallComponent::Runtime
        && IsRuntimeInstalled(PendingTag, PendingBackend))
    {
        Finish(true);
        return;
    }

    const FString ExpectedRuntimeName = RuntimeAssetName(PendingTag, PendingBackend);
    const FString ExpectedDependencyName = PendingBackend == ETextGenLlamacppBackend::Vulkan
        ? FString() : CudaDependencyAssetName(PendingBackend);
    const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
    if (!Root->TryGetArrayField(TEXT("assets"), Assets) || !Assets)
    {
        Finish(false, TEXT("The release contains no assets."));
        return;
    }
    TArray<FString> ExpectedNames;
    if (PendingComponent == ETextGenLlamacppInstallComponent::CudaDependencies)
    {
        ExpectedNames.Add(ExpectedDependencyName);
    }
    else
    {
        if (PendingBackend != ETextGenLlamacppBackend::Vulkan
            && !AreCudaDependenciesInstalled(PendingBackend))
        {
            ExpectedNames.Add(ExpectedDependencyName);
        }
        ExpectedNames.Add(ExpectedRuntimeName);
    }

    for (const FString& ExpectedName : ExpectedNames)
    {
        FPendingAsset PendingAsset;
        PendingAsset.Name = ExpectedName;
        PendingAsset.bCudaDependency = ExpectedName == ExpectedDependencyName;
        for (const TSharedPtr<FJsonValue>& Value : *Assets)
        {
            const TSharedPtr<FJsonObject> Asset = Value.IsValid() ? Value->AsObject() : nullptr;
            FString Name;
            FString Digest;
            if (!Asset.IsValid() || !Asset->TryGetStringField(TEXT("name"), Name) || Name != ExpectedName
                || !Asset->TryGetStringField(TEXT("browser_download_url"), PendingAsset.URL)
                || !Asset->TryGetStringField(TEXT("digest"), Digest))
            {
                continue;
            }
            if (!Digest.RemoveFromStart(TEXT("sha256:"), ESearchCase::IgnoreCase) || Digest.Len() != 64)
            {
                Finish(false, TEXT("A required release asset has no valid SHA-256 digest."));
                return;
            }
            PendingAsset.SHA256 = Digest.ToLower();
            double Size = 0;
            Asset->TryGetNumberField(TEXT("size"), Size);
            PendingAsset.Size = static_cast<int64>(Size);
            break;
        }
        if (PendingAsset.URL.IsEmpty() || PendingAsset.SHA256.IsEmpty())
        {
            Finish(false, FString::Printf(TEXT("Required official llama.cpp asset '%s' is missing."), *ExpectedName));
            return;
        }
        PendingAssets.Add(MoveTemp(PendingAsset));
    }
    if (!BeginNextDownload())
    {
        Finish(false, TEXT("Could not start the llama.cpp component download."));
    }
}

bool UTextGenRuntimeInstallerSubsystem::BeginNextDownload()
{
    ++ActiveAssetIndex;
    if (!PendingAssets.IsValidIndex(ActiveAssetIndex))
    {
        FString Error;
        if (!ExtractAndPublish(Error))
        {
            Finish(false, Error);
            return false;
        }
        Finish(true);
        return true;
    }
    ActiveRequest = FHttpModule::Get().CreateRequest();
    ActiveRequest->SetURL(PendingAssets[ActiveAssetIndex].URL);
    ActiveRequest->SetVerb(TEXT("GET"));
    ActiveRequest->SetHeader(TEXT("User-Agent"), TEXT("SoC-TextGen"));
    const FString DownloadDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TextGen/RuntimeDownloads"),
        TextGenLlamacppRuntimePaths::GetBackendDirectoryName(PendingBackend), PendingTag);
    IFileManager::Get().MakeDirectory(*DownloadDirectory, true);
    FPendingAsset& Asset = PendingAssets[ActiveAssetIndex];
    Asset.DownloadPath = FPaths::Combine(DownloadDirectory,
        FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("-") + Asset.Name + TEXT(".partial"));
    ActiveDownloadStream = MakeShareable(IFileManager::Get().CreateFileWriter(*Asset.DownloadPath));
    if (!ActiveDownloadStream.IsValid()
        || !ActiveRequest->SetResponseBodyReceiveStream(ActiveDownloadStream.ToSharedRef()))
    {
        ActiveDownloadStream.Reset();
        return false;
    }
    ActiveRequest->OnRequestProgress64().BindUObject(this, &UTextGenRuntimeInstallerSubsystem::HandleDownloadProgress);
    ActiveRequest->OnProcessRequestComplete().BindUObject(this, &UTextGenRuntimeInstallerSubsystem::HandleDownloadResponse);
    return ActiveRequest->ProcessRequest();
}

void UTextGenRuntimeInstallerSubsystem::HandleDownloadProgress(FHttpRequestPtr, uint64, const uint64 BytesReceived)
{
    int64 CompletedBytes = 0;
    int64 TotalBytes = 0;
    for (int32 Index = 0; Index < PendingAssets.Num(); ++Index)
    {
        TotalBytes += PendingAssets[Index].Size;
        if (Index < ActiveAssetIndex)
        {
            CompletedBytes += PendingAssets[Index].Size;
        }
    }
    OnProgress.Broadcast(CompletedBytes + static_cast<int64>(BytesReceived), TotalBytes);
    ProgressNative.Broadcast(CompletedBytes + static_cast<int64>(BytesReceived), TotalBytes);
}

void UTextGenRuntimeInstallerSubsystem::HandleDownloadResponse(FHttpRequestPtr, FHttpResponsePtr Response, const bool bSucceeded)
{
    ActiveDownloadStream.Reset();
    if (!bSucceeded || !Response.IsValid() || Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
    {
        Finish(false, TEXT("The llama.cpp runtime download failed."));
        return;
    }
    if (!PendingAssets.IsValidIndex(ActiveAssetIndex))
    {
        Finish(false, TEXT("The llama.cpp installer lost its active component."));
        return;
    }
    FPendingAsset& Asset = PendingAssets[ActiveAssetIndex];
    FString ActualHash;
    if (!HashFileSha256(Asset.DownloadPath, ActualHash)
        || !ActualHash.Equals(Asset.SHA256, ESearchCase::IgnoreCase))
    {
        Finish(false, FString::Printf(TEXT("Downloaded component '%s' failed SHA-256 verification."), *Asset.Name));
        return;
    }
    if (!BeginNextDownload() && bInstalling)
    {
        Finish(false, TEXT("Could not start the next llama.cpp component download."));
    }
}

bool UTextGenRuntimeInstallerSubsystem::ExtractAndPublish(FString& OutError)
{
    const FString BackendRoot = FPaths::Combine(TextGenLlamacppRuntimePaths::GetWritableRoot(),
        TextGenLlamacppRuntimePaths::GetBackendDirectoryName(PendingBackend));
    const FString Staging = FPaths::Combine(BackendRoot, TEXT(".staging-")) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString DependencyCache = FPaths::Combine(TextGenLlamacppRuntimePaths::GetWritableRoot(),
        TEXT("_Dependencies"), TextGenLlamacppRuntimePaths::GetBackendDirectoryName(PendingBackend));
    const FString DependencyStaging = DependencyCache + TEXT(".staging-")
        + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    IFileManager::Get().MakeDirectory(*BackendRoot, true);
    if (PendingComponent == ETextGenLlamacppInstallComponent::Runtime)
    {
        IFileManager::Get().MakeDirectory(*Staging, true);
    }
    bool bValid = true;
    bool bHasServer = false;
    bool bHasDLL = false;
    bool bHasCudaDependency = false;
    TArray<FString> Inventory;
    for (const FPendingAsset& PendingAsset : PendingAssets)
    {
        const FString& ZipPath = PendingAsset.DownloadPath;
        int ZipError = 0;
        FTCHARToUTF8 ZipPathUtf8(*ZipPath);
        zip_t* Archive = zip_open(ZipPathUtf8.Get(), ZIP_RDONLY, &ZipError);
        if (!Archive)
        {
            OutError = TEXT("A verified llama.cpp component could not be opened.");
            bValid = false;
            break;
        }
        const zip_int64_t EntryCount = zip_get_num_entries(Archive, 0);
        for (zip_uint64_t Index = 0; bValid && Index < static_cast<zip_uint64_t>(EntryCount); ++Index)
        {
            zip_stat_t Stat;
            zip_stat_init(&Stat);
            if (zip_stat_index(Archive, Index, 0, &Stat) != 0 || !Stat.name)
            {
                bValid = false;
                break;
            }
            const FString Entry = UTF8_TO_TCHAR(Stat.name);
            if (!IsSafeArchivePath(Entry))
            {
                bValid = false;
                OutError = TEXT("A llama.cpp component contains an unsafe path.");
                break;
            }
            const FString Filename = FPaths::GetCleanFilename(Entry);
            if (Filename.IsEmpty())
            {
                continue;
            }
            if (!Filename.Equals(TEXT("llama-server.exe"), ESearchCase::IgnoreCase)
                && !FPaths::GetExtension(Filename).Equals(TEXT("dll"), ESearchCase::IgnoreCase))
            {
                continue;
            }
            const FString OutputRoot = PendingAsset.bCudaDependency ? DependencyStaging : Staging;
            IFileManager::Get().MakeDirectory(*OutputRoot, true);
            const FString Output = FPaths::Combine(OutputRoot, Filename);
            Inventory.AddUnique(Filename);
            zip_file_t* Input = zip_fopen_index(Archive, Index, 0);
            TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Output));
            if (!Input || !File)
            {
                if (Input) zip_fclose(Input);
                bValid = false;
                break;
            }
            uint8 Buffer[64 * 1024];
            zip_int64_t Read = 0;
            while ((Read = zip_fread(Input, Buffer, sizeof(Buffer))) > 0)
            {
                if (!File->Write(Buffer, Read))
                {
                    bValid = false;
                    break;
                }
            }
            bValid = bValid && Read >= 0;
            zip_fclose(Input);
            bHasServer |= !PendingAsset.bCudaDependency
                && Filename.Equals(TEXT("llama-server.exe"), ESearchCase::IgnoreCase);
            bHasDLL |= !PendingAsset.bCudaDependency
                && FPaths::GetExtension(Filename).Equals(TEXT("dll"), ESearchCase::IgnoreCase);
            bHasCudaDependency |= PendingAsset.bCudaDependency
                && (Filename.StartsWith(TEXT("cublas"), ESearchCase::IgnoreCase)
                    || Filename.StartsWith(TEXT("cudart"), ESearchCase::IgnoreCase));
        }
        zip_close(Archive);
        IFileManager::Get().Delete(*ZipPath);
    }
    const bool bInventoryComplete = PendingComponent == ETextGenLlamacppInstallComponent::CudaDependencies
        ? bHasCudaDependency : bHasServer && bHasDLL;
    if (!bValid || !bInventoryComplete)
    {
        OutError = OutError.IsEmpty() ? TEXT("The llama.cpp archive inventory is incomplete or corrupt.") : OutError;
        return false;
    }
    const bool bDownloadedCudaDependencies = PendingAssets.ContainsByPredicate(
        [](const FPendingAsset& Asset) { return Asset.bCudaDependency; });

    if (PendingBackend != ETextGenLlamacppBackend::Vulkan
        && PendingComponent == ETextGenLlamacppInstallComponent::Runtime)
    {
        const FString DependencySource = bDownloadedCudaDependencies ? DependencyStaging : DependencyCache;
        TArray<FString> DependencyFiles;
        IFileManager::Get().FindFiles(DependencyFiles, *FPaths::Combine(DependencySource, TEXT("*.dll")), true, false);
        bool bCopiedCudaRuntime = false;
        for (const FString& Filename : DependencyFiles)
        {
            if (!Filename.StartsWith(TEXT("cublas"), ESearchCase::IgnoreCase)
                && !Filename.StartsWith(TEXT("cudart"), ESearchCase::IgnoreCase))
            {
                continue;
            }
            if (IFileManager::Get().Copy(*FPaths::Combine(Staging, Filename),
                *FPaths::Combine(DependencySource, Filename), true, true) != COPY_OK)
            {
                OutError = TEXT("Cached CUDA dependencies could not be materialized beside the runtime.");
                return false;
            }
            Inventory.AddUnique(Filename);
            bCopiedCudaRuntime = true;
        }
        if (!bCopiedCudaRuntime)
        {
            OutError = TEXT("CUDA dependencies are not installed. Update the CUDA dependency component first.");
            return false;
        }
    }

    const TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
    Manifest->SetStringField(TEXT("tag"), PendingTag);
    Manifest->SetStringField(TEXT("backend"),
        TextGenLlamacppRuntimePaths::GetBackendDirectoryName(PendingBackend));
    Manifest->SetStringField(TEXT("platform"), TEXT("Win64-x64"));
    TArray<TSharedPtr<FJsonValue>> AssetsJson;
    for (const FPendingAsset& Asset : PendingAssets)
    {
        const TSharedRef<FJsonObject> AssetObject = MakeShared<FJsonObject>();
        AssetObject->SetStringField(TEXT("name"), Asset.Name);
        AssetObject->SetStringField(TEXT("url"), Asset.URL);
        AssetObject->SetStringField(TEXT("sha256"), Asset.SHA256);
        AssetsJson.Add(MakeShared<FJsonValueObject>(AssetObject));
    }
    Manifest->SetArrayField(TEXT("assets"), AssetsJson);
    TArray<TSharedPtr<FJsonValue>> InventoryJson;
    for (const FString& Entry : Inventory)
    {
        InventoryJson.Add(MakeShared<FJsonValueString>(Entry));
    }
    Manifest->SetArrayField(TEXT("files"), InventoryJson);
    FString ManifestJson;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ManifestJson);
    FJsonSerializer::Serialize(Manifest, Writer);
    const FString ManifestRoot = PendingComponent == ETextGenLlamacppInstallComponent::CudaDependencies
        ? DependencyStaging : Staging;
    if (!FFileHelper::SaveStringToFile(ManifestJson, *FPaths::Combine(ManifestRoot, TEXT("manifest.json"))))
    {
        OutError = TEXT("The llama.cpp runtime manifest could not be written.");
        return false;
    }

    if (bDownloadedCudaDependencies && PendingBackend != ETextGenLlamacppBackend::Vulkan)
    {
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(DependencyCache), true);
        if (IFileManager::Get().DirectoryExists(*DependencyCache))
        {
            const FString DependencyBackup = DependencyCache + TEXT(".bak-")
                + FDateTime::UtcNow().ToString(TEXT("%Y%m%d%H%M%S"));
            if (!IFileManager::Get().Move(*DependencyBackup, *DependencyCache, true, false, false, true))
            {
                OutError = TEXT("Existing CUDA dependency cache could not be archived.");
                return false;
            }
        }
        if (!IFileManager::Get().Move(*DependencyCache, *DependencyStaging, true, false, false, true))
        {
            OutError = TEXT("Verified CUDA dependencies could not be published atomically.");
            return false;
        }
        if (PendingComponent == ETextGenLlamacppInstallComponent::CudaDependencies)
        {
            TArray<FString> InstalledRuntimeDirectories;
            IFileManager::Get().FindFiles(InstalledRuntimeDirectories,
                *FPaths::Combine(BackendRoot, TEXT("*")), false, true);
            TArray<FString> DependencyFiles;
            IFileManager::Get().FindFiles(DependencyFiles,
                *FPaths::Combine(DependencyCache, TEXT("*.dll")), true, false);
            for (const FString& RuntimeDirectory : InstalledRuntimeDirectories)
            {
                if (RuntimeDirectory.StartsWith(TEXT(".")) || RuntimeDirectory.Contains(TEXT(".bak-")))
                {
                    continue;
                }
                const FString InstalledRuntime = FPaths::Combine(BackendRoot, RuntimeDirectory);
                for (const FString& Filename : DependencyFiles)
                {
                    if (IFileManager::Get().Copy(*FPaths::Combine(InstalledRuntime, Filename),
                        *FPaths::Combine(DependencyCache, Filename), true, true) != COPY_OK)
                    {
                        OutError = TEXT("Updated CUDA dependencies could not be applied to an installed runtime.");
                        return false;
                    }
                }
            }
            return true;
        }
    }

    const FString Target = FPaths::Combine(BackendRoot, PendingTag);
    if (IFileManager::Get().DirectoryExists(*Target))
    {
        const FString TargetBackup = Target + TEXT(".bak-") + FDateTime::UtcNow().ToString(TEXT("%Y%m%d%H%M%S"));
        if (!IFileManager::Get().Move(*TargetBackup, *Target, true, false, false, true))
        {
            OutError = TEXT("Existing llama.cpp runtime could not be archived.");
            return false;
        }
    }
    if (!IFileManager::Get().Move(*Target, *Staging, true, false, false, true))
    {
        OutError = TEXT("The verified llama.cpp runtime could not be published atomically.");
        return false;
    }
    return true;
}

void UTextGenRuntimeInstallerSubsystem::Finish(const bool bSucceeded, const FString& Error)
{
    ActiveRequest.Reset();
    ActiveDownloadStream.Reset();
    bInstalling = false;
    const FText Message = bSucceeded
        ? NSLOCTEXT("TextGen", "RuntimeInstallSucceeded", "llama.cpp component installed successfully.")
        : FText::FromString(Error);
    if (bSucceeded)
    {
        UE_LOG(LogTextGenAPI, Log, TEXT("%s"), *Message.ToString());
    }
    else
    {
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *Message.ToString());
    }
    OnComplete.Broadcast(bSucceeded, bSucceeded ? PendingTag : FString(), PendingBackend, PendingComponent, Message);
    CompleteNative.Broadcast(bSucceeded, bSucceeded ? PendingTag : FString(), PendingBackend, PendingComponent, Message);
}
