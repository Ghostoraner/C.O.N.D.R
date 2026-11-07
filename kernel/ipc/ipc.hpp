#pragma once
#include <fs/nvmfs.hpp>
#include <nord/types.hpp>

namespace nord::ipc {

using Handle = u32;

enum class Error : u8 {
    none,
    invalid_argument,
    not_found,
    already_exists,
    table_full,
    would_block,
    queue_full,
    permission_denied,
    message_too_large,
};

struct Message {
    u32 sender;
    usize size;
    u8 bytes[64];
};

void init();
bool register_service(u32 owner, const char* name, nvmfs::NodeId node_id, Handle& handle, Error& error);
bool connect(u32 client, const char* name, Handle& handle, Error& error);
bool accept(u32 owner, Handle listener, Handle& connection, Error& error);
bool send(u32 sender, Handle connection, const void* bytes, usize size, Error& error);
bool receive(u32 receiver, Handle connection, Message& message, Error& error);
void close_task(u32 task_id);
bool selftest(u32& checks, Error& error);
const char* error_text(Error error);

nvmfs::NodeId service_node_id(Handle handle);

}