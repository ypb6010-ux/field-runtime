// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

namespace wc {
// Serves Swagger UI at /api/docs and the selected spec at
// /api/docs/openapi.yaml.
void registerDocs(std::string openapiPath);
}
