// Minimal scaffold smoke test. Will be replaced by Catch2-driven suites
// during Phase 1; for now its only job is to prove that the library links.

#include <cstdio>
#include <cstring>

#include "core/Version.h"
#include "core/dp/ScalarType.h"
#include "core/dp/WordOrder.h"

int main() {
    const char* v = core::versionString();
    if (!v || std::strlen(v) == 0) {
        std::fprintf(stderr, "core: empty version string\n");
        return 1;
    }
    std::printf("core version: %s\n", v);

    using core::dp::ScalarType;
    if (core::dp::registerCountFor(ScalarType::F32) != 2) return 2;
    if (core::dp::registerCountFor(ScalarType::U16) != 1) return 3;
    if (!core::dp::isMultiRegister(ScalarType::U32))      return 4;

    using core::dp::WordOrder;
    auto p = core::dp::permutationFor(WordOrder::CDAB, 4);
    // CDAB on 0x12345678 => bytes 56 78 12 34, so byte 0 should come from
    // source index 2 (the 0x56), byte 1 from 3 (0x78), 2 from 0, 3 from 1.
    if (p.order[0] != 2 || p.order[1] != 3 ||
        p.order[2] != 0 || p.order[3] != 1) {
        std::fprintf(stderr, "core: CDAB permutation incorrect\n");
        return 5;
    }

    std::printf("smoke test passed\n");
    return 0;
}
