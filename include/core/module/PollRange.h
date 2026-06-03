#pragma once

#include <atomic>
#include <memory>
#include <QString>
#include <QVector>

#include "core/core_global.h"
#include "core/module/FunctionalModule.h"
#include "core/sched/RequestScheduler.h"   // SubmitResult lives here
#include "core/transport/TransportTypes.h"

namespace core::codec    { class Codec; }
namespace core::dp       { class Datapoint; }
namespace core::transport { class Transport; }

namespace core::module {

// PollRange — periodic read of one contiguous register range from a single
// Transport. Each tick submits a single ReadRequest through the transport's
// scheduler; on success, every registered binding decodes its slice of the
// response into the bound Datapoint.
//
// Phase 1 ships the per-tick algorithm via `pollOnce()`. The QTimer-driven
// loop wiring lives in Phase 2 alongside ModuleRegistry::startAll().
class CORE_EXPORT PollRange : public FunctionalModule {
public:
    PollRange(QString                     moduleId,
              transport::Transport&       transport,
              transport::ReadRequest      request,
              int                         periodMs,
              sched::Priority             priority = sched::Priority::Normal);
    ~PollRange() override;

    CORE_DISABLE_COPY_MOVE(PollRange)

    // Wire a datapoint into this poll. `registerOffset` is the index inside
    // the poll's response (i.e. `dp_address - request.startAddress`); the
    // codec then decodes `registerCountFor(dp.type())` words starting there.
    void bind(std::shared_ptr<dp::Datapoint> datapoint,
              std::shared_ptr<codec::Codec>  codec,
              int                             registerOffset);

    int  periodMs()   const noexcept;
    transport::ReadRequest const& request() const noexcept;
    int  bindingCount() const noexcept;

    // Execute exactly one poll cycle synchronously on the calling thread.
    // Returns the scheduler submission result so callers can observe
    // success / cancellation / circuit-open without scraping the datapoint
    // state machine. Retained for tests and any synchronous caller.
    sched::SubmitResult pollOnce();

    // FunctionalModule
    void start()  override;
    void stop()   override;
    void pause()  override;
    void resume() override;
    int  tickPeriodMs() const override { return m_periodMs; }
    // Event-driven, non-blocking: submits one read via the scheduler's async
    // path and applies the result in the completion callback. Coalesces — if a
    // poll is still in flight (e.g. a slow PLC), this tick is skipped rather
    // than queueing another read.
    void driveTick()         override;

private:
    struct Binding {
        std::shared_ptr<dp::Datapoint> dp;
        std::shared_ptr<codec::Codec>  codec;
        int                             offset;
    };

    // Decode a successful read into the bound datapoints, or mark them
    // Error. Shared by the sync (pollOnce) and async (driveTick) paths.
    void applyResult(transport::ReadResult const& result);

    transport::Transport*       m_transport;
    transport::ReadRequest      m_req;
    int                         m_periodMs;
    QVector<Binding>            m_bindings;
    std::atomic<bool>           m_paused{false};   // read on tick thread, set on stop/pause
    std::atomic<bool>           m_inFlight{false};
};

} // namespace core::module
