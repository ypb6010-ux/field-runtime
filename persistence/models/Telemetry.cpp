// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Telemetry.h"

QX_REGISTER_CPP_USER(Telemetry)

namespace qx {
template <> void register_class(QxClass<Telemetry>& t) {
    t.setName("telemetry");
    t.id(&Telemetry::id, "id");
    t.data(&Telemetry::tag,     "tag")->setNotNull(true);
    t.data(&Telemetry::ts,      "ts")->setNotNull(true);
    t.data(&Telemetry::value,   "value");
    t.data(&Telemetry::quality, "quality");
}
}
