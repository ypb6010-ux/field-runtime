// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

namespace wc {
// Registers first-run administrator bootstrap, login throttling, opaque bearer
// sessions, role/permission enforcement, audit capture, and API security
// headers.
void registerAuth();

// Shared by the user-management controller. Passwords use a salted adaptive
// hash; existing legacy MD5 rows are transparently upgraded after login.
std::string makePasswordHash(std::string const& password);
void invalidateSessionsForUser(std::string const& username);
bool isSessionTokenValid(std::string const& token);
}
