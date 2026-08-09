#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Algo/AllOf.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "GameFramework/SaveGame.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProcessRuntimeSubsystem.h"
#include "ProcessRuntimeTypes.h"
#include "TextGen/TextGenSubsystem.h"
#include "TextGen/TextGenConversationCache.h"
#include "TextGen/TextGenLocalServiceSubsystem.h"
#include "TextGen/TextGenProjectSettings.h"
#include "TextGen/TextGenLlamacppRuntimePaths.h"
#include "TextGen/Providers/OpenAIAPI.h"
#include "TextGen/Data/TextGenSettingsSave.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	const FString TextGenSettingsTestSlotName = TEXT("TextGenSettingsSlot");
	constexpr int32 TextGenSettingsTestUserIndex = 0;

	class FTextGenAutomationGameInstanceScope
	{
	public:
		FTextGenAutomationGameInstanceScope(FAutomationTestBase& InTest, const TCHAR* InWorldName)
			: Test(InTest)
		{
			if (!GEngine)
			{
				Test.AddError(TEXT("GEngine is unavailable."));
				return;
			}

			GameInstance.Reset(NewObject<UGameInstance>(GEngine));
			if (!GameInstance.IsValid())
			{
				Test.AddError(TEXT("Failed to create automation game instance."));
				return;
			}

			GameInstance->InitializeStandalone(FName(InWorldName));
			if (!GameInstance->GetWorld())
			{
				Test.AddError(TEXT("Automation game instance failed to create a world."));
				GameInstance.Reset();
			}
		}

		~FTextGenAutomationGameInstanceScope()
		{
			if (!GameInstance.IsValid())
			{
				return;
			}

			UWorld* World = GameInstance->GetWorld();
			GameInstance->Shutdown();

			if (World)
			{
				World->DestroyWorld(false);
				if (GEngine)
				{
					GEngine->DestroyWorldContext(World);
				}
			}
		}

		UGameInstance* Get() const
		{
			return GameInstance.Get();
		}

	private:
		FAutomationTestBase& Test;
		TStrongObjectPtr<UGameInstance> GameInstance;
	};

	class FTextGenSaveGameSlotBackupScope
	{
	public:
		FTextGenSaveGameSlotBackupScope(const FString& InSlotName, const int32 InUserIndex)
			: SlotName(InSlotName)
			, UserIndex(InUserIndex)
		{
			if (UGameplayStatics::DoesSaveGameExist(SlotName, UserIndex))
			{
				bHadExistingSave = true;
				SavedObject.Reset(UGameplayStatics::LoadGameFromSlot(SlotName, UserIndex));
			}
		}

		~FTextGenSaveGameSlotBackupScope()
		{
			if (bHadExistingSave && SavedObject.IsValid())
			{
				UGameplayStatics::SaveGameToSlot(SavedObject.Get(), SlotName, UserIndex);
			}
			else
			{
				UGameplayStatics::DeleteGameInSlot(SlotName, UserIndex);
			}
		}

	private:
		FString SlotName;
		int32 UserIndex = 0;
		bool bHadExistingSave = false;
		TStrongObjectPtr<USaveGame> SavedObject;
	};

