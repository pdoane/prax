#include "pr/text_edit.h"
#include "pr/text_edit_command.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

extern "C" int prTestTextEditCApi(void);

namespace pr_impl {

namespace {

// Returns the text of `buffer`, read chunk by chunk.
std::string textOf(const PrTextEditBuffer* buffer) {
    std::string out;
    while (out.size() < prTextEditBufferSize(buffer)) {
        PrStringView chunk = prTextEditBufferChunk(buffer, uint32_t(out.size()));
        out.append(chunk.data, chunk.size);
    }
    return out;
}

PrTextEditChange change(uint32_t start, uint32_t end, PrStringView text) {
    return PrTextEditChange{.start = start, .end = end, .text = text};
}

// Applies one local change through `buffer`.
void replace(PrTextEditBuffer* buffer, uint32_t start, uint32_t end, PrStringView text) {
    PrTextEditChange c = change(start, end, text);
    REQUIRE(prTextEditBufferApply(buffer, &c, 1, PrTextEditOrigin_Local));
}

// Sets one selection of `view`.
void select(PrTextEditView* view, uint32_t anchor, uint32_t caret) {
    PrTextEditSelection s{.anchor = anchor, .caret = caret};
    prTextEditViewSetSelections(view, &s, 1);
}

// Removes the bytes from `start` to `end` through `view`, with the caret after them.
void erase(PrTextEditView* view, uint32_t start, uint32_t end, PrTextEditMerge merge) {
    PrTextEditChange c = change(start, end, "");
    REQUIRE(prTextEditViewApply(view, &c, 1, PrTextEditOrigin_Local, merge));
}

// An anchor and what the rules of anchors say it should hold.
struct ModelAnchor {
    PrTextEditAnchor anchor;
    PrTextEditBias bias;
    uint32_t offset;
    bool live;
};

// Moves `a` through one replacement, by the rules of anchors.
void moveModel(ModelAnchor& a, uint32_t start, uint32_t end, uint32_t size) {
    if (a.offset < start)
        return;
    if (a.offset == start) {
        a.offset = a.bias == PrTextEditBias_Right ? start + size : start;
    } else if (a.offset < end) {
        a.offset = start;
        a.live = false;
    } else {
        a.offset = a.offset - end + start + size;
    }
}

// Brings `replica` up to date from the records of `stream`.
void applyRecords(PrTextEditStream* stream, std::string& replica) {
    PrTextEditRecord records[8];
    for (;;) {
        uint32_t count = prTextEditStreamTake(stream, records, 8);
        if (count == 0)
            return;
        for (uint32_t i = 0; i < count; ++i) {
            const PrTextEditRecord& r = records[i];
            CHECK(r.newEnd == r.start + r.text.size);
            replica.replace(r.start, r.oldEnd - r.start, r.text.data, r.text.size);
        }
    }
}

// The buffer's runs, as start and style in turn.
std::vector<uint32_t> runsOf(const PrTextEditBuffer* buffer) {
    std::vector<uint32_t> out;
    const PrTextEditStyleRun* runs = prTextEditBufferStyleRuns(buffer);
    for (uint32_t i = 0; i < prTextEditBufferStyleRunCount(buffer); ++i) {
        out.push_back(runs[i].start);
        out.push_back(runs[i].style);
    }
    return out;
}

using Runs = std::vector<uint32_t>;

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestTextEditCApi() == 0);
}

TEST_CASE("a caret shows for half a second from its last move, then hides for half a second") {
    PrTextEditCaretBlink blink = prTextEditCaretBlinkAt(10, 10);
    CHECK(blink.on);
    CHECK(blink.next == 10.5);
    blink = prTextEditCaretBlinkAt(10, 10.75);
    CHECK(!blink.on);
    CHECK(blink.next == 11);
    blink = prTextEditCaretBlinkAt(10, 11.25);
    CHECK(blink.on);
    CHECK(blink.next == 11.5);
    blink = prTextEditCaretBlinkAt(10, 9);
    CHECK(blink.on);
    CHECK(blink.next == 10.5);
}

TEST_CASE("a buffer reads back through chunks wherever the gap stands") {
    PrTextEditBuffer* b = prTextEditBufferCreate("hello world");
    CHECK(textOf(b) == "hello world");
    replace(b, 5, 5, ",");
    CHECK(textOf(b) == "hello, world");
    replace(b, 0, 1, "J");
    CHECK(textOf(b) == "Jello, world");
    replace(b, 7, 12, "there");
    CHECK(textOf(b) == "Jello, there");

    char copied[5];
    prTextEditBufferCopy(b, 3, 8, copied);
    CHECK(std::string(copied, 5) == "lo, t");

    CHECK(prTextEditBufferText(b) == "Jello, there");
    prTextEditBufferDestroy(b);
}

TEST_CASE("an empty buffer has one empty line and contiguous empty text") {
    PrTextEditBuffer* b = prTextEditBufferCreate("");
    CHECK(prTextEditBufferSize(b) == 0);
    CHECK(prTextEditBufferLineCount(b) == 1);
    CHECK(prTextEditBufferLineStart(b, 0) == 0);
    CHECK(prTextEditBufferLineEnd(b, 0) == 0);
    CHECK(prTextEditBufferText(b).size == 0);
    CHECK(prTextEditBufferText(b).data != nullptr);
    prTextEditBufferDestroy(b);
}

TEST_CASE("the line index follows edits that add and remove lines") {
    PrTextEditBuffer* b = prTextEditBufferCreate("one\ntwo\nthree\n");
    REQUIRE(prTextEditBufferLineCount(b) == 4);
    CHECK(prTextEditBufferLineStart(b, 1) == 4);
    CHECK(prTextEditBufferLineEnd(b, 1) == 7);
    CHECK(prTextEditBufferLineStart(b, 3) == 14);
    CHECK(prTextEditBufferLineEnd(b, 3) == 14);

    replace(b, 4, 4, "a\nb\n");
    CHECK(textOf(b) == "one\na\nb\ntwo\nthree\n");
    REQUIRE(prTextEditBufferLineCount(b) == 6);
    CHECK(prTextEditBufferLineStart(b, 2) == 6);
    CHECK(prTextEditBufferLineStart(b, 3) == 8);
    CHECK(prTextEditBufferLineStart(b, 5) == 18);

    // An edit far before the lines after it leaves them correct, and one that spans lines removes them.
    replace(b, 0, 0, "zzz");
    CHECK(prTextEditBufferLineStart(b, 4) == 15);
    replace(b, 3, 11, "");
    CHECK(textOf(b) == "zzztwo\nthree\n");
    REQUIRE(prTextEditBufferLineCount(b) == 3);
    CHECK(prTextEditBufferLineStart(b, 1) == 7);
    CHECK(prTextEditBufferLineStart(b, 2) == 13);

    PrTextEditPoint point = prTextEditBufferPointFromOffset(b, 9);
    CHECK(point.line == 1);
    CHECK(point.column == 2);
    CHECK(prTextEditBufferOffsetFromPoint(b, point) == 9);
    CHECK(prTextEditBufferOffsetFromPoint(b, PrTextEditPoint{.line = 0, .column = 100}) == 6);
    prTextEditBufferDestroy(b);
}

