// Copyright <--\, Inc. All Rights Reserved.

#include "TextGen/TextGenSubsystem.h"
#include "TextGen/Providers/KoboldAPI.h"
#include "TextGen/Providers/OpenAIAPI.h"
#include "TextGen/TextGenLocalServiceSubsystem.h"

#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/Engine.h"
#include "TimerManager.h"
#include "Logging/LogMacros.h"
#include "TextGen/TextGenLog.h"

UTextGenSubsystem* UTextGenSubsystem::GetTextGenSubsystem(const UObject* WorldContext)
{
    if (!WorldContext)
    {
        UE_LOG(LogTextGenAPI, Warning, TEXT("WorldContext is null in GetTextGenSubsystem"));
        return nullptr;
    }

    UWorld* World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::LogAndReturnNull);
    if (!World)
    {
        UE_LOG(LogTextGenAPI, Warning, TEXT("Could not get World from WorldContext"));
        return nullptr;
    }

    UGameInstance* GameInstance = World->GetGameInstance();
    if (!GameInstance)
    {
        UE_LOG(LogTextGenAPI, Warning, TEXT("GameInstance is null"));
        return nullptr;
    }

    return GameInstance->GetSubsystem<UTextGenSubsystem>();
}

TSharedRef<IHttpRequest> UTextGenSubsystem::CreateRequest(const FString& Endpoint, const FString& Verb, float TimeoutOverrideSeconds)
{
    return CreateRequestForProvider(ActiveProvider, Endpoint, Verb, TimeoutOverrideSeconds);
}

FString UTextGenSubsystem::CombineBaseAndEndpoint(const FString& InBaseURL, const FString& InEndpoint)
{
    FString Base = InBaseURL.TrimStartAndEnd();
    FString Endpoint = InEndpoint.TrimStartAndEnd();

    if (Base.IsEmpty())
    {
        return Endpoint;
    }

    // 1. Ensure scheme / protocol (http:// or https://)
    if (!Base.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) &&
        !Base.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
    {
        const bool bIsLocalhost = Base.StartsWith(TEXT("localhost"), ESearchCase::IgnoreCase) ||
                                  Base.StartsWith(TEXT("127.0.0.1")) ||
                                  Base.StartsWith(TEXT("0.0.0.0"));
        Base = (bIsLocalhost ? TEXT("http://") : TEXT("https://")) + Base;
    }

    // 2. Normalize multiple slashes in scheme
    if (Base.StartsWith(TEXT("https:///"), ESearchCase::IgnoreCase))
    {
        Base = TEXT("https://") + Base.Mid(9);
    }
    else if (Base.StartsWith(TEXT("http:///"), ESearchCase::IgnoreCase))
    {
        Base = TEXT("http://") + Base.Mid(8);
    }

    // 3. Remove trailing slashes from Base
    while (Base.EndsWith(TEXT("/")))
    {
        Base.LeftChopInline(1);
    }

    // 4. Handle full endpoint paths pasted directly into BaseURL by mistake
    static const TArray<FString> KnownEndpointSuffixes = {
        TEXT("/chat/completions"),
        TEXT("/models"),
        TEXT("/embeddings"),
        TEXT("/completions"),
        TEXT("/v1/chat/completions"),
        TEXT("/v1/models"),
        TEXT("/v1/embeddings")
    };

    for (const FString& Suffix : KnownEndpointSuffixes)
    {
        if (Base.EndsWith(Suffix, ESearchCase::IgnoreCase))
        {
            Base.LeftChopInline(Suffix.Len());
            break;
        }
    }

    while (Base.EndsWith(TEXT("/")))
    {
        Base.LeftChopInline(1);
    }

    if (Endpoint.IsEmpty())
    {
        return Base;
    }

    // 5. Ensure Endpoint starts with "/"
    if (!Endpoint.StartsWith(TEXT("/")))
    {
        Endpoint = TEXT("/") + Endpoint;
    }

    // 6. De-duplicate overlapping version/path segments
    if (Endpoint.StartsWith(TEXT("/v1/"), ESearchCase::IgnoreCase) && Base.EndsWith(TEXT("/v1"), ESearchCase::IgnoreCase))
    {
        Endpoint = Endpoint.Mid(3);
    }
    else if (Endpoint.StartsWith(TEXT("/v2/"), ESearchCase::IgnoreCase) && Base.EndsWith(TEXT("/v2"), ESearchCase::IgnoreCase))
    {
        Endpoint = Endpoint.Mid(3);
    }
    else if (Endpoint.StartsWith(TEXT("/api/v1/"), ESearchCase::IgnoreCase) && Base.EndsWith(TEXT("/api/v1"), ESearchCase::IgnoreCase))
    {
        Endpoint = Endpoint.Mid(7);
    }
    else if (Endpoint.StartsWith(TEXT("/api/v1/"), ESearchCase::IgnoreCase) && Base.EndsWith(TEXT("/v1"), ESearchCase::IgnoreCase))
    {
        Endpoint = Endpoint.Mid(7);
    }

    // 7. Ensure double slashes inside path (e.g. "//") are collapsed to single "/" after protocol
    int32 SchemeEnd = Base.Find(TEXT("://"));
    FString SchemePart = (SchemeEnd != INDEX_NONE) ? Base.Left(SchemeEnd + 3) : TEXT("");
    FString PathPart = (SchemeEnd != INDEX_NONE) ? Base.Mid(SchemeEnd + 3) + Endpoint : Base + Endpoint;

    while (PathPart.Contains(TEXT("//")))
    {
        PathPart.ReplaceInline(TEXT("//"), TEXT("/"));
    }

    return SchemePart + PathPart;
}

