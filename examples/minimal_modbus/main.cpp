// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
// minimal_modbus — Smallest possible Core consumer.
//
// Loads a 3-datapoint TOML, polls a Modbus TCP server, prints every value
// change. Run alongside any Modbus simulator (ModRSsim, ModbusPal, etc.)
// listening on 127.0.0.1:51500.
//
// Build:   ctest target `example_minimal_modbus`
// Run:     example_minimal_modbus path/to/minimal.toml

#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>

#include "core/ICore.h"
#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    QString path = (argc > 1)
        ? QString::fromLocal8Bit(argv[1])
        : QStringLiteral("minimal.toml");

    auto core = core::ICore::create(nullptr);
    auto loaded = core->loadConfig(path);
    if (!loaded.has_value()) {
        for (auto const& e : loaded.error()) {
            out << "[config] " << e.section << "." << e.field
                << " (line " << e.lineNumber << "): " << e.message << "\n";
        }
        return 1;
    }

    // Print every datapoint update.
    auto sub = core->bus().subscribe<core::bus::DpChanged>(
        [&out](core::bus::DpChanged const& e) {
            out << e.timestamp.toString("HH:mm:ss.zzz")
                << "  " << e.id << " = " << e.value.toString() << "\n";
            out.flush();
        });

    core->start();
    out << "Core started. Ctrl-C to stop." << "\n";
    return app.exec();
}