TEST_CASE("a transaction's changes are all in the coordinates of the text before it") {
    PrTextEditBuffer* b = prTextEditBufferCreate("aXbXc");
    PrTextEditChange changes[] = {change(1, 2, "long"), change(3, 4, "")};
    uint64_t version = prTextEditBufferVersion(b);
    REQUIRE(prTextEditBufferApply(b, changes, 2, PrTextEditOrigin_Local));
    CHECK(textOf(b) == "alongbc");
    CHECK(prTextEditBufferVersion(b) == version + 1);
    prTextEditBufferDestroy(b);
}

TEST_CASE("a change that does nothing makes no version") {
    PrTextEditBuffer* b = prTextEditBufferCreate("abc");
    uint64_t version = prTextEditBufferVersion(b);
    replace(b, 1, 1, "");
    CHECK(prTextEditBufferVersion(b) == version);
    prTextEditBufferDestroy(b);
}

TEST_CASE("each stream reads every record at its own pace") {
    PrTextEditBuffer* b = prTextEditBufferCreate("ab\ncd");
    PrTextEditStream* fast = prTextEditStreamCreate(b);
    PrTextEditStream* slow = prTextEditStreamCreate(b);
    PrTextEditRecord records[4];

    replace(b, 3, 4, "xy\nz");
    REQUIRE(prTextEditStreamTake(fast, records, 4) == 1);
    CHECK(records[0].start == 3);
    CHECK(records[0].oldEnd == 4);
    CHECK(records[0].newEnd == 7);
    CHECK(records[0].startPoint.line == 1);
    CHECK(records[0].startPoint.column == 0);
    CHECK(records[0].oldEndPoint.line == 1);
    CHECK(records[0].oldEndPoint.column == 1);
    CHECK(records[0].newEndPoint.line == 2);
    CHECK(records[0].newEndPoint.column == 1);
    CHECK(records[0].version == prTextEditBufferVersion(b));
    CHECK(records[0].origin == PrTextEditOrigin_Local);
    CHECK(prTextEditStreamTake(fast, records, 4) == 0);

    replace(b, 0, 1, "");
    CHECK(prTextEditStreamTake(fast, records, 4) == 1);
    CHECK(prTextEditStreamTake(slow, records, 1) == 1);
    CHECK(records[0].start == 3);
    CHECK(prTextEditStreamTake(slow, records, 4) == 1);
    CHECK(records[0].start == 0);

    prTextEditStreamDestroy(fast);
    prTextEditStreamDestroy(slow);
    prTextEditBufferDestroy(b);
}

TEST_CASE("a stream's records apply in order, each correct for the text that the one before it left") {
    PrTextEditBuffer* b = prTextEditBufferCreate("0123456789");
    PrTextEditStream* stream = prTextEditStreamCreate(b);
    PrTextEditChange changes[] = {change(1, 2, "aa"), change(5, 6, "bbb")};
    REQUIRE(prTextEditBufferApply(b, changes, 2, PrTextEditOrigin_Local));

    std::string replica = "0123456789";
    PrTextEditRecord records[4];
    uint32_t count = prTextEditStreamTake(stream, records, 4);
    REQUIRE(count == 2);
    std::string texts[] = {"bbb", "aa"};
    for (uint32_t i = 0; i < count; ++i)
        replica.replace(records[i].start, records[i].oldEnd - records[i].start, texts[i]);
    CHECK(replica == textOf(b));

    prTextEditStreamDestroy(stream);
    prTextEditBufferDestroy(b);
}

TEST_CASE("an anchor moves by the rules of its bias") {
    PrTextEditBuffer* b = prTextEditBufferCreate("0123456789");
    PrTextEditAnchor before = prTextEditAnchorCreate(b, 2, PrTextEditBias_Left);
    PrTextEditAnchor left = prTextEditAnchorCreate(b, 4, PrTextEditBias_Left);
    PrTextEditAnchor right = prTextEditAnchorCreate(b, 4, PrTextEditBias_Right);
    PrTextEditAnchor inside = prTextEditAnchorCreate(b, 5, PrTextEditBias_Right);
    PrTextEditAnchor end = prTextEditAnchorCreate(b, 6, PrTextEditBias_Left);
    PrTextEditAnchor after = prTextEditAnchorCreate(b, 8, PrTextEditBias_Left);

    replace(b, 4, 6, "abc");

    CHECK(prTextEditAnchorOffset(b, before) == 2);
    CHECK(prTextEditAnchorOffset(b, left) == 4);
    CHECK(prTextEditAnchorOffset(b, right) == 7);
    CHECK(prTextEditAnchorOffset(b, inside) == 4);
    CHECK(prTextEditAnchorOffset(b, end) == 7);
    CHECK(prTextEditAnchorOffset(b, after) == 9);
    CHECK(prTextEditAnchorIsLive(b, before));
    CHECK(prTextEditAnchorIsLive(b, right));
    CHECK(prTextEditAnchorIsLive(b, end));
    CHECK_FALSE(prTextEditAnchorIsLive(b, inside));

    // An anchor that stopped being live stays so, and continues to follow the text.
    replace(b, 0, 0, "zz");
    CHECK(prTextEditAnchorOffset(b, inside) == 6);
    CHECK_FALSE(prTextEditAnchorIsLive(b, inside));
    prTextEditBufferDestroy(b);
}