#if WITH_EDITOR
	class FTextGenManagedLatestConfigCommand final : public IAutomationLatentCommand
	{
	public:
		explicit FTextGenManagedLatestConfigCommand(FAutomationTestBase& InTest)
			: Test(InTest)
		{
			InitialConfig = GetDefault<UTextGenProjectSettings>()->DevelopmentDefaults;
			InitialConfig.Port = 18780;
			InitialConfig.ContextSize = 2048;
			InitialConfig.StartupTimeout = 120.0f;
			IntermediateConfig = InitialConfig;
			IntermediateConfig.ContextSize = 3072;
			FinalConfig = InitialConfig;
			FinalConfig.ContextSize = 4096;
		}

		virtual bool Update() override
		{
			UTextGenLocalServiceSubsystem* Service = GEngine
				? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr;
			UProcessRuntimeSubsystem* ProcessRuntime = GEngine
				? GEngine->GetEngineSubsystem<UProcessRuntimeSubsystem>() : nullptr;
			if (!Service || !ProcessRuntime)
			{
				Test.AddError(TEXT("Managed TextGen integration subsystems are unavailable."));
				return true;
			}

			switch (Phase)
			{
			case EPhase::StopExisting:
			{
				FString Ignored;
				Service->StopManaged(Ignored);
				SetDeadline(30.0);
				Phase = EPhase::WaitForInitialStop;
				return false;
			}
			case EPhase::WaitForInitialStop:
				if (Service->GetState() == ETextGenLocalServiceState::Stopped)
				{
					FString Error;
					if (!Service->StartManaged(InitialConfig, Error))
					{
						FailAndStop(Service, FString::Printf(TEXT("Initial managed model start failed: %s"), *Error));
						return false;
					}
					SetDeadline(120.0);
					Phase = EPhase::WaitForInitialReady;
				}
				else if (HasTimedOut())
				{
					FailAndStop(Service, TEXT("Managed process did not stop before the model-switch test."));
				}
				return false;
			case EPhase::WaitForInitialReady:
				if (Service->GetState() == ETextGenLocalServiceState::Failed)
				{
					FailAndStop(Service, FString::Printf(TEXT("Initial managed model failed: %s"), *Service->GetLastError()));
					return false;
				}
				if (Service->GetState() == ETextGenLocalServiceState::Ready)
				{
					FManagedProcessStatus Status;
					if (!ProcessRuntime->GetStatus(ManagedProcessId, Status) || !Status.IsActive())
					{
						FailAndStop(Service, TEXT("Managed llama.cpp parent process is not active."));
						return false;
					}
					ParentStartedAtUtc = Status.StartedAtUtc;

					FString Error;
					if (!Service->StartManaged(IntermediateConfig, Error)
						|| !Service->StartManaged(FinalConfig, Error))
					{
						FailAndStop(Service, FString::Printf(TEXT("Rapid managed config update failed: %s"), *Error));
						return false;
					}
					SetDeadline(120.0);
					Phase = EPhase::WaitForFinalReady;
				}
				else if (HasTimedOut())
				{
					FailAndStop(Service, TEXT("Initial managed model did not become ready."));
				}
				return false;
			case EPhase::WaitForFinalReady:
				bObservedIntermediateConfig |= Service->GetActiveConfig().ContextSize == IntermediateConfig.ContextSize;
				if (Service->GetState() == ETextGenLocalServiceState::Failed)
				{
					FailAndStop(Service, FString::Printf(TEXT("Final managed model failed: %s"), *Service->GetLastError()));
					return false;
				}
				if (Service->GetState() == ETextGenLocalServiceState::Ready
					&& Service->GetActiveConfig().ContextSize == FinalConfig.ContextSize)
				{
					FManagedProcessStatus Status;
					Test.TestTrue(TEXT("Managed llama.cpp parent remains active"),
						ProcessRuntime->GetStatus(ManagedProcessId, Status) && Status.IsActive());
					Test.TestEqual(TEXT("Model-only changes preserve the parent process"),
						Status.StartedAtUtc, ParentStartedAtUtc);
					Test.TestFalse(TEXT("Superseded model configuration is never activated"), bObservedIntermediateConfig);

					FString Error;
					Service->StopManaged(Error);
					if (!Service->StartManaged(FinalConfig, Error))
					{
						FailAndStop(Service, FString::Printf(TEXT("Rapid managed restart failed: %s"), *Error));
						return false;
					}
					SetDeadline(120.0);
					Phase = EPhase::WaitForRapidRestartReady;
				}
				else if (HasTimedOut())
				{
					FailAndStop(Service, TEXT("Final managed model configuration did not become ready."));
				}
				return false;
			case EPhase::WaitForRapidRestartReady:
				if (Service->GetState() == ETextGenLocalServiceState::Failed)
				{
					FailAndStop(Service, FString::Printf(TEXT("Rapid managed restart entered failure: %s"), *Service->GetLastError()));
					return false;
				}
				if (Service->GetState() == ETextGenLocalServiceState::Ready)
				{
					FManagedProcessStatus Status;
					Test.TestTrue(TEXT("Rapid restart leaves a managed parent active"),
						ProcessRuntime->GetStatus(ManagedProcessId, Status) && Status.IsActive());
					Test.TestTrue(TEXT("Rapid stop/start replaces the stopped parent"),
						Status.StartedAtUtc != ParentStartedAtUtc);

					FString Ignored;
					Service->StopManaged(Ignored);
					SetDeadline(30.0);
					Phase = EPhase::WaitForFinalStop;
				}
				else if (HasTimedOut())
				{
					FailAndStop(Service, TEXT("Rapid managed stop/start did not recover to Ready."));
				}
				return false;
			case EPhase::WaitForFinalStop:
				if (Service->GetState() == ETextGenLocalServiceState::Stopped)
				{
					return true;
				}
				if (HasTimedOut())
				{
					Test.AddError(TEXT("Managed process did not stop after the model-switch test."));
					return true;
				}
				return false;
			}
			return true;
		}

	private:
		enum class EPhase : uint8
		{
			StopExisting,
			WaitForInitialStop,
			WaitForInitialReady,
			WaitForFinalReady,
			WaitForRapidRestartReady,
			WaitForFinalStop
		};

		void SetDeadline(const double Seconds)
		{
			DeadlineSeconds = FPlatformTime::Seconds() + Seconds;
		}

		bool HasTimedOut() const
		{
			return FPlatformTime::Seconds() >= DeadlineSeconds;
		}

		void FailAndStop(UTextGenLocalServiceSubsystem* Service, const FString& Error)
		{
			Test.AddError(Error);
			FString Ignored;
			Service->StopManaged(Ignored);
			SetDeadline(30.0);
			Phase = EPhase::WaitForFinalStop;
		}

		FAutomationTestBase& Test;
		const FManagedProcessId ManagedProcessId = FManagedProcessId(TEXT("TextGen.ManagedLocalServer"));
		FTextGenLlamacppConfig InitialConfig;
		FTextGenLlamacppConfig IntermediateConfig;
		FTextGenLlamacppConfig FinalConfig;
		FDateTime ParentStartedAtUtc;
		double DeadlineSeconds = 0.0;
		EPhase Phase = EPhase::StopExisting;
		bool bObservedIntermediateConfig = false;
	};
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSoCTextGenModelsDebugFormattingTest,
	"SoC.Editor.TextGen.Runtime.ModelsDebugFormatting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSoCTextGenConversationCacheFilenameTest,
	"SoC.Editor.TextGen.Runtime.ConversationCacheFilename",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSoCTextGenConversationCacheRequestTest,
	"SoC.Editor.TextGen.Runtime.ConversationCacheRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSoCTextGenLlamacppRuntimeResolutionTest,
	"SoC.Editor.TextGen.Runtime.LlamacppRuntimeResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSoCTextGenDefaultCacheGenerationValidationTest,
	"SoC.Editor.TextGen.Runtime.DefaultCacheGenerationValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSoCTextGenManagedLatestConfigTest,
	"SoC.Editor.TextGen.Integration.ManagedLatestConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
