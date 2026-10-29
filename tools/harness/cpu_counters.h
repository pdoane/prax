// CPU counters: the work and energy that the process has spent on its CPUs, as the system counts them.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// Counters for the process since it started. They hold the instructions retired and cycles spent by the process's
// threads, including work that the kernel did on their behalf, the part of each that ran on performance cores, and the
// energy that the CPUs used.
//
// Instruction counts barely change with the clock speed, with the kind of core that ran the work, or with other work on
// the machine. Cycles and energy change with all three.
struct CpuCounters {
    double instructions;
    double cycles;
    double performanceInstructions; // the part of `instructions` retired on performance cores
    double performanceCycles;
    double energy;                  // millijoules
    double cpuMillis;               // time that the threads ran on cores, in user and system mode, summed
};

// Returns the current counters of the process, or zeros if the system does not provide them.
CpuCounters cpuCounters();

} // namespace harness

PR_ASSUME_NONNULL_END