TEST_CASE("an anchor moves through every change of a transaction") {
    PrTextEditBuffer* b = prTextEditBufferCreate("0123456789");
    PrTextEditAnchor a = prTextEditAnchorCreate(b, 5, PrTextEditBias_Left);
    PrTextEditChange changes[] = {change(1, 1, "xxx"), change(3, 4, ""), change(7, 8, "yy")};
    REQUIRE(prTextEditBufferApply(b, changes, 3, PrTextEditOrigin_Local));
    CHECK(textOf(b) == "0xxx12456yy89");
    CHECK(prTextEditAnchorOffset(b, a) == 7);
    prTextEditBufferDestroy(b);
}

TEST_CASE("a removed anchor's value can be reused") {
    PrTextEditBuffer* b = prTextEditBufferCreate("abc");
    PrTextEditAnchor first = prTextEditAnchorCreate(b, 1, PrTextEditBias_Left);
    prTextEditAnchorRemove(b, first);
    PrTextEditAnchor second = prTextEditAnchorCreate(b, 2, PrTextEditBias_Left);
    CHECK(second == first);
    CHECK(second != kPrTextEditAnchorNone);
    CHECK(prTextEditAnchorOffset(b, second) == 2);
    prTextEditBufferDestroy(b);
}

TEST_CASE("insertion replaces every selection, and each caret lands after its text") {
    PrTextEditBuffer* b = prTextEditBufferCreate("aa bb cc");
    PrTextEditView* v = prTextEditViewCreate(b);
    PrTextEditSelection selections[] = {{.anchor = 6, .caret = 8}, {.anchor = 0, .caret = 2}};
    prTextEditViewSetSelections(v, selections, 2);
    REQUIRE(prTextEditViewInsert(v, "XYZ", PrTextEditOrigin_Local, PrTextEditMerge_None));
    CHECK(textOf(b) == "XYZ bb XYZ");
    REQUIRE(prTextEditViewSelectionCount(v) == 2);
    CHECK(prTextEditViewPrimary(v).caret == 3);
    CHECK(prTextEditViewPrimary(v).anchor == 3);
    CHECK(prTextEditViewSelections(v)[0].caret == 10);
    prTextEditViewDestroy(v);
    prTextEditBufferDestroy(b);
}

TEST_CASE("selections that overlap join, and the primary selection stays last") {
    PrTextEditBuffer* b = prTextEditBufferCreate("0123456789");
    PrTextEditView* v = prTextEditViewCreate(b);

    PrTextEditSelection overlapping[] = {{.anchor = 1, .caret = 4}, {.anchor = 8, .caret = 9}, {.anchor = 5, .caret = 3}};
    prTextEditViewSetSelections(v, overlapping, 3);
    REQUIRE(prTextEditViewSelectionCount(v) == 2);
    CHECK(prTextEditViewSelections(v)[0].caret == 9);
    CHECK(prTextEditViewPrimary(v).anchor == 5);
    CHECK(prTextEditViewPrimary(v).caret == 1);

    // Ranges that only touch stay apart, and a caret that touches a range joins it.
    PrTextEditSelection touching[] = {{.anchor = 1, .caret = 3}, {.anchor = 3, .caret = 5}};
    prTextEditViewSetSelections(v, touching, 2);
    CHECK(prTextEditViewSelectionCount(v) == 2);
    PrTextEditSelection caret[] = {{.anchor = 1, .caret = 3}, {.anchor = 3, .caret = 3}};
    prTextEditViewSetSelections(v, caret, 2);
    CHECK(prTextEditViewSelectionCount(v) == 1);

    PrTextEditSelection past{.anchor = 100, .caret = 200};
    prTextEditViewSetSelections(v, &past, 1);
    CHECK(prTextEditViewPrimary(v).anchor == 10);
    CHECK(prTextEditViewPrimary(v).caret == 10);

    prTextEditViewDestroy(v);
    prTextEditBufferDestroy(b);
}

TEST_CASE("an edit through one view carries the selections of another") {
    PrTextEditBuffer* b = prTextEditBufferCreate("0123456789");
    PrTextEditView* editing = prTextEditViewCreate(b);
    PrTextEditView* other = prTextEditViewCreate(b);

    select(other, 5, 5);
    select(editing, 2, 2);
    REQUIRE(prTextEditViewInsert(editing, "ab", PrTextEditOrigin_Local, PrTextEditMerge_None));
    CHECK(prTextEditViewPrimary(other).caret == 7);
    CHECK(prTextEditViewPrimary(editing).caret == 4);

    // Text inserted at either boundary of a range of another view stays outside it, and a caret there stays before it.
    select(other, 5, 8);
    select(editing, 5, 5);
    REQUIRE(prTextEditViewInsert(editing, "c", PrTextEditOrigin_Local, PrTextEditMerge_None));
    CHECK(prTextEditViewPrimary(other).anchor == 6);
    CHECK(prTextEditViewPrimary(other).caret == 9);
    select(editing, 9, 9);
    REQUIRE(prTextEditViewInsert(editing, "d", PrTextEditOrigin_Local, PrTextEditMerge_None));
    CHECK(prTextEditViewPrimary(other).caret == 9);

    prTextEditViewDestroy(editing);
    prTextEditViewDestroy(other);
    prTextEditBufferDestroy(b);
}

TEST_CASE("a proposal can reject a transaction or change its text") {
    struct Filter {
        bool reject = false;
        std::string text;
        PrTextEditOrigin origin = PrTextEditOrigin_Remote;
    } filter;
    PrTextEditProposeFn propose = [](void* context, PrTextEditOrigin origin, PrTextEditChange* changes, uint32_t) {
        Filter* f = static_cast<Filter*>(context);
        f->origin = origin;
        if (f->reject)
            return false;
        changes[0].text = f->text;
        return true;
    };

    PrTextEditBuffer* b = prTextEditBufferCreate("ab");
    PrTextEditView* v = prTextEditViewCreate(b);
    prTextEditBufferSetPropose(b, propose, &filter);
    select(v, 1, 1);

    filter.reject = true;
    uint64_t version = prTextEditBufferVersion(b);
    CHECK_FALSE(prTextEditViewInsert(v, "typed", PrTextEditOrigin_Local, PrTextEditMerge_None));
    CHECK(textOf(b) == "ab");
    CHECK(prTextEditBufferVersion(b) == version);
    CHECK(prTextEditViewPrimary(v).caret == 1);
    CHECK(filter.origin == PrTextEditOrigin_Local);

    // The caret follows the text that the proposal gave.
    filter.reject = false;
    filter.text = "xyz";
    CHECK(prTextEditViewInsert(v, "typed", PrTextEditOrigin_Composition, PrTextEditMerge_None));
    CHECK(textOf(b) == "axyzb");
    CHECK(prTextEditViewPrimary(v).caret == 4);
    CHECK(filter.origin == PrTextEditOrigin_Composition);

    // Remote and program transactions are not proposed against.
    filter.reject = true;
    prTextEditBufferSetText(b, "set");
    CHECK(textOf(b) == "set");

    prTextEditViewDestroy(v);
    prTextEditBufferDestroy(b);
}

