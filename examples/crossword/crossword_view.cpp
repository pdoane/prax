// The crossword's view.

#include "examples/crossword/crossword_view.h"

#include "pr/app.h"
#include "pr/count_of.h"
#include "pr/format.h"
#include "pr/string.h"
#include "pr/studio_icon.h"

namespace crossword {

namespace {

// The view's colors: the page's night sky, its panels and their lines, the text, the accent and its glow, and the
// board's cells.
constexpr PrColor kSkyTop = prColorFromHex(0x2d2357);
constexpr PrColor kSkyBottom = prColorFromHex(0x151127);
constexpr PrColor kPanel = prColorFromHexAlpha(0xffffff0d);
constexpr PrColor kPanelRaised = prColorFromHexAlpha(0xffffff14);
constexpr PrColor kLine = prColorFromHexAlpha(0xffffff1a);
constexpr PrColor kText = prColorFromHex(0xf1edff);
constexpr PrColor kMuted = prColorFromHex(0xa79fc9);
constexpr PrColor kAccent = prColorFromHex(0x7c5cff);
constexpr PrColor kAccentLight = prColorFromHex(0x9b82ff);
constexpr PrColor kGold = prColorFromHex(0xf5c542);
constexpr PrColor kFlame = prColorFromHex(0xff8a3d);
constexpr PrColor kGood = prColorFromHex(0x5fd38a);
constexpr PrColor kCell = prColorFromHex(0xffffff);
constexpr PrColor kCellLine = prColorFromHex(0x8d82b8);
constexpr PrColor kBlockCell = prColorFromHex(0x1b1633);
constexpr PrColor kWord = prColorFromHex(0xe7e0ff);
constexpr PrColor kCursor = prColorFromHex(0xbcaaff);
constexpr PrColor kCursorRing = prColorFromHex(0x5534d9);
constexpr PrColor kInk = prColorFromHex(0x17122b);
constexpr PrColor kWrong = prColorFromHex(0xe0384f);

// The sections of the sidebar, each an icon.
constexpr PrStudioIconKind kSections[] = {
    PrStudioIconKind_Play, PrStudioIconKind_Calendar, PrStudioIconKind_Grid, PrStudioIconKind_Chart,
    PrStudioIconKind_Palette,
};

// The view's state for a round: the app, and the builder that draws it.
struct View {
    CrosswordApp& app;
    UiBuilder& ui;
};

// An icon of Studio's set in a color of the view's.
void icon(const View& view, PrLayoutNode parent, PrStudioIconKind which, float size, PrColor color) {
    UiPathPart part{
        .data = prStudioIconPathData(which),
        .strokeWidth = prStudioIconIsFilled(which) ? 0 : kPrStudioIconStrokeWidth,
    };
    UiPath path{.parts = Span(&part, 1), .viewBox = PrVec2{kPrStudioIconGrid, kPrStudioIconGrid}, .color = color};
    view.ui.path(parent, path).size(size).shrink0();
}

// Returns the clock's reading, as minutes and seconds.
String clockText(double seconds) {
    uint32_t whole = uint32_t(seconds);
    return format("{}:{:02}", whole / 60, whole % 60);
}

// A pill of the header: an icon or a mark, and a label.
PrLayoutNode pill(UiBuilder& ui, PrLayoutNode parent) {
    return ui.div(parent).flexRow().itemsCenter().gap(8).h(40).px(16).roundedFull().bg(kPanelRaised).border(1)
        .borderColor(kLine).textBase().semibold();
}

// A coin: a gold disc with a lighter rim.
void coin(UiBuilder& ui, PrLayoutNode parent, float size) {
    ui.div(parent).size(size).roundedFull().bgRadial(prColorFromHex(0xffe08a), kGold).border(1.5f)
        .borderColor(prColorFromHex(0xc9951c));
}

// A square button that shows an icon. Returns true if it was clicked.
bool iconButton(const View& view, UiBuilder& ui, PrLayoutNode parent, PrStudioIconKind which, float size, bool chosen) {
    UiElement b = ui.div(parent).size(size).rounded(12).flexRow().itemsCenter().justifyCenter().cursorPointer()
        .transition();
    if (chosen)
        b.bgLinear(135, kAccentLight, kAccent).shadow(0, 6, 18, -4, prColorWithAlpha(kAccent, 0.7f));
    else
        b.hoverBg(kPanelRaised);
    PrLayoutNode node = b;
    bool clicked = b.clicked();
    icon(view, node, which, 20, chosen ? kText : kMuted);
    return clicked;
}

void header(const View& view, UiBuilder& ui, PrLayoutNode page) {
    PrLayoutNode bar = ui.div(page).flexRow().itemsCenter().gap(12).px(28).py(18);
    // The logo: four tiles in two shades of the accent.
    PrLayoutNode logo = ui.div(bar).size(36).gridCols(2).gap(4);
    for (uint32_t i = 0; i < 4; ++i)
        ui.div(logo).rounded(4).bg(i == 1 || i == 2 ? kAccentLight : kAccent);
    PrLayoutNode title = ui.div(bar).flexRow().gap(8).text2xl().bold();
    ui.span(title).text("Crossword");
    ui.span(title).text("Night").textColor(kAccentLight);
    ui.div(bar).flex1();
    PrLayoutNode streak = pill(ui, bar);
    icon(view, streak, PrStudioIconKind_Flame, 18, kFlame);
    ui.span(streak).text(format("{} day streak", view.app.streak));
    PrLayoutNode coins = pill(ui, bar);
    coin(ui, coins, 18);
    ui.span(coins).text(format("{}", view.app.coins));
    UiElement settings = ui.div(bar).size(40).roundedFull().bg(kPanelRaised).border(1).borderColor(kLine).flexRow()
        .itemsCenter().justifyCenter().cursorPointer();
    PrLayoutNode settingsNode = settings;
    icon(view, settingsNode, PrStudioIconKind_Settings, 18, kMuted);
}

void sidebar(const View& view, UiBuilder& ui, PrLayoutNode body) {
    PrLayoutNode column = ui.div(body).flexCol().itemsCenter().gap(16).w(76);
    PrLayoutNode sections = ui.div(column).flexCol().itemsCenter().gap(10).p(10).rounded(18).bg(kPanel).border(1)
        .borderColor(kLine).backdropBlur(12);
    iconButton(view, ui, sections, PrStudioIconKind_ChevronRight, 44, false);
    for (uint32_t i = 0; i < countOf(kSections); ++i) {
        if (iconButton(view, ui, sections, kSections[i], 44, view.app.section == i))
            view.app.section = i;
    }
    ui.div(column).flex1();
    // The clock, which pauses and resumes the game.
    Game& game = view.app.game;
    PrLayoutNode clock = ui.div(column).flexCol().itemsCenter().gap(8).py(12).w(64).rounded(18).bg(kPanel).border(1)
        .borderColor(kLine).backdropBlur(12);
    ui.span(clock).text(clockText(game.elapsed)).textSm().semibold().textColor(game.paused ? kMuted : kText);
    UiElement pause = ui.div(clock).size(32).roundedFull().bg(kPanelRaised).flexRow().itemsCenter().justifyCenter()
        .cursorPointer().transition();
    pause.hoverBg(prColorWithAlpha(kAccent, 0.5f));
    PrLayoutNode pauseNode = pause;
    if (pause.clicked() && !game.solved)
        game.paused = !game.paused;
    icon(view, pauseNode, game.paused ? PrStudioIconKind_Play : PrStudioIconKind_Pause, 14, kText);
}

// The clue of the cursor's slot, over the board.
void clueBar(UiBuilder& ui, PrLayoutNode parent, const Game& game, float width) {
    PrLayoutNode bar = ui.div(parent).w(width).flexRow().itemsCenter().gap(16).px(16).py(12).rounded(16).bg(kPanel)
        .border(1).borderColor(kLine).minH(64);
    uint32_t at = currentSlot(game);
    if (at == kNoCell)
        return;
    const Slot& slot = game.slots[at];
    ui.div(bar).shrink0().px(12).py(6).rounded(8).bgLinear(135, kAccentLight, kAccent).textXs().bold().tracking(1)
        .text(format("{} {}", slot.number, slot.direction == Direction_Across ? "ACROSS" : "DOWN"));
    ui.div(bar).flex1().textLg().semibold().text(format("{} ({})", slot.clue, slot.length));
}

// The board: a grid of cells, with the cursor's slot and cell lit. Clicking a cell moves the cursor there.
void board(UiBuilder& ui, PrLayoutNode parent, Game& game, float cell) {
    uint32_t size = game.size;
    PrLayoutNode grid = ui.div(parent).gridCols(size).w(cell * float(size) + 4).p(2).rounded(10).bg(kBlockCell)
        .overflowHidden().shadow(0, 30, 60, -20, prColorFromHexAlpha(0x000000a0)).ring(1, kLine);
    uint32_t at = currentSlot(game);
    const Slot* slot = at != kNoCell ? &game.slots[at] : nullptr;
    for (uint32_t i = 0; i < size * size; ++i) {
        if (game.solution[i] == kBlock) {
            ui.div(grid).h(cell).bg(kBlockCell);
            continue;
        }
        bool inWord = slot != nullptr && slotAt(game, i, slot->direction) == at;
        bool cursor = i == game.cursor && !game.solved;
        PrColor fill = cursor ? kCursor : inWord && !game.solved ? kWord : kCell;
        UiElement c = ui.div(grid).relative().h(cell).bg(fill).borderR(1).borderB(1).borderColor(kCellLine)
            .flexRow().itemsCenter().justifyCenter().cursorPointer().transition(0.12f);
        if (cursor)
            c.shadow(0, 0, 0, 2, kCursorRing, true);
        else if (!game.solved)
            c.hoverBg(inWord ? kCursor : kWord);
        PrLayoutNode node = c;
        if (c.clicked())
            selectCell(game, i);
        if (game.numbers[i] != 0) {
            ui.span(node).text(format("{}", game.numbers[i])).absolute().top(1).left(3).textSize(cell *
                0.24f).leading(cell * 0.3f) .semibold().textColor(prColorFromHex(0x3b3260));
        }
        char letter = game.entries[i];
        if (letter != 0) {
            PrColor ink = kInk;
            if (game.flags[i] & CellFlag_Wrong)
                ink = kWrong;
            else if (game.flags[i] & CellFlag_Revealed)
                ink = kAccent;
            ui.span(node).text(PrStringView{&letter, 1}).textSize(cell * 0.56f).bold().textColor(ink).mt(cell * 0.08f);
        }
    }
}

// A clue of the list: its number and text, lit while it is the cursor's slot. Returns true if it was clicked.
bool clueRow(UiBuilder& ui, PrLayoutNode list, const Slot& slot, bool current, bool crossing) {
    UiElement row = ui.div(list).flexRow().gap(10).px(10).py(6).rounded(8).cursorPointer().transition(0.12f);
    if (current)
        row.bg(prColorWithAlpha(kAccent, 0.35f));
    else if (crossing)
        row.bg(prColorWithAlpha(kAccent, 0.12f));
    else
        row.hoverBg(kPanelRaised);
    PrLayoutNode node = row;
    bool clicked = row.clicked();
    ui.span(node).text(format("{}", slot.number)).w(22).shrink0().bold().textColor(current ? kText : kAccentLight);
    ui.span(node).text(slot.clue).textColor(current ? kText : kMuted);
    return clicked;
}

// The clues of one direction, under a heading.
void clueList(UiBuilder& ui, PrLayoutNode panel, Game& game, Direction direction) {
    PrLayoutNode section = ui.div(panel).flexCol().gap(4).flex1().minH(0);
    ui.div(section).textXs().bold().tracking(1.5f).textColor(kMuted).px(10).pb(4)
        .text(direction == Direction_Across ? "ACROSS" : "DOWN");
    PrLayoutNode list = ui.div(section).flexCol().gap(2).flex1().minH(0).overflowYScroll().textSm();
    uint32_t current = currentSlot(game);
    // The slot of this direction through the cursor, which crosses the cursor's slot.
    uint32_t crossing = slotAt(game, game.cursor, direction);
    for (uint32_t i = 0; i < game.slotCount; ++i) {
        const Slot& slot = game.slots[i];
        if (slot.direction != direction)
            continue;
        if (clueRow(ui, list, slot, i == current, i == crossing && i != current)) {
            game.direction = slot.direction;
            game.cursor = slot.start;
        }
    }
}

// A button of the panel: a label beside an icon, filled with the accent if it is the main action.
bool button(const View& view, UiBuilder& ui, PrLayoutNode parent, PrStringView label, PrStudioIconKind which, bool primary) {
    UiElement b = ui.div(parent).flex1().h(52).rounded(14).flexRow().itemsCenter().justifyCenter().gap(10)
        .textBase().semibold().cursorPointer().transition();
    if (primary) {
        b.bgLinear(135, kAccentLight, kAccent).shadow(0, 10, 30, -8, prColorWithAlpha(kAccent, 0.8f));
        b.hover([](UiElement& e) { e.translateY(-1).shadow(0, 14, 34, -8, prColorWithAlpha(kAccent, 0.9f)); });
    } else {
        b.bg(kPanelRaised).border(1).borderColor(kLine);
        b.hoverBg(prColorFromHexAlpha(0xffffff22));
    }
    b.activeScale(0.98f);
    PrLayoutNode node = b;
    bool clicked = b.clicked();
    if (!primary)
        icon(view, node, which, 18, kText);
    ui.span(node).text(label);
    if (primary)
        icon(view, node, which, 18, kText);
    return clicked;
}

// The coins that a solved game earns, line by line.
struct Reward {
    PrStringView label;
    uint32_t coins;
};

uint32_t rewards(const Game& game, Reward* out) {
    uint32_t count = 0;
    out[count++] = Reward{"Solve the puzzle", game.size * 5};
    if (game.hints == 0)
        out[count++] = Reward{"No hints", game.size + 4};
    if (game.checks == 0)
        out[count++] = Reward{"No checks", 10};
    if (game.elapsed < 600)
        out[count++] = Reward{"Under ten minutes", 20};
    return count;
}

void solvedPanel(const View& view, UiBuilder& ui, PrLayoutNode panel) {
    Game& game = view.app.game;
    PrLayoutNode title = ui.div(panel).flexRow().itemsCenter().gap(12);
    icon(view, title, PrStudioIconKind_Sparkles, 28, kGold);
    ui.span(title).text("Solved!").text3xl().bold();
    PrLayoutNode stats = ui.div(panel).flexRow().itemsCenter().gap(8).textColor(kMuted);
    icon(view, stats, PrStudioIconKind_Timer, 18, kMuted);
    ui.span(stats).text(clockText(game.elapsed)).textLg().bold().textColor(kText);
    ui.span(stats).text(format("· {}×{} · {} hint{}", game.size, game.size, game.hints, game.hints == 1 ? "" : "s"));

    Reward lines[4];
    uint32_t count = rewards(game, lines);
    uint32_t total = 0;
    PrLayoutNode card = ui.div(panel).flexCol().gap(12).p(20).rounded(16).bg(prColorFromHexAlpha(0x00000040));
    for (uint32_t i = 0; i < count; ++i) {
        PrLayoutNode row = ui.div(card).flexRow().justifyBetween().textColor(kMuted);
        ui.span(row).text(lines[i].label);
        ui.span(row).text(format("+{}", lines[i].coins)).semibold().textColor(kText);
        total += lines[i].coins;
    }
    PrLayoutNode sum = ui.div(card).flexRow().itemsCenter().borderT(1).borderColor(kLine).pt(12).semibold();
    ui.span(sum).text("Coins earned");
    ui.div(sum).flex1();
    coin(ui, sum, 16);
    ui.span(sum).text(format("+{}", total)).ml(6).textLg().bold().textColor(kGold);
    PrLayoutNode streak = ui.div(panel).flexRow().itemsCenter().gap(8).semibold();
    icon(view, streak, PrStudioIconKind_Flame, 18, kFlame);
    ui.span(streak).text(format("{} day streak", view.app.streak));

    PrLayoutNode share = ui.div(panel).flexRow().itemsCenter().gap(12).p(16).rounded(14).border(1).borderColor(kLine);
    PrLayoutNode text = ui.div(share).flex1().flexCol().gap(2).textSm().textColor(kMuted);
    ui.span(text).text(format("Crossword Night · {}", game.puzzle->name)).textColor(kText);
    ui.span(text).text(format("{}×{} · {} · {} hint{}", game.size, game.size, clockText(game.elapsed), game.hints,
        game.hints == 1 ? "" : "s"));
    UiElement copy = ui.div(share).flexRow().itemsCenter().gap(6).px(12).py(8).rounded(10).bg(kPanelRaised)
        .textSm().semibold().cursorPointer();
    copy.hoverBg(prColorFromHexAlpha(0xffffff22));
    PrLayoutNode copyNode = copy;
    if (copy.clicked()) {
        String summary = format("Crossword Night · {} · {}×{} · {} · {} hint{}", game.puzzle->name, game.size, game.size,
            clockText(game.elapsed), game.hints, game.hints == 1 ? "" : "s");
        PrAppWindow* window = prUiWindow(ui.ui());
        if (window != nullptr)
            prAppDataWriteText(prAppWindowApp(window), kPrAppDataSourceClipboard, summary);
        view.app.copied = true;
    }
    icon(view, copyNode, view.app.copied ? PrStudioIconKind_Check : PrStudioIconKind_Copy, 16, view.app.copied ? kGood : kText);
    ui.span(copyNode).text(view.app.copied ? "Copied" : "Copy");

    ui.div(panel).flex1();
    PrLayoutNode actions = ui.div(panel).flexRow().gap(12);
    if (button(view, ui, actions, "Next puzzle", PrStudioIconKind_ArrowRight, true)) {
        view.app.puzzle = (view.app.puzzle + 1) % puzzleCount();
        startGame(game, puzzleAt(view.app.puzzle));
        view.app.rewarded = false;
        view.app.copied = false;
    }
    if (button(view, ui, actions, "Change size", PrStudioIconKind_Grid, false)) {
        view.app.puzzle = (view.app.puzzle + 1) % puzzleCount();
        startGame(game, puzzleAt(view.app.puzzle));
        view.app.rewarded = false;
        view.app.copied = false;
    }
}

void playingPanel(const View& view, UiBuilder& ui, PrLayoutNode panel) {
    Game& game = view.app.game;
    PrLayoutNode title = ui.div(panel).flexRow().itemsBaseline().justifyBetween();
    ui.span(title).text(game.puzzle->name).text2xl().bold();
    ui.span(title).text(format("{}×{}", game.size, game.size)).textSm().textColor(kMuted);
    PrLayoutNode lists = ui.div(panel).flexCol().gap(16).flex1().minH(0);
    clueList(ui, lists, game, Direction_Across);
    clueList(ui, lists, game, Direction_Down);
    PrLayoutNode actions = ui.div(panel).flexRow().gap(12);
    if (button(view, ui, actions, "Check", PrStudioIconKind_Check, false))
        checkGrid(game);
    if (button(view, ui, actions, "Hint", PrStudioIconKind_Lightbulb, false))
        revealCell(game);
}

// Plays the keys that the board takes: letters, erasing, arrows, Tab between slots, and Space to turn.
void keys(PrUi* context, PrLayoutNode root, Game& game) {
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        if (prUiKeyPressed(context, root, PrAppKey(letter), 0))
            typeLetter(game, letter);
    }
    if (prUiKeyPressed(context, root, PrAppKey_Backspace, 0))
        eraseLetter(game);
    if (prUiKeyPressed(context, root, PrAppKey_Left, 0))
        moveCursor(game, 0, -1);
    if (prUiKeyPressed(context, root, PrAppKey_Right, 0))
        moveCursor(game, 0, 1);
    if (prUiKeyPressed(context, root, PrAppKey_Up, 0))
        moveCursor(game, -1, 0);
    if (prUiKeyPressed(context, root, PrAppKey_Down, 0))
        moveCursor(game, 1, 0);
    if (prUiKeyPressed(context, root, PrAppKey_Tab, 0) || prUiKeyPressed(context, root, PrAppKey_Enter, 0))
        nextSlot(game, false);
    if (prUiKeyPressed(context, root, PrAppKey_Tab, PrAppModifierFlag_Shift))
        nextSlot(game, true);
    if (prUiKeyPressed(context, root, PrAppKey(' '), 0))
        selectCell(game, game.cursor);
}

} // namespace

