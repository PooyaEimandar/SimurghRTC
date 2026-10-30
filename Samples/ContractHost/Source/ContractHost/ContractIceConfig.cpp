// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractIceConfig.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"

bool LoadContractIceConfig(TArray<FSimurghRTCIceServer>& Servers, bool& bRelayOnly)
{
    FString Path;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCIceConfigFile="), Path)) return true;
    const int64 Bytes = IFileManager::Get().FileSize(*Path);
    FString Text; TSharedPtr<FJsonObject> Json;
    if (Bytes < 1 || Bytes > 65536 || !FFileHelper::LoadFileToString(Text, *Path)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid()) return false;
    const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
    bool Relay = false;
    if (!Json->TryGetBoolField(TEXT("relay_only"), Relay) || !Json->TryGetArrayField(TEXT("servers"), Entries) || Entries->Num() > 8) return false;
    TArray<FSimurghRTCIceServer> Parsed;
    for (const auto& Value : *Entries)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        FSimurghRTCIceServer Server;
        if (!Value->TryGetObject(Entry) || !Entry || !Entry->IsValid()
            || !(*Entry)->TryGetStringArrayField(TEXT("urls"), Server.Urls)
            || !(*Entry)->TryGetStringField(TEXT("username"), Server.Username)
            || !(*Entry)->TryGetStringField(TEXT("credential"), Server.Credential)) return false;
        Parsed.Add(MoveTemp(Server));
    }
    if (SimurghRTC::ValidateIceServers(Parsed, Relay).IsError()) return false;
    Servers = MoveTemp(Parsed); bRelayOnly = Relay; return true;
}
