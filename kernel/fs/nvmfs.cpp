#include <fs/nvmfs.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>

namespace nord::nvmfs {

static constexpr usize NODE_CAPACITY = 128;
static constexpr usize TAG_CAPACITY = 512;
static constexpr usize NAME_CAPACITY = 48;
static constexpr usize TAG_KEY_CAPACITY = 24;
static constexpr usize TAG_VALUE_CAPACITY = 48;
static constexpr usize MAX_STREAM_SIZE = 32768;
static constexpr u64 MAX_STREAM_BYTES = 262144;
static constexpr NodeId ID_BASE = 0x4E564D4600000000ULL;

static Node g_nodes[NODE_CAPACITY];
static TagIndex g_tag_index[TAG_CAPACITY];
static usize g_node_count;
static usize g_tag_count;
static u64 g_stream_bytes;
static u64 g_next_id;
static bool g_initialized;

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
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(g_tag_index, 0, sizeof(g_tag_index));
    g_node_count = 0;
    g_tag_count = 0;
    g_stream_bytes = 0;
    g_next_id = 1;
    g_initialized = true;
}

bool create(NodeType type, const char* node_name, NodeId container_id,
            NodeId& result, Error& error) {
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
    node.id = ID_BASE | g_next_id++;
    node.container_id = container_id;
    node.type = type;
    node.active = true;
    memcpy(node.name, node_name, length(node_name) + 1);
    ++g_node_count;
    result = node.id;
    error = Error::none;
    return true;
}

Node* find(NodeId id) {
    if (!id) return nullptr;
    for (usize i = 0; i < NODE_CAPACITY; ++i)
        if (g_nodes[i].active && g_nodes[i].id == id) return &g_nodes[i];
    return nullptr;
}

Node* at(usize active_position) {
    for (usize i = 0; i < NODE_CAPACITY; ++i) {
        if (!g_nodes[i].active) continue;
        if (active_position-- == 0) return &g_nodes[i];
    }
    return nullptr;
}

Node* directory_at(const Directory& directory, usize& cursor) {
    while (cursor < NODE_CAPACITY) {
        Node* node = &g_nodes[cursor++];
        if (node->active && node->container_id == directory.container_id) return node;
    }
    return nullptr;
}

usize count() { return g_node_count; }

bool add_tag(NodeId id, const char* key, const char* value, Error& error) {
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
        return true;
    }
    error = Error::table_full;
    return false;
}

bool remove_tag(NodeId id, const char* key, const char* value, Error& error) {
    TagIndex* entry = find_tag_entry(id, key, value);
    if (!entry) { error = Error::not_found; return false; }
    entry->active = false;
    --g_tag_count;
    error = Error::none;
    return true;
}

bool find_tag(const char* key, const char* value, usize& cursor, NodeId& result) {
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
    while (cursor < TAG_CAPACITY) {
        const TagIndex& entry = g_tag_index[cursor++];
        if (!entry.active || !find(entry.node_id)) continue;
        result = {entry.node_id, entry.key, entry.value};
        return true;
    }
    return false;
}

bool write(NodeId id, const void* bytes, usize size, bool append, Error& error) {
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
    return true;
}

bool erase(NodeId id, Error& error) {
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
    return true;
}

const char* type_name(NodeType type) {
    switch (type) {
    case NodeType::file: return "file";
    case NodeType::directory: return "directory";
    case NodeType::vault: return "vault";
    case NodeType::device: return "device";
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
    nodes = g_node_count;
    tags = g_tag_count;
    stream_bytes = g_stream_bytes;
    stream_limit = MAX_STREAM_BYTES;
}

bool selftest(u32& checks, Error& error) {
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

}