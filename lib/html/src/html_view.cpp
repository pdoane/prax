// HTML views: a top-level browsing context's document on a host's screen.

#include "pr/html_view.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/atomic.h"
#include "pr/draw_list.h"
#include "pr/html.h"
#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/text_buffer.h"
#include "pr/utf8.h"

#include "css/src/css_presentation.h"
#include "css/src/css_serialize.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_event.h"
#include "dom/src/dom_node.h"
#include "html/src/html_focus.h"
#include "html/src/html_page.h"
#include "html/src/html_window.h"

namespace pr_impl {

namespace {

// The browsing context of an iframe that the view presents in its iframe's slot.
struct ViewFrame {
    PrHtmlPage* page;
    uint64_t embed;                 // the id by which its scenes fill the slot
    PrVec2 extent;                  // its viewport: the iframe's content box in the last scene around it
    bool posted;                    // true if its presentation has a job whose scene the view has not committed
};

// The default size of an iframe's viewport, used until a scene around the iframe gives its size
// (html/#the-iframe-element).
constexpr PrVec2 kFrameExtent{300, 150};

// The part of a view that other threads post to (prHtmlViewPostEvent). The view holds a reference, and so does each
// post that has not run. The view clears `view` as it is destroyed, on the agent's thread, where the posts run.
struct ViewInbox {
    Atomic<uint32_t> references;
    PrHtmlView* PR_NULLABLE view;
    PrHtmlPage* page;
    PrDomAgent* agent;
    PrLayoutCompositor* PR_NULLABLE compositor;
};

struct HtmlView {
    PrHtmlPage* page;
    ViewInbox* inbox;
    PrLayoutCompositor* PR_NULLABLE compositor;
    double jobBudget;
    bool canvasFilled;                           // as the description gives it
    PrCssPresentation* PR_NULLABLE presentation; // the browsing context's presentation, as the view last saw it
    PrVec2 extent;                               // in CSS pixels
    bool posted;                                 // true if the presentation has a job whose scene the view has not
                                                 // committed
    bool changed;                                // true if the viewport or the presentation changed since the last job
                                                 // started
    uint32_t scrollsTaken;                       // the value that prLayoutCompositorTakeScrolls last returned
    Array<PrLayoutTrack> tracks;                 // scratch: the presentation's composited animations
    Array<PrLayoutSceneScroll> scrolls;          // scratch: the offsets that a commit gives
    Array<ViewFrame> frames;                     // the iframes' browsing contexts, including nested ones, in the order
                                                 // found
    Array<PrHtmlPage*> found;                    // scratch: the frames of one browsing context
    Array<PrLayoutSceneEmbed> embeds;            // scratch: the slots of a scene

    // The pointer's state: its position as the document last saw it, the element that it hovers, and the element that
    // the main button was pressed on, each in the innermost document that the pointer is over. The view holds the
    // pressed element.
    PrVec2 pointer;
    bool pointed;                   // true once the pointer has been over the view
    bool hoverDue;                  // true if the pointer moved, or the document scrolled under it, since last hovering
    PrDomElement* PR_NULLABLE hovered;
    PrDomElement* PR_NULLABLE pressed;
    uint16_t buttons;               // the buttons held, as bits of MouseEvent's buttons
    PrAppCursor cursor;             // the cursor at the pointer, as of the last hover or commit

    // The keyboard's state: the code and modifiers of the last key pressed, and the element that Space pressed, which
    // the view holds.
    PrAppKeyCode keyCode;
    PrDomModifiers keyModifiers;
    PrDomElement* PR_NULLABLE spaced;
    bool typing;                    // true from a keydown that no listener canceled until its text or its release
    bool composing;                 // true during an input method's composition
    bool committing;                // true from the end of a composition until the text that it commits

