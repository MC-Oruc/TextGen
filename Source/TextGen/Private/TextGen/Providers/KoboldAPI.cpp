// Copyright <--\, Inc. All Rights Reserved.

#include "TextGen/Providers/KoboldAPI.h"
#include "TextGen/TextGenSubsystem.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Local helper for KoboldCpp provider to prefix system prompt once
static FString MakeSystemPrompt_Kobold(const FString& Raw)
{
    if (Raw.IsEmpty())
    {
        return FString();
    }
    if (Raw.StartsWith(TEXT("{{[SYSTEM]}}")))
    {
        return Raw;
    }
    return FString::Printf(TEXT("{{[SYSTEM]}}%s"), *Raw);
}

//////////////////////////////////////////////////////////////////////////////
// FKoboldCppProvider implementation

void FKoboldCppProvider::BuildGenerateRequestData(const UTextGenSubsystem* Subsystem, const FTextGenGenerationParams& Params, const FTextGenGenerationSettings& Settings,
    FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const
{
    TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
    FString WrappedPrompt;
    if (Params.ConversationMessages.Num() > 0)
    {
        // Reconstruct conversation history in Kobold instruction format
        FString History;
        for (int32 i = 0; i < Params.ConversationMessages.Num(); ++i)
        {
            const FConversationMessage& Msg = Params.ConversationMessages[i];
            if (Msg.Role == EPromptRole::User)
            {
                History += FString::Printf(TEXT("{{[INPUT]}}%s{{[OUTPUT]}}"), *Msg.Message);
                // If next is assistant, append its text immediately (without extra tokens) then advance
                if (i + 1 < Params.ConversationMessages.Num() && Params.ConversationMessages[i + 1].Role == EPromptRole::Assistant)
                {
                    History += Params.ConversationMessages[i + 1].Message;
                    ++i; // skip assistant we just consumed
                }
            }
            else if (Msg.Role == EPromptRole::Assistant)
            {
                // Assistant without prior user token – just append raw
                History += Msg.Message;
            }
            // System messages ignored here (system handled separately via system_prompt)
        }
        // Append current user prompt awaiting new assistant completion
        History += FString::Printf(TEXT("{{[INPUT]}}%s{{[OUTPUT]}}"), *Params.Prompt);
        WrappedPrompt = MoveTemp(History);
    }
    else
    {
        WrappedPrompt = FString::Printf(TEXT("{{[INPUT]}}%s{{[OUTPUT]}}"), *Params.Prompt);
    }
    JsonObject->SetStringField(TEXT("prompt"), WrappedPrompt);
    if (Settings.MaxLength > 0) JsonObject->SetNumberField(TEXT("max_length"), Settings.MaxLength);
    if (Settings.Temperature >= 0.f) JsonObject->SetNumberField(TEXT("temperature"), Settings.Temperature);
    if (Settings.TopP >= 0.f) JsonObject->SetNumberField(TEXT("top_p"), Settings.TopP);
    if (Settings.MinP >= 0.f && Settings.MinP <= 1.f) JsonObject->SetNumberField(TEXT("min_p"), Settings.MinP);
    if (Settings.TopK >= 0) JsonObject->SetNumberField(TEXT("top_k"), Settings.TopK);
    if (Settings.RepetitionPenalty >= 0.f) JsonObject->SetNumberField(TEXT("rep_pen"), Settings.RepetitionPenalty);
    if (Settings.PresencePenalty >= -2.f && Settings.PresencePenalty <= 2.f) JsonObject->SetNumberField(TEXT("presence_penalty"), Settings.PresencePenalty);

    // KoboldCpp uses the "memory" field as the system prompt.
    // Standard API usage keeps the key present even when empty (sending empty vs omitting is functionally equivalent).
    const FString SystemPrompt = MakeSystemPrompt_Kobold(Params.SystemPrompt);
    JsonObject->SetStringField(TEXT("memory"), SystemPrompt);

    if (Settings.StopSequences.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> StopArray;
        for (const FString& StopSeq : Settings.StopSequences)
        {
            StopArray.Add(MakeShareable(new FJsonValueString(StopSeq)));
        }
        JsonObject->SetArrayField(TEXT("stop_sequence"), StopArray);
    }
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutBodyJson);
    FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
    OutEndpoint = TEXT("/api/extra/generate/stream");
    OutVerb = TEXT("POST");
}

