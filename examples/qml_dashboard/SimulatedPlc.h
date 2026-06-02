#pragma once

#include <QObject>
#include <QTimer>

class QModbusTcpServer;

// In-process Modbus TCP server standing in for a real PLC, so the dashboard
// has live data to poll without any external hardware or simulator. Holding
// registers HR[0..7] are mutated every tick to mimic moving sensors.
class SimulatedPlc : public QObject {
    Q_OBJECT
public:
    explicit SimulatedPlc(quint16 port, QObject* parent = nullptr);
    ~SimulatedPlc() override;

    bool start();

private:
    void tick();

    quint16           m_port;
    QModbusTcpServer* m_server = nullptr;
    QTimer            m_timer;
    int               m_n = 0;
};
