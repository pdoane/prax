// The cost of a windowed view at each scale: what a first frame, a keystroke, a caret's motion, a caret's blink and a
// scroll cost through layout, with wrap and proportional text on. A row of these that grows with the document is a
// defect.
//
// ./dev.sh bench text-view runs this and prints its rows.
//
// Usage: text_view_bench [file]
//
// With a file, it instead prints once what the file costs cold, in milliseconds, at its longest line and in its middle,
// and what rendering each change costs on the platform's device.

#include "pr/assert.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/font_set.h"
#include "pr/gpu.h"
#include "pr/layout_frame.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/string_view.h"
#include "pr/syntax.h"
#include "pr/syntax_treesitter_c.h"
#include "pr/syntax_treesitter_cpp.h"
#include "pr/syntax_treesitter_css.h"
#include "pr/syntax_treesitter_html.h"
#include "pr/syntax_treesitter_javascript.h"
#include "pr/syntax_treesitter_json.h"
#include "pr/syntax_treesitter_markdown.h"
#include "pr/syntax_treesitter_markdown_inline.h"
#include "pr/syntax_treesitter_python.h"
#include "pr/syntax_treesitter_xml.h"
#include "pr/text_harfbuzz.h"
#include "pr/text_view.h"
#include "pr/time.h"

#include "tools/harness/harness.h"

#include <stdio.h>
#include <string>
#include <vector>

using namespace harness;

namespace {

// The bytes of a piece of a file that streams in.
constexpr size_t kStreamPiece = 64 * 1024;

// The numbers of lines of the documents.
constexpr uint32_t kLines[] = {100, 10000, 1000000};

// Returns a document of `lines` lines of different text.
std::string makeDocument(uint32_t lines) {
    std::string text;
    for (uint32_t i = 0; i < lines; ++i) {
        text += "    const value";
        text += std::to_string(i);
        text += " = compute(alpha, beta) + table[index] * 42; // note\n";
    }
    return text;
}

// A view in a tree, in a port of 1000 by 800 px.
struct Bench {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFont* font = nullptr;
    PrLayoutTree* tree = nullptr;
    PrTextEditBuffer* buffer = nullptr;
    PrTextView* view = nullptr;
    PrLayoutStyleIndex portStyle = 0;
    PrLayoutStyleIndex rowStyle = 0;
    float width = 1000;
    bool caretHidden = false;

    // The syntax tree of the file's language, if it has one, whose every category shows in one color.
    PrSyntaxRegistry* registry = nullptr;
    PrSyntaxTree* syntax = nullptr;
    std::vector<PrSyntaxSpan> raw;

    Bench() {
        std::string path = std::string(PR_FONTS_DIR) + "/Arimo.ttf";
        const PrFont* PR_NULLABLE face = nullptr;
        if (prFontCreateFromFile(path, 0, nullptr, &face) != PrStatus_Ok)
            PR_FATAL("cannot open Arimo");
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(fonts, &descriptor, face);
        prFontRelease(face);
        prFontSetSetDefaultFamily(fonts, prFontFamilyFromName("Arimo"));
        PrTextFontDesc fontDesc{.set = fonts, .style = prFontStyleDefault()};
        font = prTextFontCreate(&fontDesc);
        PrLayoutTreeDesc desc{.font = font, .shaper = shaper, .targetPixelRatio = 1};
        tree = prLayoutTreeCreate(&desc);
    }

    ~Bench() {
        prSyntaxTreeDestroy(syntax);
        prSyntaxRegistryDestroy(registry);
        prTextViewDestroy(view);
        prTextEditBufferDestroy(buffer);
        prLayoutTreeDestroy(tree);
        prTextFontRelease(font);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }

    PrLayoutStyleIndex style(bool row) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = PrLayoutDisplay_Block;
        if (!row) {
            box.width = prLayoutLengthPx(width);
            box.height = prLayoutLengthPx(800);
            box.overflowX = PrLayoutOverflow_Hidden;
            box.overflowY = PrLayoutOverflow_Hidden;
        }
        PrLayoutFontStyle fontStyle = prLayoutFontStyleDefault();
        fontStyle.font = font;
        fontStyle.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
        fontStyle.overflowWrap = PrTextOverflowWrap_Anywhere;
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
        record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(tree, &fontStyle);
        return prLayoutStyleIntern(tree, &record);
    }

