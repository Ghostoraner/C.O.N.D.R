#include <fs/nvmfs.hpp>
#include <lib/spinlock.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>

namespace nord::nvmfs {

static sync::RecursiveSpinLock g_lock;

Lock::Lock() { g_lock.lock(); }
Lock::~Lock() { g_lock.unlock(); }

static constexpr usize NODE_CAPACITY = 128;
static constexpr usize TAG_CAPACITY = 512;
static constexpr usize NAME_CAPACITY = 48;
static constexpr usize TAG_KEY_CAPACITY = 24;
static constexpr usize TAG_VALUE_CAPACITY = 48;
static constexpr usize MAX_STREAM_SIZE = 32768;
static constexpr u64 MAX_STREAM_BYTES = 262144;
static constexpr u32 SNAPSHOT_MAGIC = 0x53464D4E;
static constexpr u16 SNAPSHOT_VERSION = 2;

static Node g_nodes[NODE_CAPACITY];
static TagIndex g_tag_index[TAG_CAPACITY];
static usize g_node_count;
static usize g_tag_count;
static u64 g_stream_bytes;
static u32 g_next_id;
static bool g_initialized;
static ChangeHook g_change_hook;

struct [[gnu::packed]] SnapshotHeader {
    u32 magic;
    u16 version;
    u16 node_count;
    u16 tag_count;
    u16 reserved;
    u32 stream_bytes;
    u64 next_id;
};

struct [[gnu::packed]] SnapshotNode {
    NodeId id;
    NodeId container_id;
    u8 type;
    u32 owner;
    u8 rights;
    char name[NAME_CAPACITY];
    u32 stream_size;
    u32 stream_offset;
};

struct [[gnu::packed]] SnapshotTag {
    NodeId node_id;
    char key[TAG_KEY_CAPACITY];
    char value[TAG_VALUE_CAPACITY];
};

static Node g_restore_nodes[NODE_CAPACITY];
static TagIndex g_restore_tags[TAG_CAPACITY];

static void notify_change() {
    if (g_change_hook) g_change_hook();
}

static usize length(const char* text) {
    return text ? strlen(text) : 0;
}

static bool same(const char* left, const char* right) {
    return left && right && strcmp(left, right) == 0;
}

static TagIndex* find_tag_entry(NodeId id, const char* key, const char* value) {
    for (usize i = 0; i < TAG_CAPACITY; ++i) {
        TagIndex& entry = g_tag_index[i];
        if (entry.active && entry.node_id == id
            && same(entry.key, key) && same(entry.value, value)) return &entry;
    }
    return nullptr;
}

void init() {
    Lock guard;
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(g_tag_index, 0, sizeof(g_tag_index));
    g_node_count = 0;
    g_tag_count = 0;
    g_stream_bytes = 0;
    g_next_id = 1;
    g_initialized = true;
}

void set_change_hook(ChangeHook hook) {
    Lock guard;
    g_change_hook = hook;
}

bool create(NodeType type, const char* node_name, NodeId container_id,
            NodeId& result, Error& error) {
    Lock guard;
    if (!g_initialized) { error = Error::not_initialized; return false; }
    if (!node_name || !*node_name || length(node_name) >= NAME_CAPACITY) {
        error = Error::invalid_argument;
        return false;
    }
    if (container_id) {
        Node* container = find(container_id);
        if (!container) { error = Error::not_found; return false; }
        if (container->type != NodeType::vault && container->type != NodeType::directory) {
            error = Error::invalid_argument;
            return false;
        }
    }

    usize slot = NODE_CAPACITY;
    for (usize i = 0; i < NODE_CAPACITY; ++i) {
        if (!g_nodes[i].active) { slot = i; break; }
    }
    if (slot == NODE_CAPACITY) { error = Error::table_full; return false; }

    Node& node = g_nodes[slot];
    memset(&node, 0, sizeof(node));
    node.id = g_next_id++;
    node.container_id = container_id;
    node.type = type;
    node.active = true;
    node.owner = 0;
    node.rights = static_cast<u8>(Right::all);
    memcpy(node.name, node_name, length(node_name) + 1);
    ++g_node_count;
    result = node.id;
    error = Error::none;
    notify_change();
    return true;
}

Node* find(NodeId id) {
    Lock guard;
    if (!id) return nullptr;
    for (usize i = 0; i < NODE_CAPACITY; ++i)
        if (g_nodes[i].active && g_nodes[i].id == id) return &g_nodes[i];
    return nullptr;
}

Node* at(usize active_position) {
    Lock guard;
    for (usize i = 0; i < NODE_CAPACITY; ++i) {
        if (!g_nodes[i].active) continue;
        if (active_position-- == 0) return &g_nodes[i];
    }
    return nullptr;
}

Node* directory_at(const Directory& directory, usize& cursor) {
    Lock guard;
    while (cursor < NODE_CAPACITY) {
        Node* node = &g_nodes[cursor++];
        if (node->active && node->container_id == directory.container_id) return node;
    }
    return nullptr;
}

usize count() {
    Lock guard;
    return g_node_count;
}

bool add_tag(NodeId id, const char* key, const char* value, Error& error) {
    Lock guard;
    Node* node = find(id);
    if (!node) { error = Error::not_found; return false; }
    if (!key || !*key || !value || length(key) >= TAG_KEY_CAPACITY
        || length(value) >= TAG_VALUE_CAPACITY) {
        error = Error::invalid_argument;
        return false;
    }
    if (find_tag_entry(id, key, value)) { error = Error::none; return true; }

    for (usize i = 0; i < TAG_CAPACITY; ++i) {
        TagIndex& entry = g_tag_index[i];
        if (entry.active) continue;
        memset(&entry, 0, sizeof(entry));
        entry.node_id = id;
        entry.active = true;
        memcpy(entry.key, key, length(key) + 1);
        memcpy(entry.value, value, length(value) + 1);
        ++g_tag_count;
        error = Error::none;
        notify_change();
        return true;
    }
    error = Error::table_full;
    return false;
}

bool remove_tag(NodeId id, const char* key, const char* value, Error& error) {
    Lock guard;
    TagIndex* entry = find_tag_entry(id, key, value);
    if (!entry) { error = Error::not_found; return false; }
    entry->active = false;
    --g_tag_count;
    error = Error::none;
    notify_change();
    return true;
}

bool find_tag(const char* key, const char* value, usize& cursor, NodeId& result) {
    Lock guard;
    if (!key || !value) return false;
    while (cursor < TAG_CAPACITY) {
        const TagIndex& entry = g_tag_index[cursor++];
        if (entry.active && same(entry.key, key) && same(entry.value, value)
            && find(entry.node_id)) {
            result = entry.node_id;
            return true;
        }
    }
    return false;
}

bool tag_at(usize& cursor, TagView& result) {
    Lock guard;
    while (cursor < TAG_CAPACITY) {
        const TagIndex& entry = g_tag_index[cursor++];
        if (!entry.active || !find(entry.node_id)) continue;
        result = {entry.node_id, entry.key, entry.value};
        return true;
    }
    return false;
}

bool write(NodeId id, const void* bytes, usize size, bool append, Error& error) {
    Lock guard;
    Node* node = find(id);
    if (!node) { error = Error::not_found; return false; }
    if (node->type != NodeType::file) { error = Error::invalid_argument; return false; }
    if (size && !bytes) { error = Error::invalid_argument; return false; }

    const usize offset = append ? node->stream.size : 0;
    if (size > MAX_STREAM_SIZE - offset) { error = Error::stream_too_large; return false; }
    const usize new_size = offset + size;
    if (g_stream_bytes - node->stream.size + new_size > MAX_STREAM_BYTES) {
        error = Error::stream_too_large;
        return false;
    }

    if (new_size > node->stream.capacity) {
        usize capacity = node->stream.capacity ? node->stream.capacity : 64;
        while (capacity < new_size) capacity *= 2;
        if (capacity > MAX_STREAM_SIZE) capacity = MAX_STREAM_SIZE;
        u8* data = static_cast<u8*>(mm::heap::krealloc(node->stream.bytes, capacity));
        if (!data) { error = Error::no_memory; return false; }
        node->stream.bytes = data;
        node->stream.capacity = capacity;
    }

    if (!append && node->stream.size) memset(node->stream.bytes, 0, node->stream.size);
    if (size) memcpy(node->stream.bytes + offset, bytes, size);
    g_stream_bytes = g_stream_bytes - node->stream.size + new_size;
    node->stream.size = new_size;
    error = Error::none;
    notify_change();
    return true;
}

bool erase(NodeId id, Error& error) {
    Lock guard;
    Node* node = find(id);
    if (!node) { error = Error::not_found; return false; }
    if (node->type == NodeType::vault || node->type == NodeType::directory) {
        for (usize i = 0; i < NODE_CAPACITY; ++i) {
            if (g_nodes[i].active && g_nodes[i].container_id == id) {
                error = Error::container_not_empty;
                return false;
            }
        }
    }
    for (usize i = 0; i < TAG_CAPACITY; ++i) {
        if (g_tag_index[i].active && g_tag_index[i].node_id == id) {
            g_tag_index[i].active = false;
            --g_tag_count;
        }
    }
    g_stream_bytes -= node->stream.size;
    if (node->stream.bytes) mm::heap::kfree(node->stream.bytes);
    memset(node, 0, sizeof(*node));
    --g_node_count;
    error = Error::none;
    notify_change();
    return true;
}

const char* type_name(NodeType type) {
    switch (type) {
    case NodeType::file: return "file";
    case NodeType::directory: return "directory";
    case NodeType::vault: return "vault";
    case NodeType::device: return "device";
    case NodeType::service: return "service";
    case NodeType::process: return "process";
    }
    return "unknown";
}

bool parse_type(const char* text, NodeType& type) {
    if (same(text, "file")) type = NodeType::file;
    else if (same(text, "directory")) type = NodeType::directory;
    else if (same(text, "vault")) type = NodeType::vault;
    else if (same(text, "device")) type = NodeType::device;
    else if (same(text, "process")) type = NodeType::process;
    else if (same(text, "service")) type = NodeType::service;
    else return false;
    return true;
}

const char* error_text(Error error) {
    switch (error) {
    case Error::none: return "no error";
    case Error::not_initialized: return "NVMFS is not initialized";
    case Error::invalid_argument: return "invalid argument";
    case Error::not_found: return "object not found";
    case Error::already_exists: return "already exists";
    case Error::no_memory: return "kernel heap exhausted";
    case Error::table_full: return "NVMFS table full";
    case Error::stream_too_large: return "stream storage limit reached";
    case Error::container_not_empty: return "container still owns objects";
    }
    return "unknown NVMFS error";
}

void stats(u64& nodes, u64& tags, u64& stream_bytes, u64& stream_limit) {
    Lock guard;
    nodes = g_node_count;
    tags = g_tag_count;
    stream_bytes = g_stream_bytes;
    stream_limit = MAX_STREAM_BYTES;
}

bool set_owner(NodeId id, u32 owner, Error& error) {
    Lock guard;
    if (!g_initialized) { error = Error::not_initialized; return false; }
    Node* node = find(id);
    if (!node) { error = Error::not_found; return false; }
    node->owner = owner;
    notify_change();
    error = Error::none;
    return true;
}

bool set_rights(NodeId id, u8 rights, Error& error) {
    Lock guard;
    if (!g_initialized) { error = Error::not_initialized; return false; }
    Node* node = find(id);
    if (!node) { error = Error::not_found; return false; }
    node->rights = rights;
    notify_change();
    error = Error::none;
    return true;
}

bool check_rights(NodeId id, u8 required, bool& has) {
    Lock guard;
    if (!g_initialized) { has = false; return false; }
    Node* node = find(id);
    if (!node) { has = false; return false; }
    has = (node->rights & required) == required;
    return true;
}

u8 rights_of(NodeId id) {
    Lock guard;
    if (!g_initialized) return 0;
    Node* node = find(id);
    return node ? node->rights : 0;
}

bool selftest(u32& checks, Error& error) {
    Lock guard;
    checks = 0;
    NodeId vault_id = 0;
    if (!create(NodeType::vault, "selftest-vault", 0, vault_id, error)) return false;
    ++checks;
    NodeId file_id = 0;
    if (!create(NodeType::file, "probe", vault_id, file_id, error)) {
        erase(vault_id, error);
        return false;
    }
    ++checks;
    if (!add_tag(file_id, "suite", "selftest", error)) {
        erase(file_id, error);
        erase(vault_id, error);
        return false;
    }
    ++checks;
    static constexpr char payload[] = "nvmfs-stream";
    if (!write(file_id, payload, sizeof(payload) - 1, false, error)) {
        erase(file_id, error);
        erase(vault_id, error);
        return false;
    }
    ++checks;
    usize cursor = 0;
    NodeId found = 0;
    if (!find_tag("suite", "selftest", cursor, found) || found != file_id) {
        error = Error::not_found;
        erase(file_id, error);
        erase(vault_id, error);
        return false;
    }
    ++checks;
    if (!erase(file_id, error) || !erase(vault_id, error)) return false;
    ++checks;
    error = Error::none;
    return true;
}

bool serialize(void* output, usize capacity, usize& size) {
    Lock guard;
    size = 0;
    if (!g_initialized || !output) return false;

    usize node_count = 0;
    usize tag_count = 0;
    usize stream_bytes = 0;
    for (usize i = 0; i < NODE_CAPACITY; ++i) {
        if (!g_nodes[i].active) continue;
        ++node_count;
        stream_bytes += g_nodes[i].stream.size;
    }
    for (usize i = 0; i < TAG_CAPACITY; ++i)
        if (g_tag_index[i].active) ++tag_count;

    const usize records_size = sizeof(SnapshotHeader)
        + node_count * sizeof(SnapshotNode)
        + tag_count * sizeof(SnapshotTag);
    if (stream_bytes > MAX_STREAM_BYTES || records_size > capacity
        || stream_bytes > capacity - records_size) return false;

    auto* bytes = static_cast<u8*>(output);
    SnapshotHeader header{SNAPSHOT_MAGIC, SNAPSHOT_VERSION,
                          static_cast<u16>(node_count), static_cast<u16>(tag_count),
                          0, static_cast<u32>(stream_bytes), g_next_id};
    memcpy(bytes, &header, sizeof(header));
    usize offset = sizeof(header);
    usize stream_offset = 0;
    for (usize i = 0; i < NODE_CAPACITY; ++i) {
        const Node& node = g_nodes[i];
        if (!node.active) continue;
        SnapshotNode record{};
        record.id = node.id;
        record.container_id = node.container_id;
        record.type = static_cast<u8>(node.type);
    record.owner = node.owner;
    record.rights = node.rights;
        memcpy(record.name, node.name, NAME_CAPACITY);
        record.stream_size = static_cast<u32>(node.stream.size);
        record.stream_offset = static_cast<u32>(stream_offset);
        memcpy(bytes + offset, &record, sizeof(record));
        offset += sizeof(record);
        stream_offset += node.stream.size;
    }
    for (usize i = 0; i < TAG_CAPACITY; ++i) {
        const TagIndex& tag = g_tag_index[i];
        if (!tag.active) continue;
        SnapshotTag record{};
        record.node_id = tag.node_id;
        memcpy(record.key, tag.key, TAG_KEY_CAPACITY);
        memcpy(record.value, tag.value, TAG_VALUE_CAPACITY);
        memcpy(bytes + offset, &record, sizeof(record));
        offset += sizeof(record);
    }
    const usize stream_data_offset = offset;
    stream_offset = 0;
    for (usize i = 0; i < NODE_CAPACITY; ++i) {
        const Node& node = g_nodes[i];
        if (!node.active || !node.stream.size) continue;
        memcpy(bytes + stream_data_offset + stream_offset, node.stream.bytes, node.stream.size);
        stream_offset += node.stream.size;
    }
    size = records_size + stream_bytes;
    return true;
}

static bool has_terminator(const char* text, usize capacity) {
    for (usize i = 0; i < capacity; ++i) if (!text[i]) return true;
    return false;
}

static void free_restore_nodes() {
    for (usize i = 0; i < NODE_CAPACITY; ++i) {
        if (g_restore_nodes[i].stream.bytes) mm::heap::kfree(g_restore_nodes[i].stream.bytes);
        memset(&g_restore_nodes[i], 0, sizeof(Node));
    }
}

bool deserialize(const void* input, usize size) {
    Lock guard;
    if (!input || size < sizeof(SnapshotHeader)) return false;
    SnapshotHeader header{};
    memcpy(&header, input, sizeof(header));
    if (header.magic != SNAPSHOT_MAGIC || header.version != SNAPSHOT_VERSION
        || header.node_count > NODE_CAPACITY || header.tag_count > TAG_CAPACITY
        || header.stream_bytes > MAX_STREAM_BYTES) return false;

    const usize nodes_size = static_cast<usize>(header.node_count) * sizeof(SnapshotNode);
    const usize tags_size = static_cast<usize>(header.tag_count) * sizeof(SnapshotTag);
    const usize stream_offset = sizeof(SnapshotHeader) + nodes_size + tags_size;
    if (stream_offset > size || header.stream_bytes > size - stream_offset) return false;

    free_restore_nodes();
    memset(g_restore_tags, 0, sizeof(g_restore_tags));
    const auto* bytes = static_cast<const u8*>(input);
    usize offset = sizeof(SnapshotHeader);
    u64 max_id = 0;
    u64 stream_total = 0;
    for (usize i = 0; i < header.node_count; ++i) {
        SnapshotNode record{};
        memcpy(&record, bytes + offset, sizeof(record));
        offset += sizeof(record);
        if (!record.id || record.type > static_cast<u8>(NodeType::process)
            || !has_terminator(record.name, NAME_CAPACITY)
            || record.stream_size > MAX_STREAM_SIZE
            || record.stream_offset > header.stream_bytes
            || record.stream_size > header.stream_bytes - record.stream_offset) {
            free_restore_nodes();
            return false;
        }
        for (usize prior = 0; prior < i; ++prior) {
            if (g_restore_nodes[prior].id == record.id) {
                free_restore_nodes();
                return false;
            }
        }

        Node& node = g_restore_nodes[i];
        node.id = record.id;
        node.container_id = record.container_id;
        node.type = static_cast<NodeType>(record.type);
        node.active = true;
        node.owner = record.owner;
        node.rights = record.rights;
        memcpy(node.name, record.name, NAME_CAPACITY);
        node.stream.size = record.stream_size;
        node.stream.capacity = record.stream_size;
        if (record.stream_size) {
            node.stream.bytes = static_cast<u8*>(mm::heap::kmalloc(record.stream_size));
            if (!node.stream.bytes) {
                free_restore_nodes();
                return false;
            }
            memcpy(node.stream.bytes, bytes + stream_offset + record.stream_offset,
                   record.stream_size);
        }
        stream_total += record.stream_size;
        if (record.id > max_id) max_id = record.id;
    }
    if (stream_total != header.stream_bytes) {
        free_restore_nodes();
        return false;
    }

    for (usize i = 0; i < header.tag_count; ++i) {
        SnapshotTag record{};
        memcpy(&record, bytes + offset, sizeof(record));
        offset += sizeof(record);
        if (!record.node_id || !has_terminator(record.key, TAG_KEY_CAPACITY)
            || !record.key[0] || !has_terminator(record.value, TAG_VALUE_CAPACITY)) {
            free_restore_nodes();
            return false;
        }
        bool node_found = false;
        for (usize n = 0; n < header.node_count; ++n)
            if (g_restore_nodes[n].id == record.node_id) node_found = true;
        if (!node_found) {
            free_restore_nodes();
            return false;
        }
        TagIndex& tag = g_restore_tags[i];
        tag.node_id = record.node_id;
        tag.active = true;
        memcpy(tag.key, record.key, TAG_KEY_CAPACITY);
        memcpy(tag.value, record.value, TAG_VALUE_CAPACITY);
    }

    if (!header.next_id || header.next_id <= max_id) {
        free_restore_nodes();
        return false;
    }
    for (usize i = 0; i < header.node_count; ++i) {
        const Node& node = g_restore_nodes[i];
        if (!node.container_id) continue;
        bool valid_container = false;
        for (usize parent = 0; parent < header.node_count; ++parent) {
            const Node& candidate = g_restore_nodes[parent];
            if (candidate.id == node.container_id
                && (candidate.type == NodeType::vault || candidate.type == NodeType::directory))
                valid_container = true;
        }
        if (!valid_container) {
            free_restore_nodes();
            return false;
        }
    }

    for (usize i = 0; i < NODE_CAPACITY; ++i)
        if (g_nodes[i].stream.bytes) mm::heap::kfree(g_nodes[i].stream.bytes);
    memset(g_nodes, 0, sizeof(g_nodes));
    memcpy(g_nodes, g_restore_nodes, sizeof(g_nodes));
    memset(g_restore_nodes, 0, sizeof(g_restore_nodes));
    memcpy(g_tag_index, g_restore_tags, sizeof(g_tag_index));
    g_node_count = header.node_count;
    g_tag_count = header.tag_count;
    g_stream_bytes = stream_total;
    g_next_id = header.next_id;
    g_initialized = true;
    return true;
}

}