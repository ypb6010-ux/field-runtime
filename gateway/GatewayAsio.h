// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#ifdef FIELDRUNTIME_GATEWAY_USE_BOOST_ASIO
#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
namespace gateway_asio = boost::asio;
using gateway_error_code = boost::system::error_code;
#else
#include <asio.hpp>
namespace gateway_asio = asio;
using gateway_error_code = asio::error_code;
#endif
