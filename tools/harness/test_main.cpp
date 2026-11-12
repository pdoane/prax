// The main() of every module's tests: the crash handler, then doctest's run of the binary's tests, which a watchdog
// times case by case. It takes core's declarations only, so that each binary links the core that it tests, and the
// watchdog is the standard library's, since a configured core may have no threads of its own.

#include "pr/assert.h"

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

using namespace pr;

namespace {

// A web build without threads has no watchdog.
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
constexpr bool kWatchdog = false;
#else
constexpr bool kWatchdog = true;
#endif

// Ends the run once a test case takes longer than PR_TEST_CASE_TIMEOUT seconds, after naming the case on standard
// error, so that a case that hangs is reported as itself rather than as its binary's time running out. A run without
// the variable, or with 0, gives cases as long as they take, as a run under a debugger needs. The time is the whole
// case's, with all of its subcases.
struct Watchdog : doctest::IReporter {
    using Clock = std::chrono::steady_clock;

    std::mutex mutex;
    std::condition_variable changed;
    std::thread thread;
    bool stopping = false;
    double seconds = 0;
    bool timing = false;
    Clock::time_point deadline;
    std::string name;

    explicit Watchdog(const doctest::ContextOptions&) {
        const char* limit = std::getenv("PR_TEST_CASE_TIMEOUT");
        seconds = limit != nullptr ? std::atof(limit) : 0;
        if (kWatchdog && seconds > 0)
            thread = std::thread([this] { watch(); });
    }

    ~Watchdog() override {
        if (!thread.joinable())
            return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        changed.notify_one();
        thread.join();
    }

    void watch() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping) {
            if (!timing) {
                changed.wait(lock);
                continue;
            }
            if (changed.wait_until(lock, deadline) != std::cv_status::timeout || !timing || stopping)
                continue;
            std::fflush(stdout);
            std::fprintf(stderr, "\nTIMEOUT: the test case \"%s\" ran longer than %g seconds (PR_TEST_CASE_TIMEOUT)\n",
                name.c_str(), seconds);
            std::fflush(stderr);
            std::_Exit(3);
        }
    }

    void test_case_start(const doctest::TestCaseData& in) override {
        if (!thread.joinable())
            return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            name = in.m_name;
            std::chrono::duration<double> limit(seconds);
            deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(limit);
            timing = true;
        }
        changed.notify_one();
    }

    void test_case_end(const doctest::CurrentTestCaseStats&) override {
        if (!thread.joinable())
            return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            timing = false;
        }
        changed.notify_one();
    }

    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

REGISTER_LISTENER("watchdog", 1, Watchdog);

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    return doctest::Context(argc, argv).run();
}
