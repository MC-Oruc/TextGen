// Copyright <--\, Inc. All Rights Reserved.

#include "TextGen/TextGenSubsystem.h"
#include "TextGen/Data/TextGenSettingsSave.h"
#include "TextGen/TextGenProjectSettings.h"
#include "TextGen/TextGenLocalServiceSubsystem.h"

#include "Kismet/GameplayStatics.h"
#include "Engine/Engine.h"
#include "Misc/Guid.h"
#include "TextGen/TextGenLog.h"

static const TCHAR* GTextGenSettingsSlotName = TEXT("TextGenSettingsSlot");
static const int32 TextGenSettingsUserIndex = 0;
static const TCHAR* GProjectDefaultProfileId = TEXT("project-default-llamacpp");
static const int32 GTextGenProfileSchemaVersion = 2;

namespace
{
    void NormalizeProfileSettings(FTextGenProviderProfile& InOutProfile)
    {
        if (InOutProfile.DisplayName.IsEmpty())
        {
            InOutProfile.DisplayName = TEXT("Profile");
        }

        if (InOutProfile.Provider != ELLMProvider::KoboldCpp && InOutProfile.Provider != ELLMProvider::OpenAI
            && InOutProfile.Provider != ELLMProvider::Llamacpp)
        {
            InOutProfile.Provider = ELLMProvider::KoboldCpp;
        }

        if (InOutProfile.Kobold.BaseURL.IsEmpty())
        {
            InOutProfile.Kobold.BaseURL = TEXT("http://localhost:5001");
        }

        InOutProfile.Kobold.RequestTimeout = FMath::Clamp(InOutProfile.Kobold.RequestTimeout, 1.0f, 300.0f);

        if (InOutProfile.OpenAI.BaseURL.IsEmpty())
        {
            InOutProfile.OpenAI.BaseURL = TEXT("https://api.openai.com");
        }

        if (InOutProfile.OpenAI.ChatModel.IsEmpty())
        {
            InOutProfile.OpenAI.ChatModel = TEXT("gpt-4o-mini");
        }

        FTextGenLlamacppConfig& Llamacpp = InOutProfile.Llamacpp;
        Llamacpp.Port = FMath::Clamp(Llamacpp.Port, 1, 65535);
        if (Llamacpp.ContextSize <= 0) Llamacpp.ContextSize = 12288;
        Llamacpp.ContextSize = FMath::Clamp(Llamacpp.ContextSize, 1024, 131072);
        Llamacpp.RequestTimeout = FMath::Clamp(Llamacpp.RequestTimeout, 1.0f, 3600.0f);
        Llamacpp.StartupTimeout = FMath::Clamp(Llamacpp.StartupTimeout, 1.0f, 3600.0f);
    }

}

void UTextGenSubsystem::EnsureSettingsLoaded()
{
    if (SettingsSave)
    {
        return;
    }

    if (UGameplayStatics::DoesSaveGameExist(GTextGenSettingsSlotName, TextGenSettingsUserIndex))
    {
        if (USaveGame* Loaded = UGameplayStatics::LoadGameFromSlot(GTextGenSettingsSlotName, TextGenSettingsUserIndex))
        {
            SettingsSave = Cast<UTextGenSettingsSaveGame>(Loaded);
            if (SettingsSave)
            {
                TEXTGEN_DEBUG_LOG(Log, TEXT("TextGen settings loaded."));
            }
        }
    }

    if (!SettingsSave)
    {
        SettingsSave = Cast<UTextGenSettingsSaveGame>(UGameplayStatics::CreateSaveGameObject(UTextGenSettingsSaveGame::StaticClass()));
        TEXTGEN_DEBUG_LOG(Log, TEXT("TextGen settings created (empty)."));
    }
}

