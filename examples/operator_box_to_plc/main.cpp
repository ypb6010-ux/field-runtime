// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
// operator_box_to_plc — Mirror a Modbus TCP server (operator box) into a
// PLC client through a SinkWindow.
//
// One Core instance hosts:
//   - `box` transport     : listens on 127.0.0.1:5020 (operator-box facing)
//   - `plc` transport     : connects to 127.0.0.1:51500 (PLC facing)
//   - `sw.plc` SinkWindow : batches writes to PLC HR[100..104]
//   - route cmd_in → cmd_in: copies operator-box HR[0] writes into the
//                              SinkWindow at HR[100]
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

    auto core = core::ICore::create();
    auto loaded = core->loadConfig(path.toStdString());
    if (!loaded.has_value()) {
        for (auto const& e : loaded.error()) {
            out << "[config] " << e.section.c_str() << "." << e.field.c_str()
                << ": " << e.message.c_str() << "\n";
        }
        return 1;
    }

    auto subWrites = core->bus().subscribe<core::bus::ServerWriteEvent>(
        [&out](core::bus::ServerWriteEvent const& e) {
            out << "[operator-box] write to " << e.transportId.c_str()
                << " @" << e.startAddress << " ×" << e.values.size() << "\n";
            out.flush();
        });
    auto subStats = core->bus().subscribe<core::bus::SchedulerStatsEvent>(
        [&out](core::bus::SchedulerStatsEvent const& s) {
            out << "[stats] " << s.transportId.c_str()
                << "  queue=" << s.stats.queueDepth
                << "  inflight=" << s.stats.inflight
                << "  p99=" << s.stats.p99LatencyMs << "ms"
                << "  circuit=" << int(s.stats.circuitState) << "\n";
            out.flush();
        });

    core->start();
    out << "Bridge running. Connect a Modbus client to 127.0.0.1:5020 and "
           "write to HR[0]; the PLC at 127.0.0.1:51500 will see it on HR[100]."
        << "\n";
    return app.exec();
}