    // The current wheel sequence (kPrLayoutWheelSequenceGap): the time and position of its last wheel, its target and
    // that target's box, which the sequence keeps, and whether a listener canceled its first wheel, which lets the rest
    // of it be canceled too. The view holds the target.
    double wheelTime;
    PrVec2 wheelAt;
    PrVec2 wheelPoint;              // the sequence's point in the viewport of its target's document
    PrDomElement* PR_NULLABLE wheelTarget;
    PrLayoutNode wheelBox;
    bool wheelCanceled;
};

PrDomDocument* documentOf(HtmlView* v) {
    return prHtmlWindowDocument(prHtmlPageWindow(v->page));
}

// Returns the browsing context's presentation, which a navigation replaces, with the view's job budget set on it.
PrCssPresentation* PR_NULLABLE presentationOf(HtmlView* v) {
    PrCssPresentation* presentation = prHtmlPagePresentation(v->page);
    if (presentation != v->presentation) {
        v->presentation = presentation;
        v->posted = false;
        v->changed = true;
        if (presentation != nullptr)
            prCssPresentationSetJobBudget(presentation, v->jobBudget);
    }
    return presentation;
}

// Sets the viewport of the view and of its page.
void setViewport(HtmlView* v, PrVec2 extent) {
    v->extent = extent;
    prHtmlPageSetViewport(v->page, extent);
    v->changed = true;
}

// Returns the frame whose scenes fill the slots of `embed`, or null if there is none.
ViewFrame* PR_NULLABLE frameOf(HtmlView* v, uint64_t embed) {
    for (uint32_t i = 0; i < v->frames.count; ++i) {
        if (v->frames.data[i].embed == embed)
            return &v->frames.data[i];
    }
    return nullptr;
}

// Returns a frame's presentation, with the view's job budget set on it, or null if the frame's host gives none.
PrCssPresentation* PR_NULLABLE frameRender(HtmlView* v, const ViewFrame& frame) {
    PrCssPresentation* presentation = prHtmlPagePresentation(frame.page);
    if (presentation != nullptr)
        prCssPresentationSetJobBudget(presentation, v->jobBudget);
    return presentation;
}

// Finds the iframes' browsing contexts in the window and in their own windows, and keeps the view's state for those
// that it already knew. Drops what the compositor held for those that are gone.
void findFrames(HtmlView* v) {
    Array<ViewFrame> last = v->frames;
    v->frames = {};
    v->frames.push(ViewFrame{v->page, 0, v->extent, false});
    for (uint32_t at = 0; at < v->frames.count; ++at) {
        v->found.clear();
        htmlPageFrames(v->frames.data[at].page, &v->found);
        for (uint32_t i = 0; i < v->found.count; ++i) {
            uint64_t embed = htmlPageEmbed(v->found.data[i]);
            htmlPageFollowFrameScheme(v->frames.data[at].page, v->found.data[i]);
            ViewFrame frame{v->found.data[i], embed, kFrameExtent, false};
            for (uint32_t k = 0; k < last.count; ++k) {
                if (last.data[k].embed == embed)
                    frame = last.data[k];
            }
            v->frames.push(frame);
        }
    }
    // The first entry stood for the window itself while the frames were being found.
    v->frames.erase(0);
    for (uint32_t k = 0; k < last.count; ++k) {
        if (frameOf(v, last.data[k].embed) == nullptr && v->compositor != nullptr)
            prLayoutCompositorRemove(v->compositor, last.data[k].embed);
    }
    last.reset();
}

// Sizes the frames whose slots `scene` holds to those slots, as the scene's layout determined them.
void sizeFrames(HtmlView* v, const PrLayoutScene* scene) {
    uint32_t count = prLayoutSceneEmbeds(scene, nullptr, 0);
    v->embeds.resize(count);
    prLayoutSceneEmbeds(scene, v->embeds.data, count);
    for (uint32_t i = 0; i < count; ++i) {
        ViewFrame* frame = frameOf(v, v->embeds.data[i].embed);
        PrVec2 extent = prLayoutVecToPx(v->embeds.data[i].extent);
        if (frame == nullptr || (frame->extent.x == extent.x && frame->extent.y == extent.y))
            continue;
        frame->extent = extent;
        prHtmlPageSetViewport(frame->page, extent);
        v->changed = true;
    }
}

// Sets `out` to the presentation's scroll offsets for the containers of `scene`.
void sceneScrolls(PrCssPresentation* presentation, const PrLayoutScene* scene, Array<PrLayoutSceneScroll>& out) {
    const PrLayoutTree* tree = prCssPresentationTree(presentation);
    out.resize(prLayoutSceneScrolls(tree, scene, nullptr, 0));
    prLayoutSceneScrolls(tree, scene, out.data, out.count);
}

// The maximum number of offsets that the view takes from a presentation at once.
constexpr uint32_t kTakenScrolls = 16;

// Commits the scene that the presentation recorded for `embed`, if it recorded one, with the presentation's composited
// animations and scroll offsets. Sizes the frames whose slots the scene holds. Returns true if it committed a scene.
bool commitScene(HtmlView* v, PrCssPresentation* presentation, uint64_t embed, PrAffine2 transform) {
    const PrLayoutScene* scene = prCssPresentationTakeScene(presentation);
    if (scene == nullptr)
        return false;
    sizeFrames(v, scene);
    if (v->compositor != nullptr) {
        uint32_t count = prCssPresentationTracks(presentation, nullptr, 0);
        v->tracks.resize(count);
        prCssPresentationTracks(presentation, v->tracks.data, count);
        sceneScrolls(presentation, scene, v->scrolls);
        PrLayoutCommit commit{
            .embed = embed,
            .scene = scene,
            .transform = transform,
            .tracks = v->tracks.data,
            .trackCount = count,
            .scrolls = v->scrolls.data,
            .scrollCount = v->scrolls.count,
            .scrollsApplied = v->scrollsTaken,
        };
        prLayoutCompositorCommit(v->compositor, &commit);
    }
    prLayoutSceneRelease(scene);
    return true;
}

// Gives the compositor the offsets that the document of `presentation` scrolled itself to since they were last taken,
// for `embed`. No job is needed for these offsets.
void forwardScrolls(HtmlView* v, PrCssPresentation* presentation, uint64_t embed) {
    PrLayoutSceneScroll taken[kTakenScrolls];
    for (;;) {
        uint32_t count = prCssPresentationTakeScrolls(presentation, taken, kTakenScrolls);
        if (count == 0)
            break;
        if (v->compositor != nullptr)
            prLayoutCompositorSetScrolls(v->compositor, embed, taken, count, v->scrollsTaken);
    }
}

void paintEmbedded(void* PR_NULLABLE context, uint64_t embed, PrLayoutVec extent, PrDrawList* list);

// Plays the scene of the presentation's tree into `list` at the tree's scroll offsets, with its composited animations
// sampled at the presentation's time and each frame's document in its slot, and without a fill for its canvas if
// `canvasFilled` is true. Returns the scene's canvas color.
PrColor16 playTree(HtmlView* v, PrCssPresentation* presentation, PrDrawList* list, bool canvasFilled) {
    const PrLayoutScene* scene = prLayoutTreeScene(prCssPresentationTree(presentation));
    Array<PrLayoutSceneScroll> scrolls;
    sceneScrolls(presentation, scene, scrolls);
    Array<PrLayoutTrack> tracks;
    tracks.resize(prCssPresentationTracks(presentation, nullptr, 0));
    prCssPresentationTracks(presentation, tracks.data, tracks.count);
    Array<PrLayoutSceneEffect> effects;
    double time = presentation->animations.time;
    effects.resize(prLayoutSceneSampleTracks(scene, tracks.data, tracks.count, time, nullptr, 0));
    prLayoutSceneSampleTracks(scene, tracks.data, tracks.count, time, effects.data, effects.count);
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.effects = effects.data;
    options.effectCount = effects.count;
    options.scrolls = scrolls.data;
    options.scrollCount = scrolls.count;
    options.embed = paintEmbedded;
    options.embedContext = v;
    options.canvasFilled = canvasFilled;
    prLayoutScenePlay(scene, list, &options);
    PrColor16 canvas = prLayoutSceneCanvasColor(scene);
    scrolls.reset();
    tracks.reset();
    effects.reset();
    prLayoutSceneRelease(scene);
    return canvas;
}

// Applies an offset that the compositor scrolled to, for the window's document or a frame's document.
void applyScroll(void* PR_NULLABLE context, const PrLayoutScroll* scroll) {
    HtmlView* v = static_cast<HtmlView*>(context);
    PrCssPresentation* presentation = nullptr;
    if (scroll->embed == 0)
        presentation = presentationOf(v);
    else if (const ViewFrame* frame = frameOf(v, scroll->embed))
        presentation = frameRender(v, *frame);
    if (presentation != nullptr)
        prCssPresentationSetScroll(presentation, scroll->node, scroll->offset);
}

// Plays a frame's document into its slot in the same way that prHtmlViewPaint records a document: brought up to date on
// the caller's thread, at the slot's extent.
void paintEmbedded(void* PR_NULLABLE context, uint64_t embed, PrLayoutVec extent, PrDrawList* list) {
    HtmlView* v = static_cast<HtmlView*>(context);
    ViewFrame* frame = frameOf(v, embed);
    PrCssPresentation* presentation = frame != nullptr ? frameRender(v, *frame) : nullptr;
    if (presentation == nullptr || prHtmlPageIsRenderBlocked(frame->page))
        return;
    PrVec2 size = prLayoutVecToPx(extent);
    if (size.x != frame->extent.x || size.y != frame->extent.y) {
        frame->extent = size;
        prHtmlPageSetViewport(frame->page, size);
    }
    prHtmlPresentationUpdate(presentation, extent);
    playTree(v, presentation, list, false);
}

// Returns the presentation of `document`, which is the view's document or the current document of a frame's browsing
// context, or null if it has none or is render-blocked. A frame's presentation is brought up to date first, so that
// input hit tests and scrolls its document as it now is, as a host brings the view's own document up to date before
// input (prHtmlPresentationUpdate).
PrCssPresentation* PR_NULLABLE presentationOfDocument(HtmlView* v, const PrDomDocument* document) {
    if (document == documentOf(v))
        return presentationOf(v);
    PrHtmlPage* page = htmlDocumentPage(document);
    PrCssPresentation* presentation = page != nullptr ? prHtmlPagePresentation(page) : nullptr;
    if (presentation == nullptr || prHtmlPageIsRenderBlocked(page))
        return nullptr;
    cssPresentationFlush(presentation);
    return presentation;
}

// What a point of the viewport lands on: the deepest element whose box is topmost there, or a text node's parent, in
// the innermost document that the point reaches through the frames over it, and the point in that document's viewport.
// Over the canvas, past the boxes of a document's elements, it is that document's root. The element is null only for a
// document with no root.
struct ViewHit {
    PrDomElement* PR_NULLABLE element;
    PrVec2 point;
    bool text;                      // true if the point hits text, whose parent is `element`

