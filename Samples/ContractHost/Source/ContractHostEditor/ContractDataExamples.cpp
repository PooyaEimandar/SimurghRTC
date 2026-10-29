// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractDataExample.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/WorldFactory.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompiler.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/SavePackage.h"

namespace
{
struct FDataGraph
{
    UEdGraph* Graph;
    bool bValid = true;
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    template<class T> T* Node(int32 X, int32 Y, const TCHAR* Comment = TEXT(""))
    {
        auto* N = NewObject<T>(Graph); Graph->AddNode(N,false,false); N->CreateNewGuid();
        N->NodePosX = X; N->NodePosY = Y; N->NodeComment = Comment; N->bCommentBubbleVisible = *Comment != 0;
        return N;
    }
    void Wire(UEdGraphNode* A, const TCHAR* Out, UEdGraphNode* B, const TCHAR* In)
    {
        if (!A || !B || !A->FindPin(Out) || !B->FindPin(In) || !Schema->TryCreateConnection(A->FindPin(Out),B->FindPin(In)))
        { bValid = false; UE_LOG(LogTemp,Error,TEXT("Data example cannot connect %s.%s -> %s.%s"),
            A ? *A->GetNodeTitle(ENodeTitleType::FullTitle).ToString() : TEXT("null"), Out,
            B ? *B->GetNodeTitle(ENodeTitleType::FullTitle).ToString() : TEXT("null"), In); }
    }
    void Then(UEdGraphNode* A, UEdGraphNode* B) { Wire(A,TEXT("then"),B,TEXT("execute")); }
    UK2Node_CallFunction* Call(UClass* Class, const TCHAR* Name, int32 X, int32 Y)
    {
        auto* N = Node<UK2Node_CallFunction>(X,Y); N->SetFromFunction(Class->FindFunctionByName(Name)); N->AllocateDefaultPins(); return N;
    }
    UK2Node_CallFunction* Sample(const TCHAR* Name, int32 X, int32 Y) { return Call(AContractDataExample::StaticClass(),Name,X,Y); }
    UK2Node_VariableGet* Get(const TCHAR* Name, int32 X, int32 Y)
    {
        auto* N = Node<UK2Node_VariableGet>(X,Y); N->VariableReference.SetSelfMember(Name); N->AllocateDefaultPins(); return N;
    }
    UK2Node_VariableSet* Set(const TCHAR* Name, int32 X, int32 Y)
    {
        auto* N = Node<UK2Node_VariableSet>(X,Y); N->VariableReference.SetSelfMember(Name); N->AllocateDefaultPins(); return N;
    }
    UK2Node_CallFunction* SessionCall(const TCHAR* Name, int32 X, int32 Y)
    {
        auto* N = Call(USimurghRTCSession::StaticClass(),Name,X,Y);
        Wire(Get(TEXT("Session"),X,Y+170),TEXT("Session"),N,TEXT("self")); return N;
    }
    UK2Node_AsyncAction* Async(const TCHAR* Name, int32 X, int32 Y)
    {
        auto* N = Node<UK2Node_AsyncAction>(X,Y,TEXT("Async nodes activate automatically; keep the session on the actor"));
        N->InitializeProxyFromFunction(USimurghRTCOperation::StaticClass()->FindFunctionByName(Name)); N->AllocateDefaultPins();
        Wire(Get(TEXT("Session"),X-240,Y+150),TEXT("Session"),N,TEXT("Session"));
        const TCHAR* Input = FCString::Strcmp(Name,TEXT("Connect")) == 0 ? TEXT("Config")
            : FCString::Strcmp(Name,TEXT("OpenChannel")) == 0 ? TEXT("Options")
            : FCString::Strcmp(Name,TEXT("CloseChannel")) == 0 ? TEXT("Channel") : nullptr;
        if (Input) Wire(Get(Input,X-240,Y+230),Input,N,Input);
        auto* Track = Sample(TEXT("TrackOperation"),X+420,Y-240);
        Track->NodeComment = TEXT("Retain the pending proxy and record its ID; this helper performs no RTC operation"); Track->bCommentBubbleVisible = true;
        Then(N,Track); Wire(N,TEXT("AsyncTaskProxy"),Track,TEXT("Operation"));
        return N;
    }
    UK2Node_CallFunction* Outcome(UK2Node_AsyncAction* Task, const TCHAR* Step, const TCHAR* Branch, int32 X, int32 Y)
    {
        auto* Record = Sample(TEXT("RecordOutcome"),X,Y);
        Record->FindPinChecked(TEXT("Step"))->DefaultValue = Step;
        Record->FindPinChecked(TEXT("Branch"))->DefaultValue = Branch;
        Wire(Task,TEXT("Result"),Record,TEXT("Result"));
        const FString Pin = FString(TEXT("On"))+Branch;
        Wire(Task,*Pin,Record,TEXT("execute")); return Record;
    }
    void FailureBranches(UK2Node_AsyncAction* Task, const TCHAR* Step, int32 X, int32 Y, bool Disconnect = false)
    {
        auto* Failed = Outcome(Task,Step,TEXT("Failure"),X,Y);
        auto* Result = Node<UK2Node_BreakStruct>(X,Y+160); Result->StructType = FSimurghRTCOperationResult::StaticStruct(); Result->AllocateDefaultPins();
        auto* Error = Node<UK2Node_BreakStruct>(X+360,Y+160); Error->StructType = FSimurghRTCError::StaticStruct(); Error->AllocateDefaultPins();
        auto* Print = Call(UKismetSystemLibrary::StaticClass(),TEXT("PrintString"),X+720,Y);
        Print->NodeComment = TEXT("Display the safe shared error; never print credentials or provider payloads"); Print->bCommentBubbleVisible = true;
        Print->FindPinChecked(TEXT("bPrintToScreen"))->DefaultValue = TEXT("false");
        Wire(Task,TEXT("Result"),Result,TEXT("SimurghRTCOperationResult")); Wire(Result,TEXT("Error"),Error,TEXT("SimurghRTCError"));
        Wire(Error,TEXT("Message"),Print,TEXT("InString")); Then(Failed,Print);
        Then(Print,Sample(Disconnect ? TEXT("CompleteExample") : TEXT("RequestDisconnect"),X+1100,Y));
        Outcome(Task,Step,TEXT("Cancelled"),X,Y+650);
    }
    UK2Node_AddDelegate* Bind(const TCHAR* Delegate, const TCHAR* EventName, int32 X, int32 Y, int32 EventY, UK2Node_CustomEvent*& Event)
    {
        auto* Property = FindFProperty<FMulticastDelegateProperty>(USimurghRTCSession::StaticClass(),Delegate);
        auto* Bind = Node<UK2Node_AddDelegate>(X,Y); Bind->SetFromProperty(Property,false,USimurghRTCSession::StaticClass()); Bind->AllocateDefaultPins();
        Wire(Get(TEXT("Session"),X,Y+160),TEXT("Session"),Bind,TEXT("self"));
        Event = UK2Node_CustomEvent::CreateFromFunction(FVector2D(0,EventY),Graph,EventName,Property->SignatureFunction,false);
        bValid &= Schema->TryCreateConnection(Event->FindPinChecked(UK2Node_Event::DelegateOutputName),Bind->GetDelegatePin());
        return Bind;
    }
};

bool Save(UObject* Object, const FString& Filename)
{
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename),true);
    return UPackage::SavePackage(Object->GetOutermost(),Object,*Filename,Args);
}
bool SaveMap(const TCHAR* Name, UClass* ActorClass, bool Replace)
{
    const FString PackageName = FString(TEXT("/Game/Examples/Direct/Maps/"))+Name;
    const auto Filename = FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetMapPackageExtension());
    UWorld* World = nullptr;
    if (FPaths::FileExists(Filename))
    {
        if (!Replace) return false;
        UPackage* Package = LoadPackage(nullptr,*PackageName,LOAD_None);
        World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
        if (!World) return false;
        // A loaded inactive map is not registered with TActorIterator. Inspect its level directly.
        TArray<AActor*> Old;
        for (AActor* Actor : World->PersistentLevel->Actors)
            if (Actor && Actor->IsA<AContractDataExample>()) Old.Add(Actor);
        for (auto* Actor : Old)
            if (!World->DestroyActor(Actor,true)) return false;
    }
    else
    {
        auto* Factory = NewObject<UWorldFactory>(); Factory->bCreateWorldPartition = false;
        World = Cast<UWorld>(Factory->FactoryCreateNew(UWorld::StaticClass(),CreatePackage(*PackageName),Name,RF_Public|RF_Standalone,nullptr,GWarn));
    }
    if (!World || !World->SpawnActor<AContractDataExample>(ActorClass)) return false;
    int32 Count = 0;
    for (AActor* Actor : World->PersistentLevel->Actors)
        if (IsValid(Actor) && Actor->IsA<AContractDataExample>()) ++Count;
    if (Count != 1) { UE_LOG(LogTemp,Error,TEXT("Generated data map must contain exactly one sample actor.")); return false; }
    World->GetOutermost()->SetPackageFlags(PKG_ContainsMap);
    return Save(World,Filename);
}
}

