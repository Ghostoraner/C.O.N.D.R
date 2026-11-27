#pragma once
#include <nord/types.hpp>

namespace nord::nvmfs {

using NodeId = u32;

enum class NodeType : u8 {
    file,
    directory,
    vault,
    device,
    service,
    process,
};

enum class Error : u8 {
    none,
    not_initialized,
    invalid_argument,
    not_found,
    already_exists,
    no_memory,
    table_full,
    stream_too_large,
    container_not_empty,
};

struct FileStream {
    u8* bytes;
    usize size;
    usize capacity;
};

struct Node {
    NodeId id;
    NodeId container_id;
    NodeType type;
    bool active;
    u32 owner;
    u8 rights;
    char name[48];
    FileStream stream;
};

// Detached copy of a node. Returned by the lookup helpers so callers never
// hold a pointer into the node table after the internal lock is released.
// `stream` remains owned by NVMFS and is only valid while a Lock is held.
struct NodeInfo {
    NodeId id;
    NodeId container_id;
    NodeType type;
    u32 owner;
    u8 rights;
    char name[48];
    const u8* stream;
    usize stream_size;
};

struct TagIndex {
    NodeId node_id;
    bool active;
    char key[24];
    char value[48];
};

// Values are copied out of the tag table, which may be reallocated by add_tag.
struct TagView {
    NodeId node_id;
    char key[24];
    char value[48];
};

struct Directory {
    NodeId container_id;
};

enum class Right : u8 {
    none     = 0,
    read     = 1 << 0,
    write    = 1 << 1,
    connect  = 1 << 2,
    accept   = 1 << 3,
    owner    = 1 << 4,
    all      = 0x1F,
};

using ChangeHook = void (*)();

// RAII guard for callers that need a stable view of the node table across
// several operations. Individual NVMFS calls are already internally
// serialized; this only extends that serialization over a sequence of calls.
class Lock {
public:
    Lock();
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

void init();
void set_change_hook(ChangeHook hook);
bool create(NodeType type, const char* name, NodeId container_id,
            NodeId& result, Error& error);
bool find(NodeId id, NodeInfo& info);
// Finds the first active node of `type` whose name matches `name`. A leading
// '/' is ignored because NVMFS is a flat namespace. As with find(), the
// returned stream pointer is only valid while a Lock is held.
bool find_name(const char* name, NodeType type, NodeInfo& info);
bool at(usize active_position, NodeInfo& info);
bool directory_at(const Directory& directory, usize& cursor, NodeInfo& info);
usize count();

bool add_tag(NodeId id, const char* key, const char* value, Error& error);
bool remove_tag(NodeId id, const char* key, const char* value, Error& error);
bool find_tag(const char* key, const char* value, usize& cursor, NodeId& result);
bool tag_at(usize& cursor, TagView& result);

bool write(NodeId id, const void* bytes, usize size, bool append, Error& error);
bool erase(NodeId id, Error& error);
const char* type_name(NodeType type);
bool parse_type(const char* text, NodeType& type);
const char* error_text(Error error);
void stats(u64& nodes, u64& tags, u64& stream_bytes, u64& stream_limit);

bool set_owner(NodeId id, u32 owner, Error& error);
bool set_rights(NodeId id, u8 rights, Error& error);
bool check_rights(NodeId id, u8 required, bool& has);
u8 rights_of(NodeId id);

bool selftest(u32& checks, Error& error);
bool serialize(void* output, usize capacity, usize& size);
bool deserialize(const void* input, usize size);

}