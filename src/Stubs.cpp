// Minimum stubs to make Core link during the scaffold phase. Each subsystem
// gets a real implementation file as Phase 1 progresses; this file shrinks
// every iteration. See doc/design/Core-Greenfield-Spec.md §8 for the order.
//
// Stubs left here either return defaults or throw; calling into a stubbed
// subsystem before its real implementation lands is a programmer error and
// produces a clear runtime message rather than silent misbehaviour.

#include <QObject>
#include <stdexcept>

#include "core/dp/ScalarType.h"
#include "core/dp/WordOrder.h"
#include "core/plugin/PluginRegistry.h"
#include "core/plugin/PortRegistry.h"

namespace {
[[noreturn]] void notImplemented(const char* what) {
    throw std::runtime_error(std::string("core: not yet implemented: ") + what);
}
} // namespace

// ---------------------------------------------------------------------------
// ScalarType helpers
// ---------------------------------------------------------------------------
namespace core::dp {

int registerCountFor(ScalarType type) noexcept {
    switch (type) {
        case ScalarType::Bool:
        case ScalarType::U16:
        case ScalarType::S16:
        case ScalarType::EnumU16: return 1;
        case ScalarType::U32:
        case ScalarType::S32:
        case ScalarType::F32:     return 2;
        case ScalarType::U64:
        case ScalarType::S64:
        case ScalarType::F64:     return 4;
        case ScalarType::String:  return 0;   // variable length
    }
    return 0;
}

bool isMultiRegister(ScalarType type) noexcept {
    return registerCountFor(type) > 1;
}

const char* scalarTypeName(ScalarType type) noexcept {
    switch (type) {
        case ScalarType::Bool:    return "Bool";
        case ScalarType::U16:     return "U16";
        case ScalarType::S16:     return "S16";
        case ScalarType::U32:     return "U32";
        case ScalarType::S32:     return "S32";
        case ScalarType::F32:     return "F32";
        case ScalarType::U64:     return "U64";
        case ScalarType::S64:     return "S64";
        case ScalarType::F64:     return "F64";
        case ScalarType::EnumU16: return "EnumU16";
        case ScalarType::String:  return "String";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// WordOrder helpers
// ---------------------------------------------------------------------------
const char* wordOrderName(WordOrder w) noexcept {
    switch (w) {
        case WordOrder::ABCD: return "ABCD";
        case WordOrder::CDAB: return "CDAB";
        case WordOrder::BADC: return "BADC";
        case WordOrder::DCBA: return "DCBA";
    }
    return "?";
}

BytePermutation permutationFor(WordOrder w, int byteCount) noexcept {
    BytePermutation p{};
    if (byteCount == 4) {
        switch (w) {
            case WordOrder::ABCD: p.order = {0,1,2,3,0,0,0,0}; break;
            case WordOrder::CDAB: p.order = {2,3,0,1,0,0,0,0}; break;
            case WordOrder::BADC: p.order = {1,0,3,2,0,0,0,0}; break;
            case WordOrder::DCBA: p.order = {3,2,1,0,0,0,0,0}; break;
        }
    } else if (byteCount == 8) {
        switch (w) {
            case WordOrder::ABCD: p.order = {0,1,2,3,4,5,6,7}; break;
            case WordOrder::CDAB: p.order = {6,7,4,5,2,3,0,1}; break;
            case WordOrder::BADC: p.order = {1,0,3,2,5,4,7,6}; break;
            case WordOrder::DCBA: p.order = {7,6,5,4,3,2,1,0}; break;
        }
    }
    return p;
}

} // namespace core::dp

// Datapoint / DatapointRegistry         live in Datapoint.cpp / DatapointRegistry.cpp
// EventBus / Subscription               live in EventBus.cpp
// BuiltinScalarCodec / EnumU16Codec     live in BuiltinCodecs.cpp
// CodecRegistry                         lives in CodecRegistry.cpp
// SerialScheduler / makeScheduler       live in SerialScheduler.cpp

// ---------------------------------------------------------------------------
// DatapointQmlBridge — thin pass-through; could be promoted to its own file
// once it grows beyond a single lookup.
// ---------------------------------------------------------------------------
#include "core/qml/DatapointQmlBridge.h"
#include "core/dp/DatapointRegistry.h"
#include "core/dp/Datapoint.h"

namespace core::qml {

DatapointQmlBridge::DatapointQmlBridge(dp::DatapointRegistry& reg, QObject* parent)
    : QObject(parent), m_registry(reg) {}

DatapointQmlBridge::~DatapointQmlBridge() = default;

dp::Datapoint* DatapointQmlBridge::dp(QString const& id) const {
    return m_registry.find(id).get();
}

} // namespace core::qml

// ModuleRegistry lives in ModuleRegistry.cpp.

// PluginRegistry / PortRegistry live in PluginSystem.cpp.
// ConfigLoader lives in ConfigLoader.cpp, ICore in Core.cpp.

// Suppress unused warning for notImplemented when nothing uses it yet.
namespace { [[maybe_unused]] auto* dummy = &notImplemented; }