void UTextGenSubsystem::EnsureProfilesLoaded()
{
    EnsureSettingsLoaded();
    if (!SettingsSave)
    {
        return;
    }

    const UTextGenProjectSettings* ProjectSettings = GetDefault<UTextGenProjectSettings>();
    FTextGenProviderProfile& ProjectDefault = SettingsSave->ProfilesById.FindOrAdd(GProjectDefaultProfileId);
    ProjectDefault.Id = GProjectDefaultProfileId;
    ProjectDefault.DisplayName = TEXT("Default");
    ProjectDefault.Provider = ELLMProvider::Llamacpp;
    ProjectDefault.bIsProjectDefault = true;
#if WITH_EDITOR
    ProjectDefault.Llamacpp = ProjectSettings->DevelopmentDefaults;
#else
    ProjectDefault.Llamacpp = ProjectSettings->PackagedDefaults;
#endif
    ProjectDefault.GenerationSettings = GetDefaultGenerationSettings();
    ProjectDefault.ParamUsageConfig = GetDefaultUsageConfig();
    NormalizeProfileSettings(ProjectDefault);

    if (SettingsSave->ProfileSchemaVersion < GTextGenProfileSchemaVersion)
    {
        SettingsSave->ProfileSchemaVersion = GTextGenProfileSchemaVersion;
        SaveSettings();
    }

    for (auto& Pair : SettingsSave->ProfilesById)
    {
        NormalizeProfileSettings(Pair.Value);
    }

    if (SettingsSave->ActiveProvider != ELLMProvider::KoboldCpp && SettingsSave->ActiveProvider != ELLMProvider::OpenAI
        && SettingsSave->ActiveProvider != ELLMProvider::Llamacpp)
    {
        SettingsSave->ActiveProvider = ELLMProvider::Llamacpp;
    }

    if (SettingsSave->ActiveProfileId.IsEmpty() || !SettingsSave->ProfilesById.Contains(SettingsSave->ActiveProfileId))
    {
        const FString FirstId = FindFirstProfileIdForProvider(SettingsSave->ActiveProvider);
        SettingsSave->ActiveProfileId = FirstId.IsEmpty() ? SettingsSave->ProfilesById.CreateConstIterator()->Key : FirstId;
    }

    const bool bRuntimeProfileMissing = ActiveProfileId.IsEmpty() || !SettingsSave->ProfilesById.Contains(ActiveProfileId);
    if (bRuntimeProfileMissing)
    {
        ActiveProfileId = SettingsSave->ActiveProfileId;
        ActiveProvider = SettingsSave->ActiveProvider;
        ApplyProfileById(ActiveProfileId);
    }
}

void UTextGenSubsystem::SaveProfiles()
{
    if (!SettingsSave)
    {
        return;
    }

    if (FTextGenProviderProfile* Profile = GetActiveProfileMutable())
    {
        if (Profile->bIsProjectDefault)
        {
            SettingsSave->ActiveProvider = ActiveProvider;
            SettingsSave->ActiveProfileId = ActiveProfileId;
            SaveSettings();
            return;
        }
        Profile->Provider = ActiveProvider;
        Profile->Kobold.BaseURL = BaseURL;
        Profile->Kobold.RequestTimeout = RequestTimeout;
        Profile->Kobold.bEnableDebugLogging = bEnableDebugLogging;
        Profile->OpenAI.BaseURL = OpenAIBaseURL;
        Profile->OpenAI.APIKey = OpenAIAPIKey;
        Profile->OpenAI.ChatModel = OpenAIChatModel;
        Profile->Llamacpp = LlamacppConfig;
        Profile->GenerationSettings = ActiveGenerationSettings;
        Profile->ParamUsageConfig = ParamUsageConfig;
        NormalizeProfileSettings(*Profile);
    }

    SettingsSave->ActiveProvider = ActiveProvider;
    SettingsSave->ActiveProfileId = ActiveProfileId;
    SaveSettings();
}

bool UTextGenSubsystem::ApplyProfileById(const FString& ProfileId)
{
    if (!SettingsSave)
    {
        return false;
    }

    const FTextGenProviderProfile* Profile = SettingsSave->ProfilesById.Find(ProfileId);
    if (!Profile)
    {
        return false;
    }

    ActiveProfileId = ProfileId;
    ActiveProvider = Profile->Provider;
    LoadSettingsForActiveProfile();

    SettingsSave->ActiveProvider = ActiveProvider;
    SettingsSave->ActiveProfileId = ActiveProfileId;
    return true;
}

void UTextGenSubsystem::SaveSettings()
{
    EnsureSettingsLoaded();
    if (!SettingsSave)
    {
        return;
    }

    const bool bOK = UGameplayStatics::SaveGameToSlot(SettingsSave, GTextGenSettingsSlotName, TextGenSettingsUserIndex);
    if (!bOK)
    {
        UE_LOG(LogTextGenAPI, Warning, TEXT("TextGen settings save failed."));
    }
}

FTextGenGenerationSettings UTextGenSubsystem::GetDefaultGenerationSettings() const
{
    return FTextGenGenerationSettings();
}

FTextGenParamUsageConfig UTextGenSubsystem::GetDefaultUsageConfig() const
{
    return FTextGenParamUsageConfig();
}

FTextGenProviderProfile* UTextGenSubsystem::GetActiveProfileMutable()
{
    if (!SettingsSave || ActiveProfileId.IsEmpty())
    {
        return nullptr;
    }

    return SettingsSave->ProfilesById.Find(ActiveProfileId);
}

