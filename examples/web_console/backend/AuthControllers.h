// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace wc {
// W7.6 auth + RBAC: seeds default users, registers /auth/login·me·logout, a
// global pre-routing auth gate, and CORS. Token model is an opaque bearer token
// (demo); swap for JWT + argon2 in production.
void registerAuth();
}
