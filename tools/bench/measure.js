// Measures the cost of one call of a function in a benchmark's script, and defines what the script's loader provides.
//
// A script under tools/bench/scripts runs the same way in every engine. Its loader (tools/bench/shell.html in
// pr_shell, tools/bench/browser.html in a browser) runs this file first, then the script, and supplies these globals:
//
//   __INPUT__   the inputs that the script reads, by name: the text of each file that the benchmark names
//               (tools/bench/registry.py)
//   __ONLY__    {name, runs}: the one case to run `runs` times instead of measuring every case, or undefined
//   __TIMED__   true to measure times for a comparison; `measure` then measures the same way in every engine
//   __CPU__     in a browser, if the runner can count it, a function that returns the milliseconds that the browser's
//               processes have run on cores; pr_shell's counters() reports its own
//   __REPORT__  a function that receives the rows
//
// A measure is either counted or timed. A counted measure runs in pr_shell, which defines `counters()`, and measures a
// call the same way the engine's harnesses do (harnessMeasure). Batches grow until one retires enough instructions
// that the cost of reading the counters is negligible, and the batch's counts divided by its size are the cost of one
// call. Counted measures are quick and steady, and the engine's results are compared on them over time.
//
// A timed measure runs in a browser, or wherever __TIMED__ is set. Batches grow until one runs well past the coarsest
// clock that a browser offers. Then a few more batches of that size run, and the result is their median time. A pause
// of the garbage collector or of the machine affects one batch and not the others, so the median ignores it.

const kMeasureInstructions = 5e6;
const kMeasureMillis = 20;
const kMeasureMaxBatch = 1 << 20;

// The number of batches in a timed measure, and the minimum time each batch runs. The minimum is well past a clock that
// steps by a millisecond, as a browser's clock does if its page is not isolated across origins.
const kTimedSamples = 5;
const kTimedMillis = 25;

// The counters that `counters()` reads. A counted measure reports each of them per call.
const kMeasureCounts = ["instructions", "cycles", "performanceInstructions", "performanceCycles", "energy", "time"];

// The finest clock the engine offers, in milliseconds: pr_shell's counters, or performance.now.
const measureClock = typeof globalThis.counters === "function" ? () => globalThis.counters().time
                                                               : () => performance.now();

// A function that returns the milliseconds that the engine's processes have run on cores, summed over their threads,
// or null if nothing counts them.
const measureCpu = typeof globalThis.counters === "function" ? () => globalThis.counters().cpu
                 : typeof globalThis.__CPU__ === "function" ? globalThis.__CPU__ : null;

// Returns the resolution of `measureClock` in milliseconds: the smallest nonzero difference between two readings.
function clockResolution() {
    let least = Infinity;
    let last = measureClock();
    for (let i = 0; i < 100000 && least > 1e-6; ++i) {
        const now = measureClock();
        if (now > last) {
            least = Math.min(least, now - last);
            last = now;
        }
    }
    return least;
}

// Returns how long `runs` calls of `body` take, in milliseconds.
function elapsed(body, runs) {
    const started = measureClock();
    for (let i = 0; i < runs; ++i)
        body();
    return measureClock() - started;
}

// Returns the cost of one call of `body`, in milliseconds. A first, unmeasured call warms the caches for the rest.
//
// A counted measure returns {time, counts}, where `counts` holds each value that `counters()` reads, per call. A timed
// measure returns {time, range, cores}. `range` holds the time per call in the fastest and the slowest batch. `cores`
// is the number of cores that the engine kept busy during the measure, if the engine counts them: its processes' time
// on cores divided by the elapsed time, which is above 1 if it works on more than one thread.
function measure(body) {
    const counted = typeof globalThis.counters === "function" && !globalThis.__TIMED__;
    if (!counted && measureCpu !== null) {
        const cpu = measureCpu();
        const started = measureClock();
        const result = measureOnce(body, counted);
        const elapsed = measureClock() - started;
        result.cores = elapsed > 0 ? (measureCpu() - cpu) / elapsed : 0;
        return result;
    }
    return measureOnce(body, counted);
}

function measureOnce(body, counted) {
    const read = counted ? globalThis.counters : () => ({time: measureClock()});
    const mark = counted ? kMeasureMillis : kTimedMillis;
    body();
    let batch = 1;
    for (;;) {
        const from = read();
        for (let i = 0; i < batch; ++i)
            body();
        const to = read();
        let reached = (to.time - from.time) / mark;
        if (counted)
            reached = Math.max(reached, (to.instructions - from.instructions) / kMeasureInstructions);
        if (reached >= 1 || batch >= kMeasureMaxBatch) {
            if (!counted)
                return sampled(body, batch, (to.time - from.time) / batch);
            const result = {time: (to.time - from.time) / batch, counts: {}};
            for (const key of kMeasureCounts)
                result.counts[key] = (to[key] - from[key]) / batch;
            return result;
        }
        // The next batch is sized to run a little past the mark, based on how far this batch reached.
        const grow = reached > 0 ? 1.25 / reached : 16;
        batch = Math.min(kMeasureMaxBatch, Math.floor(batch * Math.min(1024, Math.max(2, grow))));
    }
}

// Returns a timed measure from batches of `batch` calls. The first batch, already run, took `first` per call.
function sampled(body, batch, first) {
    const times = [first];
    while (times.length < kTimedSamples)
        times.push(elapsed(body, batch) / batch);
    times.sort((a, b) => a - b);
    return {time: times[times.length >> 1], range: [times[0], times[times.length - 1]]};
}

// Returns the measured fields of a case's row. `measured` maps each field's name to its measure. Each field holds its
// time, and the counts, range and cores of each field are grouped under "counts", "range" and "cores".
function measuredFields(measured) {
    const fields = {};
    for (const [key, m] of Object.entries(measured)) {
        fields[key] = m.time;
        if (m.counts) {
            fields.counts ??= {};
            fields.counts[key] = m.counts;
        }
        if (m.range) {
            fields.range ??= {};
            fields.range[key] = m.range;
        }
        if (m.cores !== undefined) {
            fields.cores ??= {};
            fields.cores[key] = m.cores;
        }
    }
    return fields;
}
