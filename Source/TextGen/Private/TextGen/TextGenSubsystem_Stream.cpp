// Copyright <--\, Inc. All Rights Reserved.

#include "TextGen/TextGenSubsystem.h"
#include "TextGen/Providers/KoboldAPI.h"
#include "TextGen/Providers/OpenAIAPI.h"

#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "TimerManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Logging/LogMacros.h"
#include "TextGen/TextGenLog.h"

DEFINE_LOG_CATEGORY_STATIC(LogTextGenAsyncNode, Log, All);

#define TEXTGEN_ASYNC_DEBUG_LOG(Verbosity, Format, ...) \
    do { \
        if (TextGen_IsDebugEnabled()) { UE_LOG(LogTextGenAsyncNode, Verbosity, Format, ##__VA_ARGS__); } \
    } while (0)

bool UTextGenSubsystem::GenerateTextStream(const UObject* WorldContext, const FTextGenGenerationParams& GenerationParams, FString& OutErrorMessage)
{
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem)
    {
        OutErrorMessage = TEXT("Failed to get TextGen Subsystem");
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *OutErrorMessage);
        return false;
    }

    // Copy params to sanitize prompt (trim leading/trailing whitespace & newlines)
    FTextGenGenerationParams SanitizedParams = GenerationParams;
    if (!SanitizedParams.Prompt.IsEmpty())
    {
        const FString BeforeTrim = SanitizedParams.Prompt;
        SanitizedParams.Prompt.TrimStartAndEndInline();
        if (Subsystem->bEnableDebugLogging && !BeforeTrim.Equals(SanitizedParams.Prompt))
        {
	        TEXTGEN_DEBUG_LOG(Verbose, TEXT("Prompt sanitized (trimmed). OriginalLen=%d NewLen=%d"), BeforeTrim.Len(), SanitizedParams.Prompt.Len());
        }
    }

    if (SanitizedParams.Prompt.IsEmpty())
    {
        OutErrorMessage = TEXT("Prompt cannot be empty");
        Subsystem->LastErrorMessage = OutErrorMessage;
        return false;
    }

    const bool bSuccess = Subsystem->GenerateTextStreamInternal(SanitizedParams);
    if (!bSuccess)
    {
        OutErrorMessage = Subsystem->LastErrorMessage;
    }
    else
    {
        OutErrorMessage.Empty();
    }

    return bSuccess;
}

bool UTextGenSubsystem::AbortGeneration(const UObject* WorldContext, FString& OutErrorMessage)
{
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem)
    {
        OutErrorMessage = TEXT("Failed to get TextGen Subsystem");
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *OutErrorMessage);
        return false;
    }

    const bool bSuccess = Subsystem->AbortGenerationInternal();
    if (!bSuccess)
    {
        OutErrorMessage = Subsystem->LastErrorMessage;
    }
    else
    {
        OutErrorMessage.Empty();
    }

    return bSuccess;
}

bool UTextGenSubsystem::IsGenerating(const UObject* WorldContext)
{
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem)
    {
        return false;
    }
    return Subsystem->bIsGenerating;
}

// ===================== Async Stream Node =====================
UTextGenGenerateTextStreamAsync::UTextGenGenerateTextStreamAsync()
{
    WorldContextObject = nullptr;
    APISubsystem = nullptr;
    AccumulatedText = TEXT("");
    bHasStarted = false;
}