const FTextGenProviderProfile* UTextGenSubsystem::GetActiveProfile() const
{
    if (!SettingsSave || ActiveProfileId.IsEmpty())
    {
        return nullptr;
    }

    return SettingsSave->ProfilesById.Find(ActiveProfileId);
}

void UTextGenSubsystem::LoadSettingsForActiveProfile()
{
    EnsureSettingsLoaded();
    if (!SettingsSave)
    {
        ActiveGenerationSettings = GetDefaultGenerationSettings();
        ParamUsageConfig = GetDefaultUsageConfig();
        return;
    }

    const FTextGenProviderProfile* Profile = GetActiveProfile();
    if (!Profile)
    {
        ActiveGenerationSettings = GetDefaultGenerationSettings();
        ParamUsageConfig = GetDefaultUsageConfig();
        return;
    }

    ActiveProvider = Profile->Provider;
    BaseURL = Profile->Kobold.BaseURL;
    RequestTimeout = Profile->Kobold.RequestTimeout;
    bEnableDebugLogging = Profile->Kobold.bEnableDebugLogging;
    OpenAIBaseURL = Profile->OpenAI.BaseURL;
    OpenAIAPIKey = Profile->OpenAI.APIKey;
    OpenAIChatModel = Profile->OpenAI.ChatModel;
    LlamacppConfig = Profile->Llamacpp;
    ActiveGenerationSettings = Profile->GenerationSettings;
    ParamUsageConfig = Profile->ParamUsageConfig;
}

FString UTextGenSubsystem::CreateProfileInternal(const FString& DisplayName, ELLMProvider Provider)
{
    EnsureProfilesLoaded();
    if (!SettingsSave)
    {
        return TEXT("");
    }

    FTextGenProviderProfile Profile;
    Profile.Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    Profile.DisplayName = DisplayName.IsEmpty() ? TEXT("Profile") : DisplayName;
    Profile.Provider = Provider;
    Profile.bIsProjectDefault = false;

    // Reset provider configs to clean default structures per provider type
    Profile.Kobold = FTextGenKoboldConfig();
    Profile.OpenAI = FTextGenOpenAIConfig();
    Profile.Llamacpp = FTextGenLlamacppConfig();

    if (Provider == ELLMProvider::Llamacpp)
    {
        if (const FTextGenProviderProfile* ProjectDefault = SettingsSave->ProfilesById.Find(GProjectDefaultProfileId))
        {
            Profile.Llamacpp = ProjectDefault->Llamacpp;
        }
        else
        {
#if WITH_EDITOR
            Profile.Llamacpp = GetDefault<UTextGenProjectSettings>()->DevelopmentDefaults;
#else
            Profile.Llamacpp = GetDefault<UTextGenProjectSettings>()->PackagedDefaults;
#endif
        }
    }
    Profile.GenerationSettings = GetDefaultGenerationSettings();
    Profile.ParamUsageConfig = GetDefaultUsageConfig();
    NormalizeProfileSettings(Profile);

    SettingsSave->ProfilesById.Add(Profile.Id, Profile);
    SaveProfiles();
    return Profile.Id;
}

bool UTextGenSubsystem::RenameProfileInternal(const FString& ProfileId, const FString& NewDisplayName)
{
    if (!SettingsSave)
    {
        return false;
    }
    if (FTextGenProviderProfile* Profile = SettingsSave->ProfilesById.Find(ProfileId))
    {
        if (Profile->bIsProjectDefault)
        {
            return false;
        }
        Profile->DisplayName = NewDisplayName;
        SaveProfiles();
        return true;
    }
    return false;
}

bool UTextGenSubsystem::DeleteProfileInternal(const FString& ProfileId)
{
    if (!SettingsSave)
    {
        return false;
    }
    if (const FTextGenProviderProfile* Profile = SettingsSave->ProfilesById.Find(ProfileId); Profile && Profile->bIsProjectDefault)
    {
        return false;
    }
    const bool bRemoved = SettingsSave->ProfilesById.Remove(ProfileId) > 0;
    if (!bRemoved)
    {
        return false;
    }

    const bool bDeletedActiveProfile = ActiveProfileId == ProfileId;
    if (bDeletedActiveProfile)
    {
        const FString FallbackId = FindFirstProfileIdForProvider(ELLMProvider::KoboldCpp);
        if (!FallbackId.IsEmpty())
        {
            ApplyProfileById(FallbackId);
        }
        else
        {
            const FString NewId = CreateProfileInternal(TEXT("Default KoboldCpp"), ELLMProvider::KoboldCpp);
            if (!NewId.IsEmpty())
            {
                ApplyProfileById(NewId);
            }
        }
    }

    SaveProfiles();
    if (bDeletedActiveProfile)
    {
        HandleActiveProfileChanged();
    }
    return true;
}