    // The spans of a window, kept while the text and the window stay the same, as an editor keeps them.
    uint64_t rawVersion = UINT64_MAX;
    uint32_t rawStart = 0;
    uint32_t rawEnd = 0;

    static uint32_t spansOf(void* context, uint32_t start, uint32_t end, PrTextViewSpan* out, uint32_t capacity) {
        Bench& b = *static_cast<Bench*>(context);
        uint64_t version = prTextEditBufferVersion(b.buffer);
        if (version != b.rawVersion || start != b.rawStart || end != b.rawEnd) {
            b.raw.resize(prSyntaxTreeSpans(b.syntax, start, end, nullptr, 0));
            prSyntaxTreeSpans(b.syntax, start, end, b.raw.data(), uint32_t(b.raw.size()));
            b.rawVersion = version;
            b.rawStart = start;
            b.rawEnd = end;
        }
        for (uint32_t i = 0; i < b.raw.size() && i < capacity; ++i)
            out[i] = PrTextViewSpan{b.raw[i].start, b.raw[i].end, prColor16FromColor(PrColor{0.2f, 0.4f, 0.8f, 1})};
        return uint32_t(b.raw.size());
    }

    // Runs a frame, and returns whether the view asks for another.
    bool frame() {
        if (syntax != nullptr)
            prSyntaxTreeUpdate(syntax, pr::kInfinityF64);
        prLayoutTreeBeginFrame(tree);
        // The root does not scroll, so the port is under it.
        PrLayoutNode top = prLayoutTreeFrameElement(tree, kPrLayoutNodeNone, 0, style(true));
        PrLayoutNode port = prLayoutTreeFrameElement(tree, top, 0, style(false));
        PrTextViewFrameDesc desc{
            .tree = tree,
            .parent = port,
            .extent = PrVec2{width, 800},
            .rowStyle = style(true),
            .spans = syntax != nullptr ? spansOf : nullptr,
            .spansContext = this,
            .wrap = true,
            .showCaret = true,
            .caretHidden = caretHidden,
        };
        prTextViewFrame(view, &desc);
        PrLayoutRoot root{top, prLayoutVecFromPx(PrVec2{width, 800})};
        prLayoutTreeLayout(tree, &root, 1);
        return prTextViewLaidOut(view);
    }

    // Runs a frame with the caret turned off or on, and records the scene that a host would paint.
    void blink(bool on) {
        caretHidden = !on;
        frame();
        prLayoutSceneRelease(prLayoutTreeScene(tree));
    }
};

// A texture of the port's extent at a pixel ratio of 2, which frames of a tree render into as a window's would.
struct Rendering {
    static constexpr float kRatio = 2;

    PrGpuDevice* device = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* frame = nullptr;
    PrDrawList* list = prDrawListCreate();
    PrGpuTexture texture{};
    PrUVec2 extent{};

    ~Rendering() {
        prDrawListRelease(list);
        prDrawFrameDestroy(frame);
        prDrawRendererDestroy(renderer);
        if (texture.id != 0)
            prGpuTextureDestroy(device, texture);
        prGpuDeviceDestroy(device);
    }

    // Returns false if the platform has no device.
    bool open(PrUVec2 size) {
        extent = size;
        if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) != PrStatus_Ok ||
                prDrawRendererCreate(PrDrawRendererBackend_Default, device, nullptr, &renderer) != PrStatus_Ok)
            return false;
        frame = prDrawFrameCreate(renderer);
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Bgra8, extent,
            PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
        return prGpuTextureCreate(device, &desc, &texture) == PrStatus_Ok;
    }
};

// What one rendered frame costs, in milliseconds.
struct RenderCost {
    double scene;   // painting the scene, and playing it into a list
    double encode;  // preparing the list and recording its commands
    double gpu;     // the device's time
};

