#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "eph/sbe.hpp"

using namespace eph::sbe;

// ---------------------------------------------------------------------------
// Builders for the WS API envelope + nested order responses (spot_3_2.xml).
//   WebSocketResponse(50): block(3) | rateLimits group | id vs8 | result messageData
//   NewOrderAckResponse(300): block(24) | symbol vs8 | clientOrderId vs8
//   ErrorResponse(100): block(18) | msg varString16 | data optionalMessageData
// ---------------------------------------------------------------------------
namespace {

void put_le16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put_le32(std::vector<uint8_t>& b, uint32_t v) { for (int i=0;i<4;++i) b.push_back(uint8_t(v >> (8*i))); }
void put_le_i64(std::vector<uint8_t>& b, int64_t v) { uint64_t u; std::memcpy(&u,&v,8); for (int i=0;i<8;++i) b.push_back(uint8_t(u >> (8*i))); }
void put_vs8(std::vector<uint8_t>& b, const std::string& s) { b.push_back(uint8_t(s.size())); b.insert(b.end(), s.begin(), s.end()); }
void put_vs16(std::vector<uint8_t>& b, const std::string& s) { put_le16(b, uint16_t(s.size())); b.insert(b.end(), s.begin(), s.end()); }

std::vector<uint8_t> hdr(uint16_t block_len, uint16_t tid, uint16_t schema=3, uint16_t ver=2) {
    std::vector<uint8_t> b; put_le16(b,block_len); put_le16(b,tid); put_le16(b,schema); put_le16(b,ver); return b;
}

std::vector<uint8_t> build_new_order_ack(int64_t order_id, int64_t transact_us,
                                         const std::string& symbol, const std::string& cid) {
    auto b = hdr(24, 300);
    put_le_i64(b, order_id);
    put_le_i64(b, -1);            // orderListId (null)
    put_le_i64(b, transact_us);
    put_vs8(b, symbol);
    put_vs8(b, cid);
    return b;
}

std::vector<uint8_t> build_error(int16_t code, const std::string& msg) {
    auto b = hdr(18, 100);
    put_le16(b, uint16_t(code));  // code int16
    put_le_i64(b, 0);             // serverTime (opt)
    put_le_i64(b, 0);             // retryAfter (opt)
    put_vs16(b, msg);             // msg varString16
    put_le32(b, 0);               // data optionalMessageData -> length 0 (null)
    return b;
}

// CancelOrderResponse(305): fixed block(137) | symbol vs8 | origClientOrderId vs8 |
// clientOrderId vs8. The block carries transactTime @ +18 and orderStatus @ +58
// (the only two fixed fields the accessor surfaces); the rest is left zeroed.
std::vector<uint8_t> build_cancel_order(int64_t transact_us, uint8_t status, const std::string& symbol,
                                        const std::string& orig_cid, const std::string& cid) {
    auto b = hdr(137, 305);
    std::vector<uint8_t> block(137, 0);
    uint64_t u; std::memcpy(&u, &transact_us, 8);
    for (int i = 0; i < 8; ++i) block[18 + i] = uint8_t(u >> (8 * i));   // transactTime @ +18
    block[58] = status;                                                  // orderStatus @ +58
    b.insert(b.end(), block.begin(), block.end());
    put_vs8(b, symbol);       // data 200
    put_vs8(b, orig_cid);     // data 201
    put_vs8(b, cid);          // data 202 — our cid
    return b;
}

// Wrap an inner full SBE message as the WebSocketResponse(50) envelope.
std::vector<uint8_t> build_envelope(uint16_t status, const std::string& id,
                                    const std::vector<uint8_t>& inner) {
    auto b = hdr(3, 50);
    b.push_back(0);               // sbeSchemaIdVersionDeprecated boolEnum
    put_le16(b, status);          // status u16
    // rateLimits group (groupSize16Encoding): entryBlockLength=19, numInGroup=0.
    put_le16(b, 19);
    put_le16(b, 0);
    put_vs8(b, id);               // echoed request id
    put_le32(b, uint32_t(inner.size()));  // result messageData length
    b.insert(b.end(), inner.begin(), inner.end());
    return b;
}

} // namespace

