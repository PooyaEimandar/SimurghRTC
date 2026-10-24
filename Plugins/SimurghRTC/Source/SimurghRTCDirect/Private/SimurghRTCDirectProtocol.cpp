// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCDirectProtocol.h"
#include "Hash/Blake3.h"
#include "Misc/Guid.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"

namespace SimurghRTC::Direct
{
TArray<uint8> Bytes(const FString& Value)
{
    FTCHARToUTF8 Utf8(*Value, Value.Len());
    TArray<uint8> Result; Result.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()); return Result;
}
FString Write(const TSharedRef<FJsonObject>& Object)
{
    FString Result;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result));
    return Result;
}
bool Fields(const TSharedPtr<FJsonObject>& Object, std::initializer_list<const TCHAR*> Names)
{
    if (!Object || Object->Values.Num() != int32(Names.size())) return false;
    for (const auto* Name : Names) if (!Object->HasField(Name)) return false;
    return true;
}
bool Integer(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, int32& Out, int32 Max)
{
    double N;
    if (!Object || !Object->HasTypedField<EJson::Number>(Name) || !Object->TryGetNumberField(Name, N)
        || !FMath::IsFinite(N) || N < 0 || N > Max || N != FMath::FloorToDouble(N)) return false;
    Out = int32(N); return true;
}
bool Text(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, FString& Out, int32 Max, bool bEmpty)
{
    if (!Object || !Object->TryGetStringField(Name, Out) || (!bEmpty && Out.IsEmpty())) return false;
    for (TCHAR C : Out) if (C == 0) return false;
    return Bytes(Out).Num() <= Max;
}
bool SessionId(const FString& Value)
{
    FGuid Id;
    return FGuid::ParseExact(Value, EGuidFormats::DigitsWithHyphens, Id)
        && Id.ToString(EGuidFormats::DigitsWithHyphensLower) == Value;
}
bool Sdp(const FString& Value)
{
    const FString Lines = Value.Replace(TEXT("\r\n"), TEXT(""));
    return Bytes(Value).Num() <= MaxSdpBytes && Value.StartsWith(TEXT("v=0\r\n")) && Value.EndsWith(TEXT("\r\n"))
        && !Lines.Contains(TEXT("\r")) && !Lines.Contains(TEXT("\n"));
}
bool Ice(const TSharedPtr<FJsonObject>& Body, FNativeSignalRequest& Out)
{
    int32 Revision;
    if (!Integer(Body, TEXT("revision"), Revision, 1) || Revision != 1) return false;
    if (Body->HasField(TEXT("completed")))
    {
        bool Complete = false; Out.Action = ENativeSignalAction::EndIce;
        return Fields(Body, {TEXT("revision"), TEXT("completed")}) && Body->TryGetBoolField(TEXT("completed"), Complete) && Complete;
    }
    Out.Action = ENativeSignalAction::AddIce;
    return Fields(Body, {TEXT("revision"), TEXT("candidate"), TEXT("sdp_mid"), TEXT("sdp_mline_index"), TEXT("username_fragment")})
        && Text(Body, TEXT("candidate"), Out.Ice.Candidate, 2048) && Out.Ice.Candidate.StartsWith(TEXT("candidate:"))
        && !Out.Ice.Candidate.Contains(TEXT("\r")) && !Out.Ice.Candidate.Contains(TEXT("\n"))
        && Text(Body, TEXT("sdp_mid"), Out.Ice.Mid, 32) && Integer(Body, TEXT("sdp_mline_index"), Out.Ice.MLineIndex, 31)
        && Text(Body, TEXT("username_fragment"), Out.Ice.UsernameFragment, 256);
}
bool ParseJson(const TArray<uint8>& Input, TSharedPtr<FJsonObject>& Out, FString& Error)
{
    Out.Reset(); Error = TEXT("InvalidArgument");
    if (Input.Num() > MaxMessageBytes) { Error = TEXT("PayloadTooLarge"); return false; }
    if (Input.IsEmpty() || Input.Contains(0)) return false;
    FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Input.GetData()), Input.Num());
    const FString Json(Converted.Length(), Converted.Get());
    if (Bytes(Json) != Input) return false; // Reject malformed UTF-8 rather than replacing it.
    struct FScope { bool bObject; TSet<FString> Keys; };
    TArray<FScope> Scopes;
    auto Reader = TJsonReaderFactory<>::Create(Json);
    EJsonNotation Token; int32 Roots = 0;
    while (Reader->ReadNext(Token))
    {
        if (Token == EJsonNotation::Error) return false;
        const bool bEnd = Token == EJsonNotation::ObjectEnd || Token == EJsonNotation::ArrayEnd;
        if (!bEnd)
        {
            if (Scopes.IsEmpty()) { if (++Roots != 1 || Token != EJsonNotation::ObjectStart) return false; }
            else if (Scopes.Last().bObject)
            {
                const auto Key = Reader->GetIdentifier();
                if (Scopes.Last().Keys.Contains(Key)) return false;
                Scopes.Last().Keys.Add(Key);
            }
        }
        if (Token == EJsonNotation::ObjectStart || Token == EJsonNotation::ArrayStart)
        {
            if (Scopes.Num() >= 8) return false;
            Scopes.Add({Token == EJsonNotation::ObjectStart, {}});
        }
        else if (bEnd) { if (Scopes.IsEmpty()) return false; Scopes.Pop(); }
        else if (Token == EJsonNotation::Number)
        {
            // v1 numbers are integers only. Reject 1.0/1e0 before JSON loses that distinction.
            const FString Number(Reader->GetValueAsNumberString());
            if (Number.IsEmpty()) return false;
            for (TCHAR C : Number) if (C < TEXT('0') || C > TEXT('9')) return false;
            if (Number.Len() > 10 || Reader->GetValueAsNumber() > MAX_int32) return false;
        }
        else if (Token == EJsonNotation::String)
        {
            const FString Value = Reader->GetValueAsString();
            for (TCHAR C : Value) if (C == 0) return false;
            const auto Encoded = Bytes(Value);
            FUTF8ToTCHAR RoundTrip(reinterpret_cast<const ANSICHAR*>(Encoded.GetData()), Encoded.Num());
            if (FString(RoundTrip.Length(), RoundTrip.Get()) != Value) return false;
        }
    }
    if (Roots != 1 || !Scopes.IsEmpty() || !Reader->GetErrorMessage().IsEmpty()) return false;
    return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Out) && Out.IsValid();
}
static TSharedPtr<FJsonValue> Sorted(const TSharedPtr<FJsonValue>& Value)
{
    if (Value->Type == EJson::Object)
    {
        auto Object = MakeShared<FJsonObject>(); TArray<FString> Keys;
        for (const auto& Entry : Value->AsObject()->Values) Keys.Add(FString(Entry.Key));
        Keys.Sort();
        for (const auto& Key : Keys) Object->SetField(Key, Sorted(Value->AsObject()->TryGetField(Key)));
        return MakeShared<FJsonValueObject>(Object);
    }
    if (Value->Type == EJson::Array)
    {
        TArray<TSharedPtr<FJsonValue>> Values; for (const auto& Item : Value->AsArray()) Values.Add(Sorted(Item));
        return MakeShared<FJsonValueArray>(Values);
    }
    return Value;
}
bool ParseRequest(const TArray<uint8>& Input, FRequest& Out, FString& Error)
{
    Out = {}; TSharedPtr<FJsonObject> Object;
    if (!ParseJson(Input, Object, Error)) return false;
    FString Protocol; int32 V;
    if (!Text(Object, TEXT("protocol"), Protocol, 32) || Protocol != TEXT("simurghrtc.direct")
        || !Integer(Object, TEXT("version"), V, 1) || V != 1) { Error = TEXT("UnsupportedVersion"); return false; }
    Error = TEXT("InvalidArgument");
    if (!Text(Object, TEXT("type"), Out.Type, 8) || !Text(Object, TEXT("request_id"), Out.Id, 64)
        || !Integer(Object, TEXT("sequence"), Out.Sequence)) return false;
    for (TCHAR C : Out.Id) if (!((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_' || C == '-')) return false;
    const bool Create = Out.Type == TEXT("create");
    if (Create)
    {
        if (!Fields(Object, {TEXT("protocol"), TEXT("version"), TEXT("type"), TEXT("request_id"), TEXT("sequence"), TEXT("body")})) return false;
    }
    else if (!Fields(Object, {TEXT("protocol"), TEXT("version"), TEXT("type"), TEXT("request_id"), TEXT("sequence"), TEXT("body"), TEXT("session_id")})
        || !Text(Object, TEXT("session_id"), Out.Session, 36) || !SessionId(Out.Session)) return false;
    const TSharedPtr<FJsonObject>* Body;
    if (!Object->TryGetObjectField(TEXT("body"), Body)) return false;
    Out.Body = *Body; FString Value, Purpose; int32 N; FNativeSignalRequest Candidate;
    bool Valid = false;
    if (Create) Valid = Fields(Out.Body, {TEXT("purpose"), TEXT("identity")})
        && Text(Out.Body, TEXT("purpose"), Purpose, 16) && (Purpose == TEXT("data") || Purpose == TEXT("call") || Purpose == TEXT("text-room"))
        && Text(Out.Body, TEXT("identity"), Value, 128);
    else if (Out.Type == TEXT("offer")) Valid = Fields(Out.Body, {TEXT("revision"), TEXT("sdp")})
        && Integer(Out.Body, TEXT("revision"), N, 1) && N == 1 && Text(Out.Body, TEXT("sdp"), Value, MaxSdpBytes) && Sdp(Value);
    else if (Out.Type == TEXT("ice")) Valid = Ice(Out.Body, Candidate);
    else if (Out.Type == TEXT("poll")) Valid = Fields(Out.Body, {TEXT("after_event")}) && Integer(Out.Body, TEXT("after_event"), N);
    else if (Out.Type == TEXT("close")) Valid = Fields(Out.Body, {});
    if (!Valid) return false;
    const auto Canonical = Bytes(Write(Sorted(MakeShared<FJsonValueObject>(Object))->AsObject().ToSharedRef()));
    const auto Hash = FBlake3::HashBuffer(Canonical.GetData(), Canonical.Num());
    Out.Digest = BytesToHex(Hash.GetBytes(), 32); Error.Reset(); return true;
}
TSharedRef<FJsonObject> Envelope(const FRequest& Request, const FString& Session, const TCHAR* Status, TSharedRef<FJsonObject> Body)
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("protocol"), TEXT("simurghrtc.direct")); Result->SetNumberField(TEXT("version"), Version);
    Result->SetStringField(TEXT("type"), TEXT("result")); Result->SetStringField(TEXT("request_id"), Request.Id);
    Result->SetNumberField(TEXT("sequence"), Request.Sequence); Result->SetStringField(TEXT("session_id"), Session);
    Result->SetStringField(TEXT("status"), Status); Result->SetObjectField(TEXT("body"), Body); return Result;
}
TSharedRef<FJsonObject> IceBody(const FNativeIceCandidate& Candidate)
{
    auto Body = MakeShared<FJsonObject>(); Body->SetNumberField(TEXT("revision"), 1);
    Body->SetStringField(TEXT("candidate"), Candidate.Candidate); Body->SetStringField(TEXT("sdp_mid"), Candidate.Mid);
    Body->SetNumberField(TEXT("sdp_mline_index"), Candidate.MLineIndex); Body->SetStringField(TEXT("username_fragment"), Candidate.UsernameFragment); return Body;
}
int32 HttpStatus(const FString& Code)
{
    if (Code == TEXT("Unauthorized")) return 401;
    if (Code == TEXT("AdmissionRejected")) return 403;
    if (Code == TEXT("SessionNotFound")) return 404;
    if (Code == TEXT("PayloadTooLarge")) return 413;
    if (Code == TEXT("ResourceLimit") || Code == TEXT("Unavailable")) return 503;
    if (Code == TEXT("UnsupportedVersion")) return 426;
    if (Code == TEXT("InvalidArgument") || Code == TEXT("InvalidCursor")) return 400;
    return 409;
}
}
