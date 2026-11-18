// The crossword's rules.

#include "examples/crossword/crossword_game.h"

#include "pr/assert.h"
#include "pr/mem.h"

namespace crossword {

namespace {

bool isOpen(const Game& game, int32_t row, int32_t column) {
    int32_t size = int32_t(game.size);
    return row >= 0 && column >= 0 && row < size && column < size &&
        game.solution[uint32_t(row) * game.size + uint32_t(column)] != kBlock;
}

// The clue of `answer` in the puzzle, or an empty view if it has none.
PrStringView clueFor(const PuzzleData& puzzle, const char* answer, uint32_t length) {
    for (uint32_t i = 0; i < puzzle.clueCount; ++i) {
        const Clue& clue = puzzle.clues[i];
        if (clue.answer.size == length && prMemEquals(clue.answer.data, answer, length))
            return clue.text;
    }
    return PrStringView{};
}

// The cells of a slot, read from the solution.
void answerOf(const Game& game, const Slot& slot, char* out) {
    for (uint32_t i = 0; i < slot.length; ++i)
        out[i] = game.solution[slotCell(game, slot, i)];
}

// Numbers the grid's slots as crosswords do: each cell that starts a slot, across or down, takes the next number in
// reading order.
void numberSlots(Game& game) {
    uint32_t number = 0;
    Slot down[kMaxSlots];
    uint32_t downCount = 0;
    game.slotCount = 0;
    for (uint32_t r = 0; r < game.size; ++r) {
        for (uint32_t c = 0; c < game.size; ++c) {
            int32_t row = int32_t(r);
            int32_t column = int32_t(c);
            if (!isOpen(game, row, column))
                continue;
            bool across = !isOpen(game, row, column - 1) && isOpen(game, row, column + 1);
            bool downward = !isOpen(game, row - 1, column) && isOpen(game, row + 1, column);
            if (!across && !downward)
                continue;
            game.numbers[r * game.size + c] = ++number;
            if (across) {
                uint32_t length = 0;
                while (isOpen(game, row, column + int32_t(length)))
                    ++length;
                game.slots[game.slotCount++] = Slot{
                    .start = r * game.size + c,
                    .length = length,
                    .number = number,
                    .direction = Direction_Across,
                };
            }
            if (downward) {
                uint32_t length = 0;
                while (isOpen(game, row + int32_t(length), column))
                    ++length;
                down[downCount++] = Slot{
                    .start = r * game.size + c,
                    .length = length,
                    .number = number,
                    .direction = Direction_Down,
                };
            }
        }
    }
    for (uint32_t i = 0; i < downCount; ++i)
        game.slots[game.slotCount++] = down[i];
    for (uint32_t i = 0; i < game.slotCount; ++i) {
        Slot& slot = game.slots[i];
        char answer[kMaxSize];
        answerOf(game, slot, answer);
        slot.clue = clueFor(*game.puzzle, answer, slot.length);
    }
}

// The index of `cell` in `slot`.
uint32_t indexIn(const Game& game, const Slot& slot, uint32_t cell) {
    uint32_t step = slot.direction == Direction_Across ? 1 : game.size;
    return (cell - slot.start) / step;
}

// Updates whether the game is solved, which stops its clock.
void settle(Game& game) {
    game.solved = isSolved(game);
}

} // namespace

void startGame(Game& game, const PuzzleData* puzzle) {
    PR_ASSERT(puzzle->size <= kMaxSize);
    game = Game{.puzzle = puzzle, .size = puzzle->size};
    uint32_t cells = 0;
    for (uint32_t i = 0; i < puzzle->grid.size; ++i) {
        char ch = puzzle->grid.data[i];
        if (ch != '\n' && cells < kMaxCells)
            game.solution[cells++] = ch;
    }
    PR_ASSERT(cells == puzzle->size * puzzle->size);
    numberSlots(game);
    game.cursor = game.slotCount != 0 ? game.slots[0].start : 0;
}

void tick(Game& game, double seconds) {
    if (!game.paused && !game.solved)
        game.elapsed += seconds;
}

uint32_t slotAt(const Game& game, uint32_t cell, Direction direction) {
    for (uint32_t i = 0; i < game.slotCount; ++i) {
        const Slot& slot = game.slots[i];
        if (slot.direction != direction)
            continue;
        uint32_t step = direction == Direction_Across ? 1 : game.size;
        if (cell < slot.start || (cell - slot.start) % step != 0)
            continue;
        if ((cell - slot.start) / step < slot.length &&
                (direction == Direction_Down || cell / game.size == slot.start / game.size))
            return i;
    }
    return kNoCell;
}

uint32_t currentSlot(const Game& game) {
    return slotAt(game, game.cursor, game.direction);
}

uint32_t slotCell(const Game& game, const Slot& slot, uint32_t index) {
    return slot.start + index * (slot.direction == Direction_Across ? 1 : game.size);
}

void selectCell(Game& game, uint32_t cell) {
    if (game.solution[cell] == kBlock)
        return;
    Direction other = game.direction == Direction_Across ? Direction_Down : Direction_Across;
    if (cell == game.cursor || slotAt(game, cell, game.direction) == kNoCell) {
        if (slotAt(game, cell, other) != kNoCell)
            game.direction = other;
    }
    game.cursor = cell;
}

void moveCursor(Game& game, int32_t rows, int32_t columns) {
    Direction direction = rows != 0 ? Direction_Down : Direction_Across;
    if (direction != game.direction && slotAt(game, game.cursor, direction) != kNoCell) {
        // The first press of an arrow across the cursor's direction turns it, as crosswords do.
        game.direction = direction;
        return;
    }
    int32_t row = int32_t(game.cursor / game.size) + rows;
    int32_t column = int32_t(game.cursor % game.size) + columns;
    int32_t size = int32_t(game.size);
    while (row >= 0 && column >= 0 && row < size && column < size) {
        if (isOpen(game, row, column)) {
            game.cursor = uint32_t(row) * game.size + uint32_t(column);
            return;
        }
        row += rows;
        column += columns;
    }
}

void nextSlot(Game& game, bool backward) {
    uint32_t at = currentSlot(game);
    if (at == kNoCell || game.slotCount == 0)
        return;
    uint32_t next = backward ? (at + game.slotCount - 1) % game.slotCount : (at + 1) % game.slotCount;
    const Slot& slot = game.slots[next];
    game.direction = slot.direction;
    game.cursor = slot.start;
    for (uint32_t i = 0; i < slot.length; ++i) {
        uint32_t cell = slotCell(game, slot, i);
        if (game.entries[cell] == 0) {
            game.cursor = cell;
            break;
        }
    }
}

void typeLetter(Game& game, char letter) {
    if (game.solved || game.paused)
        return;
    uint32_t cell = game.cursor;
    if ((game.flags[cell] & CellFlag_Revealed) == 0) {
        game.entries[cell] = letter;
        game.flags[cell] &= CellFlags(~CellFlag_Wrong);
    }
    uint32_t at = currentSlot(game);
    if (at != kNoCell) {
        // The cursor moves to the next empty cell of its slot, or the next cell if the rest are full.
        const Slot& slot = game.slots[at];
        uint32_t index = indexIn(game, slot, cell);
        uint32_t next = index + 1 < slot.length ? slotCell(game, slot, index + 1) : cell;
        for (uint32_t i = index + 1; i < slot.length; ++i) {
            if (game.entries[slotCell(game, slot, i)] == 0) {
                next = slotCell(game, slot, i);
                break;
            }
        }
        game.cursor = next;
    }
    settle(game);
}

void eraseLetter(Game& game) {
    if (game.solved || game.paused)
        return;
    uint32_t cell = game.cursor;
    uint32_t at = currentSlot(game);
    if (game.entries[cell] == 0 && at != kNoCell) {
        const Slot& slot = game.slots[at];
        uint32_t index = indexIn(game, slot, cell);
        if (index > 0)
            cell = slotCell(game, slot, index - 1);
    }
    if ((game.flags[cell] & CellFlag_Revealed) == 0) {
        game.entries[cell] = 0;
        game.flags[cell] &= CellFlags(~CellFlag_Wrong);
    }
    game.cursor = cell;
}

void checkGrid(Game& game) {
    ++game.checks;
    for (uint32_t cell = 0; cell < game.size * game.size; ++cell) {
        if (game.entries[cell] != 0 && game.entries[cell] != game.solution[cell])
            game.flags[cell] |= CellFlag_Wrong;
    }
}

void revealCell(Game& game) {
    if (game.solved || game.paused)
        return;
    uint32_t cell = game.cursor;
    if (game.entries[cell] == game.solution[cell])
        return;
    game.entries[cell] = game.solution[cell];
    game.flags[cell] = CellFlag_Revealed;
    ++game.hints;
    settle(game);
}

bool isSolved(const Game& game) {
    for (uint32_t cell = 0; cell < game.size * game.size; ++cell) {
        if (game.solution[cell] != kBlock && game.entries[cell] != game.solution[cell])
            return false;
    }
    return true;
}

} // namespace crossword
