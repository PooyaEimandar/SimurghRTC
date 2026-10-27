// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractExamplesCommandlet.h"
#include "ContractLifecycleExample.h"
#include "ContractExtensionExample.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "Kismet/KismetSystemLibrary.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompiler.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Modules/ModuleManager.h"
#include "UObject/SavePackage.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, ContractHostEditor);

namespace
{
struct FExampleGraph
{
    UEdGraph* Graph;
    bool bValid = true;
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    template<class T> T* Node(int32 X, int32 Y, const TCHAR* Comment)
    {
        T* Value = NewObject<T>(Graph);
        Graph->AddNode(Value, false, false);
        Value->CreateNewGuid();
        Value->NodePosX = X; Value->NodePosY = Y;
        Value->NodeComment = Comment; Value->bCommentBubbleVisible = true;
        return Value;
    }
    UK2Node_CallFunction* Call(UClass* Class, const TCHAR* Name, int32 X, int32 Y)
    {
        auto* Value = Node<UK2Node_CallFunction>(X, Y, Name);
        Value->SetFromFunction(Class->FindFunctionByName(Name));
        Value->AllocateDefaultPins();
        return Value;
    }
    UK2Node_VariableGet* Get(const TCHAR* Name, int32 X, int32 Y)
    {
        auto* Value = Node<UK2Node_VariableGet>(X, Y, TEXT(""));
        Value->VariableReference.SetSelfMember(Name); Value->AllocateDefaultPins();
        return Value;
    }
    void Wire(UEdGraphNode* From, const TCHAR* Output, UEdGraphNode* To, const TCHAR* Input)
    {
        if (!From || !To || !From->FindPin(Output) || !To->FindPin(Input)
            || !Schema->TryCreateConnection(From->FindPin(Output), To->FindPin(Input)))
        {
            bValid = false;
            UE_LOG(LogTemp, Error, TEXT("Cannot connect Blueprint pins %s -> %s"), Output, Input);
        }
    }
    UK2Node_AsyncAction* Async(const TCHAR* Name, int32 X, int32 Y, const TCHAR* Comment)
    {
        auto* Value = Node<UK2Node_AsyncAction>(X, Y, Comment);
        Value->InitializeProxyFromFunction(USimurghRTCOperation::StaticClass()->FindFunctionByName(Name));
        Value->AllocateDefaultPins();
        auto* Session = Get(TEXT("Session"), X - 220, Y + 150);
        Wire(Session, TEXT("Session"), Value, TEXT("Session"));
        if (FCString::Strcmp(Name, TEXT("Connect")) == 0)
        {
            auto* Config = Get(TEXT("Config"), X - 220, Y + 230);
            Wire(Config, TEXT("Config"), Value, TEXT("Config"));
        }
        return Value;
    }
    UK2Node_CallFunction* Outcome(UK2Node_AsyncAction* Task, const TCHAR* Branch, const TCHAR* Step, int32 X, int32 Y)
    {
        auto* Record = Call(AContractLifecycleExample::StaticClass(), TEXT("RecordOutcome"), X, Y);
        Record->FindPinChecked(TEXT("Step"))->DefaultValue = Step;
        if (FCString::Strcmp(Step, TEXT("Connect")) == 0)
        {
            auto* Result = Node<UK2Node_BreakStruct>(X - 200, Y - 600, TEXT("Inspect the shared structured error"));
            Result->StructType = FSimurghRTCOperationResult::StaticStruct(); Result->AllocateDefaultPins();
            auto* Error = Node<UK2Node_BreakStruct>(X + 150, Y - 600, TEXT("Never log provider payloads or credentials"));
            Error->StructType = FSimurghRTCError::StaticStruct(); Error->AllocateDefaultPins();
            auto* Print = Call(UKismetSystemLibrary::StaticClass(), TEXT("PrintString"), X + 500, Y - 450);
            Print->FindPinChecked(TEXT("bPrintToScreen"))->DefaultValue = TEXT("false");
            Print->FindPinChecked(TEXT("bPrintToLog"))->DefaultValue = TEXT("true");
            Wire(Task, TEXT("Result"), Result, TEXT("SimurghRTCOperationResult"));
            Wire(Result, TEXT("Error"), Error, TEXT("SimurghRTCError"));
            Wire(Error, TEXT("Message"), Print, TEXT("InString"));
            Wire(Task, Branch, Print, TEXT("execute")); Wire(Print, TEXT("then"), Record, TEXT("execute"));
        }
        else Wire(Task, Branch, Record, TEXT("execute"));
        Wire(Task, TEXT("Result"), Record, TEXT("Result"));
        return Record;
    }
    void Unexpected(UK2Node_AsyncAction* Task, const TCHAR* Except, int32 X, int32 Y)
    {
        for (const TCHAR* Branch : {TEXT("OnSuccess"), TEXT("OnFailure"), TEXT("OnCancelled")})
        {
            if (FCString::Strcmp(Branch, Except) == 0) continue;
            auto* Fail = Call(AContractLifecycleExample::StaticClass(), TEXT("UnexpectedCompletion"), X, Y);
            Wire(Task, Branch, Fail, TEXT("execute")); Wire(Task, TEXT("Result"), Fail, TEXT("Result"));
            Y += 180;
        }
    }
    UK2Node_CallFunction* Shutdown(int32 X, int32 Y)
    {
        auto* Value = Call(USimurghRTCSession::StaticClass(), TEXT("Shutdown"), X, Y);
        Wire(Get(TEXT("Session"), X - 220, Y + 120), TEXT("Session"), Value, TEXT("self"));
        return Value;
    }
};
}