TEST_CASE("undo and redo restore the text and the selections") {
    PrTextEditBuffer* b = prTextEditBufferCreate("aa bb");
    PrTextEditHistory* h = prTextEditHistoryCreate(b);
    PrTextEditView* v = prTextEditViewCreate(b);

    CHECK_FALSE(prTextEditHistoryCanUndo(h));
    select(v, 3, 5);
    REQUIRE(prTextEditViewInsert(v, "XYZ", PrTextEditOrigin_Local, PrTextEditMerge_None));
    CHECK(textOf(b) == "aa XYZ");
    CHECK(prTextEditHistoryCanUndo(h));

    REQUIRE(prTextEditHistoryUndo(h, v));
    CHECK(textOf(b) == "aa bb");
    CHECK(prTextEditViewPrimary(v).anchor == 3);
    CHECK(prTextEditViewPrimary(v).caret == 5);
    CHECK(prTextEditHistoryCanRedo(h));
    CHECK_FALSE(prTextEditHistoryCanUndo(h));

    REQUIRE(prTextEditHistoryRedo(h, v));
    CHECK(textOf(b) == "aa XYZ");
    CHECK(prTextEditViewPrimary(v).caret == 6);
    CHECK_FALSE(prTextEditHistoryRedo(h, v));

    // A new edit drops what could be redone.
    REQUIRE(prTextEditHistoryUndo(h, v));
    select(v, 0, 0);
    REQUIRE(prTextEditViewInsert(v, "n", PrTextEditOrigin_Local, PrTextEditMerge_None));
    CHECK_FALSE(prTextEditHistoryCanRedo(h));

    prTextEditViewDestroy(v);
    prTextEditHistoryDestroy(h);
    prTextEditBufferDestroy(b);
}

TEST_CASE("undo reverts every change of a transaction") {
    PrTextEditBuffer* b = prTextEditBufferCreate("aa bb cc");
    PrTextEditHistory* h = prTextEditHistoryCreate(b);
    PrTextEditView* v = prTextEditViewCreate(b);
    PrTextEditSelection selections[] = {{.anchor = 0, .caret = 2}, {.anchor = 6, .caret = 8}};
    prTextEditViewSetSelections(v, selections, 2);
    REQUIRE(prTextEditViewInsert(v, "longer", PrTextEditOrigin_Local, PrTextEditMerge_None));
    CHECK(textOf(b) == "longer bb longer");
    REQUIRE(prTextEditHistoryUndo(h, v));
    CHECK(textOf(b) == "aa bb cc");
    CHECK(prTextEditViewSelectionCount(v) == 2);
    REQUIRE(prTextEditHistoryRedo(h, v));
    CHECK(textOf(b) == "longer bb longer");
    prTextEditViewDestroy(v);
    prTextEditHistoryDestroy(h);
    prTextEditBufferDestroy(b);
}

TEST_CASE("a run of typing is one step until it is broken or the caret moves") {
    PrTextEditBuffer* b = prTextEditBufferCreate("");
    PrTextEditHistory* h = prTextEditHistoryCreate(b);
    PrTextEditView* v = prTextEditViewCreate(b);

    for (const char* letter : {"a", "b", "c"})
        REQUIRE(prTextEditViewInsert(v, letter, PrTextEditOrigin_Local, PrTextEditMerge_Typing));
    prTextEditHistoryBreak(h);
    REQUIRE(prTextEditViewInsert(v, "d", PrTextEditOrigin_Local, PrTextEditMerge_Typing));
    select(v, 0, 0);
    REQUIRE(prTextEditViewInsert(v, "e", PrTextEditOrigin_Local, PrTextEditMerge_Typing));
    CHECK(textOf(b) == "eabcd");

    REQUIRE(prTextEditHistoryUndo(h, v));
    CHECK(textOf(b) == "abcd");
    REQUIRE(prTextEditHistoryUndo(h, v));
    CHECK(textOf(b) == "abc");
    REQUIRE(prTextEditHistoryUndo(h, v));
    CHECK(textOf(b) == "");
    CHECK_FALSE(prTextEditHistoryCanUndo(h));

    REQUIRE(prTextEditHistoryRedo(h, v));
    CHECK(textOf(b) == "abc");

    prTextEditViewDestroy(v);
    prTextEditHistoryDestroy(h);
    prTextEditBufferDestroy(b);
}

TEST_CASE("runs of deletion in either direction are one step") {
    PrTextEditBuffer* b = prTextEditBufferCreate("abcdef");
    PrTextEditHistory* h = prTextEditHistoryCreate(b);
    PrTextEditView* v = prTextEditViewCreate(b);

    SUBCASE("backward") {
        select(v, 3, 3);
        erase(v, 2, 3, PrTextEditMerge_Deleting);
        erase(v, 1, 2, PrTextEditMerge_Deleting);
        CHECK(textOf(b) == "adef");
    }
    SUBCASE("forward") {
        select(v, 1, 1);
        erase(v, 1, 2, PrTextEditMerge_Deleting);
        erase(v, 1, 2, PrTextEditMerge_Deleting);
        CHECK(textOf(b) == "adef");
    }
    REQUIRE(prTextEditHistoryUndo(h, v));
    CHECK(textOf(b) == "abcdef");
    CHECK_FALSE(prTextEditHistoryCanUndo(h));

    prTextEditViewDestroy(v);
    prTextEditHistoryDestroy(h);
    prTextEditBufferDestroy(b);
}

