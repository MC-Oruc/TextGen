// Copyright <--\, Inc. All Rights Reserved.

#include "TextGen/Providers/OpenAIAPI.h"
#include "TextGen/TextGenSubsystem.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

void FOpenAIProvider::BuildGenerateRequestData(const UTextGenSubsystem* Subsystem, const FTextGenGenerationParams& Params, const FTextGenGenerationSettings& Settings,
    FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const
{
    TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
    JsonObject->SetStringField(TEXT("model"), Subsystem->OpenAIChatModel);
    JsonObject->SetBoolField(TEXT("stream"), true);

    TArray<TSharedPtr<FJsonValue>> Messages;
    // Send system prompt as-is (no provider-specific prefixes)
    {
        const FString SystemContent = Params.SystemPrompt;
        if (!SystemContent.IsEmpty())
        {
            TSharedPtr<FJsonObject> SystemMsg = MakeShareable(new FJsonObject);
            SystemMsg->SetStringField(TEXT("role"), TEXT("system"));
            SystemMsg->SetStringField(TEXT("content"), SystemContent);
            Messages.Add(MakeShareable(new FJsonValueObject(SystemMsg)));
        }
    }
    // Inject prior conversation messages (already ordered oldest->newest)
    if (Params.ConversationMessages.Num() > 0)
    {
        for (const FConversationMessage& ConvMsg : Params.ConversationMessages)
        {
            if (ConvMsg.Message.IsEmpty())
            {
                continue;
            }
            // Tool calling is not supported; ignore non-chat roles in history to avoid polluting context.
            if (ConvMsg.Role != EPromptRole::User && ConvMsg.Role != EPromptRole::Assistant && ConvMsg.Role != EPromptRole::System)
            {
                continue;
            }
            FString RoleStr;
            switch (ConvMsg.Role)
            {
            case EPromptRole::User: RoleStr = TEXT("user"); break;
            case EPromptRole::Assistant: RoleStr = TEXT("assistant"); break;
            case EPromptRole::System: RoleStr = TEXT("system"); break;
            default: continue;
            }
            TSharedPtr<FJsonObject> HistMsg = MakeShareable(new FJsonObject);
            HistMsg->SetStringField(TEXT("role"), RoleStr);
            HistMsg->SetStringField(TEXT("content"), ConvMsg.Message);
            Messages.Add(MakeShareable(new FJsonValueObject(HistMsg)));
        }
    }
    TSharedPtr<FJsonObject> UserMsg = MakeShareable(new FJsonObject);
    UserMsg->SetStringField(TEXT("role"), TEXT("user"));
    UserMsg->SetStringField(TEXT("content"), Params.Prompt);
    Messages.Add(MakeShareable(new FJsonValueObject(UserMsg)));
    JsonObject->SetArrayField(TEXT("messages"), Messages);

    if (Settings.MaxLength > 0)
    {
        const int32 MaxTokens = Subsystem->ActiveProvider == ELLMProvider::Llamacpp
            && Subsystem->LlamacppConfig.bEnableReasoning
            ? Settings.MaxLength + FMath::Clamp(Subsystem->LlamacppConfig.ReasoningBudgetTokens, 128, 4096)
            : Settings.MaxLength;
        JsonObject->SetNumberField(TEXT("max_tokens"), MaxTokens);
    }
    if (Settings.Temperature >= 0.f) { JsonObject->SetNumberField(TEXT("temperature"), Settings.Temperature); }
    if (Settings.TopP >= 0.f) { JsonObject->SetNumberField(TEXT("top_p"), Settings.TopP); }
    if (Settings.MinP >= 0.f && Settings.MinP <= 1.f) { JsonObject->SetNumberField(TEXT("min_p"), Settings.MinP); }
    if (Settings.FrequencyPenalty >= -2.f && Settings.FrequencyPenalty <= 2.f) { JsonObject->SetNumberField(TEXT("frequency_penalty"), Settings.FrequencyPenalty); }
    if (Settings.PresencePenalty >= -2.f && Settings.PresencePenalty <= 2.f) { JsonObject->SetNumberField(TEXT("presence_penalty"), Settings.PresencePenalty); }

    if (Settings.StopSequences.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> StopArray;
        for (const FString& StopSeq : Settings.StopSequences)
        {
            StopArray.Add(MakeShareable(new FJsonValueString(StopSeq)));
        }
        JsonObject->SetArrayField(TEXT("stop"), StopArray);
    }

    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutBodyJson);
    FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
    OutEndpoint = TEXT("/v1/chat/completions");
    OutVerb = TEXT("POST");
}

