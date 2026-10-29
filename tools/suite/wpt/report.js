// Reports each testharness.js result on one line, which tools/suite/wpt/wpt.py reads back.
//
// This runs before the test file, because a completion callback added after a test has finished is never called.

// The runner enforces the file's deadline from outside the page, which also covers stalled scripts.
setup({ output: false, explicit_timeout: true });

function reportResult(test) {
    // A test from a worker or another window has no index until the harness registers it. The completion callback
    // reports it then.
    if (test.index === null)
        return;
    const names = ["PASS", "FAIL", "TIMEOUT", "NOTRUN", "PRECONDITION_FAILED"];
    print("RESULT " + JSON.stringify({index: test.index, status: names[test.status],
                                     name: test.name, message: test.message || ""}));
}

add_result_callback(reportResult);

add_completion_callback(function (tests, status) {
    // Reports NOTRUN tests and any statuses that changed during cleanup. The runner identifies an update by its test's
    // index.
    tests.forEach(reportResult);
    const names = ["OK", "ERROR", "TIMEOUT", "PRECONDITION_FAILED"];
    print("HARNESS " + JSON.stringify({status: names[status.status], message: status.message || ""}));
    // The run does not need to wait for work that the page left running, such as a long animation.
    if (typeof stopRun === "function")
        stopRun();
});