    // The presentation of the element's document, or null if it has none.
    PrCssPresentation* PR_NULLABLE presentation;
};

// Hit tests a point of the viewport. A point in the content box of a frame whose document the view presents goes on
// into that document.
ViewHit hitAt(HtmlView* v, PrVec2 point) {
    ViewHit hit{nullptr, point, false, nullptr};
    PrDomDocument* document = documentOf(v);
    for (PrCssPresentation* presentation = presentationOf(v); presentation != nullptr;) {
        hit.presentation = presentation;
        PrDomNode* node = prCssPresentationHitTest(presentation, prLayoutVecFromPx(hit.point));
        PrDomElement* element = node != nullptr ? prDomElementQuery(prDomNodeAsEventTarget(node)) : nullptr;
        hit.text = node != nullptr && element == nullptr;
        hit.element = hit.text ? prDomNodeParentElement(node) : element;
        if (hit.element == nullptr)
            hit.element = prDomDocumentElement(document);
        PrHtmlWindow* window = hit.element != nullptr ? htmlFrameWindow(hit.element) : nullptr;
        PrCssPresentation* inner = window != nullptr ? presentationOfDocument(v, prHtmlWindowDocument(window)) : nullptr;
        if (inner == nullptr)
            break;
        // The frame's document fills its content box, through whatever transforms the frame is under.
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        PrLayoutNode box = prCssPresentationLayoutNode(presentation, prDomElementAsNode(hit.element));
        PrLayoutVec local{};
        if (box == kPrLayoutNodeNone || !prLayoutNodeLocalPoint(tree, box, prLayoutVecFromPx(hit.point), &local))
            break;
        PrRect content = prLayoutRectToPx(prLayoutNodeReplacedContent(tree, box));
        PrVec2 at = prLayoutVecToPx(local);
        if (at.x < content.left || at.x >= content.right || at.y < content.top || at.y >= content.bottom)
            break;
        hit.point = PrVec2{at.x - content.left, at.y - content.top};
        presentation = inner;
        document = prHtmlWindowDocument(window);
    }
    if (hit.element == nullptr)
        hit.element = prDomDocumentElement(document);
    return hit;
}

// Designates `element` with `set`, which is prHtmlDocumentSetHover or prHtmlDocumentSetActive, in its own document, and
// in each document around it the frame that holds that document, as html/#selector-hover has a frame match when the
// element in it does. Every other document that the view presents designates nothing.
void designate(HtmlView* v, PrDomElement* PR_NULLABLE element, void (*set)(PrDomDocument*, PrDomElement*)) {
    auto designated = [&](const PrDomDocument* document) {
        for (PrDomElement* at = element; at != nullptr; at = htmlFrameContainer(at->document)) {
            if (at->document == document)
                return at;
        }
        return static_cast<PrDomElement*>(nullptr);
    };
    set(documentOf(v), designated(documentOf(v)));
    for (uint32_t i = 0; i < v->frames.count; ++i) {
        PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(v->frames.data[i].page));
        set(document, designated(document));
    }
    // A frame that the view has not found since it loaded is designated along the element's own chain.
    for (PrDomElement* at = element; at != nullptr; at = htmlFrameContainer(at->document))
        set(at->document, at);
}

// Returns the cursor of a predefined keyword of the cursor property other than auto (css-ui-4/#cursor).
PrAppCursor cursorFromKeyword(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_None: return PrAppCursor_None;
        case PrCssKeyword_ContextMenu: return PrAppCursor_ContextMenu;
        case PrCssKeyword_Help: return PrAppCursor_Help;
        case PrCssKeyword_Pointer: return PrAppCursor_Hand;
        case PrCssKeyword_Progress: return PrAppCursor_Progress;
        case PrCssKeyword_Wait: return PrAppCursor_Wait;
        case PrCssKeyword_Cell: return PrAppCursor_Cell;
        case PrCssKeyword_Crosshair: return PrAppCursor_Crosshair;
        case PrCssKeyword_Text: return PrAppCursor_Text;
        case PrCssKeyword_VerticalText: return PrAppCursor_VerticalText;
        case PrCssKeyword_Alias: return PrAppCursor_Alias;
        case PrCssKeyword_Copy: return PrAppCursor_Copy;
        case PrCssKeyword_Move: return PrAppCursor_Move;
        case PrCssKeyword_NoDrop: return PrAppCursor_NoDrop;
        case PrCssKeyword_NotAllowed: return PrAppCursor_NotAllowed;
        case PrCssKeyword_Grab: return PrAppCursor_Grab;
        case PrCssKeyword_Grabbing: return PrAppCursor_Grabbing;
        case PrCssKeyword_NResize: return PrAppCursor_ResizeN;
        case PrCssKeyword_EResize: return PrAppCursor_ResizeE;
        case PrCssKeyword_SResize: return PrAppCursor_ResizeS;
        case PrCssKeyword_WResize: return PrAppCursor_ResizeW;
        case PrCssKeyword_NeResize: return PrAppCursor_ResizeNE;
        case PrCssKeyword_NwResize: return PrAppCursor_ResizeNW;
        case PrCssKeyword_SeResize: return PrAppCursor_ResizeSE;
        case PrCssKeyword_SwResize: return PrAppCursor_ResizeSW;
        case PrCssKeyword_EwResize: return PrAppCursor_ResizeX;
        case PrCssKeyword_NsResize: return PrAppCursor_ResizeY;
        case PrCssKeyword_NeswResize: return PrAppCursor_ResizeNESW;
        case PrCssKeyword_NwseResize: return PrAppCursor_ResizeNWSE;
        case PrCssKeyword_ColResize: return PrAppCursor_ResizeColumn;
        case PrCssKeyword_RowResize: return PrAppCursor_ResizeRow;
        case PrCssKeyword_AllScroll: return PrAppCursor_AllScroll;
        case PrCssKeyword_ZoomIn: return PrAppCursor_ZoomIn;
        case PrCssKeyword_ZoomOut: return PrAppCursor_ZoomOut;
        default: return PrAppCursor_Arrow;
    }
}

// Returns the predefined keyword that ends a computed value of the cursor property. The view shows no cursor images, so
// the keyword is the cursor, as it is when none of the images loads.
PrCssKeyword cursorKeyword(CssValueRun value) {
    PrCssKeyword keyword = PrCssKeyword_Auto;
    for (uint32_t i = 0; i < value.count; i += value.values[i].next) {
        if (value.values[i].kind == PrCssValueKind_Keyword)
            keyword = cssValueKeyword(&value.values[i]);
    }
    return keyword;
}

// Returns true if the used value of the element's user-select is not none (css-ui-4/#content-selection). The value auto
// takes the parent's used value, so the nearest value along the ancestors that is not auto decides.
bool selectable(const PrCssPresentation* presentation, const PrDomElement* element) {
    for (const PrDomNode* at = element; at != nullptr; at = domShadowIncludingParent(at)) {
        const PrDomElement* ancestor = domElementFromNode(at);
        const CssComputedStyle* style = ancestor != nullptr ? cssPresentationStyle(presentation, ancestor) : nullptr;
        if (style == nullptr)
            continue;
        PrCssKeyword value = cssComputedFirstKeyword(style, PrCssProperty_UserSelect);
        if (value != PrCssKeyword_Auto)
            return value != PrCssKeyword_None;
    }
    return true;
}

// Returns true if an element whose box is hit is part of a widget other than a text control, such as a button's label.
// The text of such a widget is not selectable as a document's text is.
bool inWidget(const PrCssPresentation* presentation, const PrDomElement* element) {
    for (const PrDomNode* at = element; at != nullptr; at = domShadowIncludingParent(at)) {
        const PrDomElement* ancestor = domElementFromNode(at);
        const CssComputedStyle* style = ancestor != nullptr ? cssPresentationStyle(presentation, ancestor) : nullptr;
        if (style != nullptr && style->appearance != CssAppearance_None)
            return true;
    }
    return false;
}

// Returns true if the element is editable, or is in the shadow tree of a text control.
bool inEditable(const PrDomElement* element) {
    for (const PrDomNode* at = element; at != nullptr; at = domShadowIncludingParent(at)) {
        const PrDomElement* ancestor = domElementFromNode(at);
        if (ancestor != nullptr && ancestor->space == PrDomNamespace_Html && domElementReadWrite(ancestor))
            return true;
    }
    return false;
}

// Returns the cursor that `hit` shows, as prHtmlViewCursor describes. The text cursor is vertical in a vertical writing
// mode.
PrAppCursor cursorOf(const ViewHit& hit) {
    if (hit.element == nullptr || hit.presentation == nullptr)
        return PrAppCursor_Arrow;
    const CssComputedStyle* style = cssPresentationStyle(hit.presentation, hit.element);
    if (style == nullptr)
        return PrAppCursor_Arrow;
    PrCssKeyword keyword = cursorKeyword(cssComputedValue(style, PrCssProperty_Cursor));
    if (keyword != PrCssKeyword_Auto)
        return cursorFromKeyword(keyword);
    bool text = hit.text && selectable(hit.presentation, hit.element) && !inWidget(hit.presentation, hit.element);
    if (!text && !inEditable(hit.element))
        return PrAppCursor_Arrow;
    switch (cssComputedFirstKeyword(style, PrCssProperty_WritingMode)) {
        case PrCssKeyword_VerticalRl:
        case PrCssKeyword_VerticalLr:
        case PrCssKeyword_SidewaysRl:
        case PrCssKeyword_SidewaysLr:
            return PrAppCursor_VerticalText;
        default:
            return PrAppCursor_Text;
    }
}

// Hovers the element under the pointer, and takes the cursor there.
void hover(HtmlView* v) {
    v->hoverDue = false;
    ViewHit hit = hitAt(v, v->pointer);
    v->cursor = cursorOf(hit);
    PrDomElement* element = hit.element;
    if (element == v->hovered)
        return;
    v->hovered = element;
    designate(v, element, prHtmlDocumentSetHover);
}

// Scrolls as a user's scroll by `delta` does from the box of `element`: in its document, then from the frame that holds
// that document in the document around it, outward, as far as each document passes the scroll on.
void scrollFrom(HtmlView* v, PrDomElement* element, PrVec2 delta) {
    PrLayoutVec rest = prLayoutVecFromPx(delta);
    for (PrDomElement* at = element; at != nullptr && rest != PrLayoutVec{}; at = htmlFrameContainer(at->document)) {
        PrCssPresentation* presentation = presentationOfDocument(v, at->document);
        if (presentation == nullptr)
            return;
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(at));
        rest = prCssPresentationScrollFrom(presentation, node, rest);
    }
}

