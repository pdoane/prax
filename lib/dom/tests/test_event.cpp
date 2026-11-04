#include "dom/src/dom_node.h"

#include "tools/harness/test.h"

#include <utility>
#include <vector>

namespace pr_impl {

namespace {

struct ListenerState {
    int references = 0;
    int calls = 0;
    PrDomEventTarget* currentTarget = nullptr;
};

void invoke(void* context, PrDomEvent* event) {
    ListenerState* state = static_cast<ListenerState*>(context);
    ++state->calls;
    state->currentTarget = prDomEventCurrentTarget(event);
}

void retain(void* context) { ++static_cast<ListenerState*>(context)->references; }
void release(void* context) { --static_cast<ListenerState*>(context)->references; }

PrDomEventListener listener(ListenerState* state) {
    return PrDomEventListener{invoke, state, uint64_t(uintptr_t(state)), retain, release};
}

PrDomEventTarget* targetOf(PrDomNode* node) {
    return node;
}

// The phases and current targets that the listeners saw, in call order.
struct Trace {
    std::vector<std::pair<PrDomEventPhase, PrDomEventTarget*>> calls;
    bool stop = false;
    bool cancel = false;
};

void trace(void* context, PrDomEvent* event) {
    Trace* state = static_cast<Trace*>(context);
    state->calls.push_back({prDomEventPhase(event), prDomEventCurrentTarget(event)});
    if (state->stop)
        prDomEventStopPropagation(event);
    if (state->cancel)
        prDomEventPreventDefault(event);
}

void addTrace(PrDomNode* node, Trace* state, bool capture, uint64_t identity) {
    PrDomEventListener callback{trace, state, identity, nullptr, nullptr};
    PrDomAddEventListenerOptions options = prDomAddEventListenerOptionsDefault(targetOf(node), "test");
    options.capture = capture;
    prDomEventTargetAddEventListener(targetOf(node), "test", &callback, &options);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a target holds no listener list until a listener is added") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomNode* node = domDocumentFragmentCreate(document);
    ListenerState state;
    PrDomEventListener callback = listener(&state);
    PrDomEvent* event = prDomEventCreate("test", nullptr);
    CHECK(document->listeners == nullptr);
    CHECK(node->listeners == nullptr);
    prDomEventTargetRemoveEventListener(targetOf(node), "test", &callback, nullptr);
    prDomEventTargetDispatchEvent(targetOf(node), event, nullptr);
    CHECK(node->listeners == nullptr);
    CHECK(state.calls == 0);
    prDomEventTargetAddEventListener(targetOf(node), "test", &callback, nullptr);
    CHECK(node->listeners != nullptr);
    CHECK(document->listeners == nullptr);
    CHECK(state.references == 1);
    prDomEventTargetDispatchEvent(targetOf(document), event, nullptr);
    CHECK(state.calls == 0);
    CHECK(document->listeners == nullptr);
    prDomEventRelease(event);
    domNodeRelease(document);
    CHECK(state.references == 0);
}

TEST_CASE("a listener added again with the same callback and capture is added once") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomEventTarget* target = targetOf(document);
    ListenerState first, second;
    PrDomEventListener a = listener(&first), b = listener(&second);
    PrDomAddEventListenerOptions options = prDomAddEventListenerOptionsDefault(target, "test");
    prDomEventTargetAddEventListener(target, "test", &a, nullptr);
    prDomEventTargetAddEventListener(target, "test", &a, nullptr);
    prDomEventTargetAddEventListener(target, "test", &b, nullptr);
    CHECK(first.references == 1);
    CHECK(second.references == 1);
    options.capture = true;
    prDomEventTargetAddEventListener(target, "test", &a, &options);
    CHECK(first.references == 2);
    PrDomEvent* event = prDomEventCreate("test", nullptr);
    bool dispatched = false;
    CHECK(prDomEventTargetDispatchEvent(target, event, &dispatched) == PrStatus_Ok);
    CHECK(dispatched);
    CHECK(first.calls == 2);
    CHECK(second.calls == 1);
    CHECK(first.currentTarget == target);
    CHECK(prDomEventCurrentTarget(event) == nullptr);
    prDomEventTargetRemoveEventListener(target, "test", &a, nullptr);
    CHECK(first.references == 1);
    prDomEventRelease(event);
    domNodeRelease(document);
    CHECK(first.references == 0);
    CHECK(second.references == 0);
    prDomEventRelease(nullptr);
}

TEST_CASE("two listeners with one identity are one listener, whatever context they carry") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomEventTarget* target = targetOf(document);
    ListenerState first, second;
    PrDomEventListener a = listener(&first), b = listener(&second);
    a.identity = 7;
    b.identity = 7;
    prDomEventTargetAddEventListener(target, "test", &a, nullptr);
    prDomEventTargetAddEventListener(target, "test", &b, nullptr);
    CHECK(first.references == 1);
    CHECK(second.references == 0);
    PrDomEvent* event = prDomEventCreate("test", nullptr);
    prDomEventTargetDispatchEvent(target, event, nullptr);
    CHECK(first.calls == 1);
    CHECK(second.calls == 0);
    prDomEventTargetRemoveEventListener(target, "test", &b, nullptr);
    CHECK(first.references == 0);