UTextGenGenerateTextStreamAsync* UTextGenGenerateTextStreamAsync::GenerateTextStreamAsync(UObject* WorldContextObject, const FTextGenGenerationParams& GenerationParams)
{
    if (!WorldContextObject)
    {
        UE_LOG(LogTextGenAsyncNode, Error, TEXT("WorldContext is null in GenerateTextStreamAsync"));
        return nullptr;
    }
    if (GenerationParams.Prompt.IsEmpty())
    {
        UE_LOG(LogTextGenAsyncNode, Error, TEXT("Generation prompt cannot be empty"));
        return nullptr;
    }
    UTextGenGenerateTextStreamAsync* AsyncNode = NewObject<UTextGenGenerateTextStreamAsync>();
    if (!AsyncNode)
    {
        UE_LOG(LogTextGenAsyncNode, Error, TEXT("Failed to create async node instance"));
        return nullptr;
    }
    AsyncNode->WorldContextObject = WorldContextObject;
    AsyncNode->GenerationParams = GenerationParams;
    AsyncNode->AccumulatedText = TEXT("");
    AsyncNode->bHasStarted = false;
    AsyncNode->APISubsystem = UTextGenSubsystem::GetTextGenSubsystem(WorldContextObject);
    if (!AsyncNode->APISubsystem)
    {
        UE_LOG(LogTextGenAsyncNode, Error, TEXT("Failed to get TextGen Subsystem"));
        return nullptr;
    }
    TEXTGEN_ASYNC_DEBUG_LOG(Log, TEXT("Created async node for text generation"));
    return AsyncNode;
}

void UTextGenGenerateTextStreamAsync::Activate()
{
    Super::Activate();
    if (!APISubsystem || !WorldContextObject)
    {
        UE_LOG(LogTextGenAsyncNode, Error, TEXT("Invalid state in Activate - missing subsystem or world context"));
        OnFailed.Broadcast(TEXT(""), TEXT("Invalid WorldContext or Subsystem"));
        return;
    }
    if (UTextGenSubsystem::IsGenerating(WorldContextObject))
    {
        UE_LOG(LogTextGenAsyncNode, Warning, TEXT("Generation already in progress, aborting previous generation"));
        FString AbortError;
        if (!UTextGenSubsystem::AbortGeneration(WorldContextObject, AbortError))
        {
            UE_LOG(LogTextGenAsyncNode, Error, TEXT("Failed to abort existing generation: %s"), *AbortError);
            OnFailed.Broadcast(TEXT(""), AbortError);
            return;
        }
    }
    APISubsystem->OnGenerationComplete.AddDynamic(this, &UTextGenGenerateTextStreamAsync::OnGenerationComplete);
    APISubsystem->OnGenerationChunk.AddDynamic(this, &UTextGenGenerateTextStreamAsync::OnGenerationChunk);
    APISubsystem->OnGenerationReasoningChunk.AddDynamic(this, &UTextGenGenerateTextStreamAsync::OnGenerationReasoningChunk);
    FString ErrorMessage;
    const bool bStarted = UTextGenSubsystem::GenerateTextStream(WorldContextObject, GenerationParams, ErrorMessage);
    if (!bStarted)
    {
        UE_LOG(LogTextGenAsyncNode, Error, TEXT("Failed to start text generation: %s"), *ErrorMessage);
        CleanupBindings();
        OnFailed.Broadcast(TEXT(""), ErrorMessage);
    }
}

void UTextGenGenerateTextStreamAsync::OnGenerationComplete(const FTextGenOperationResult& Result, const FTextGenGenerationResponse& Response)
{
    TEXTGEN_ASYNC_DEBUG_LOG(Log, TEXT("Generation complete event received"));

    CleanupBindings();
    if (Result.Result == ETextGenResult::Success)
    {
        const FString FinalText = Response.GeneratedText.IsEmpty() ? AccumulatedText : Response.GeneratedText;

        // If we never received any chunks (bHasStarted still false) AND the final text is empty,
        // treat this as a startup failure rather than a successful (but empty) generation.
        if (!bHasStarted && FinalText.Len() == 0)
        {
            const FString EmptyError = TEXT("Generation produced no output (empty response) – treating as failure");
            UE_LOG(LogTextGenAsyncNode, Warning, TEXT("%s"), *EmptyError);
            OnFailed.Broadcast(TEXT(""), EmptyError);
            return;
        }
        // Broadcast OnStarted only if we actually have something to deliver or we had begun streaming earlier.
        if (!bHasStarted)
        {
            bHasStarted = true;
            OnStarted.Broadcast(TEXT(""), TEXT(""));
        }
        TEXTGEN_ASYNC_DEBUG_LOG(Log, TEXT("Generation completed successfully with %d characters"), FinalText.Len());
        OnCompleted.Broadcast(FinalText, TEXT(""));
    }
    else
    {
        const FString ErrorMsg = Result.ErrorMessage.IsEmpty() ? TEXT("Unknown generation error") : Result.ErrorMessage;
        const int32 Code = Result.ResponseCode;
        const FString Combined = (Code > 0) ? FString::Printf(TEXT("HTTP %d: %s"), Code, *ErrorMsg) : ErrorMsg;
        UE_LOG(LogTextGenAsyncNode, Error, TEXT("Generation failed: %s"), *Combined);
        if (bHasStarted)
        {
            OnStreamFailed.Broadcast(TEXT(""), Combined);
        }
        else
        {
            OnFailed.Broadcast(TEXT(""), Combined);
        }
    }
}