void FKoboldCppProvider::BuildAbortRequestData(const UTextGenSubsystem* Subsystem,
    bool& bHasServerAbort, FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const
{
    bHasServerAbort = true;
    OutEndpoint = TEXT("/api/extra/abort");
    OutVerb = TEXT("POST");
    OutBodyJson = TEXT("");
}

void FKoboldCppProvider::BuildTokenCountRequestData(const UTextGenSubsystem* Subsystem, const FString& Text,
    FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const
{
    TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
    JsonObject->SetStringField(TEXT("prompt"), Text);
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutBodyJson);
    FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
    OutEndpoint = TEXT("/api/extra/tokencount");
    OutVerb = TEXT("POST");
}

void FKoboldCppProvider::BuildVersionRequestData(const UTextGenSubsystem* Subsystem,
    FString& OutEndpoint, FString& OutVerb) const
{
    OutEndpoint = TEXT("/api/extra/version");
    OutVerb = TEXT("GET");
}

void FKoboldCppProvider::BuildMaxContextRequestData(const UTextGenSubsystem* Subsystem,
    bool& bImmediate, int32& OutImmediateValue, FString& OutEndpoint, FString& OutVerb) const
{
    bImmediate = false;
    OutImmediateValue = 0;
    OutEndpoint = TEXT("/api/extra/true_max_context_length");
    OutVerb = TEXT("GET");
}

void FKoboldCppProvider::BuildHealthRequestData(const UTextGenSubsystem* Subsystem,
    FString& OutEndpoint, FString& OutVerb) const
{
    OutEndpoint = TEXT("/api/extra/version");
    OutVerb = TEXT("GET");
}

bool FKoboldCppProvider::ParseTokenCountResponse(const FString& ResponseString, int32& OutTokenCount, FString& OutError) const
{
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
    if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
    {
        OutTokenCount = JsonObject->GetIntegerField(TEXT("value"));
        return true;
    }
    OutError = TEXT("Failed to parse token count response");
    return false;
}

bool FKoboldCppProvider::ParseVersionResponse(const FString& ResponseString, FString& OutVersion, FString& OutBuildInfo, FString& OutError) const
{
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
    if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
    {
        OutVersion = JsonObject->GetStringField(TEXT("result"));
        return true;
    }
    OutError = TEXT("Failed to parse version response");
    return false;
}

bool FKoboldCppProvider::ParseMaxContextResponse(const FString& ResponseString, int32& OutMaxContext, FString& OutError) const
{
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
    if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
    {
        OutMaxContext = JsonObject->GetIntegerField(TEXT("value"));
        return true;
    }
    OutError = TEXT("Failed to parse max context response");
    return false;
}

bool FKoboldCppProvider::ParseHealthResponse(const FString& ResponseString, bool& bOutHealthy, FString& OutError) const
{
    FString Version;
    FString Build;
    if (ParseVersionResponse(ResponseString, Version, Build, OutError))
    {
        bOutHealthy = !Version.IsEmpty();
        return bOutHealthy;
    }
    bOutHealthy = false;
    return false;
}

bool FKoboldCppProvider::ParseModelsResponse(const FString& ResponseString, TArray<FTextGenModelInfo>& OutModels, FString& OutError) const
{
    // KoboldCpp model listing is currently implemented in the subsystem as a multi-endpoint aggregation.
    OutModels.Reset();
    OutError = TEXT("KoboldCpp models parsing not supported via ParseModelsResponse (handled by subsystem aggregator)");
    return false;
}

bool FKoboldCppProvider::ExtractStreamDelta(const FString& JSONString, FString& OutDeltaText, FString& OutReasoningText, bool& OutIsFinal) const
{
    OutReasoningText.Reset();
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JSONString);
    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        return false;
    }

    OutIsFinal = false;
    if (JsonObject->HasField(TEXT("token")))
    {
        OutDeltaText = JsonObject->GetStringField(TEXT("token"));
        if (JsonObject->HasField(TEXT("final")))
        {
            OutIsFinal = JsonObject->GetBoolField(TEXT("final"));
        }
        return !OutDeltaText.IsEmpty();
    }
    if (JsonObject->HasField(TEXT("text")))
    {
        OutDeltaText = JsonObject->GetStringField(TEXT("text"));
        if (JsonObject->HasField(TEXT("finished")))
        {
            OutIsFinal = JsonObject->GetBoolField(TEXT("finished"));
        }
        else if (JsonObject->HasField(TEXT("final")))
        {
            OutIsFinal = JsonObject->GetBoolField(TEXT("final"));
        }
        return !OutDeltaText.IsEmpty();
    }
    return false;
}