bool UTextGenSubsystem::SetActiveProfileInternal(const FString& ProfileId)
{
    EnsureProfilesLoaded();
    if (!ApplyProfileById(ProfileId))
    {
        return false;
    }

    SaveProfiles();
    HandleActiveProfileChanged();

    return true;
}

void UTextGenSubsystem::HandleActiveProfileChanged()
{
    CancelConversationPreparation(TEXT("Active provider profile changed."));
    InvalidateConversationCommits();
    PreparedCacheKey.Reset();
    PreparedModelId.Reset();
    PreparedSlotPreference = ETextGenConversationSlotPreference::None;
    bIsAPIAvailable = false;

#if WITH_EDITOR
    ReconcileManagedService(false);
#else
    ReconcileManagedService(true);
#endif
}

void UTextGenSubsystem::ReconcileManagedService(const bool bAllowStartWhenStopped)
{
    UTextGenLocalServiceSubsystem* Service = GEngine ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
    if (!Service)
    {
        return;
    }


    Service->SetInteractiveWorkloadActive(
        bInteractiveSessionActive
        && ActiveProvider == ELLMProvider::Llamacpp
        && !LlamacppConfig.bManagedDisabled);

    FString Error;
    if (ActiveProvider != ELLMProvider::Llamacpp || LlamacppConfig.bManagedDisabled)
    {
        Service->StopManaged(Error);
        return;
    }

    const ETextGenLocalServiceState State = Service->GetState();
    const bool bAlreadyManaged = State != ETextGenLocalServiceState::Stopped
        && State != ETextGenLocalServiceState::Disabled;
    if (bAllowStartWhenStopped || bAlreadyManaged)
    {
        Service->StartManaged(LlamacppConfig, Error);
    }
}

FString UTextGenSubsystem::FindFirstProfileIdForProvider(ELLMProvider Provider) const
{
    if (!SettingsSave)
    {
        return TEXT("");
    }
    for (const auto& Pair : SettingsSave->ProfilesById)
    {
        if (Pair.Value.Provider == Provider)
        {
            return Pair.Key;
        }
    }
    return TEXT("");
}

void UTextGenSubsystem::ApplyUsageConfigToSettings(FTextGenGenerationSettings& InOutSettings) const
{
    if (!ParamUsageConfig.bUseStopSequences)
    {
        InOutSettings.StopSequences.Reset();
    }
    if (!ParamUsageConfig.bUseMaxLength) { InOutSettings.MaxLength = -1; }
    if (!ParamUsageConfig.bUseTemperature) { InOutSettings.Temperature = -1.f; }
    if (!ParamUsageConfig.bUseTopP) { InOutSettings.TopP = -1.f; }
    if (!ParamUsageConfig.bUseMinP) { InOutSettings.MinP = -1.f; }
    if (!ParamUsageConfig.bUseTopK) { InOutSettings.TopK = -1; }
    if (!ParamUsageConfig.bUseRepetitionPenalty) { InOutSettings.RepetitionPenalty = -1.f; }
    if (!ParamUsageConfig.bUseFrequencyPenalty) { InOutSettings.FrequencyPenalty = -999.f; }
    if (!ParamUsageConfig.bUsePresencePenalty) { InOutSettings.PresencePenalty = -999.f; }
}

void UTextGenSubsystem::SetGenerationSettingsInternal(const FTextGenGenerationSettings& Settings)
{
    EnsureProfilesLoaded();
    if (const FTextGenProviderProfile* Profile = GetActiveProfile(); Profile && Profile->bIsProjectDefault)
    {
        return;
    }
    ActiveGenerationSettings = Settings;

    if (FTextGenProviderProfile* Profile = GetActiveProfileMutable())
    {
        Profile->GenerationSettings = ActiveGenerationSettings;
        SaveProfiles();
    }
}

void UTextGenSubsystem::SetGenerationSettings(const UObject* WorldContext, const FTextGenGenerationSettings& Settings)
{
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        Subsystem->SetGenerationSettingsInternal(Settings);
    }
}

FTextGenGenerationSettings UTextGenSubsystem::GetGenerationSettings(const UObject* WorldContext)
{
    if (UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext))
    {
        return Subsystem->ActiveGenerationSettings;
    }

    return FTextGenGenerationSettings();
}