#endif

bool FSoCTextGenLlamacppRuntimeResolutionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Tag = TEXT("automation-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString RuntimeDirectory = TextGenLlamacppRuntimePaths::GetWritableRuntimeDirectory(
		Tag, ETextGenLlamacppBackend::CUDA13);
	IFileManager::Get().MakeDirectory(*RuntimeDirectory, true);
	FFileHelper::SaveStringToFile(TEXT("test"),
		*FPaths::Combine(RuntimeDirectory, TEXT("llama-server.exe")));
	TestFalse(TEXT("Incomplete CUDA runtime is rejected"),
		TextGenLlamacppRuntimePaths::HasRuntimeInventory(
			RuntimeDirectory, ETextGenLlamacppBackend::CUDA13));

	FFileHelper::SaveStringToFile(TEXT("test"),
		*FPaths::Combine(RuntimeDirectory, TEXT("cudart64_13.dll")));
	FFileHelper::SaveStringToFile(TEXT("test"),
		*FPaths::Combine(RuntimeDirectory, TEXT("cublas64_13.dll")));
	TestTrue(TEXT("Complete CUDA runtime is accepted"),
		TextGenLlamacppRuntimePaths::HasRuntimeInventory(
			RuntimeDirectory, ETextGenLlamacppBackend::CUDA13));

	FString ResolvedDirectory;
	TestTrue(TEXT("Writable runtime resolves"),
		TextGenLlamacppRuntimePaths::ResolveRuntimeDirectory(
			Tag, ETextGenLlamacppBackend::CUDA13, ResolvedDirectory));
	TestEqual(TEXT("Writable runtime has priority"), ResolvedDirectory, RuntimeDirectory);
	IFileManager::Get().DeleteDirectory(*RuntimeDirectory, false, true);
	return true;
}