TSharedRef<IHttpRequest> UTextGenSubsystem::CreateRequestForProvider(ELLMProvider Provider, const FString& Endpoint, const FString& Verb, float TimeoutOverrideSeconds)
{
    TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
    FString ProviderBaseURL = BaseURL;
    FString APIKey;
    if (Provider == ELLMProvider::OpenAI)
    {
        ProviderBaseURL = OpenAIBaseURL;
        APIKey = OpenAIAPIKey;
    }
    else if (Provider == ELLMProvider::Llamacpp)
    {
        if (const UTextGenLocalServiceSubsystem* Service = GEngine ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr)
        {
            ProviderBaseURL = Service->GetEffectiveBaseURL();
        }
    }
    const FString FullURL = CombineBaseAndEndpoint(ProviderBaseURL, Endpoint);
    const float ProviderTimeout = Provider == ELLMProvider::Llamacpp ? LlamacppConfig.RequestTimeout : RequestTimeout;
    const float EffectiveTimeoutSeconds = (TimeoutOverrideSeconds > 0.0f) ? TimeoutOverrideSeconds : ProviderTimeout;

    Request->SetURL(FullURL);
    Request->SetVerb(Verb);
    Request->SetTimeout(EffectiveTimeoutSeconds);
    Request->SetHeader(TEXT("User-Agent"), TEXT("UnrealEngine/TextGen"));
    Request->SetHeader(TEXT("Accept"), TEXT("application/json"));

    if (!APIKey.IsEmpty())
    {
        Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *APIKey));
    }

    const bool bOverrideProvider = (Provider != ActiveProvider);
    if (bOverrideProvider)
    {
        TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("(Override Provider) Creating %s request to: %s (Provider: %s)"), *Verb, *FullURL,
            Provider == ELLMProvider::OpenAI ? TEXT("OpenAI") : Provider == ELLMProvider::Llamacpp ? TEXT("Llamacpp") : TEXT("KoboldCpp"));
    }
    else
    {
        TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("Creating %s request to: %s (Provider: %s)"), *Verb, *FullURL,
            Provider == ELLMProvider::OpenAI ? TEXT("OpenAI") : Provider == ELLMProvider::Llamacpp ? TEXT("Llamacpp") : TEXT("KoboldCpp"));
    }

    return Request;
}

