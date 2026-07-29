// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "core/core_global.h"
#include "core/config/ConfigSchema.h"
#include "core/config/ValidationError.h"

namespace core::config {

class CORE_EXPORT ConfigLoader {
public:
    // Parse a TOML file and run the full schema validation. On failure
    // returns the full list of validation errors.
    std::expected<ConfigSchema, ValidationErrors>
        loadFromToml(
            std::string const& path,
            std::span<std::string_view const> allowedRootExtensions = {});

    std::expected<void, ValidationErrors>
        validate(ConfigSchema const& schema);
};

} // namespace core::config
