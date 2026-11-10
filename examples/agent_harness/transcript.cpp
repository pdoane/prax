#include "examples/agent_harness/transcript.h"

#include "pr/environment.h"
#include "pr/file.h"
#include "pr/json.h"
#include "pr/min_max.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

namespace harness {

namespace {

// How much of each end of a file a list of sessions reads: the head for the first prompt and the directory, and the
// tail for the newest title.
constexpr size_t kHeadBytes = 256 * 1024;
constexpr size_t kTailBytes = 512 * 1024;

// The longest line that summarizes a record or stands in for a title, in bytes.
constexpr size_t kLineBytes = 200;

PrStringView stringOf(const PrJson* json, PrJsonValueIndex object, PrStringView key) {
    PrJsonValueIndex value = prJsonMember(json, object, key);
    return value != kPrJsonMissing ? prJsonString(json, value) : PrStringView{};
}

bool flagOf(const PrJson* json, PrJsonValueIndex object, PrStringView key) {
    return prJsonKind(json, prJsonMember(json, object, key)) == PrJsonKind_True;
}

// Calls `fn` with each line of `text`, without its line break. Returns early if `fn` returns false.
template <typename Fn>
void eachLine(PrStringView text, Fn fn) {
    while (text.size != 0) {
        size_t end = prStringFindByte(text, '\n');
        PrStringView line = prStringSlice(text, 0, end);
        if (line.size != 0 && line.data[line.size - 1] == '\r')
            --line.size;
        if (!fn(line))
            return;
        text = prStringFrom(text, end < text.size ? end + 1 : end);
    }
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

// Returns the first line of `text` that is not blank, without the spaces around it, cut to kLineBytes at the start of a
// character.
PrStringView firstLine(PrStringView text) {
    PrStringView found{};
    eachLine(text, [&](PrStringView line) {
        while (line.size != 0 && isSpace(line.data[0]))
            line = prStringFrom(line, 1);
        while (line.size != 0 && isSpace(line.data[line.size - 1]))
            --line.size;
        found = line;
        return line.size == 0;
    });
    if (found.size > kLineBytes) {
        found.size = kLineBytes;
        while (found.size != 0 && (uint8_t(found.data[found.size]) & 0xc0) == 0x80)
            --found.size;
    }
    return found;
}

// Returns the text of a message's content: a string, or the first text block of an array.
PrStringView contentText(const PrJson* json, PrJsonValueIndex content) {
    if (prJsonKind(json, content) == PrJsonKind_String)
        return prJsonString(json, content);
    for (PrJsonValueIndex at = prJsonFirst(json, content); at != kPrJsonMissing; at = prJsonAfter(json, content, at)) {
        if (stringOf(json, at, "type") == "text")
            return stringOf(json, at, "text");
    }
    return PrStringView{};
}

// Returns the text of a tool's result: its content, a string or an array whose text blocks are joined by line feeds.
String resultText(const PrJson* json, PrJsonValueIndex block) {
    PrJsonValueIndex content = prJsonMember(json, block, "content");
    if (prJsonKind(json, content) == PrJsonKind_String)
        return String(prJsonString(json, content));
    String text;
    for (PrJsonValueIndex at = prJsonFirst(json, content); at != kPrJsonMissing; at = prJsonAfter(json, content, at)) {
        if (stringOf(json, at, "type") != "text")
            continue;
        if (!text.isEmpty())
            text.append('\n');
        text.append(stringOf(json, at, "text"));
    }
    return text;
}

// Returns true if `text` is wholly one element of markup, as <system-reminder>...</system-reminder> is, whose name is
// lowercase letters, digits, hyphens and underscores.
bool isMarkup(PrStringView text) {
    PrStringView trimmed = prStringTrim(text);
    if (trimmed.size < 3 || trimmed.data[0] != '<')
        return false;
    size_t end = 1;
    while (end < trimmed.size && ((trimmed.data[end] >= 'a' && trimmed.data[end] <= 'z') ||
            (trimmed.data[end] >= '0' && trimmed.data[end] <= '9') || trimmed.data[end] == '-' || trimmed.data[end] == '_'))
        ++end;
    if (end == 1 || end == trimmed.size || (trimmed.data[end] != '>' && trimmed.data[end] != ' '))
        return false;
    String closing = "</";
    closing.append(prStringSlice(trimmed, 1, end - 1));
    closing.append('>');
    return prStringEndsWith(trimmed, closing);
}

// Returns the value at `index` as compact JSON.
String jsonText(const PrJson* json, PrJsonValueIndex index) {
    PrJsonWriter* writer = prJsonWriterCreate();
    prJsonWriterValue(writer, json, index);
    String text(prJsonWriterText(writer));
    prJsonWriterDestroy(writer);
    return text;
}

// Returns the first line of the first string among the members of the object at `object`, other than its type, which
// says what the object is about.
PrStringView summaryOf(const PrJson* json, PrJsonValueIndex object) {
    for (PrJsonValueIndex key = prJsonFirst(json, object); key != kPrJsonMissing; key = prJsonAfter(json, object, key)) {
        if (prJsonKind(json, key + 1) == PrJsonKind_String && prJsonString(json, key) != "type") {
            PrStringView line = firstLine(prJsonString(json, key + 1));
            if (line.size != 0)
                return line;
        }
    }
    return PrStringView{};
}

// Reads what a list of sessions shows from one window of a file: the first prompt and the directory from the head, and
// the newest titles from either.
void indexWindow(PrJson* json, PrStringView window, bool head, SessionInfo& info, String& customTitle,
        String& aiTitle, String& prompt) {
    eachLine(window, [&](PrStringView line) {
        bool title = prStringContains(line, "\"custom-title\"") || prStringContains(line, "\"ai-title\"");
        bool wanted = title || (head && (prompt.isEmpty() || info.directory.isEmpty()));
        if (!wanted || !prJsonParse(json, line, nullptr))
            return true;
        PrStringView type = stringOf(json, 0, "type");
        if (type == "custom-title")
            customTitle = stringOf(json, 0, "customTitle");
        else if (type == "ai-title")
            aiTitle = stringOf(json, 0, "aiTitle");
        if (!head)
            return true;
        if (info.directory.isEmpty())
            info.directory = stringOf(json, 0, "cwd");
        if (prompt.isEmpty() && type == "user" && !flagOf(json, 0, "isMeta")) {
            PrJsonValueIndex message = prJsonMember(json, 0, "message");
            prompt = firstLine(contentText(json, prJsonMember(json, message, "content")));
        }
        return true;
    });
}

// Reads the title and the directory of the session at `info.path`.
void indexSession(PrJson* json, SessionInfo& info) {
    PrByteView bytes{};
    if (!prFileMap(info.path, &bytes))
        return;
    PrStringView text{reinterpret_cast<const char*>(bytes.data), bytes.size};
    String customTitle;
    String aiTitle;
    String prompt;
    // Each window leaves out the line that it cuts.
    PrStringView head = prStringSlice(text, 0, min(text.size, kHeadBytes));
    if (head.size < text.size)
        head.size = prStringFindLastByte(head, '\n');
    indexWindow(json, head, true, info, customTitle, aiTitle, prompt);
    if (head.size < text.size) {
        PrStringView tail = prStringFrom(text, max(head.size, text.size - min(text.size, kTailBytes)));
        size_t start = prStringFindByte(tail, '\n');
        indexWindow(json, prStringFrom(tail, start < tail.size ? start + 1 : start), false, info, customTitle, aiTitle, prompt);
    }
    prFileUnmap(&bytes);
    info.title = !customTitle.isEmpty() ? move(customTitle) : !aiTitle.isEmpty() ? move(aiTitle) : move(prompt);
}

// Returns `directory` with `name` appended as a path's last part.
String joinPath(PrStringView directory, PrStringView name) {
    PrTextBuffer path{};
    prTextBufferAppend(&path, directory);
    prFilePathAppend(&path, name);
    String joined(prTextBufferView(&path));
    prTextBufferReset(&path);
    return joined;
}

// Collects the names of a directory's entries of one kind.
struct Names {
    PrFileKind kind;
    Array<String> names;
};

void collectName(void* context, const PrFileEntry* entry) {
    Names& names = *static_cast<Names*>(context);
    if (entry->info.kind == names.kind)
        names.names.push(String(entry->name));
}

// Adds the store of the configuration directory `config`, named `name`, if it has projects and is not already listed.
void addStore(Array<Store>& stores, PrStringView name, PrStringView config) {
    String projects = joinPath(config, "projects");
    PrFileInfo info{};
    if (!prFileInfo(projects, &info) || info.kind != PrFileKind_Directory)
        return;
    for (const Store& store : stores) {
        if (prFileSame(store.projects, projects))
            return;
    }
    stores.push(Store{String(name), move(projects)});
}

// Adds the agents whose transcripts are in `directory`, as agent-<id>.jsonl beside agent-<id>.meta.json.
void addAgents(PrJson* json, PrStringView directory, Array<AgentInfo>& agents) {
    Names files{.kind = PrFileKind_Regular};
    prFileEachEntry(directory, collectName, &files);
    for (const String& file : files.names) {
        if (!prStringStartsWith(file, "agent-") || !prStringEndsWith(file, ".jsonl"))
            continue;
        AgentInfo& agent = agents.emplace();
        agent.path = joinPath(directory, file);
        String meta(prStringSlice(file, 0, file.count() - 6));
        meta.append(".meta.json");
        PrByteView bytes{};
        if (prFileMap(joinPath(directory, meta), &bytes)) {
            if (prJsonParse(json, PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size}, nullptr)) {
                agent.agentType = stringOf(json, 0, "agentType");
                agent.description = stringOf(json, 0, "description");
                agent.toolUseId = stringOf(json, 0, "toolUseId");
            }
            prFileUnmap(&bytes);
        }
    }
}

// The offset in a file of no call.
constexpr size_t kNoCall = ~size_t(0);

// Returns where the transcript at `path` holds the call `toolUseId`, or kNoCall if it does not.
size_t callAt(PrStringView path, PrStringView toolUseId) {
    PrByteView bytes{};
    if (toolUseId.size == 0 || !prFileMap(path, &bytes))
        return kNoCall;
    String needle = "\"id\":\"";
    needle.append(toolUseId);
    needle.append('"');
    PrStringView text{reinterpret_cast<const char*>(bytes.data), bytes.size};
    size_t at = prStringFind(text, needle);
    prFileUnmap(&bytes);
    return at < text.size ? at : kNoCall;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// The store

Array<Store> findStores() {
    Array<Store> stores;
    String home = String::adopt(prFileUserDirectory(PrFileUserDirectory_Home));
    if (!home.isEmpty()) {
        Names configs{.kind = PrFileKind_Directory};
        prFileEachEntry(home, collectName, &configs);
        sortItems(configs.names.span(), [](const String& a, const String& b) { return prStringCompare(a, b) < 0; });
        for (const String& name : configs.names) {
            if (name == ".claude" || prStringStartsWith(name, ".claude-"))
                addStore(stores, name, joinPath(home, name));
        }
    }
    String config = String::adopt(prEnvironmentVariable("CLAUDE_CONFIG_DIR"));
    if (!config.isEmpty())
        addStore(stores, prFilePathName(config), config);
    return stores;
}

Array<SessionInfo> listSessions(const Array<Store>& stores) {
    Array<SessionInfo> sessions;
    for (uint32_t s = 0; s < stores.count(); ++s) {
        Names projects{.kind = PrFileKind_Directory};
        prFileEachEntry(stores[s].projects, collectName, &projects);
        for (const String& project : projects.names) {
            String directory = joinPath(stores[s].projects, project);
            Names files{.kind = PrFileKind_Regular};
            prFileEachEntry(directory, collectName, &files);
            for (const String& file : files.names) {
                if (!prStringEndsWith(file, ".jsonl"))
                    continue;
                SessionInfo& info = sessions.emplace();
                info.path = joinPath(directory, file);
                info.store = s;
                PrFileInfo stat{};
                prFileInfo(info.path, &stat);
                info.size = stat.size;
                info.modified = stat.modified;
            }
        }
    }
    PrJson* json = prJsonCreate();
    for (SessionInfo& info : sessions)
        indexSession(json, info);
    prJsonDestroy(json);
    sortItems(sessions.span(), [](const SessionInfo& a, const SessionInfo& b) { return a.modified > b.modified; });
    return sessions;
}

// ---------------------------------------------------------------------------------------------------------------------
// Subagents

Array<AgentInfo> listAgents(PrStringView path) {
    Array<AgentInfo> found;
    PrStringView name = prFilePathName(path);
    String directory = joinPath(joinPath(prFilePathParent(path), prStringSlice(name, 0, name.size - 6)), "subagents");
    PrJson* json = prJsonCreate();
    addAgents(json, directory, found);
    Names workflows{.kind = PrFileKind_Directory};
    String workflowsDirectory = joinPath(directory, "workflows");
    prFileEachEntry(workflowsDirectory, collectName, &workflows);
    for (const String& workflow : workflows.names)
        addAgents(json, joinPath(workflowsDirectory, workflow), found);
    prJsonDestroy(json);
    // An agent hangs under the agent whose transcript holds its call, and keeps where the call is there, which orders
    // it among its siblings.
    Array<size_t> calls;
    for (uint32_t a = 0; a < found.count(); ++a) {
        AgentInfo& agent = found[a];
        size_t at = callAt(path, agent.toolUseId);
        for (uint32_t i = 0; at == kNoCall && i < found.count(); ++i) {
            at = i != a ? callAt(found[i].path, agent.toolUseId) : kNoCall;
            agent.parent = at != kNoCall ? i : kNone;
        }
        calls.push(at);
    }
    Array<uint32_t> order;
    for (uint32_t i = 0; i < found.count(); ++i)
        order.push(i);
    sortItems(order.span(), [&](uint32_t a, uint32_t b) { return calls[a] < calls[b]; });
    Array<AgentInfo> agents;
    Array<uint32_t> placed;
    placed.resize(found.count());
    auto place = [&](auto& placeUnder, uint32_t parent, uint32_t depth) -> void {
        for (uint32_t i : order) {
            if (found[i].parent != parent)
                continue;
            placed[i] = agents.count();
            AgentInfo& agent = agents.emplace();
            agent = found[i];
            agent.parent = parent != kNone ? placed[parent] : kNone;
            agent.depth = depth;
            placeUnder(placeUnder, i, depth + 1);
        }
    };
    place(place, kNone, 1);
    return agents;
}

// ---------------------------------------------------------------------------------------------------------------------
// Transcripts

Transcript::Transcript() : json(prJsonCreate()) {}

Transcript::~Transcript() {
    prJsonDestroy(json);
}

void Transcript::clear() {
    path.clear();
    title.clear();
    entries.clear();
    conversation.clear();
    consumed = 0;
    customTitle.clear();
    aiTitle.clear();
    leafUuid.clear();
    byUuid.clear();
    byMessage.clear();
    calls.clear();
    results.clear();
    ++revision;
}

bool Transcript::load(PrStringView file) {
    String loaded(file);
    clear();
    path = move(loaded);
    PrFileInfo info{};
    if (!prFileInfo(path, &info) || info.kind != PrFileKind_Regular)
        return false;
    refresh();
    return true;
}

bool Transcript::refresh() {
    PrFileInfo info{};
    if (path.isEmpty() || !prFileInfo(path, &info) || info.kind != PrFileKind_Regular)
        return false;
    if (info.size < consumed) {
        String reloaded(move(path));
        clear();
        path = move(reloaded);
    }
    if (info.size == consumed)
        return false;
    PrByteView bytes{};
    if (!prFileMap(path, &bytes))
        return false;
    PrStringView text{reinterpret_cast<const char*>(bytes.data) + consumed, bytes.size - consumed};
    size_t last = prStringFindLastByte(text, '\n');
    bool whole = last < text.size;
    if (whole) {
        size_t offset = consumed;
        text.size = last + 1;
        consumed += text.size;
        read(text, offset);
    }
    prFileUnmap(&bytes);
    return whole;
}

String Transcript::record(RecordRange range) const {
    PrByteView bytes{};
    if (!prFileMap(path, &bytes))
        return String();
    String line;
    if (range.offset + range.size <= bytes.size)
        line = PrStringView{reinterpret_cast<const char*>(bytes.data) + range.offset, range.size};
    prFileUnmap(&bytes);
    return line;
}

void Transcript::read(PrStringView text, size_t offset) {
    eachLine(text, [&](PrStringView line) {
        if (line.size != 0 && prJsonParse(json, line, nullptr))
            readRecord(RecordRange{offset + size_t(line.data - text.data), line.size});
        return true;
    });
    title = !customTitle.isEmpty() ? customTitle : aiTitle;
    link();
    ++revision;
}

void Transcript::readRecord(RecordRange range) {
    PrStringView type = stringOf(json, 0, "type");
    PrStringView uuid = stringOf(json, 0, "uuid");
    // A record without a uuid carries the session's state, which the newest such record of each type sets.
    if (uuid.size == 0) {
        if (type == "custom-title")
            customTitle = stringOf(json, 0, "customTitle");
        else if (type == "ai-title")
            aiTitle = stringOf(json, 0, "aiTitle");
        else if (type == "last-prompt")
            leafUuid = stringOf(json, 0, "leafUuid");
        return;
    }
    // A record read twice, as from a file that was written again, joins nothing.
    if (byUuid.contains(uuid))
        return;
    EntryKind kind = EntryKind_Other;
    if (type == "user")
        kind = EntryKind_User;
    else if (type == "assistant")
        kind = EntryKind_Assistant;
    else if (type == "system")
        kind = EntryKind_System;
    else if (type == "attachment")
        kind = EntryKind_Attachment;
    PrJsonValueIndex message = prJsonMember(json, 0, "message");
    PrStringView messageId = kind == EntryKind_Assistant ? stringOf(json, message, "id") : PrStringView{};
    uint32_t index = kNone;
    if (messageId.size != 0) {
        if (const uint32_t* joined = byMessage.get(messageId))
            index = *joined;
    }
    if (index == kNone) {
        index = entries.count();
        Entry& entry = entries.emplace();
        entry.kind = kind;
        entry.meta = flagOf(json, 0, "isMeta");
        entry.sidechain = flagOf(json, 0, "isSidechain");
        entry.type = type;
        entry.uuid = uuid;
        // A compaction starts a new root, which names the message that it follows as its logical parent.
        PrStringView parent = stringOf(json, 0, "parentUuid");
        entry.parentUuid = parent.size != 0 ? parent : stringOf(json, 0, "logicalParentUuid");
        entry.messageId = messageId;
        entry.model = stringOf(json, message, "model");
        entry.timestamp = stringOf(json, 0, "timestamp");
        if (kind == EntryKind_System) {
            entry.subtype = stringOf(json, 0, "subtype");
            entry.summary = firstLine(stringOf(json, 0, "content"));
        } else if (kind == EntryKind_Attachment) {
            PrJsonValueIndex attachment = prJsonMember(json, 0, "attachment");
            entry.subtype = stringOf(json, attachment, "type");
            entry.summary = summaryOf(json, attachment);
        } else if (kind == EntryKind_Other) {
            entry.summary = summaryOf(json, 0);
        }
        if (messageId.size != 0)
            byMessage.set(messageId, index);
    }
    byUuid.set(uuid, index);
    entries[index].records.push(range);
    if (kind == EntryKind_User || kind == EntryKind_Assistant)
        addBlocks(index, prJsonMember(json, message, "content"));
}

void Transcript::addBlocks(uint32_t index, PrJsonValueIndex content) {
    if (prJsonKind(json, content) == PrJsonKind_String) {
        Block& block = entries[index].blocks.emplace();
        block.kind = BlockKind_Text;
        block.text = prJsonString(json, content);
        block.context = isMarkup(block.text);
    }
    for (PrJsonValueIndex at = prJsonFirst(json, content); at != kPrJsonMissing; at = prJsonAfter(json, content, at)) {
        PrStringView type = stringOf(json, at, "type");
        BlockRef ref{index, entries[index].blocks.count()};
        Block& block = entries[index].blocks.emplace();
        if (type == "text") {
            block.kind = BlockKind_Text;
            block.text = stringOf(json, at, "text");
            block.context = isMarkup(block.text);
        } else if (type == "thinking") {
            block.kind = BlockKind_Thinking;
            block.text = stringOf(json, at, "thinking");
        } else if (type == "tool_use") {
            block.kind = BlockKind_ToolUse;
            block.name = stringOf(json, at, "name");
            block.id = stringOf(json, at, "id");
            PrJsonValueIndex input = prJsonMember(json, at, "input");
            block.input = jsonText(json, input);
            block.summary = summaryOf(json, input);
            calls.set(block.id, ref);
            if (const BlockRef* result = results.get(block.id))
                block.result = *result;
        } else if (type == "tool_result") {
            block.kind = BlockKind_ToolResult;
            block.id = stringOf(json, at, "tool_use_id");
            block.error = flagOf(json, at, "is_error");
            block.text = resultText(json, at);
            results.set(block.id, ref);
            if (const BlockRef* call = calls.get(block.id))
                entries[call->entry].blocks[call->block].result = ref;
        } else if (type == "image") {
            block.kind = BlockKind_Image;
            block.text = stringOf(json, prJsonMember(json, at, "source"), "media_type");
        } else {
            block.name = type;
        }
    }
    Entry& entry = entries[index];
    entry.answers = entry.kind == EntryKind_User && entry.blocks.count() != 0;
    for (const Block& block : entry.blocks)
        entry.answers = entry.answers && block.kind == BlockKind_ToolResult;
}

void Transcript::link() {
    // A parent may arrive after its child, so a link that did not resolve is tried again with each read. A link that
    // resolved does not change, because a uuid keeps its entry.
    for (uint32_t i = 0; i < entries.count(); ++i) {
        Entry& entry = entries[i];
        if (entry.parent == kNone && !entry.parentUuid.isEmpty()) {
            const uint32_t* parent = byUuid.get(entry.parentUuid);
            entry.parent = parent != nullptr && *parent != i ? *parent : kNone;
        }
        if (entry.parent != kNone && !entry.sidechain)
            entries[entry.parent].newestChild = i;
    }
    // The conversation shown is the branch of the newest message as of the user's last prompt, down to the newest
    // message under it, which the turn that followed the prompt added. With no last prompt, it ends at the newest
    // message of the main conversation. A malformed file whose links cycle ends it after every entry.
    uint32_t tip = kNone;
    if (const uint32_t* leaf = byUuid.get(leafUuid))
        tip = *leaf;
    for (uint32_t steps = 0; tip != kNone && entries[tip].newestChild != kNone && steps < entries.count(); ++steps)
        tip = entries[tip].newestChild;
    for (uint32_t i = entries.count(); tip == kNone && i > 0; --i) {
        if (!entries[i - 1].sidechain)
            tip = i - 1;
    }
    conversation.clear();
    for (uint32_t at = tip; at != kNone && conversation.count() < entries.count(); at = entries[at].parent)
        conversation.push(at);
    reverseItems(conversation.span());
}

} // namespace harness
