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

int Command::writeCount() const noexcept { return m_cfg.writes.size(); }

sched::SubmitResult Command::execute() {
    sched::SubmitResult firstFailure{sched::ResultKind::Ok, {}, 0};

    for (auto const& e : m_cfg.writes) {
        transport::WriteBatch batch;
        batch.table        = e.table;
        batch.startAddress = e.address;
        batch.values       = QList<quint16>{e.value};

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

void Command::start()  {}
void Command::stop()   {}
void Command::pause()  {}
void Command::resume() {}

} // namespace core::module
