#pragma once

#include <array>
#include <QDateTime>
#include <QString>

#include "core/core_global.h"

namespace core::sched {

enum class Priority : int {
    Low      = 0,
    Normal   = 1,
    High     = 2,
    Critical = 3,
};
constexpr int kPriorityCount = 4;

enum class ResultKind {
    Ok,
    TimedOut,
    Cancelled,
    CircuitOpen,
    Error,
};

enum class BackoffKind {
    Linear,
    Exponential,
};

enum class SchedulerKind {
    Serial,
    Credit,
    Priority,
};

enum class CircuitState {
    Closed,
    Open,
    HalfOpen,
};

struct RequestTag {
    QString  moduleId;
    Priority priority       = Priority::Normal;
    int      timeoutMs      = 1000;
    int      maxRetries     = 0;
    int      retryBackoffMs = 100;
    bool     coalesce       = false;
    bool     interruptable  = false;
};

struct SchedulerConfig {
    SchedulerKind kind                   = SchedulerKind::Serial;
    int           defaultTimeoutMs       = 1000;
    int           interRequestGapMs      = 0;
    int           maxQueueDepth          = 256;
    int           maxInflight            = 1;
    int           starvationGuardMs      = 5000;
    bool          fifoWithinLane         = true;
    int           circuitBreakerThreshold = 10;
    int           circuitBreakerOpenMs   = 5000;
    BackoffKind   backoff                = BackoffKind::Exponential;
};

struct ModuleStats {
    quint64    submitted        = 0;
    quint64    completed        = 0;
    quint64    failed           = 0;
    quint64    cancelled        = 0;
    int        lastLatencyMs    = 0;
    int        p50LatencyMs     = 0;
    int        p99LatencyMs     = 0;
    QDateTime  lastSuccessAt;
    QDateTime  lastErrorAt;
    QString    lastErrorMessage;
};

struct SchedulerStats {
    int        queueDepth        = 0;
    int        inflight          = 0;
    int        maxQueueDepth     = 0;
    quint64    totalSubmitted    = 0;
    quint64    totalCompleted    = 0;
    quint64    totalFailed       = 0;
    quint64    totalTimedOut     = 0;
    quint64    totalCancelled    = 0;
    int        p50LatencyMs      = 0;
    int        p99LatencyMs      = 0;
    std::array<int, kPriorityCount> laneQueueDepth{};
    CircuitState circuitState    = CircuitState::Closed;
    int        circuitErrorStreak = 0;
};

} // namespace core::sched
