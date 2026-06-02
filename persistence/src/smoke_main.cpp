// Live-DB smoke check for CorePersistence. Connects to Postgres, exercises all
// three streams (telemetry / operation / system), reads them back, prints a
// summary. Run manually against a reachable database.
#include <cstdio>
#include <memory>

#include <QCoreApplication>
#include <QDateTime>
#include <QThread>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/ICore.h"
#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/log/Logger.h"
#include "core/persistence/Persistence.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    auto core = core::ICore::create();

    // A datapoint carrying a persistTag so DpChanged → telemetry.
    core::dp::DatapointSpec spec;
    spec.id         = QStringLiteral("smoke.dp");
    spec.persistTag = QStringLiteral("smoke.tag");
    auto dp = std::make_shared<core::dp::Datapoint>(spec);
    core->datapoints().registerDp(dp);

    core::persist::Config cfg;
    cfg.host     = QStringLiteral("localhost");
    cfg.port     = 5432;
    cfg.user     = QStringLiteral("postgres");
    cfg.password = QStringLiteral("ylkj123");
    cfg.dbname   = QStringLiteral("jmj_core");

    core::persist::Persistence db(*core, cfg);
    if (!db.start()) {
        std::fprintf(stderr, "persistence start FAILED\n");
        return 2;
    }

    QString const now = QDateTime::currentDateTime()
                            .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));

    // system log
    core->logger().logf(core::log::LogLevel::Warn,
        QStringLiteral("transport"), QStringLiteral("PLC1"),
        QStringLiteral("smoke reconnect"));
    // operation log
    core::log::OperationRecord op;
    op.actor = QStringLiteral("ui:user");
    op.action = QStringLiteral("reset");
    op.target = QStringLiteral("belt2");
    op.result = QStringLiteral("ok");
    core->logger().logOperation(op);
    // telemetry via DpChanged
    core->bus().publish(core::bus::DpChanged{
        QStringLiteral("smoke.dp"), QVariant(42),
        QDateTime::currentDateTime()});

    core->logger().flush();
    QThread::msleep(800);   // let the writer thread flush a batch
    db.stop();

    QString const later = QDateTime::currentDateTime().addSecs(60)
                              .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    QString const early = QDateTime::currentDateTime().addSecs(-3600)
                              .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));

    auto tel = db.getTelemetry(QStringLiteral("smoke.tag"), early, later, 0);
    auto ops = db.getOperationLog(early, later, 0);
    auto sys = db.getSystemLog(0, early, later, 0);

    auto count = [](QJsonObject const& o) {
        return int(o.value(QStringLiteral("data")).toArray().size());
    };
    std::printf("telemetry rows in last hour: %d\n",  count(tel));
    std::printf("operation rows in last hour: %d\n",  count(ops));
    std::printf("system    rows in last hour: %d\n",  count(sys));
    std::fflush(stdout);

    bool ok = count(tel) >= 1 && count(ops) >= 1 && count(sys) >= 1;
    std::printf("%s\n", ok ? "SMOKE OK" : "SMOKE INCOMPLETE");
    return ok ? 0 : 1;
}