// Returns the bit in MouseEvent's buttons for a button numbered as in MouseEvent's button. The two numberings swap the
// secondary and the auxiliary buttons.
uint16_t buttonBit(int16_t button) {
    uint32_t bit = button == 1 ? 2 : button == 2 ? 1 : uint32_t(button);
    return uint16_t(1u << (bit & 15));
}

// Returns the modifiers of an app's event as the DOM names them.
PrDomModifiers modifiersOf(PrAppModifierFlags flags) {
    struct Modifier {
        PrAppModifierFlags app;
        PrDomModifiers dom;
    };
    constexpr Modifier kModifiers[] = {
        {PrAppModifierFlag_Shift, PrDomModifier_Shift}, {PrAppModifierFlag_Control, PrDomModifier_Control},
        {PrAppModifierFlag_Alt, PrDomModifier_Alt},     {PrAppModifierFlag_Super, PrDomModifier_Meta},
        {PrAppModifierFlag_CapsLock, PrDomModifier_CapsLock},
    };
    PrDomModifiers modifiers = 0;
    for (const Modifier& modifier : kModifiers) {
        if ((flags & modifier.app) != 0)
            modifiers = PrDomModifiers(modifiers | modifier.dom);
    }
    return modifiers;
}

// Returns an app's button as MouseEvent numbers it. An app numbers the secondary button 1 and the auxiliary button 2,
// and MouseEvent the other way around.
int16_t mouseButtonOf(uint8_t button) {
    return int16_t(button == 1 ? 2 : button == 2 ? 1 : button);
}

// Drops the view's hold on the pressed element.
void release(HtmlView* v) {
    if (v->pressed != nullptr)
        prDomNodeRelease(prDomElementAsNode(v->pressed));
    v->pressed = nullptr;
}

// Ends the wheel sequence, and drops the view's hold on its target.
void endWheel(HtmlView* v) {
    if (v->wheelTarget != nullptr)
        prDomNodeRelease(prDomElementAsNode(v->wheelTarget));
    v->wheelTarget = nullptr;
}

// Drops the view's hold on the element that Space pressed.
void releaseSpaced(HtmlView* v) {
    if (v->spaced != nullptr)
        prDomNodeRelease(prDomElementAsNode(v->spaced));
    v->spaced = nullptr;
}

// Dispatches `event` at `target` as an event that the engine fires, which is trusted. Returns true if no listener
// canceled it.
bool dispatchTrusted(PrDomEventTarget* target, PrDomEvent* event) {
    event->trusted = true;
    return domEventDispatch(target, event);
}

// Returns the init of an event that input fires at `target`: one that bubbles, can be canceled if `cancelable`, and
// crosses shadow boundaries, with the window of the target's document as its view.
PrDomUiEventInit inputInit(const PrDomElement* target, bool cancelable) {
    PrDomUiEventInit init = prDomUiEventInitDefault();
    init.event.bubbles = true;
    init.event.cancelable = cancelable;
    init.event.composed = true;
    init.view = target->document->global;
    return init;
}

// Fires a MouseEvent of `type` at `element` for the pointer's action, at `point` in the viewport of the element's
// document.
bool fireMouse(HtmlView* v, PrStringView type, PrDomElement* element, PrVec2 point, const PrAppEvent* event) {
    PrDomMouseEventInit init = prDomMouseEventInitDefault();
    init.ui = inputInit(element, true);
    init.ui.detail = 1;
    init.screenX = double(event->position.x);
    init.screenY = double(event->position.y);
    init.clientX = double(point.x);
    init.clientY = double(point.y);
    init.button = mouseButtonOf(event->button);
    init.buttons = v->buttons;
    init.modifiers = modifiersOf(event->modifiers);
    PrDomMouseEvent* mouse = prDomMouseEventCreate(type, &init);
    bool proceed = dispatchTrusted(prDomNodeAsEventTarget(prDomElementAsNode(element)), prDomMouseEventAsEvent(mouse));
    prDomEventRelease(prDomMouseEventAsEvent(mouse));
    return proceed;
}

// Fires a synthetic click at `element`, as a key activates it (html/#fire-a-synthetic-pointer-event). The click runs
// the element's activation behavior.
void fireClick(PrDomElement* element, PrDomModifiers modifiers) {
    PrDomMouseEventInit init = prDomMouseEventInitDefault();
    init.ui = inputInit(element, true);
    init.modifiers = modifiers;
    PrDomMouseEvent* click = prDomMouseEventCreate("click", &init);
    dispatchTrusted(prDomNodeAsEventTarget(prDomElementAsNode(element)), prDomMouseEventAsEvent(click));
    prDomEventRelease(prDomMouseEventAsEvent(click));
}

// Returns the target of the keyboard's events (htmlKeyTarget). Returns null if the document has no element.
PrDomElement* PR_NULLABLE keyTarget(HtmlView* v) {
    return htmlKeyTarget(documentOf(v));
}

// Returns true if `key` is a modifier key, which alone does not make focus show
// (selectors-4/#the-focus-visible-pseudo).
bool isModifierKey(PrAppKey key) {
    return key == PrAppKey_Shift || key == PrAppKey_Control || key == PrAppKey_Alt || key == PrAppKey_Meta ||
        key == PrAppKey_CapsLock || key == PrAppKey_NumLock;
}

// Returns the key value of a key event (uievents-key): the name of a named key, or else the character that the key
// types, or else "Unidentified".
PrStringView keyValue(const PrAppEvent* event) {
    PrStringView name = prAppKeyName(event->key);
    if (name.size != 0)
        return name;
    return event->text.size != 0 ? event->text : PrStringView{"Unidentified"};
}

// Returns where the key at `code` is among the keys that share its value (uievents/#events-keyboard-key-location).
uint32_t keyLocation(PrAppKeyCode code) {
    switch (code) {
        case PrAppKeyCode_AltLeft:
        case PrAppKeyCode_ControlLeft:
        case PrAppKeyCode_MetaLeft:
        case PrAppKeyCode_ShiftLeft:
            return PrDomKeyLocation_Left;
        case PrAppKeyCode_AltRight:
        case PrAppKeyCode_ControlRight:
        case PrAppKeyCode_MetaRight:
        case PrAppKeyCode_ShiftRight:
            return PrDomKeyLocation_Right;
        default:
            if (code >= PrAppKeyCode_Numpad0 && code <= PrAppKeyCode_NumpadSubtract)
                return PrDomKeyLocation_Numpad;
            return PrDomKeyLocation_Standard;
    }
}

// The legacy key code of a keydown or keyup during a composition, which reports a key that the input method takes
// (uievents/#determine-keydown-keyup-keyCode).
constexpr uint32_t kComposingKeyCode = 229;

