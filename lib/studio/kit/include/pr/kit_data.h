// Studio Kit's readouts: stat tiles, and the lines of a console's log.

#pragma once

#include "pr/kit.h"
#include "pr/kit_text.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Stat tiles

// What a stat tile shows: a caption, a reading in large numerals, and a note under it in `noteTone`, such as the change
// since the last reading or the budget that the reading is against. An empty note shows nothing.
typedef struct PrKitStatTile {
    uint64_t key;
    PrStringView label;
    PrStringView value;
    PrStringView note;
    PrKitTone noteTone;             // Auto is muted
} PrKitStatTile;

// Creates a stat tile: a card of the caption, the reading and the note. In a row, the tile is as wide as its content.
PrLayoutNode prKitStatTile(PrKit* kit, PrLayoutNode parent, PrKitStatTile tile);

// ---------------------------------------------------------------------------------------------------------------------
// Log lines

// The severity of a log line.
enum {
    PrKitLogLevel_Info,
    PrKitLogLevel_Warning,
    PrKitLogLevel_Error,
};
typedef uint8_t PrKitLogLevel;

// What a log line shows: when it was logged, its severity, its message, and what logged it. An empty time or source
// shows nothing in its place.
typedef struct PrKitLogLine {
    uint64_t key;
    PrStringView time;
    PrStringView message;
    PrStringView source;
    PrKitLogLevel level;
} PrKitLogLine;

// Creates a log line: a row of its time, its severity's tag, its message, which takes the room that the row leaves and
// clips, and its source, ruled below. A warning and an error are washed in their tones. The columns line up across the
// lines of a log.
PrLayoutNode prKitLogLine(PrKit* kit, PrLayoutNode parent, PrKitLogLine line);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
