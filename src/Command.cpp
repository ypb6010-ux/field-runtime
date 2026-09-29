// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/module/Command.h"

#include <utility>

#include "core/transport/Transport.h"

namespace core::module {

Command::Command(Config cfg, transport::Transport& transport)
    : m_transport(&transport)
    , m_cfg(std::move(cfg)) {
    m_id          = m_cfg.moduleId;
    m_transportId = transport.id();
    m_priority    = m_cfg.priority;
}

Command::~Command() = default;

int Command::writeCount() const noexcept { return int(m_cfg.writes.size()); }

sched::SubmitResult Command::execute() {
    sched::SubmitResult firstFailure{sched::ResultKind::Ok, {}, 0};

    for (auto const& e : m_cfg.writes) {
        transport::WriteBatch batch;
        batch.table        = e.table;
        batch.startAddress = e.address;
        batch.values       = core::RegisterWords{e.value};

        sched::RequestTag tag;
        tag.moduleId      = m_id;
        tag.priority      = m_priority;
        tag.interruptable = m_cfg.interruptable;

        transport::WriteResult write{};
        auto submission = m_transport->scheduler().submit(tag, [&] {
            write = m_transport->writeBatch(batch);
        });
        if (submission.kind != sched::ResultKind::Ok && firstFailure.kind == sched::ResultKind::Ok) {
            firstFailure = submission;
        } else if (!write.ok && firstFailure.kind == sched::ResultKind::Ok) {
            firstFailure = {sched::ResultKind::Error, write.errorMessage,
                            submission.latencyMs};
        }
    }
    return firstFailure;
}

void Command::executeAsync() {
    for (auto const& e : m_cfg.writes) {
        auto result = std::make_shared<bus::WriteCompleted>();
        result->moduleId = m_id;
        result->transportId = m_transportId;
        result->batch = {e.table, e.address, {e.value}};
        sched::RequestTag tag;
        tag.moduleId = m_id;
        tag.priority = m_priority;
        tag.interruptable = m_cfg.interruptable;
        // The scheduler belongs to one transport. Different command modules
        // targeting the same table/address replace each other's queued value.
        tag.coalesceKey = "register:" + std::to_string(int(e.table))
            + ":" + std::to_string(e.address);
        auto callback = m_cfg.onResult;
        tag.onFinished = [result, callback](sched::ResultKind kind) {
            result->kind = kind;
            if (kind == sched::ResultKind::Cancelled && result->errorMessage.empty()) {
                result->errorMessage = "write cancelled before dispatch";
            }
            if (kind == sched::ResultKind::Error && result->errorMessage.empty()) result->errorMessage = "write failed";
            if (callback) callback(*result);
        };
        auto finish = tag.onFinished;
        auto* transport = m_transport;
        auto submission = transport->scheduler().submitAsync(std::move(tag),
            [transport, result](sched::AsyncDone done) {
                transport->writeAsync(result->batch,
                    [result, done = std::move(done)](transport::WriteResult w) mutable {
                        result->errorMessage = std::move(w.errorMessage);
                        done(w.ok);
                    });
            });
        if (submission.kind != sched::ResultKind::Ok) {
            result->errorMessage = submission.errorMessage;
            finish(submission.kind);
        }
    }
}

void Command::start()  {}
void Command::stop()   {}
void Command::pause()  {}
void Command::resume() {}

} // namespace core::module
