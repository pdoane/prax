// The word guess game's view.

#include "examples/word_guess/word_guess_view.h"

#include "pr/float.h"

namespace word_guess {

namespace {

// The view's colors.
constexpr PrColor kPage = prColorFromHex(0x121213);
constexpr PrColor kText = prColorFromHex(0xffffff);
constexpr PrColor kEmptyBorder = prColorFromHex(0x3a3a3c);
constexpr PrColor kFilledBorder = prColorFromHex(0x565758);
constexpr PrColor kCorrect = prColorFromHex(0x538d4e);
constexpr PrColor kPresent = prColorFromHex(0xb59f3b);
constexpr PrColor kAbsent = prColorFromHex(0x3a3a3c);
constexpr PrColor kKey = prColorFromHex(0x818384);
constexpr PrColor kAccent = prColorFromHex(0x0a84ff);

// The timing of the animations, in seconds: a tile's flip as its row is revealed, and the delay between the tiles'
// flips; a letter's pop as it is typed; a rejected row's shake; and a won row's bounce, and the delay between tiles.
constexpr double kFlip = 0.5;
constexpr double kFlipStagger = 0.3;
constexpr double kPop = 0.12;
constexpr double kShake = 0.6;
constexpr double kBounce = 0.45;
constexpr double kBounceStagger = 0.1;

// How long a rejection's message shows, in seconds.
constexpr double kToast = 1.5;

PrColor markColor(Mark mark) {
    switch (mark) {
        case Mark_Correct: return kCorrect;
        case Mark_Present: return kPresent;
        case Mark_Absent: return kAbsent;
        default: return kKey;
    }
}

// The time at which the last accepted row has finished flipping.
double revealedAt(const Game& game) {
    return game.submittedAt + kFlipStagger * (kWordLength - 1) + kFlip;
}

// Returns true while the last accepted row is still flipping.
bool revealing(const Game& game, double time) {
    return game.guessCount > 0 && time < revealedAt(game);
}

// The best mark of a letter among the rows that have finished flipping, as the keyboard shows it.
Mark shownMark(const Game& game, char letter, double time) {
    uint32_t rows = game.guessCount - (revealing(game, time) ? 1 : 0);
    Mark best = Mark_Unknown;
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t i = 0; i < kWordLength; ++i) {
            if (game.guesses[row][i] == letter && game.marks[row][i] > best)
                best = game.marks[row][i];
        }
    }
    return best;
}

// The praise for a game won in `guesses` guesses.
PrStringView praise(uint32_t guesses) {
    constexpr PrStringView kPraise[kMaxGuesses] = {"Genius", "Magnificent", "Impressive", "Splendid", "Great", "Phew"};
    return kPraise[guesses - 1];
}

// The message under the title: the game's outcome once its last row has flipped, or why a guess was rejected while that
// shows.
void message(UiBuilder& ui, PrLayoutNode parent, const Game& game, double time) {
    UiElement line = ui.div(parent).h(24).textLg().medium();
    if (game.outcome != Outcome_Playing && !revealing(game, time)) {
        if (game.outcome == Outcome_Won) {
            line.textColor(kCorrect).text(praise(game.guessCount)).text("!");
        } else {
            char answer[kWordLength];
            for (uint32_t i = 0; i < kWordLength; ++i)
                answer[i] = game.answer[i];
            line.textColor(kPresent).text(PrStringView{answer, kWordLength});
        }
    } else if (game.rejectedAt >= 0 && time - game.rejectedAt < kToast) {
        // The message fades over the last part of the time that it shows.
        float fade = ui.animate({.start = game.rejectedAt, .delay = kToast - 0.3, .duration = 0.3}, 1, 0);
        line.textColor(kText).opacity(fade)
            .text(game.rejection == Submit_TooShort ? "Not enough letters" : "Not in word list");
    }
}

// A tile of the board: a letter, with the mark that its row's reveal has shown so far.
void tile(UiBuilder& ui, PrLayoutNode row, char letter, Mark mark, float flip, float scale, float lift) {
    UiElement t = ui.div(row).size(62).flexRow().itemsCenter().justifyCenter().text3xl().bold().textColor(kText)
        .uppercase();
    if (mark != Mark_Unknown)
        t.bg(markColor(mark));
    else
        t.border(2).borderColor(letter != 0 ? kFilledBorder : kEmptyBorder);
    if (flip != 0)
        t.rotateX(flip);
    if (scale != 1)
        t.scale(scale);
    if (lift != 0)
        t.translateY(lift);
    if (letter != 0)
        t.text(PrStringView{&letter, 1});
}

