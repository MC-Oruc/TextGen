#include "TextGenContentBrowserDataSource.h"

#include "Misc/Paths.h"
#include "TextGen/TextGenProjectSettings.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <shellapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
    FString NormalizePath(const FString& Path)
    {
        FString Result = FPaths::IsRelative(Path) ? FPaths::ProjectDir() / Path : Path;
        Result = FPaths::ConvertRelativePathToFull(Result);
        FPaths::CollapseRelativeDirectories(Result);
        FPaths::NormalizeFilename(Result);
        return Result;
    }

    bool Matches(const FString& ConfiguredPath, const FString& AbsolutePath)
    {
        return !ConfiguredPath.IsEmpty()
            && NormalizePath(ConfiguredPath).Equals(NormalizePath(AbsolutePath), ESearchCase::IgnoreCase);
    }

    bool DeleteThroughOperatingSystem(const FString& Path)
    {
#if PLATFORM_WINDOWS
        FString NativePath = FPaths::ConvertRelativePathToFull(Path);
        FPaths::MakePlatformFilename(NativePath);
        TArray<WCHAR> DoubleNullTerminatedPath;
        DoubleNullTerminatedPath.SetNumZeroed(NativePath.Len() + 2);
        FCStringWide::Strncpy(
            DoubleNullTerminatedPath.GetData(),
            StringCast<WIDECHAR>(*NativePath).Get(),
            DoubleNullTerminatedPath.Num());

        SHFILEOPSTRUCTW Operation{};
        Operation.wFunc = FO_DELETE;
        Operation.pFrom = DoubleNullTerminatedPath.GetData();
        Operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        return SHFileOperationW(&Operation) == 0 && !Operation.fAnyOperationsAborted;
#else
        return false;
#endif
    }

    void ClearConfiguredModelReference(const FString& DeletedPath)
    {
        UTextGenProjectSettings* Settings = GetMutableDefault<UTextGenProjectSettings>();
        bool bChanged = false;
        if (Matches(Settings->DevelopmentDefaults.ModelPath, DeletedPath))
        {
            Settings->DevelopmentDefaults.ModelPath.Reset();
            bChanged = true;
        }
        if (Matches(Settings->PackagedDefaults.ModelPath, DeletedPath))
        {
            Settings->PackagedDefaults.ModelPath.Reset();
            bChanged = true;
        }
        if (bChanged)
        {
            Settings->TryUpdateDefaultConfigFile();
        }
    }
}

bool UTextGenContentBrowserDataSource::DeleteItem(const FContentBrowserItemData& InItem)
{
    FString Path;
    if (!GetItemPhysicalPath(InItem, Path) || !DeleteThroughOperatingSystem(Path))
    {
        return false;
    }
    if (FPaths::GetExtension(Path).Equals(TEXT("gguf"), ESearchCase::IgnoreCase))
    {
        ClearConfiguredModelReference(Path);
    }
    return true;
}

bool UTextGenContentBrowserDataSource::BulkDeleteItems(TArrayView<const FContentBrowserItemData> InItems)
{
    bool bDeletedAny = false;
    for (const FContentBrowserItemData& Item : InItems)
    {
        bDeletedAny |= DeleteItem(Item);
    }
    return bDeletedAny;
}
