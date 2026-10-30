#include "pr/dom_abort.h"

#include "doctest/doctest.h"

#include <string>

namespace pr_impl {

namespace {

// What the listeners, algorithms and reason handlers of the signals saw, in the order they ran. Each is named by a
// letter.
struct Log {
    std::string text;
};

// A reason handler that appends `mark` to a log and keeps the last reason that it was given.
struct Keeper {
    Log* log;
    char mark;
    const void* reason = nullptr;
};

void keep(PrDomAbortSignal* signal, Keeper* keeper) {
    prDomAbortSignalSetReasonHandler(signal, [](void* context, PrDomAbortSignal*, const void* reason) {
        Keeper* k = static_cast<Keeper*>(context);
        k->log->text += k->mark;
        k->reason = reason;
    }, keeper);
}

// A listener or algorithm that appends `mark` to a log, and checks that `others` are aborted when it runs.
struct Probe {
    Log* log;
    char mark;
    PrDomAbortSignal* others[4];
    int otherCount;
    bool othersAborted = true;
};

void record(Probe* probe) {
    probe->log->text += probe->mark;
    for (int i = 0; i < probe->otherCount; ++i)
        probe->othersAborted &= prDomAbortSignalAborted(probe->others[i]);
}

void listen(PrDomAbortSignal* signal, Probe* probe) {
    PrDomEventListener callback = {
        [](void* context, PrDomEvent*) { record(static_cast<Probe*>(context)); },
        probe,
        uint64_t(uintptr_t(probe)),
        nullptr,
        nullptr,
    };
    prDomEventTargetAddEventListener(prDomAbortSignalAsEventTarget(signal), "abort", &callback, nullptr);
}

PrDomAbortAlgorithm algorithm(Probe* probe) {
    return PrDomAbortAlgorithm{
        [](void* context, PrDomAbortSignal*) { record(static_cast<Probe*>(context)); },
        probe,
        nullptr,
        nullptr,
    };
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a controller's abort gives its signal the reason, runs its algorithms, then fires abort, once") {
    PrDomAbortController* controller = prDomAbortControllerCreate();
    PrDomAbortSignal* signal = prDomAbortControllerSignal(controller);
    Log log;
    Keeper keeper = {&log, 'k'};
    Probe heard = {&log, 'e', {}, 0};
    Probe ran = {&log, 'a', {}, 0};
    Probe removed = {&log, 'x', {}, 0};
    keep(signal, &keeper);
    listen(signal, &heard);
    PrDomAbortAlgorithm first = algorithm(&ran);
    PrDomAbortAlgorithm second = algorithm(&removed);
    CHECK(prDomAbortSignalAdd(signal, &first) != 0);
    prDomAbortSignalRemove(signal, prDomAbortSignalAdd(signal, &second));
    CHECK(!prDomAbortSignalAborted(signal));

    int reason = 0;
    int later = 0;
    prDomAbortControllerAbort(controller, &reason);
    prDomAbortControllerAbort(controller, &later);
    CHECK(log.text == "kae");
    CHECK(prDomAbortSignalAborted(signal));
    CHECK(keeper.reason == &reason);
    CHECK(prDomAbortSignalAdd(signal, &first) == 0);
    prDomAbortControllerDestroy(controller);
}

TEST_CASE("a dependent signal follows the signals at the root of the signals it was made from") {
    PrDomAbortSignal* root = prDomAbortSignalCreate();
    PrDomAbortSignal* other = prDomAbortSignalCreate();
    PrDomAbortSignal* first = prDomAbortSignalCreateDependent(&root, 1);
    PrDomAbortSignal* pair[] = {first, other};
    PrDomAbortSignal* second = prDomAbortSignalCreateDependent(pair, 2);
    PrDomAbortSignal* third = prDomAbortSignalCreateDependent(&root, 1);

    // Every dependent is aborted before the first listener runs, and the listeners run in the order the dependents were
    // made.
    Log log;
    Probe heardRoot = {&log, 'r', {first, second, third}, 3};
    Probe heardFirst = {&log, '1', {}, 0};
    Probe heardSecond = {&log, '2', {}, 0};
    Probe heardThird = {&log, '3', {}, 0};
    Probe heardOther = {&log, 'o', {}, 0};
    listen(root, &heardRoot);
    listen(first, &heardFirst);
    listen(second, &heardSecond);
    listen(third, &heardThird);
    listen(other, &heardOther);

    // Each signal that the abort reaches is given the reason before any listener runs.
    Keeper keptRoot = {&log, 'R'};
    Keeper keptSecond = {&log, 'S'};
    keep(root, &keptRoot);
    keep(second, &keptSecond);
    int reason = 0;
    prDomAbortSignalSignalAbort(root, &reason);
    CHECK(log.text == "RSr123");
    CHECK(heardRoot.othersAborted);
    CHECK(!prDomAbortSignalAborted(other));
    CHECK(keptRoot.reason == &reason);
    CHECK(keptSecond.reason == &reason);

    // A signal made from an aborted signal is aborted, and the caller gives it the first such signal's reason.
    prDomAbortSignalSignalAbort(other, &reason);
    PrDomAbortSignal* signals[] = {other, root};
    PrDomAbortSignal* aborted = prDomAbortSignalCreateDependent(signals, 2);
    CHECK(prDomAbortSignalAborted(aborted));

    for (PrDomAbortSignal* signal : {root, other, first, second, third, aborted})
        prDomAbortSignalRelease(signal);
}

TEST_CASE("a signal that is freed leaves the signals that it follows and that follow it") {
    PrDomAbortSignal* source = prDomAbortSignalCreate();
    PrDomAbortSignal* dependent = prDomAbortSignalCreateDependent(&source, 1);
    PrDomAbortSignal* chained = prDomAbortSignalCreateDependent(&dependent, 1);
    prDomAbortSignalRelease(dependent);
    prDomAbortSignalRelease(source);
    PrDomAbortSignal* empty = prDomAbortSignalCreateDependent(&chained, 1);
    CHECK(!prDomAbortSignalAborted(empty));
    prDomAbortSignalRelease(empty);
    prDomAbortSignalRelease(chained);
}

TEST_CASE("a listener with a signal is removed when the signal aborts, and not added once it has") {
    PrDomAbortSignal* signal = prDomAbortSignalCreate();
    PrDomEventTarget* target = prDomEventTargetCreate();
    PrDomEventTarget* freed = prDomEventTargetCreate();
    Log log;
    Probe heard = {&log, 'h', {}, 0};
    PrDomEventListener callback = {
        [](void* context, PrDomEvent*) { record(static_cast<Probe*>(context)); },
        &heard,
        uint64_t(uintptr_t(&heard)),
        nullptr,
        nullptr,
    };
    PrDomAddEventListenerOptions options = prDomAddEventListenerOptionsDefault(target, "test");
    options.signal = signal;
    prDomEventTargetAddEventListener(target, "test", &callback, &options);
    prDomEventTargetAddEventListener(freed, "test", &callback, &options);
    // The listener of a freed target leaves its signal.
    prDomEventTargetRelease(freed);

    auto dispatch = [&] {
        PrDomEvent* event = prDomEventCreate("test", nullptr);
        CHECK(prDomEventTargetDispatchEvent(target, event, nullptr) == PrStatus_Ok);
        prDomEventRelease(event);
    };
    dispatch();
    CHECK(log.text == "h");
    prDomAbortSignalSignalAbort(signal, nullptr);
    dispatch();
    prDomEventTargetAddEventListener(target, "test", &callback, &options);
    dispatch();
    CHECK(log.text == "h");

    prDomEventTargetRelease(target);
    prDomAbortSignalRelease(signal);
}
