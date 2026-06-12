// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>

class QThread;
class QTimer;
class QModbusTcpServer;

// In-process Modbus TCP server standing in for a real PLC, so the HMI has live
// data to poll without any external hardware. HR 0..7 are driven with smooth
// synthetic signals each tick; HR 8..31 are left untouched so values written by
// the gateway's sink / bridge land here and can be read straight back (the
// control round-trip the UI demonstrates).
//
// The server and its tick timer live on their OWN thread: the core polls
// synchronously, so the responder must not share the polling (GUI) thread.
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
