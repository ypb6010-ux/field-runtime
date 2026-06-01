#pragma once

#include <QList>
#include <QString>

#include "core/core_global.h"

namespace core::config {

struct ValidationError {
    QString section;    // e.g. "datapoint[3]"
    QString field;      // e.g. "source.wordOrder"
    QString message;
    int     lineNumber = -1;
};

using ValidationErrors = QList<ValidationError>;

} // namespace core::config