TEST_CASE("only local transactions enter the history, and remote and program ones empty it") {
    PrTextEditBuffer* b = prTextEditBufferCreate("abc");
    PrTextEditHistory* h = prTextEditHistoryCreate(b);
    PrTextEditView* v = prTextEditViewCreate(b);

    REQUIRE(prTextEditViewInsert(v, "x", PrTextEditOrigin_Composition, PrTextEditMerge_None));
    CHECK_FALSE(prTextEditHistoryCanUndo(h));

    REQUIRE(prTextEditViewInsert(v, "y", PrTextEditOrigin_Local, PrTextEditMerge_None));
    CHECK(prTextEditHistoryCanUndo(h));

    SUBCASE("remote") {
        PrTextEditChange c = change(0, 0, "r");
        REQUIRE(prTextEditBufferApply(b, &c, 1, PrTextEditOrigin_Remote));
    }
    SUBCASE("program") {
        prTextEditBufferSetText(b, "set");
    }
    CHECK_FALSE(prTextEditHistoryCanUndo(h));

    prTextEditViewDestroy(v);
    prTextEditHistoryDestroy(h);
    prTextEditBufferDestroy(b);
}

TEST_CASE("text from outside the program keeps what an edit asks for, with each line ending a line feed") {
    auto normalized = [](PrStringView text, PrTextEditInputFlags keep) {
        std::string out(text.size, '\0');
        out.resize(prTextEditInputNormalize(text, keep, out.data()));
        return out;
    };
    PrStringView text = "a\r\nb\rc\nd\te\x01" "f\x7f";
    CHECK(normalized(text, 0) == "abcdef");
    CHECK(normalized(text, PrTextEditInputFlag_LineFeeds) == "a\nb\nc\ndef");
    CHECK(normalized(text, PrTextEditInputFlag_LineFeeds | PrTextEditInputFlag_Tabs) == "a\nb\nc\nd\tef");
    uint8_t all = PrTextEditInputFlag_LineFeeds | PrTextEditInputFlag_Tabs | PrTextEditInputFlag_Controls;
    CHECK(normalized(text, all) == "a\nb\nc\nd\te\x01" "f\x7f");
    CHECK(normalized("", all).empty());
}

TEST_CASE("a selection's ends are in the order of the text, and the last selection is primary") {
    PrTextEditSelection backward{.anchor = 5, .caret = 2};
    CHECK(prTextEditSelectionStart(backward) == 2);
    CHECK(prTextEditSelectionEnd(backward) == 5);
    PrTextEditBuffer* b = prTextEditBufferCreate("abcdefgh");
    PrTextEditView* v = prTextEditViewCreate(b);
    PrTextEditSelection two[2] = {{.anchor = 6, .caret = 7}, {.anchor = 1, .caret = 1}};
    prTextEditViewSetSelections(v, two, 2);
    CHECK(prTextEditViewPrimary(v).caret == 1);
    prTextEditViewDestroy(v);
    prTextEditBufferDestroy(b);
}

TEST_CASE("text that loads at the end stays out of the history, and keeps its steps") {
    PrTextEditBuffer* b = prTextEditBufferCreate("abc");
    PrTextEditHistory* h = prTextEditHistoryCreate(b);
    PrTextEditView* v = prTextEditViewCreate(b);
    REQUIRE(prTextEditViewInsert(v, "x", PrTextEditOrigin_Local, PrTextEditMerge_None));
    PrTextEditChange load = change(4, 4, "\ndef");
    REQUIRE(prTextEditBufferApply(b, &load, 1, PrTextEditOrigin_Load));
    CHECK(textOf(b) == "xabc\ndef");
    REQUIRE(prTextEditHistoryUndo(h, v));
    CHECK(textOf(b) == "abc\ndef");
    CHECK_FALSE(prTextEditHistoryCanUndo(h));
    REQUIRE(prTextEditHistoryRedo(h, v));
    CHECK(textOf(b) == "xabc\ndef");

    prTextEditViewDestroy(v);
    prTextEditHistoryDestroy(h);
    prTextEditBufferDestroy(b);
}

TEST_CASE("what is attached to a buffer stays safe after the buffer is destroyed") {
    PrTextEditBuffer* b = prTextEditBufferCreate("abc");
    PrTextEditView* v = prTextEditViewCreate(b);
    PrTextEditStream* s = prTextEditStreamCreate(b);
    PrTextEditHistory* h = prTextEditHistoryCreate(b);
    REQUIRE(prTextEditViewInsert(v, "x", PrTextEditOrigin_Local, PrTextEditMerge_None));
    prTextEditBufferDestroy(b);

    CHECK(prTextEditViewBuffer(v) == nullptr);
    CHECK_FALSE(prTextEditViewInsert(v, "y", PrTextEditOrigin_Local, PrTextEditMerge_None));
    PrTextEditRecord record;
    CHECK(prTextEditStreamTake(s, &record, 1) == 0);
    CHECK_FALSE(prTextEditHistoryUndo(h, v));

    prTextEditHistoryDestroy(h);
    prTextEditStreamDestroy(s);
    prTextEditViewDestroy(v);
}

TEST_CASE("a stream's records carry what a replica needs, whichever stream reads first") {
    PrTextEditBuffer* b = prTextEditBufferCreate("abc");
    PrTextEditStream* fast = prTextEditStreamCreate(b);
    PrTextEditStream* slow = prTextEditStreamCreate(b);
    PrTextEditRecord record;

    replace(b, 1, 1, "one");
    REQUIRE(prTextEditStreamTake(fast, &record, 1) == 1);
    CHECK(record.text == "one");
    replace(b, 0, 0, "two");
    REQUIRE(prTextEditStreamTake(fast, &record, 1) == 1);
    CHECK(record.text == "two");
    REQUIRE(prTextEditStreamTake(slow, &record, 1) == 1);
    CHECK(record.text == "one");
    REQUIRE(prTextEditStreamTake(slow, &record, 1) == 1);
    CHECK(record.text == "two");

    prTextEditStreamDestroy(fast);
    prTextEditStreamDestroy(slow);
    prTextEditBufferDestroy(b);
}