FTextGenOperationResult UTextGenSubsystem::ParseJSONResponse(const FString& ResponseString, int32 ResponseCode)
{
    FTextGenOperationResult Result;
    Result.ResponseCode = ResponseCode;
    Result.ResponseData = ResponseString;

    if (ResponseCode >= 200 && ResponseCode < 300)
    {
        Result.Result = ETextGenResult::Success;
    }
    else
    {
        Result.Result = ETextGenResult::NetworkError;
        Result.ErrorMessage = FString::Printf(TEXT("HTTP Error %d"), ResponseCode);
    }

    return Result;
}

FTextGenOperationResult UTextGenSubsystem::CreateErrorResult(ETextGenResult ErrorType, const FString& ErrorMessage, int32 ResponseCode)
{
    FTextGenOperationResult Result;
    Result.Result = ErrorType;
    Result.ErrorMessage = ErrorMessage;
    Result.ResponseCode = ResponseCode;
    LastErrorMessage = ErrorMessage;
    return Result;
}

bool UTextGenSubsystem::GetTokenCount(const UObject* WorldContext, const FString& Text, FString& OutErrorMessage)
{
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem)
    {
        OutErrorMessage = TEXT("Failed to get TextGen Subsystem");
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *OutErrorMessage);
        return false;
    }

    if (Text.IsEmpty())
    {
        OutErrorMessage = TEXT("Text cannot be empty for token count");
        Subsystem->LastErrorMessage = OutErrorMessage;
        return false;
    }

    const bool bSuccess = Subsystem->GetTokenCountInternal(Text);
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

bool UTextGenSubsystem::GetVersion(const UObject* WorldContext, FString& OutErrorMessage)
{
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem)
    {
        OutErrorMessage = TEXT("Failed to get TextGen Subsystem");
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *OutErrorMessage);
        return false;
    }

    const bool bSuccess = Subsystem->GetVersionInternal();
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

bool UTextGenSubsystem::GetMaxContextLength(const UObject* WorldContext, FString& OutErrorMessage)
{
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem)
    {
        OutErrorMessage = TEXT("Failed to get TextGen Subsystem");
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *OutErrorMessage);
        return false;
    }

    const bool bSuccess = Subsystem->GetMaxContextLengthInternal();
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

bool UTextGenSubsystem::CheckAPIHealth(const UObject* WorldContext, FString& OutErrorMessage)
{
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem)
    {
        OutErrorMessage = TEXT("Failed to get TextGen Subsystem");
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *OutErrorMessage);
        return false;
    }

    const bool bSuccess = Subsystem->CheckAPIHealthInternal();
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

bool UTextGenSubsystem::IsAPIAvailable(const UObject* WorldContext)
{
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem)
    {
        return false;
    }
    return Subsystem->bIsAPIAvailable;
}

FString UTextGenSubsystem::GetLastErrorMessage(const UObject* WorldContext)
{
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem)
    {
        return TEXT("Failed to get TextGen Subsystem");
    }
    return Subsystem->LastErrorMessage;
}

