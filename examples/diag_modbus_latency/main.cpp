// Diagnostic probe: is the ~1 s modbus read latency (when reads are issued from
// a non-main thread) inherent to QModbusTcpClient, or an artifact of an
// in-process loopback server?
//
// Modes:
//   --serve --port P            run a Modbus TCP server forever (separate proc)
//   --inproc --port P           in-process server + client A/B in one process
//   --host H --port P           client A/B against an external server
//
// The A/B issues N reads from the MAIN thread, then N from a WORKER thread
// (both via scheduler().submit, exactly like a poll), and prints median latency.

#include <algorithm>
#include <cstdio>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QModbusDataUnit>
#include <QModbusTcpServer>
#include <QThread>
#include <QTimer>

#include "core/sched/SchedulerTypes.h"
#include "core/transport/ModbusTcpClientTransport.h"
#include "core/transport/Transport.h"

using namespace core::transport;

static int median(std::vector<int> v) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

static QModbusTcpServer* startServer(QThread* thread, QString const& host, int port) {
    auto* srv = new QModbusTcpServer;
    srv->moveToThread(thread);
    thread->start();
    QMetaObject::invokeMethod(srv, [srv, host, port]() {
        srv->setMap({{QModbusDataUnit::HoldingRegisters,
                      {QModbusDataUnit::HoldingRegisters, 0, 16}}});
        for (int i = 0; i < 8; ++i)
            srv->setData(QModbusDataUnit::HoldingRegisters, quint16(i), quint16(i + 1));
        srv->setServerAddress(1);
        srv->setConnectionParameter(QModbusDevice::NetworkPortParameter, port);
        srv->setConnectionParameter(QModbusDevice::NetworkAddressParameter, host);
        bool ok = srv->connectDevice();
        std::printf("server connectDevice=%d state=%d err='%s'\n",
                    int(ok), int(srv->state()), qPrintable(srv->errorString()));
        std::fflush(stdout);

        // Mimic SimulatedPlc: a 500 ms timer updating registers on the server
        // thread, to test whether that is what slows worker-thread reads.
        if (qEnvironmentVariableIsSet("PROBE_SERVER_TIMER")) {
            auto* timer = new QTimer(srv);
            static int n = 0;
            QObject::connect(timer, &QTimer::timeout, srv, [srv]() {
                ++n;
                for (int i = 0; i < 8; ++i)
                    srv->setData(QModbusDataUnit::HoldingRegisters,
                                 quint16(i), quint16(n + i));
            });
            timer->start(500);
        }
    }, Qt::BlockingQueuedConnection);
    return srv;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    bool serve = false, inproc = false;
    QString host = QStringLiteral("127.0.0.1");
    int port = 5599;
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLatin1(argv[i]);
        if      (a == "--serve")  serve  = true;
        else if (a == "--inproc") inproc = true;
        else if (a == "--host" && i + 1 < argc) host = QString::fromLatin1(argv[++i]);
        else if (a == "--port" && i + 1 < argc) port = QString::fromLatin1(argv[++i]).toInt();
    }

    if (serve) {
        auto* th = new QThread;
        startServer(th, host, port);
        std::printf("serving Modbus TCP on %s:%d — Ctrl+C to stop\n",
                    qPrintable(host), port);
        std::fflush(stdout);
        return app.exec();
    }

    QThread* srvThread = nullptr;
    if (inproc) {
        srvThread = new QThread;
        startServer(srvThread, host, port);
        QThread::msleep(200);
    }

    static ModbusTcpClientTransport::Config cfg;
    cfg.id = QStringLiteral("probe");
    cfg.host = host;
    cfg.port = quint16(port);
    cfg.slaveId = 1;
    cfg.connectTimeoutMs = 2000;
    cfg.requestTimeoutMs = 1000;
    cfg.reconnectIntervalMs = 0;
    static ModbusTcpClientTransport t(std::move(cfg), nullptr);

    // Run the whole A/B inside the event loop (like a real app), so the main
    // thread is spinning app.exec() throughout — the dashboard does this and
    // polls fast; doing reads in main() with no event loop is not representative.
    QTimer::singleShot(0, &app, [&app, srvThread, host, port, inproc]() {
        if (auto c = t.connect(); !c) {
            std::printf("connect FAILED: %s\n", qPrintable(c.error()));
            app.exit(2);
            return;
        }
        std::printf("connected to %s:%d (inproc=%d)\n", qPrintable(host), port, int(inproc));

        ReadRequest req;
        req.table = QModbusDataUnit::HoldingRegisters;
        req.startAddress = 0;
        req.count = 8;

        auto doRead = [&]() -> std::pair<bool, int> {
            core::sched::RequestTag tag;
            tag.moduleId = QStringLiteral("probe");
            ReadResult r;
            QElapsedTimer et;
            et.start();
            t.scheduler().submit(tag, [&]() { r = t.read(req); });
            return {r.ok, int(et.elapsed())};
        };

        {   // warm up + show why a read fails, if it does
            core::sched::RequestTag tag; tag.moduleId = QStringLiteral("probe");
            ReadResult r;
            t.scheduler().submit(tag, [&]() { r = t.read(req); });
            std::printf("warmup read ok=%d state=%d err='%s' values=%lld\n",
                        int(r.ok), int(t.state()), qPrintable(r.errorMessage),
                        (long long)r.values.size());
            std::fflush(stdout);
        }

        // MAIN-thread reads: issued from this event-loop callback (the main
        // thread), exactly like a QTimer-driven poll on the GUI thread.
        std::vector<int> mainMs;
        int mainOk = 0;
        for (int i = 0; i < 10; ++i) { auto [ok, ms] = doRead(); mainMs.push_back(ms); mainOk += ok; }
        std::printf("MAIN-thread   reads ok=%2d/10  medianMs=%d\n", mainOk, median(mainMs));
        std::fflush(stdout);

        // WORKER-thread reads: a detached thread polls while the main thread
        // keeps spinning app.exec() (representative of a worker-tick poll).
        static std::thread worker;
        worker = std::thread([&app, srvThread]() {
            ReadRequest wreq;
            wreq.table = QModbusDataUnit::HoldingRegisters;
            wreq.startAddress = 0; wreq.count = 8;
            std::vector<int> wMs; int wOk = 0;
            for (int i = 0; i < 10; ++i) {
                core::sched::RequestTag tag; tag.moduleId = QStringLiteral("probe");
                ReadResult r; QElapsedTimer et; et.start();
                t.scheduler().submit(tag, [&]() { r = t.read(wreq); });
                wMs.push_back(int(et.elapsed())); wOk += r.ok;
            }
            std::printf("WORKER-thread reads ok=%2d/10  medianMs=%d\n", wOk, median(wMs));
            std::fflush(stdout);
            QMetaObject::invokeMethod(&app, [&app, srvThread]() {
                t.disconnect();
                if (srvThread) { srvThread->quit(); srvThread->wait(); }
                app.quit();
            }, Qt::QueuedConnection);
        });
        worker.detach();
    });

    return app.exec();
}
