// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "core/core_global.h"

namespace core::codec {

class Codec;

class CORE_EXPORT CodecRegistry {
public:
    CodecRegistry();
    ~CodecRegistry();

    CORE_DISABLE_COPY_MOVE(CodecRegistry)

    void registerCodec(std::shared_ptr<Codec> codec);
    std::shared_ptr<Codec> find(std::string const& id) const;

    // Populates the builtin scalar and enum codecs.
    void loadBuiltins();

private:
    std::unordered_map<std::string, std::shared_ptr<Codec>> m_codecs;
};

} // namespace core::codec
