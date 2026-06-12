// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/core_global.h"
#include "core/log/ILogSink.h"

namespace core::log {

// Single-line text rendering shared by the built-in sinks and reusable by
// custom ones. Format follows Core-Greenfield-Spec §6.4:
//   2026-05-29 14:23:45.678 [Core/Sched] PLC1 queue=3 inflight=1
// Qt-free: time via <chrono>/<ctime>, fields via dp::Value.
CORE_EXPORT std::string formatLine(LogRecord const& rec);
CORE_EXPORT std::string formatLine(OperationRecord const& rec);

// Writes to stderr, optionally with ANSI level colouring. Holds only POD so it
// is safe to export without PIMPL.
class CORE_EXPORT ConsoleSink : public ILogSink {
public:
    explicit ConsoleSink(bool colour = true) noexcept : m_colour(colour) {}

    void write(LogRecord const& rec) override;
    void write(OperationRecord const& rec) override;

private:
    bool m_colour;
};

// Appends to a file, rotating to file.1, file.2 … when it exceeds maxBytes,
// keeping at most maxFiles rotated copies.
class CORE_EXPORT RollingFileSink : public ILogSink {
public:
    RollingFileSink(std::string  filePath,
                    std::int64_t maxBytes = 5 * 1024 * 1024,
                    int          maxFiles = 5);
    ~RollingFileSink() override;

    CORE_DISABLE_COPY_MOVE(RollingFileSink)

    void write(LogRecord const& rec) override;
    void write(OperationRecord const& rec) override;
    void flush() override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::log