bool FSoCTextGenConversationCacheFilenameTest::RunTest(const FString& Parameters)
{
	const FString Default = FTextGenConversationCachePaths::BuildDefaultFilename(
		TEXT("NPC:Mayor__Day/03"), TEXT("b10333"), TEXT("gemma model"), 12288,
		ETextGenLlamacppKVCacheType::Q8_0, ETextGenLlamacppKVCacheType::Q4_0);
	const FString Progress = FTextGenConversationCachePaths::BuildProgressFilename(
		TEXT("NPC:Mayor__Day/03"), TEXT("b10333"), TEXT("gemma model"), 12288,
		ETextGenLlamacppKVCacheType::Q8_0, ETextGenLlamacppKVCacheType::Q4_0);
	TestEqual(TEXT("Default cache filename is readable and deterministic"), Default,
		TEXT("NPC-Mayor-Day-03_b10333_gemma-model_Ctx12288_KQ8-0_VQ4-0_Default.bin"));
	TestEqual(TEXT("Progress cache filename uses the same key"), Progress,
		TEXT("NPC-Mayor-Day-03_b10333_gemma-model_Ctx12288_KQ8-0_VQ4-0_Progress.bin"));
	const FString AlternateKV = FTextGenConversationCachePaths::BuildDefaultFilename(
		TEXT("NPC:Mayor__Day/03"), TEXT("b10333"), TEXT("gemma model"), 12288,
		ETextGenLlamacppKVCacheType::Q4_0, ETextGenLlamacppKVCacheType::Q4_0);
	TestNotEqual(TEXT("Different KV cache types use different files"), AlternateKV, Default);
	return true;
}

bool FSoCTextGenConversationCacheRequestTest::RunTest(const FString& Parameters)
{
	FTextGenConversationContext Context;
	Context.DefaultCacheKey = TEXT("NPC_Mayor");
	Context.ProgressCacheKey = TEXT("NPC_Mayor__Day03");
	TestFalse(TEXT("None never requests a cache"), Context.HasCacheRequest());

	Context.SlotPreference = ETextGenConversationSlotPreference::Default;
	TestTrue(TEXT("Default uses the stable baseline key"), Context.HasCacheRequest());
	TestEqual(TEXT("Default resolves the baseline key"), Context.GetRequestedCacheKey(), TEXT("NPC_Mayor"));

	Context.SlotPreference = ETextGenConversationSlotPreference::Progress;
	TestTrue(TEXT("Progress uses the partitioned key"), Context.HasCacheRequest());
	TestEqual(TEXT("Progress resolves the partitioned key"), Context.GetRequestedCacheKey(), TEXT("NPC_Mayor__Day03"));

	Context.ProgressCacheKey.Reset();
	TestFalse(TEXT("A cache kind without an owner key is rejected"), Context.HasCacheRequest());
	return true;
}

#if WITH_EDITOR
bool FSoCTextGenManagedLatestConfigTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FTextGenManagedLatestConfigCommand(*this));
	return true;
}

bool FSoCTextGenDefaultCacheGenerationValidationTest::RunTest(const FString& Parameters)
{
	UTextGenLocalServiceSubsystem* Service = GEngine
		? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>()
		: nullptr;
	TestNotNull(TEXT("Local service exists"), Service);
	if (!Service)
	{
		return false;
	}

	bool bCallbackCalled = false;
	bool bCallbackSuccess = true;
	FString CallbackError;
	const bool bStarted = Service->GenerateDefaultConversationCache(
		FTextGenLlamacppConfig(),
		FString(),
		FString(),
		[&bCallbackCalled, &bCallbackSuccess, &CallbackError](
			const bool bSuccess,
			const FString& Error,
			const FString& OutputPath)
		{
			(void)OutputPath;
			bCallbackCalled = true;
			bCallbackSuccess = bSuccess;
			CallbackError = Error;
		});

	TestFalse(TEXT("Incomplete generation request does not start"), bStarted);
	TestTrue(TEXT("Incomplete generation request completes synchronously"), bCallbackCalled);
	TestFalse(TEXT("Incomplete generation request reports failure"), bCallbackSuccess);
	TestTrue(TEXT("Incomplete generation request explains the failure"), !CallbackError.IsEmpty());
	return true;
}
#endif

