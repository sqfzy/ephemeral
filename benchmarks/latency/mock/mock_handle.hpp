/// @file mock/mock_handle.hpp
/// Unified lifecycle for in-process mock servers.
///
/// Used by DPDK bench scenarios where the mock runs in a dedicated thread
/// (kernel TCP/UDP, on NIC-A) while the bench client uses DPDK on NIC-B.

#pragma once

#include <atomic>
#include <memory>
#include <thread>

namespace bench {

struct MockHandle {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> running =
        std::make_shared<std::atomic<bool>>(true);
};

inline void stop_mock(MockHandle& h) {
    if (h.running) h.running->store(false, std::memory_order_release);
    if (h.thread.joinable()) h.thread.join();
}

} // namespace bench
