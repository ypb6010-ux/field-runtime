// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
//
// Minimal in-process Siemens S7 server (snap7) for the gateway S7 self-test.
// Registers a single DB area whose first three words the gateway's AsioS7Client
// polls (word index = byte/2):
//   DBW0 = 230  (temperature, scale 0.1 -> 23.0)
//   DBW2 = 1450 (speed)
//   DBW4 = 2    (run_state -> enum_u16 "fault")
//
// Usage: field_gateway_s7_mock_server [bind-addr] [db-number]
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#include <snap7/snap7_libmain.h>

namespace {

std::atomic<bool> g_running{true};
void onSignal(int) { g_running = false; }

// Store a 16-bit value big-endian at word index `w` (byte offset w*2).
void putWord(std::vector<std::uint8_t>& buf, std::size_t w, std::uint16_t v) {
    buf[w * 2]     = std::uint8_t(v >> 8);
    buf[w * 2 + 1] = std::uint8_t(v & 0xFFu);
}

} // namespace

int main(int argc, char** argv) {
    char const* addr = argc > 1 ? argv[1] : "127.0.0.1";
    int const db = argc > 2 ? std::atoi(argv[2]) : 1;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::vector<std::uint8_t> dbBuf(1024, 0);
    putWord(dbBuf, 0, 230);
    putWord(dbBuf, 1, 1450);
    putWord(dbBuf, 2, 2);

    S7Object server = Srv_Create();
    int rc = Srv_RegisterArea(server, srvAreaDB, word(db), dbBuf.data(), int(dbBuf.size()));
    if (rc != 0) {
        std::cerr << "Srv_RegisterArea failed: " << rc << "\n";
        Srv_Destroy(server);
        return EXIT_FAILURE;
    }
    rc = Srv_StartTo(server, addr);
    if (rc != 0) {
        std::cerr << "Srv_StartTo(" << addr << ") failed: " << rc
                  << " (port 102 may require privileges)\n";
        Srv_Destroy(server);
        return EXIT_FAILURE;
    }

    std::cout << "mock S7 server on " << addr << ":102 DB" << db
              << " (DBW0=230 DBW2=1450 DBW4=2)" << std::endl;

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    Srv_Stop(server);
    Srv_Destroy(server);
    return EXIT_SUCCESS;
}
