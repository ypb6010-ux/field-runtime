#include "SimulatedPlc.h"

#include <cmath>

#include <QModbusTcpServer>
#include <QModbusDataUnit>

SimulatedPlc::SimulatedPlc(quint16 port, QObject* parent)
    : QObject(parent), m_port(port) {}

SimulatedPlc::~SimulatedPlc() {
    if (m_server) m_server->disconnectDevice();
}

bool SimulatedPlc::start() {
    m_server = new QModbusTcpServer(this);
    m_server->setMap({
        {QModbusDataUnit::HoldingRegisters, {QModbusDataUnit::HoldingRegisters, 0, 16}},
    });
    m_server->setConnectionParameter(QModbusDevice::NetworkPortParameter, m_port);
    m_server->setConnectionParameter(QModbusDevice::NetworkAddressParameter,
                                     QStringLiteral("127.0.0.1"));
    if (!m_server->connectDevice()) return false;

    m_timer.setInterval(500);
    QObject::connect(&m_timer, &QTimer::timeout, this, &SimulatedPlc::tick);
    m_timer.start();
    tick();
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