// Returns the legacy key code of a keydown or keyup (uievents/#legacy-key-models): the virtual key code that Windows
// gives the key, which browsers report on every platform. A named key has the code of its meaning, and a key that types
// a character has the code of its position on a US keyboard. Returns zero for a key that has no code.
uint32_t legacyKeyCode(PrAppKey key, PrAppKeyCode code) {
    struct NamedCode {
        PrAppKey key;
        uint8_t keyCode;
    };
    constexpr NamedCode kNamed[] = {
        {PrAppKey_Backspace, 8},  {PrAppKey_Tab, 9},       {PrAppKey_Clear, 12},     {PrAppKey_Enter, 13},
        {PrAppKey_Shift, 16},     {PrAppKey_Control, 17},  {PrAppKey_Alt, 18},       {PrAppKey_CapsLock, 20},
        {PrAppKey_Escape, 27},    {PrAppKey_PageUp, 33},   {PrAppKey_PageDown, 34},  {PrAppKey_End, 35},
        {PrAppKey_Home, 36},      {PrAppKey_Left, 37},     {PrAppKey_Up, 38},        {PrAppKey_Right, 39},
        {PrAppKey_Down, 40},      {PrAppKey_Insert, 45},   {PrAppKey_Delete, 46},    {PrAppKey_Help, 47},
        {PrAppKey_Meta, 91},      {PrAppKey_ContextMenu, 93}, {PrAppKey_NumLock, 144},
    };
    for (const NamedCode& named : kNamed) {
        if (named.key == key)
            return named.keyCode;
    }
    if (key >= PrAppKey_F1 && key <= PrAppKey_F20)
        return 112 + uint32_t(key - PrAppKey_F1);
    struct PositionCode {
        PrAppKeyCode code;
        uint8_t keyCode;
    };
    constexpr PositionCode kPositions[] = {
        {PrAppKeyCode_Space, 32},          {PrAppKeyCode_Semicolon, 186},      {PrAppKeyCode_Equal, 187},
        {PrAppKeyCode_Comma, 188},         {PrAppKeyCode_Minus, 189},          {PrAppKeyCode_Period, 190},
        {PrAppKeyCode_Slash, 191},         {PrAppKeyCode_Backquote, 192},      {PrAppKeyCode_BracketLeft, 219},
        {PrAppKeyCode_Backslash, 220},     {PrAppKeyCode_BracketRight, 221},   {PrAppKeyCode_Quote, 222},
        {PrAppKeyCode_IntlBackslash, 226}, {PrAppKeyCode_IntlRo, 193},         {PrAppKeyCode_IntlYen, 255},
        {PrAppKeyCode_NumpadMultiply, 106}, {PrAppKeyCode_NumpadAdd, 107},     {PrAppKeyCode_NumpadSubtract, 109},
        {PrAppKeyCode_NumpadDecimal, 110}, {PrAppKeyCode_NumpadDivide, 111},   {PrAppKeyCode_NumpadEqual, 187},
        {PrAppKeyCode_NumpadComma, 194},   {PrAppKeyCode_PrintScreen, 44},     {PrAppKeyCode_ScrollLock, 145},
        {PrAppKeyCode_Pause, 19},
    };
    for (const PositionCode& position : kPositions) {
        if (position.code == code)
            return position.keyCode;
    }
    if (code >= PrAppKeyCode_KeyA && code <= PrAppKeyCode_KeyZ)
        return 'A' + uint32_t(code - PrAppKeyCode_KeyA);
    if (code >= PrAppKeyCode_Digit0 && code <= PrAppKeyCode_Digit9)
        return '0' + uint32_t(code - PrAppKeyCode_Digit0);
    if (code >= PrAppKeyCode_Numpad0 && code <= PrAppKeyCode_Numpad9)
        return 96 + uint32_t(code - PrAppKeyCode_Numpad0);
    // A key whose position is not known has the code of its character, if it types a letter, a digit or a space.
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == ' ')
        return key;
    return 0;
}

// Fires a KeyboardEvent of `type` at `element`, as `init` describes it. Returns true if no listener canceled it.
bool fireKey(PrDomElement* element, PrStringView type, const PrDomKeyboardEventInit& init) {
    PrDomKeyboardEvent* key = prDomKeyboardEventCreate(type, &init);
    bool proceed = dispatchTrusted(prDomNodeAsEventTarget(prDomElementAsNode(element)), prDomKeyboardEventAsEvent(key));
    prDomEventRelease(prDomKeyboardEventAsEvent(key));
    return proceed;
}

// Returns the init of the keydown or keyup of `event` at `target`. During a composition, a keydown reports the key that
// the input method takes as Process (uievents/#keys-IME).
PrDomKeyboardEventInit keyInit(HtmlView* v, const PrDomElement* target, const PrAppEvent* event) {
    PrDomKeyboardEventInit init = prDomKeyboardEventInitDefault();
    init.ui = inputInit(target, true);
    init.modifiers = modifiersOf(event->modifiers);
    init.code = prAppKeyCodeName(event->code);
    init.location = keyLocation(event->code);
    init.repeat = event->repeat;
    init.isComposing = v->composing;
    bool process = v->composing && event->kind == PrAppEventKind_KeyDown;
    init.key = process ? PrStringView{"Process"} : keyValue(event);
    init.keyCode = process ? kComposingKeyCode : legacyKeyCode(event->key, event->code);
    return init;
}

// Fires a keypress at `element` for `character`, which the last key pressed typed (uievents/#event-type-keypress). Its
// legacy codes are the character's code point. Returns true if no listener canceled it.
bool fireKeypress(HtmlView* v, PrDomElement* element, PrStringView character, uint32_t codePoint) {
    PrDomKeyboardEventInit init = prDomKeyboardEventInitDefault();
    init.ui = inputInit(element, true);
    init.modifiers = v->keyModifiers;
    init.key = character;
    init.code = prAppKeyCodeName(v->keyCode);
    init.location = keyLocation(v->keyCode);
    init.charCode = codePoint;
    init.keyCode = codePoint;
    return fireKey(element, "keypress", init);
}

// Returns true if `element` is a link, which Enter follows.
bool isLink(const PrDomElement* element) {
    return (domIsHtml(element, PrDomTag_A) || domIsHtml(element, PrDomTag_Area)) &&
        domAttributeValue(element, "href").data != nullptr;
}

// Returns true if `element` is a button, which Enter and Space activate: a button element, or an input of a button
// type.
bool isButton(const PrDomElement* element) {
    if (domIsHtml(element, PrDomTag_Button))
        return true;
    if (!domIsHtml(element, PrDomTag_Input))
        return false;
    PrStringView type = domAttributeValue(element, "type");
    return asciiEqualsFold(type, "button") || asciiEqualsFold(type, "submit") || asciiEqualsFold(type, "reset") ||
        asciiEqualsFold(type, "image");
}

// Returns true if `element` takes the keys that would scroll for its own use, as a form control does.
bool takesScrollKeys(const PrDomElement* element) {
    return (domIsHtml(element, PrDomTag_Input) && !isButton(element)) || domIsHtml(element, PrDomTag_Textarea) ||
        domIsHtml(element, PrDomTag_Select);
}

// The distance that a key scrolls by a line, in CSS pixels. cssom-view-1 leaves it to the user agent.
constexpr float kScrollKeyLine = 40;

// The ways in which a key scrolls.
enum {
    KeyScroll_None,
    KeyScroll_Line,     // a line along the arrow's axis
    KeyScroll_Page,     // a page less a line's overlap
    KeyScroll_End,      // to the start or the end of the range
};
typedef uint8_t KeyScroll;

// Scrolls as the key of `event` does, from the box of `element`. The key scrolls the nearest scroll container that can
// move its way, as far as that container can move: in the element's document, or else in the documents around it, from
// the frame that holds each.
void scrollByKey(HtmlView* v, PrDomElement* element, const PrAppEvent* event) {
    bool shift = (event->modifiers & PrAppModifierFlag_Shift) != 0;
    bool command = (event->modifiers & (PrAppModifierFlag_Control | PrAppModifierFlag_Super)) != 0;
    if ((event->modifiers & PrAppModifierFlag_Alt) != 0)
        return;
    KeyScroll way = KeyScroll_None;
    PrVec2 direction{};
    switch (event->key) {
        case PrAppKey_Up: way = command ? KeyScroll_End : KeyScroll_Line; direction.y = -1; break;
        case PrAppKey_Down: way = command ? KeyScroll_End : KeyScroll_Line; direction.y = 1; break;
        case PrAppKey_Left: way = command ? KeyScroll_None : KeyScroll_Line; direction.x = -1; break;
        case PrAppKey_Right: way = command ? KeyScroll_None : KeyScroll_Line; direction.x = 1; break;
        case PrAppKey_PageUp: way = KeyScroll_Page; direction.y = -1; break;
        case PrAppKey_PageDown: way = KeyScroll_Page; direction.y = 1; break;
        case PrAppKey_Home: way = KeyScroll_End; direction.y = -1; break;
        case PrAppKey_End: way = KeyScroll_End; direction.y = 1; break;
        case ' ': way = command ? KeyScroll_None : KeyScroll_Page; direction.y = shift ? -1 : 1; break;
        default: break;
    }
    if (way == KeyScroll_None)
        return;
    // The nearest container that can move along the key's direction takes the scroll. `room` is how far it can move.
    bool back = direction.x + direction.y < 0;
    PrLayoutScrollFlags axis = direction.x != 0 ? PrLayoutScrollFlag_X : PrLayoutScrollFlag_Y;
    for (PrDomElement* at = element; at != nullptr; at = htmlFrameContainer(at->document)) {
        PrCssPresentation* presentation = presentationOfDocument(v, at->document);
        if (presentation == nullptr)
            return;
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(at));
        PrLayoutScrollInfo info{};
        if (node != kPrLayoutNodeNone && !(prLayoutNodeScrollInfo(tree, node, &info) && info.scrollContainer))
            node = prLayoutNodeScrollContainer(tree, node);
        PrLayoutUnit room{};
        for (;;) {
            if (prLayoutNodeScrollInfo(tree, node, &info) && info.scrollContainer && (info.userAxes & axis) != 0) {
                PrLayoutUnit offset = direction.x != 0 ? info.offset.x : info.offset.y;
                if (back)
                    room = offset - (direction.x != 0 ? info.range.left : info.range.top);
                else
                    room = (direction.x != 0 ? info.range.right : info.range.bottom) - offset;
            }
            if (room > PrLayoutUnit{} || node == kPrLayoutNodeNone)
                break;
            node = prLayoutNodeScrollContainer(tree, node);
        }
        if (room <= PrLayoutUnit{})
            continue;
        PrVec2 port = prLayoutVecToPx(info.portExtent);
        float distance = kScrollKeyLine;
        if (way == KeyScroll_Page && (direction.x != 0 ? port.x : port.y) > 2 * kScrollKeyLine)
            distance = (direction.x != 0 ? port.x : port.y) - kScrollKeyLine;
        if (way == KeyScroll_End || distance > prLayoutUnitToPx(room))
            distance = prLayoutUnitToPx(room);
        prCssPresentationScrollFrom(presentation, node, prLayoutVecFromPx(PrVec2{
            direction.x * distance,
            direction.y * distance,
        }));
        return;
    }
}

