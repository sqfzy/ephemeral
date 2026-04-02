/// @file binance_book.cpp
/// End-to-end Binance bookTicker example — WebSocket → JSON → Book → BBO.
///
/// Connects to Binance's futures WebSocket feed, subscribes to a bookTicker
/// stream, parses each message with the zero-copy JSON parser, updates an
/// ArrayBook via the BinanceBookAdapter, and prints BBO / mid / spread.
///
/// Usage:
///   ./binance_book                                  # BTCUSDT, 10s
///   ./binance_book --symbol ethusdt --duration 30
///   ./binance_book --host stream.binance.com        # spot instead of futures

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/net/socket_transport.hpp"
#include "eph/transport/transport.hpp"
#include "eph/json/parser.hpp"
#include "eph/json/adapters/binance.hpp"
#include "eph/book/binance_adapter.hpp"

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    // -- CLI arguments --------------------------------------------------------
    std::string host     = "fstream.binance.com";
    std::string symbol   = "btcusdt";
    int         duration = 10;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc)     host = argv[++i];
        else if (arg == "--symbol" && i + 1 < argc) symbol = argv[++i];
        else if (arg == "--duration" && i + 1 < argc) duration = std::atoi(argv[++i]);
        else if (arg == "--help") {
            std::cerr << std::format(
                "Usage: {} [--host H] [--symbol S] [--duration N]\n"
                "  --host      WebSocket host (default: fstream.binance.com)\n"
                "  --symbol    Trading pair   (default: btcusdt)\n"
                "  --duration  Run time in seconds (default: 10)\n",
                argv[0]);
            return 0;
        }
    }

    // -- Build WebSocket path -------------------------------------------------
    auto ws_path = eph::json::binance::ws_path(symbol, "bookTicker");
    spdlog::info("Connecting to wss://{}{}  (duration={}s)", host, ws_path, duration);

    // -- Transport setup ------------------------------------------------------
    eph::net::SocketConfig sock_cfg{
        .host        = host,
        .port        = 443,
        .tcp_nodelay = true,
    };

    eph::net::TransportConfig transport_cfg{
        .remote_host = host,
        .remote_port = 443,
        .ws_path     = ws_path,
        .use_tls     = true,
        .verify_peer = false,  // Relaxed for demo — enable in production
    };

    auto tcp_factory = [&sock_cfg]()
        -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
        auto tcp = std::make_unique<eph::net::SocketTransport>(sock_cfg);
        auto result = tcp->connect(std::chrono::milliseconds{5000});
        if (!result) return std::unexpected(result.error());
        return tcp;
    };

    auto result = eph::net::Transport<eph::net::SocketTransport>::create(
        std::move(tcp_factory), transport_cfg);
    if (!result) {
        spdlog::error("Connection failed: {}", result.error().message());
        return 1;
    }
    auto& tp = **result;
    spdlog::info("Connected — receiving {} bookTicker", symbol);

    // -- Receive loop ---------------------------------------------------------
    eph::book::BinanceBookAdapter<5> adapter;  // BBO-only, 5 levels is plenty
    uint64_t msg_count   = 0;
    uint64_t parse_fails = 0;

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(duration);

    while (std::chrono::steady_clock::now() < deadline) {
        bool got = tp.recv([&](const uint8_t* data, size_t len) {
            // Parse JSON
            auto json = eph::json::parse(data, len);
            if (!json) {
                ++parse_fails;
                SPDLOG_DEBUG("JSON parse failed: {}",
                             eph::json::parse_error_name(json.error()));
                return;
            }

            // Extract BookTicker
            auto ticker = eph::json::binance::BookTicker::from(json.value());
            if (!ticker) {
                // Subscription confirmations and other non-ticker messages
                SPDLOG_DEBUG("Non-ticker message, skipping");
                return;
            }

            // Update order book
            if (!adapter.update_from_ticker(*ticker)) {
                SPDLOG_WARN("Book update failed for symbol={}", ticker->symbol);
                return;
            }

            ++msg_count;
            const auto& book = adapter.book();
            auto mid    = book.mid_price();
            auto spread = book.spread();
            auto bid    = book.best_bid();
            auto ask    = book.best_ask();

            if (bid && ask && mid && spread) {
                std::cout << std::format(
                    "[#{:>6}] {} | bid {:.2f} x {:.4f} | ask {:.2f} x {:.4f} "
                    "| mid {:.2f} | spread {:.2f}\n",
                    msg_count, ticker->symbol,
                    bid->price, bid->qty,
                    ask->price, ask->qty,
                    *mid, *spread);
            }
        });

        if (!got) {
            // No message ready — brief yield to avoid busy-spin
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
    }

    // -- Summary stats --------------------------------------------------------
    tp.stop();

    std::cout << std::format(
        "\n--- Summary ---\n"
        "  Symbol:       {}\n"
        "  Duration:     {}s\n"
        "  Messages:     {}\n"
        "  Parse fails:  {}\n"
        "  Msg/sec:      {:.1f}\n",
        symbol, duration, msg_count, parse_fails,
        duration > 0 ? static_cast<double>(msg_count) / duration : 0.0);

    if (auto mid = adapter.book().mid_price()) {
        std::cout << std::format("  Last mid:     {:.2f}\n", *mid);
    }
    if (auto spread = adapter.book().spread()) {
        std::cout << std::format("  Last spread:  {:.2f}\n", *spread);
    }

    return 0;
}
