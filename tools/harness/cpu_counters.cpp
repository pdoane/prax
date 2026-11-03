#include "tools/harness/cpu_counters.h"

#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach_time.h>
#include <unistd.h>

using namespace pr;

#elif defined(_WIN32)
#include <windows.h>
#endif

namespace harness {

CpuCounters cpuCounters() {
    CpuCounters counters{};
#if defined(__APPLE__)
    rusage_info_v6 usage{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V6, reinterpret_cast<rusage_info_t*>(&usage)) == 0) {
        counters.instructions = double(usage.ri_instructions);
        counters.cycles = double(usage.ri_cycles);
        counters.performanceInstructions = double(usage.ri_pinstructions);
        counters.performanceCycles = double(usage.ri_pcycles);
        counters.energy = double(usage.ri_energy_nj) / 1e6;
        // The times are in the units of the absolute clock, which the timebase converts to nanoseconds.
        mach_timebase_info_data_t timebase{};
        mach_timebase_info(&timebase);
        double ticks = double(usage.ri_user_time + usage.ri_system_time);
        counters.cpuMillis = ticks * timebase.numer / timebase.denom / 1e6;
    }
#elif defined(_WIN32)
    // Windows counts the cycles that the process's threads ran for, without a driver, but not the instructions they
    // retired, which need the kernel's performance counters.
    HANDLE process = GetCurrentProcess();
    ULONG64 cycles = 0;
    if (QueryProcessCycleTime(process, &cycles))
        counters.cycles = double(cycles);
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        // The times are in units of 100 nanoseconds.
        auto ticks = [](FILETIME time) { return double((uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime); };
        counters.cpuMillis = (ticks(kernel) + ticks(user)) / 1e4;
    }
#endif
    return counters;
}

} // namespace harness