// ===================== HTTP Response Handlers =====================
void UTextGenSubsystem::OnGenerationStreamResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
    TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("OnGenerationStreamResponse called - Success: %s, IsGenerating: %s"),
           bWasSuccessful ? TEXT("Yes") : TEXT("No"), bIsGenerating ? TEXT("Yes") : TEXT("No"));
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(StreamProcessingTimer);
        TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("Stream processing timer cleared"));
    }
    CurrentGenerationRequest.Reset();
    FTextGenOperationResult Result;
    FTextGenGenerationResponse GenerationResponse;
    if (!bWasSuccessful || !Response.IsValid())
    {
        const FString FailureReason = PendingStreamFailureReason.IsEmpty()
            ? TEXT("Network request failed")
            : PendingStreamFailureReason;
        Result = CreateErrorResult(ETextGenResult::NetworkError, FailureReason);
        bIsAPIAvailable = false;
        if (bIsGenerating)
        {
            bIsGenerating = false;
            UE_LOG(LogTextGenAPI, Warning, TEXT("Generation state cleaned up due to network failure"));
        }
        LastProcessedLength = 0;
        UnprocessedStreamBuffer.Empty();
        StreamBuffer.Empty();
        UE_LOG(LogTextGenAPI, Error, TEXT("Generation failed - %s"), *FailureReason);
        CheckAPIHealthInternal();
    }
    else
    {
        int32 ResponseCode = Response->GetResponseCode();
        FString ResponseString = Response->GetContentAsString();
        TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("Response code: %d, Buffer length: %d"), ResponseCode, StreamBuffer.Len());
        Result = ParseJSONResponse(ResponseString, ResponseCode);
        bIsAPIAvailable = (ResponseCode >= 200 && ResponseCode < 300);
        if (Result.Result == ETextGenResult::Success)
        {
            FString BufferBeforeFinal = StreamBuffer;
            // Some Unreal HTTP backends expose no incremental response body and deliver the
            // complete SSE payload only in this completion callback. Consume any bytes the
            // polling timer did not see before finalizing the stream.
            if (ResponseString.Len() > LastProcessedLength)
            {
                ProcessStreamData(ResponseString.RightChop(LastProcessedLength));
                LastProcessedLength = ResponseString.Len();
            }
            ProcessFinalStreamChunk();
            if (bIsGenerating)
            {
                bIsGenerating = false;
            }
            TEXTGEN_DEBUG_LOG(Log, TEXT("Generation state cleaned up in final response processing"));
            if (StreamBuffer != BufferBeforeFinal)
            {
                TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("Final processing added %d characters"), StreamBuffer.Len() - BufferBeforeFinal.Len());
            }
            GenerationResponse.GeneratedText = StreamBuffer;
            // Remove trailing newline / carriage return characters from final output
            if (!GenerationResponse.GeneratedText.IsEmpty())
            {
                int32 Removed = 0;
                while (GenerationResponse.GeneratedText.EndsWith(TEXT("\n")) || GenerationResponse.GeneratedText.EndsWith(TEXT("\r")))
                {
                    GenerationResponse.GeneratedText.LeftChopInline(1);
                    ++Removed;
                }
                if (Removed > 0)
                {
                    TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Verbose, TEXT("Stripped %d trailing newline character(s) from final output"), Removed);
                }
            }
            GenerationResponse.bIsComplete = true;
            GenerationResponse.TokensGenerated = StreamBuffer.Len();
            if (StreamBuffer.Len() == 0)
            {
                // Treat a zero-length successful response as a parse error so higher layers can surface failure.
                Result = CreateErrorResult(ETextGenResult::ParseError, TEXT("Empty generation response"), ResponseCode);
                GenerationResponse.bIsComplete = false; // not a valid completion
                UE_LOG(LogTextGenAPI, Warning, TEXT("Generation returned empty content; converting success to ParseError"));
            }
            else
            {
                GenerationResponse.CommitHandle = CreateConversationCommitHandle();
                TEXTGEN_DEBUG_LOG(Log, TEXT("Text generation completed successfully (%d characters)"), StreamBuffer.Len());
            }
        }
        else
        {
            UE_LOG(LogTextGenAPI, Error, TEXT("Generation failed with HTTP %d"), ResponseCode);
            if (bIsGenerating)
            {
                bIsGenerating = false;
                UE_LOG(LogTextGenAPI, Warning, TEXT("Generation state cleaned up due to HTTP error"));
            }
            CheckAPIHealthInternal();
        }
        LastProcessedLength = 0;
        UnprocessedStreamBuffer.Empty();
        StreamBuffer.Empty();
        CurrentGenerationSettings = FTextGenGenerationSettings();
    }
    PendingStreamFailureReason.Empty();
    LastStreamActivityTimeSeconds = 0.0;
    TEXTGEN_DEBUG_LOG_LOCAL(bEnableDebugLogging, Log, TEXT("Firing OnGenerationComplete event (text length: %d)"), GenerationResponse.GeneratedText.Len());
    OnGenerationComplete.Broadcast(Result, GenerationResponse);
}