bool FSoCTextGenModelsDebugFormattingTest::RunTest(const FString& Parameters)
{
	FTextGenModelInfo FirstModel;
	FirstModel.Id = TEXT("gpt-4o-mini");
	FirstModel.Name = TEXT("GPT 4o Mini");
	FirstModel.ContextLength = 128000;
	FirstModel.MaxCompletionTokens = 4096;
	FirstModel.SupportedParameters = {
		TEXT("max_length"),
		TEXT("temperature"),
		TEXT("top_p"),
		TEXT("min_p"),
		TEXT("top_k"),
		TEXT("rep_pen"),
		TEXT("stop_sequence"),
		TEXT("system_prompt")
	};
	FirstModel.Pricing.Add(TEXT("prompt"), TEXT("1.0"));
	FirstModel.Pricing.Add(TEXT("completion"), TEXT("2.0"));

	FTextGenModelInfo SecondModel;
	SecondModel.Id = TEXT("koboldcpp");
	SecondModel.Name = TEXT("");

	const TArray<FTextGenModelInfo> Models = { FirstModel, SecondModel };

	const FString Summary = UTextGenSubsystem::BuildModelsDebugSummary(Models);
	const TArray<FString> Lines = UTextGenSubsystem::BuildModelsDebugLines(Models);

	TestTrue(TEXT("Summary includes canonical header"), Summary.StartsWith(TEXT("Models Summary (Pricing per 1M tokens, USD)")));
	TestTrue(TEXT("Summary preserves model display name"), Summary.Contains(TEXT("[0] GPT 4o Mini")));
	TestTrue(TEXT("Summary includes explicit model id when name differs"), Summary.Contains(TEXT("Id: gpt-4o-mini")));
	TestTrue(TEXT("Summary uses model id when name is empty"), Summary.Contains(TEXT("[1] koboldcpp")));
	TestTrue(TEXT("Summary pricing entries are sorted alphabetically"), Summary.Contains(TEXT("Pricing(1M USD): completion=2.0, prompt=1.0")));

	TestEqual(TEXT("Debug lines count matches model count"), Lines.Num(), 2);
	if (Lines.Num() >= 2)
	{
		TestTrue(TEXT("First line includes context and max completion"),
			Lines[0].Contains(TEXT("Ctx=128000")) && Lines[0].Contains(TEXT("MaxComp=4096")));
		TestTrue(TEXT("First line compacts long parameter list with plus suffix"),
			Lines[0].Contains(TEXT("Params=max_length|temperature|top_p|min_p|top_k|rep_pen|stop_sequence|+")));
		TestTrue(TEXT("Debug line pricing entries are sorted and compact"),
			Lines[0].Contains(TEXT("Pricing(1M USD): completion=2.0,prompt=1.0")));
		TestEqual(TEXT("Second line falls back to model id"), Lines[1], FString(TEXT("[1] koboldcpp")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSoCTextGenModelsEmptyFormattingTest,
	"SoC.Editor.TextGen.Runtime.ModelsEmptyFormatting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSoCTextGenModelsEmptyFormattingTest::RunTest(const FString& Parameters)
{
	const TArray<FTextGenModelInfo> EmptyModels;

	const FString Summary = UTextGenSubsystem::BuildModelsDebugSummary(EmptyModels);
	const TArray<FString> Lines = UTextGenSubsystem::BuildModelsDebugLines(EmptyModels);

	TestEqual(TEXT("Empty summary has explicit no-model marker"), Summary, FString(TEXT("(No Models)")));
	TestEqual(TEXT("Empty lines returns single marker row"), Lines.Num(), 1);
	if (Lines.Num() >= 1)
	{
		TestEqual(TEXT("Empty lines marker is stable"), Lines[0], FString(TEXT("(No Models)")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSoCTextGenParamUsageAndSettingsRoundTripTest,
	"SoC.Editor.TextGen.Runtime.ParamUsageAndSettingsRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSoCTextGenProfileMigrationPreservesActiveProviderTest,
	"SoC.Editor.TextGen.Runtime.ProfileMigrationPreservesActiveProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSoCTextGenProfileMigrationPreservesActiveProviderTest::RunTest(const FString& Parameters)
{
	FTextGenSaveGameSlotBackupScope SaveBackup(TextGenSettingsTestSlotName, TextGenSettingsTestUserIndex);
	UGameplayStatics::DeleteGameInSlot(TextGenSettingsTestSlotName, TextGenSettingsTestUserIndex);

	UTextGenSettingsSaveGame* LegacySave = Cast<UTextGenSettingsSaveGame>(
		UGameplayStatics::CreateSaveGameObject(UTextGenSettingsSaveGame::StaticClass()));
	TestNotNull(TEXT("Legacy settings save can be created"), LegacySave);
	if (!LegacySave)
	{
		return false;
	}

	const FString OpenAIProfileId = TEXT("Migration.OpenAI");
	FTextGenProviderProfile OpenAIProfile;
	OpenAIProfile.Id = OpenAIProfileId;
	OpenAIProfile.DisplayName = TEXT("Migration OpenAI");
	OpenAIProfile.Provider = ELLMProvider::OpenAI;
	OpenAIProfile.OpenAI.ChatModel = TEXT("migration-model");
	LegacySave->ProfilesById.Add(OpenAIProfileId, OpenAIProfile);
	LegacySave->ActiveProfileId = OpenAIProfileId;
	LegacySave->ActiveProvider = ELLMProvider::OpenAI;
	LegacySave->ProfileSchemaVersion = 1;
	TestTrue(TEXT("Legacy settings save is persisted"),
		UGameplayStatics::SaveGameToSlot(LegacySave, TextGenSettingsTestSlotName, TextGenSettingsTestUserIndex));

	FTextGenAutomationGameInstanceScope GameInstanceScope(*this, TEXT("TextGenMigrationWorld"));
	UGameInstance* GameInstance = GameInstanceScope.Get();
	if (!GameInstance)
	{
		return false;
	}

	TestEqual(TEXT("Schema migration preserves active profile"),
		UTextGenSubsystem::GetActiveProfileId(GameInstance), OpenAIProfileId);
	TestEqual(TEXT("Schema migration preserves active provider"),
		UTextGenSubsystem::GetActiveProviderType(GameInstance), ELLMProvider::OpenAI);
	return true;
}

bool FSoCTextGenParamUsageAndSettingsRoundTripTest::RunTest(const FString& Parameters)
{
	FTextGenSaveGameSlotBackupScope SaveBackup(TextGenSettingsTestSlotName, TextGenSettingsTestUserIndex);
	UGameplayStatics::DeleteGameInSlot(TextGenSettingsTestSlotName, TextGenSettingsTestUserIndex);

	FTextGenAutomationGameInstanceScope GameInstanceScope(*this, TEXT("TextGenParamUsageWorld"));
	UGameInstance* GameInstance = GameInstanceScope.Get();
	if (!GameInstance)
	{
		return false;
	}

	UTextGenSubsystem* TextGenSubsystem = GameInstance->GetSubsystem<UTextGenSubsystem>();
	TestNotNull(TEXT("TextGen subsystem exists"), TextGenSubsystem);
	if (!TextGenSubsystem)
	{
		return false;
	}

	const FString DefaultProfileId = UTextGenSubsystem::GetActiveProfileId(GameInstance);
	const TArray<FTextGenProviderProfileSummary> InitialProfiles = UTextGenSubsystem::GetProviderProfiles(GameInstance);
	const FTextGenProviderProfileSummary* ProjectDefault = InitialProfiles.FindByPredicate(
		[&DefaultProfileId](const FTextGenProviderProfileSummary& Profile) { return Profile.Id == DefaultProfileId; });
	TestNotNull(TEXT("Project default profile exists"), ProjectDefault);
	if (ProjectDefault)
	{
		TestTrue(TEXT("Project default profile is immutable"), ProjectDefault->bIsProjectDefault);
		TestEqual(TEXT("Project default provider is llama.cpp"), ProjectDefault->Provider, ELLMProvider::Llamacpp);
	}
	const FTextGenGenerationSettings DefaultGenerationSettings =
		UTextGenSubsystem::GetGenerationSettings(GameInstance);
	TestEqual(TEXT("Project default temperature is 1.0"), DefaultGenerationSettings.Temperature, 1.0f);
	TestEqual(TEXT("Project default top-p is 0.95"), DefaultGenerationSettings.TopP, 0.95f);
	TestEqual(TEXT("Project default min-p is 0.0"), DefaultGenerationSettings.MinP, 0.0f);
	TestEqual(TEXT("Project default top-k is 64"), DefaultGenerationSettings.TopK, 64);
	TestEqual(TEXT("Project default repetition penalty is 1.0"), DefaultGenerationSettings.RepetitionPenalty, 1.0f);
	TestEqual(TEXT("Project default presence penalty is 0.0"), DefaultGenerationSettings.PresencePenalty, 0.0f);
	TestEqual(TEXT("Project default frequency penalty is 0.0"), DefaultGenerationSettings.FrequencyPenalty, 0.0f);
	bool bFoundDefaultConfig = false;
	const FTextGenLlamacppConfig DefaultConfig = UTextGenSubsystem::GetLlamacppProfileConfig(GameInstance, DefaultProfileId, bFoundDefaultConfig);
	TestTrue(TEXT("Project default llama.cpp config exists"), bFoundDefaultConfig);
	TestEqual(TEXT("Development default uses configured project model"), DefaultConfig.ModelPath,
		GetDefault<UTextGenProjectSettings>()->DevelopmentDefaults.ModelPath);
	TestEqual(TEXT("Project default context is 12K"), DefaultConfig.ContextSize, 12288);
	TestEqual(TEXT("Project default resolves CPU threads automatically"), DefaultConfig.Threads, 0);
	TestEqual(TEXT("Project default logical batch is performance-oriented"), DefaultConfig.BatchSize, 2048);
	TestEqual(TEXT("Project default physical batch is performance-oriented"), DefaultConfig.UBatchSize, 512);
	TestEqual(TEXT("Project default backend is CUDA 13"), DefaultConfig.Backend, ETextGenLlamacppBackend::CUDA13);
	TestEqual(TEXT("Project default placement is automatic"), DefaultConfig.OffloadMode, ETextGenLlamacppOffloadMode::Auto);
	TestTrue(TEXT("Project default enables flash attention"), DefaultConfig.bFlashAttention);
	TestFalse(TEXT("Project default disables continuous batching"), DefaultConfig.bContinuousBatching);
	TestFalse(TEXT("Project default cannot be renamed"), UTextGenSubsystem::RenameProviderProfile(GameInstance, DefaultProfileId, TEXT("Changed")));
	TestFalse(TEXT("Project default cannot be deleted"), UTextGenSubsystem::DeleteProviderProfile(GameInstance, DefaultProfileId));
	TestFalse(TEXT("Project default config cannot be changed"), UTextGenSubsystem::SetLlamacppProfileConfig(GameInstance, DefaultProfileId, FTextGenLlamacppConfig()));
	const FString EditableLlamacppProfileId = UTextGenSubsystem::CreateProviderProfile(GameInstance, TEXT("Editable llama.cpp"), ELLMProvider::Llamacpp);
	bool bFoundEditableLlamacppConfig = false;
	const FTextGenLlamacppConfig EditableLlamacppConfig = UTextGenSubsystem::GetLlamacppProfileConfig(
		GameInstance, EditableLlamacppProfileId, bFoundEditableLlamacppConfig);
	TestTrue(TEXT("Editable llama.cpp profile config exists"), bFoundEditableLlamacppConfig);
	TestEqual(TEXT("Editable llama.cpp profile inherits the default GGUF"), EditableLlamacppConfig.ModelPath, DefaultConfig.ModelPath);
	TestFalse(TEXT("Editable llama.cpp profile disables reasoning by default"), EditableLlamacppConfig.bEnableReasoning);
	TestEqual(TEXT("Editable llama.cpp profile has the default reasoning budget"), EditableLlamacppConfig.ReasoningBudgetTokens, 256);

	const FString EditableProfileId = UTextGenSubsystem::CreateProviderProfile(GameInstance, TEXT("Editable Test Profile"), ELLMProvider::KoboldCpp);
	TestFalse(TEXT("Editable profile was created"), EditableProfileId.IsEmpty());
	TestTrue(TEXT("Editable profile can become active"), UTextGenSubsystem::SetActiveProfile(GameInstance, EditableProfileId));

	FTextGenParamUsageConfig UsageConfig;
	UsageConfig.bUseConversationMessages = false;
	UsageConfig.bUseStopSequences = false;
	UsageConfig.bUseMaxLength = false;
	UsageConfig.bUseTemperature = true;
	UsageConfig.bUseTopP = false;
	UsageConfig.bUseMinP = true;
	UsageConfig.bUseTopK = false;
	UsageConfig.bUseRepetitionPenalty = true;
	UsageConfig.bUseFrequencyPenalty = false;
	UsageConfig.bUsePresencePenalty = true;
	UsageConfig.SystemPromptMode = ESystemPromptMode::Ignore;

	UTextGenSubsystem::SetGenerationParamUsage(GameInstance, UsageConfig);
	const FTextGenParamUsageConfig LoadedUsageConfig = UTextGenSubsystem::GetGenerationParamUsage(GameInstance);

	TestEqual(TEXT("System prompt mode round-trips"), LoadedUsageConfig.SystemPromptMode, ESystemPromptMode::Ignore);
	TestFalse(TEXT("Conversation messages usage flag round-trips"), LoadedUsageConfig.bUseConversationMessages);
	TestFalse(TEXT("Stop sequences usage flag round-trips"), LoadedUsageConfig.bUseStopSequences);
	TestFalse(TEXT("Max length usage flag round-trips"), LoadedUsageConfig.bUseMaxLength);
	TestFalse(TEXT("TopP usage flag round-trips"), LoadedUsageConfig.bUseTopP);
	TestFalse(TEXT("TopK usage flag round-trips"), LoadedUsageConfig.bUseTopK);
	TestFalse(TEXT("Frequency penalty usage flag round-trips"), LoadedUsageConfig.bUseFrequencyPenalty);

	FTextGenGenerationSettings InputSettings;
	InputSettings.MaxLength = 222;
	InputSettings.Temperature = 0.42f;
	InputSettings.TopP = 0.73f;
	InputSettings.MinP = 0.11f;
	InputSettings.TopK = 77;
	InputSettings.RepetitionPenalty = 1.23f;
	InputSettings.FrequencyPenalty = 0.45f;
	InputSettings.PresencePenalty = 0.67f;
	InputSettings.StopSequences = { TEXT("###"), TEXT("END") };

	UTextGenSubsystem::SetGenerationSettings(GameInstance, InputSettings);
	const FTextGenGenerationSettings LoadedSettings = UTextGenSubsystem::GetGenerationSettings(GameInstance);

	TestEqual(TEXT("Generation settings MaxLength round-trips"), LoadedSettings.MaxLength, 222);
	TestEqual(TEXT("Generation settings Temperature round-trips"), LoadedSettings.Temperature, 0.42f);
	TestEqual(TEXT("Generation settings TopP round-trips"), LoadedSettings.TopP, 0.73f);
	TestEqual(TEXT("Generation settings MinP round-trips"), LoadedSettings.MinP, 0.11f);
	TestEqual(TEXT("Generation settings TopK round-trips"), LoadedSettings.TopK, 77);
	TestEqual(TEXT("Generation settings RepetitionPenalty round-trips"), LoadedSettings.RepetitionPenalty, 1.23f);
	TestEqual(TEXT("Generation settings FrequencyPenalty round-trips"), LoadedSettings.FrequencyPenalty, 0.45f);
	TestEqual(TEXT("Generation settings PresencePenalty round-trips"), LoadedSettings.PresencePenalty, 0.67f);
	TestEqual(TEXT("Generation settings stop sequence count round-trips"), LoadedSettings.StopSequences.Num(), 2);
	if (LoadedSettings.StopSequences.Num() >= 2)
	{
		TestEqual(TEXT("First stop sequence round-trips"), LoadedSettings.StopSequences[0], FString(TEXT("###")));
		TestEqual(TEXT("Second stop sequence round-trips"), LoadedSettings.StopSequences[1], FString(TEXT("END")));
	}

	TestTrue(TEXT("Deleting the active profile selects a valid fallback"),
		UTextGenSubsystem::DeleteProviderProfile(GameInstance, EditableProfileId));
	TestNotEqual(TEXT("Deleted profile is no longer active"),
		UTextGenSubsystem::GetActiveProfileId(GameInstance), EditableProfileId);
	TestEqual(TEXT("Fallback provider remains reconciled"),
		UTextGenSubsystem::GetActiveProviderType(GameInstance), ELLMProvider::KoboldCpp);
	TestTrue(TEXT("Editable llama.cpp test profile is deleted"),
		UTextGenSubsystem::DeleteProviderProfile(GameInstance, EditableLlamacppProfileId));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTextGenOpenAIReasoningStreamAutomationTest,
	"TextGen.Runtime.OpenAI.ReasoningStream",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTextGenOpenAIReasoningStreamAutomationTest::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FString Content;
	FString Reasoning;
	bool bIsFinal = false;

	TestTrue(TEXT("Reasoning-only delta is accepted"), Provider.ExtractStreamDelta(
		TEXT("{\"choices\":[{\"finish_reason\":null,\"delta\":{\"content\":null,\"reasoning_content\":\"thinking\"}}]}"),
		Content, Reasoning, bIsFinal));
	TestTrue(TEXT("Reasoning-only delta has no content"), Content.IsEmpty());
	TestEqual(TEXT("Reasoning text is extracted"), Reasoning, FString(TEXT("thinking")));
	TestFalse(TEXT("Reasoning-only delta is not final"), bIsFinal);

	TestTrue(TEXT("Content delta is accepted"), Provider.ExtractStreamDelta(
		TEXT("{\"choices\":[{\"finish_reason\":null,\"delta\":{\"content\":\"answer\"}}]}"),
		Content, Reasoning, bIsFinal));
	TestEqual(TEXT("Final answer text is extracted"), Content, FString(TEXT("answer")));
	TestTrue(TEXT("Content delta has no reasoning"), Reasoning.IsEmpty());

	TestTrue(TEXT("Empty final delta is accepted"), Provider.ExtractStreamDelta(
		TEXT("{\"choices\":[{\"finish_reason\":\"stop\",\"delta\":{\"content\":null}}]}"),
		Content, Reasoning, bIsFinal));
	TestTrue(TEXT("Final delta is marked complete"), bIsFinal);
	return true;
}

#endif
