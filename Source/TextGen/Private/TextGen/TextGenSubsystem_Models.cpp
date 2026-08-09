// Copyright <--\, Inc. All Rights Reserved.

#include "TextGen/TextGenSubsystem.h"

#include "TextGen/TextGenLog.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

bool UTextGenSubsystem::ListModels(const UObject* WorldContext, ELLMProvider Provider, FString& OutErrorMessage)
{
    UTextGenSubsystem* Subsystem = GetTextGenSubsystem(WorldContext);
    if (!Subsystem)
    {
        OutErrorMessage = TEXT("Failed to get TextGen Subsystem");
        UE_LOG(LogTextGenAPI, Error, TEXT("%s"), *OutErrorMessage);
        return false;
    }

    const bool bSuccess = Subsystem->ListModelsInternal(Provider);
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

bool UTextGenSubsystem::ListModelsInternal(ELLMProvider Provider)
{
    // For OpenAI: single request to /v1/models
    // For KoboldCpp: aggregate /api/v1/model, /api/v1/config/max_context_length, /api/v1/config/max_length
    if (Provider == ELLMProvider::OpenAI)
    {
        FString Endpoint = TEXT("/v1/models");
        FString Verb = TEXT("GET");
        TSharedRef<IHttpRequest> Request = CreateRequestForProvider(Provider, Endpoint, Verb);
        Request->OnProcessRequestComplete().BindUObject(this, &UTextGenSubsystem::OnListModelsResponse);
        if (!Request->ProcessRequest())
        {
            LastErrorMessage = TEXT("Failed to start models list request (OpenAI)");
            return false;
        }
        return true;
    }

    // KoboldCpp path: chain requests. Simpler approach: issue three requests sequentially by nesting lambdas.
    // We store interim data in a shared accumulator captured by lambdas; thread safety acceptable (game thread only).
    struct FKoboldModelAccum
    {
        FString ModelId;
        int32 MaxContext = 0;
        int32 MaxLength = 0;
    };

    TSharedPtr<FKoboldModelAccum> Accum = MakeShared<FKoboldModelAccum>();

    auto BroadcastKobold = [this, Accum]()
    {
        FTextGenOperationResult Result;
        Result.Result = ETextGenResult::Success;
        Result.ErrorMessage = TEXT("");
        Result.ResponseCode = 200;

        FTextGenModelsResponse ModelsResp;
        FTextGenModelInfo Info;
        Info.Id = Accum->ModelId;
        Info.Name = Accum->ModelId;
        Info.ContextLength = Accum->MaxContext;
        Info.MaxCompletionTokens = Accum->MaxLength;

        // Kobold supports all parameters already - fill a representative list
        Info.SupportedParameters = {
            TEXT("max_length"),
            TEXT("temperature"),
            TEXT("top_p"),
            TEXT("top_k"),
            TEXT("rep_pen"),
            TEXT("stop_sequence"),
            TEXT("system_prompt")
        };

        // Free pricing
        Info.Pricing.Add(TEXT("prompt"), TEXT("0"));
        Info.Pricing.Add(TEXT("completion"), TEXT("0"));

        ModelsResp.Models.Add(Info);
        ModelsResp.DebugSummary = UTextGenSubsystem::BuildModelsDebugSummary(ModelsResp.Models);
        ModelsResp.DebugLines = UTextGenSubsystem::BuildModelsDebugLines(ModelsResp.Models);

        OnModelsListed.Broadcast(Result, ModelsResp);
    };

    // Request 1: model id
    {
        TSharedRef<IHttpRequest> ReqModel = CreateRequestForProvider(Provider, TEXT("/api/v1/model"), TEXT("GET"));
        ReqModel->OnProcessRequestComplete().BindLambda([this, Accum, BroadcastKobold](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
        {
            if (!bWasSuccessful || !Response.IsValid())
            {
                FTextGenOperationResult Err = CreateErrorResult(ETextGenResult::NetworkError, TEXT("Failed to get model id"));
                FTextGenModelsResponse Empty;
                OnModelsListed.Broadcast(Err, Empty);
                return;
            }

            const FString Content = Response->GetContentAsString();
            TSharedPtr<FJsonObject> Json;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
            if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
            {
                Accum->ModelId = Json->GetStringField(TEXT("result"));
            }
            else
            {
                Accum->ModelId = TEXT("UnknownModel");
            }

            // chain next request (max_context)
            TSharedRef<IHttpRequest> ReqCtx = CreateRequestForProvider(ELLMProvider::KoboldCpp, TEXT("/api/v1/config/max_context_length"), TEXT("GET"));
            ReqCtx->OnProcessRequestComplete().BindLambda([this, Accum, BroadcastKobold](FHttpRequestPtr R2, FHttpResponsePtr Resp2, bool bOk2)
            {
                if (bOk2 && Resp2.IsValid())
                {
                    TSharedPtr<FJsonObject> J;
                    TSharedRef<TJsonReader<>> Rd = TJsonReaderFactory<>::Create(Resp2->GetContentAsString());
                    if (FJsonSerializer::Deserialize(Rd, J) && J.IsValid())
                    {
                        Accum->MaxContext = J->GetIntegerField(TEXT("value"));
                    }
                }

                // chain final request (max_length)
                TSharedRef<IHttpRequest> ReqMaxLen = CreateRequestForProvider(ELLMProvider::KoboldCpp, TEXT("/api/v1/config/max_length"), TEXT("GET"));
                ReqMaxLen->OnProcessRequestComplete().BindLambda([this, Accum, BroadcastKobold](FHttpRequestPtr R3, FHttpResponsePtr Resp3, bool bOk3)
                {
                    if (bOk3 && Resp3.IsValid())
                    {
                        TSharedPtr<FJsonObject> J3;
                        TSharedRef<TJsonReader<>> Rd3 = TJsonReaderFactory<>::Create(Resp3->GetContentAsString());
                        if (FJsonSerializer::Deserialize(Rd3, J3) && J3.IsValid())
                        {
                            Accum->MaxLength = J3->GetIntegerField(TEXT("value"));
                        }
                    }
                    BroadcastKobold();
                });

                if (!ReqMaxLen->ProcessRequest())
                {
                    BroadcastKobold();
                }
            });

            if (!ReqCtx->ProcessRequest())
            {
                BroadcastKobold();
            }
        });

        if (!ReqModel->ProcessRequest())
        {
            LastErrorMessage = TEXT("Failed to start model listing (KoboldCpp)");
            return false;
        }
    }

    return true;
}

FString UTextGenSubsystem::BuildModelsDebugSummary(const TArray<FTextGenModelInfo>& Models)
{
    if (Models.Num() == 0)
    {
        return TEXT("(No Models)");
    }

    FString Out;
    Out += TEXT("Models Summary (Pricing per 1M tokens, USD)\n");

    for (int32 i = 0; i < Models.Num(); ++i)
    {
        const FTextGenModelInfo& M = Models[i];
        Out += FString::Printf(TEXT("[%d] %s\n"), i, *(!M.Name.IsEmpty() ? M.Name : M.Id));

        if (M.Name != M.Id && !M.Name.IsEmpty())
        {
            Out += FString::Printf(TEXT("    Id: %s\n"), *M.Id);
        }

        if (M.ContextLength > 0 || M.MaxCompletionTokens > 0)
        {
            Out += FString::Printf(TEXT("    Context=%d  MaxCompletion=%d\n"), M.ContextLength, M.MaxCompletionTokens);
        }

        if (M.SupportedParameters.Num() > 0)
        {
            const FString ParamsStr = FString::Join(M.SupportedParameters, TEXT(", "));
            Out += FString::Printf(TEXT("    Params: %s\n"), *ParamsStr);
        }

        if (M.Pricing.Num() > 0)
        {
            TArray<FString> PricingEntries;
            for (const auto& Pair : M.Pricing)
            {
                PricingEntries.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key, *Pair.Value));
            }
            PricingEntries.Sort();
            Out += FString::Printf(TEXT("    Pricing(1M USD): %s\n"), *FString::Join(PricingEntries, TEXT(", ")));
        }
    }

    return Out;
}