    b = a;
    b.identity = 8;
    prDomEventTargetAddEventListener(target, "test", &a, nullptr);
    prDomEventTargetAddEventListener(target, "test", &b, nullptr);
    CHECK(first.references == 2);
    prDomEventTargetDispatchEvent(target, event, nullptr);
    CHECK(first.calls == 3);
    prDomEventRelease(event);
    domNodeRelease(document);
    CHECK(first.references == 0);
}

TEST_CASE("an adopted node keeps its listeners, and a destroyed one releases them") {
    PrDomDocument* source = domDocumentCreate(DomDocumentKind_Html);
    PrDomDocument* destination = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* root = domElementCreate(source, PrDomNamespace_Html, PrDomTag_Template, "");
    PrDomNode* child = domDocumentFragmentCreate(source);
    domAttach(root, child, nullptr);
    domSetAttribute(root, PrDomNamespace_None, "id", "test");
    PrDomNode* targets[] = {root, child, root->firstAttribute, domElementContents(root)};
    ListenerState state;
    PrDomEventListener callback = listener(&state);
    for (PrDomNode* node : targets)
        prDomEventTargetAddEventListener(targetOf(node), "test", &callback, nullptr);
    CHECK(state.references == 4);
    domAdopt(destination, root);
    domNodeRelease(source);
    CHECK(state.references == 4);
    PrDomEvent* event = prDomEventCreate("test", nullptr);
    for (PrDomNode* node : targets)
        prDomEventTargetDispatchEvent(targetOf(node), event, nullptr);
    CHECK(state.calls == 4);
    domDetach(child);
    domDropTree(child);
    CHECK(state.references == 3);
    // The event holds its last target, which keeps the document alive.
    domNodeRelease(destination);
    CHECK(state.references == 3);
    prDomEventRelease(event);
    CHECK(state.references == 0);
}

TEST_CASE("a once listener is removed before it is called") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    ListenerState state;
    PrDomEventListener callback = listener(&state);
    PrDomAddEventListenerOptions options = prDomAddEventListenerOptionsDefault(targetOf(document), "test");
    options.once = true;
    prDomEventTargetAddEventListener(targetOf(document), "test", &callback, &options);
    PrDomEvent* event = prDomEventCreate("test", nullptr);
    prDomEventTargetDispatchEvent(targetOf(document), event, nullptr);
    CHECK(state.calls == 1);
    CHECK(state.references == 0);
    prDomEventTargetDispatchEvent(targetOf(document), event, nullptr);
    CHECK(state.calls == 1);
    domNodeRelease(document);
    prDomEventRelease(event);
}

