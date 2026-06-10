// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>

class QThread;
class QTimer;
class QModbusTcpServer;

// In-process Modbus TCP server standing in for a real PLC, so the dashboard
// has live data to poll without any external hardware.
//
// IMPORTANT: the server and its tick timer live on their OWN thread. The core
// polls synchronously — a poll blocks the calling (GUI) thread until the read
// reply arrives — so the responder must NOT share that thread, or it cannot
// answer while the poll is waiting and every read times out.
class SimulatedPlc : public QObject {
    Q_OBJECT
public:
    explicit SimulatedPlc(quint16 port, QObject* parent = nullptr);
    ~SimulatedPlc() override;

    bool start();

private:
    void tick();   // runs on the worker thread

    quint16           m_port;
    QThread*          m_thread = nullptr;
    QModbusTcpServer* m_server = nullptr;   // lives on m_thread
    QTimer*           m_timer  = nullptr;   // lives on m_thread
    int               m_n = 0;
};
