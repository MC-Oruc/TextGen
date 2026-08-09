// Copyright <--\, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "TextGen/TextGenProvider.h"

class UTextGenSubsystem;
struct FTextGenGenerationParams;
struct FTextGenGenerationSettings;

class FOpenAIProvider : public ITextGenProvider
{
public:
    virtual void BuildGenerateRequestData(const UTextGenSubsystem* Subsystem, const FTextGenGenerationParams& Params, const FTextGenGenerationSettings& Settings,
        FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const override;

    virtual void BuildAbortRequestData(const UTextGenSubsystem* Subsystem,
        bool& bHasServerAbort, FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const override;

    virtual void BuildTokenCountRequestData(const UTextGenSubsystem* Subsystem, const FString& Text,
        FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const override;

    virtual void BuildVersionRequestData(const UTextGenSubsystem* Subsystem,
        FString& OutEndpoint, FString& OutVerb) const override;

    virtual void BuildMaxContextRequestData(const UTextGenSubsystem* Subsystem,
        bool& bImmediate, int32& OutImmediateValue, FString& OutEndpoint, FString& OutVerb) const override;

    virtual void BuildHealthRequestData(const UTextGenSubsystem* Subsystem,
        FString& OutEndpoint, FString& OutVerb) const override;

    virtual bool ParseTokenCountResponse(const FString& ResponseString, int32& OutTokenCount, FString& OutError) const override;
    virtual bool ParseVersionResponse(const FString& ResponseString, FString& OutVersion, FString& OutBuildInfo, FString& OutError) const override;
    virtual bool ParseMaxContextResponse(const FString& ResponseString, int32& OutMaxContext, FString& OutError) const override;
    virtual bool ParseHealthResponse(const FString& ResponseString, bool& bOutHealthy, FString& OutError) const override;

    virtual bool ParseModelsResponse(const FString& ResponseString, TArray<FTextGenModelInfo>& OutModels, FString& OutError) const override;

    virtual bool ExtractStreamDelta(const FString& JSONString, FString& OutDeltaText, FString& OutReasoningText, bool& OutIsFinal) const override;
};
