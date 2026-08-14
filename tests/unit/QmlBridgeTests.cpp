// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <QQmlEngine>

#include <memory>
#include <utility>

#include "core/bus/EventBus.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/qml/DatapointQmlBridge.h"
#include "core/qml/QtDatapoint.h"

TEST_CASE("QML datapoint wrappers remain C++ owned and cached", "[qml][dp]") {
    core::dp::DatapointRegistry registry;
    core::bus::EventBus bus;

    core::dp::DatapointSpec spec;
    spec.id = "motor.speed";
    registry.registerDp(std::make_shared<core::dp::Datapoint>(std::move(spec)));

    core::qml::DatapointQmlBridge bridge(registry, bus);
    auto* const first = bridge.dp(QStringLiteral("motor.speed"));

    REQUIRE(first != nullptr);
    REQUIRE(first->parent() == &bridge);
    REQUIRE(QQmlEngine::objectOwnership(first) == QQmlEngine::CppOwnership);
    REQUIRE(bridge.dp(QStringLiteral("motor.speed")) == first);
    REQUIRE(bridge.dp(QStringLiteral("missing")) == nullptr);
}