// Runs the default actions of a keydown that no listener canceled, and returns what the document did with the key. Tab
// and Shift+Tab move the focus. Enter fires its keypress, then follows a link or activates a button. Space presses a
// button, which its release activates. The keys that scroll do.
PrHtmlViewEventResult keyDefault(HtmlView* v, PrDomElement* target, const PrAppEvent* event) {
    PrDomModifiers modifiers = modifiersOf(event->modifiers);
    bool focused = target == target->document->focused;
    if (event->key == PrAppKey_Tab &&
            (event->modifiers & (PrAppModifierFlag_Control | PrAppModifierFlag_Alt | PrAppModifierFlag_Super)) == 0) {
        bool back = (event->modifiers & PrAppModifierFlag_Shift) != 0;
        bool moved = htmlFocusNavigate(documentOf(v), back ? HtmlFocusDirection_Backward : HtmlFocusDirection_Forward);
        return moved ? PrHtmlViewEventResult_Handled : PrHtmlViewEventResult_FocusLeft;
    }
    if (event->key == PrAppKey_Enter) {
        // Enter types no text, and fires its keypress as a key that does (uievents/#event-type-keypress).
        bool proceed = fireKeypress(v, target, "Enter", '\r');
        if (proceed && focused && (isLink(target) || isButton(target)))
            fireClick(target, modifiers);
        return PrHtmlViewEventResult_Handled;
    }
    if (event->key == ' ' && isButton(target) && focused) {
        releaseSpaced(v);
        v->spaced = target;
        prDomNodeRetain(prDomElementAsNode(target));
        prHtmlDocumentSetActive(target->document, target);
        return PrHtmlViewEventResult_Handled;
    }
    if (!takesScrollKeys(target))
        scrollByKey(v, target, event);
    return PrHtmlViewEventResult_Handled;
}

// Fires the events of a key going down, and runs its default actions if no listener cancels the keydown.
PrHtmlViewEventResult keyDown(HtmlView* v, const PrAppEvent* event) {
    v->typing = false;
    PrDomElement* target = keyTarget(v);
    if (target == nullptr)
        return PrHtmlViewEventResult_Ignored;
    v->keyCode = event->code;
    v->keyModifiers = modifiersOf(event->modifiers);
    if (!isModifierKey(event->key))
        htmlFocusKey(target->document);
    if (!fireKey(target, "keydown", keyInit(v, target, event)))
        return PrHtmlViewEventResult_Canceled;
    // A key that the input method takes types through the composition.
    v->typing = !v->composing;
    return v->composing ? PrHtmlViewEventResult_Handled : keyDefault(v, target, event);
}

// Fires the keyup of a key, and activates the button that Space pressed if no listener cancels the keyup.
PrHtmlViewEventResult keyUp(HtmlView* v, const PrAppEvent* event) {
    v->typing = false;
    PrDomElement* target = keyTarget(v);
    if (target == nullptr)
        return PrHtmlViewEventResult_Ignored;
    bool proceed = fireKey(target, "keyup", keyInit(v, target, event));
    if (event->key == ' ' && v->spaced != nullptr) {
        PrDomElement* spaced = v->spaced;
        prDomNodeRetain(prDomElementAsNode(spaced));
        releaseSpaced(v);
        prHtmlDocumentSetActive(spaced->document, nullptr);
        if (proceed && spaced == spaced->document->focused)
            fireClick(spaced, modifiersOf(event->modifiers));
        prDomNodeRelease(prDomElementAsNode(spaced));
    }
    return proceed ? PrHtmlViewEventResult_Handled : PrHtmlViewEventResult_Canceled;
}

// Fires the keypress of each character of text that a key typed. The text of a key whose keydown was canceled is
// dropped, as is the text of a key held with Control or Meta, which is a shortcut. The text that a composition commits
// fires no keypress. No element takes the text yet.
PrHtmlViewEventResult typed(HtmlView* v, const PrAppEvent* event) {
    if (v->committing) {
        v->committing = false;
        return PrHtmlViewEventResult_Handled;
    }
    if (!v->typing)
        return PrHtmlViewEventResult_Ignored;
    v->typing = false;
    PrDomElement* target = keyTarget(v);
    if (target == nullptr || (v->keyModifiers & (PrDomModifier_Control | PrDomModifier_Meta)) != 0)
        return PrHtmlViewEventResult_Ignored;
    bool proceed = true;
    for (size_t at = 0; at < event->text.size;) {
        uint32_t codePoint = 0;
        uint32_t length = prUtf8DecodeScalar(event->text, at, &codePoint);
        if (codePoint >= 0x20 && codePoint != 0x7f)
            proceed = fireKeypress(v, target, PrStringView{event->text.data + at, length}, codePoint) && proceed;
        at += length;
    }
    return proceed ? PrHtmlViewEventResult_Handled : PrHtmlViewEventResult_Canceled;
}

// Fires a CompositionEvent of `type` at `element`, whose data is `data`. Only compositionstart can be canceled
// (uievents/#events-compositionevents). Returns true if no listener canceled it.
bool fireComposition(PrDomElement* element, PrStringView type, PrStringView data) {
    PrDomUiEventInit init = inputInit(element, type == "compositionstart");
    PrDomCompositionEvent* composition = prDomCompositionEventCreate(type, &init, data);
    bool proceed = dispatchTrusted(prDomNodeAsEventTarget(prDomElementAsNode(element)),
        prDomCompositionEventAsEvent(composition));
    prDomEventRelease(prDomCompositionEventAsEvent(composition));
    return proceed;
}

// Fires the events of an input method's composition (uievents/#events-composition-order). The first update starts the
// composition with compositionstart, which continues even if a listener cancels it
// (uievents/#events-composition-canceling).
PrHtmlViewEventResult compose(HtmlView* v, const PrAppEvent* event) {
    PrDomElement* target = keyTarget(v);
    if (target == nullptr)
        return PrHtmlViewEventResult_Ignored;
    if (event->kind == PrAppEventKind_CompositionUpdate) {
        if (!v->composing) {
            v->composing = true;
            fireComposition(target, "compositionstart", "");
        }
        fireComposition(target, "compositionupdate", event->text);
        return PrHtmlViewEventResult_Handled;
    }
    if (!v->composing)
        return PrHtmlViewEventResult_Ignored;
    v->composing = false;
    v->committing = event->text.size != 0;
    fireComposition(target, "compositionend", event->text);
    return PrHtmlViewEventResult_Handled;
}

// Returns the layout node of the box of `element`, or kPrLayoutNodeNone.
PrLayoutNode boxOf(HtmlView* v, PrDomElement* element) {
    PrCssPresentation* presentation = presentationOfDocument(v, element->document);
    if (presentation == nullptr)
        return kPrLayoutNodeNone;
    return prCssPresentationLayoutNode(presentation, prDomElementAsNode(element));
}

// Returns true if a wheel continues the current sequence: it comes soon after the sequence's last wheel, at the same
// point, and the target keeps the box that it had as the sequence began. A target that leaves the document or loses its
// box ends the sequence, as does a pointer that moves.
bool continuesWheel(HtmlView* v, const PrAppEvent* event) {
    return v->wheelTarget != nullptr && event->time - v->wheelTime <= kPrLayoutWheelSequenceGap / 1000 &&
        event->position.x == v->wheelAt.x && event->position.y == v->wheelAt.y &&
        domIsConnected(prDomElementAsNode(v->wheelTarget)) && v->wheelBox != kPrLayoutNodeNone &&
        boxOf(v, v->wheelTarget) == v->wheelBox;
}

