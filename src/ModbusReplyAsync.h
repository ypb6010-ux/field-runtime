// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared non-blocking read/write for QModbusClient-backed transports (Modbus
// TCP client + Modbus RTU client). Both speak QModbusReply, so the async path
// is identical: post the request to the client's thread (QueuedConnection, so
// the caller returns at once) and deliver the result on `finished`. The client
// owns its per-request timeout (setTimeout + retries 0), so the reply always
// finishes and `done` always runs — no caller thread is ever parked.

#include <functional>
#include <utility>

#include <QMetaObject>
#include <QtSerialBus/QModbusClient>
#include <QtSerialBus/QModbusReply>

#include "core/transport/TransportTypes.h"
#include "core/transport/RegisterTableQt.h"

namespace core::transport::detail {

inline void modbusReadAsync(QModbusClient* client, int slaveId,
                            ReadRequest const& req,
                            std::function<void(ReadResult)> done) {
    QMetaObject::invokeMethod(client,
        [client, slaveId, req, done = std::move(done)]() mutable {
            auto* reply = client->sendReadRequest(
                QModbusDataUnit(core::toQModbus(req.table), req.startAddress, req.count), slaveId);
            if (!reply) {
                ReadResult r;
                r.startAddress = req.startAddress;
                r.ok           = false;
                r.errorMessage = client->errorString();
                if (r.errorMessage.isEmpty())
                    r.errorMessage = QStringLiteral("sendReadRequest failed");
                done(std::move(r));
                return;
            }
            auto finish = [reply, req, done = std::move(done)]() {
                ReadResult r;
                r.startAddress = req.startAddress;
                if (reply->error() != QModbusDevice::NoError) {
                    r.ok           = false;
                    r.errorMessage = reply->errorString();
                } else {
                    r.ok     = true;
                    r.values = core::fromQtWords(reply->result().values());
                }
                reply->deleteLater();
                done(std::move(r));
            };
            if (reply->isFinished()) finish();
            else QObject::connect(reply, &QModbusReply::finished, client,
                                  std::move(finish));
        }, Qt::QueuedConnection);
}

inline void modbusWriteAsync(QModbusClient* client, int slaveId,
                             WriteBatch const& batch,
                             std::function<void(WriteResult)> done) {
    if (batch.values.empty()) { done(WriteResult{true, {}}); return; }
    QMetaObject::invokeMethod(client,
        [client, slaveId, batch, done = std::move(done)]() mutable {
            int const valueCount = int(batch.values.size());
            QModbusDataUnit unit(core::toQModbus(batch.table), batch.startAddress, quint16(valueCount));
            for (int i = 0; i < valueCount; ++i)
                unit.setValue(i, batch.values.at(i));
            auto* reply = client->sendWriteRequest(unit, slaveId);
            if (!reply) {
                QString err = client->errorString();
                if (err.isEmpty()) err = QStringLiteral("sendWriteRequest failed");
                done(WriteResult{false, std::move(err)});
                return;
            }
            auto finish = [reply, done = std::move(done)]() {
                WriteResult r;
                if (reply->error() != QModbusDevice::NoError) {
                    r.ok           = false;
                    r.errorMessage = reply->errorString();
                } else {
                    r.ok = true;
                }
                reply->deleteLater();
                done(std::move(r));
            };
            if (reply->isFinished()) finish();
            else QObject::connect(reply, &QModbusReply::finished, client,
                                  std::move(finish));
        }, Qt::QueuedConnection);
}

} // namespace core::transport::detail
