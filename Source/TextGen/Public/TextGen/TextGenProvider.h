// Copyright <--\, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UTextGenSubsystem;
struct FTextGenGenerationParams;
struct FTextGenGenerationSettings;
struct FTextGenModelInfo;

/**
 * Lightweight provider interface for building requests and parsing responses
 * Implementations are stateless and use data from the Subsystem
 */
class ITextGenProvider
{
public:
    virtual ~ITextGenProvider() {}

    // Build requests
    virtual void BuildGenerateRequestData(const UTextGenSubsystem* Subsystem, const FTextGenGenerationParams& Params, const FTextGenGenerationSettings& Settings,
        FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const = 0;

    virtual void BuildAbortRequestData(const UTextGenSubsystem* Subsystem,
        bool& bHasServerAbort, FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const = 0;

    virtual void BuildTokenCountRequestData(const UTextGenSubsystem* Subsystem, const FString& Text,
        FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const = 0;

    virtual void BuildVersionRequestData(const UTextGenSubsystem* Subsystem,
        FString& OutEndpoint, FString& OutVerb) const = 0;

    virtual void BuildMaxContextRequestData(const UTextGenSubsystem* Subsystem,
        bool& bImmediate, int32& OutImmediateValue, FString& OutEndpoint, FString& OutVerb) const = 0;

    virtual void BuildHealthRequestData(const UTextGenSubsystem* Subsystem,
        FString& OutEndpoint, FString& OutVerb) const = 0;

    // Parse responses
    virtual bool ParseTokenCountResponse(const FString& ResponseString, int32& OutTokenCount, FString& OutError) const = 0;
    virtual bool ParseVersionResponse(const FString& ResponseString, FString& OutVersion, FString& OutBuildInfo, FString& OutError) const = 0;
    virtual bool ParseMaxContextResponse(const FString& ResponseString, int32& OutMaxContext, FString& OutError) const = 0;
    virtual bool ParseHealthResponse(const FString& ResponseString, bool& bOutHealthy, FString& OutError) const = 0;

    // Models listing
    virtual bool ParseModelsResponse(const FString& ResponseString, TArray<FTextGenModelInfo>& OutModels, FString& OutError) const = 0;

    // Streaming delta extraction
    virtual bool ExtractStreamDelta(const FString& JSONString, FString& OutDeltaText, FString& OutReasoningText, bool& OutIsFinal) const = 0;
};