TEST_CASE("random transactions agree across the text, lines, anchors, views, streams and history") {
    for (uint32_t seed : {1u, 2u, 3u, 4u, 5u}) {
        uint32_t state = seed * 2654435761u;
        auto next = [&](uint32_t bound) {
            state = state * 1664525u + 1013904223u;
            return (state >> 8) % bound;
        };
        std::string model = "first line\nsecond line\n\nfourth\n";
        PrTextEditBuffer* b = prTextEditBufferCreate(model);
        PrTextEditView* editing = prTextEditViewCreate(b);
        PrTextEditView* other = prTextEditViewCreate(b);
        PrTextEditStream* stream = prTextEditStreamCreate(b);
        PrTextEditHistory* history = prTextEditHistoryCreate(b);
        std::string replica = model;

        std::vector<ModelAnchor> anchors;
        for (uint32_t i = 0; i < 40; ++i) {
            PrTextEditBias bias = i % 2 != 0 ? PrTextEditBias_Right : PrTextEditBias_Left;
            uint32_t at = next(uint32_t(model.size()) + 1);
            anchors.push_back({prTextEditAnchorCreate(b, at, bias), bias, at, true});
        }
        std::vector<std::string> before;

        for (uint32_t step = 0; step < 300; ++step) {
            // Up to three changes in ascending order, which do not touch.
            const char* texts[] = {"", "x", "a\nb", "\n", "longer text", "\xc3\xa9"};
            std::vector<PrTextEditChange> changes;
            uint32_t at = next(uint32_t(model.size()) / 2 + 1);
            uint32_t count = next(3) + 1;
            for (uint32_t i = 0; i < count && at <= model.size(); ++i) {
                uint32_t end = at + next(6);
                end = end < model.size() ? end : uint32_t(model.size());
                changes.push_back(change(at, end, texts[next(6)]));
                at = end + 1 + next(20);
            }
            select(other, next(uint32_t(model.size()) + 1), next(uint32_t(model.size()) + 1));

            before.push_back(model);
            uint64_t version = prTextEditBufferVersion(b);
            REQUIRE(prTextEditViewApply(editing, changes.data(), uint32_t(changes.size()), PrTextEditOrigin_Local,
                PrTextEditMerge_None));
            for (size_t i = changes.size(); i-- > 0;) {
                const PrTextEditChange& c = changes[i];
                model.replace(c.start, c.end - c.start, c.text.data, c.text.size);
                for (ModelAnchor& a : anchors)
                    moveModel(a, c.start, c.end, uint32_t(c.text.size));
            }
            if (prTextEditBufferVersion(b) == version)
                before.pop_back();

            REQUIRE(textOf(b) == model);
            applyRecords(stream, replica);
            REQUIRE(replica == model);
            for (const ModelAnchor& a : anchors) {
                REQUIRE(prTextEditAnchorOffset(b, a.anchor) == a.offset);
                REQUIRE(prTextEditAnchorIsLive(b, a.anchor) == a.live);
            }
            for (const PrTextEditView* v : {editing, other}) {
                const PrTextEditSelection& s = prTextEditViewPrimary(v);
                REQUIRE(s.anchor <= model.size());
                REQUIRE(s.caret <= model.size());
            }
        }
        uint32_t lines = 1;
        for (char c : model)
            lines += c == '\n' ? 1 : 0;
        CHECK(prTextEditBufferLineCount(b) == lines);

        // Undoing every step passes through every earlier text, and redoing returns to the last.
        std::string last = model;
        for (size_t i = before.size(); i-- > 0;) {
            REQUIRE(prTextEditHistoryUndo(history, editing));
            REQUIRE(textOf(b) == before[i]);
            applyRecords(stream, replica);
            REQUIRE(replica == before[i]);
        }
        CHECK_FALSE(prTextEditHistoryCanUndo(history));
        while (prTextEditHistoryCanRedo(history))
            REQUIRE(prTextEditHistoryRedo(history, editing));
        CHECK(textOf(b) == last);
        applyRecords(stream, replica);
        CHECK(replica == last);

        prTextEditHistoryDestroy(history);
        prTextEditStreamDestroy(stream);
        prTextEditViewDestroy(other);
        prTextEditViewDestroy(editing);
        prTextEditBufferDestroy(b);
    }
}

TEST_CASE("random runs of typing and deleting merge into steps that undo through earlier texts in order") {
    for (uint32_t seed : {11u, 12u, 13u, 14u, 15u}) {
        uint32_t state = seed * 2654435761u;
        auto next = [&](uint32_t bound) {
            state = state * 1664525u + 1013904223u;
            return (state >> 8) % bound;
        };
        std::string model = "the quick brown fox";
        PrTextEditBuffer* b = prTextEditBufferCreate(model);
        PrTextEditHistory* h = prTextEditHistoryCreate(b);
        PrTextEditView* v = prTextEditViewCreate(b);
        std::vector<std::string> texts{model};

        for (uint32_t step = 0; step < 400; ++step) {
            uint32_t caret = prTextEditViewPrimary(v).caret;
            char letter = 0;
            switch (next(8)) {
                case 0:
                    select(v, next(uint32_t(model.size()) + 1), next(uint32_t(model.size()) + 1));
                    break;
                case 1:
                    prTextEditHistoryBreak(h);
                    break;
                case 2:
                case 3:
                case 4:
                    letter = char('a' + step % 26);
                    REQUIRE(prTextEditViewInsert(v, PrStringView{&letter, 1}, PrTextEditOrigin_Local,
                        PrTextEditMerge_Typing));
                    break;
                case 5:
                case 6:
                    if (caret > 0 && prTextEditViewPrimary(v).anchor == caret)
                        erase(v, caret - 1, caret, PrTextEditMerge_Deleting);
                    break;
                default:
                    if (caret < model.size() && prTextEditViewPrimary(v).anchor == caret)
                        erase(v, caret, caret + 1, PrTextEditMerge_Deleting);
                    break;
            }
            model = textOf(b);
            if (model != texts.back())
                texts.push_back(model);
        }

        // Each undo lands on the current text or an earlier one, so the search for it only moves back. A step can leave
        // the text as it was, by replacing a selection with the same letter.
        size_t at = texts.size() - 1;
        uint32_t steps = 0;
        while (prTextEditHistoryCanUndo(h)) {
            REQUIRE(prTextEditHistoryUndo(h, v));
            ++steps;
            std::string text = textOf(b);
            while (at > 0 && texts[at] != text)
                --at;
            REQUIRE(texts[at] == text);
            REQUIRE(prTextEditViewPrimary(v).caret <= text.size());
        }
        CHECK(at == 0);
        CHECK(steps < texts.size() - 1);
        while (prTextEditHistoryCanRedo(h))
            REQUIRE(prTextEditHistoryRedo(h, v));
        CHECK(textOf(b) == texts.back());

        prTextEditViewDestroy(v);
        prTextEditHistoryDestroy(h);
        prTextEditBufferDestroy(b);
    }
}