int32 GenerateContractDataExamples(const FString& Params)
{
    if (!SimurghRTC::IsProviderCompiled(ESimurghRTCProvider::DirectUnreal))
    { UE_LOG(LogTemp,Error,TEXT("The Direct data examples require a Direct-enabled profile.")); return 1; }
    const bool Replace = FParse::Param(*Params,TEXT("ReplaceGeneratedExample"));
    const FString PackageName = TEXT("/Game/Examples/Direct/BP_RtcData");
    const auto Filename = FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetAssetPackageExtension());
    UBlueprint* BP = nullptr;
    if (FPaths::FileExists(Filename))
    {
        if (!Replace) { UE_LOG(LogTemp,Error,TEXT("Data example exists; use -ReplaceGeneratedExample to regenerate.")); return 1; }
        auto* Package = LoadPackage(nullptr,*PackageName,LOAD_None); if (Package) Package->FullyLoad();
        BP = Package ? FindObject<UBlueprint>(Package,TEXT("BP_RtcData")) : nullptr;
        if (!BP || BP->ParentClass != AContractDataExample::StaticClass()) return 1;
    }
    else BP = FKismetEditorUtilities::CreateBlueprint(AContractDataExample::StaticClass(),CreatePackage(*PackageName),TEXT("BP_RtcData"),BPTYPE_Normal);
    FDataGraph G{FBlueprintEditorUtils::FindEventGraph(BP)};
    // Removing delegate bindings can materialize replacement Self nodes. Remove those too.
    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        const auto OldNodes = G.Graph->Nodes;
        for (UEdGraphNode* N : OldNodes) FBlueprintEditorUtils::RemoveNode(BP,N,true);
    }
    if (!G.Graph->Nodes.IsEmpty()) { UE_LOG(LogTemp,Error,TEXT("Could not clear the generated data graph.")); return 2; }
    int32 Y = 0;
    auto* Begin = FKismetEditorUtilities::AddDefaultEventNode(BP,G.Graph,TEXT("ReceiveBeginPlay"),AActor::StaticClass(),Y);
    auto* Prepare = G.Sample(TEXT("PrepareExample"),300,0); G.Then(Begin,Prepare);
    auto* Ready = G.Node<UK2Node_IfThenElse>(650,0); Ready->AllocateDefaultPins(); G.Then(Prepare,Ready); G.Wire(Prepare,TEXT("ReturnValue"),Ready,TEXT("Condition"));
    auto* Create = G.Call(USimurghRTCSession::StaticClass(),TEXT("CreateSession"),950,0); G.Wire(Ready,TEXT("then"),Create,TEXT("execute"));
    auto* Store = G.Set(TEXT("Session"),1300,0); G.Then(Create,Store); G.Wire(Create,TEXT("ReturnValue"),Store,TEXT("Session"));
    UK2Node_CustomEvent *Message, *ChannelEvent, *StateEvent;
    auto* BindMessage = G.Bind(TEXT("OnMessage"),TEXT("ReceiveRtcMessage"),1650,0,2000,Message);
    auto* BindChannel = G.Bind(TEXT("OnChannelChanged"),TEXT("RtcChannelChanged"),2000,0,3500,ChannelEvent);
    auto* BindState = G.Bind(TEXT("OnStateChanged"),TEXT("RtcStateChanged"),2350,0,3900,StateEvent);
    G.Then(Store,BindMessage); G.Then(BindMessage,BindChannel); G.Then(BindChannel,BindState);
    auto* Connect = G.Async(TEXT("Connect"),2800,0); G.Then(BindState,Connect);
    auto* Connected = G.Outcome(Connect,TEXT("Connect"),TEXT("Success"),3250,0);
    G.FailureBranches(Connect,TEXT("Connect"),3250,700);
    auto* Open = G.Async(TEXT("OpenChannel"),3700,0); G.Then(Connected,Open);
    auto* Opened = G.Outcome(Open,TEXT("OpenChannel"),TEXT("Success"),4150,0);
    G.FailureBranches(Open,TEXT("OpenChannel"),4650,700);
    auto* Break = G.Node<UK2Node_BreakStruct>(4150,250); Break->StructType = FSimurghRTCOperationResult::StaticStruct(); Break->AllocateDefaultPins();
    G.Wire(Open,TEXT("Result"),Break,TEXT("SimurghRTCOperationResult"));
    auto* KeepChannel = G.Set(TEXT("Channel"),4600,0); G.Then(Opened,KeepChannel); G.Wire(Break,TEXT("Channel"),KeepChannel,TEXT("Channel"));
    auto* Info = G.SessionCall(TEXT("GetChannelInfo"),4950,220); G.Wire(G.Get(TEXT("Channel"),4750,350),TEXT("Channel"),Info,TEXT("Channel"));
    // UHT exposes this const query as pure; evaluate it after the channel variable is stored.
    auto* RecordInfo = G.Sample(TEXT("RecordChannelInfo"),5350,0); G.Then(KeepChannel,RecordInfo); G.Wire(Info,TEXT("ReturnValue"),RecordInfo,TEXT("Error")); G.Wire(Info,TEXT("Info"),RecordInfo,TEXT("Info"));
    UEdGraphNode* Last = RecordInfo;
    int32 X = 5800;
    for (const auto Kind : {TEXT("Text"),TEXT("Bytes")})
    {
        const FString Function = FString(TEXT("Send"))+Kind, Variable = FString(Kind)+TEXT("ToSend");
        auto* Send = G.SessionCall(*Function,X,0); G.Then(Last,Send);
        G.Wire(G.Get(TEXT("Channel"),X,250),TEXT("Channel"),Send,TEXT("Channel"));
        G.Wire(G.Get(*Variable,X,330),*Variable,Send,Kind);
        auto* Record = G.Sample(TEXT("RecordSend"),X+400,0); G.Then(Send,Record); G.Wire(Send,TEXT("ReturnValue"),Record,TEXT("Error")); Last = Record; X += 850;
    }
    auto* Received = G.Sample(TEXT("RecordMessage"),350,2000); G.Then(Message,Received); G.Wire(Message,TEXT("Message"),Received,TEXT("Message"));
    auto* Both = G.Node<UK2Node_IfThenElse>(750,2000,TEXT("Close only after both exact echoes arrive")); Both->AllocateDefaultPins();
    G.Then(Received,Both); G.Wire(Received,TEXT("ReturnValue"),Both,TEXT("Condition"));
    auto* Close = G.Async(TEXT("CloseChannel"),1100,2000); G.Then(Both,Close);
    auto* Closed = G.Outcome(Close,TEXT("CloseChannel"),TEXT("Success"),1550,2000); G.Then(Closed,G.Sample(TEXT("RequestDisconnect"),1950,2000));
    G.FailureBranches(Close,TEXT("CloseChannel"),1550,2600);
    auto* ChannelRecord = G.Sample(TEXT("RecordChannelEvent"),400,3500); G.Then(ChannelEvent,ChannelRecord); G.Wire(ChannelEvent,TEXT("Channel"),ChannelRecord,TEXT("Info"));
    auto* StateRecord = G.Sample(TEXT("RecordStateEvent"),400,3900); G.Then(StateEvent,StateRecord); G.Wire(StateEvent,TEXT("State"),StateRecord,TEXT("State"));
    Y = 4500;
    auto* DisconnectEvent = FKismetEditorUtilities::AddDefaultEventNode(BP,G.Graph,TEXT("RequestDisconnect"),AContractDataExample::StaticClass(),Y);
    auto* Disconnect = G.Async(TEXT("Disconnect"),400,4500); G.Then(DisconnectEvent,Disconnect);
    auto* Disconnected = G.Outcome(Disconnect,TEXT("Disconnect"),TEXT("Success"),850,4500); G.Then(Disconnected,G.Sample(TEXT("CompleteExample"),1250,4500));
    G.FailureBranches(Disconnect,TEXT("Disconnect"),850,5100,true);
    Y = 6100;
    auto* End = FKismetEditorUtilities::AddDefaultEventNode(BP,G.Graph,TEXT("ReceiveEndPlay"),AActor::StaticClass(),Y);
    auto* ValidSession = G.Call(UKismetSystemLibrary::StaticClass(),TEXT("IsValid"),0,6350);
    G.Wire(G.Get(TEXT("Session"),0,6500),TEXT("Session"),ValidSession,TEXT("Object"));
    auto* HasSession = G.Node<UK2Node_IfThenElse>(350,6100,TEXT("A missing runtime configuration may leave no session to close")); HasSession->AllocateDefaultPins();
    G.Then(End,HasSession); G.Wire(ValidSession,TEXT("ReturnValue"),HasSession,TEXT("Condition"));
    auto* Shutdown = G.SessionCall(TEXT("Shutdown"),700,6100); G.Then(HasSession,Shutdown); G.Then(Shutdown,G.Sample(TEXT("RecordEndPlay"),1150,6100));
    Shutdown->NodeComment = TEXT("Always shut down at EndPlay; pending operations settle silently"); Shutdown->bCommentBubbleVisible = true;
    if (!G.bValid) return 2;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FCompilerResultsLog Results;
    FKismetEditorUtilities::CompileBlueprint(BP,EBlueprintCompileOptions::SkipGarbageCollection,&Results);
    if (Results.NumErrors || Results.NumWarnings || BP->Status == BS_Error) return 2;
    if (!Save(BP,Filename) || !SaveMap(TEXT("RtcDataBlueprint"),BP->GeneratedClass,Replace)
        || !SaveMap(TEXT("RtcDataCpp"),AContractDataCpp::StaticClass(),Replace)) return 3;
    UE_LOG(LogTemp,Display,TEXT("Saved data Blueprint (%d public/standard/reporting nodes; zero warnings/errors) and both data-client maps."),G.Graph->Nodes.Num());
    return 0;
}