// Fires a WheelEvent at `element`, at `point` in the viewport of the element's document, for a scroll by `delta` in CSS
// pixels, which can be canceled if `cancelable`. Returns true if no listener canceled it.
bool fireWheel(HtmlView* v, PrDomElement* element, PrVec2 point, const PrAppEvent* event, PrVec2 delta, bool cancelable) {
    PrDomWheelEventInit init = prDomWheelEventInitDefault();
    init.mouse.ui = inputInit(element, cancelable);
    init.mouse.screenX = double(event->position.x);
    init.mouse.screenY = double(event->position.y);
    init.mouse.clientX = double(point.x);
    init.mouse.clientY = double(point.y);
    init.mouse.buttons = v->buttons;
    init.mouse.modifiers = modifiersOf(event->modifiers);
    init.deltaX = double(delta.x);
    init.deltaY = double(delta.y);
    init.deltaMode = PrDomDeltaMode_Pixel;
    PrDomWheelEvent* wheel = prDomWheelEventCreate("wheel", &init);
    bool proceed = dispatchTrusted(prDomNodeAsEventTarget(prDomElementAsNode(element)), prDomWheelEventAsEvent(wheel));
    prDomEventRelease(prDomWheelEventAsEvent(wheel));
    return proceed;
}

// Fires the wheel event of a Scroll, and scrolls as its default action unless a listener cancels it or the host
// scrolled already. A wheel sequence keeps the target of its first wheel. The first wheel can be canceled if a listener
// on its path is not passive, and the rest of the sequence can be canceled only if a listener canceled the first. Once
// the first is not canceled, the compositor lets the rest of the sequence scroll without waiting.
PrHtmlViewEventResult wheel(HtmlView* v, const PrAppEvent* event, PrHtmlViewEventFlags flags) {
    bool starts = !continuesWheel(v, event);
    v->wheelTime = event->time;
    v->wheelAt = event->position;
    if (starts) {
        endWheel(v);
        ViewHit hit = hitAt(v, event->position);
        v->wheelTarget = hit.element;
        v->wheelPoint = hit.point;
        if (v->wheelTarget == nullptr)
            return PrHtmlViewEventResult_Ignored;
        prDomNodeRetain(prDomElementAsNode(v->wheelTarget));
        v->wheelBox = boxOf(v, v->wheelTarget);
    }
    // The target's listeners may end the sequence and drop the view's hold, so the event holds the target too.
    PrDomElement* target = v->wheelTarget;
    prDomNodeRetain(prDomElementAsNode(target));
    bool scrolled = (flags & PrHtmlViewEventFlag_Scrolled) != 0;
    bool cancelable = !scrolled && (starts ? domWheelPathBlocks(target) : v->wheelCanceled);
    PrVec2 delta = prAppEventScrollPixels(event, kPrAppScrollLineHeight);
    bool proceed = fireWheel(v, target, v->wheelPoint, event, delta, cancelable);
    if (starts) {
        v->wheelCanceled = !proceed;
        if (proceed && v->compositor != nullptr)
            prLayoutCompositorReleaseWheel(v->compositor);
    }
    // The wheel scrolls from the target's box outward, so the sequence keeps scrolling what it began to scroll.
    if (proceed && !scrolled)
        scrollFrom(v, target, delta);
    prDomNodeRelease(prDomElementAsNode(target));
    return proceed ? PrHtmlViewEventResult_Handled : PrHtmlViewEventResult_Canceled;
}

// What a post to the view carries.
enum {
    ViewPostKind_Event,
    ViewPostKind_Frame,
    ViewPostKind_Traverse,
};
typedef uint8_t ViewPostKind;

// What a thread posted to the view, which a task of the agent applies. An event's text follows the post.
struct ViewPost {
    ViewInbox* inbox;
    ViewPostKind kind;
    PrHtmlViewEventFlags flags;
    int32_t delta;
    PrAppEvent event;
    PrAppFrame frame;
};

void releaseInbox(ViewInbox* inbox) {
    if (inbox->references.fetchSub(1) == 1)
        prMemoryFree(inbox);
}

// Applies a post on the agent's thread, unless its view is gone.
void applyPost(void* PR_NULLABLE context) {
    ViewPost* post = static_cast<ViewPost*>(context);
    if (PrHtmlView* view = post->inbox->view) {
        switch (post->kind) {
            case ViewPostKind_Event:
                post->event.text = PrStringView{reinterpret_cast<const char*>(post + 1), post->event.text.size};
                prHtmlViewEvent(view, &post->event, post->flags);
                break;
            case ViewPostKind_Frame:
                prHtmlViewSetViewport(view, post->frame.extent);
                prHtmlViewSetDisplay(view, &post->frame.display);
                prHtmlViewSetUserPreferences(view, &post->frame.preferences);
                break;
            case ViewPostKind_Traverse:
                prHtmlPageTraverse(post->inbox->page, post->delta);
                break;
        }
    }
    releaseInbox(post->inbox);
    prMemoryFree(post);
}

// Returns a post to `inbox` with room for `size` bytes of text after it.
ViewPost* newPost(ViewInbox* inbox, ViewPostKind kind, size_t size) {
    ViewPost* post = static_cast<ViewPost*>(prMemoryAllocZeroed(sizeof(ViewPost) + size));
    inbox->references.fetchAdd(1);
    post->inbox = inbox;
    post->kind = kind;
    return post;
}

