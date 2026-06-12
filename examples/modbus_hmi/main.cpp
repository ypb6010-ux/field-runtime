// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
// modbus_hmi — a QML HMI for the new Core that demonstrates, end to end:
//   1. connection-parameter configuration   (host / PLC port / poll period)
//   2. Modbus datapoint configuration        (add / remove points, addr/type/scale)
//   3. live data display                      (decoded value + quality, refreshed)
//   4. protocol-conversion control            (operator-box Modbus server bridged
//      to the PLC; a forwarding gate, a downlink setpoint, and a simulated
//      operator-box write that travels server → bridge → PLC and echoes back).
//
// An in-process SimulatedPlc supplies live Modbus data, so the demo runs with no
// external hardware. Set HMI_SELFTEST=1 (with QT_QPA_PLATFORM=offscreen) to run
// a headless scripted flow that verifies the wiring and prints results.

#include <memory>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTimer>
#include <QVariantMap>

#include "GatewayController.h"
#include "SimulatedPlc.h"
#include "UiLogSink.h"

namespace {
int echoValue(GatewayController const& gw) {
    for (auto const& v : gw.points()) {
        auto m = v.toMap();
        if (m.value(QStringLiteral("id")).toString() == QStringLiteral("setpoint_echo"))
            return m.value(QStringLiteral("value")).toInt();
    }
    return -1;
}
}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    quint16 const plcPort   = 5602;
    quint16 const opboxPort = 5603;

    SimulatedPlc plc(plcPort);
    if (!plc.start()) {
        qWarning("failed to start simulated PLC on 127.0.0.1:%u", plcPort);
        return 2;
    }

    GatewayController gw(plcPort, opboxPort);
    gw.setLogSink(std::make_shared<UiLogSink>(&gw));

    // ── headless self-test: scripted flow, no window ───────────────────
    if (qEnvironmentVariableIsSet("HMI_SELFTEST")) {
        if (!gw.apply()) { qWarning("apply() failed"); return 1; }
        QTimer::singleShot(2200, &app, [&] {
            qInfo("SELFTEST connected=%d points=%lld",
                  gw.connected(), (long long)gw.points().size());
            gw.writeSetpoint(1234);                 // downlink via Core sink
        });
        QTimer::singleShot(3800, &app, [&] {
            qInfo("SELFTEST after writeSetpoint(1234): echo=%d", echoValue(gw));
            gw.setForwarding(true);
            gw.simulateOperatorWrite(4321);         // opbox → bridge → PLC
        });
        QTimer::singleShot(6000, &app, [&] {
            qInfo("SELFTEST after operatorWrite(4321): echo=%d", echoValue(gw));
            qInfo("SELFTEST logs=%lld status=%s", (long long)gw.logs().size(),
                  qPrintable(gw.status()));
            gw.stop();
            app.quit();
        });
        return app.exec();
    }

    // ── normal GUI mode ────────────────────────────────────────────────
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("gw"), &gw);
    gw.apply();   // bring the runtime up with the seeded configuration

    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 3;
    return app.exec();
}