TEST_CASE("a target made on its own keeps its own listeners") {
    PrDomEventTarget* first = prDomEventTargetCreate();
    PrDomEventTarget* second = prDomEventTargetCreate();
    ListenerState state;
    PrDomEventListener callback = listener(&state);
    prDomEventTargetAddEventListener(first, "test", &callback, nullptr);
    prDomEventTargetAddEventListener(second, "test", &callback, nullptr);
    CHECK(state.references == 2);
    PrDomEvent* event = prDomEventCreate("test", nullptr);
    prDomEventTargetDispatchEvent(first, event, nullptr);
    CHECK(state.calls == 1);
    CHECK(state.currentTarget == first);
    // The event holds its target until another dispatch sets another, or the event is destroyed.
    prDomEventTargetRelease(first);
    CHECK(state.references == 2);
    prDomEventTargetDispatchEvent(second, event, nullptr);
    CHECK(state.calls == 2);
    CHECK(state.currentTarget == second);
    CHECK(state.references == 1);
    prDomEventTargetRelease(second);
    CHECK(state.references == 1);
    prDomEventRelease(event);
    CHECK(state.references == 0);
    prDomEventTargetRelease(nullptr);
}

TEST_CASE("an event is captured from the root in, then bubbles from the target out") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* parent = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    PrDomText* child = domTextCreate(document, "x");
    domAttach(document, parent, nullptr);
    domAttach(parent, child, nullptr);
    Trace state;
    addTrace(parent, &state, false, 1);
    addTrace(parent, &state, true, 2);
    addTrace(child, &state, false, 3);
    addTrace(child, &state, true, 4);

    PrDomEventInit init = prDomEventInitDefault();
    init.bubbles = true;
    PrDomEvent* event = prDomEventCreate("test", &init);
    bool proceed = false;
    CHECK(prDomEventTargetDispatchEvent(targetOf(child), event, &proceed) == PrStatus_Ok);
    CHECK(proceed);
    REQUIRE(state.calls.size() == 4);
    CHECK(state.calls[0] == std::make_pair(PrDomEventPhase_Capturing, targetOf(parent)));
    CHECK(state.calls[1] == std::make_pair(PrDomEventPhase_AtTarget, targetOf(child)));
    CHECK(state.calls[2] == std::make_pair(PrDomEventPhase_AtTarget, targetOf(child)));
    CHECK(state.calls[3] == std::make_pair(PrDomEventPhase_Bubbling, targetOf(parent)));
    CHECK(prDomEventTarget(event) == targetOf(child));
    CHECK(prDomEventCurrentTarget(event) == nullptr);
    CHECK(prDomEventPhase(event) == PrDomEventPhase_None);

    // Without bubbles, the parent sees only its capture pass.
    state.calls.clear();
    prDomEventInitEvent(event, "test", false, false);
    prDomEventTargetDispatchEvent(targetOf(child), event, nullptr);
    CHECK(state.calls.size() == 3);
    prDomEventRelease(event);
    domNodeRelease(document);
}

TEST_CASE("stopping propagation ends dispatch after the current target, and canceling needs cancelable") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* parent = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    PrDomText* child = domTextCreate(document, "x");
    domAttach(document, parent, nullptr);
    domAttach(parent, child, nullptr);
    Trace state;
    state.stop = true;
    state.cancel = true;
    addTrace(child, &state, false, 1);
    addTrace(child, &state, false, 2);
    addTrace(parent, &state, false, 3);

    PrDomEventInit init = prDomEventInitDefault();
    init.bubbles = true;
    PrDomEvent* event = prDomEventCreate("test", &init);
    bool proceed = false;
    prDomEventTargetDispatchEvent(targetOf(child), event, &proceed);
    CHECK(state.calls.size() == 2);
    CHECK(proceed);
    CHECK(!prDomEventDefaultPrevented(event));

    state.calls.clear();
    prDomEventInitEvent(event, "test", true, true);
    prDomEventTargetDispatchEvent(targetOf(child), event, &proceed);
    CHECK(!proceed);
    CHECK(prDomEventDefaultPrevented(event));
    CHECK(!prDomEventReturnValue(event));
    prDomEventRelease(event);
    domNodeRelease(document);
}

