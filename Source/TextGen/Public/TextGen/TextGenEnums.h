// Copyright <--\, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "TextGenEnums.generated.h"

/** Common roles used to label messages/prompts across the TextGen system. */
UENUM(BlueprintType)
enum class EPromptRole : uint8
{
	System    UMETA(DisplayName = "System"),
	User      UMETA(DisplayName = "User"),
	Assistant UMETA(DisplayName = "Assistant"),
	Tool      UMETA(DisplayName = "Tool"),
	ToolCall  UMETA(DisplayName = "Tool Call")
};

UENUM(BlueprintType)
enum class ETextGenResult : uint8
{
	Success         UMETA(DisplayName = "Success"),
	NetworkError    UMETA(DisplayName = "Network Error"),
	ParseError      UMETA(DisplayName = "Parse Error"),
	Timeout         UMETA(DisplayName = "Timeout"),
	Aborted         UMETA(DisplayName = "Aborted"),
	Unknown         UMETA(DisplayName = "Unknown Error")
};

UENUM(BlueprintType)
enum class ELLMProvider : uint8
{
	KoboldCpp   UMETA(DisplayName = "KoboldCpp"),
	OpenAI      UMETA(DisplayName = "OpenAI API"),
	Llamacpp    UMETA(DisplayName = "llama.cpp")
};

UENUM(BlueprintType)
enum class ETextGenManagedLifecycleMode : uint8
{
	EditorSession UMETA(DisplayName = "Editor Session"),
	PIESession    UMETA(DisplayName = "PIE Session"),
	Manual        UMETA(DisplayName = "Manual")
};

UENUM(BlueprintType)
enum class ETextGenLlamacppKVCacheType : uint8
{
	F32   UMETA(DisplayName = "F32"),
	F16   UMETA(DisplayName = "F16"),
	BF16  UMETA(DisplayName = "BF16"),
	Q8_0  UMETA(DisplayName = "Q8_0"),
	Q4_0  UMETA(DisplayName = "Q4_0"),
	Q4_1  UMETA(DisplayName = "Q4_1"),
	IQ4_NL UMETA(DisplayName = "IQ4_NL"),
	Q5_0  UMETA(DisplayName = "Q5_0"),
	Q5_1  UMETA(DisplayName = "Q5_1")
};

UENUM(BlueprintType)
enum class ETextGenLlamacppBackend : uint8
{
	CUDA13 UMETA(DisplayName = "CUDA 13"),
	CUDA12 UMETA(DisplayName = "CUDA 12"),
	Vulkan UMETA(DisplayName = "Vulkan")
};

UENUM(BlueprintType)
enum class ETextGenLlamacppOffloadMode : uint8
{
	Auto           UMETA(DisplayName = "Automatic"),
	DenseSharedGPU UMETA(DisplayName = "Dense + Shared GPU"),
	DenseOnlyGPU   UMETA(DisplayName = "Dense Only GPU"),
	CPUOnly        UMETA(DisplayName = "CPU Only")
};

UENUM(BlueprintType)
enum class ETextGenLlamacppInstallComponent : uint8
{
	Runtime          UMETA(DisplayName = "Runtime"),
	CudaDependencies UMETA(DisplayName = "CUDA Dependencies")
};

UENUM(BlueprintType)
enum class ETextGenLocalServiceState : uint8
{
	Disabled,
	Stopped,
	Starting,
	LoadingModel,
	Ready,
	Unhealthy,
	Stopping,
	Failed
};

UENUM(BlueprintType)
enum class ETextGenConversationSlotPreference : uint8
{
	None,
	Default,
	Progress
};

UENUM(BlueprintType)
enum class ESystemPromptMode : uint8
{
	SendAsSystem        UMETA(DisplayName = "Send as System Message"),
	EmbedInFirstUser    UMETA(DisplayName = "Embed in First User Message"),
	Ignore              UMETA(DisplayName = "Ignore System Prompt")
};
