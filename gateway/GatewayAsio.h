// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#ifdef FIELDRUNTIME_GATEWAY_USE_BOOST_ASIO
#include <boost/asio.hpp>
namespace gateway_asio = boost::asio;
#else
#include <asio.hpp>
namespace gateway_asio = asio;
#endif