TArray<FString> UTextGenSubsystem::BuildModelsDebugLines(const TArray<FTextGenModelInfo>& Models)
{
    TArray<FString> Lines;
    if (Models.Num() == 0)
    {
        Lines.Add(TEXT("(No Models)"));
        return Lines;
    }

    for (int32 i = 0; i < Models.Num(); ++i)
    {
        const FTextGenModelInfo& M = Models[i];
        FString Line = FString::Printf(TEXT("[%d] %s"), i, *(!M.Name.IsEmpty() ? M.Name : M.Id));

        if (M.ContextLength > 0)
        {
            Line += FString::Printf(TEXT(" Ctx=%d"), M.ContextLength);
        }
        if (M.MaxCompletionTokens > 0)
        {
            Line += FString::Printf(TEXT(" MaxComp=%d"), M.MaxCompletionTokens);
        }

        if (M.SupportedParameters.Num() > 0)
        {
            // Keep line compact: list up to first 6 params then '+' if more
            const int32 MaxShow = 7;
            TArray<FString> Slice;
            Slice.Reserve(FMath::Min(MaxShow, M.SupportedParameters.Num()));

            for (int32 pi = 0; pi < M.SupportedParameters.Num() && pi < MaxShow; ++pi)
            {
                Slice.Add(M.SupportedParameters[pi]);
            }

            FString ParamsJoined = FString::Join(Slice, TEXT("|"));
            if (M.SupportedParameters.Num() > MaxShow)
            {
                ParamsJoined += TEXT("|+");
            }

            Line += FString::Printf(TEXT(" Params=%s"), *ParamsJoined);
        }

        if (M.Pricing.Num() > 0)
        {
            TArray<FString> PricingEntries;
            PricingEntries.Reserve(M.Pricing.Num());

            for (const auto& Pair : M.Pricing)
            {
                PricingEntries.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key, *Pair.Value));
            }
            PricingEntries.Sort();

            Line += FString::Printf(TEXT(" | Pricing(1M USD): %s"), *FString::Join(PricingEntries, TEXT(",")));
        }

        Lines.Add(Line);
    }

    return Lines;
}