void UTextGenGenerateTextStreamAsync::OnGenerationChunk(const FString& ChunkText, bool bIsComplete)
{
    if (!bHasStarted)
    {
        bHasStarted = true;
        OnStarted.Broadcast(TEXT(""), TEXT(""));
    }
    if (!ChunkText.IsEmpty())
    {
        AccumulatedText += ChunkText;
        OnChunk.Broadcast(ChunkText, TEXT(""));
        TEXTGEN_ASYNC_DEBUG_LOG(Verbose, TEXT("Received chunk: %d characters (Total: %d)"), ChunkText.Len(), AccumulatedText.Len());
    }
}

void UTextGenGenerateTextStreamAsync::OnGenerationReasoningChunk(const FString& ChunkText, bool bIsComplete)
{
    if (!bHasStarted)
    {
        bHasStarted = true;
        OnStarted.Broadcast(TEXT(""), TEXT(""));
    }
    if (!ChunkText.IsEmpty())
    {
        OnReasoning.Broadcast(ChunkText, TEXT(""));
    }
}

void UTextGenGenerateTextStreamAsync::CleanupBindings()
{
    if (APISubsystem)
    {
        APISubsystem->OnGenerationComplete.RemoveDynamic(this, &UTextGenGenerateTextStreamAsync::OnGenerationComplete);
        APISubsystem->OnGenerationChunk.RemoveDynamic(this, &UTextGenGenerateTextStreamAsync::OnGenerationChunk);
        APISubsystem->OnGenerationReasoningChunk.RemoveDynamic(this, &UTextGenGenerateTextStreamAsync::OnGenerationReasoningChunk);
        TEXTGEN_ASYNC_DEBUG_LOG(Log, TEXT("Cleaned up event bindings"));
    }
}

// ===================== Async List Models Node =====================
UTextGenListModelsAsync::UTextGenListModelsAsync()
{
    WorldContextObject = nullptr;
    APISubsystem = nullptr;
}

UTextGenListModelsAsync* UTextGenListModelsAsync::ListModelsAsync(UObject* WorldContextObject, ELLMProvider Provider)
{
    if (!WorldContextObject)
    {
        return nullptr;
    }
    UTextGenListModelsAsync* Node = NewObject<UTextGenListModelsAsync>();
    Node->WorldContextObject = WorldContextObject;
    Node->RequestedProvider = Provider;
    return Node;
}

void UTextGenListModelsAsync::Activate()
{
    Super::Activate();
    if (!WorldContextObject)
    {
        OnFailure.Broadcast(TArray<FTextGenModelInfo>(), TArray<FString>{TEXT("(No Models)")}, TEXT("Invalid WorldContext"));
        return;
    }
    APISubsystem = UTextGenSubsystem::GetTextGenSubsystem(WorldContextObject);
    if (!APISubsystem)
    {
        OnFailure.Broadcast(TArray<FTextGenModelInfo>(), TArray<FString>{TEXT("(No Models)")}, TEXT("Subsystem not available"));
        return;
    }
    APISubsystem->OnModelsListed.AddDynamic(this, &UTextGenListModelsAsync::OnModelsListedInternal);
    FString Error;
    if (!UTextGenSubsystem::ListModels(WorldContextObject, RequestedProvider, Error))
    {
        Cleanup();
        OnFailure.Broadcast(TArray<FTextGenModelInfo>(), TArray<FString>{TEXT("(No Models)")}, Error.IsEmpty() ? TEXT("Failed to start request") : Error);
    }
}

