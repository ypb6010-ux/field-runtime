#pragma once

#include <memory>
#include <QString>

#include "core/core_global.h"
#include "core/codec/Codec.h"

namespace core::codec {

// LuaCodec — a Codec whose decode/encode are implemented by a Lua script, for
// conversions the builtin scalar pipeline can't express (BCD packing, bit-field
// composites, checksum-bearing frames, vendor RTC layouts, …).
//
// The script must evaluate to a table carrying `decode` and `encode`:
//
//     -- data/codec/bcd_datetime.lua
//     return {
//         decode = function(raw, ctx) ... return value end,  -- raw = {r1,r2,..}
//         encode = function(value, ctx) ... return {r1,r2,..} end,
//     }
//
//   * `raw` / the encode return are 1-indexed Lua arrays of register words
//     (integers 0..65535); `ctx` carries { address, count, scale, offset }.
//   * decode may return a number / string / boolean; encode must return an
//     array of register words.
//
// PIMPL keeps sol2 / Lua out of this public header, so consumers of Core need
// neither. Instances own one `sol::state` and are safe to share between
// datapoints — decode/encode are serialized by an internal mutex. When the
// library is built without Lua (CORE_BUILD_LUA=OFF), `fromFile` returns nullptr.
class CORE_EXPORT LuaCodec : public Codec {
public:
    // Load `scriptPath` and bind its decode/encode. Returns nullptr on failure
    // (Lua disabled, file missing, script error, or no decode/encode); when
    // `error` is non-null it receives a human-readable reason.
    static std::shared_ptr<LuaCodec> fromFile(QString const& id,
                                              QString const& scriptPath,
                                              QString*       error = nullptr);

    ~LuaCodec() override;

    QString        id() const override;
    QVariant       decode(QList<quint16> const& raw,
                          dp::PortRef const&     ref) override;
    QList<quint16> encode(QVariant const&        value,
                          dp::PortRef const&     ref) override;

private:
    class Impl;
    explicit LuaCodec(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::codec
