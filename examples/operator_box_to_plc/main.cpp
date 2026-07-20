// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
// operator_box_to_plc — Segment bridge between a Modbus TCP server
// (operator box) and a PLC client.
//
// One Core instance hosts:
//   - `box` transport     : listens on 127.0.0.1:5020 (operator-box facing)
//   - `plc` transport     : connects to 127.0.0.1:51500 (PLC facing)
//   - bridge command area : forwards operator-box HR[0..3] to PLC HR[0..3]
//   - bridge status area  : mirrors raw PLC HR[50..53] after every good poll
//
// Useful as a HMI-side bridge: operator boxes keep writing into their
// usual local register table; Core handles the real PLC connection
// underneath, with proper batching / debouncing / reconnect.

#include <QCoreApplication>
#include <QTextStream>

#include "core/ICore.h"
#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    QString path = (argc > 1)
        ? QString::fromLocal8Bit(argv[1])
        : QStringLiteral("bridge.toml");

    auto core = core::ICore::create(nullptr);
    auto loaded = core->loadConfig(path);
    if (!loaded.has_value()) {
        for (auto const& e : loaded.error()) {
            out << "[config] " << e.section << "." << e.field
                << ": " << e.message << "\n";
        }
        return 1;
    }

    auto subWrites = core->bus().subscribe<core::bus::ServerWriteEvent>(
        [&out](core::bus::ServerWriteEvent const& e) {
            out << "[operator-box] write to " << e.transportId
                << " @" << e.startAddress << " ×" << e.values.size() << "\n";
            out.flush();
        });
    auto subStats = core->bus().subscribe<core::bus::SchedulerStatsEvent>(
        [&out](core::bus::SchedulerStatsEvent const& s) {
            out << "[stats] " << s.transportId
                << "  queue=" << s.stats.queueDepth
                << "  inflight=" << s.stats.inflight
                << "  p99=" << s.stats.p99LatencyMs << "ms"
                << "  circuit=" << int(s.stats.circuitState) << "\n";
            out.flush();
        });
    auto subState = core->bus().subscribe<core::bus::TransportStateChanged>(
        [&out](core::bus::TransportStateChanged const& event) {
            out << "[transport] " << event.after.transportId
                << " state=" << int(event.after.state)
                << " revision=" << event.after.revision;
            if (!event.after.errorMessage.isEmpty()) {
                out << " error=" << event.after.errorMessage;
            }
            out << "\n";
            out.flush();
        });
    auto subPeer = core->bus().subscribe<core::bus::PeerSessionChanged>(
        [&out](core::bus::PeerSessionChanged const& event) {
            out << "[peer] " << event.session.transportId << " "
                << (event.kind == core::bus::PeerSessionChangeKind::Connected
                    ? "connected " : "disconnected ")
                << event.session.remoteEndpoint.address << ":"
                << event.session.remoteEndpoint.port << "\n";
            out.flush();
        });

    core->start();
    out << "Bridge running. Write box HR[0..3] to command the PLC and read "
           "box HR[50..53] for the latest successful raw PLC poll."
        << "\n";
    return app.exec();
}
