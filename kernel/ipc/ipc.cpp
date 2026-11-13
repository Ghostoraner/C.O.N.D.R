#include <ipc/ipc.hpp>
#include <fs/nvmfs.hpp>
#include <lib/spinlock.hpp>
#include <lib/string.hpp>

namespace nord::ipc {

static constexpr usize MAX_SERVICES = 8;
static constexpr usize MAX_ENDPOINTS = 32;
static constexpr usize MAX_CAPABILITIES = 64;
static constexpr usize MAX_HANDLES_PER_TASK = 8;
static constexpr usize QUEUE_DEPTH = 8;
static constexpr usize SERVICE_NAME_SIZE = 24;
static constexpr u8 RIGHT_ACCEPT = 1;
static constexpr u8 RIGHT_SEND = 2;
static constexpr u8 RIGHT_RECEIVE = 4;

struct Endpoint {
    bool active;
    bool listener;
    u32 owner;
    u16 peer;
    nvmfs::NodeId node_id;
    u16 pending[QUEUE_DEPTH];
    u8 pending_head;
    u8 pending_tail;
    u8 pending_count;
    Message queue[QUEUE_DEPTH];
    u8 queue_head;
    u8 queue_tail;
    u8 queue_count;
};

struct Service {
    bool active;
    u32 owner;
    u16 endpoint;
    nvmfs::NodeId node_id;
    char name[SERVICE_NAME_SIZE];
};

struct Capability {
    bool active;
    u32 task_id;
    Handle handle;
    u16 endpoint;
    u8 rights;
};

static Endpoint g_endpoints[MAX_ENDPOINTS];
static Service g_services[MAX_SERVICES];
static Capability g_capabilities[MAX_CAPABILITIES];
static Handle g_next_handle;
static sync::SpinLock g_lock;

static bool same(const char* left, const char* right) {
    return left && right && strcmp(left, right) == 0;
}

static bool valid_endpoint(u16 index) {
    return index < MAX_ENDPOINTS && g_endpoints[index].active;
}

static Endpoint* allocate_endpoint(u32 owner, bool listener, u16& index) {
    for (usize i = 0; i < MAX_ENDPOINTS; ++i) {
        if (g_endpoints[i].active) continue;
        Endpoint& endpoint = g_endpoints[i];
        memset(&endpoint, 0, sizeof(endpoint));
        endpoint.active = true;
        endpoint.listener = listener;
        endpoint.owner = owner;
        endpoint.peer = 0xFFFF;
        index = static_cast<u16>(i);
        return &endpoint;
    }
    return nullptr;
}

static bool pending_push(u16 listener_index, u16 connection_index) {
    Endpoint& listener = g_endpoints[listener_index];
    if (listener.pending_count >= QUEUE_DEPTH) return false;
    listener.pending[listener.pending_tail] = connection_index;
    listener.pending_tail = static_cast<u8>((listener.pending_tail + 1) % QUEUE_DEPTH);
    ++listener.pending_count;
    return true;
}

static bool pending_push_front(u16 listener_index, u16 connection_index) {
    Endpoint& listener = g_endpoints[listener_index];
    if (listener.pending_count >= QUEUE_DEPTH) return false;
    listener.pending_head = static_cast<u8>((listener.pending_head + QUEUE_DEPTH - 1) % QUEUE_DEPTH);
    listener.pending[listener.pending_head] = connection_index;
    ++listener.pending_count;
    return true;
}

static bool pending_pop(u16 listener_index, u16& connection_index) {
    Endpoint& listener = g_endpoints[listener_index];
    if (listener.pending_count == 0) return false;
    connection_index = listener.pending[listener.pending_head];
    listener.pending_head = static_cast<u8>((listener.pending_head + 1) % QUEUE_DEPTH);
    --listener.pending_count;
    return true;
}

static void pending_remove(u16 listener_index, u16 connection_index) {
    Endpoint& listener = g_endpoints[listener_index];
    u16 kept[QUEUE_DEPTH];
    u8 kept_count = 0;
    for (u8 i = 0; i < listener.pending_count; ++i) {
        const u16 candidate = listener.pending[(listener.pending_head + i) % QUEUE_DEPTH];
        if (candidate != connection_index) kept[kept_count++] = candidate;
    }
    listener.pending_head = 0;
    listener.pending_tail = static_cast<u8>(kept_count % QUEUE_DEPTH);
    listener.pending_count = kept_count;
    for (u8 i = 0; i < kept_count; ++i) listener.pending[i] = kept[i];
}

static void pending_discard(u16 listener_index) {
    u16 connection_index = 0;
    while (pending_pop(listener_index, connection_index)) {
        if (valid_endpoint(connection_index))
            g_endpoints[connection_index].peer = 0xFFFF;
    }
}

static Service* service_by_name(const char* name) {
    for (usize i = 0; i < MAX_SERVICES; ++i)
        if (g_services[i].active && same(g_services[i].name, name)) return &g_services[i];
    return nullptr;
}

static Service* service_by_endpoint(u16 endpoint) {
    for (usize i = 0; i < MAX_SERVICES; ++i)
        if (g_services[i].active && g_services[i].endpoint == endpoint) return &g_services[i];
    return nullptr;
}

static Capability* find_capability(u32 task_id, Handle handle) {
    for (usize i = 0; i < MAX_CAPABILITIES; ++i) {
        Capability& capability = g_capabilities[i];
        if (capability.active && capability.task_id == task_id && capability.handle == handle)
            return &capability;
    }
    return nullptr;
}

static bool create_capability(u32 task_id, u16 endpoint, u8 rights, Handle& handle) {
    usize task_handles = 0;
    usize free_slot = MAX_CAPABILITIES;
    for (usize i = 0; i < MAX_CAPABILITIES; ++i) {
        if (!g_capabilities[i].active) {
            if (free_slot == MAX_CAPABILITIES) free_slot = i;
            continue;
        }
        if (g_capabilities[i].task_id == task_id) ++task_handles;
    }
    if (task_handles >= MAX_HANDLES_PER_TASK || free_slot == MAX_CAPABILITIES) return false;
    Handle next = ++g_next_handle;
    if (!next) next = ++g_next_handle;
    g_capabilities[free_slot] = {true, task_id, next, endpoint, rights};
    handle = next;
    return true;
}

static bool register_service_locked(u32 owner, const char* name, nvmfs::NodeId node_id,
                                    Handle& handle, Error& error) {
    if (!name || !*name || strlen(name) >= SERVICE_NAME_SIZE) {
        error = Error::invalid_argument;
        return false;
    }
    if (service_by_name(name)) {
        error = Error::already_exists;
        return false;
    }
    usize slot = MAX_SERVICES;
    for (usize i = 0; i < MAX_SERVICES; ++i) {
        if (!g_services[i].active) { slot = i; break; }
    }
    if (slot == MAX_SERVICES) { error = Error::table_full; return false; }

    u16 endpoint_index = 0;
    Endpoint* endpoint = allocate_endpoint(owner, true, endpoint_index);
    if (!endpoint) { error = Error::table_full; return false; }

    Service& service = g_services[slot];
    memset(&service, 0, sizeof(service));
    service.active = true;
    service.owner = owner;
    service.endpoint = endpoint_index;
    service.node_id = node_id;
    size_t name_len = strlen(name);
    if (name_len >= SERVICE_NAME_SIZE) name_len = SERVICE_NAME_SIZE - 1;
    memcpy(service.name, name, name_len);
    service.name[name_len] = '\0';
    endpoint->node_id = node_id;

    nvmfs::Error fs_error;
    nvmfs::set_owner(node_id, owner, fs_error);
    nvmfs::set_rights(node_id,
        static_cast<u8>(nvmfs::Right::read) | static_cast<u8>(nvmfs::Right::write) |
        static_cast<u8>(nvmfs::Right::connect) | static_cast<u8>(nvmfs::Right::accept) |
        static_cast<u8>(nvmfs::Right::owner), fs_error);

    Handle new_handle;
    if (!create_capability(owner, endpoint_index, 0xFF, new_handle)) {
        memset(&service, 0, sizeof(service));
        memset(endpoint, 0, sizeof(Endpoint));
        error = Error::table_full;
        return false;
    }
    handle = new_handle;
    error = Error::none;
    return true;
}

static bool connect_locked(u32 client, const char* name, Handle& handle, Error& error) {
    if (!name || !*name || strlen(name) >= SERVICE_NAME_SIZE) {
        error = Error::invalid_argument;
        return false;
    }
    Service* service = service_by_name(name);
    if (!service) { error = Error::not_found; return false; }
    if (!valid_endpoint(service->endpoint) || !g_endpoints[service->endpoint].listener) {
        error = Error::not_found;
        return false;
    }

    bool has_right = false;
    nvmfs::check_rights(service->node_id,
        static_cast<u8>(nvmfs::Right::connect), has_right);
    if (!has_right) { error = Error::permission_denied; return false; }

    u16 endpoint_index = 0;
    Endpoint* endpoint = allocate_endpoint(client, false, endpoint_index);
    if (!endpoint) { error = Error::table_full; return false; }
    if (!pending_push(service->endpoint, endpoint_index)) {
        memset(endpoint, 0, sizeof(Endpoint));
        error = Error::queue_full;
        return false;
    }

    Handle new_handle;
    if (!create_capability(client, endpoint_index, RIGHT_SEND | RIGHT_RECEIVE, new_handle)) {
        pending_remove(service->endpoint, endpoint_index);
        memset(endpoint, 0, sizeof(Endpoint));
        error = Error::table_full;
        return false;
    }
    handle = new_handle;
    error = Error::none;
    return true;
}

static bool accept_locked(u32 owner, Handle listener, Handle& connection, Error& error) {
    Capability* capability = find_capability(owner, listener);
    if (!capability || !(capability->rights & RIGHT_ACCEPT)) {
        error = Error::permission_denied;
        return false;
    }
    Service* service = service_by_endpoint(capability->endpoint);
    if (!service) { error = Error::not_found; return false; }

    bool has_right = false;
    nvmfs::check_rights(service->node_id,
        static_cast<u8>(nvmfs::Right::accept), has_right);
    if (!has_right) { error = Error::permission_denied; return false; }

    u16 client_index = 0;
    if (!pending_pop(service->endpoint, client_index)) {
        error = Error::would_block;
        return false;
    }
    if (!valid_endpoint(client_index)) { error = Error::not_found; return false; }

    u16 server_index = 0;
    Endpoint* server = allocate_endpoint(owner, false, server_index);
    if (!server) {
        pending_push_front(service->endpoint, client_index);
        error = Error::table_full;
        return false;
    }
    server->peer = client_index;
    server->node_id = service->node_id;
    g_endpoints[client_index].peer = server_index;

    Handle new_handle;
    if (!create_capability(owner, server_index, RIGHT_SEND | RIGHT_RECEIVE, new_handle)) {
        g_endpoints[client_index].peer = 0xFFFF;
        memset(server, 0, sizeof(Endpoint));
        pending_push_front(service->endpoint, client_index);
        error = Error::table_full;
        return false;
    }
    connection = new_handle;
    error = Error::none;
    return true;
}

static bool send_locked(u32 sender, Handle connection, const void* bytes, usize size,
                        Error& error) {
    Capability* capability = find_capability(sender, connection);
    if (!capability || !(capability->rights & RIGHT_SEND)) {
        error = Error::permission_denied;
        return false;
    }
    if (!valid_endpoint(capability->endpoint)) { error = Error::not_found; return false; }
    Endpoint* endpoint = &g_endpoints[capability->endpoint];
    if (endpoint->peer == 0xFFFF || !valid_endpoint(endpoint->peer)) {
        endpoint->peer = 0xFFFF;
        error = Error::not_found;
        return false;
    }
    if (size > sizeof(Message::bytes)) { error = Error::message_too_large; return false; }

    Endpoint* peer = &g_endpoints[endpoint->peer];
    if (peer->queue_count >= QUEUE_DEPTH) { error = Error::queue_full; return false; }

    usize slot = peer->queue_tail;
    peer->queue[slot].sender = sender;
    peer->queue[slot].size = size;
    memcpy(peer->queue[slot].bytes, bytes, size);
    peer->queue_tail = static_cast<u8>((peer->queue_tail + 1) % QUEUE_DEPTH);
    ++peer->queue_count;

    error = Error::none;
    return true;
}

static bool receive_locked(u32 receiver, Handle connection, Message& message, Error& error) {
    Capability* capability = find_capability(receiver, connection);
    if (!capability || !(capability->rights & RIGHT_RECEIVE)) {
        error = Error::permission_denied;
        return false;
    }
    if (!valid_endpoint(capability->endpoint)) { error = Error::not_found; return false; }
    Endpoint* endpoint = &g_endpoints[capability->endpoint];
    if (endpoint->queue_count == 0) { error = Error::would_block; return false; }

    usize slot = endpoint->queue_head;
    message = endpoint->queue[slot];
    endpoint->queue_head = static_cast<u8>((endpoint->queue_head + 1) % QUEUE_DEPTH);
    --endpoint->queue_count;

    error = Error::none;
    return true;
}

static void close_task_locked(u32 task_id) {
    for (usize i = 0; i < MAX_ENDPOINTS; ++i) {
        Endpoint& endpoint = g_endpoints[i];
        if (endpoint.active && endpoint.listener && endpoint.owner == task_id)
            pending_discard(static_cast<u16>(i));
    }
    for (usize i = 0; i < MAX_ENDPOINTS; ++i) {
        const Endpoint& endpoint = g_endpoints[i];
        if (!endpoint.active || endpoint.owner != task_id) continue;
        for (usize listener = 0; listener < MAX_ENDPOINTS; ++listener)
            if (g_endpoints[listener].active && g_endpoints[listener].listener)
                pending_remove(static_cast<u16>(listener), static_cast<u16>(i));
    }
    for (usize i = 0; i < MAX_SERVICES; ++i) {
        Service& service = g_services[i];
        if (!service.active || service.owner != task_id) continue;
        nvmfs::Error fs_error;
        nvmfs::erase(service.node_id, fs_error);
        memset(&service, 0, sizeof(service));
    }
    for (usize i = 0; i < MAX_CAPABILITIES; ++i) {
        Capability& capability = g_capabilities[i];
        if (capability.active && capability.task_id == task_id)
            memset(&capability, 0, sizeof(capability));
    }
    for (usize i = 0; i < MAX_ENDPOINTS; ++i) {
        Endpoint& endpoint = g_endpoints[i];
        if (!endpoint.active || endpoint.owner != task_id) continue;
        if (valid_endpoint(endpoint.peer) && g_endpoints[endpoint.peer].peer == i)
            g_endpoints[endpoint.peer].peer = 0xFFFF;
        memset(&endpoint, 0, sizeof(endpoint));
    }
}

static bool selftest_locked(u32& checks, Error& error) {
    static constexpr u32 SERVICE_OWNER = 0xFFFFFFF0;
    static constexpr u32 CLIENT_A = 0xFFFFFFF1;
    static constexpr u32 CLIENT_B = 0xFFFFFFF2;
    static constexpr char REQUEST_A[] = "ping-a";
    static constexpr char REQUEST_B[] = "ping-b";
    static constexpr char RESPONSE_A[] = "pong-a";
    static constexpr char RESPONSE_B[] = "pong-b";
    checks = 0;

    Handle listener = 0;
    Handle client_a = 0;
    Handle client_b = 0;
    Handle server_a = 0;
    Handle server_b = 0;
    Error failure;
    nvmfs::NodeId test_node;
    nvmfs::Error fs_error;
    if (!nvmfs::create(nvmfs::NodeType::service, "ipc-check-node", 0, test_node, fs_error)) {
        error = Error::not_found;
        return false;
    }
    if (!register_service_locked(SERVICE_OWNER, "ipc-check", test_node, listener, failure)) {
        nvmfs::erase(test_node, fs_error);
        error = failure;
        return false;
    }
    ++checks;

    bool success = connect_locked(CLIENT_A, "ipc-check", client_a, failure);
    if (success) ++checks;
    if (success) success = connect_locked(CLIENT_B, "ipc-check", client_b, failure);
    if (success) ++checks;
    if (success) success = accept_locked(SERVICE_OWNER, listener, server_a, failure);
    if (success) ++checks;
    if (success) success = accept_locked(SERVICE_OWNER, listener, server_b, failure);
    if (success) ++checks;

    Message message{};
    if (success) success = send_locked(CLIENT_A, client_a, REQUEST_A,
                                       sizeof(REQUEST_A) - 1, failure);
    if (success) ++checks;
    if (success) success = receive_locked(SERVICE_OWNER, server_a, message, failure)
        && message.sender == CLIENT_A && message.size == sizeof(REQUEST_A) - 1
        && memcmp(message.bytes, REQUEST_A, sizeof(REQUEST_A) - 1) == 0;
    if (success) ++checks;

    if (success) success = send_locked(CLIENT_B, client_b, REQUEST_B,
                                       sizeof(REQUEST_B) - 1, failure);
    if (success) ++checks;
    if (success) success = receive_locked(SERVICE_OWNER, server_b, message, failure)
        && message.sender == CLIENT_B && message.size == sizeof(REQUEST_B) - 1
        && memcmp(message.bytes, REQUEST_B, sizeof(REQUEST_B) - 1) == 0;
    if (success) ++checks;

    if (success) success = send_locked(SERVICE_OWNER, server_a, RESPONSE_A,
                                       sizeof(RESPONSE_A) - 1, failure);
    if (success) ++checks;
    if (success) success = send_locked(SERVICE_OWNER, server_b, RESPONSE_B,
                                       sizeof(RESPONSE_B) - 1, failure);
    if (success) ++checks;

    if (success) success = receive_locked(CLIENT_A, client_a, message, failure)
        && message.sender == SERVICE_OWNER && message.size == sizeof(RESPONSE_A) - 1
        && memcmp(message.bytes, RESPONSE_A, sizeof(RESPONSE_A) - 1) == 0;
    if (success) ++checks;
    if (success) success = receive_locked(CLIENT_B, client_b, message, failure)
        && message.sender == SERVICE_OWNER && message.size == sizeof(RESPONSE_B) - 1
        && memcmp(message.bytes, RESPONSE_B, sizeof(RESPONSE_B) - 1) == 0;
    if (success) ++checks;

    close_task_locked(SERVICE_OWNER);
    close_task_locked(CLIENT_A);
    close_task_locked(CLIENT_B);
    error = success ? Error::none : failure;
    return success;
}

void init() {
    sync::LockGuard guard(g_lock);
    memset(g_endpoints, 0, sizeof(g_endpoints));
    memset(g_services, 0, sizeof(g_services));
    memset(g_capabilities, 0, sizeof(g_capabilities));
    g_next_handle = 0;
}

bool register_service(u32 owner, const char* name, nvmfs::NodeId node_id, Handle& handle,
                      Error& error) {
    sync::LockGuard guard(g_lock);
    return register_service_locked(owner, name, node_id, handle, error);
}

bool connect(u32 client, const char* name, Handle& handle, Error& error) {
    sync::LockGuard guard(g_lock);
    return connect_locked(client, name, handle, error);
}

bool accept(u32 owner, Handle listener, Handle& connection, Error& error) {
    sync::LockGuard guard(g_lock);
    return accept_locked(owner, listener, connection, error);
}

bool send(u32 sender, Handle connection, const void* bytes, usize size, Error& error) {
    sync::LockGuard guard(g_lock);
    return send_locked(sender, connection, bytes, size, error);
}

bool receive(u32 receiver, Handle connection, Message& message, Error& error) {
    sync::LockGuard guard(g_lock);
    return receive_locked(receiver, connection, message, error);
}

void close_task(u32 task_id) {
    sync::LockGuard guard(g_lock);
    close_task_locked(task_id);
}

nvmfs::NodeId service_node_id(Handle handle) {
    sync::LockGuard guard(g_lock);
    for (usize i = 0; i < MAX_CAPABILITIES; ++i) {
        Capability& capability = g_capabilities[i];
        if (capability.active && capability.handle == handle) {
            if (valid_endpoint(capability.endpoint))
                return g_endpoints[capability.endpoint].node_id;
            return 0;
        }
    }
    return 0;
}

bool selftest(u32& checks, Error& error) {
    sync::LockGuard guard(g_lock);
    return selftest_locked(checks, error);
}

const char* error_text(Error error) {
    switch (error) {
    case Error::none: return "no error";
    case Error::invalid_argument: return "invalid argument";
    case Error::not_found: return "service or endpoint not found";
    case Error::table_full: return "IPC table full";
    case Error::would_block: return "no message available";
    case Error::queue_full: return "IPC queue full";
    case Error::permission_denied: return "capability denied";
    case Error::message_too_large: return "message exceeds 64 bytes";
    case Error::already_exists: return "already exists";
    }
    return "unknown IPC error";
}

}
