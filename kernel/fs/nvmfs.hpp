#pragma once
#include <nord/types.hpp>

namespace nord::nvmfs {

using NodeId = u64;

enum class NodeType : u8 {
    file,
    directory,
    vault,
    device,
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
    char name[48];
    FileStream stream;
};

struct TagIndex {
    NodeId node_id;
    bool active;
    char key[24];
    char value[48];
};

struct TagView {
    NodeId node_id;
    const char* key;
    const char* value;
};

struct Directory {
    NodeId container_id;
};

void init();
bool create(NodeType type, const char* name, NodeId container_id,
            NodeId& result, Error& error);
Node* find(NodeId id);
Node* at(usize active_position);
Node* directory_at(const Directory& directory, usize& cursor);
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

bool selftest(u32& checks, Error& error);

}