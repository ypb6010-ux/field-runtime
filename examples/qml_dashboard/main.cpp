// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
// qml_dashboard — a QML showcase for the new Core's logging and persistence
// modules. An in-process simulated PLC feeds live Modbus data; the UI shows
// each datapoint's source (Modbus / MQTT / OPC UA), a live system+operation
// log stream, and (when Postgres is reachable) paginated history queries.

#include <memory>

#include <QDateTime>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QThread>
#include <QTimer>

#include "core/ICore.h"
#include "core/log/Logger.h"
#include "core/transport/Transport.h"

#include "DemoController.h"
#include "SimulatedPlc.h"
#include "UiLogSink.h"

#ifdef DASHBOARD_HAS_PERSISTENCE
#include "core/persistence/Persistence.h"
#endif

#ifndef DASHBOARD_TOML
#define DASHBOARD_TOML "dashboard.toml"
#endif

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    // 1. Simulated PLC so Modbus polling has live data with no real hardware.
    SimulatedPlc plc(5502);
    if (!plc.start()) {
        qWarning("failed to start simulated PLC on 127.0.0.1:5502");
        return 2;
    }

    // 2. Core with the QML context, so the `log` bridge is auto-injected.
    QQmlApplicationEngine engine;
    auto core = core::ICore::create(engine.rootContext());

    QString const toml = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                    : QStringLiteral(DASHBOARD_TOML);
    if (auto r = core->loadConfig(toml); !r.has_value()) {
        for (auto const& e : r.error())
            qWarning("[config] %s.%s: %s", e.section.c_str(),
                     e.field.c_str(), e.message.c_str());
        return 1;
    }

    // 3. Optional persistence (graceful if Postgres is down).
    core::persist::Persistence* dbPtr = nullptr;
#ifdef DASHBOARD_HAS_PERSISTENCE
    core::persist::Config cfg;
    cfg.password = QStringLiteral("ylkj123");   // local dev DB
    auto db = std::make_unique<core::persist::Persistence>(*core, cfg);
    if (db->start()) dbPtr = db.get();
    else qWarning("persistence unavailable — History tab will be empty");
#endif

    // 4. View-model + a custom UI log sink feeding the live log view.
    DemoController controller(*core, dbPtr);
    auto uiSink = std::make_shared<UiLogSink>(&controller);
    core->logger().addSink(uiSink);
    engine.rootContext()->setContextProperty(QStringLiteral("demo"), &controller);

    core->start();

    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 3;

    // Headless self-test: run a few seconds, emit an operation, stop gracefully
    // (flushing logger + persistence), then count what landed and quit.
    if (qEnvironmentVariableIsSet("DASHBOARD_SELFTEST")) {
        QTimer::singleShot(4000, &app, [&]() {
            if (auto* t = core->transport(QStringLiteral("plc1"))) {
                auto s = t->scheduler().stats();
                qInfo("SELFTEST plc1 submitted=%llu completed=%llu p50=%dms p99=%dms",
                      (unsigned long long)s.totalSubmitted,
                      (unsigned long long)s.totalCompleted, s.p50LatencyMs, s.p99LatencyMs);
            }
            controller.emitOperation(QStringLiteral("selftest"), QStringLiteral("line1"));
            core->stop();
            QThread::msleep(700);   // let the persistence writer drain
            QString const beg = QDateTime::currentDateTime().addSecs(-3600)
                                    .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
            QString const end = QDateTime::currentDateTime().addSecs(60)
                                    .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
            auto n = [](QVariantMap const& m) {
                return int(m.value(QStringLiteral("data")).toList().size());
            };
            qInfo("SELFTEST telemetry(plc1.temperature)=%d system=%d operation=%d",
                  n(controller.queryTelemetry(QStringLiteral("plc1.temperature"), beg, end, 0)),
                  n(controller.querySystem(0, beg, end, 0)),
                  n(controller.queryOperation(beg, end, 0)));
            app.quit();
        });
    }
    return app.exec();
}