// The view's update after each run of the agent's tasks.
void updateView(void* PR_NULLABLE context) {
    prHtmlViewUpdate(static_cast<PrHtmlView*>(context));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrHtmlView : HtmlView {};

PrHtmlView* prHtmlViewCreate(const PrHtmlViewDesc* desc) {
    PrHtmlView* v = allocZeroed<PrHtmlView>();
    v->page = desc->page;
    v->compositor = desc->compositor;
    v->jobBudget = desc->jobBudget;
    v->canvasFilled = desc->canvasFilled;
    v->extent = desc->page->viewport;
    v->changed = true;
    v->inbox = allocZeroed<ViewInbox>();
    v->inbox->references.store(1);
    v->inbox->view = v;
    v->inbox->page = v->page;
    v->inbox->agent = prHtmlPageAgent(v->page);
    v->inbox->compositor = v->compositor;
    if (v->compositor != nullptr)
        prDomAgentAddUpdate(v->inbox->agent, updateView, v);
    return v;
}

void prHtmlViewDestroy(PrHtmlView* PR_NULLABLE view) {
    if (view == nullptr)
        return;
    if (view->compositor != nullptr)
        prDomAgentRemoveUpdate(view->inbox->agent, updateView, view);
    view->inbox->view = nullptr;
    releaseInbox(view->inbox);
    prHtmlViewWait(view);
    release(view);
    releaseSpaced(view);
    endWheel(view);
    for (uint32_t i = 0; i < view->frames.count && view->compositor != nullptr; ++i)
        prLayoutCompositorRemove(view->compositor, view->frames.data[i].embed);
    view->tracks.reset();
    view->scrolls.reset();
    view->frames.reset();
    view->found.reset();
    view->embeds.reset();
    prMemoryFree(view);
}

void prHtmlViewSetViewport(PrHtmlView* view, PrVec2 extent) {
    if (extent.x != view->extent.x || extent.y != view->extent.y)
        setViewport(view, extent);
}

void prHtmlViewSetDisplay(PrHtmlView* view, const PrAppDisplay* display) {
    if (display->pixelRatio != view->page->display.pixelRatio)
        view->changed = true;
    prHtmlPageSetDisplay(view->page, display);
}

void prHtmlViewSetUserPreferences(PrHtmlView* view, const PrAppUserPreferences* preferences) {
    prHtmlPageSetUserPreferences(view->page, preferences);
}

PrHtmlViewUpdate prHtmlViewUpdate(PrHtmlView* view) {
    PrHtmlViewUpdate update{};
    // The offsets that the compositor scrolled to reach the documents now, and the scenes of the next jobs show them.
    if (view->compositor != nullptr) {
        uint32_t taken = prLayoutCompositorTakeScrolls(view->compositor, applyScroll, view);
        view->hoverDue = view->hoverDue || (view->pointed && taken != view->scrollsTaken);
        view->scrollsTaken = taken;
    }
    PrCssPresentation* presentation = presentationOf(view);
    if (presentation == nullptr || prHtmlPageIsRenderBlocked(view->page) || prCssPresentationIsBusy(presentation))
        return update;
    // The moves since the last update, and the scrolls under the pointer, cause one hover, in the next job.
    if (view->hoverDue)
        hover(view);
    // The documents' own scrolls go to the compositor as they are, with no job, before any scene. The scenes' offsets
    // then include them.
    forwardScrolls(view, presentation, 0);
    for (uint32_t i = 0; i < view->frames.count; ++i) {
        if (PrCssPresentation* inner = frameRender(view, view->frames.data[i]))
            forwardScrolls(view, inner, view->frames.data[i].embed);
    }
    // The completed jobs go to the compositor together: the window's scene, then the scenes of its frames, each sized
    // by the scene around it.
    if (view->posted) {
        view->posted = false;
        update.layout = prLayoutTreeStats(prCssPresentationTree(presentation));
        float ratio = view->page->display.pixelRatio;
        update.committed = commitScene(view, presentation, 0, prAffine2Scale(PrVec2{ratio, ratio}));
        for (uint32_t i = 0; i < view->frames.count; ++i) {
            ViewFrame& frame = view->frames.data[i];
            PrCssPresentation* inner = frame.posted ? frameRender(view, frame) : nullptr;
            frame.posted = false;
            if (inner != nullptr)
                commitScene(view, inner, frame.embed, prAffine2Identity());
        }
        // A new scene can put a box with another cursor under a pointer that has not moved, or change the cursor of the
        // box there.
        if (view->pointed)
            view->cursor = cursorOf(hitAt(view, view->pointer));
    }
    findFrames(view);
    bool current = !view->changed && prCssPresentationIsCurrent(presentation);
    for (uint32_t i = 0; i < view->frames.count && current; ++i) {
        PrCssPresentation* inner = frameRender(view, view->frames.data[i]);
        current = inner == nullptr || prHtmlPageIsRenderBlocked(view->frames.data[i].page) ||
            prCssPresentationIsCurrent(inner);
    }
    if (current)
        return update;
    // Every document's changes go into the tree before any job is posted, so that the jobs run back to back.
    htmlPresentationPrepareJob(presentation, prLayoutVecFromPx(view->extent));
    for (uint32_t i = 0; i < view->frames.count; ++i) {
        ViewFrame& frame = view->frames.data[i];
        PrCssPresentation* inner = frameRender(view, frame);
        frame.posted = inner != nullptr && !prHtmlPageIsRenderBlocked(frame.page);
        if (frame.posted)
            htmlPresentationPrepareJob(inner, prLayoutVecFromPx(frame.extent));
    }
    cssPresentationPostJob(presentation, prLayoutVecFromPx(view->extent));
    update.style = prCssPresentationStats(presentation);
    for (uint32_t i = 0; i < view->frames.count; ++i) {
        const ViewFrame& frame = view->frames.data[i];
        if (frame.posted)
            cssPresentationPostJob(prHtmlPagePresentation(frame.page), prLayoutVecFromPx(frame.extent));
    }
    update.started = true;
    view->posted = true;
    view->changed = false;
    return update;
}

void prHtmlViewWait(PrHtmlView* view) {
    if (PrCssPresentation* presentation = presentationOf(view))
        prCssPresentationWait(presentation);
}

PrColor16 prHtmlViewPaint(PrHtmlView* view, PrDrawList* list) {
    PrCssPresentation* presentation = presentationOf(view);
    if (presentation == nullptr || prHtmlPageIsRenderBlocked(view->page))
        return PrColor16{};
    findFrames(view);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(view->extent));
    return playTree(view, presentation, list, view->canvasFilled);
}

PrAppCursor prHtmlViewCursor(const PrHtmlView* view) {
    return view->cursor;
}

PrHtmlViewEventResult prHtmlViewEvent(PrHtmlView* view, const PrAppEvent* event, PrHtmlViewEventFlags flags) {
    switch (event->kind) {
        case PrAppEventKind_KeyDown:
            return keyDown(view, event);
        case PrAppEventKind_KeyUp:
            return keyUp(view, event);
        case PrAppEventKind_Text:
            return typed(view, event);
        case PrAppEventKind_CompositionUpdate:
        case PrAppEventKind_CompositionEnd:
            return compose(view, event);
        case PrAppEventKind_PointerMove:
            view->pointer = event->position;
            view->pointed = true;
            view->hoverDue = true;
            return PrHtmlViewEventResult_Handled;
        case PrAppEventKind_Scroll:
            return wheel(view, event, flags);
        case PrAppEventKind_PointerDown: {
            view->pointer = event->position;
            view->pointed = true;
            ViewHit hit = hitAt(view, event->position);
            PrDomElement* element = hit.element;
            if (element == nullptr)
                return PrHtmlViewEventResult_Ignored;
            int16_t button = mouseButtonOf(event->button);
            view->buttons = uint16_t(view->buttons | buttonBit(button));
            designate(view, element, prHtmlDocumentSetActive);
            // A press that no listener cancels moves the focus (html/#focus-processing-model).
            if (fireMouse(view, "mousedown", element, hit.point, event))
                htmlFocusPress(element);
            if (button == 0) {
                release(view);
                view->pressed = element;
                prDomNodeRetain(prDomElementAsNode(element));
            }
            return PrHtmlViewEventResult_Handled;
        }
        case PrAppEventKind_PointerUp: {
            view->pointer = event->position;
            view->pointed = true;
            int16_t button = mouseButtonOf(event->button);
            view->buttons = uint16_t(view->buttons & ~buttonBit(button));
            designate(view, nullptr, prHtmlDocumentSetActive);
            ViewHit hit = hitAt(view, event->position);
            PrDomElement* element = hit.element;
            if (element != nullptr) {
                fireMouse(view, "mouseup", element, hit.point, event);
                // pointerevents4/#click: a click goes to the nearest element that contained both the press and the
                // release.
                if (button == 0 && view->pressed != nullptr) {
                    PrDomNode* common = prDomElementAsNode(element);
                    while (common != nullptr && !prDomNodeContains(common, prDomElementAsNode(view->pressed)))
                        common = prDomNodeParentNode(common);
                    PrDomElement* clicked = common != nullptr ? prDomElementQuery(prDomNodeAsEventTarget(common)) : nullptr;
                    if (clicked != nullptr)
                        fireMouse(view, "click", clicked, hit.point, event);
                }
            }
            if (button == 0)
                release(view);
            return element != nullptr ? PrHtmlViewEventResult_Handled : PrHtmlViewEventResult_Ignored;
        }
        default:
            return PrHtmlViewEventResult_Ignored;
    }
}

void prHtmlViewPostEvent(PrHtmlView* view, const PrAppEvent* event) {
    PrHtmlViewEventFlags flags = 0;
    PrLayoutCompositor* compositor = view->inbox->compositor;
    if (event->kind == PrAppEventKind_Scroll && compositor != nullptr) {
        PrLayoutVec at = prLayoutVecFromPx(event->position);
        if (!prLayoutCompositorWheelBlocks(compositor, at)) {
            prLayoutCompositorScrollAt(compositor, at,
                prLayoutVecFromPx(prAppEventScrollPixels(event, kPrAppScrollLineHeight)));
            flags = PrHtmlViewEventFlag_Scrolled;
        }
    }
    size_t size = event->text.data != nullptr ? event->text.size : 0;
    ViewPost* post = newPost(view->inbox, ViewPostKind_Event, size);
    post->event = *event;
    post->event.text = PrStringView{nullptr, size};
    post->flags = flags;
    if (size != 0)
        memCopy(post + 1, event->text.data, size);
    prDomAgentPost(view->inbox->agent, applyPost, post);
}

void prHtmlViewPostFrame(PrHtmlView* view, const PrAppFrame* frame) {
    ViewPost* post = newPost(view->inbox, ViewPostKind_Frame, 0);
    post->frame = *frame;
    prDomAgentPost(view->inbox->agent, applyPost, post);
}

void prHtmlViewPostTraverse(PrHtmlView* view, int32_t delta) {
    ViewPost* post = newPost(view->inbox, ViewPostKind_Traverse, 0);
    post->delta = delta;
    prDomAgentPost(view->inbox->agent, applyPost, post);
}
