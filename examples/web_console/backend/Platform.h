// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Must be included before any drogon header in every backend TU: drogon's
// drogon/orm/SqlBinder.h uses htonll/ntohll, which this MSVC SDK does not expose
// in our translation units. Provide them (x64 host is little-endian).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdlib>
inline unsigned long long htonll(unsigned long long v) { return _byteswap_uint64(v); }
inline unsigned long long ntohll(unsigned long long v) { return _byteswap_uint64(v); }
#endif