// Renders the tree's last layout, and returns what it cost.
RenderCost render(Rendering& r, PrLayoutTree* tree) {
    double start = prTimeMonotonicMilliseconds();
    const PrLayoutScene* scene = prLayoutTreeScene(tree);
    std::vector<PrLayoutSceneScroll> scrolls(prLayoutSceneScrolls(tree, scene, nullptr, 0));
    prLayoutSceneScrolls(tree, scene, scrolls.data(), uint32_t(scrolls.size()));
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = scrolls.data();
    options.scrollCount = uint32_t(scrolls.size());
    options.canvasFilled = true;
    prDrawListClear(r.list);
    prDrawListPushTransformAffine2(r.list, prAffine2Scale(PrVec2{Rendering::kRatio, Rendering::kRatio}));
    prLayoutScenePlay(scene, r.list, &options);
    prDrawListPopTransform(r.list);
    prLayoutSceneRelease(scene);
    double played = prTimeMonotonicMilliseconds();
    PrGpuCommands* commands = nullptr;
    if (!prGpuCommandsBegin(r.device, nullptr, &commands))
        PR_FATAL("the device is lost");
    PrDrawTarget target{.extent = r.extent, .format = PrPixelFormat_Bgra8, .commands = commands};
    if (prDrawFramePrepare(r.frame, r.list, &target) != PrStatus_Ok)
        PR_FATAL("the frame cannot be prepared");
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, r.texture, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc desc = prGpuPassDescColor(r.texture, PrGpuLoad_Clear, PrColor{1, 1, 1, 1});
    PrGpuPass* pass = prGpuPassBegin(commands, &desc);
    prDrawFrameRender(r.frame, pass, PrIVec2{0, 0});
    prGpuPassEnd(pass);
    prGpuCommandsTextureBarrier(commands, r.texture, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
    double encoded = prTimeMonotonicMilliseconds();
    double gpu = prGpuCommandsSubmitAndWait(commands) * 1000;
    return RenderCost{played - start, encoded - played, gpu};
}

// Prints what `change` costs to lay out and render, on average over repeats, once the caches hold what it shows.
template <typename Change>
void rendered(Rendering& r, Bench& b, const char* label, Change change) {
    constexpr uint32_t kWarm = 4;
    constexpr uint32_t kRepeats = 20;
    double layout = 0;
    RenderCost total{};
    for (uint32_t i = 0; i < kWarm + kRepeats; ++i) {
        double start = prTimeMonotonicMilliseconds();
        change(i);
        b.frame();
        double took = prTimeMonotonicMilliseconds() - start;
        RenderCost cost = render(r, b.tree);
        if (i < kWarm)
            continue;
        layout += took;
        total.scene += cost.scene;
        total.encode += cost.encode;
        total.gpu += cost.gpu;
    }
    printf("%-34s layout %7.3f  scene %7.3f  encode %7.3f  gpu %7.3f ms\n", label, layout / kRepeats,
        total.scene / kRepeats, total.encode / kRepeats, total.gpu / kRepeats);
}

// Prints what each change at the start of `line` costs to render.
void renderedAt(Rendering& r, Bench& b, uint32_t line, const char* where) {
    uint32_t at = prTextEditBufferLineStart(b.buffer, line);
    PrTextEditSelection caret{.anchor = at, .caret = at};
    prTextEditViewSetSelections(prTextViewEdit(b.view), &caret, 1);
    prTextViewReveal(b.view);
    b.frame();
    b.frame();
    printf("%s, rendered at a pixel ratio of 2:\n", where);
    rendered(r, b, "  nothing changed", [](uint32_t) {});
    rendered(r, b, "  caret blink", [&](uint32_t i) { b.caretHidden = i % 2 == 0; });
    b.caretHidden = false;
    rendered(r, b, "  arrow", [&](uint32_t i) {
        prTextViewRun(b.view, i % 2 == 0 ? PrTextEditCommand_Right : PrTextEditCommand_Left,
            PrTextEditSelectionMove_Collapse);
    });
    rendered(r, b, "  keystroke", [&](uint32_t i) {
        if (i % 2 == 0)
            prTextViewInsert(b.view, "x", PrTextEditMerge_None);
        else
            prTextViewRun(b.view, PrTextEditCommand_DeleteBackward, PrTextEditSelectionMove_Collapse);
    });
    rendered(r, b, "  scroll", [&](uint32_t i) { prTextViewScrollBy(b.view, PrVec2{0, i % 2 == 0 ? 40.0f : -40.0f}); });
}

// Times `work` once, and prints it.
template <typename Work>
void timed(const char* label, Work work) {
    double start = prTimeMonotonicMilliseconds();
    work();
    printf("%-44s %10.2f ms\n", label, prTimeMonotonicMilliseconds() - start);
}

// Prints what a real file costs cold.
int measureFile(const char* path) {
    PrFileMapping mapping{};
    if (!prFileMap(path, &mapping)) {
        fprintf(stderr, "text_view_bench: cannot read %s\n", path);
        return 1;
    }
    Bench b;
    PrStringView text{reinterpret_cast<const char*>(mapping.data), mapping.size};
    timed("open", [&] {
        b.buffer = prTextEditBufferCreate(text);
        b.view = prTextViewCreate(b.buffer);
    });
    b.registry = prSyntaxRegistryCreate();
    for (const PrSyntaxLanguage* language : {
        prSyntaxTreeSitterC(), prSyntaxTreeSitterCpp(), prSyntaxTreeSitterCss(), prSyntaxTreeSitterHtml(),
            prSyntaxTreeSitterJavaScript(), prSyntaxTreeSitterJson(), prSyntaxTreeSitterMarkdown(),
            prSyntaxTreeSitterMarkdownInline(), prSyntaxTreeSitterPython(), prSyntaxTreeSitterXml(),
    })
        prSyntaxRegistryAdd(b.registry, language);
    const PrSyntaxLanguage* language = prSyntaxRegistryFromPath(b.registry, path);
    if (language != nullptr) {
        b.syntax = prSyntaxTreeCreate(language, b.registry, b.buffer);
        printf("highlighting as %.*s\n", int(language->name.size), language->name.data);
        // The file streams in as an editor loads it: a piece at a time, each parsed before the next arrives.
        PrTextEditBuffer* streamed = prTextEditBufferCreate("");
        PrSyntaxTree* growing = prSyntaxTreeCreate(language, b.registry, streamed);
        double slowest = 0;
        uint32_t pieces = 0;
        double start = prTimeMonotonicMilliseconds();
        for (size_t at = 0; at < text.size; at += kStreamPiece) {
            size_t size = text.size - at < kStreamPiece ? text.size - at : kStreamPiece;
            double pieceStart = prTimeMonotonicMilliseconds();
            uint32_t end = prTextEditBufferSize(streamed);
            PrTextEditChange piece{.start = end, .end = end, .text = PrStringView{text.data + at, size}};
            prTextEditBufferApply(streamed, &piece, 1, PrTextEditOrigin_Load);
            prSyntaxTreeUpdate(growing, pr::kInfinityF64);
            double took = prTimeMonotonicMilliseconds() - pieceStart;
            slowest = took > slowest ? took : slowest;
            ++pieces;
        }
        printf("%-44s %10.2f ms, %u pieces, slowest %.2f ms\n", "streamed load, parsing each piece",
            prTimeMonotonicMilliseconds() - start, pieces, slowest);
        prSyntaxTreeDestroy(growing);
        prTextEditBufferDestroy(streamed);
        timed("first parse of the whole text", [&] { prSyntaxTreeUpdate(b.syntax, pr::kInfinityF64); });
        PrSyntaxSpan none{};
        timed("spans of the first page", [&] {
            prSyntaxTreeSpans(b.syntax, 0, prTextEditBufferLineEnd(b.buffer, 60), &none, 0);
        });
    }
    timed("first frame", [&] { b.frame(); });
    timed("second frame, with every estimate measured", [&] { b.frame(); });
    uint32_t lines = prTextEditBufferLineCount(b.buffer);
    uint32_t longest = 0;
    uint32_t longestBytes = 0;
    for (uint32_t line = 0; line < lines; ++line) {
        uint32_t bytes = prTextEditBufferLineEnd(b.buffer, line) - prTextEditBufferLineStart(b.buffer, line);
        if (bytes > longestBytes) {
            longestBytes = bytes;
            longest = line;
        }
    }
    printf("%u lines, %u bytes, longest line %u of %u bytes, %.0f px estimated\n", lines,
        prTextEditBufferSize(b.buffer), longest, longestBytes, prTextViewInfo(b.view).documentSize);
    auto typeHere = [&](uint32_t line, const char* where) {
        uint32_t at = prTextEditBufferLineStart(b.buffer, line);
        PrTextEditSelection caret{.anchor = at, .caret = at};
        prTextEditViewSetSelections(prTextViewEdit(b.view), &caret, 1);
        prTextViewReveal(b.view);
        std::string jump = std::string("jump to ") + where;
        timed(jump.c_str(), [&] { b.frame(); });
        std::string settle = std::string("settle at ") + where;
        timed(settle.c_str(), [&] { b.frame(); });
        if (b.syntax != nullptr) {
            // A keystroke's syntax in its parts: the incremental parse, and the spans of a window of its lines.
            uint32_t lines = prTextEditBufferLineCount(b.buffer);
            uint32_t windowStart = prTextEditBufferLineStart(b.buffer, line > 60 ? line - 60 : 0);
            uint32_t windowEnd = prTextEditBufferLineEnd(b.buffer, line + 60 < lines ? line + 60 : lines - 1);
            double parse = 0;
            double spans = 0;
            double firstParse = 0;
            double firstSpans = 0;
            constexpr uint32_t kRuns = 20;
            for (uint32_t i = 0; i < kRuns; ++i) {
                prTextViewInsert(b.view, "x", PrTextEditMerge_None);
                double start = prTimeMonotonicMilliseconds();
                prSyntaxTreeUpdate(b.syntax, pr::kInfinityF64);
                double parsed = prTimeMonotonicMilliseconds();
                prSyntaxTreeSpans(b.syntax, windowStart, windowEnd, nullptr, 0);
                spans += prTimeMonotonicMilliseconds() - parsed;
                parse += parsed - start;
                if (i == 0) {
                    firstParse = parse;
                    firstSpans = spans;
                }
            }
            printf("%-44s %10.2f ms\n", (std::string("parse of a first keystroke at ") + where).c_str(), firstParse);
            printf("%-44s %10.2f ms\n", (std::string("spans of a first keystroke at ") + where).c_str(), firstSpans);
            printf("%-44s %10.2f ms\n", (std::string("parse of a keystroke at ") + where).c_str(), parse / kRuns);
            printf("%-44s %10.2f ms\n", (std::string("spans of a keystroke at ") + where).c_str(), spans / kRuns);
        }
        std::string key = std::string("keystroke at ") + where;
        timed(key.c_str(), [&] {
            prTextViewInsert(b.view, "x", PrTextEditMerge_None);
            b.frame();
        });
        std::string again = std::string("next keystroke at ") + where;
        timed(again.c_str(), [&] {
            prTextViewInsert(b.view, "x", PrTextEditMerge_Typing);
            b.frame();
        });
        std::string arrow = std::string("arrow at ") + where;
        timed(arrow.c_str(), [&] {
            prTextViewRun(b.view, PrTextEditCommand_Right, PrTextEditSelectionMove_Collapse);
            b.frame();
        });
        b.blink(true);
        std::string off = std::string("caret off at ") + where;
        timed(off.c_str(), [&] { b.blink(false); });
        std::string on = std::string("caret on at ") + where;
        timed(on.c_str(), [&] { b.blink(true); });
        std::string scroll = std::string("scroll at ") + where;
        timed(scroll.c_str(), [&] {
            prTextViewScrollBy(b.view, PrVec2{0, 40});
            b.frame();
        });
    };
    // A drag of the window's edge, and what the frames after it cost and how many there are.
    double slowest = 0;
    double spent = 0;
    for (uint32_t i = 0; i < 120; ++i) {
        b.width = 1000 + float(i) * 2;
        double start = prTimeMonotonicMilliseconds();
        b.frame();
        double took = prTimeMonotonicMilliseconds() - start;
        spent += took;
        slowest = took > slowest ? took : slowest;
    }
    printf("%-44s %10.2f ms, slowest %.2f ms\n", "120 frames of a drag", spent, slowest);
    uint32_t settling = 0;
    double start = prTimeMonotonicMilliseconds();
    while (settling < 1000 && b.frame())
        ++settling;
    printf("%-44s %10.2f ms over %u frames\n", "frames until the view settles", prTimeMonotonicMilliseconds() - start, settling);
    start = prTimeMonotonicMilliseconds();
    while (prTextViewRefine(b.view, 1000)) {
    }
    printf("%-44s %10.2f ms\n", "refining every line", prTimeMonotonicMilliseconds() - start);
    typeHere(lines / 2, "the middle");
    typeHere(longest, "the longest line");
    b.width = 1000;
    prLayoutTreeSetTargetPixelRatio(b.tree, Rendering::kRatio);
    Rendering rendering;
    if (rendering.open(PrUVec2{2000, 1600})) {
        renderedAt(rendering, b, lines / 2, "The middle");
        renderedAt(rendering, b, longest, "The longest line");
    } else {
        printf("no device to render with\n");
    }
    prFileUnmap(&mapping);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc == 2)
        return measureFile(argv[1]);
    bool first = true;
    printf("[");
    for (uint32_t lines : kLines) {
        std::string document = makeDocument(lines);
        auto report = [&](const char* label, const HarnessCounters& cost) {
            std::string name = std::to_string(lines) + " lines " + label;
            harnessWriteCase(first, name.c_str(), cost);
            first = false;
        };
        Bench b;
        report("open and first frame", harnessMeasure([&] {
            prTextViewDestroy(b.view);
            prTextEditBufferDestroy(b.buffer);
            b.buffer = prTextEditBufferCreate(document);
            b.view = prTextViewCreate(b.buffer);
            b.frame();
        }));
        prTextViewScrollTo(b.view, prTextViewInfo(b.view).documentSize / 2);
        prTextViewRun(b.view, PrTextEditCommand_TextStart, PrTextEditSelectionMove_Collapse);
        uint32_t middle = prTextEditBufferLineStart(b.buffer, lines / 2);
        PrTextEditSelection caret{.anchor = middle, .caret = middle};
        prTextEditViewSetSelections(prTextViewEdit(b.view), &caret, 1);
        prTextViewReveal(b.view);
        b.frame();
        b.frame();
        report("refine", harnessMeasure([&] {
            b.width += 1;
            b.frame();
            while (prTextViewRefine(b.view, 1000)) {
            }
        }));
        report("keystroke", harnessMeasure([&] {
            prTextViewInsert(b.view, "x", PrTextEditMerge_None);
            b.frame();
            prTextViewRun(b.view, PrTextEditCommand_DeleteBackward, PrTextEditSelectionMove_Collapse);
            b.frame();
        }));
        report("arrow", harnessMeasure([&] {
            prTextViewRun(b.view, PrTextEditCommand_Right, PrTextEditSelectionMove_Collapse);
            b.frame();
            prTextViewRun(b.view, PrTextEditCommand_Left, PrTextEditSelectionMove_Collapse);
            b.frame();
        }));
        report("down", harnessMeasure([&] {
            prTextViewRun(b.view, PrTextEditCommand_Down, PrTextEditSelectionMove_Collapse);
            b.frame();
            prTextViewRun(b.view, PrTextEditCommand_Up, PrTextEditSelectionMove_Collapse);
            b.frame();
        }));
        b.blink(true);
        report("caret blink", harnessMeasure([&] {
            b.blink(false);
            b.blink(true);
        }));
        report("scroll", harnessMeasure([&] {
            prTextViewScrollBy(b.view, PrVec2{0, 40});
            b.frame();
            prTextViewScrollBy(b.view, PrVec2{0, -40});
            b.frame();
        }));
    }
    printf("\n]\n");
    return 0;
}
