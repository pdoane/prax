// The crossword's puzzles.

#pragma once

#include "examples/crossword/crossword_game.h"

PR_ASSUME_NONNULL_BEGIN

namespace crossword {

// The puzzles that the game offers, and the puzzle at `index`, wrapping.
uint32_t puzzleCount();
const PuzzleData* puzzleAt(uint32_t index);

} // namespace crossword

PR_ASSUME_NONNULL_END