void UTextGenListModelsAsync::OnModelsListedInternal(const FTextGenOperationResult& Result, const FTextGenModelsResponse& Response)
{
    Cleanup();
    if (Result.Result == ETextGenResult::Success)
    {
        OnSuccess.Broadcast(Response.Models, Response.DebugLines, TEXT(""));
    }
    else
    {
        const FString Err = Result.ErrorMessage.IsEmpty() ? TEXT("Models request failed") : Result.ErrorMessage;
        OnFailure.Broadcast(Response.Models, Response.DebugLines, Err);
    }
}

void UTextGenListModelsAsync::Cleanup()
{
    if (APISubsystem)
    {
        APISubsystem->OnModelsListed.RemoveDynamic(this, &UTextGenListModelsAsync::OnModelsListedInternal);
    }
}

// ===================== Stream Processing =====================
void UTextGenSubsystem::ProcessStreamChunks()
{
    // Streaming timeout semantics: for active streams, timeout should be based on inactivity (no incoming bytes),
    // not total generation duration.
    const double NowSeconds = FPlatformTime::Seconds();

    if (!bIsGenerating || !CurrentGenerationRequest.IsValid())
    {
        return;
    }
    FHttpResponsePtr Response = CurrentGenerationRequest->GetResponse();
    if (!Response.IsValid())
    {
        return;
    }
    FString CurrentContent = Response->GetContentAsString();
    if (CurrentContent.Len() > LastProcessedLength)
    {
        FString NewData = CurrentContent.RightChop(LastProcessedLength);
        LastProcessedLength = CurrentContent.Len();
        LastStreamActivityTimeSeconds = NowSeconds;
        TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("Processing %d new bytes from stream"), NewData.Len());
        ProcessStreamData(NewData);
    }

    if (RequestTimeout > 0.0f && LastStreamActivityTimeSeconds > 0.0)
    {
        const double IdleForSeconds = NowSeconds - LastStreamActivityTimeSeconds;
        if (IdleForSeconds >= static_cast<double>(RequestTimeout))
        {
            CancelActiveStreamDueToTimeout(
                FString::Printf(TEXT("Streaming idle timeout after %.2fs (no data received)"), RequestTimeout));
        }
    }
}

void UTextGenSubsystem::CancelActiveStreamDueToTimeout(const FString& Reason)
{
    if (!bIsGenerating || !CurrentGenerationRequest.IsValid())
    {
        return;
    }

    PendingStreamFailureReason = Reason;
    LastErrorMessage = Reason;
    UE_LOG(LogTextGenAPI, Warning, TEXT("%s"), *Reason);

    // Let OnGenerationStreamResponse finalize state and broadcast a single failure event.
    CurrentGenerationRequest->CancelRequest();

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(StreamProcessingTimer);
    }
}

void UTextGenSubsystem::ProcessFinalStreamChunk()
{
    if (!UnprocessedStreamBuffer.IsEmpty())
    {
        TArray<FString> JsonObjects = ExtractJSONObjects(UnprocessedStreamBuffer);
        for (const FString& JsonString : JsonObjects)
        {
            ProcessJSONChunk(JsonString);
        }
        if (!UnprocessedStreamBuffer.IsEmpty())
        {
            FString RemainingText = UnprocessedStreamBuffer.TrimStartAndEnd();
            if (!RemainingText.IsEmpty())
            {
                // Detect and ignore SSE end sentinel variants left without trailing newline
                auto IsSSEEndSentinel = [](const FString& InLine)->bool
                {
                    FString T = InLine; T.TrimStartAndEndInline();
                    return T.Equals(TEXT("data: [DONE]"), ESearchCase::IgnoreCase) || T.Equals(TEXT("[DONE]"), ESearchCase::IgnoreCase);
                };
                if (IsSSEEndSentinel(RemainingText))
                {
                    TEXTGEN_DEBUG_LOG(Log, TEXT("Stream end sentinel received in final buffer (ignored)."));
                    // Mark generation complete (no extra text broadcast)
                    bIsGenerating = false;
                    if (UWorld* World = GetWorld())
                    {
                        World->GetTimerManager().ClearTimer(StreamProcessingTimer);
                    }
                }
                else
                {
                    FString ProcessedRemainingText = RemainingText;
                    bool bStopFound = CheckForStopSequences(ProcessedRemainingText, CurrentGenerationSettings.StopSequences);
                    StreamBuffer += ProcessedRemainingText;
                    OnGenerationChunk.Broadcast(ProcessedRemainingText, true);
                    TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("Processed remaining plain text as final chunk: '%s' | Stop: %s"),
                           *ProcessedRemainingText.Left(50), bStopFound ? TEXT("Yes") : TEXT("No"));
                    if (!bEnableDebugLogging && bStopFound)
                    {
            	                    TEXTGEN_DEBUG_LOG(Log, TEXT("Stop sequence found in final chunk"));
                    }
                }
            }
        }
        UnprocessedStreamBuffer.Empty();
    }
}

