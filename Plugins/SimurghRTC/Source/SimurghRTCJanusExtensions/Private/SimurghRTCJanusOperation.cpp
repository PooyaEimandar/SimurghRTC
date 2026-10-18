#include "SimurghRTCJanusOperation.h"
#include "SimurghRTCSession.h"

USimurghRTCJanusOperation* USimurghRTCJanusOperation::JoinVideoRoom(USimurghRTCSession* Session, const FSimurghRTCJanusRoomOptions& Options)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCJanusOperation>();
    FSimurghRTCError Error;
    if (Options.RoomId.IsEmpty())
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("A room identity is required."));
    else if (IsValid(Session) && Session->GetProvider() != ESimurghRTCProvider::Janus)
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::ServiceUnsupported, TEXT("VideoRoom requires a Janus session."));
    Operation->Initialize(Session, TEXT("JoinVideoRoom"), ESimurghRTCFeature::JanusRooms, Error);
    return Operation;
}
