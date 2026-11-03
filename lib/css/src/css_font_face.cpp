#include "css/src/css_font_face.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/string_array.h"
#include "pr/string_ops.h"
#include "pr/url.h"

#include "css/src/css_at_rule.h"
#include "css/src/css_math.h"
#include "css/src/css_parser.h"
#include "css/src/css_root.h"
#include "css/src/css_serialize.h"
#include "css/src/css_style.h"
#include "css/src/css_syntax.h"
#include "dom/src/dom_node.h"

namespace pr_impl {

// A load of a face's sources. It holds a reference to the face and to the document.
struct CssFontLoad {
    PrCssFontFace* face;
    PrDomDocument* document;
    Array<PrString> requests;       // URLs of the sources, in order
    uint32_t next;                  // index of the first source not yet tried
};

namespace {

// The descriptor that each attribute of a face reflects, and the descriptor's value if the constructor is not given one
// (css-font-loading-3/#dictdef-fontfacedescriptors).
struct Reflected {
    PrCssDescriptor descriptor;
    PrStringView initial;
};

constexpr Reflected kReflected[PrCssFontFaceDescriptor_Count] = {
    {PrCssDescriptor_FontFaceFontFamily, ""},
    {PrCssDescriptor_FontFaceFontStyle, "normal"},
    {PrCssDescriptor_FontFaceFontWeight, "normal"},
    {PrCssDescriptor_FontFaceFontWidth, "normal"},
    {PrCssDescriptor_FontFaceUnicodeRange, "U+0-10FFFF"},
    {PrCssDescriptor_FontFaceFontFeatureSettings, "normal"},
    {PrCssDescriptor_FontFaceFontVariationSettings, "normal"},
    {PrCssDescriptor_FontFaceFontDisplay, "auto"},
    {PrCssDescriptor_FontFaceAscentOverride, "normal"},
    {PrCssDescriptor_FontFaceDescentOverride, "normal"},
    {PrCssDescriptor_FontFaceLineGapOverride, "normal"},
};

CssDeclaredName reflected(uint32_t descriptor) {
    return cssDescriptorDeclaredName(kReflected[descriptor].descriptor);
}

// Returns the font of the first face of the file in `bytes`, or null if the bytes are not a font that the engine reads.
// `compress` decompresses a WOFF file. If `compress` is null, a WOFF file is not read.
const PrFont* PR_NULLABLE fontOf(PrByteView bytes, PrCompressCodec* PR_NULLABLE compress) {
    bool woff = bytes.size >= 4 && bytes.data[0] == 'w' && bytes.data[1] == 'O' && bytes.data[2] == 'F';
    const PrFont* font = nullptr;
    if (!woff)
        prFontCreate(bytes, 0, nullptr, &font);
    else if (compress != nullptr)
        prFontCreateWoff(bytes, 0, nullptr, compress, &font);
    return font;
}

// Appends the family names in a font-family value, in the order the list is written. Each name is unquoted and has its
// escapes resolved. A run of identifiers is joined with single spaces.
void familyNames(PrStringView list, Array<PrString>* out) {
    CssParser parser;
    cssParserInit(&parser);
    cssTokenize(&parser.tokens, &parser.scratch, list);
    const CssTokenArray* tokens = &parser.tokens;
    PrTextBuffer name = {};
    bool any = false;
    for (uint32_t i = 0; i <= cssTokenCount(tokens); ++i) {
        CssTokenKind kind = i < cssTokenCount(tokens) ? cssTokenKind(tokens, i) : CssTokenKind_Eof;
        if (kind == CssTokenKind_Comma || kind == CssTokenKind_Eof) {
            if (any)
                out->push(prStringCopy(prTextBufferView(&name)));
            name.size = 0;
            any = false;
            continue;
        }
        if (kind != CssTokenKind_Ident && kind != CssTokenKind_String)
            continue;
        if (any)
            prTextBufferAppendChar(&name, ' ');
        cssValueAppend(&name, cssTokenValueText(tokens, i));
        any = true;
    }
    prTextBufferReset(&name);
    cssParserReset(&parser);
}

void resetNames(Array<PrString>* names) {
    stringsReset(names);
}

// Returns the face's descriptors: the block of its rule if the face is CSS-connected, and its own block otherwise.
const CssDeclarationBlock* blockOf(const PrCssFontFace* face) {
    return face->rule != nullptr ? &face->rule->data->style : &face->descriptors;
}

// Returns the face's descriptors for modification.
CssDeclarationBlock* writableBlockOf(PrCssFontFace* face) {
    return face->rule != nullptr ? &cssRuleWritable(face->rule)->style : &face->descriptors;
}

// Returns the family that the face names, unquoted, or the null string if it names none.
PrString faceFamily(const PrCssFontFace* face) {
    PrTextBuffer text = {};
    cssSerializePropertyValue(&text, blockOf(face), cssDescriptorDeclaredName(PrCssDescriptor_FontFaceFontFamily));
    Array<PrString> names;
    familyNames(prTextBufferView(&text), &names);
    prTextBufferReset(&text);
    PrString family = names.count == 1 ? prStringClone(&names.data[0]) : prStringNull();
    resetNames(&names);
    return family;
}

// Returns the URL that the face's relative url()s resolve against (css-values-4/#relative-urls). This is the URL of the
// style sheet that holds the face's rule. It is the document's base URL if the sheet has no URL of its own, as with a
// sheet in a style element, and for a face with no rule.
PrUrl* PR_NULLABLE baseOf(const PrCssFontFace* face, PrDomDocument* document) {
    const PrCssStyleSheet* sheet = face->rule != nullptr ? face->rule->sheet : nullptr;
    if (sheet != nullptr && !prStringIsNull(&sheet->href)) {
        PrUrl* url = nullptr;
        if (prUrlCreate(prStringView(&sheet->href), nullptr, PrCharset_Utf8, &url) == PrStatus_Ok)
            return url;
        prUrlDestroy(url);
    }
    return domDocumentBaseUrl(document);
}

// Appends the url()s that the face's src names, in order, each resolved and serialized as a request.
void sourceRequests(PrCssFontFace* face, PrDomDocument* document, Array<PrString>* out) {
    PrTextBuffer text = {};
    cssSerializePropertyValue(&text, blockOf(face), cssDescriptorDeclaredName(PrCssDescriptor_FontFaceSrc));
    PrStringView src = prTextBufferView(&text);
    PrUrl* base = baseOf(face, document);
    CssParser parser;
    cssParserInit(&parser);
    cssTokenize(&parser.tokens, &parser.scratch, src);
    const CssTokenArray* tokens = &parser.tokens;
    for (uint32_t i = 0; i < cssTokenCount(tokens); ++i) {
        // A url() serializes as the function with a string argument.
        CssTokenKind kind = cssTokenKind(tokens, i);
        PrStringView reference = {};
        if (kind == CssTokenKind_Url)
            reference = cssTokenValueText(tokens, i);
        else if (kind == CssTokenKind_Function && prAsciiEqualsFold(cssTokenValueText(tokens, i), "url") &&
                i + 1 < cssTokenCount(tokens) && cssTokenKind(tokens, i + 1) == CssTokenKind_String)
            reference = cssTokenValueText(tokens, i + 1);
        else
            continue;
        PrTextBuffer resolved = {};
        cssValueAppend(&resolved, reference);
        PrUrl* url = nullptr;
        if (prUrlCreate(prTextBufferView(&resolved), base, PrCharset_Utf8, &url) == PrStatus_Ok)
            out->push(prUrlSerialize(url, PrUrlSerialization_ExcludeFragment));
        prUrlDestroy(url);
        prTextBufferReset(&resolved);
    }
    cssParserReset(&parser);
    prUrlDestroy(base);
    prTextBufferReset(&text);
}

// Appends the CSS-connected faces of the rules of `list`, and of the lists nested in them, in order. A face in a
// conditional group rule is left out, because the cascade decides whether its condition holds.
void connectedRules(PrCssRuleList* list, Array<PrCssFontFace* PR_NONNULL>* out);

void connectedFaces(PrCssStyleSheet* sheet, Array<PrCssFontFace* PR_NONNULL>* out) {
    if (!sheet->disabled)
        connectedRules(&sheet->cssRules, out);
}

void connectedRules(PrCssRuleList* list, Array<PrCssFontFace* PR_NONNULL>* out) {
    const Array<CssRuleData* PR_NONNULL>& rules = cssRuleListData(list);
    for (uint32_t i = 0; i < rules.count; ++i) {
        const CssRuleData* data = rules.data[i];
        if (data->kind != CssRuleDataKind_At)
            continue;
        if (data->atRule == CssAtRule_FontFace) {
            out->push(cssFontFaceOfRule(cssRuleListItem(list, i)));
        } else if (data->atRule == CssAtRule_Import && data->imported != nullptr) {
            connectedFaces(cssImportRuleSheet(cssRuleListItem(list, i)), out);
        } else if (data->atRule == CssAtRule_Layer && data->block) {
            connectedRules(&cssRuleListItem(list, i)->cssRules, out);
        }
    }
}

// Notifies the document's font callback of the set's status. The callback is notified of each change to the status of a
// face and of its set.
void tellSetStatus(PrCssFontFaceSet* set) {
    const CssRoot* style = set->document->style;
    if (style != nullptr && style->fontCallback.setStatus != nullptr)
        style->fontCallback.setStatus(style->fontCallback.context, set->document, set);
}

// Fires the font load event `type` at the set (css-font-loading-3/#fire-a-font-load-event), naming `faces`.
void fireLoadEvent(PrCssFontFaceSet* set, PrStringView type, const Array<PrCssFontFace* PR_NONNULL>* PR_NULLABLE faces) {
    PrCssFontFaceSetLoadEvent* event =
        domEventCreate<PrCssFontFaceSetLoadEvent>(type, prDomEventInitDefault(), PrDomEventKind_FontFaceSetLoad);
    event->trusted = true;
    if (faces != nullptr)
        cssFontFaceSetLoadEventSetFaces(event, faces->data, faces->count);
    else
        cssFontFaceSetLoadEventSetFaces(event, nullptr, 0);
    domEventDispatch(set, event);
    domEventRelease(event);
}

// The task that fires `loading` at a set. The task holds a reference to the set's document.
void fireLoading(void* data) {
    PrCssFontFaceSet* set = static_cast<PrCssFontFaceSet*>(data);
    PrDomDocument* document = set->document;
    fireLoadEvent(set, "loading", nullptr);
    domNodeRelease(document);
}

// Called when a face of the set starts loading. The set switches to loading if it was loaded
// (css-font-loading-3/#fontfaceset-pending-on-the-environment).
void faceLoading(PrCssFontFaceSet* set, PrCssFontFace* face) {
    if (set->loading.count == 0) {
        if (set->status != PrCssFontFaceSetLoadStatus_Loading) {
            set->status = PrCssFontFaceSetLoadStatus_Loading;
            tellSetStatus(set);
        }
        if (const DomDocumentHost* host = set->document->host) {
            domNodeRetain(set->document);
            host->queueTask(host->context, fireLoading, set, [](void* data) {
                domNodeRelease(static_cast<PrCssFontFaceSet*>(data)->document);
            });
        }
    }
    set->loading.push(face);
}

// Returns true if the set is pending on the environment (css-font-loading-3/#fontfaceset-pending-on-the-environment):
// the document has not finished loading, or its presentation has a layout to make that may request a font.
bool pendingOnEnvironment(const PrCssFontFaceSet* set) {
    if (set->document->readiness != DomReadiness_Complete)
        return true;
    const CssRoot* style = cssDocumentRoot(set->document);
    return style->presentationHooks != nullptr && style->presentationHooks->awaitsFontRequests(style->presentation);
}

// Switches the set to loaded (css-font-loading-3/#switch-the-fontfaceset-to-loaded) once no face is loading and the
// environment is not pending.
void switchToLoaded(PrCssFontFaceSet* set) {
    if (set->status == PrCssFontFaceSetLoadStatus_Loaded || set->loading.count != 0 || pendingOnEnvironment(set))
        return;
    set->status = PrCssFontFaceSetLoadStatus_Loaded;
    Array<PrCssFontFace* PR_NONNULL> loaded = set->loaded;
    Array<PrCssFontFace* PR_NONNULL> failed = set->failed;
    set->loaded = {};
    set->failed = {};
    fireLoadEvent(set, "loadingdone", &loaded);
    if (failed.count != 0)
        fireLoadEvent(set, "loadingerror", &failed);
    loaded.reset();
    failed.reset();
    tellSetStatus(set);
}

// Called when a face of the set settles. The face moves to the loaded or failed faces. The set switches to loaded when
// its last loading face settles.
void faceSettled(PrCssFontFaceSet* set, PrCssFontFace* face) {
    if (!set->loading.removeFirst(face))
        return;
    (face->status == PrCssFontFaceStatus_Loaded ? set->loaded : set->failed).push(face);
    switchToLoaded(set);
}

// Removes the face from the set's loading, loaded and failed faces when the face leaves the set. Returns true if the
// face was loading.
bool dropFace(PrCssFontFaceSet* set, PrCssFontFace* face) {
    set->loaded.removeFirst(face);
    set->failed.removeFirst(face);
    return set->loading.removeFirst(face);
}

// Removes the face from the set's lists when the face leaves the set. If it was the last loading face, switches the set
// to loaded (css-font-loading-3/#dom-fontfaceset-delete).
void forget(PrCssFontFaceSet* set, PrCssFontFace* face) {
    face->sets.removeFirst(set);
    if (dropFace(set, face))
        switchToLoaded(set);
}

// Appends the sets that hold the face now: the sets it was added to, and its document's set while the face is
// CSS-connected to a rule of the document's style sheets. A set that the face has left sees nothing of its load.
void setsOf(PrCssFontFace* face, PrDomDocument* document, Array<PrCssFontFaceSet* PR_NONNULL>* out) {
    out->append(face->sets.data, face->sets.count);
    PrCssFontFaceSet* fonts = document->style != nullptr ? document->style->fonts : nullptr;
    if (face->rule != nullptr && fonts != nullptr && fonts->faces.find(face) == kArrayMissing &&
            cssFontFaceSetHas(fonts, face))
        out->push(fonts);
}

// Sets the face's status and notifies the document's font callback, then the face's sets
// (css-font-loading-3/#font-face-load).
void setStatus(PrCssFontFace* face, PrDomDocument* document, PrCssFontFaceStatus status) {
    face->status = status;
    ++cssDocumentRoot(document)->fontsVersion;
    face->loading = status == PrCssFontFaceStatus_Loading ? document : nullptr;
    const CssRoot* style = document->style;
    if (style != nullptr && style->fontCallback.faceStatus != nullptr)
        style->fontCallback.faceStatus(style->fontCallback.context, document, face);
    Array<PrCssFontFaceSet* PR_NONNULL> sets;
    setsOf(face, document, &sets);
    for (uint32_t i = 0; i < sets.count; ++i) {
        if (status == PrCssFontFaceStatus_Loading)
            faceLoading(sets.data[i], face);
        else
            faceSettled(sets.data[i], face);
    }
    sets.reset();
}

// Limits on the sources that no face uses and that a document keeps for reuse: their total bytes and their number.
constexpr size_t kMaxUnusedSourceBytes = 32u << 20;
constexpr uint32_t kMaxUnusedSources = 64;

// Releases what a load held.
void dropLoad(void* PR_NULLABLE data) {
    CssFontLoad* load = static_cast<CssFontLoad*>(data);
    cssFontFaceRelease(load->face);
    domNodeRelease(load->document);
    stringsReset(&load->requests);
    prMemoryFree(load);
}

void trimSources(CssRoot* style);

// Releases a reference to the source. If the document's cache is left as its only owner, the cache is trimmed to its
// limits. This keeps the sources that no face uses within the limits as they become unused.
void sourceRelease(CssFontSource* PR_NULLABLE source) {
    if (source == nullptr)
        return;
    uint32_t left = --source->references;
    // The trim may evict and free this source, so nothing reads the source afterward.
    if (left == 1 && source->cache != nullptr && source->settled) {
        trimSources(source->cache);
        return;
    }
    if (left != 0)
        return;
    PR_ASSERT(source->waiting.count == 0);
    prStringReset(&source->url);
    prFontRelease(source->font);
    source->waiting.reset();
    prMemoryFree(source);
}

// Returns the document's source for `url`, or null.
CssFontSource* PR_NULLABLE findSource(const CssRoot* style, PrStringView url) {
    for (uint32_t i = 0; i < style->fontSources.count; ++i) {
        if (prStringView(&style->fontSources.data[i]->url) == url)
            return style->fontSources.data[i];
    }
    return nullptr;
}

// Frees the least recently used of the document's settled sources that no face uses, until those sources are within the
// limits on their bytes and their number.
void trimSources(CssRoot* style) {
    for (;;) {
        size_t bytes = 0;
        uint32_t unused = 0;
        uint32_t oldest = kArrayMissing;
        for (uint32_t i = 0; i < style->fontSources.count; ++i) {
            const CssFontSource* source = style->fontSources.data[i];
            if (!source->settled || source->references != 1)
                continue;
            bytes += source->bytes;
            ++unused;
            if (oldest == kArrayMissing || source->used < style->fontSources.data[oldest]->used)
                oldest = i;
        }
        if (bytes <= kMaxUnusedSourceBytes && unused <= kMaxUnusedSources)
            return;
        CssFontSource* evicted = style->fontSources.data[oldest];
        style->fontSources.erase(oldest);
        evicted->cache = nullptr;
        sourceRelease(evicted);
    }
}

// Settles the load's face with the source's font, which the face then shares.
void adopt(CssFontLoad* load, CssFontSource* source) {
    PrCssFontFace* face = load->face;
    prFontRetain(source->font);
    prFontRelease(face->font);
    face->font = source->font;
    ++source->references;
    sourceRelease(face->source);
    face->source = source;
    setStatus(face, load->document, PrCssFontFaceStatus_Loaded);
    dropLoad(load);
}

void fetchNext(CssFontLoad* load);

// A fetch of a source. It holds a reference to the source and to the document.
struct SourceFetch {
    CssFontSource* source;
    PrDomDocument* document;
};

void dropFetch(void* PR_NULLABLE data) {
    SourceFetch* fetch = static_cast<SourceFetch*>(data);
    CssFontSource* source = fetch->source;
    // Nothing will settle the source. It is removed from the document, and the loads that waited for it end.
    CssRoot* style = cssDocumentRoot(fetch->document);
    if (style->fontSources.removeFirst(source)) {
        source->cache = nullptr;
        sourceRelease(source);
    }
    Array<CssFontLoad*> waiting = source->waiting;
    source->waiting = {};
    for (uint32_t i = 0; i < waiting.count; ++i)
        dropLoad(waiting.data[i]);
    waiting.reset();
    sourceRelease(source);
    domNodeRelease(fetch->document);
    prMemoryFree(fetch);
}

// Called when a source's response arrives. If the bytes make a font, the font settles every load that waited for the
// source. Otherwise each waiting load tries its next source.
void sourceFetched(void* PR_NULLABLE data, const DomFetchResult* result) {
    SourceFetch* fetch = static_cast<SourceFetch*>(data);
    CssFontSource* source = fetch->source;
    PrByteView bytes{reinterpret_cast<const uint8_t*>(result->body.data), result->body.size};
    const DomDocumentHost* host = fetch->document->host;
    source->font = result->status == PrStatus_Ok && host != nullptr ? fontOf(bytes, host->compress) : nullptr;
    source->bytes = source->font != nullptr ? prFontData(source->font).size : 0;
    source->settled = true;
    Array<CssFontLoad*> waiting = source->waiting;
    source->waiting = {};
    for (uint32_t i = 0; i < waiting.count; ++i) {
        if (source->font != nullptr)
            adopt(waiting.data[i], source);
        else
            fetchNext(waiting.data[i]);
    }
    waiting.reset();
    // The fetch releases its reference last, so that a source that no load took is trimmed like any other unused
    // source.
    sourceRelease(source);
    domNodeRelease(fetch->document);
    prMemoryFree(fetch);
}

// Tries the load's sources in order, starting from the next one. A source that the document has decoded settles the
// load at once. A source that failed moves on to the next. A source that is being fetched makes the load wait for it.
// Fails the face if no source is left.
void fetchNext(CssFontLoad* load) {
    PrDomDocument* document = load->document;
    const DomDocumentHost* host = document->host;
    CssRoot* style = cssDocumentRoot(document);
    for (;;) {
        if (host == nullptr || load->next == load->requests.count) {
            setStatus(load->face, document, PrCssFontFaceStatus_Error);
            dropLoad(load);
            return;
        }
        PrStringView url = prStringView(&load->requests.data[load->next++]);
        CssFontSource* source = findSource(style, url);
        if (source == nullptr) {
            source = allocZeroed<CssFontSource>();
            source->references = 2;
            source->url = prStringCopy(url);
            source->waiting.push(load);
            source->cache = style;
            style->fontSources.push(source);
            source->used = ++style->fontSourceUses;
            SourceFetch* fetch = allocZeroed<SourceFetch>();
            fetch->source = source;
            fetch->document = document;
            domNodeRetain(document);
            PrString request = prStringClone(&source->url);
            // A font is fetched with CORS (css-fonts-4/#font-fetching-requirements).
            DomFetchRequest asked{
                prStringView(&request), DomFetchDestination_Font, DomCorsSettings_Anonymous, DomFetchInitiator_Css,
            };
            host->fetch(host->context, &asked, sourceFetched, fetch, dropFetch);
            prStringReset(&request);
            return;
        }
        source->used = ++style->fontSourceUses;
        if (!source->settled) {
            source->waiting.push(load);
            return;
        }
        if (source->font != nullptr) {
            adopt(load, source);
            return;
        }
    }
}

// The task that starts fetching a face's sources.
void startLoad(void* data) {
    CssFontLoad* load = static_cast<CssFontLoad*>(data);
    sourceRequests(load->face, load->document, &load->requests);
    fetchNext(load);
}

// Removes from the set's loading, loaded and failed faces the faces that are not among `faces`, the document's font
// faces. Returns true if a loading face was removed.
bool dropLeft(PrCssFontFaceSet* set, const Array<PrCssFontFace* PR_NONNULL>& faces) {
    bool wasLoading = false;
    Array<PrCssFontFace* PR_NONNULL>* lists[] = {&set->loading, &set->loaded, &set->failed};
    for (Array<PrCssFontFace* PR_NONNULL>* list : lists) {
        for (uint32_t i = 0; i < list->count;) {
            if (faces.find(list->data[i]) != kArrayMissing) {
                ++i;
            } else {
                wasLoading = wasLoading || list == &set->loading;
                list->erase(i);
            }
        }
    }
    return wasLoading;
}

// The task that updates the document's set to match its font source after its sheets or rules changed. It switches the
// set to loaded if a loading face was removed. The task holds a reference to the document.
void reconcile(void* data) {
    PrDomDocument* document = static_cast<PrDomDocument*>(data);
    CssRoot* style = cssDocumentRoot(document);
    style->fontsReconcileQueued = false;
    if (PrCssFontFaceSet* set = style->fonts) {
        Array<PrCssFontFace* PR_NONNULL> faces;
        cssDocumentFontFaces(document, &faces);
        if (dropLeft(set, faces))
            switchToLoaded(set);
        faces.reset();
    }
    domNodeRelease(document);
}

} // namespace

PrCssFontFace* cssFontFaceCreate(PrStringView family, PrStringView source, PrByteView data,
        PrCompressCodec* PR_NULLABLE compress, const PrStringView* PR_NULLABLE descriptors) {
    PrCssFontFace* face = allocZeroed<PrCssFontFace>();
    face->descriptors.rule = CssAtRule_FontFace;
    face->references = 1;
    // The family is the name as written if it parses as a name and serializes the same. Otherwise it is a string of the
    // name, as for a generic keyword or a list (csswg-drafts issue 6236).
    CssDeclaredName familyName = reflected(PrCssFontFaceDescriptor_Family);
    bool named = family.size != 0 && cssDeclarationBlockSet(&face->descriptors, familyName, family, false, nullptr) !=
        CssEditResult_Rejected;
    if (named) {
        PrTextBuffer written = {};
        cssSerializePropertyValue(&written, &face->descriptors, familyName);
        named = prTextBufferView(&written) == family;
        prTextBufferReset(&written);
    }
    bool parsed = true;
    if (!named) {
        PrTextBuffer quoted = {};
        cssSerializeString(&quoted, family);
        parsed = cssDeclarationBlockSet(&face->descriptors, familyName, prTextBufferView(&quoted), false, nullptr) !=
            CssEditResult_Rejected;
        prTextBufferReset(&quoted);
    }
    for (uint32_t i = PrCssFontFaceDescriptor_Family + 1; i < PrCssFontFaceDescriptor_Count; ++i) {
        PrStringView value;
        if (descriptors != nullptr && descriptors[i - 1].data != nullptr)
            value = descriptors[i - 1];
        else
            value = kReflected[i].initial;
        parsed = cssDeclarationBlockSet(&face->descriptors, reflected(i), value, false, nullptr) !=
            CssEditResult_Rejected && parsed;
    }
    face->fromData = data.data != nullptr;
    if (face->fromData) {
        face->font = fontOf(data, compress);
        // css-font-loading-3/#font-face-constructor: data that is not a font fails as a syntax error.
        face->unparsed = !parsed || face->font == nullptr;
        face->status = face->unparsed ? PrCssFontFaceStatus_Error : PrCssFontFaceStatus_Loaded;
        return face;
    }
    parsed = cssDeclarationBlockSet(&face->descriptors, cssDescriptorDeclaredName(PrCssDescriptor_FontFaceSrc), source,
        false, nullptr) != CssEditResult_Rejected && parsed;
    face->unparsed = !parsed;
    face->status = parsed ? PrCssFontFaceStatus_Unloaded : PrCssFontFaceStatus_Error;
    return face;
}

void cssFontFaceRetain(PrCssFontFace* face) {
    ++face->references;
}

void cssFontFaceRelease(PrCssFontFace* PR_NULLABLE face) {
    if (face == nullptr || --face->references != 0)
        return;
    PR_ASSERT(face->rule == nullptr && face->sets.count == 0);
    cssDeclarationBlockReset(&face->descriptors);
    face->sets.reset();
    prFontRelease(face->font);
    sourceRelease(face->source);
    prMemoryFree(face);
}

PrString cssFontFaceDescriptor(const PrCssFontFace* face, PrCssFontFaceDescriptor descriptor) {
    PrTextBuffer text = {};
    cssSerializePropertyValue(&text, blockOf(face), reflected(descriptor));
    PrString out = prStringCopy(prTextBufferView(&text));
    prTextBufferReset(&text);
    return out;
}

PrStatus cssFontFaceSetDescriptor(PrCssFontFace* face, PrCssFontFaceDescriptor descriptor, PrStringView value) {
    // An empty value is not a value of any descriptor, and would remove the declaration instead.
    if (prStringTrim(value).size == 0)
        return PrStatus_Syntax;
    CssEdit edit;
    cssEditBegin(&edit, nullptr);
    CssEditResult result = cssDeclarationBlockPrepareSet(&edit, blockOf(face), reflected(descriptor), value, false);
    if (result == CssEditResult_Changed)
        cssDeclarationBlockCommit(&edit, writableBlockOf(face));
    cssEditEnd(&edit);
    return result == CssEditResult_Rejected ? PrStatus_Syntax : PrStatus_Ok;
}

void cssFontFaceLoad(PrCssFontFace* face, PrDomDocument* document) {
    if (face->status != PrCssFontFaceStatus_Unloaded)
        return;
    setStatus(face, document, PrCssFontFaceStatus_Loading);
    const DomDocumentHost* host = document->host;
    if (host == nullptr) {
        setStatus(face, document, PrCssFontFaceStatus_Error);
        return;
    }
    CssFontLoad* load = allocZeroed<CssFontLoad>();
    load->face = face;
    load->document = document;
    cssFontFaceRetain(face);
    domNodeRetain(document);
    host->queueTask(host->context, startLoad, load, dropLoad);
}

bool cssFontFaceDescribe(const PrCssFontFace* face, PrString* family, PrFontFaceDescriptor* out,
        Array<PrFontCodePointRange>* ranges) {
    *family = faceFamily(face);
    if (prStringIsNull(family))
        return false;
    const CssDeclarationBlock* block = blockOf(face);
    auto valueOf = [&](PrCssDescriptor descriptor) {
        uint32_t at = cssDeclarationBlockFind(block, cssDescriptorDeclaredName(descriptor));
        return at < block->count ? cssDeclarationValue(block, cssDeclarations(block) + at) : CssValueRun{};
    };
    // A range of one or two numbers, or of keywords that `keyword` maps to numbers. The range is normal for auto and if
    // no value is given.
    auto range = [&](CssValueRun run, float normal, double (*keyword)(PrCssKeyword), float* min, float* max) {
        float values[2] = {normal, normal};
        uint32_t count = 0;
        for (uint32_t i = 0; i < run.count && count < 2; i += run.values[i].next) {
            const CssValueNode* value = &run.values[i];
            if (value->kind == PrCssValueKind_Number || value->kind == PrCssValueKind_Integer ||
                    value->kind == PrCssValueKind_Percentage)
                values[count++] = float(value->number);
            else if (value->kind == PrCssValueKind_Keyword && cssValueKeyword(value) != PrCssKeyword_Auto)
                values[count++] = float(keyword(cssValueKeyword(value)));
        }
        *min = values[0];
        *max = count == 2 ? values[1] : values[0];
        if (*min > *max) {
            float swap = *min;
            *min = *max;
            *max = swap;
        }
    };
    PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
    d.automatic = PrFontFaceAutoFlag_Emoji;
    // auto matches as normal and leaves the font's axes unclamped (css-fonts-4/#font-prop-desc).
    auto isAuto = [](CssValueRun run) {
        return run.count == 0 || (run.count == 1 && run.values[0].kind == PrCssValueKind_Keyword &&
            cssValueKeyword(&run.values[0]) == PrCssKeyword_Auto);
    };
    d.family = prStringView(family);
    range(valueOf(PrCssDescriptor_FontFaceFontWeight), kPrFontWeightNormal, [](PrCssKeyword keyword) {
        return double(keyword == PrCssKeyword_Bold ? kPrFontWeightBold : kPrFontWeightNormal);
    }, &d.weightMin, &d.weightMax);
    CssValueRun width = valueOf(PrCssDescriptor_FontFaceFontWidth);
    if (width.count == 0)
        width = valueOf(PrCssDescriptor_FontFaceFontStretch);
    range(width, kPrFontWidthNormal, cssFontWidthOfKeyword, &d.widthMin, &d.widthMax);
    CssValueRun style = valueOf(PrCssDescriptor_FontFaceFontStyle);
    bool autoWeight = isAuto(valueOf(PrCssDescriptor_FontFaceFontWeight));
    d.unclamped = PrFontFaceAutoFlags((autoWeight ? PrFontFaceAutoFlag_Weight : 0) |
        (isAuto(width) ? PrFontFaceAutoFlag_Width : 0) | (isAuto(style) ? PrFontFaceAutoFlag_Slope : 0));
    PrCssKeyword slope = PrCssKeyword_Normal;
    if (style.count != 0 && style.values[0].kind == PrCssValueKind_Keyword)
        slope = cssValueKeyword(&style.values[0]);
    if (slope == PrCssKeyword_Italic) {
        d.slope = PrFontSlope_Italic;
    } else if (slope == PrCssKeyword_Oblique) {
        d.slope = PrFontSlope_Oblique;
        double angles[2] = {double(kPrFontSetObliqueAngle), double(kPrFontSetObliqueAngle)};
        uint32_t count = 0;
        for (uint32_t i = style.values[0].next; i < style.count && count < 2; i += style.values[i].next) {
            if (cssMathTo(style, i, PrCssUnit_Deg, &angles[count]))
                ++count;
        }
        // A decreasing range is swapped (css-fonts-4/#font-prop-desc).
        double last = count == 2 ? angles[1] : angles[0];
        d.obliqueMin = float(angles[0] < last ? angles[0] : last);
        d.obliqueMax = float(angles[0] < last ? last : angles[0]);
    }
    CssValueRun unicode = valueOf(PrCssDescriptor_FontFaceUnicodeRange);
    for (uint32_t i = 0; i < unicode.count; i += unicode.values[i].next) {
        if (unicode.values[i].kind == PrCssValueKind_UnicodeRange)
            ranges->push(PrFontCodePointRange{unicode.values[i].range.first, unicode.values[i].range.last});
    }
    d.ranges = ranges->data;
    d.rangeCount = ranges->count;
    // A percentage of the em, or normal for the font's own value.
    auto percent = [&](PrCssDescriptor descriptor, float* out, PrFontOverrideFlags flag) {
        CssValueRun run = valueOf(descriptor);
        if (run.count == 1 && run.values[0].kind == PrCssValueKind_Percentage) {
            *out = float(run.values[0].number / 100);
            d.derivation.overrides |= flag;
        }
    };
    percent(PrCssDescriptor_FontFaceAscentOverride, &d.derivation.ascent, PrFontOverrideFlag_Ascent);
    percent(PrCssDescriptor_FontFaceDescentOverride, &d.derivation.descent, PrFontOverrideFlag_Descent);
    percent(PrCssDescriptor_FontFaceLineGapOverride, &d.derivation.lineGap, PrFontOverrideFlag_LineGap);
    CssValueRun size = valueOf(PrCssDescriptor_FontFaceSizeAdjust);
    if (size.count == 1 && size.values[0].kind == PrCssValueKind_Percentage && size.values[0].number > 0)
        d.derivation.sizeAdjust = float(size.values[0].number / 100);
    *out = d;
    return true;
}

PrCssFontFace* cssFontFaceOfRule(PrCssRule* rule) {
    PR_ASSERT(rule->data->kind == CssRuleDataKind_At && rule->data->atRule == CssAtRule_FontFace);
    if (rule->face == nullptr) {
        PrCssFontFace* face = allocZeroed<PrCssFontFace>();
        face->descriptors.rule = CssAtRule_FontFace;
        face->references = 1;
        face->rule = rule;
        rule->face = face;
    }
    return rule->face;
}

void cssFontFaceDisconnect(PrCssFontFace* face) {
    if (face->rule == nullptr)
        return;
    PrTextBuffer text = {};
    cssSerializeDeclarationBlock(&text, &face->rule->data->style);
    face->descriptors.rule = CssAtRule_FontFace;
    cssDeclarationBlockParse(&face->descriptors, prTextBufferView(&text), nullptr);
    prTextBufferReset(&text);
    face->rule = nullptr;
}

PrCssFontFaceSet* cssFontFaceSetCreate(PrDomDocument* document) {
    PrCssFontFaceSet* set = allocZeroed<PrCssFontFaceSet>();
    set->type = DomTargetType_FontFaceSet;
    set->document = document;
    set->status = pendingOnEnvironment(set) ? PrCssFontFaceSetLoadStatus_Loading : PrCssFontFaceSetLoadStatus_Loaded;
    return set;
}

void cssFontFaceSetDestroy(PrCssFontFaceSet* PR_NULLABLE set) {
    if (set == nullptr)
        return;
    // The set is destroyed with its document. Its faces leave it without the events and transitions that a removal
    // causes.
    for (uint32_t i = 0; i < set->faces.count; ++i) {
        set->faces.data[i]->sets.removeFirst(set);
        cssFontFaceRelease(set->faces.data[i]);
    }
    set->faces.reset();
    set->loading.reset();
    set->loaded.reset();
    set->failed.reset();
    domListenersDestroy(set->listeners);
    prMemoryFree(set);
}

void cssFontFaceSetFaces(const PrCssFontFaceSet* set, Array<PrCssFontFace* PR_NONNULL>* out) {
    cssDocumentFontFaces(set->document, out);
}

bool cssFontFaceSetHas(const PrCssFontFaceSet* set, const PrCssFontFace* face) {
    Array<PrCssFontFace* PR_NONNULL> faces;
    cssFontFaceSetFaces(set, &faces);
    bool found = faces.find(const_cast<PrCssFontFace*>(face)) != kArrayMissing;
    faces.reset();
    return found;
}

PrStatus cssFontFaceSetAdd(PrCssFontFaceSet* set, PrCssFontFace* face) {
    if (face->rule != nullptr)
        return PrStatus_NotAllowed;
    if (set->faces.find(face) != kArrayMissing)
        return PrStatus_Ok;
    cssFontFaceRetain(face);
    set->faces.push(face);
    face->sets.push(set);
    ++cssDocumentRoot(set->document)->fontsVersion;
    ++cssDocumentRoot(set->document)->fontSetVersion;
    // css-font-loading-3/#dom-fontfaceset-add: a face that is already loading joins the set's loading faces.
    if (face->status == PrCssFontFaceStatus_Loading)
        faceLoading(set, face);
    return PrStatus_Ok;
}

bool cssFontFaceSetDelete(PrCssFontFaceSet* set, PrCssFontFace* face) {
    if (!set->faces.removeFirst(face))
        return false;
    ++cssDocumentRoot(set->document)->fontsVersion;
    ++cssDocumentRoot(set->document)->fontSetVersion;
    forget(set, face);
    cssFontFaceRelease(face);
    return true;
}

void cssFontFaceSetClear(PrCssFontFaceSet* set) {
    ++cssDocumentRoot(set->document)->fontsVersion;
    ++cssDocumentRoot(set->document)->fontSetVersion;
    // Every face leaves before the set switches, so that the set's events name none of them.
    bool wasLoading = false;
    while (set->faces.count != 0) {
        PrCssFontFace* face = set->faces.last();
        set->faces.pop();
        face->sets.removeFirst(set);
        wasLoading = dropFace(set, face) || wasLoading;
        cssFontFaceRelease(face);
    }
    if (wasLoading)
        switchToLoaded(set);
}

PrStatus cssFontFaceSetMatch(const PrCssFontFaceSet* set, PrStringView font, Array<PrCssFontFace* PR_NONNULL>* out) {
    // The value of the font property, which cannot be a CSS-wide keyword here.
    PrStringView trimmed = prStringTrim(font);
    if (cssIsCssWideKeyword(cssKeywordFromText(trimmed)))
        return PrStatus_Syntax;
    CssDeclarationBlock block = {};
    bool parsed = trimmed.size != 0 && cssDeclarationBlockSet(&block, cssPropertyDeclaredName(PrCssProperty_Font),
        font, false, nullptr) != CssEditResult_Rejected;
    PrTextBuffer families = {};
    if (parsed)
        cssSerializePropertyValue(&families, &block, cssPropertyDeclaredName(PrCssProperty_FontFamily));
    cssDeclarationBlockReset(&block);
    // A value containing a substitution function has no family until it is computed.
    if (!parsed || families.size == 0) {
        prTextBufferReset(&families);
        return PrStatus_Syntax;
    }
    Array<PrString> names;
    familyNames(prTextBufferView(&families), &names);
    prTextBufferReset(&families);
    Array<PrCssFontFace* PR_NONNULL> faces;
    cssFontFaceSetFaces(set, &faces);
    for (uint32_t i = 0; i < faces.count; ++i) {
        PrCssFontFace* face = faces.data[i];
        PrString family = faceFamily(face);
        for (uint32_t j = 0; j < names.count; ++j) {
            if (!prStringIsNull(&family) && prAsciiEqualsFold(prStringView(&family), prStringView(&names.data[j]))) {
                out->push(face);
                break;
            }
        }
        prStringReset(&family);
    }
    faces.reset();
    resetNames(&names);
    return PrStatus_Ok;
}

void cssDocumentFontFaces(const PrDomDocument* document, Array<PrCssFontFace* PR_NONNULL>* out) {
    const CssRoot* style = document->style;
    if (style == nullptr)
        return;
    for (uint32_t i = 0; i < style->sheets.sheets.count; ++i)
        connectedFaces(style->sheets.sheets.data[i], out);
    if (style->fonts != nullptr)
        out->append(style->fonts->faces.data, style->fonts->faces.count);
}

const PrFont* PR_NULLABLE cssFontFaceCachedFont(const PrCssFontFace* face, PrDomDocument* document) {
    if (face->status != PrCssFontFaceStatus_Unloaded || face->fromData || document->style == nullptr)
        return nullptr;
    // Reading the face's sources costs time in proportion to the length of their URLs, and a data URL is long.
    bool decoded = false;
    for (uint32_t i = 0; i < document->style->fontSources.count && !decoded; ++i)
        decoded = document->style->fontSources.data[i]->font != nullptr;
    if (!decoded)
        return nullptr;
    Array<PrString> requests;
    sourceRequests(const_cast<PrCssFontFace*>(face), document, &requests);
    const PrFont* found = nullptr;
    for (uint32_t i = 0; i < requests.count; ++i) {
        const CssFontSource* source = findSource(document->style, prStringView(&requests.data[i]));
        if (source == nullptr || !source->settled)
            break;
        if (source->font != nullptr) {
            found = source->font;
            break;
        }
    }
    stringsReset(&requests);
    return found;
}

void cssFontSourcesReset(Array<CssFontSource* PR_NONNULL>* sources) {
    Array<CssFontSource* PR_NONNULL> held = *sources;
    *sources = {};
    for (uint32_t i = 0; i < held.count; ++i) {
        held.data[i]->cache = nullptr;
        sourceRelease(held.data[i]);
    }
    held.reset();
}

void cssFontsSourceChanged(PrDomDocument* document) {
    CssRoot* style = document->style;
    PrCssFontFaceSet* set = style != nullptr ? style->fonts : nullptr;
    const DomDocumentHost* host = document->host;
    if (set == nullptr || host == nullptr || style->fontsReconcileQueued ||
            set->loading.count + set->loaded.count + set->failed.count == 0)
        return;
    style->fontsReconcileQueued = true;
    domNodeRetain(document);
    host->queueTask(host->context, reconcile, document, [](void* data) {
        PrDomDocument* dropped = static_cast<PrDomDocument*>(data);
        cssDocumentRoot(dropped)->fontsReconcileQueued = false;
        domNodeRelease(dropped);
    });
}

void cssFontsSourceSynced(PrDomDocument* document, const Array<PrCssFontFace* PR_NONNULL>& faces) {
    PrCssFontFaceSet* set = document->style != nullptr ? document->style->fonts : nullptr;
    if (set != nullptr)
        dropLeft(set, faces);
}

void cssFontsReadinessChanged(PrDomDocument* document) {
    if (document->style != nullptr && document->style->fonts != nullptr)
        switchToLoaded(document->style->fonts);
}

void cssFontFaceSetLoadEventSetFaces(PrDomEvent* event, PrCssFontFace* PR_NONNULL const* PR_NULLABLE faces, uint32_t count) {
    CssFontFacesData* data = allocZeroed<CssFontFacesData>();
    data->destroy = [](DomEventExtra* extra) {
        CssFontFacesData* data = static_cast<CssFontFacesData*>(extra);
        for (uint32_t i = 0; i < data->faces.count; ++i)
            cssFontFaceRelease(data->faces.data[i]);
        data->faces.reset();
        prMemoryFree(data);
    };
    for (uint32_t i = 0; i < count; ++i) {
        cssFontFaceRetain(faces[i]);
        data->faces.push(faces[i]);
    }
    event->extra = data;
}

} // namespace pr_impl
