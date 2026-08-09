#include "TextGen/Providers/LlamacppAPI.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Engine/Engine.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "TextGen/TextGenLocalServiceSubsystem.h"
#include "TextGen/TextGenSubsystem.h"

namespace
{
    FString GetRouterModelId()
    {
        if (const UTextGenLocalServiceSubsystem* Service = GEngine
            ? GEngine->GetEngineSubsystem<UTextGenLocalServiceSubsystem>() : nullptr)
        {
            return Service->GetActiveModelId();
        }
        return FString();
    }
}

void FLlamacppProvider::BuildGenerateRequestData(const UTextGenSubsystem* Subsystem, const FTextGenGenerationParams& Params, const FTextGenGenerationSettings& Settings,
    FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const
{
    FOpenAIProvider::BuildGenerateRequestData(Subsystem, Params, Settings, OutEndpoint, OutVerb, OutBodyJson);

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutBodyJson);
    if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
    {
        Root->SetNumberField(TEXT("id_slot"), 0);
        Root->SetBoolField(TEXT("cache_prompt"), true);
        Root->SetStringField(TEXT("model"), GetRouterModelId());
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutBodyJson);
        FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
    }
}

void FLlamacppProvider::BuildTokenCountRequestData(const UTextGenSubsystem* Subsystem, const FString& Text,
    FString& OutEndpoint, FString& OutVerb, FString& OutBodyJson) const
{
    const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("content"), Text);
    Root->SetStringField(TEXT("model"), GetRouterModelId());
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutBodyJson);
    FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
    OutEndpoint = TEXT("/tokenize");
    OutVerb = TEXT("POST");
}

void FLlamacppProvider::BuildVersionRequestData(const UTextGenSubsystem* Subsystem, FString& OutEndpoint, FString& OutVerb) const
{
    OutEndpoint = TEXT("/props?model=") + FGenericPlatformHttp::UrlEncode(GetRouterModelId());
    OutVerb = TEXT("GET");
}

void FLlamacppProvider::BuildMaxContextRequestData(const UTextGenSubsystem* Subsystem,
    bool& bImmediate, int32& OutImmediateValue, FString& OutEndpoint, FString& OutVerb) const
{
    bImmediate = false;
    OutImmediateValue = 0;
    OutEndpoint = TEXT("/props?model=") + FGenericPlatformHttp::UrlEncode(GetRouterModelId());
    OutVerb = TEXT("GET");
}

void FLlamacppProvider::BuildHealthRequestData(const UTextGenSubsystem* Subsystem, FString& OutEndpoint, FString& OutVerb) const
{
    OutEndpoint = TEXT("/health");
    OutVerb = TEXT("GET");
}

bool FLlamacppProvider::ParseTokenCountResponse(const FString& ResponseString, int32& OutTokenCount, FString& OutError) const
{
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
    const TArray<TSharedPtr<FJsonValue>>* Tokens = nullptr;
    if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid() && Root->TryGetArrayField(TEXT("tokens"), Tokens) && Tokens)
    {
        OutTokenCount = Tokens->Num();
        return true;
    }
    OutError = TEXT("llama.cpp tokenize response is invalid.");
    return false;
}

bool FLlamacppProvider::ParseVersionResponse(const FString& ResponseString, FString& OutVersion, FString& OutBuildInfo, FString& OutError) const
{
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        OutError = TEXT("llama.cpp props response is invalid.");
        return false;
    }
    Root->TryGetStringField(TEXT("build_info"), OutBuildInfo);
    Root->TryGetStringField(TEXT("version"), OutVersion);
    if (OutVersion.IsEmpty())
    {
        OutVersion = TEXT("llama.cpp");
    }
    return true;
}

bool FLlamacppProvider::ParseMaxContextResponse(const FString& ResponseString, int32& OutMaxContext, FString& OutError) const
{
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
    if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
    {
        double Context = 0.0;
        if (Root->TryGetNumberField(TEXT("n_ctx"), Context))
        {
            OutMaxContext = static_cast<int32>(Context);
            return true;
        }
        const TSharedPtr<FJsonObject>* Defaults = nullptr;
        if (Root->TryGetObjectField(TEXT("default_generation_settings"), Defaults) && Defaults && Defaults->IsValid()
            && (*Defaults)->TryGetNumberField(TEXT("n_ctx"), Context))
        {
            OutMaxContext = static_cast<int32>(Context);
            return true;
        }
    }
    OutError = TEXT("llama.cpp context size is missing from props.");
    return false;
}

bool FLlamacppProvider::ParseHealthResponse(const FString& ResponseString, bool& bOutHealthy, FString& OutError) const
{
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
    FString Status;
    if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid() && Root->TryGetStringField(TEXT("status"), Status))
    {
        bOutHealthy = Status.Equals(TEXT("ok"), ESearchCase::IgnoreCase);
        return true;
    }
    bOutHealthy = false;
    OutError = TEXT("llama.cpp health response is invalid.");
    return false;
}