TEST_CASE("a passive listener cannot cancel, and wheel listeners on a document are passive") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    CHECK(prDomAddEventListenerOptionsDefault(targetOf(document), "wheel").passive);
    CHECK(!prDomAddEventListenerOptionsDefault(targetOf(document), "click").passive);

    Trace state;
    state.cancel = true;
    PrDomEventListener callback{trace, &state, 1, nullptr, nullptr};
    PrDomAddEventListenerOptions options = prDomAddEventListenerOptionsDefault(targetOf(document), "test");
    options.passive = true;
    prDomEventTargetAddEventListener(targetOf(document), "test", &callback, &options);
    PrDomEventInit init = prDomEventInitDefault();
    init.cancelable = true;
    PrDomEvent* event = prDomEventCreate("test", &init);
    bool proceed = false;
    prDomEventTargetDispatchEvent(targetOf(document), event, &proceed);
    CHECK(state.calls.size() == 1);
    CHECK(proceed);
    prDomEventRelease(event);
    domNodeRelease(document);
}

TEST_CASE("an event createEvent makes cannot be dispatched until it is initialized") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomEvent* event = nullptr;
    PrDomEventKind kind = PrDomEventKind_Event;
    CHECK_FALSE(prDomEventKindFromInterface("WheelEvent", &kind));
    REQUIRE(prDomEventKindFromInterface("MouseEvents", &kind));
    CHECK(kind == PrDomEventKind_Mouse);
    REQUIRE(prDomEventKindFromInterface("CustomEvent", &kind));
    CHECK(kind == PrDomEventKind_Custom);
    CHECK(prDomEventCreateUninitialized(PrDomEventKind_Message, &event) == PrStatus_NotSupported);
    CHECK(prDomEventCreateUninitialized(kind, &event) == PrStatus_Ok);
    REQUIRE(event != nullptr);
    CHECK(prDomCustomEventQuery(event) != nullptr);
    CHECK(prDomEventTargetDispatchEvent(targetOf(document), event, nullptr) == PrStatus_InvalidState);
    CHECK(prDomCustomEventInitCustomEvent(prDomCustomEventQuery(event), "test", false, false));
    CHECK(prDomEventTargetDispatchEvent(targetOf(document), event, nullptr) == PrStatus_Ok);
    CHECK(!prDomEventIsTrusted(event));
    prDomEventRelease(event);
    domNodeRelease(document);
}

TEST_CASE("a keyboard event keeps its init, and initKeyboardEvent sets only its key, location and modifiers") {
    PrDomKeyboardEventInit init = prDomKeyboardEventInitDefault();
    init.ui.event.bubbles = true;
    init.key = "a";
    init.code = "KeyA";
    init.location = 7;
    init.modifiers = PrDomModifier_Shift;
    init.repeat = true;
    init.keyCode = 65;
    PrDomKeyboardEvent* event = prDomKeyboardEventCreate("keydown", &init);
    PrDomKeyboardEventInit members = prDomKeyboardEventMembers(event);
    CHECK(members.key == "a");
    CHECK(members.code == "KeyA");
    CHECK(members.location == 7);
    CHECK(members.repeat);
    CHECK(prDomUiEventWhich(prDomKeyboardEventAsUiEvent(event)) == 65);

    PrDomKeyboardEventInit legacy = prDomKeyboardEventInitDefault();
    legacy.key = "Enter";
    legacy.modifiers = PrDomModifier_Control;
    prDomKeyboardEventInitKeyboardEvent(event, "keyup", &legacy);
    members = prDomKeyboardEventMembers(event);
    CHECK(members.key == "Enter");
    CHECK(members.code == "KeyA");
    CHECK(members.location == 0);
    CHECK(members.modifiers == PrDomModifier_Control);
    CHECK(members.keyCode == 65);
    CHECK(!prDomEventBubbles(prDomKeyboardEventAsEvent(event)));
    prDomEventRelease(prDomKeyboardEventAsEvent(event));
}