void UTextGenSubsystem::ProcessStreamData(const FString& NewData)
{
    if (NewData.IsEmpty())
    {
        return;
    }
    UnprocessedStreamBuffer += NewData;
    TArray<FString> JsonObjects = ExtractJSONObjects(UnprocessedStreamBuffer);
    if (JsonObjects.Num() > 0)
    {
        for (const FString& JsonString : JsonObjects)
        {
            ProcessJSONChunk(JsonString);
        }
        TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("Processed %d JSON chunks from %d bytes"), JsonObjects.Num(), NewData.Len());
    }
}

TArray<FString> UTextGenSubsystem::ExtractJSONObjects(FString& Buffer)
{
    TArray<FString> JsonObjects;
    if (Buffer.IsEmpty())
    {
        return JsonObjects;
    }
    TArray<FString> Lines;
    Buffer.ParseIntoArray(Lines, TEXT("\n"), false);
    FString RemainingBuffer;
    for (int32 i = 0; i < Lines.Num(); ++i)
    {
        FString Line = Lines[i].TrimStartAndEnd();
        bool bIsLastLine = (i == Lines.Num() - 1);
        bool bBufferEndsWithNewline = Buffer.EndsWith(TEXT("\n")) || Buffer.EndsWith(TEXT("\r\n"));
        if (bIsLastLine && !bBufferEndsWithNewline)
        {
            RemainingBuffer = Line;
            break;
        }
        if (!Line.IsEmpty())
        {
            if (Line.StartsWith(TEXT("data:")))
            {
                FString JsonPart = Line.RightChop(5).TrimStartAndEnd();
                if (!JsonPart.IsEmpty() && JsonPart != TEXT("[DONE]"))
                {
                    JsonObjects.Add(JsonPart);
                }
            }
            else if (Line.StartsWith(TEXT("{")))
            {
                JsonObjects.Add(Line);
            }
        }
    }
    Buffer = RemainingBuffer;
    if (JsonObjects.Num() > 0)
    {
        TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("Extracted %d JSON objects"), JsonObjects.Num());
    }
    return JsonObjects;
}

