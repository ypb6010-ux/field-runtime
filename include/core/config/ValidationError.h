// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <vector>

#include "core/core_global.h"

namespace core::config {

struct ValidationError {
    std::string section;    // e.g. "datapoint[3]"
    std::string field;      // e.g. "source.wordOrder"
    std::string message;
    int         lineNumber = -1;
};

using ValidationErrors = std::vector<ValidationError>;

} // namespace core::config
