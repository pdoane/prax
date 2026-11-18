// The crossword's rules: a square grid of letters and blocks, its numbered slots with their clues, and a solver's
// entries, cursor and clock.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace crossword {

// The largest grid's side, and its cells.
constexpr uint32_t kMaxSize = 15;
constexpr uint32_t kMaxCells = kMaxSize * kMaxSize;

// The most slots that a grid of kMaxSize holds: a slot is at least 3 cells long, in each direction.
constexpr uint32_t kMaxSlots = 2 * kMaxSize * ((kMaxSize + 1) / 4);

// The block of a grid's text, and the cell of a cell index with none.
constexpr char kBlock = '#';
constexpr uint32_t kNoCell = 0xffffffffu;

// Which way a slot runs.
enum {
    Direction_Across,
    Direction_Down,
};
typedef uint8_t Direction;

// A clue of a puzzle: the answer that it is for, in uppercase, and its text.
struct Clue {
    PrStringView answer;
    PrStringView text;
};

// A puzzle as it is written: its name, its side, its rows of letters and blocks in uppercase, each row on a line of its
// own, and a clue for each of its answers, in any order. Line breaks in the grid are not cells.
struct PuzzleData {
    PrStringView name;
    uint32_t size;
    PrStringView grid;
    const Clue* clues;
    uint32_t clueCount;
};

// A slot: the cells of one answer, from its first cell on, and its number and clue.
struct Slot {
    uint32_t start;
    uint32_t length;
    uint32_t number;
    Direction direction;
    PrStringView clue;
};

// What the solver has done to a cell besides typing in it.
enum {
    CellFlag_Wrong = 1u << 0,       // a check found its letter wrong
    CellFlag_Revealed = 1u << 1,    // a hint gave its letter
};
typedef uint8_t CellFlags;

// A game of a puzzle: the puzzle's cells and slots, the solver's entries, the cursor and its direction, and the clock.
struct Game {
    const PuzzleData* puzzle;
    uint32_t size;
    char solution[kMaxCells];       // the letter of each cell, or kBlock
    uint32_t numbers[kMaxCells];    // the number of each cell that starts a slot, or 0
    Slot slots[kMaxSlots];          // the across slots by number, then the down slots by number
    uint32_t slotCount;
    char entries[kMaxCells];        // the solver's letter of each cell, or 0
    CellFlags flags[kMaxCells];
    uint32_t cursor;
    Direction direction;
    uint32_t hints;                 // the letters that hints gave
    uint32_t checks;                // the checks that the solver asked for
    double elapsed;                 // seconds of play, which pauses stop
    bool paused;
    bool solved;
};

// Starts a game of `puzzle`, with the cursor on its first across slot.
void startGame(Game& game, const PuzzleData* puzzle);

// Advances the clock by `seconds` while the game is played.
void tick(Game& game, double seconds);

// The slot that holds `cell` in `direction`, or kNoCell if none does.
uint32_t slotAt(const Game& game, uint32_t cell, Direction direction);

// The slot of the cursor in its direction.
uint32_t currentSlot(const Game& game);

// The cell of `slot` at `index`.
uint32_t slotCell(const Game& game, const Slot& slot, uint32_t index);

// Puts the cursor on `cell`, or turns its direction if it is there already. The direction turns too if the cell has no
// slot in the cursor's direction.
void selectCell(Game& game, uint32_t cell);

// Moves the cursor a cell in a direction, past blocks, and turns the cursor's direction to match.
void moveCursor(Game& game, int32_t rows, int32_t columns);

// Moves the cursor to the first empty cell of the next slot, or the previous, wrapping.
void nextSlot(Game& game, bool backward);

// Types a letter, A to Z, at the cursor and advances it through its slot, or erases the letter at the cursor, or the
// one before it if the cursor's cell is empty.
void typeLetter(Game& game, char letter);
void eraseLetter(Game& game);

// Marks the wrong letters of the grid, which stay marked until they change.
void checkGrid(Game& game);

// Gives the letter at the cursor.
void revealCell(Game& game);

// Returns true if every cell holds its letter.
bool isSolved(const Game& game);

} // namespace crossword

PR_ASSUME_NONNULL_END
