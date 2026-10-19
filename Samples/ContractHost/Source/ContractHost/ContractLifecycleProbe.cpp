// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractLifecycleProbe.h"
#include "ContractLifecycleExample.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

void FContractLifecycleProbe::Startup()
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCLifecycleProbe"))) return;
    ExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(this, &FContractLifecycleProbe::Shutdown);
    InitHandle = FCoreDelegates::GetOnPostEngineInit().AddLambda([this]
    {
        Started = FPlatformTime::Seconds();
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FContractLifecycleProbe::Tick));
    });
}
void FContractLifecycleProbe::Shutdown()
{
    FCoreDelegates::GetOnPostEngineInit().Remove(InitHandle);
    FCoreDelegates::OnEnginePreExit.Remove(ExitHandle);
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    InitHandle.Reset(); ExitHandle.Reset(); TickHandle.Reset();
    if (IsValid(Native.Get()) && IsValid(Native->Session)) Native->Session->Shutdown();
    if (IsValid(Blueprint.Get()) && IsValid(Blueprint->Session)) Blueprint->Session->Shutdown();
    Native.Reset(); Blueprint.Reset(); Extension.Reset();
}
bool FContractLifecycleProbe::Tick(float)
{
    if (FPlatformTime::Seconds() - Started > 30) { Finish(false, TEXT("Lifecycle probe timed out.")); return false; }
    if (!Native)
    {
        UWorld* World = nullptr;
        for (const auto& Context : GEngine->GetWorldContexts())
            if (Context.World() && Context.World()->IsGameWorld() && Context.World()->HasBegunPlay()) World = Context.World();
        if (!World) return true;
        // Package existence works with cooked Pak/IoStore content as well as loose Editor assets.
        const bool bHasAsset = FPackageName::DoesPackageExist(TEXT("/Game/Examples/Janus/BP_JanusProfile"));
        const bool bHasExtension = FindObject<UClass>(nullptr, TEXT("/Script/SimurghRTCJanusExtensions.SimurghRTCJanusOperation")) != nullptr;
        const bool bHasMedia = FindObject<UClass>(nullptr, TEXT("/Script/SimurghRTCMedia.SimurghRTCMediaOperation")) != nullptr;
        bProfileChecked = bHasAsset == bool(SIMURGHRTC_WITH_JANUS_EXTENSIONS)
            && bHasExtension == bool(SIMURGHRTC_WITH_JANUS_EXTENSIONS) && bHasMedia == bool(SIMURGHRTC_WITH_MEDIA);
        for (auto Pair : {TPair<FName,bool>(TEXT("SimurghRTCDirect"), bool(SIMURGHRTC_WITH_DIRECT)),
                          TPair<FName,bool>(TEXT("SimurghRTCJanus"), bool(SIMURGHRTC_WITH_JANUS))})
            bProfileChecked &= FModuleManager::Get().IsModuleLoaded(Pair.Key) == Pair.Value;
        if (!bProfileChecked) { Finish(false, TEXT("Profile modules, reflected types or optional assets do not match.")); return false; }
        if (bHasAsset)
        {
            UClass* ExtensionClass = LoadClass<AContractExtensionExample>(nullptr, TEXT("/Game/Examples/Janus/BP_JanusProfile.BP_JanusProfile_C"));
            if (ExtensionClass) Extension.Reset(World->SpawnActor<AContractExtensionExample>(ExtensionClass));
            if (!Extension) { Finish(false, TEXT("Optional Janus Blueprint could not run.")); return false; }
        }
        UClass* Class = LoadClass<AContractLifecycleExample>(nullptr, TEXT("/Game/Examples/BP_RtcLifecycle.BP_RtcLifecycle_C"));
        if (!Class) { Finish(false, TEXT("Cooked Blueprint example is missing.")); return false; }
        FString Provider = TEXT("DirectUnreal");
        FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCLifecycleProvider="), Provider);
        if (Provider != TEXT("DirectUnreal") && Provider != TEXT("Janus"))
        { Finish(false, TEXT("Unknown lifecycle provider.")); return false; }
        Native.Reset(World->SpawnActorDeferred<AContractLifecycleCpp>(AContractLifecycleCpp::StaticClass(), FTransform::Identity));
        Blueprint.Reset(World->SpawnActorDeferred<AContractLifecycleExample>(Class, FTransform::Identity));
        if (!Native || !Blueprint) { Finish(false, TEXT("Could not spawn lifecycle examples.")); return false; }
        for (AContractLifecycleExample* Example : {Native.Get(), Blueprint.Get()})
        {
            Example->Config.Provider = Provider == TEXT("Janus") ? ESimurghRTCProvider::Janus : ESimurghRTCProvider::DirectUnreal;
            Example->FinishSpawning(FTransform::Identity);
        }
    }
    if (!Native->IsComplete() || !Blueprint->IsComplete()) return true;
    if (Extension && !Extension->IsComplete()) return true;
    if (++QuietTicks < 3) return true; // Give stale ticker callbacks a chance to surface.
    if (!Native->Passed() || !Blueprint->Passed() || (Extension && !Extension->Passed()))
    { Finish(false, TEXT("Lifecycle outcomes failed.")); return false; }
    if (!bEndPlayChecked)
    {
        bEndPlayChecked = true;
        if (!Native->PrepareEndPlayCheck() || !Blueprint->PrepareEndPlayCheck())
        { Finish(false, TEXT("EndPlay did not immediately shut down the session.")); return false; }
        QuietTicks = 0;
        return true;
    }
    Finish(Native->EndPlayPassed() && Blueprint->EndPlayPassed(), TEXT(""));
    return false;
}
void FContractLifecycleProbe::Finish(bool bPassed, const FString& Error)
{
    FString RunId, Output;
    FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCLifecycleRunId="), RunId);
    FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCLifecycleReport="), Output);
    FGuid Guid;
    bPassed &= FGuid::ParseExact(RunId, EGuidFormats::DigitsWithHyphens, Guid) && !Output.IsEmpty();
    auto Report = MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("passed"), bPassed);
    Report->SetNumberField(TEXT("schema"), 2);
    Report->SetNumberField(TEXT("api_contract_version"), SimurghRTC::ApiContractVersion);
    Report->SetStringField(TEXT("run_id"), RunId);
    Report->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
    Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    Report->SetBoolField(TEXT("editor_build"), WITH_EDITOR != 0);
    Report->SetBoolField(TEXT("with_janus"), SIMURGHRTC_WITH_JANUS != 0);
    Report->SetBoolField(TEXT("with_direct"), SIMURGHRTC_WITH_DIRECT != 0);
    Report->SetBoolField(TEXT("with_media"), SIMURGHRTC_WITH_MEDIA != 0);
    Report->SetBoolField(TEXT("with_janus_extensions"), SIMURGHRTC_WITH_JANUS_EXTENSIONS != 0);
    Report->SetBoolField(TEXT("profile_checked"), bProfileChecked);
    Report->SetStringField(TEXT("janus_blueprint"), Extension ? (Extension->Passed() ? TEXT("passed") : TEXT("failed")) : TEXT("excluded"));
    Report->SetBoolField(TEXT("network_transport_tested"), false);
    Report->SetStringField(TEXT("provider"), Native && Native->Config.Provider == ESimurghRTCProvider::Janus ? TEXT("Janus") : TEXT("DirectUnreal"));
    Report->SetStringField(TEXT("error"), Error);
    for (auto Pair : {TPair<FString,AContractLifecycleExample*>(TEXT("cpp"), Native.Get()),
                      TPair<FString,AContractLifecycleExample*>(TEXT("blueprint"), Blueprint.Get())})
    {
        auto Value = MakeShared<FJsonObject>();
        Value->SetBoolField(TEXT("passed"), Pair.Value && Pair.Value->EndPlayPassed());
        TArray<TSharedPtr<FJsonValue>> Outcomes, Errors;
        if (Pair.Value)
        {
            for (const FString& Item : Pair.Value->GetOutcomes()) Outcomes.Add(MakeShared<FJsonValueString>(Item));
            for (const FString& Item : Pair.Value->GetErrors()) Errors.Add(MakeShared<FJsonValueString>(Item));
        }
        Value->SetArrayField(TEXT("outcomes"), Outcomes);
        Value->SetArrayField(TEXT("errors"), Errors);
        Value->SetBoolField(TEXT("end_play_checked"), bEndPlayChecked);
        Report->SetObjectField(Pair.Key, Value);
    }
    FString Json;
    FJsonSerializer::Serialize(Report, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output), true);
    FFileHelper::SaveStringToFile(Json, *Output);
    // Packaged Mac file policy can refuse an external report path; retain a correlated log copy.
    UE_LOG(LogTemp, Display, TEXT("SIMURGHRTC_LIFECYCLE_REPORT %s"), *Json);
    // Release UObject references before engine teardown; module unload is too late.
    Shutdown();
    FPlatformMisc::RequestExitWithStatus(false, bPassed ? 0 : 5);
}
