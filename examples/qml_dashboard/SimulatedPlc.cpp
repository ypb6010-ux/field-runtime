#include "SimulatedPlc.h"

#include <cmath>

#include <QModbusDataUnit>
#include <QModbusTcpServer>
#include <QThread>
#include <QTimer>

SimulatedPlc::SimulatedPlc(quint16 port, QObject* parent)
    : QObject(parent), m_port(port) {}

SimulatedPlc::~SimulatedPlc() {
    if (m_thread) {
        // Tear the server (and its child timer) down ON the worker thread —
        // deleting a QObject from a foreign thread aborts the process.
        if (m_server) {
            QMetaObject::invokeMethod(m_server, [this]() {
                m_server->disconnectDevice();
                delete m_server;        // deletes the child QTimer too
            }, Qt::BlockingQueuedConnection);
            m_server = nullptr;
            m_timer  = nullptr;
        }
        m_thread->quit();
        m_thread->wait();
        delete m_thread;
    }
}

bool SimulatedPlc::start() {
    m_thread = new QThread;
    m_server = new QModbusTcpServer;
    m_timer  = new QTimer(m_server);   // child → moves and dies with the server

    m_server->moveToThread(m_thread);  // m_timer follows as a child
    m_thread->start();

    QMetaObject::invokeMethod(m_server, [this]() {
        m_server->setMap({
            {QModbusDataUnit::HoldingRegisters,
             {QModbusDataUnit::HoldingRegisters, 0, 16}},
        });
        m_server->setServerAddress(1);   // respond to unit id 1 (the client's slave_id)
        m_server->setConnectionParameter(QModbusDevice::NetworkPortParameter, m_port);
        m_server->setConnectionParameter(QModbusDevice::NetworkAddressParameter,
                                         QStringLiteral("127.0.0.1"));
        m_server->connectDevice();

        m_timer->setInterval(500);
        QObject::connect(m_timer, &QTimer::timeout, m_server, [this]() { tick(); });
        m_timer->start();
        tick();
    }, Qt::QueuedConnection);

    return true;
}

void SimulatedPlc::tick() {
    ++m_n;
    auto set = [this](int addr, quint16 v) {
        m_server->setData(QModbusDataUnit::HoldingRegisters, quint16(addr), v);
    };
    double const t = m_n * 0.5;
    set(0, quint16(250 + 80 * std::sin(t / 3.0)));        // temperature  x10 °C
    set(1, quint16(700 + 600 * std::sin(t / 7.0 + 1.0))); // belt speed   rpm
    set(2, quint16(1200 + 300 * std::sin(t / 5.0)));      // pressure     x100 bar
    set(3, quint16((m_n / 6) % 4));                        // run state    enum 0..3
    set(4, quint16(150 + 50 * std::sin(t / 4.0)));        // current      x10 A
}
