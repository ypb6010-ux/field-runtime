// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <map>
#include <memory>
#include <shared_mutex>
#include <QString>

#include "core/core_global.h"

namespace core::codec {

class Codec;

class CORE_EXPORT CodecRegistry {
public:
    CodecRegistry();
    ~CodecRegistry();

    CORE_DISABLE_COPY_MOVE(CodecRegistry)

    void registerCodec(std::shared_ptr<Codec> codec);
    std::shared_ptr<Codec> find(QString const& id) const;

    // Populates the builtin scalar and enum codecs.
    void loadBuiltins();

private:
    mutable std::shared_mutex                  m_mutex;
    std::map<QString, std::shared_ptr<Codec>> m_codecs;
};

} // namespace core::codec
