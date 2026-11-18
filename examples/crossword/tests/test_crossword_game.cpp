// The crossword's rules: how a grid's slots are numbered, and how the cursor, typing, checks and hints play.

#include "examples/crossword/crossword_game.h"
#include "examples/crossword/puzzles.h"

#include "tools/harness/test.h"

using namespace crossword;

namespace {

// A puzzle of three answers across and three down around a block in the middle:
//
//     C A T
//     A # O
//     B E D
constexpr Clue kClues[] = {
    {"CAT", "Pet that purrs"},
    {"BED", "Place to sleep"},
    {"CAB", "Taxi"},
    {"TOD", "Fox, in Scotland"},
};
constexpr PuzzleData kRing = {
    .name = "Ring",
    .size = 3,
    .grid = R"(
CAT
A#O
BED
)",
    .clues = kClues,
    .clueCount = 4,
};

} // namespace

TEST_CASE("a grid's slots are numbered in reading order, across before down, with their clues") {
    Game game;
    startGame(game, &kRing);
    REQUIRE(game.slotCount == 4);
    CHECK(game.slots[0].number == 1);
    CHECK(game.slots[0].direction == Direction_Across);
    CHECK(game.slots[0].clue == "Pet that purrs");
    CHECK(game.slots[1].number == 3);
    CHECK(game.slots[1].clue == "Place to sleep");
    CHECK(game.slots[2].number == 1);
    CHECK(game.slots[2].direction == Direction_Down);
    CHECK(game.slots[2].clue == "Taxi");
    CHECK(game.slots[3].number == 2);
    CHECK(game.numbers[0] == 1);
    CHECK(game.numbers[2] == 2);
    CHECK(game.numbers[6] == 3);
}

TEST_CASE("typing fills the cursor's slot and advances, and erasing an empty cell erases the one before") {
    Game game;
    startGame(game, &kRing);
    typeLetter(game, 'C');
    CHECK(game.cursor == 1);
    typeLetter(game, 'X');
    CHECK(game.cursor == 2);
    // The cursor's cell is empty, so erasing takes the letter before it.
    eraseLetter(game);
    CHECK(game.entries[1] == 0);
    CHECK(game.cursor == 1);
    eraseLetter(game);
    CHECK(game.entries[0] == 0);
    CHECK(game.cursor == 0);
}

TEST_CASE("selecting the cursor's cell turns it, and an arrow across its direction turns it first") {
    Game game;
    startGame(game, &kRing);
    selectCell(game, 0);
    CHECK(game.direction == Direction_Down);
    moveCursor(game, 0, 1);
    CHECK(game.direction == Direction_Across);
    CHECK(game.cursor == 0);
    moveCursor(game, 0, 1);
    CHECK(game.cursor == 1);
    // Down from the top's middle is the block, and then the bottom row.
    moveCursor(game, 1, 0);
    moveCursor(game, 1, 0);
    CHECK(game.cursor == 7);
}

TEST_CASE("checking marks wrong letters, a hint gives a letter, and the last right letter solves the grid") {
    Game game;
    startGame(game, &kRing);
    typeLetter(game, 'C');
    typeLetter(game, 'A');
    typeLetter(game, 'T');
    game.entries[3] = 'Z';
    checkGrid(game);
    CHECK((game.flags[3] & CellFlag_Wrong) != 0);
    CHECK((game.flags[0] & CellFlag_Wrong) == 0);
    const char rest[] = "ABEDO";
    const uint32_t cells[] = {3, 6, 7, 8, 5};
    for (uint32_t i = 0; i < 4; ++i)
        game.entries[cells[i]] = rest[i];
    CHECK(!isSolved(game));
    game.cursor = 5;
    revealCell(game);
    CHECK(game.hints == 1);
    CHECK(game.solved);
}

TEST_CASE("every answer of every puzzle has a clue") {
    for (uint32_t i = 0; i < puzzleCount(); ++i) {
        const PuzzleData* puzzle = puzzleAt(i);
        Game game;
        startGame(game, puzzle);
        for (uint32_t s = 0; s < game.slotCount; ++s)
            CHECK(game.slots[s].clue.size > 0);
    }
}
