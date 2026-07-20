// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
// stats_dashboard — Subscribe to SchedulerStatsEvent + TransportStateChanged for
// every transport and print a one-line snapshot per second. Designed to
// be wired to a real QML / web dashboard in production.

#include <QCoreApplication>
#include <QTextStream>

#include "core/ICore.h"
#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/transport/Transport.h"

namespace {

QString stateChar(core::transport::ConnectionState s) {
    switch (s) {
        case core::transport::ConnectionState::Connected:    return "●";
        case core::transport::ConnectionState::Connecting:   return "○";
        case core::transport::ConnectionState::Disconnected: return "×";
        case core::transport::ConnectionState::Error:        return "!";
    }
    return "?";
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    QString path = (argc > 1)
        ? QString::fromLocal8Bit(argv[1])
        : QStringLiteral("dashboard.toml");

    auto core = core::ICore::create(nullptr);
    auto loaded = core->loadConfig(path);
    if (!loaded.has_value()) {
        for (auto const& e : loaded.error()) {
            out << "[config] " << e.section << "." << e.field
                << ": " << e.message << "\n";
        }
        return 1;
    }

    auto subStats = core->bus().subscribe<core::bus::SchedulerStatsEvent>(
        [&out, &core](core::bus::SchedulerStatsEvent const& s) {
            auto const status = core->transportStatus(s.transportId);
            out << QStringLiteral("[%1] %2  q=%3  inflight=%4  p50=%5ms  "
                                   "p99=%6ms  done=%7  fail=%8  circuit=%9\n")
                       .arg(s.transportId)
                       .arg(stateChar(status.state))
                       .arg(s.stats.queueDepth)
                       .arg(s.stats.inflight)
                       .arg(s.stats.p50LatencyMs)
                       .arg(s.stats.p99LatencyMs)
                       .arg(s.stats.totalCompleted)
                       .arg(s.stats.totalFailed)
                       .arg(int(s.stats.circuitState));
            out.flush();
        });

    auto subEvents = core->bus().subscribe<core::bus::TransportStateChanged>(
        [&out](core::bus::TransportStateChanged const& e) {
            out << "[event] " << e.after.transportId << " "
                << stateChar(e.before.state) << " → "
                << stateChar(e.after.state);
            if (!e.after.errorMessage.isEmpty()) {
                out << " (" << e.after.errorMessage << ")";
            }
            out << "\n";
            out.flush();
        });

    core->start();
    out << "Stats dashboard running. Stats emitted every 1 s." << "\n";
    return app.exec();
}
