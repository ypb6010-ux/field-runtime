// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
// Minimum stubs to make Core link during the scaffold phase. Each subsystem
// gets a real implementation file as Phase 1 progresses; this file shrinks
// every iteration. See doc/design/Core-Greenfield-Spec.md §8 for the order.
//
// Stubs left here either return defaults or throw; calling into a stubbed
// subsystem before its real implementation lands is a programmer error and
// produces a clear runtime message rather than silent misbehaviour.

#include <QObject>
#include <stdexcept>

namespace {
[[noreturn]] void notImplemented(const char* what) {
    throw std::runtime_error(std::string("core: not yet implemented: ") + what);
}
} // namespace

// ScalarType / WordOrder helpers (core::dp) live in DpTypeHelpers.cpp — a
// Qt-free source shared by Core and FieldRuntimeBase.
// Datapoint / DatapointRegistry         live in Datapoint.cpp / DatapointRegistry.cpp
// EventBus / Subscription               live in EventBus.cpp
// BuiltinScalarCodec / EnumU16Codec     live in BuiltinCodecs.cpp
// CodecRegistry                         lives in CodecRegistry.cpp
// SerialScheduler / makeScheduler       live in SerialScheduler.cpp

#ifdef CORE_HAS_QML
#include <QQmlEngine>

#include "core/qml/DatapointQmlBridge.h"
#include "core/qml/QtDatapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/dp/Datapoint.h"

namespace core::qml {

DatapointQmlBridge::DatapointQmlBridge(dp::DatapointRegistry& reg,
                                       bus::EventBus& bus, QObject* parent)
    : QObject(parent), m_registry(reg), m_bus(bus) {}

DatapointQmlBridge::~DatapointQmlBridge() = default;

QtDatapoint* DatapointQmlBridge::dp(QString const& id) {
    auto const key = id.toStdString();
    if (auto it = m_cache.find(key); it != m_cache.end()) return it->second;
    auto model = m_registry.find(key);
    if (!model) return nullptr;
    auto* wrapper = new QtDatapoint(std::move(model), m_bus, this);
    QQmlEngine::setObjectOwnership(wrapper, QQmlEngine::CppOwnership);
    m_cache.emplace(key, wrapper);
    return wrapper;
}

} // namespace core::qml
#endif

// ModuleRegistry lives in ModuleRegistry.cpp.

// PluginRegistry / PortRegistry live in PluginSystem.cpp.
// ConfigLoader lives in ConfigLoader.cpp, ICore in Core.cpp.

// Suppress unused warning for notImplemented when nothing uses it yet.
namespace { [[maybe_unused]] auto* dummy = &notImplemented; }
