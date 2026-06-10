// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/codec/CodecRegistry.h"

#include <utility>

#include "core/codec/BuiltinCodecs.h"

namespace core::codec {

CodecRegistry::CodecRegistry()  = default;
CodecRegistry::~CodecRegistry() = default;

void CodecRegistry::registerCodec(std::shared_ptr<Codec> codec) {
    if (!codec) return;
    m_codecs.insert_or_assign(codec->id(), std::move(codec));
}

std::shared_ptr<Codec> CodecRegistry::find(QString const& id) const {
    auto it = m_codecs.find(id);
    if (it == m_codecs.end()) return nullptr;
    return it->second;
}

void CodecRegistry::loadBuiltins() {
    using dp::ScalarType;
    constexpr ScalarType kBuiltins[] = {
        ScalarType::Bool,
        ScalarType::U16, ScalarType::S16,
        ScalarType::U32, ScalarType::S32, ScalarType::F32,
        ScalarType::U64, ScalarType::S64, ScalarType::F64,
        ScalarType::EnumU16,
    };
    for (auto type : kBuiltins) {
        registerCodec(std::make_shared<BuiltinScalarCodec>(type));
    }
}

} // namespace core::codec
