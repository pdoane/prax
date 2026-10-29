#include "tools/harness/cpu_counters.h"

#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach_time.h>
#include <unistd.h>

using namespace pr;

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
#endif
    return counters;
}

} // namespace harness
