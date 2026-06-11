// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
// Catch2 entry point that boots a QCoreApplication first. Any test that
// touches Qt signal/slot dispatch, QThread event loops, or Modbus classes
// requires this to be alive for the duration of the run.

#include <QCoreApplication>
#include <catch2/catch_session.hpp>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
