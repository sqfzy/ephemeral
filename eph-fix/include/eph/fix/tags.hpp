#pragma once

/// @file tags.hpp
/// FIX protocol tag constants and metadata.
///
/// Covers the most common tags used in HFT for order entry and market data.
/// Users can define additional tags as `inline constexpr uint32_t MyTag = NNNN;`.

#include <cstdint>
#include <string_view>

namespace eph::fix::tag {

// -- Session-level --
inline constexpr uint32_t BeginString       = 8;
inline constexpr uint32_t BodyLength        = 9;
inline constexpr uint32_t MsgType           = 35;
inline constexpr uint32_t SenderCompID      = 49;
inline constexpr uint32_t TargetCompID      = 56;
inline constexpr uint32_t MsgSeqNum         = 34;
inline constexpr uint32_t SendingTime       = 52;
inline constexpr uint32_t CheckSum          = 10;
inline constexpr uint32_t BeginSeqNo        = 7;
inline constexpr uint32_t EndSeqNo          = 16;
inline constexpr uint32_t PossDupFlag       = 43;
inline constexpr uint32_t PossResend        = 97;
inline constexpr uint32_t EncryptMethod     = 98;
inline constexpr uint32_t HeartBtInt        = 108;
inline constexpr uint32_t TestReqID         = 112;
inline constexpr uint32_t OrigSendingTime   = 122;
inline constexpr uint32_t GapFillFlag       = 123;
inline constexpr uint32_t ResetSeqNumFlag   = 141;
inline constexpr uint32_t NewSeqNo          = 36;
inline constexpr uint32_t SenderSubID       = 50;
inline constexpr uint32_t TargetSubID       = 57;

// -- Order entry --
inline constexpr uint32_t ClOrdID           = 11;
inline constexpr uint32_t OrigClOrdID       = 41;
inline constexpr uint32_t OrderID           = 37;
inline constexpr uint32_t ExecID            = 17;
inline constexpr uint32_t ExecType          = 150;
inline constexpr uint32_t OrdStatus         = 39;
inline constexpr uint32_t Symbol            = 55;
inline constexpr uint32_t SecurityID        = 48;
inline constexpr uint32_t SecurityIDSource  = 22;
inline constexpr uint32_t Side              = 54;
inline constexpr uint32_t OrderQty          = 38;
inline constexpr uint32_t OrdType           = 40;
inline constexpr uint32_t Price             = 44;
inline constexpr uint32_t StopPx            = 99;
inline constexpr uint32_t TimeInForce       = 59;
inline constexpr uint32_t TransactTime      = 60;
inline constexpr uint32_t Account           = 1;
inline constexpr uint32_t HandlInst         = 21;
inline constexpr uint32_t ExDestination     = 100;
inline constexpr uint32_t MinQty            = 110;
inline constexpr uint32_t MaxFloor          = 111;
inline constexpr uint32_t CumQty            = 14;
inline constexpr uint32_t AvgPx             = 6;
inline constexpr uint32_t LastPx            = 31;
inline constexpr uint32_t LastQty           = 32;
inline constexpr uint32_t LeavesQty         = 151;
inline constexpr uint32_t Text              = 58;
inline constexpr uint32_t CxlRejReason      = 102;
inline constexpr uint32_t OrdRejReason      = 103;

// -- Market data --
inline constexpr uint32_t MDReqID                = 262;
inline constexpr uint32_t SubscriptionRequestType = 263;
inline constexpr uint32_t MarketDepth            = 264;
inline constexpr uint32_t MDUpdateType           = 265;
inline constexpr uint32_t NoMDEntryTypes         = 267;
inline constexpr uint32_t MDEntryType            = 269;
inline constexpr uint32_t MDEntryPx              = 270;
inline constexpr uint32_t MDEntrySize            = 271;
inline constexpr uint32_t NoMDEntries            = 268;
inline constexpr uint32_t MDUpdateAction         = 279;
inline constexpr uint32_t SecurityExchange       = 207;

// -- Common MsgType values --
namespace msg_type {
inline constexpr char Heartbeat            = '0';
inline constexpr char TestRequest          = '1';
inline constexpr char ResendRequest        = '2';
inline constexpr char Reject               = '3';
inline constexpr char SequenceReset        = '4';
inline constexpr char Logon                = 'A';
inline constexpr char Logout               = '5';
inline constexpr char NewOrderSingle       = 'D';
inline constexpr char OrderCancelRequest   = 'F';
inline constexpr char OrderCancelReplace   = 'G';
inline constexpr char ExecutionReport      = '8';
inline constexpr char OrderCancelReject    = '9';
inline constexpr char MarketDataRequest    = 'V';
inline constexpr char MarketDataSnapshot   = 'W';
inline constexpr char MarketDataIncRefresh = 'X';

// Additional single-char MsgType values (FIX 4.4+)
inline constexpr char SecurityDefinition       = 'd';
inline constexpr char SecurityStatus           = 'f';
inline constexpr char MassQuote                = 'i';
inline constexpr char QuoteCancel              = 'Z';
inline constexpr char SecurityList             = 'y';
inline constexpr char SecurityListRequest      = 'x';

// Multi-character MsgType values (FIX 4.4+)
inline constexpr std::string_view TradeCaptureReport       = "AE";
inline constexpr std::string_view TradeCaptureReportAck    = "AR";
inline constexpr std::string_view PositionReport           = "AP";
} // namespace msg_type

/// Get human-readable name for a FIX tag number.
/// Returns "Unknown" for unrecognized tags.
inline constexpr std::string_view tag_name(uint32_t t) noexcept {
    switch (t) {
    case Account:               return "Account";
    case AvgPx:                 return "AvgPx";
    case BeginSeqNo:            return "BeginSeqNo";
    case BeginString:           return "BeginString";
    case BodyLength:            return "BodyLength";
    case CheckSum:              return "CheckSum";
    case ClOrdID:               return "ClOrdID";
    case CumQty:                return "CumQty";
    case CxlRejReason:          return "CxlRejReason";
    case EncryptMethod:         return "EncryptMethod";
    case EndSeqNo:              return "EndSeqNo";
    case ExDestination:         return "ExDestination";
    case ExecID:                return "ExecID";
    case ExecType:              return "ExecType";
    case GapFillFlag:           return "GapFillFlag";
    case HandlInst:             return "HandlInst";
    case HeartBtInt:            return "HeartBtInt";
    case LastPx:                return "LastPx";
    case LastQty:               return "LastQty";
    case LeavesQty:             return "LeavesQty";
    case MaxFloor:              return "MaxFloor";
    case MDEntryPx:             return "MDEntryPx";
    case MDEntrySize:           return "MDEntrySize";
    case MDEntryType:           return "MDEntryType";
    case MDReqID:               return "MDReqID";
    case MDUpdateAction:        return "MDUpdateAction";
    case MDUpdateType:          return "MDUpdateType";
    case MarketDepth:           return "MarketDepth";
    case MinQty:                return "MinQty";
    case MsgSeqNum:             return "MsgSeqNum";
    case MsgType:               return "MsgType";
    case NewSeqNo:              return "NewSeqNo";
    case NoMDEntries:           return "NoMDEntries";
    case NoMDEntryTypes:        return "NoMDEntryTypes";
    case OrdRejReason:          return "OrdRejReason";
    case OrdStatus:             return "OrdStatus";
    case OrdType:               return "OrdType";
    case OrderID:               return "OrderID";
    case OrderQty:              return "OrderQty";
    case OrigClOrdID:           return "OrigClOrdID";
    case OrigSendingTime:       return "OrigSendingTime";
    case PossDupFlag:           return "PossDupFlag";
    // Note: TestReqID added between PossDupFlag and PossResend alphabetically
    case TestReqID:             return "TestReqID";
    case PossResend:            return "PossResend";
    case Price:                 return "Price";
    case ResetSeqNumFlag:       return "ResetSeqNumFlag";
    case SecurityExchange:      return "SecurityExchange";
    case SecurityID:            return "SecurityID";
    case SecurityIDSource:      return "SecurityIDSource";
    case SenderCompID:          return "SenderCompID";
    case SenderSubID:           return "SenderSubID";
    case SendingTime:           return "SendingTime";
    case Side:                  return "Side";
    case StopPx:                return "StopPx";
    case SubscriptionRequestType: return "SubscriptionRequestType";
    case Symbol:                return "Symbol";
    case TargetCompID:          return "TargetCompID";
    case TargetSubID:           return "TargetSubID";
    case Text:                  return "Text";
    case TimeInForce:           return "TimeInForce";
    case TransactTime:          return "TransactTime";
    default:                    return "Unknown";
    }
}

/// Get human-readable name for a FIX MsgType character.
/// Returns "Unknown" for unrecognized message types.
inline constexpr std::string_view msg_type_name(char mt) noexcept {
    switch (mt) {
    case msg_type::Heartbeat:            return "Heartbeat";
    case msg_type::TestRequest:          return "TestRequest";
    case msg_type::ResendRequest:        return "ResendRequest";
    case msg_type::Reject:               return "Reject";
    case msg_type::SequenceReset:        return "SequenceReset";
    case msg_type::Logon:                return "Logon";
    case msg_type::Logout:               return "Logout";
    case msg_type::NewOrderSingle:       return "NewOrderSingle";
    case msg_type::OrderCancelRequest:   return "OrderCancelRequest";
    case msg_type::OrderCancelReplace:   return "OrderCancelReplace";
    case msg_type::ExecutionReport:      return "ExecutionReport";
    case msg_type::OrderCancelReject:    return "OrderCancelReject";
    case msg_type::MarketDataRequest:    return "MarketDataRequest";
    case msg_type::MarketDataSnapshot:   return "MarketDataSnapshot";
    case msg_type::MarketDataIncRefresh: return "MarketDataIncRefresh";
    case msg_type::SecurityDefinition:   return "SecurityDefinition";
    case msg_type::SecurityStatus:       return "SecurityStatus";
    case msg_type::MassQuote:            return "MassQuote";
    case msg_type::QuoteCancel:          return "QuoteCancel";
    case msg_type::SecurityList:         return "SecurityList";
    case msg_type::SecurityListRequest:  return "SecurityListRequest";
    default:                             return "Unknown";
    }
}

/// Get human-readable name for a multi-char MsgType string.
/// Falls back to single-char lookup for length-1 strings.
inline constexpr std::string_view msg_type_name(std::string_view mt) noexcept {
    if (mt.empty()) return "Unknown";
    if (mt.size() == 1) return msg_type_name(mt[0]);
    if (mt == msg_type::TradeCaptureReport)    return "TradeCaptureReport";
    if (mt == msg_type::TradeCaptureReportAck) return "TradeCaptureReportAck";
    if (mt == msg_type::PositionReport)        return "PositionReport";
    return "Unknown";
}

} // namespace eph::fix::tag