TEST_CASE("many edits at scattered places agree with a plain string") {
    std::string model = "first line\nsecond line\nthird\n";
    PrTextEditBuffer* b = prTextEditBufferCreate(model);
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < 2000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        uint32_t start = (seed >> 8) % (uint32_t(model.size()) + 1);
        seed = seed * 1664525u + 1013904223u;
        uint32_t length = (seed >> 8) % 6;
        uint32_t end = start + length < model.size() ? start + length : uint32_t(model.size());
        const char* texts[] = {"", "x", "a\nb", "\n", "longer text"};
        std::string text = texts[(seed >> 20) % 5];
        replace(b, start, end, text);
        model.replace(start, end - start, text);
    }
    CHECK(textOf(b) == model);

    std::vector<uint32_t> starts{0};
    for (size_t i = 0; i < model.size(); ++i) {
        if (model[i] == '\n')
            starts.push_back(uint32_t(i) + 1);
    }
    REQUIRE(prTextEditBufferLineCount(b) == starts.size());
    for (uint32_t line = 0; line < starts.size(); ++line)
        CHECK(prTextEditBufferLineStart(b, line) == starts[line]);
    prTextEditBufferDestroy(b);
}

TEST_CASE("an editor's units follow its text's clusters, words and lines") {
    PrStringView text = "one two\nthree";
    CHECK(prTextEditWordAfter(text, 0) == 3);
    CHECK(prTextEditWordAfter(text, 3) == 7);
    CHECK(prTextEditWordBefore(text, 7) == 4);
    CHECK(prTextEditLineStart(text, 10) == 8);
    CHECK(prTextEditLineEnd(text, 2) == 7);
    uint32_t low = 0;
    uint32_t high = 0;
    prTextEditUnitAround(text, 5, PrTextEditUnit_Word, &low, &high);
    CHECK(low == 4);
    CHECK(high == 7);
    prTextEditUnitAround(text, 9, PrTextEditUnit_Line, &low, &high);
    CHECK(low == 8);
    CHECK(high == 13);
    PrStringView accent = "e\xcc\x81x";
    CHECK(prTextEditClusterAfter(accent, 0) == 3);
    CHECK(prTextEditClusterBefore(accent, 3) == 0);
}

TEST_CASE("a key press names a command") {
    bool extend = false;
    PrAppEvent left{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = PrAppKey_Left}};
    CHECK(prTextEditCommandFromKey(&left, &extend) == PrTextEditCommand_Left);
    CHECK(!extend);
    left.modifiers = PrAppModifierFlag_Alt | PrAppModifierFlag_Shift;
    CHECK(prTextEditCommandFromKey(&left, &extend) == PrTextEditCommand_WordLeft);
    CHECK(extend);
    PrAppEvent undo{.kind = PrAppEventKind_KeyDown, .modifiers = PrAppModifierFlag_Super, .keyboard = {.key = 'Z'}};
    CHECK(prTextEditCommandFromKey(&undo, &extend) == PrTextEditCommand_Undo);
    undo.modifiers = PrAppModifierFlag_Super | PrAppModifierFlag_Shift;
    CHECK(prTextEditCommandFromKey(&undo, &extend) == PrTextEditCommand_Redo);
    PrAppEvent letter{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = 'Z'}};
    CHECK(prTextEditCommandFromKey(&letter, &extend) == PrTextEditCommand_None);
}

TEST_CASE("an editor's words are UAX #29's, with runs of Han and of Hiragana whole") {
    uint32_t low = 0;
    uint32_t high = 0;
    PrStringView english = "don't stop, 3.14 now";
    prTextEditUnitAround(english, 1, PrTextEditUnit_Word, &low, &high);
    CHECK(low == 0);
    CHECK(high == 5);
    prTextEditUnitAround(english, 13, PrTextEditUnit_Word, &low, &high);
    CHECK(low == 12);
    CHECK(high == 16);
    CHECK(prTextEditWordAfter(english, 0) == 5);
    CHECK(prTextEditWordBefore(english, 12) == 6);
    // Katakana, then Han, then Hiragana: each script's run is its own word.
    PrStringView japanese = "\xe3\x82\xab\xe3\x82\xbf\xe6\xbc\xa2\xe5\xad\x97\xe3\x81\xb2\xe3\x82\x89";
    prTextEditUnitAround(japanese, 0, PrTextEditUnit_Word, &low, &high);
    CHECK(low == 0);
    CHECK(high == 6);
    prTextEditUnitAround(japanese, 6, PrTextEditUnit_Word, &low, &high);
    CHECK(low == 6);
    CHECK(high == 12);
    CHECK(prTextEditWordAfter(japanese, 12) == 18);
    CHECK(prTextEditWordBefore(japanese, 18) == 12);
}

TEST_CASE("a buffer with no styles has no runs, and setting style zero back removes them") {
    PrTextEditBuffer* b = prTextEditBufferCreate("abcdef");
    CHECK(prTextEditBufferStyleRunCount(b) == 0);
    CHECK(prTextEditBufferStyleAt(b, 3) == 0);
    prTextEditBufferSetStyle(b, 2, 4, 7);
    CHECK(runsOf(b) == Runs{0, 0, 2, 7, 4, 0});
    CHECK(prTextEditBufferStyleAt(b, 3) == 7);
    prTextEditBufferSetStyle(b, 2, 4, 0);
    CHECK(prTextEditBufferStyleRunCount(b) == 0);
    prTextEditBufferDestroy(b);
}