TEST_CASE("which is a mouse button plus one, a key code or else a character code, and the init's value otherwise") {
    PrDomMouseEventInit mouse = prDomMouseEventInitDefault();
    mouse.button = 2;
    mouse.ui.which = 9;
    PrDomMouseEvent* click = prDomMouseEventCreate("click", &mouse);
    CHECK(prDomUiEventWhich(prDomMouseEventAsUiEvent(click)) == 3);
    prDomEventRelease(prDomMouseEventAsEvent(click));

    PrDomKeyboardEventInit key = prDomKeyboardEventInitDefault();
    key.charCode = 97;
    PrDomKeyboardEvent* press = prDomKeyboardEventCreate("keypress", &key);
    CHECK(prDomUiEventWhich(prDomKeyboardEventAsUiEvent(press)) == 97);
    prDomEventRelease(prDomKeyboardEventAsEvent(press));

    PrDomUiEventInit ui = prDomUiEventInitDefault();
    ui.which = 9;
    PrDomCompositionEvent* composition = prDomCompositionEventCreate("compositionend", &ui, "\xe6\xbc\xa2");
    CHECK(prDomUiEventWhich(prDomCompositionEventAsUiEvent(composition)) == 9);
    CHECK(prDomCompositionEventData(composition) == "\xe6\xbc\xa2");
    prDomEventRelease(prDomCompositionEventAsEvent(composition));
}

TEST_CASE("a wheel event is a mouse event with deltas") {
    PrDomWheelEventInit init = prDomWheelEventInitDefault();
    init.mouse.clientX = 10;
    init.deltaY = 120;
    init.deltaMode = PrDomDeltaMode_Line;
    PrDomWheelEvent* event = prDomWheelEventCreate("wheel", &init);
    CHECK(prDomMouseEventQuery(prDomWheelEventAsEvent(event)) != nullptr);
    CHECK(prDomMouseEventMembers(prDomWheelEventAsMouseEvent(event)).clientX == 10);
    PrDomWheelEventInit members = prDomWheelEventMembers(event);
    CHECK(members.deltaY == 120);
    CHECK(members.deltaMode == PrDomDeltaMode_Line);
    prDomEventRelease(prDomWheelEventAsEvent(event));
}

TEST_CASE("an event in a shadow tree reaches the host retargeted, and past it only where composed") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* host = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, PrStringView{});
    domInsert(document, host, nullptr);
    PrDomShadowRootInit init = prDomShadowRootInitDefault(PrDomShadowRootMode_Open);
    PrDomShadowRoot* shadow = nullptr;
    REQUIRE(domAttachShadow(host, &init, &shadow) == PrStatus_Ok);
    PrDomShadowRoot* second = nullptr;
    CHECK(domAttachShadow(host, &init, &second) == PrStatus_NotSupported);
    CHECK(second == nullptr);
    PrDomElement* inner = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Span, PrStringView{});
    domInsert(shadow, inner, nullptr);
    CHECK(domIsConnected(inner));
    CHECK(domNodeRoot(inner) == shadow);

    struct Seen {
        PrDomEventTarget* target = nullptr;
        PrDomEventPhase phase = PrDomEventPhase_None;
        int calls = 0;
    } atHost, atDocument;
    auto record = [](void* context, PrDomEvent* event) {
        Seen* seen = static_cast<Seen*>(context);
        seen->target = prDomEventTarget(event);
        seen->phase = prDomEventPhase(event);
        ++seen->calls;
    };
    PrDomEventListener hostListener = {record, &atHost, 1, nullptr, nullptr};
    PrDomEventListener documentListener = {record, &atDocument, 2, nullptr, nullptr};
    prDomEventTargetAddEventListener(targetOf(host), "test", &hostListener, nullptr);
    prDomEventTargetAddEventListener(targetOf(document), "test", &documentListener, nullptr);

    PrDomEventInit composed = {true, false, true};
    PrDomEvent* event = prDomEventCreate("test", &composed);
    prDomEventTargetDispatchEvent(targetOf(inner), event, nullptr);
    CHECK(atHost.calls == 1);
    CHECK(atHost.target == targetOf(host));
    CHECK(atHost.phase == PrDomEventPhase_AtTarget);
    CHECK(atDocument.calls == 1);
    CHECK(atDocument.target == targetOf(host));
    CHECK(prDomEventTarget(event) == targetOf(host));
    prDomEventRelease(event);

    PrDomEventInit contained = {true, false, false};
    event = prDomEventCreate("test", &contained);
    prDomEventTargetDispatchEvent(targetOf(inner), event, nullptr);
    CHECK(atHost.calls == 1);
    CHECK(atDocument.calls == 1);
    prDomEventRelease(event);
    domNodeRelease(document);
}