void UTextGenSubsystem::OnAbortResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
    FTextGenOperationResult Result;
    if (!bWasSuccessful || !Response.IsValid())
    {
        Result = CreateErrorResult(ETextGenResult::NetworkError, TEXT("Abort request failed"));
    }
    else
    {
        int32 ResponseCode = Response->GetResponseCode();
        Result = ParseJSONResponse(Response->GetContentAsString(), ResponseCode);
        if (Result.Result == ETextGenResult::Success)
        {
            TEXTGEN_DEBUG_LOG(Log, TEXT("Generation aborted successfully"));
        }
    }
    OnAbortComplete.Broadcast(Result);
}

void UTextGenSubsystem::OnTokenCountResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
    FTextGenOperationResult Result;
    FTextGenTokenCountResponse TokenResponse;
    if (!bWasSuccessful || !Response.IsValid())
    {
        Result = CreateErrorResult(ETextGenResult::NetworkError, TEXT("Token count request failed"));
    }
    else
    {
        int32 ResponseCode = Response->GetResponseCode();
        FString ResponseString = Response->GetContentAsString();
        Result = ParseJSONResponse(ResponseString, ResponseCode);
        if (Result.Result == ETextGenResult::Success)
        {
            int32 TokenCount = 0;
            FString ParseError;
            if (GetActiveProvider()->ParseTokenCountResponse(ResponseString, TokenCount, ParseError))
            {
                TokenResponse.TokenCount = TokenCount;
                TEXTGEN_DEBUG_LOG(Log, TEXT("Token count: %d"), TokenResponse.TokenCount);
            }
            else
            {
                Result = CreateErrorResult(ETextGenResult::ParseError, ParseError.IsEmpty() ? TEXT("Failed to parse token count response") : ParseError);
            }
        }
    }
    OnTokenCountComplete.Broadcast(Result, TokenResponse);
}

void UTextGenSubsystem::OnVersionResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
    FTextGenOperationResult Result;
    FTextGenVersionResponse VersionResponse;
    if (!bWasSuccessful || !Response.IsValid())
    {
        Result = CreateErrorResult(ETextGenResult::NetworkError, TEXT("Version request failed"));
    }
    else
    {
        int32 ResponseCode = Response->GetResponseCode();
        FString ResponseString = Response->GetContentAsString();
        Result = ParseJSONResponse(ResponseString, ResponseCode);
        if (Result.Result == ETextGenResult::Success)
        {
            FString Version;
            FString BuildInfo;
            FString ParseError;
            if (GetActiveProvider()->ParseVersionResponse(ResponseString, Version, BuildInfo, ParseError))
            {
                VersionResponse.Version = Version;
                VersionResponse.BuildInfo = BuildInfo;
                const FString BuildInfoSuffix = BuildInfo.IsEmpty() ? TEXT("") : FString::Printf(TEXT(" (%s)"), *BuildInfo);
                TEXTGEN_DEBUG_LOG(Log, TEXT("Provider Version: %s%s"), *VersionResponse.Version, *BuildInfoSuffix);
            }
            else
            {
                Result = CreateErrorResult(ETextGenResult::ParseError, ParseError.IsEmpty() ? TEXT("Failed to parse version response") : ParseError);
            }
        }
    }
    OnVersionComplete.Broadcast(Result, VersionResponse);
}