TEST_CASE("style runs follow edits, and inserted text takes the style of what it replaces or follows") {
    struct Case {
        const char* name;
        uint32_t start;
        uint32_t end;
        const char* text;
        Runs expected;
    };
    const Case cases[] = {
        {"before the run", 1, 1, "x", {0, 0, 3, 7, 5, 0}},
        {"at the run's start joins the text before it", 2, 2, "x", {0, 0, 3, 7, 5, 0}},
        {"inside the run", 3, 3, "x", {0, 0, 2, 7, 5, 0}},
        {"at the run's end joins the run", 4, 4, "x", {0, 0, 2, 7, 5, 0}},
        {"after the run", 5, 5, "x", {0, 0, 2, 7, 4, 0}},
        {"replacing the run takes its style", 2, 4, "xyz", {0, 0, 2, 7, 5, 0}},
        {"removing the run removes it", 2, 4, "", {}},
        {"replacing across the run takes the first byte's style", 1, 3, "x", {0, 0, 2, 7, 3, 0}},
        {"at the start takes the first style", 0, 0, "x", {0, 0, 3, 7, 5, 0}},
    };
    for (const Case& c : cases) {
        INFO(std::string(c.name));
        PrTextEditBuffer* b = prTextEditBufferCreate("abcdef");
        prTextEditBufferSetStyle(b, 2, 4, 7);
        PrTextEditChange change{.start = c.start, .end = c.end, .text = c.text};
        prTextEditBufferApply(b, &change, 1, PrTextEditOrigin_Local);
        CHECK(runsOf(b) == c.expected);
        prTextEditBufferDestroy(b);
    }
}

TEST_CASE("a transaction's changes each move the runs") {
    PrTextEditBuffer* b = prTextEditBufferCreate("abcdef");
    prTextEditBufferSetStyle(b, 2, 4, 7);
    PrTextEditChange changes[] = {{.start = 0, .end = 0, .text = "xx"}, {.start = 5, .end = 6, .text = ""}};
    prTextEditBufferApply(b, changes, 2, PrTextEditOrigin_Local);
    CHECK(runsOf(b) == Runs{0, 0, 4, 7, 6, 0});
    prTextEditBufferDestroy(b);
}

TEST_CASE("undo restores the styles of deleted text, and redo removes them again") {
    PrTextEditBuffer* b = prTextEditBufferCreate("abcdef");
    PrTextEditHistory* history = prTextEditHistoryCreate(b);
    PrTextEditView* view = prTextEditViewCreate(b);
    prTextEditBufferSetStyle(b, 2, 4, 7);
    Runs start = runsOf(b);
    PrTextEditChange change{.start = 1, .end = 5};
    prTextEditViewApply(view, &change, 1, PrTextEditOrigin_Local, PrTextEditMerge_None);
    CHECK(runsOf(b) == Runs{});
    prTextEditHistoryUndo(history, view);
    CHECK(runsOf(b) == start);
    prTextEditHistoryRedo(history, view);
    CHECK(runsOf(b) == Runs{});
    prTextEditHistoryUndo(history, view);
    CHECK(runsOf(b) == start);
    prTextEditViewDestroy(view);
    prTextEditHistoryDestroy(history);
    prTextEditBufferDestroy(b);
}

TEST_CASE("undo of a run of deletions restores each deleted byte's style") {
    for (bool forward : {false, true}) {
        INFO(forward);
        PrTextEditBuffer* b = prTextEditBufferCreate("abcdef");
        PrTextEditHistory* history = prTextEditHistoryCreate(b);
        PrTextEditView* view = prTextEditViewCreate(b);
        prTextEditBufferSetStyle(b, 2, 3, 7);
        Runs start = runsOf(b);
        for (uint32_t i = 0; i < 4; ++i) {
            uint32_t at = forward ? 0 : 5 - i;
            PrTextEditSelection caret{.anchor = at + (forward ? 0 : 1), .caret = at + (forward ? 0 : 1)};
            prTextEditViewSetSelections(view, &caret, 1);
            PrTextEditChange change{.start = at, .end = at + 1};
            prTextEditViewApply(view, &change, 1, PrTextEditOrigin_Local, PrTextEditMerge_Deleting);
        }
        prTextEditHistoryUndo(history, view);
        CHECK(!prTextEditHistoryCanUndo(history));
        CHECK(runsOf(b) == start);
        prTextEditViewDestroy(view);
        prTextEditHistoryDestroy(history);
        prTextEditBufferDestroy(b);
    }
}

TEST_CASE("a view's style change is a step of the history of each selection") {
    PrTextEditBuffer* b = prTextEditBufferCreate("abcdef");
    PrTextEditHistory* history = prTextEditHistoryCreate(b);
    PrTextEditView* view = prTextEditViewCreate(b);
    PrTextEditSelection selections[] = {{.anchor = 0, .caret = 1}, {.anchor = 3, .caret = 4}};
    prTextEditViewSetSelections(view, selections, 2);
    uint64_t version = prTextEditBufferVersion(b);
    prTextEditViewSetStyle(view, 7);
    CHECK(runsOf(b) == Runs{0, 7, 1, 0, 3, 7, 4, 0});
    CHECK(prTextEditBufferVersion(b) == version);
    prTextEditHistoryUndo(history, view);
    CHECK(runsOf(b) == Runs{});
    prTextEditHistoryRedo(history, view);
    CHECK(runsOf(b) == Runs{0, 7, 1, 0, 3, 7, 4, 0});
    prTextEditViewDestroy(view);
    prTextEditHistoryDestroy(history);
    prTextEditBufferDestroy(b);
}

TEST_CASE("a style set at a caret waits for the next insertion, until the caret moves") {
    PrTextEditBuffer* b = prTextEditBufferCreate("abc");
    PrTextEditView* view = prTextEditViewCreate(b);
    PrTextEditSelection caret{.anchor = 1, .caret = 1};
    prTextEditViewSetSelections(view, &caret, 1);
    prTextEditViewSetStyle(view, 7);
    CHECK(prTextEditBufferStyleRunCount(b) == 0);
    prTextEditViewSetSelections(view, &caret, 1);
    prTextEditViewInsert(view, "x", PrTextEditOrigin_Local, PrTextEditMerge_Typing);
    CHECK(runsOf(b) == Runs{0, 0, 1, 7, 2, 0});
    // The next insertion follows the text that it joins.
    prTextEditViewInsert(view, "y", PrTextEditOrigin_Local, PrTextEditMerge_Typing);
    CHECK(runsOf(b) == Runs{0, 0, 1, 7, 3, 0});

    PrTextEditSelection end{.anchor = 4, .caret = 4};
    prTextEditViewSetSelections(view, &end, 1);
    prTextEditViewSetStyle(view, 9);
    PrTextEditSelection moved{.anchor = 0, .caret = 0};
    prTextEditViewSetSelections(view, &moved, 1);
    prTextEditViewInsert(view, "z", PrTextEditOrigin_Local, PrTextEditMerge_None);
    CHECK(prTextEditBufferStyleAt(b, 0) == 0);
    prTextEditViewDestroy(view);
    prTextEditBufferDestroy(b);
}
