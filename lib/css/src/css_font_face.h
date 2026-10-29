// Font faces
//
// css-font-loading-3 - https://drafts.csswg.org/css-font-loading-3/

#pragma once

#include "pr/array.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/css.h"
#include "pr/dom.h"
#include "pr/font_set.h"
#include "pr/string_view.h"

#include "css/src/css_object.h"
#include "dom/src/dom_event.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssFontFaceSet;
struct CssFontLoad;
struct CssRoot;

// A source that a document fetched for its faces: its URL, and the font that its bytes make. Faces that load from one
// URL share one source, and so share one font. A face whose source the document has decoded loads at once. A face whose
// source is still being fetched waits for that fetch. A document keeps the sources that faces use. It also keeps other
// sources up to a limit on their total bytes, and evicts the least recently used first.
struct CssFontSource {
    // References held by the document, by the fetch while it runs, and by each face that loaded from it.
    uint32_t references;

    PrString url;
    const PrFont* PR_NULLABLE font;         // font made from the bytes, without any face's derivation, or null
    bool settled;                           // true once the response has arrived
    size_t bytes;                           // size of the decoded font, counted by the cache limit; 0 on failure
    CssRoot* PR_NULLABLE cache;             // the document whose cache holds the source, or null once it is evicted
    uint64_t used;                          // the last time a load used the source, by the document's count of uses
    Array<CssFontLoad* PR_NONNULL> waiting; // the loads that wait for the response
};

// A FontFace (css-font-loading-3/#fontface-interface): the descriptors of a font, and the font they name. A script and
// a font face set share a face by reference.
//
// A CSS-connected face (css-font-loading-3/#css-connected) belongs to its @font-face rule. The rule holds a reference
// to the face, and the face names the rule without holding it. The face's descriptors are the rule's block until the
// rule is destroyed and gives the face a copy.
struct CssFontFace {
    // The descriptors that parsed, as an @font-face block. Used by a face with no rule.
    CssDeclarationBlock descriptors;

    PrCssRule* PR_NULLABLE rule;         // the @font-face rule of a CSS-connected face

    // The sets that hold the face. A set removes itself from this list when the face leaves it.
    Array<PrCssFontFaceSet* PR_NONNULL> sets;

    // The font that loaded, without the derivation that the descriptors give. Null until the face has loaded.
    const PrFont* PR_NULLABLE font;

    CssFontSource* PR_NULLABLE source;  // the source that the font loaded from; the face holds a reference to it

    // The document that a loading face loads through. The load holds a reference to the document.
    PrDomDocument* PR_NULLABLE loading;

    uint32_t references;
    PrCssFontFaceStatus status;
    bool fromData;                     // true if created from font data rather than from a source to fetch
    bool unparsed;                     // true if a descriptor or the source did not parse
};

// A document's FontFaceSet (css-font-loading-3/#fontfaceset), which the document owns. A reference to the set is a
// reference to its document. The set's faces are the CSS-connected faces of the document's style sheets, followed by
// the faces added to the set, in the order they were added.
struct CssFontFaceSet : DomDocumentTarget {
    Array<PrCssFontFace* PR_NONNULL> faces;     // the faces added to the set; the set holds a reference to each

    // The faces that are loading, have loaded and have failed since the set last switched to loaded
    // (css-font-loading-3/#dom-fontfaceset-loadingfonts-slot). The faces that the set holds keep them alive.
    Array<PrCssFontFace* PR_NONNULL> loading;
    Array<PrCssFontFace* PR_NONNULL> loaded;
    Array<PrCssFontFace* PR_NONNULL> failed;

    PrCssFontFaceSetLoadStatus status;
};

// Creates a face (css-font-loading-3/#font-face-constructor) whose source is the text in `source`, or the font in
// `data` if `data` is not null. `compress` decompresses `data` if it is compressed. `descriptors` holds a value for
// each PrCssFontFaceDescriptor after the family. A null view keeps the descriptor's default. The caller holds the
// face's only reference.
PrCssFontFace* cssFontFaceCreate(PrStringView family, PrStringView source, PrByteView data,
                                 PrCompressCodec* PR_NULLABLE compress, const PrStringView* PR_NULLABLE descriptors);

// Adds or releases a reference. The last release frees the face.
void cssFontFaceRetain(PrCssFontFace* face);
void cssFontFaceRelease(PrCssFontFace* PR_NULLABLE face);

// Returns the serialization of `descriptor`, or the empty string if the descriptor did not parse.
PrString cssFontFaceDescriptor(const PrCssFontFace* face, PrCssFontFaceDescriptor descriptor);

// Sets `descriptor` to `value`. Returns Syntax, and leaves the descriptor unchanged, if the descriptor does not accept
// `value`.
PrStatus cssFontFaceSetDescriptor(PrCssFontFace* face, PrCssFontFaceDescriptor descriptor,
                                  PrStringView value);

// Loads the font of an unloaded face from the first of its sources whose bytes make a font
// (css-font-loading-3/#dom-fontface-load). The document's host fetches each source that the document has not already
// fetched (CssFontSource), and the host's codec decompresses a compressed font. The load runs in a separate task, or
// immediately if the document has no host. The face is Loading until the task runs. The task holds a reference to the
// face and to the document.
void cssFontFaceLoad(PrCssFontFace* face, PrDomDocument* document);

