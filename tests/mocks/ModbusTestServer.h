#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#include <QMetaObject>
#include <QString>
#include <QThread>
#include <QtSerialBus/QModbusDataUnit>
#include <QtSerialBus/QModbusDataUnitMap>
#include <QtSerialBus/QModbusTcpServer>

namespace core::test {

// Header-only QModbusTcpServer fixture for integration tests. Spins the
// server up on its own QThread, listens on 127.0.0.1:<port>, owns a 100-word
// HoldingRegister table by default. RAII: constructor blocks until the
// server is listening; destructor stops it cleanly.
class ModbusTestServer {
public:
    explicit ModbusTestServer(quint16 port = 51502,
                              int     slaveId = 1,
                              int     registerCount = 100)
        : m_port(port)
        , m_slaveId(slaveId)
        , m_registerCount(registerCount)
        , m_thread(new QThread)
        , m_server(new QModbusTcpServer) {

        m_server->moveToThread(m_thread);
        m_thread->start();

        bool ok = false;
        QMetaObject::invokeMethod(m_server, [this, &ok] {
            QModbusDataUnitMap map;
            map.insert(QModbusDataUnit::HoldingRegisters,
                       QModbusDataUnit(QModbusDataUnit::HoldingRegisters,
                                       0, m_registerCount));
            map.insert(QModbusDataUnit::InputRegisters,
                       QModbusDataUnit(QModbusDataUnit::InputRegisters,
                                       0, m_registerCount));
            m_server->setMap(map);
            m_server->setServerAddress(m_slaveId);
            m_server->setConnectionParameter(
                QModbusDevice::NetworkAddressParameter, QStringLiteral("127.0.0.1"));
            m_server->setConnectionParameter(
                QModbusDevice::NetworkPortParameter, m_port);
            ok = m_server->connectDevice();
        }, Qt::BlockingQueuedConnection);

        if (!ok) {
            m_listening.store(false);
            return;
        }
        // Wait briefly until the server reaches ConnectedState.
        auto const deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(2000);
        while (std::chrono::steady_clock::now() < deadline) {
            if (m_server->state() == QModbusDevice::ConnectedState) {
                m_listening.store(true);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        m_listening.store(false);
    }

    ~ModbusTestServer() {
        QMetaObject::invokeMethod(m_server, [this] {
            m_server->disconnectDevice();
        }, Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait();
        delete m_server;
        delete m_thread;
    }

    ModbusTestServer(ModbusTestServer const&)            = delete;
    ModbusTestServer& operator=(ModbusTestServer const&) = delete;

    quint16 port()      const noexcept { return m_port; }
    int     slaveId()   const noexcept { return m_slaveId; }
    bool    listening() const noexcept { return m_listening.load(); }

    void setData(QModbusDataUnit::RegisterType table, int address, quint16 value) {
        QMetaObject::invokeMethod(m_server, [this, table, address, value] {
            m_server->setData(table, address, value);
        }, Qt::BlockingQueuedConnection);
    }

    quint16 getData(QModbusDataUnit::RegisterType table, int address) const {
        quint16 v = 0;
        QMetaObject::invokeMethod(m_server, [this, table, address, &v] {
            m_server->data(table, address, &v);
        }, Qt::BlockingQueuedConnection);
        return v;
    }

private:
    quint16            m_port;
    int                m_slaveId;
    int                m_registerCount;
    QThread*           m_thread;
    QModbusTcpServer*  m_server;
    std::atomic<bool>  m_listening{false};
};

} // namespace core::test