void UTextGenSubsystem::ProcessJSONChunk(const FString& JSONString)
{
    if (JSONString.IsEmpty())
    {
        return;
    }
    // Debug: log raw OpenAI JSON chunk for analysis
    if (ActiveProvider == ELLMProvider::OpenAI || ActiveProvider == ELLMProvider::Llamacpp)
    {
        TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("[OpenAI Stream JSON] %s"), *JSONString);
    }
    // Try provider-specific fast path first to avoid parsing JSON twice.
    {
        const ITextGenProvider* Provider = GetActiveProvider();
        if (Provider)
        {
            FString Delta;
            FString ReasoningDelta;
            bool bIsComplete = false;
            if (Provider->ExtractStreamDelta(JSONString, Delta, ReasoningDelta, bIsComplete))
            {
                if (!ReasoningDelta.IsEmpty())
                {
                    OnGenerationReasoningChunk.Broadcast(ReasoningDelta, false);
                }

                bool bStopFound = false;
                FString Processed;
                int32 BufferLengthBefore = StreamBuffer.Len();

                if (!Delta.IsEmpty())
                {
                    Processed = Delta;
                    bStopFound = CheckForStopSequences(Processed, CurrentGenerationSettings.StopSequences);
                    StreamBuffer += Processed;
                }

                if (bStopFound)
                {
                    bIsComplete = true;
                }

                // IMPORTANT: Some providers can signal completion with an empty delta.
                // We must still stop our processing timer or the subsystem may remain stuck generating.
                if (bIsComplete)
                {
                    bIsGenerating = false;
                    if (UWorld* World = GetWorld())
                    {
                        World->GetTimerManager().ClearTimer(StreamProcessingTimer);
                    }
                }

                if (!Processed.IsEmpty() || bIsComplete)
                {
                    // Only broadcast text when we actually have text, but allow the completion flag to propagate.
                    OnGenerationChunk.Broadcast(Processed, bIsComplete);
                }

                TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("Delta: '%s' | Buffer: %d->%d | Final: %s"), *Processed.Left(30), BufferLengthBefore, StreamBuffer.Len(), bIsComplete ? TEXT("Yes") : TEXT("No"));
                return;
            }
        }
    }

    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JSONString);
    if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
    {
        if (JsonObject->HasField(TEXT("text")))
        {
            bool bIsComplete = false;
            FString Text = JsonObject->GetStringField(TEXT("text"));
            if (!Text.IsEmpty())
            {
                int32 BufferLengthBefore = StreamBuffer.Len();
                FString ProcessedText = Text;
                bool bStopFound = CheckForStopSequences(ProcessedText, CurrentGenerationSettings.StopSequences);
                StreamBuffer += ProcessedText;
                bIsComplete = false;
                if (JsonObject->HasField(TEXT("finished")))
                {
                    bIsComplete = JsonObject->GetBoolField(TEXT("finished"));
                }
                else if (JsonObject->HasField(TEXT("final")))
                {
                    bIsComplete = JsonObject->GetBoolField(TEXT("final"));
                }
                if (bStopFound)
                {
                    bIsComplete = true;
	                TEXTGEN_DEBUG_LOG(Log, TEXT("Generation stopped due to stop sequence"));
                }
                if (bIsComplete)
                {
                    bIsGenerating = false;
                    if (UWorld* World = GetWorld())
                    {
                        World->GetTimerManager().ClearTimer(StreamProcessingTimer);
                    }
                    TEXTGEN_DEBUG_LOG(Log, TEXT("Generation marked as complete via %s"), bStopFound ? TEXT("stop sequence") : TEXT("JSON finished/final flag"));
                }
                OnGenerationChunk.Broadcast(ProcessedText, bIsComplete);
                TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("Text: '%s' | Buffer: %d->%d | Final: %s | Stop: %s"), *ProcessedText.Left(30), BufferLengthBefore, StreamBuffer.Len(), bIsComplete ? TEXT("Yes") : TEXT("No"), bStopFound ? TEXT("Yes") : TEXT("No"));
                if (!bEnableDebugLogging && bIsComplete)
                {
	                TEXTGEN_DEBUG_LOG(Log, TEXT("Generation completed with %d characters"), StreamBuffer.Len());
                }
            }
        }
        else
        {
            TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Warning, TEXT("JSON chunk without text field: %s"), *JSONString.Left(100));
        }
    }
    else
    {
        TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Error, TEXT("Failed to parse JSON chunk: %s"), *JSONString.Left(100));
    }
}

bool UTextGenSubsystem::CheckForStopSequences(FString& Text, const TArray<FString>& StopSequences)
{
    if (Text.IsEmpty() || StopSequences.Num() == 0)
    {
        return false;
    }
    for (const FString& StopSeq : StopSequences)
    {
        if (StopSeq.IsEmpty())
        {
            continue;
        }
        int32 StopIndex = Text.Find(StopSeq);
        if (StopIndex != INDEX_NONE)
        {
            Text = Text.Left(StopIndex);
            TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("Stop sequence found: '%s' - Truncating text"), *StopSeq);
            return true;
        }
    }
    return false;
}
