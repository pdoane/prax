// The crossword's puzzles.

#include "examples/crossword/puzzles.h"

#include "pr/count_of.h"

using namespace pr;

namespace crossword {

namespace {

// An 11 by 11 grid of everyday words.
constexpr Clue kNightClues[] = {
    {"ANTI", "Opposed to"},
    {"TOSS", "Flip, as a coin"},
    {"REALM", "Kingdom"},
    {"FIFTH", "Next after fourth"},
    {"EARLY", "Before the appointed time"},
    {"ACTOR", "Stage performer"},
    {"ART", "What a gallery shows"},
    {"ROT", "Decay"},
    {"EVE", "Night before a holiday"},
    {"ARC", "A rainbow's shape"},
    {"ANEW", "Once more, from the start"},
    {"SHELL", "A turtle's home"},
    {"PECK", "Quick kiss on the cheek"},
    {"AIL", "Feel unwell"},
    {"OVA", "Egg cells"},
    {"INN", "Roadside lodging"},
    {"CAB", "Taxi"},
    {"SONIC", "Of sound"},
    {"ELOPE", "Run off to marry"},
    {"SKATE", "Glide on ice"},
    {"REPEL", "Drive back"},
    {"EELS", "Slippery swimmers"},
    {"TEXT", "Message on a phone"},
    {"AREA", "Length times width"},
    {"NEAR", "Close by"},
    {"TART", "Small fruit pie"},
    {"ILL", "Under the weather"},
    {"TIC", "Involuntary twitch"},
    {"OFTEN", "Many times"},
    {"STOVE", "Kitchen range"},
    {"SHREW", "Small mouselike mammal"},
    {"MYRRH", "Fragrant resin among the Magi's gifts"},
    {"FAT", "Butter or lard, for example"},
    {"OCEAN", "The Pacific, for one"},
    {"ASK", "Pose a question"},
    {"ALL", "Every bit"},
    {"LINER", "Large passenger ship"},
    {"POSSE", "A sheriff's band"},
    {"EVOKE", "Call to mind"},
    {"CANAL", "Panama's waterway"},
    {"ICE", "Frozen water"},
    {"COPE", "Manage under pressure"},
    {"APEX", "Highest point"},
    {"BELT", "It holds up trousers"},
    {"ITS", "Belonging to it"},
    {"LET", "Allow"},
};

// A word square, whose rows and columns read the same.
constexpr Clue kMiniClues[] = {
    {"HEART", "Organ that keeps the beat"},
    {"EMBER", "Glowing piece of a dying fire"},
    {"ABUSE", "Treat badly"},
    {"RESIN", "Sticky stuff that pines ooze"},
    {"TREND", "What's in fashion right now"},
};

constexpr PuzzleData kPuzzles[] = {
    {
        .name = "Night Owl",
        .size = 11,
        .grid = R"(
ANTI###TOSS
REALM#FIFTH
EARLY#ACTOR
ART#ROT#EVE
###ARC#ANEW
###SHELL###
PECK#AIL###
OVA#INN#CAB
SONIC#ELOPE
SKATE#REPEL
EELS###TEXT
)",
        .clues = kNightClues,
        .clueCount = uint32_t(countOf(kNightClues)),
    },
    {
        .name = "Mini",
        .size = 5,
        .grid = R"(
HEART
EMBER
ABUSE
RESIN
TREND
)",
        .clues = kMiniClues,
        .clueCount = uint32_t(countOf(kMiniClues)),
    },
};

} // namespace

uint32_t puzzleCount() {
    return uint32_t(countOf(kPuzzles));
}

const PuzzleData* puzzleAt(uint32_t index) {
    return &kPuzzles[index % puzzleCount()];
}

} // namespace crossword