void FOpenAIProvider::BuildAbortRequestData(const UTextGenSubsystem* Subsystem,
    bool& bHasServerAbort, FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const
{
    bHasServerAbort = false;
    OutEndpoint = TEXT("");
    OutVerb = TEXT("");
    OutBodyJson = TEXT("");
}

void FOpenAIProvider::BuildTokenCountRequestData(const UTextGenSubsystem* Subsystem, const FString& Text,
    FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const
{
    TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
    JsonObject->SetStringField(TEXT("model"), Subsystem->OpenAIChatModel);
    JsonObject->SetStringField(TEXT("input"), Text);
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutBodyJson);
    FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
    OutEndpoint = TEXT("/v1/embeddings");
    OutVerb = TEXT("POST");
}

void FOpenAIProvider::BuildVersionRequestData(const UTextGenSubsystem* Subsystem,
    FString& OutEndpoint, FString& OutVerb) const
{
    OutEndpoint = TEXT("/v1/models");
    OutVerb = TEXT("GET");
}

void FOpenAIProvider::BuildMaxContextRequestData(const UTextGenSubsystem* Subsystem,
    bool& bImmediate, int32& OutImmediateValue, FString& OutEndpoint, FString& OutVerb) const
{
    bImmediate = true;
    OutImmediateValue = Subsystem->OpenAIContextWindow;
    OutEndpoint = TEXT("");
    OutVerb = TEXT("");
}

void FOpenAIProvider::BuildHealthRequestData(const UTextGenSubsystem* Subsystem,
    FString& OutEndpoint, FString& OutVerb) const
{
    OutEndpoint = TEXT("/v1/models");
    OutVerb = TEXT("GET");
}

bool FOpenAIProvider::ParseTokenCountResponse(const FString& ResponseString, int32& OutTokenCount, FString& OutError) const
{
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
    if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
    {
        if (JsonObject->HasField(TEXT("usage")))
        {
            TSharedPtr<FJsonObject> Usage = JsonObject->GetObjectField(TEXT("usage"));
            if (Usage.IsValid() && Usage->HasField(TEXT("prompt_tokens")))
            {
                OutTokenCount = Usage->GetIntegerField(TEXT("prompt_tokens"));
                return true;
            }
        }
    }
    OutError = TEXT("Failed to parse embeddings usage for token count");
    return false;
}

bool FOpenAIProvider::ParseVersionResponse(const FString& ResponseString, FString& OutVersion, FString& OutBuildInfo, FString& OutError) const
{
    // We don't have a simple version field; expose first model id if present (OpenAI API compatible)
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
    if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
    {
        OutVersion = TEXT("OpenAI");
        if (JsonObject->HasField(TEXT("data")))
        {
            const TArray<TSharedPtr<FJsonValue>>* DataArrayPtr;
            if (JsonObject->TryGetArrayField(TEXT("data"), DataArrayPtr))
            {
                if (DataArrayPtr && DataArrayPtr->Num() > 0)
                {
                    TSharedPtr<FJsonObject> First = (*DataArrayPtr)[0]->AsObject();
                    if (First.IsValid() && First->HasField(TEXT("id")))
                    {
                        OutBuildInfo = First->GetStringField(TEXT("id"));
                    }
                }
            }
        }
        return true;
    }
    OutError = TEXT("Failed to parse models response");
    return false;
}

bool FOpenAIProvider::ParseMaxContextResponse(const FString& ResponseString, int32& OutMaxContext, FString& OutError) const
{
    // Not used (immediate path)
    OutError = TEXT("");
    return false;
}

bool FOpenAIProvider::ParseHealthResponse(const FString& ResponseString, bool& bOutHealthy, FString& OutError) const
{
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
    if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
    {
        // Per https://platform.openai.com/docs/api-reference/responses/object, a list of models in 'data' is healthy
        bOutHealthy = JsonObject->HasField(TEXT("data"));
        return bOutHealthy;
    }
    OutError = TEXT("Failed to parse health response");
    bOutHealthy = false;
    return false;
}

bool FOpenAIProvider::ParseModelsResponse(const FString& ResponseString, TArray<FTextGenModelInfo>& OutModels, FString& OutError) const
{
    OutModels.Reset();

    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        OutError = TEXT("Failed to deserialize models response JSON");
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* DataArrayPtr = nullptr;
    if (!JsonObject->TryGetArrayField(TEXT("data"), DataArrayPtr) || !DataArrayPtr)
    {
        OutError = TEXT("Models response missing 'data' array");
        return false;
    }

    auto ScalePricePerMillion = [](const FString& Raw)->FString
    {
        if (Raw.IsEmpty()) return Raw;
        double Value = 0.0;
        if (!LexTryParseString(Value, *Raw))
        {
            return Raw;
        }
        const double Scaled = Value * 1000000.0;
        FString Out = FString::Printf(TEXT("%.6f"), Scaled);
        while (Out.Contains(TEXT(".")) && (Out.EndsWith(TEXT("0")) || Out.EndsWith(TEXT("."))))
        {
            if (Out.EndsWith(TEXT("."))) { Out.LeftChopInline(1); break; }
            Out.LeftChopInline(1);
        }
        return Out;
    };

    auto TryGetJsonIntSafely = [](const TSharedPtr<FJsonObject>& JsonObj, const FString& FieldName, int32& OutValue) -> bool
    {
        if (!JsonObj.IsValid()) return false;
        const TSharedPtr<FJsonValue> FieldVal = JsonObj->TryGetField(FieldName);
        if (FieldVal.IsValid() && FieldVal->Type == EJson::Number)
        {
            OutValue = static_cast<int32>(FieldVal->AsNumber());
            return true;
        }
        return false;
    };

    for (const TSharedPtr<FJsonValue>& Val : *DataArrayPtr)
    {
        if (!Val.IsValid() || Val->Type != EJson::Object)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Obj = Val->AsObject();
        if (!Obj.IsValid())
        {
            continue;
        }

        FTextGenModelInfo Info;
        Info.Id = Obj->GetStringField(TEXT("id"));
        if (Obj->HasField(TEXT("name")))
        {
            Info.Name = Obj->GetStringField(TEXT("name"));
        }
        else
        {
            Info.Name = Info.Id;
        }

        // context length sources: top_provider.context_length OR context_length at root
        int32 ContextLen = 0;
        if (TryGetJsonIntSafely(Obj, TEXT("context_length"), ContextLen))
        {
            Info.ContextLength = ContextLen;
        }

        if (Obj->HasField(TEXT("top_provider")))
        {
            TSharedPtr<FJsonObject> TopProv = Obj->GetObjectField(TEXT("top_provider"));
            if (TopProv.IsValid())
            {
                if (Info.ContextLength == 0)
                {
                    TryGetJsonIntSafely(TopProv, TEXT("context_length"), Info.ContextLength);
                }
                TryGetJsonIntSafely(TopProv, TEXT("max_completion_tokens"), Info.MaxCompletionTokens);
            }
        }

        if (Obj->HasField(TEXT("supported_parameters")))
        {
            const TArray<TSharedPtr<FJsonValue>>* ParamsArray = nullptr;
            if (Obj->TryGetArrayField(TEXT("supported_parameters"), ParamsArray) && ParamsArray)
            {
                for (const TSharedPtr<FJsonValue>& PVal : *ParamsArray)
                {
                    FString PStr;
                    if (PVal->TryGetString(PStr))
                    {
                        Info.SupportedParameters.Add(PStr);
                    }
                }
            }
        }

        if (Obj->HasField(TEXT("pricing")))
        {
            TSharedPtr<FJsonObject> PricingObj = Obj->GetObjectField(TEXT("pricing"));
            if (PricingObj.IsValid())
            {
                for (const auto& Pair : PricingObj->Values)
                {
                    const FString Key(Pair.Key.ToView());
                    FString ValStr;

                    if (Pair.Value->TryGetString(ValStr))
                    {
                        ValStr = ScalePricePerMillion(ValStr);
                        Info.Pricing.Add(Key, ValStr);
                    }
                    else if (Pair.Value->Type == EJson::Number)
                    {
                        ValStr = FString::SanitizeFloat(Pair.Value->AsNumber());
                        ValStr = ScalePricePerMillion(ValStr);
                        Info.Pricing.Add(Key, ValStr);
                    }
                }

                if (const FString* PromptPriceStr = Info.Pricing.Find(TEXT("prompt")))
                {
                    double ParsedPriceVal = 0.0;
                    if (LexTryParseString(ParsedPriceVal, **PromptPriceStr))
                    {
                        Info.PromptCostPerMToken = ParsedPriceVal;
                        Info.bHasPricing = true;
                    }
                }

                if (const FString* CompPriceStr = Info.Pricing.Find(TEXT("completion")))
                {
                    double ParsedPriceVal = 0.0;
                    if (LexTryParseString(ParsedPriceVal, **CompPriceStr))
                    {
                        Info.CompletionCostPerMToken = ParsedPriceVal;
                        Info.bHasPricing = true;
                    }
                }
            }
        }

        // Fallback: OpenAI /v1/models does not (currently) expose supported_parameters.
        if (Info.SupportedParameters.Num() == 0)
        {
            static const TCHAR* DefaultParams[] = {
                TEXT("max_tokens"), TEXT("temperature"), TEXT("top_p"), TEXT("min_p"), TEXT("frequency_penalty"),
                TEXT("presence_penalty"), TEXT("stop"), TEXT("seed")
            };
            for (const TCHAR* P : DefaultParams)
            {
                Info.SupportedParameters.Add(P);
            }
        }

        OutModels.Add(MoveTemp(Info));
    }

    if (OutModels.Num() == 0)
    {
        OutError = TEXT("No models parsed from response");
        return false;
    }

    return true;
}

bool FOpenAIProvider::ExtractStreamDelta(const FString& JSONString, FString& OutDeltaText, FString& OutReasoningText, bool& OutIsFinal) const
{
    OutDeltaText.Reset();
    OutReasoningText.Reset();
    OutIsFinal = false;

    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JSONString);
    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        return false;
    }

    if (JsonObject->HasField(TEXT("choices")))
    {
        const TArray<TSharedPtr<FJsonValue>>* ChoicesPtr;
        if (JsonObject->TryGetArrayField(TEXT("choices"), ChoicesPtr) && ChoicesPtr && ChoicesPtr->Num() > 0)
        {
            for (const TSharedPtr<FJsonValue>& ChoiceVal : *ChoicesPtr)
            {
                TSharedPtr<FJsonObject> ChoiceObj = ChoiceVal->AsObject();
                if (!ChoiceObj.IsValid())
                {
                    continue;
                }
                FString FinishReason;
                if (ChoiceObj->TryGetStringField(TEXT("finish_reason"), FinishReason))
                {
                    if (!FinishReason.IsEmpty())
                    {
                        OutIsFinal = true;
                    }
                }
                const TSharedPtr<FJsonObject>* DeltaObj = nullptr;
                if (ChoiceObj->TryGetObjectField(TEXT("delta"), DeltaObj) && DeltaObj && DeltaObj->IsValid())
                {
                    (*DeltaObj)->TryGetStringField(TEXT("content"), OutDeltaText);
                    (*DeltaObj)->TryGetStringField(TEXT("reasoning_content"), OutReasoningText);
                    if (OutReasoningText.IsEmpty())
                    {
                        (*DeltaObj)->TryGetStringField(TEXT("reasoning"), OutReasoningText);
                    }
                }
            }
        }
    }
    return !OutDeltaText.IsEmpty() || !OutReasoningText.IsEmpty() || OutIsFinal;
}
