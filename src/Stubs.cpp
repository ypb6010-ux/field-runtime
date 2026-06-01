// Minimum stubs to make Core link during the scaffold phase. Each subsystem
// gets a real implementation file in Phase 1 onwards (see
// doc/design/Core-Greenfield-Spec.md section 8).
//
// Functions here either throw or return defaults; they exist solely so that
// downstream code (and a smoke test) can link against the library while the
// real implementations are written.

#include <QObject>
#include <stdexcept>

#include "core/ICore.h"
#include "core/codec/CodecRegistry.h"
#include "core/config/ConfigLoader.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/dp/ScalarType.h"
#include "core/dp/WordOrder.h"
#include "core/module/ModuleRegistry.h"
#include "core/plugin/PluginRegistry.h"
#include "core/plugin/PortRegistry.h"
#include "core/qml/DatapointQmlBridge.h"

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
        case ScalarType::F32:    return 2;
        case ScalarType::U64:
        case ScalarType::S64:
        case ScalarType::F64:    return 4;
        case ScalarType::String: return 0;   // variable
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

// ---------------------------------------------------------------------------
// Datapoint — stub
// ---------------------------------------------------------------------------
class Datapoint::Impl {};

Datapoint::Datapoint(QObject* parent)
    : QObject(parent), m_impl(nullptr) {}

QString   Datapoint::id() const                   { return {}; }
QVariant  Datapoint::value() const                { return {}; }
bool      Datapoint::valid() const                { return false; }
QDateTime Datapoint::timestamp() const            { return {}; }
DpState   Datapoint::state() const                { return DpState::Missing; }
QString   Datapoint::stateText() const            { return QStringLiteral("Missing"); }
Kind      Datapoint::kind() const                 { return Kind::Status; }
ScalarType Datapoint::type() const                { return ScalarType::U16; }

std::optional<PortRef> const& Datapoint::source() const {
    static const std::optional<PortRef> none;
    return none;
}
std::optional<PortRef> const& Datapoint::sink() const {
    static const std::optional<PortRef> none;
    return none;
}
QString Datapoint::uiBinding()  const { return {}; }
QString Datapoint::persistTag() const { return {}; }

void Datapoint::setValue(QVariant, QDateTime) { notImplemented("Datapoint::setValue"); }
void Datapoint::setState(DpState)             { notImplemented("Datapoint::setState"); }
void Datapoint::write(QVariant)               { notImplemented("Datapoint::write"); }

// ---------------------------------------------------------------------------
// DatapointRegistry — stub
// ---------------------------------------------------------------------------
DatapointRegistry::DatapointRegistry()  = default;
DatapointRegistry::~DatapointRegistry() = default;

void DatapointRegistry::registerDp(std::shared_ptr<Datapoint>) {}
std::shared_ptr<Datapoint> DatapointRegistry::find(QString const&) const { return nullptr; }
QList<std::shared_ptr<Datapoint>> DatapointRegistry::all() const { return {}; }

} // namespace core::dp

// EventBus & Subscription have real implementations in EventBus.cpp.

// ---------------------------------------------------------------------------
// CodecRegistry — stub
// ---------------------------------------------------------------------------
namespace core::codec {

CodecRegistry::CodecRegistry()  = default;
CodecRegistry::~CodecRegistry() = default;

void CodecRegistry::registerCodec(std::shared_ptr<Codec>) {}
std::shared_ptr<Codec> CodecRegistry::find(QString const&) const { return nullptr; }
void CodecRegistry::loadBuiltins() {}

} // namespace core::codec

// ---------------------------------------------------------------------------
// ModuleRegistry — stub
// ---------------------------------------------------------------------------
namespace core::module {

ModuleRegistry::ModuleRegistry()  = default;
ModuleRegistry::~ModuleRegistry() = default;

void ModuleRegistry::registerModule(std::unique_ptr<FunctionalModule>) {}
FunctionalModule* ModuleRegistry::find(QString const&) const { return nullptr; }
QList<FunctionalModule*> ModuleRegistry::byTransport(QString const&) const { return {}; }
QList<FunctionalModule*> ModuleRegistry::all() const { return {}; }
void ModuleRegistry::startAll()  {}
void ModuleRegistry::stopAll()   {}
void ModuleRegistry::pauseAll()  {}
void ModuleRegistry::resumeAll() {}

} // namespace core::module

// ---------------------------------------------------------------------------
// PluginRegistry — stub
// ---------------------------------------------------------------------------
namespace core::plugin {

class PluginRegistry::Impl {};

PluginRegistry::PluginRegistry()  : m_impl(std::make_unique<Impl>()) {}
PluginRegistry::~PluginRegistry() = default;

bool PluginRegistry::load(QString const&) { return false; }
void PluginRegistry::unloadAll() {}
void PluginRegistry::registerAllPorts(PortRegistry&) {}
QList<Plugin*> PluginRegistry::all() const { return {}; }

// PortRegistry
class PortRegistry::Impl {};

PortRegistry::PortRegistry(dp::DatapointRegistry&, bus::EventBus&) : m_impl(nullptr) {}
PortRegistry::~PortRegistry() = default;

} // namespace core::plugin

// ---------------------------------------------------------------------------
// DatapointQmlBridge — stub
// ---------------------------------------------------------------------------
namespace core::qml {

DatapointQmlBridge::DatapointQmlBridge(dp::DatapointRegistry& reg, QObject* parent)
    : QObject(parent), m_registry(reg) {}

DatapointQmlBridge::~DatapointQmlBridge() = default;

dp::Datapoint* DatapointQmlBridge::dp(QString const& id) const {
    auto sp = m_registry.find(id);
    return sp.get();
}

} // namespace core::qml

// Scheduler factory lives in SerialScheduler.cpp.

// ---------------------------------------------------------------------------
// ConfigLoader — stub
// ---------------------------------------------------------------------------
namespace core::config {

std::expected<ConfigSchema, ValidationErrors>
ConfigLoader::loadFromToml(QString const&) {
    ValidationErrors errs;
    errs.push_back({"meta", "toml", "ConfigLoader not yet implemented", -1});
    return std::unexpected(std::move(errs));
}

std::expected<void, ValidationErrors>
ConfigLoader::validate(ConfigSchema const&) {
    return {};
}

} // namespace core::config

// ---------------------------------------------------------------------------
// ICore — stub
// ---------------------------------------------------------------------------
namespace core {

std::unique_ptr<ICore> ICore::create(QQmlContext*) {
    return nullptr;
}

} // namespace core
