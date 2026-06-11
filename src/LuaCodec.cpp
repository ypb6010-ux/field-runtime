// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/codec/LuaCodec.h"

#include "core/dp/PortRef.h"

#ifdef CORE_HAS_LUA

#include <mutex>
#include <string>

#include <QFileInfo>

// sol2 pulls in the Lua C headers; keep it strictly out of the public header.
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace core::codec {

namespace {

// QVariant -> Lua object, preserving the natural Lua type so scripts get an
// integer / number / boolean / string rather than always a string.
sol::object toLua(sol::state& lua, QVariant const& v) {
    switch (v.typeId()) {
        case QMetaType::Bool:
            return sol::make_object(lua, v.toBool());
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::Long:
        case QMetaType::LongLong:
            return sol::make_object(lua, static_cast<int64_t>(v.toLongLong()));
        case QMetaType::ULong:
        case QMetaType::ULongLong:
            return sol::make_object(lua, static_cast<int64_t>(v.toULongLong()));
        case QMetaType::Float:
        case QMetaType::Double:
            return sol::make_object(lua, v.toDouble());
        case QMetaType::QString:
            return sol::make_object(lua, v.toString().toStdString());
        default:
            return sol::make_object(lua, v.toString().toStdString());
    }
}

// Lua return value -> QVariant. Integers stay integral (Lua 5.4 distinguishes
// them) so a decoded register count / enum doesn't become 3.0.
QVariant fromLua(sol::object const& o) {
    switch (o.get_type()) {
        case sol::type::boolean:
            return o.as<bool>();
        case sol::type::number:
            if (o.is<int64_t>())
                return QVariant::fromValue<qlonglong>(o.as<int64_t>());
            return o.as<double>();
        case sol::type::string:
            return QString::fromStdString(o.as<std::string>());
        default:
            return {};
    }
}

} // namespace

class LuaCodec::Impl {
public:
    QString                  id;
    QString                  arg;   // opaque selector → ctx.arg
    sol::state               lua;
    sol::protected_function  decodeFn;
    sol::protected_function  encodeFn;
    std::mutex               mtx;   // serializes sol::state across datapoints

    sol::table makeCtx(int regCount, dp::PortRef const& ref) {
        sol::table ctx = lua.create_table();
        ctx["address"] = ref.address;
        ctx["count"]   = regCount;
        ctx["scale"]   = ref.scale;
        ctx["offset"]  = ref.offset;
        ctx["arg"]     = arg.toStdString();
        return ctx;
    }
};

LuaCodec::LuaCodec(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
LuaCodec::~LuaCodec() = default;

QString LuaCodec::id() const { return m_impl->id; }

std::shared_ptr<LuaCodec> LuaCodec::fromFile(QString const& id,
                                            QString const& scriptPath,
                                            QString const& arg,
                                            QString*       error) {
    auto fail = [&](QString const& msg) -> std::shared_ptr<LuaCodec> {
        if (error) *error = msg;
        return nullptr;
    };
    if (!QFileInfo::exists(scriptPath))
        return fail(QStringLiteral("lua script not found: %1").arg(scriptPath));

    auto impl = std::make_unique<Impl>();
    impl->id  = id;
    impl->arg = arg;
    impl->lua.open_libraries(sol::lib::base, sol::lib::math,
                             sol::lib::string, sol::lib::table);
    // sol::lib::base 自带 dofile/loadfile/load/loadstring —— 即便不开 io/os/package,
    // 这些也能读取并执行任意 Lua 文件 / 动态加载代码,会突破"无文件/外部访问"的沙箱承诺。
    // codec 只做纯数据变换,显式移除它们,让沙箱与文档一致。
    for (char const* g : {"dofile", "loadfile", "load", "loadstring"})
        impl->lua[g] = sol::nil;

    sol::protected_function_result res =
        impl->lua.safe_script_file(scriptPath.toStdString(),
                                   sol::script_pass_on_error);
    if (!res.valid()) {
        sol::error e = res;
        return fail(QStringLiteral("lua load error in %1: %2")
                        .arg(scriptPath, QString::fromStdString(e.what())));
    }

    // Prefer the `return { decode=, encode= }` form; fall back to globals.
    sol::object ret = res;
    sol::protected_function dec, enc;
    if (ret.get_type() == sol::type::table) {
        sol::table t = ret;
        dec = t["decode"];
        enc = t["encode"];
    }
    if (!dec.valid()) dec = impl->lua["decode"];
    if (!enc.valid()) enc = impl->lua["encode"];
    if (!dec.valid() || !enc.valid())
        return fail(QStringLiteral("lua script %1 must provide decode/encode")
                        .arg(scriptPath));

    impl->decodeFn = std::move(dec);
    impl->encodeFn = std::move(enc);
    return std::shared_ptr<LuaCodec>(new LuaCodec(std::move(impl)));
}

QVariant LuaCodec::decode(core::RegisterWords const& raw, dp::PortRef const& ref) {
    std::lock_guard lk(m_impl->mtx);
    sol::table luaRaw = m_impl->lua.create_table(int(raw.size()), 0);
    for (int i = 0; i < raw.size(); ++i)
        luaRaw[i + 1] = static_cast<int64_t>(raw.at(i));

    sol::protected_function_result r =
        m_impl->decodeFn(luaRaw, m_impl->makeCtx(int(raw.size()), ref));
    if (!r.valid()) return {};
    sol::object o = r;
    return fromLua(o);
}

core::RegisterWords LuaCodec::encode(QVariant const& value, dp::PortRef const& ref) {
    std::lock_guard lk(m_impl->mtx);
    sol::object luaVal = toLua(m_impl->lua, value);
    sol::protected_function_result r =
        m_impl->encodeFn(luaVal, m_impl->makeCtx(0, ref));
    if (!r.valid()) return {};
    sol::object o = r;
    if (o.get_type() != sol::type::table) return {};

    sol::table t = o;
    core::RegisterWords out;
    for (std::size_t i = 1;; ++i) {
        sol::object e = t[i];
        // Stop at the first hole / non-numeric entry; go through double so a
        // float value can't throw the way as<integer>() would under safeties.
        if (e.get_type() != sol::type::number) break;
        out.push_back(quint16(quint32(e.as<double>()) & 0xFFFFu));
    }
    return out;
}

} // namespace core::codec

#else  // !CORE_HAS_LUA

namespace core::codec {

class LuaCodec::Impl { public: QString id; };

LuaCodec::LuaCodec(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
LuaCodec::~LuaCodec() = default;

QString LuaCodec::id() const { return m_impl ? m_impl->id : QString(); }

std::shared_ptr<LuaCodec> LuaCodec::fromFile(QString const&, QString const&,
                                            QString const&, QString* error) {
    if (error)
        *error = QStringLiteral("Lua codec disabled at build time "
                                "(CORE_BUILD_LUA=OFF)");
    return nullptr;
}

QVariant       LuaCodec::decode(core::RegisterWords const&, dp::PortRef const&) { return {}; }
core::RegisterWords LuaCodec::encode(QVariant const&, dp::PortRef const&)       { return {}; }

} // namespace core::codec

#endif // CORE_HAS_LUA