void UTextGenSubsystem::OnMaxContextResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
    FTextGenOperationResult Result;
    FTextGenMaxContextResponse MaxContextResponse;
    if (!bWasSuccessful || !Response.IsValid())
    {
        Result = CreateErrorResult(ETextGenResult::NetworkError, TEXT("Max context request failed"));
    }
    else
    {
        int32 ResponseCode = Response->GetResponseCode();
        FString ResponseString = Response->GetContentAsString();
        Result = ParseJSONResponse(ResponseString, ResponseCode);
        if (Result.Result == ETextGenResult::Success)
        {
            int32 MaxContext = 0;
            FString ParseError;
            if (GetActiveProvider()->ParseMaxContextResponse(ResponseString, MaxContext, ParseError))
            {
                MaxContextResponse.MaxContextLength = MaxContext;
                TEXTGEN_DEBUG_LOG(Log, TEXT("Max Context Length: %d"), MaxContextResponse.MaxContextLength);
            }
            else
            {
                Result = CreateErrorResult(ETextGenResult::ParseError, ParseError.IsEmpty() ? TEXT("Failed to parse max context response") : ParseError);
            }
        }
    }
    OnMaxContextComplete.Broadcast(Result, MaxContextResponse);
}

void UTextGenSubsystem::OnHealthCheckResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
    FTextGenOperationResult Result;
    bool bIsAPIHealthy = false;
    if (!bWasSuccessful || !Response.IsValid())
    {
        Result = CreateErrorResult(ETextGenResult::NetworkError, TEXT("API health check failed - Server not responding"));
        bIsAPIAvailable = false;
        bIsAPIHealthy = false;
        UE_LOG(LogTextGenAPI, Warning, TEXT("API health check failed: Server not responding"));
    }
    else
    {
        int32 ResponseCode = Response->GetResponseCode();
        FString ResponseString = Response->GetContentAsString();
        Result = ParseJSONResponse(ResponseString, ResponseCode);
        if (Result.Result == ETextGenResult::Success)
        {
            bool bHealthy = false;
            FString ParseError;
            if (GetActiveProvider()->ParseHealthResponse(ResponseString, bHealthy, ParseError))
            {
                bIsAPIHealthy = bHealthy;
                bIsAPIAvailable = bHealthy;
            }
            else
            {
                Result = CreateErrorResult(ETextGenResult::ParseError, ParseError.IsEmpty() ? TEXT("API responding but unable to parse health response") : ParseError);
                bIsAPIAvailable = false;
                bIsAPIHealthy = false;
            }
        }
        else
        {
            Result = CreateErrorResult(ETextGenResult::NetworkError, FString::Printf(TEXT("API returned HTTP %d"), ResponseCode));
            bIsAPIAvailable = false;
            bIsAPIHealthy = false;
            UE_LOG(LogTextGenAPI, Warning, TEXT("API health check failed: HTTP %d - %s"), ResponseCode, *Result.ErrorMessage);
        }
    }
    OnHealthCheckComplete.Broadcast(Result, bIsAPIHealthy);
}

void UTextGenSubsystem::OnListModelsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
    FTextGenOperationResult Result;
    FTextGenModelsResponse ModelsResp;
    if (!bWasSuccessful || !Response.IsValid())
    {
        Result = CreateErrorResult(ETextGenResult::NetworkError, TEXT("Models request failed"));
        OnModelsListed.Broadcast(Result, ModelsResp);
        return;
    }
    int32 Code = Response->GetResponseCode();
    FString Content = Response->GetContentAsString();
    Result = ParseJSONResponse(Content, Code);
    if (Result.Result != ETextGenResult::Success)
    {
        OnModelsListed.Broadcast(Result, ModelsResp);
        return;
    }
    // Delegate parsing to the provider (OpenAI/OpenRouter style response)
    FString ParseError;
    if (!OpenAIProvider || !OpenAIProvider->ParseModelsResponse(Content, ModelsResp.Models, ParseError))
    {
        Result = CreateErrorResult(ETextGenResult::ParseError,
            ParseError.IsEmpty() ? TEXT("Failed to parse models response") : ParseError,
            Code);
    }
    ModelsResp.DebugSummary = UTextGenSubsystem::BuildModelsDebugSummary(ModelsResp.Models);
    ModelsResp.DebugLines = UTextGenSubsystem::BuildModelsDebugLines(ModelsResp.Models);
    OnModelsListed.Broadcast(Result, ModelsResp);
}
