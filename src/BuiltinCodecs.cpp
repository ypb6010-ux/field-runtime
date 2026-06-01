#include "core/codec/BuiltinCodecs.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include "core/dp/PortRef.h"
#include "core/dp/WordOrder.h"

namespace core::codec {

namespace {

// Pack two-register-wide network bytes from QList<quint16>. Each Modbus
// register is big-endian on the wire; we record the network-order bytes in
// the order they would appear over TCP (i.e. position 0 = high byte of the
// first register), then apply the WordOrder permutation.
quint64 unpackInt(QList<quint16> const& raw,
                  int                   regCount,
                  dp::WordOrder         wordOrder) {
    if (regCount == 1) {
        return raw.value(0);
    }
    int const               byteCount = regCount * 2;
    std::array<quint8, 8>   network{};
    for (int i = 0; i < regCount; ++i) {
        quint16 const r = raw.value(i);
        network[2 * i]     = quint8(r >> 8);
        network[2 * i + 1] = quint8(r & 0xFF);
    }
    auto const perm = dp::permutationFor(wordOrder, byteCount);
    quint64    result = 0;
    for (int i = 0; i < byteCount; ++i) {
        result = (result << 8) | network[perm.order[i]];
    }
    return result;
}

QList<quint16> packInt(quint64       value,
                       int           regCount,
                       dp::WordOrder wordOrder) {
    if (regCount == 1) {
        return {quint16(value & 0xFFFFu)};
    }
    int const               byteCount = regCount * 2;
    std::array<quint8, 8>   result{};
    for (int i = 0; i < byteCount; ++i) {
        result[i] = quint8(value >> (8 * (byteCount - 1 - i)));
    }
    // Inverse permutation: network[perm[i]] = result[i]
    auto const             perm = dp::permutationFor(wordOrder, byteCount);
    std::array<quint8, 8>  network{};
    for (int i = 0; i < byteCount; ++i) {
        network[perm.order[i]] = result[i];
    }
    QList<quint16> out;
    out.reserve(regCount);
    for (int i = 0; i < regCount; ++i) {
        quint16 r = quint16(network[2 * i]) << 8;
        r |= quint16(network[2 * i + 1]);
        out.append(r);
    }
    return out;
}

bool hasLinearTransform(dp::PortRef const& ref) noexcept {
    return ref.scale != 1.0 || ref.offset != 0.0;
}

} // namespace

BuiltinScalarCodec::BuiltinScalarCodec(dp::ScalarType type) : m_type(type) {}

QString BuiltinScalarCodec::id() const { return idFor(m_type); }

QString BuiltinScalarCodec::idFor(dp::ScalarType type) {
    return QStringLiteral("builtin.") + QString::fromUtf8(dp::scalarTypeName(type)).toLower();
}

QVariant BuiltinScalarCodec::decode(QList<quint16> const& raw,
                                     dp::PortRef const&     ref) {
    using dp::ScalarType;
    int const rc = dp::registerCountFor(m_type);
    if (rc > 0 && raw.size() < rc) {
        return {};
    }

    if (m_type == ScalarType::Bool) {
        int bit = ref.bit.value_or(0);
        return bool(((raw.value(0) >> bit) & 1u) != 0);
    }

    if (m_type == ScalarType::F32) {
        quint64 const bits32 = unpackInt(raw, rc, ref.wordOrder);
        quint32 const b      = quint32(bits32);
        float         f      = 0.0f;
        std::memcpy(&f, &b, sizeof(f));
        double        v      = double(f);
        if (hasLinearTransform(ref)) {
            v = v * ref.scale + ref.offset;
        }
        return v;
    }
    if (m_type == ScalarType::F64) {
        quint64 const bits = unpackInt(raw, rc, ref.wordOrder);
        double        v    = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        if (hasLinearTransform(ref)) {
            v = v * ref.scale + ref.offset;
        }
        return v;
    }

    quint64 const concat = unpackInt(raw, rc, ref.wordOrder);
    quint64 const masked = (concat >> ref.shift) & ref.mask;

    auto integerVariant = [&](auto signedT, auto unsignedT) -> QVariant {
        using S = decltype(signedT);
        using U = decltype(unsignedT);
        (void)signedT; (void)unsignedT;
        if (m_type == ScalarType::S16 || m_type == ScalarType::S32 ||
            m_type == ScalarType::S64) {
            S v = S(U(masked));
            if (hasLinearTransform(ref)) {
                return double(v) * ref.scale + ref.offset;
            }
            return QVariant::fromValue(v);
        }
        U v = U(masked);
        if (hasLinearTransform(ref)) {
            return double(v) * ref.scale + ref.offset;
        }
        return QVariant::fromValue(v);
    };

    switch (m_type) {
        case ScalarType::U16:     return integerVariant(qint16(0),  quint16(0));
        case ScalarType::S16:     return integerVariant(qint16(0),  quint16(0));
        case ScalarType::U32:     return integerVariant(qint32(0),  quint32(0));
        case ScalarType::S32:     return integerVariant(qint32(0),  quint32(0));
        case ScalarType::U64:     return integerVariant(qint64(0),  quint64(0));
        case ScalarType::S64:     return integerVariant(qint64(0),  quint64(0));
        case ScalarType::EnumU16: return quint16(masked);   // wrapping codec maps it
        default:                  return {};
    }
}

QList<quint16> BuiltinScalarCodec::encode(QVariant const&    value,
                                            dp::PortRef const& ref) {
    using dp::ScalarType;
    int const rc = dp::registerCountFor(m_type);
    if (rc <= 0) return {};

    if (m_type == ScalarType::Bool) {
        QList<quint16> out(rc, 0);
        if (value.toBool()) {
            int bit = ref.bit.value_or(0);
            out[0]  = quint16(1u << bit);
        }
        return out;
    }

    if (m_type == ScalarType::F32) {
        double  v    = value.toDouble();
        if (hasLinearTransform(ref)) v = (v - ref.offset) / ref.scale;
        float   f    = float(v);
        quint32 bits = 0;
        std::memcpy(&bits, &f, sizeof(f));
        return packInt(bits, rc, ref.wordOrder);
    }
    if (m_type == ScalarType::F64) {
        double  v    = value.toDouble();
        if (hasLinearTransform(ref)) v = (v - ref.offset) / ref.scale;
        quint64 bits = 0;
        std::memcpy(&bits, &v, sizeof(v));
        return packInt(bits, rc, ref.wordOrder);
    }

    // Stay in 64-bit integer space when no linear transform is configured —
    // a double round-trip would lose precision above 2^53.
    qint64 raw = 0;
    if (hasLinearTransform(ref)) {
        double dval = (value.toDouble() - ref.offset) / ref.scale;
        raw         = qint64(std::llround(dval));
    } else if (m_type == ScalarType::U64) {
        raw = qint64(value.toULongLong());
    } else {
        raw = value.toLongLong();
    }
    quint64 const masked  = quint64(raw) & ref.mask;
    quint64 const shifted = masked << ref.shift;
    return packInt(shifted, rc, ref.wordOrder);
}

// ---------------------------------------------------------------------------
// EnumU16Codec
// ---------------------------------------------------------------------------

EnumU16Codec::EnumU16Codec(QString                              id,
                            std::unordered_map<quint16, QString> map)
    : m_id(std::move(id))
    , m_forward(std::move(map)) {
    m_reverse.reserve(m_forward.size());
    for (auto const& [raw, name] : m_forward) {
        m_reverse.emplace(name, raw);
    }
}

QString EnumU16Codec::id() const { return m_id; }

QVariant EnumU16Codec::decode(QList<quint16> const& raw,
                               dp::PortRef const&     ref) {
    if (raw.isEmpty()) return {};
    quint16 const masked = quint16((raw.value(0) >> ref.shift) & quint16(ref.mask));
    auto it = m_forward.find(masked);
    if (it == m_forward.end()) {
        return QStringLiteral("Unknown(%1)").arg(masked);
    }
    return it->second;
}

QList<quint16> EnumU16Codec::encode(QVariant const&    value,
                                     dp::PortRef const& ref) {
    QString const name = value.toString();
    quint16       raw  = 0;
    if (auto it = m_reverse.find(name); it != m_reverse.end()) {
        raw = it->second;
    } else {
        raw = quint16(value.toUInt());
    }
    quint16 shifted = quint16((raw & quint16(ref.mask)) << ref.shift);
    return {shifted};
}

} // namespace core::codec