void buildCrossword(CrosswordApp& app, UiBuilder& ui, PrLayoutNode root, PrVec2 extent) {
    View view{.app = app, .ui = ui};
    if (app.game.puzzle == nullptr)
        startGame(app.game, puzzleAt(app.puzzle));
    PrUi* context = ui.ui();
    Game& game = app.game;
    const PrUiRound* round = prUiCurrentRound(context);
    tick(game, round->delta);
    keys(context, root, game);
    if (game.solved && !app.rewarded) {
        // A solved game pays its coins and extends the streak once, before anything shows the totals.
        Reward lines[4];
        uint32_t count = rewards(game, lines);
        for (uint32_t i = 0; i < count; ++i)
            app.coins += lines[i].coins;
        app.streak += 1;
        app.rewarded = true;
    }
    if (!game.solved && !game.paused)
        prUiRequestFrameAt(context, round->time + (1 - (game.elapsed - double(uint32_t(game.elapsed)))));

    PrLayoutNode page = ui.div(root).wFull().hFull().flexCol().bgLinear(180, kSkyTop, kSkyBottom).textColor(kText)
        .textBase();
    header(view, ui, page);
    PrLayoutNode body = ui.div(page).flex1().minH(0).flexRow().gap(24).px(28).pb(28);
    sidebar(view, ui, body);

    // The board takes the room that the clue bar and the panels leave it, a whole number of px for each cell.
    float side = extent.y - 250;
    float across = extent.x - 76 - 420 - 4 * 24 - 56;
    float room = side < across ? side : across;
    float cell = float(uint32_t(room / float(game.size)));
    cell = cell < 28 ? 28 : cell > 64 ? 64 : cell;
    PrLayoutNode center = ui.div(body).flex1().flexCol().itemsCenter().gap(20).minW(0);
    clueBar(ui, center, game, cell * float(game.size) + 4);
    board(ui, center, game, cell);

    PrLayoutNode panel = ui.div(body).w(420).shrink0().flexCol().gap(20).p(28).rounded(22).bg(kPanel).border(1)
        .borderColor(kLine).backdropBlur(12);
    if (game.solved)
        solvedPanel(view, ui, panel);
    else
        playingPanel(view, ui, panel);
}

} // namespace crossword
