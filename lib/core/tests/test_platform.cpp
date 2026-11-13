#include "pr/core_libc.h"
#include "pr/core_mimalloc.h"
#include "pr/mem.h"
#include "pr/random.h"
#include "pr/thread.h"
#include "pr/time.h"

#include "tools/harness/test.h"

extern "C" int prTestCoreCApi(void);

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestCoreCApi() == 0);
}

TEST_CASE("random bytes fill the whole of a request longer than one read of the generator, and two draws differ") {
    uint8_t first[1000] = {};
    uint8_t second[1000] = {};
    prRandomBytes(first, sizeof(first));
    prRandomBytes(second, sizeof(second));
    // Two draws are equal, and a 64-byte run of a draw is all zero, with negligible probability.
    uint8_t zero[64] = {};
    CHECK(!prMemEquals(first, second, sizeof(first)));
    for (size_t at = 0; at + sizeof(zero) <= sizeof(first); at += sizeof(zero))
        CHECK(!prMemEquals(first + at, zero, sizeof(zero)));
    prRandomBytes(nullptr, 0);
}

TEST_CASE("the local time zone's offset from UTC is a whole number of minutes, less than a day, in any season") {
    // 2026-01-15 and 2026-07-15 at noon UTC, one of which is in daylight saving time where the zone has it.
    const PrTimeSystem times[] = {
        prTimeSystemFromUnix(1768478400, 0),
        prTimeSystemFromUnix(1784116800, 0),
        prTimeSystemNow(),
    };
    for (PrTimeSystem time : times) {
        int32_t offset = prTimeLocalOffsetSeconds(time);
        CHECK(offset > -86400);
        CHECK(offset < 86400);
        CHECK(offset % 60 == 0);
    }
}

TEST_CASE("the performance counter and the monotonic clock never run backward, and a difference converts alike") {
    PrTimePerf start = prTimePerfNow();
    double monotonic = prTimeMonotonicMilliseconds();
    prThreadSleep(1);
    PrTimePerf end = prTimePerfNow();
    CHECK(end.ticks > start.ticks);
    CHECK(prTimeMonotonicMilliseconds() > monotonic);
    double measured = prTimePerfMilliseconds(start, end);
    CHECK(double(prTimePerfNanoseconds(start, end)) / 1e6 == doctest::Approx(measured).epsilon(1e-6));
    CHECK(prTimePerfSeconds(start, end) * 1e3 == doctest::Approx(measured).epsilon(1e-9));
    // A long difference, whose ticks times a counter's rate would not fit in 64 bits, converts without overflow.
    PrTimePerf later{start.ticks + 1000000000000000};
    double milliseconds = prTimePerfMilliseconds(start, later);
    CHECK(double(prTimePerfNanoseconds(start, later)) / 1e6 == doctest::Approx(milliseconds).epsilon(1e-9));
}

TEST_CASE("the system time is after the Unix epoch, and its offset from the epoch is a Unix time") {
    PrTimeSystem now = prTimeSystemNow();
    CHECK(now.ticks > kPrTimeSystemUnixEpoch);
    // Any time after 2026-01-01.
    CHECK(prTimeSystemToUnixMilliseconds(now) > 1767225600000.0);
}

TEST_CASE("the mimalloc backend allocates, resizes and frees") {
    uint8_t* bytes = static_cast<uint8_t*>(prCoreMimallocAllocBytes(32));
    REQUIRE(bytes != nullptr);
    prMemFill(bytes, 7, 32);
    CHECK(prCoreMimallocUsableBytes(bytes) >= 32);

    bytes = static_cast<uint8_t*>(prCoreMimallocReallocBytes(bytes, 4096));
    REQUIRE(bytes != nullptr);
    CHECK(bytes[0] == 7);
    CHECK(bytes[31] == 7);
    prCoreMimallocFreeBytes(bytes);

    uint8_t* zeroed = static_cast<uint8_t*>(prCoreMimallocAllocZeroedBytes(64));
    REQUIRE(zeroed != nullptr);
    CHECK(zeroed[0] == 0);
    CHECK(zeroed[63] == 0);
    prCoreMimallocTrimBytes();
    CHECK(zeroed[63] == 0);
    prCoreMimallocFreeBytes(zeroed);
}

TEST_CASE("the C library backend allocates, resizes and frees") {
    uint8_t* bytes = static_cast<uint8_t*>(prCoreLibcAllocBytes(32));
    REQUIRE(bytes != nullptr);
    prMemFill(bytes, 7, 32);
    CHECK(prCoreLibcUsableBytes(bytes) >= 32);

    bytes = static_cast<uint8_t*>(prCoreLibcReallocBytes(bytes, 4096));
    REQUIRE(bytes != nullptr);
    CHECK(bytes[0] == 7);
    CHECK(bytes[31] == 7);
    prCoreLibcTrimBytes();
    CHECK(bytes[31] == 7);
    prCoreLibcFreeBytes(bytes);

    uint8_t* zeroed = static_cast<uint8_t*>(prCoreLibcAllocZeroedBytes(64));
    REQUIRE(zeroed != nullptr);
    CHECK(zeroed[0] == 0);
    CHECK(zeroed[63] == 0);
    prCoreLibcFreeBytes(zeroed);
}
