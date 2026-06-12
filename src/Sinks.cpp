// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/log/Sinks.h"

#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include "core/dp/TimeQt.h"
#include "core/dp/ValueQt.h"

namespace core::log {

namespace {

QString tsString(LogTime ts) {
    return dp::toQDateTime(ts).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

QString categoryTag(std::string const& category) {
    QString c = category.empty() ? QStringLiteral("app")
                                  : QString::fromStdString(category);
    if (!c.isEmpty()) c[0] = c[0].toUpper();
    return QStringLiteral("[Core/%1]").arg(c);
}

QString fieldsToString(std::map<std::string, dp::Value> const& fields) {
    if (fields.empty()) return {};
    QStringList parts;
    parts.reserve(int(fields.size()));
    for (auto const& [key, value] : fields) {
        parts << QStringLiteral("%1=%2").arg(QString::fromStdString(key),
                                             QString::fromStdString(dp::toString(value)));
    }
    return QLatin1Char(' ') + parts.join(QLatin1Char(' '));
}

const char* ansiColour(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:    return "\033[90m";  // bright black
        case LogLevel::Debug:    return "\033[36m";  // cyan
        case LogLevel::Info:     return "\033[0m";   // default
        case LogLevel::Warn:     return "\033[33m";  // yellow
        case LogLevel::Error:    return "\033[31m";  // red
        case LogLevel::Critical: return "\033[1;31m";// bold red
    }
    return "\033[0m";
}

} // namespace

QString formatLine(LogRecord const& rec) {
    QString const source  = QString::fromStdString(rec.source);
    QString const message = QString::fromStdString(rec.message);
    return QStringLiteral("%1 %2 %3 %4%5")
        .arg(tsString(rec.ts),
             QString::fromLatin1(levelName(rec.level)).leftJustified(5),
             categoryTag(rec.category).leftJustified(16),
             source.isEmpty() ? message
                              : QStringLiteral("%1 %2").arg(source, message),
             fieldsToString(rec.fields));
}

QString formatLine(OperationRecord const& rec) {
    return QStringLiteral("%1 %2 %3 action=%4 target=%5 %6->%7 result=%8%9")
        .arg(tsString(rec.ts),
             QStringLiteral("OP   "),
             QStringLiteral("[Core/Operation]"),
             QString::fromStdString(rec.action),
             QString::fromStdString(rec.target),
             QString::fromStdString(dp::toString(rec.oldValue)),
             QString::fromStdString(dp::toString(rec.newValue)),
             QString::fromStdString(rec.result),
             rec.note.empty() ? QString()
                              : QStringLiteral(" note=%1").arg(QString::fromStdString(rec.note)))
        + QStringLiteral(" actor=%1").arg(QString::fromStdString(rec.actor));
}

// ---------------------------------------------------------------------------
// ConsoleSink
// ---------------------------------------------------------------------------
void ConsoleSink::write(LogRecord const& rec) {
    QString line = formatLine(rec);
    if (m_colour) {
        std::fprintf(stderr, "%s%s\033[0m\n",
                     ansiColour(rec.level), line.toUtf8().constData());
    } else {
        std::fprintf(stderr, "%s\n", line.toUtf8().constData());
    }
}

void ConsoleSink::write(OperationRecord const& rec) {
    std::fprintf(stderr, "%s\n", formatLine(rec).toUtf8().constData());
}

// ---------------------------------------------------------------------------
// RollingFileSink
// ---------------------------------------------------------------------------
class RollingFileSink::Impl {
public:
    Impl(QString path, qint64 maxBytes, int maxFiles)
        : path(std::move(path)), maxBytes(maxBytes), maxFiles(maxFiles) {
        QFileInfo info(this->path);
        QDir().mkpath(info.absolutePath());
        open();
    }

    ~Impl() {
        if (file.isOpen()) file.close();
    }

    void open() {
        file.setFileName(path);
        if (file.open(QIODevice::Append | QIODevice::Text)) {
            stream.setDevice(&file);
        }
    }

    void rotateIfNeeded() {
        if (maxBytes <= 0 || file.size() < maxBytes) return;
        stream.flush();
        file.close();
        // path.(maxFiles-1) is dropped; shift the rest up by one.
        QString const oldest = QStringLiteral("%1.%2").arg(path).arg(maxFiles);
        QFile::remove(oldest);
        for (int i = maxFiles - 1; i >= 1; --i) {
            QString const from = QStringLiteral("%1.%2").arg(path).arg(i);
            QString const to   = QStringLiteral("%1.%2").arg(path).arg(i + 1);
            if (QFile::exists(from)) QFile::rename(from, to);
        }
        QFile::rename(path, QStringLiteral("%1.1").arg(path));
        open();
    }

    void writeLine(QString const& line) {
        if (!file.isOpen()) return;
        stream << line << '\n';
        stream.flush();
        rotateIfNeeded();
    }

    QString      path;
    qint64       maxBytes;
    int          maxFiles;
    QFile        file;
    QTextStream  stream;
};

RollingFileSink::RollingFileSink(QString filePath, qint64 maxBytes, int maxFiles)
    : m_impl(std::make_unique<Impl>(std::move(filePath), maxBytes, maxFiles)) {}

RollingFileSink::~RollingFileSink() = default;

void RollingFileSink::write(LogRecord const& rec)       { m_impl->writeLine(formatLine(rec)); }
void RollingFileSink::write(OperationRecord const& rec) { m_impl->writeLine(formatLine(rec)); }
void RollingFileSink::flush()                           { m_impl->stream.flush(); }

} // namespace core::log