TEST(SbeOrder, envelope_wraps_new_order_ack) {
    auto inner = build_new_order_ack(/*orderId*/555, /*transact*/1718000000123000, "BTCUSDT", "1001");
    auto buf = build_envelope(/*status*/200, /*id*/"42", inner);

    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value()) << parse_error_name(v.error());
    EXPECT_EQ(v->template_id, 50);

    auto env = binance::web_socket_response::decode(*v);
    ASSERT_TRUE(env.has_value()) << parse_error_name(env.error());
    EXPECT_EQ(env->status, 200);
    EXPECT_EQ(env->id, "42");
    EXPECT_EQ(env->result.template_id, 300);

    namespace ack = binance::new_order_ack;
    EXPECT_EQ(ack::order_id(env->result), 555);
    EXPECT_EQ(ack::transact_time_us(env->result), 1718000000123000);
    EXPECT_EQ(ack::symbol(env->result), "BTCUSDT");
    EXPECT_EQ(ack::client_order_id(env->result), "1001");
}

TEST(SbeOrder, envelope_wraps_cancel_order) {
    auto inner = build_cancel_order(/*transact*/1718000000999000, /*status Canceled*/3, "SOLUSDT", "orig9", "1042");
    auto buf = build_envelope(/*status*/200, /*id*/"99", inner);

    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value()) << parse_error_name(v.error());
    auto env = binance::web_socket_response::decode(*v);
    ASSERT_TRUE(env.has_value()) << parse_error_name(env.error());
    EXPECT_EQ(env->result.template_id, 305);

    namespace co = binance::cancel_order;
    EXPECT_EQ(co::transact_time_us(env->result), 1718000000999000);   // offset +18 verified here
    EXPECT_EQ(co::status(env->result), 3);                            // offset +58
    EXPECT_EQ(co::symbol(env->result), "SOLUSDT");
    EXPECT_EQ(co::client_order_id(env->result), "1042");
}

TEST(SbeOrder, envelope_wraps_error_response) {
    auto inner = build_error(/*code*/-2010, "Account has insufficient balance");
    auto buf = build_envelope(/*status*/400, /*id*/"7", inner);

    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());
    auto env = binance::web_socket_response::decode(*v);
    ASSERT_TRUE(env.has_value()) << parse_error_name(env.error());
    EXPECT_EQ(env->status, 400);
    EXPECT_EQ(env->id, "7");
    EXPECT_EQ(env->result.template_id, 100);
    EXPECT_EQ(binance::error_response::code(env->result), -2010);
    EXPECT_EQ(binance::error_response::msg(env->result), "Account has insufficient balance");
}

TEST(SbeOrder, status_accessor_without_walking) {
    auto inner = build_new_order_ack(1, 1, "X", "1");
    auto buf = build_envelope(200, "1", inner);
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(binance::web_socket_response::status(*v), 200);
}

TEST(SbeOrder, truncated_result_length_errors) {
    auto inner = build_new_order_ack(1, 1, "BTCUSDT", "1001");
    auto buf = build_envelope(200, "42", inner);
    buf.resize(buf.size() - 5);   // chop into the nested message
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());
    auto env = binance::web_socket_response::decode(*v);
    EXPECT_FALSE(env.has_value());
    EXPECT_EQ(env.error(), ParseError::kTruncated);
}

TEST(SbeOrder, dispatch_routes_envelope_and_nested) {
    auto inner = build_new_order_ack(1, 1, "X", "1");
    auto buf = build_envelope(200, "1", inner);
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());
    bool env_routed = false, ack_routed = false;
    dispatch(*v, [&](auto tag, const MessageView& mv) {
        if constexpr (std::is_same_v<decltype(tag), msg::WebSocketResponse>) {
            env_routed = true;
            auto e = binance::web_socket_response::decode(mv);
            ASSERT_TRUE(e.has_value());
            dispatch(e->result, [&](auto t2, const MessageView&) {
                if constexpr (std::is_same_v<decltype(t2), msg::NewOrderAck>) ack_routed = true;
            });
        }
    });
    EXPECT_TRUE(env_routed);
    EXPECT_TRUE(ack_routed);
}