UContractExamplesCommandlet::UContractExamplesCommandlet()
{
    IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}
int32 UContractExamplesCommandlet::Main(const FString& Params)
{
    if (FParse::Param(*Params, TEXT("JanusExample")))
    {
        UClass* Extension = FindObject<UClass>(nullptr, TEXT("/Script/SimurghRTCJanusExtensions.SimurghRTCJanusOperation"));
        if (!Extension) { UE_LOG(LogTemp, Error, TEXT("This profile excludes the Janus extension module.")); return 1; }
        const FString PackageName = TEXT("/Game/Examples/Janus/BP_JanusProfile");
        const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
        if (FPaths::FileExists(Filename) && !FParse::Param(*Params, TEXT("ReplaceGeneratedExample")))
        { UE_LOG(LogTemp, Error, TEXT("Example exists; use -ReplaceGeneratedExample to regenerate it.")); return 1; }
        UPackage* Package = nullptr;
        UBlueprint* Blueprint = nullptr;
        if (FPaths::FileExists(Filename))
        {
            Package = LoadPackage(nullptr, *PackageName, LOAD_None);
            if (Package) Package->FullyLoad();
            Blueprint = Package ? FindObject<UBlueprint>(Package, TEXT("BP_JanusProfile")) : nullptr;
            if (!Blueprint || Blueprint->ParentClass != AContractExtensionExample::StaticClass()) return 1;
        }
        else
        {
            Package = CreatePackage(*PackageName);
            Blueprint = FKismetEditorUtilities::CreateBlueprint(AContractExtensionExample::StaticClass(), Package,
                TEXT("BP_JanusProfile"), BPTYPE_Normal);
        }
        FExampleGraph G{FBlueprintEditorUtils::FindEventGraph(Blueprint)};
        const auto PreviousNodes = G.Graph->Nodes;
        for (UEdGraphNode* Node : PreviousNodes) FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
        int32 Y = 0;
        auto* Begin = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, G.Graph, TEXT("ReceiveBeginPlay"), AActor::StaticClass(), Y);
        auto* Join = G.Node<UK2Node_AsyncAction>(300, 0, TEXT("Build-profile check: missing session must fail locally"));
        Join->InitializeProxyFromFunction(Extension->FindFunctionByName(TEXT("JoinVideoRoom")));
        Join->AllocateDefaultPins();
        auto* Options = G.Node<UK2Node_MakeStruct>(0, 200, TEXT("Optional reflected Janus options"));
        Options->StructType = FindObject<UScriptStruct>(nullptr, TEXT("/Script/SimurghRTCJanusExtensions.SimurghRTCJanusRoomOptions"));
        if (!Options->StructType) return 1;
        Options->AllocateDefaultPins();
        G.Wire(Options, TEXT("SimurghRTCJanusRoomOptions"), Join, TEXT("Options"));
        G.Wire(Begin, TEXT("then"), Join, TEXT("execute"));
        for (const TCHAR* Branch : {TEXT("OnSuccess"), TEXT("OnFailure"), TEXT("OnCancelled")})
        {
            auto* Record = G.Call(AContractExtensionExample::StaticClass(), TEXT("RecordExtensionResult"), 800, Y);
            Record->FindPinChecked(TEXT("bFailureBranch"))->DefaultValue = FCString::Strcmp(Branch, TEXT("OnFailure")) == 0 ? TEXT("true") : TEXT("false");
            G.Wire(Join, Branch, Record, TEXT("execute"));
            G.Wire(Join, TEXT("Result"), Record, TEXT("Result"));
            Y += 250;
        }
        if (!G.bValid) return 2;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        FCompilerResultsLog Results;
        FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
        if (Results.NumErrors || Results.NumWarnings || Blueprint->Status == BS_Error) return 2;
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
        if (!UPackage::SavePackage(Package, Blueprint, *Filename, Args)) return 3;
        UE_LOG(LogTemp, Display, TEXT("Saved compiled Janus Blueprint: %s (%d nodes, zero warnings/errors)"), *Filename, G.Graph->Nodes.Num());
        return 0;
    }
    const FString PackageName = TEXT("/Game/Examples/BP_RtcLifecycle");
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    if (FPaths::FileExists(Filename) && !FParse::Param(*Params, TEXT("ReplaceGeneratedExample")))
    {
        UE_LOG(LogTemp, Error, TEXT("Example exists. Use -ReplaceGeneratedExample only to regenerate this sample."));
        return 1;
    }
    UPackage* Package = nullptr;
    UBlueprint* Blueprint = nullptr;
    if (FPaths::FileExists(Filename))
    {
        Package = LoadPackage(nullptr, *PackageName, LOAD_None);
        if (Package) Package->FullyLoad();
        Blueprint = Package ? FindObject<UBlueprint>(Package, TEXT("BP_RtcLifecycle")) : nullptr;
        if (!Blueprint || Blueprint->ParentClass != AContractLifecycleExample::StaticClass())
        {
            UE_LOG(LogTemp, Error, TEXT("Existing sample could not be loaded with its expected parent class."));
            return 1;
        }
    }
    else
    {
        Package = CreatePackage(*PackageName);
        Blueprint = FKismetEditorUtilities::CreateBlueprint(AContractLifecycleExample::StaticClass(), Package,
            TEXT("BP_RtcLifecycle"), BPTYPE_Normal);
    }
    FExampleGraph G{FBlueprintEditorUtils::FindEventGraph(Blueprint)};
    check(G.Graph);
    // The replacement flag deliberately rebuilds this sample's event graph.
    const auto PreviousNodes = G.Graph->Nodes;
    for (UEdGraphNode* Node : PreviousNodes) FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
    int32 Y = 0;
    auto* Begin = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, G.Graph, TEXT("ReceiveBeginPlay"), AActor::StaticClass(), Y);
    auto* Create = G.Call(USimurghRTCSession::StaticClass(), TEXT("CreateSession"), 300, 0);
    auto* Store = G.Node<UK2Node_VariableSet>(650, 0, TEXT("Retain the session for the actor's lifetime"));
    Store->VariableReference.SetSelfMember(TEXT("Session")); Store->AllocateDefaultPins();
    G.Wire(Begin, TEXT("then"), Create, TEXT("execute"));
    G.Wire(Create, TEXT("then"), Store, TEXT("execute")); G.Wire(Create, TEXT("ReturnValue"), Store, TEXT("Session"));
    auto* Disconnect = G.Async(TEXT("Disconnect"), 1050, 0, TEXT("1. Local success; no network connection is claimed"));
    G.Wire(Store, TEXT("then"), Disconnect, TEXT("execute"));
    auto* Success = G.Outcome(Disconnect, TEXT("OnSuccess"), TEXT("Disconnect"), 1500, 0);
    G.Unexpected(Disconnect, TEXT("OnSuccess"), 1500, 260);

    auto* Connect = G.Async(TEXT("Connect"), 2050, 0, TEXT("2. Invalid Direct endpoint / unavailable Janus fails explicitly"));
    G.Wire(Success, TEXT("then"), Connect, TEXT("execute"));
    auto* Failure = G.Outcome(Connect, TEXT("OnFailure"), TEXT("Connect"), 2500, 0);
    G.Unexpected(Connect, TEXT("OnFailure"), 2500, 260);

    auto* CancelTask = G.Async(TEXT("Connect"), 3050, 0, TEXT("3. Cancel through the exposed Operation proxy"));
    G.Wire(Failure, TEXT("then"), CancelTask, TEXT("execute"));
    auto* Cancel = G.Call(USimurghRTCOperation::StaticClass(), TEXT("Cancel"), 3500, -250);
    G.Wire(CancelTask, TEXT("then"), Cancel, TEXT("execute")); G.Wire(CancelTask, TEXT("AsyncTaskProxy"), Cancel, TEXT("self"));
    auto* Cancelled = G.Outcome(CancelTask, TEXT("OnCancelled"), TEXT("Cancel"), 3500, 0);
    G.Unexpected(CancelTask, TEXT("OnCancelled"), 3500, 260);

    auto* Quiet = G.Async(TEXT("Connect"), 4050, 0, TEXT("4. Shutdown silently cancels outstanding work"));
    G.Wire(Cancelled, TEXT("then"), Quiet, TEXT("execute"));
    G.Unexpected(Quiet, TEXT(""), 4500, 260);
    auto* Stop = G.Shutdown(4500, 0);
    G.Wire(Quiet, TEXT("then"), Stop, TEXT("execute"));
    auto* Verify = G.Call(AContractLifecycleExample::StaticClass(), TEXT("VerifyQuietTeardown"), 4950, 0);
    G.Wire(Stop, TEXT("then"), Verify, TEXT("execute")); G.Wire(Quiet, TEXT("AsyncTaskProxy"), Verify, TEXT("Operation"));

    Y = 1200;
    auto* End = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, G.Graph, TEXT("ReceiveEndPlay"), AActor::StaticClass(), Y);
    auto* EndStop = G.Shutdown(400, 1200);
    EndStop->NodeComment = TEXT("Always shut down at owner EndPlay; do not wait for a completion callback");
    G.Wire(End, TEXT("then"), EndStop, TEXT("execute"));
    if (!G.bValid) return 2;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FCompilerResultsLog Results;
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
    if (Results.NumErrors || Results.NumWarnings || Blueprint->Status == BS_Error) return 2;
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
    if (!UPackage::SavePackage(Package, Blueprint, *Filename, Args)) return 3;
    UE_LOG(LogTemp, Display, TEXT("Saved compiled lifecycle Blueprint: %s (%d nodes, zero warnings/errors)"), *Filename, G.Graph->Nodes.Num());
    return 0;
}
