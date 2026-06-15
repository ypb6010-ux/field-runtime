// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0

#include "core/base/RegisterTable.h"
#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/codec/Codec.h"
#include "core/codec/CodecRegistry.h"
#include "core/config/ConfigLoader.h"
#include "core/config/ConfigSchema.h"
#include "core/config/ValidationError.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/dp/PortRef.h"
#include "core/dp/ScalarType.h"
#include "core/dp/State.h"
#include "core/dp/Value.h"
#include "core/dp/WordOrder.h"
#include "core/log/DedupFilter.h"
#include "core/log/ILogSink.h"
#include "core/log/LogFilter.h"
#include "core/log/LogTypes.h"
#include "core/log/Sinks.h"
#include "core/module/FunctionalModule.h"
#include "core/plugin/Plugin.h"
#include "core/plugin/PluginRegistry.h"
#include "core/plugin/PortRegistry.h"
#include "core/sched/RequestScheduler.h"
#include "core/sched/SchedulerTypes.h"
#include "core/transport/Transport.h"
#include "core/transport/TransportTypes.h"

void smoke() {
    core::RegisterWords words{1, 2};
    core::dp::Value value = std::int64_t(1);
    core::dp::DatapointSpec spec;
    spec.id = "dp";
    spec.type = core::dp::ScalarType::U16;
    core::dp::PortRef ref;
    ref.transport = "main";
    core::sched::RequestTag tag;
    tag.moduleId = "module";
    core::transport::ReadRequest read;
    read.count = int(words.size());
    core::bus::DpChanged changed{"dp", value, {}};
    core::log::LogRecord log;
    log.category = "core";
    core::config::ConfigSchema schema;
    core::config::ValidationError err;
    err.message = "none";
    (void)ref;
    (void)tag;
    (void)read;
    (void)changed;
    (void)log;
    (void)schema;
    (void)err;
}

int main() {
    smoke();
    return 0;
}