// The board: the guesses, the guess being typed, and the empty rows.
void board(UiBuilder& ui, PrLayoutNode parent, const Game& game) {
    PrLayoutNode rows = ui.div(parent).flexCol().gap(5).mt(8);
    for (uint32_t r = 0; r < kMaxGuesses; ++r) {
        UiElement row = ui.div(rows).flexRow().gap(5);
        bool typing = r == game.guessCount && game.outcome == Outcome_Playing;
        if (typing && game.rejectedAt >= 0) {
            // A rejected guess shakes its row, less as the shake ends.
            float p = ui.progress({.start = game.rejectedAt, .duration = kShake, .ease = UiEase_Linear});
            if (p < 1)
                row.translateX(prF32Sin(p * float(kShake) * 40) * 9 * (1 - p));
        }
        PrLayoutNode rowNode = row;
        for (uint32_t c = 0; c < kWordLength; ++c) {
            if (r < game.guessCount) {
                Mark mark = game.marks[r][c];
                float flip = 0;
                float lift = 0;
                if (r + 1 == game.guessCount) {
                    // The last row turns over each tile in turn, edge on at the halfway point, where its mark shows.
                    UiAnimation turn{
                        .start = game.submittedAt,
                        .delay = kFlipStagger * c,
                        .duration = kFlip,
                        .ease = UiEase_Linear,
                    };
                    float p = ui.progress(turn);
                    if (p < 0.5f)
                        mark = Mark_Unknown;
                    flip = 90 * (p < 0.5f ? p * 2 : (1 - p) * 2);
                    if (game.outcome == Outcome_Won) {
                        // A won row bounces once it has turned over, each tile after the one before it.
                        UiAnimation bounce{
                            .start = revealedAt(game),
                            .delay = kBounceStagger * c,
                            .duration = kBounce,
                            .ease = UiEase_Linear,
                        };
                        lift = -24 * prF32Sin(kPrF32Pi * ui.progress(bounce));
                    }
                }
                tile(ui, rowNode, game.guesses[r][c], mark, flip, 1, lift);
            } else if (typing && c < game.typed) {
                float scale = ui.animate({.start = game.typedAt[c], .duration = kPop, .ease = UiEase_Out}, 1.12f, 1);
                tile(ui, rowNode, game.typing[c], Mark_Unknown, 0, scale, 0);
            } else {
                tile(ui, rowNode, 0, Mark_Unknown, 0, 1, 0);
            }
        }
    }
}

// What a key of the keyboard does.
enum {
    KeyAction_Letter,
    KeyAction_Enter,
    KeyAction_Erase,
};
typedef uint8_t KeyAction;

void act(Game& game, KeyAction action, char letter, double time) {
    switch (action) {
        case KeyAction_Letter: typeLetter(game, letter, time); break;
        case KeyAction_Enter: submitGuess(game, time); break;
        case KeyAction_Erase: eraseLetter(game); break;
    }
}

// A key of the on-screen keyboard. Returns true if it was clicked.
bool key(UiBuilder& ui, PrLayoutNode row, PrStringView label, float width, PrColor color) {
    UiElement k = ui.div(row).w(width).h(58).rounded(4).bg(color).flexRow().itemsCenter().justifyCenter()
        .textSm().bold().textColor(kText).cursorPointer().transition(0.25f);
    return k.hoverOpacity(0.85f).activeScale(0.96f).text(label).clicked();
}

// The keyboard, whose letters take the color of the best mark that the revealed rows gave them.
void keyboard(UiBuilder& ui, PrLayoutNode parent, Game& game, double time) {
    constexpr PrStringView kRows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    PrLayoutNode keys = ui.div(parent).flexCol().itemsCenter().gap(8).mt(24);
    for (uint32_t r = 0; r < 3; ++r) {
        PrLayoutNode row = ui.div(keys).flexRow().gap(6);
        if (r == 2 && key(ui, row, "ENTER", 65, kKey))
            act(game, KeyAction_Enter, 0, time);
        for (uint32_t i = 0; i < kRows[r].size; ++i) {
            char letter = kRows[r].data[i];
            if (key(ui, row, PrStringView{&kRows[r].data[i], 1}, 43, markColor(shownMark(game, letter, time))))
                act(game, KeyAction_Letter, letter, time);
        }
        if (r == 2 && key(ui, row, "DEL", 65, kKey))
            act(game, KeyAction_Erase, 0, time);
    }
}

} // namespace

void buildWordGuess(WordGuessApp& app, UiBuilder& ui, PrLayoutNode root) {
    PrUi* context = ui.ui();
    double time = ui.time();
    if (!app.started) {
        // The first game's answer comes from the clock, and each later game takes the next word.
        app.next = uint32_t(time * 1000) % answerCount();
        startGame(app.game, app.next++);
        app.started = true;
    }
    Game& game = app.game;

    // The physical keyboard types into the game wherever focus is.
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        if (prUiKeyPressed(context, root, PrAppKey(letter), 0))
            act(game, KeyAction_Letter, letter, time);
    }
    if (prUiKeyPressed(context, root, PrAppKey_Enter, 0))
        act(game, KeyAction_Enter, 0, time);
    if (prUiKeyPressed(context, root, PrAppKey_Backspace, 0))
        act(game, KeyAction_Erase, 0, time);

    PrLayoutNode page = ui.div(root).wFull().hFull().bg(kPage).flexCol().itemsCenter().pt(36).textColor(kText);
    ui.span(page).text("WORD").text3xl().light().tracking(8);
    message(ui, page, game, time);
    board(ui, page, game);
    keyboard(ui, page, game, time);
    if (game.outcome != Outcome_Playing && !revealing(game, time)) {
        UiElement again = ui.div(page).mt(24).px(16).py(8).rounded(6).bg(kAccent).hoverBg(prColorFromHex(0x3d9bff))
            .textBase().semibold().cursorPointer().transition().text("Play again");
        if (again.clicked())
            startGame(game, app.next++);
    }
}

} // namespace word_guess
