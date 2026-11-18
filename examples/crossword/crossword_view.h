// The crossword's view: the board, its clues, the clock and the solved summary, built with lib/ui's elements
// (pr/ui_element.h), with Studio's icons (pr/studio_icon.h).

#pragma once

#include "pr/ui_element.h"

#include "examples/crossword/crossword_game.h"
#include "examples/crossword/puzzles.h"

PR_ASSUME_NONNULL_BEGIN

using namespace pr;

namespace crossword {

// The game, what the player has earned, and what the view keeps between rounds.
struct CrosswordApp {
    Game game;
    uint32_t puzzle;                // the index of the puzzle being played
    uint32_t coins;
    uint32_t streak;                // the puzzles solved in a row
    bool rewarded;                  // the solved game's coins are counted
    uint32_t section;               // the sidebar's chosen section
    bool copied;                    // the share card's text was copied
};

// Declares a round of the view under `root`, which fills a viewport of `extent` px, and plays the round's input. The
// builder's resources draw the view's icons.
void buildCrossword(CrosswordApp& app, UiBuilder& ui, PrLayoutNode root, PrVec2 extent);

} // namespace crossword

PR_ASSUME_NONNULL_END
