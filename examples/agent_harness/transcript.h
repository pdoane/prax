// Claude Code's session transcripts: the sessions that the store keeps under its projects, and a session's
// conversation, read from a file that grows a line at a time while the session runs.
//
// A transcript file is JSON Lines, one record to a line. A record that takes part in the conversation has a uuid and
// names its parent's, so those records form a tree whose branches are the conversation's retries and rewinds. They are
// the messages, the attachments that the harness gives the model, and the harness's own events. An assistant's reply
// arrives as one record for each content block, each naming the reply's message id, and a tool's result arrives in a
// user record that names the call that it answers. The records without a uuid carry the session's state, such as its
// title and the newest message of the conversation that the user sees.

#pragma once

#include "pr/array.h"
#include "pr/map.h"
#include "pr/string.h"
#include "pr/string_view.h"

#include <stddef.h>
#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

// The JSON document that a transcript reads each record into (pr/json.h).
struct PrJson;

namespace harness {

using namespace pr;

// The index of no entry or block.
constexpr uint32_t kNone = 0xffffffffu;

// ---------------------------------------------------------------------------------------------------------------------
// The store

// A store of sessions: the projects directory of one of Claude Code's configuration directories.
struct Store {
    String name;                    // the configuration directory's name, such as ".claude-personal"
    String projects;
};

// Returns the stores of the configuration directories in the user's home, ~/.claude and each ~/.claude-<name>, and of
// $CLAUDE_CONFIG_DIR if it names another, each one that has a projects directory.
Array<Store> findStores();

// A session in a store, as a list of sessions shows it.
struct SessionInfo {
    String path;
    uint32_t store = 0;             // the index of its store
    String title;                   // the session's title, or its first prompt if it has none
    String directory;               // the directory that the session worked in
    uint64_t size = 0;              // the size of its file in bytes
    double modified = 0;            // when the file last changed, in milliseconds since the Unix epoch
};

// Returns the sessions of every project of `stores`, the most recently modified first. A session's title and directory
// come from the head and the tail of its file, which is not read whole.
Array<SessionInfo> listSessions(const Array<Store>& stores);

// A subagent of a session: its transcript, what it is, and the agent whose call spawned it.
struct AgentInfo {
    String path;
    String agentType;
    String description;
    String toolUseId;               // the id of the call that spawned it
    uint32_t parent = kNone;        // the index of the agent whose call spawned it, or kNone for the session
    uint32_t depth = 1;             // 1 for an agent that the session spawned, and one more for each agent between
};

// Returns the subagents of the session at `path`: the transcripts under its directory of subagents, and of the
// workflows there. An agent hangs under the agent whose transcript holds the call that spawned it, or under the session
// if the session's does or none does. Each agent comes after its parent, and the agents of one parent in the order of
// their calls.
Array<AgentInfo> listAgents(PrStringView path);

// ---------------------------------------------------------------------------------------------------------------------
// Transcripts

// What kind of record an entry is.
enum {
    EntryKind_User,
    EntryKind_Assistant,
    EntryKind_System,               // an event of the harness, such as a compaction or a hook's summary
    EntryKind_Attachment,           // context that the harness attaches for the model, such as the date
    EntryKind_Other,                // a record of a type that the reader does not know
};
typedef uint8_t EntryKind;

// What a block of a message holds.
enum {
    BlockKind_Text,
    BlockKind_Thinking,
    BlockKind_ToolUse,
    BlockKind_ToolResult,
    BlockKind_Image,
    BlockKind_Other,                // a block of a type that the reader does not know
};
typedef uint8_t BlockKind;

// Where a block is: its entry, and its index among the entry's blocks.
struct BlockRef {
    uint32_t entry = kNone;
    uint32_t block = kNone;
};

// A content block of a message.
struct Block {
    BlockKind kind = BlockKind_Other;
    bool error = false;             // ToolResult: the tool failed

    // Text: the block is wholly one element of the harness's markup, such as <system-reminder>, which the harness adds
    // to a user's message as context for the model, and which no one typed.
    bool context = false;

    String text;                    // Text and Thinking: the text. ToolResult: the result's text. Image: its type
    String name;                    // ToolUse: the tool. Other: the block's type
    String id;                      // ToolUse: the call's id. ToolResult: the id of the call that it answers
    String input;                   // ToolUse: the call's input, as JSON
    String summary;                 // ToolUse: the first string of the input, which says what the call is about
    BlockRef result;                // ToolUse: the result that answers the call, once it arrives
};

// Where a record's line is in the file, without its line feed.
struct RecordRange {
    size_t offset = 0;
    size_t size = 0;
};

// A node of the conversation: a record, or the records of one assistant reply joined in the order that they arrived.
struct Entry {
    EntryKind kind = EntryKind_Other;
    bool meta = false;              // User: context that the harness gave the model, which no one typed
    bool sidechain = false;         // a record of a subagent's conversation
    bool answers = false;           // User: a message that only answers tool calls
    String type;                    // the record's type
    String subtype;                 // System: the event. Attachment: what it attaches
    String summary;                 // System, Attachment and Other: a line that says what the record holds
    String uuid;                    // the uuid of the first record
    String parentUuid;
    String messageId;               // Assistant: the id that the records of one reply share
    String model;
    String timestamp;               // ISO 8601
    Array<Block> blocks;            // User and Assistant: the message's content
    Array<RecordRange> records;
    uint32_t parent = kNone;
    uint32_t newestChild = kNone;   // the child that arrived last
};

// A session's transcript, which follows its file as it grows.
struct Transcript {
    Transcript();
    ~Transcript();
    Transcript(const Transcript&) = delete;
    Transcript& operator=(const Transcript&) = delete;

    // Reads the transcript at `path`, in place of what the transcript held. Returns false if the file cannot be read.
    bool load(PrStringView path);

    // Reads the lines that were added to the file since the last read. A line that is still being written waits for the
    // next refresh. A file that shrank is read again from its start. Returns true if the transcript changed.
    bool refresh();

    // Empties the transcript.
    void clear();

    // Returns the line of a record, read from the file again, or the null string if the file no longer holds it.
    String record(RecordRange range) const;

    String path;
    String title;                   // the title that the user gave the session, or else the one that was made for it
    Array<Entry> entries;           // in the order that their first records arrived
    Array<uint32_t> conversation;   // the entries from the root to the newest message of the conversation shown
    uint64_t revision = 0;          // changes each time that the transcript changes

private:
    void read(PrStringView text, size_t offset);
    void readRecord(RecordRange range);
    void addBlocks(uint32_t entry, uint32_t content);
    void link();

    PrJson* json;
    size_t consumed = 0;            // the bytes of the file that were read, up to the end of the last whole line
    String customTitle;
    String aiTitle;
    String leafUuid;                // the newest message as of the user's last prompt, which names the branch shown
    Map<String, uint32_t> byUuid;   // the entry of each record's uuid
    Map<String, uint32_t> byMessage;
    Map<String, BlockRef> calls;    // the call of each tool use id
    Map<String, BlockRef> results;  // the result of each tool use id, which may arrive before its call
};

} // namespace harness

PR_ASSUME_NONNULL_END