// Describes the face in the terms of a font set (pr/font_set.h). Writes its family into `family`, and into `out` its
// ranges of weight, width and slope and the derivation that its size-adjust and metric overrides make
// (css-fonts-4/#font-face-rule). Appends the code point ranges that its unicode-range descriptor names to `ranges`. An
// auto range matches as normal. Returns false for a face with no family.
bool cssFontFaceDescribe(const PrCssFontFace* face, PrString* family, PrFontFaceDescriptor* out,
                         Array<PrFontCodePointRange>* ranges);

// Returns the CSS-connected face of an @font-face rule, and creates the face on the first request.
PrCssFontFace* cssFontFaceOfRule(PrCssRule* rule);

// Gives a CSS-connected face a copy of its rule's descriptors. Called when the rule is destroyed.
void cssFontFaceDisconnect(PrCssFontFace* face);

// Creates a set for `document`. While the document is loading, the set is pending on the environment, and its status is
// Loading.
PrCssFontFaceSet* cssFontFaceSetCreate(PrDomDocument* document);

// Frees the set, and releases the faces added to it.
void cssFontFaceSetDestroy(PrCssFontFaceSet* PR_NULLABLE set);

// Appends the set's faces to `out`, in order (css-font-loading-3/#fontfaceset-set-entries).
void cssFontFaceSetFaces(const PrCssFontFaceSet* set, Array<PrCssFontFace* PR_NONNULL>* out);

// Returns true if the set holds `face`.
bool cssFontFaceSetHas(const PrCssFontFaceSet* set, const PrCssFontFace* face);

// Adds `face` and retains it, unless the set already holds it (css-font-loading-3/#dom-fontfaceset-add). Returns
// NotAllowed for a CSS-connected face.
PrStatus cssFontFaceSetAdd(PrCssFontFaceSet* set, PrCssFontFace* face);

// Removes `face` and releases it. If `face` was the last loading face, switches the set to loaded. Returns false for a
// face that the set does not hold, or for a CSS-connected face.
bool cssFontFaceSetDelete(PrCssFontFaceSet* set, PrCssFontFace* face);

// Removes the faces added to the set and keeps the CSS-connected ones. If a loading face was removed, switches the set
// to loaded.
void cssFontFaceSetClear(PrCssFontFaceSet* set);

// Appends to `out` the faces in the set whose family is one that `font` names
// (css-font-loading-3/#find-the-matching-font-faces). Returns Syntax if `font` is not a value of the font property.
PrStatus cssFontFaceSetMatch(const PrCssFontFaceSet* set, PrStringView font,
                             Array<PrCssFontFace* PR_NONNULL>* out);

// Appends the document's font faces to `out`, in order. These are the faces of its FontFaceSet. The document has them
// whether or not the set's object has been created.
void cssDocumentFontFaces(const PrDomDocument* document, Array<PrCssFontFace* PR_NONNULL>* out);

// Returns the font of the face's first source that is not cached as failed, if the document has decoded that source, so
// that the face can be set at once. Returns null if the face is not unloaded, or if that source is not decoded.
const PrFont* PR_NULLABLE cssFontFaceCachedFont(const PrCssFontFace* face, PrDomDocument* document);

// Releases a document's sources.
void cssFontSourcesReset(Array<CssFontSource* PR_NONNULL>* sources);

// Called when the document's style sheets or rules change, which may change its font source. If the document's set has
// faces that are loading, loaded or failed, a task removes the faces that left the source. The task switches the set to
// loaded if a loading face left, whether or not the document is rendered.
void cssFontsSourceChanged(PrDomDocument* document);

// Removes the faces that are not among `faces` from the loading, loaded and failed faces of the document's set. `faces`
// are the document's font faces, as cssDocumentFontFaces lists them. Called when the document's faces are listed after
// its style sheets or rules change, which may be during a rendering update. The set switches to loaded at the next
// cssFontsReadinessChanged, which fires the set's events.
void cssFontsSourceSynced(PrDomDocument* document, const Array<PrCssFontFace* PR_NONNULL>& faces);

// Called when the document's readiness changes, which determines whether its set is pending on the environment
// (css-font-loading-3/#fontfaceset-pending-on-the-environment).
void cssFontsReadinessChanged(PrDomDocument* document);

// The fontfaces of a FontFaceSetLoadEvent, which are its data (DomEvent::extra). The event holds a reference to each.
struct CssFontFacesData : DomEventExtra {
    Array<PrCssFontFace* PR_NONNULL> faces;
};

// Gives a FontFaceSetLoadEvent `count` faces, taking a reference to each.
void cssFontFaceSetLoadEventSetFaces(PrDomEvent* event, PrCssFontFace* PR_NONNULL const* PR_NULLABLE faces,
                                     uint32_t count);

} // namespace pr_impl

// C opaque types
struct PrCssFontFace : pr_impl::CssFontFace {};
struct PrCssFontFaceSet : pr_impl::CssFontFaceSet {};

PR_ASSUME_NONNULL_END
