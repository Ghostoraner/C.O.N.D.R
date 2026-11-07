#include <ipc/ipc.hpp>
#include <fs/nvmfs.hpp>
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

static bool same(const char* left, const char* right) {
    return left && right && strcmp(left, right) == 0;
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

[[maybe_unused]] static Service* service_by_node(nvmfs::NodeId node_id) {
    for (usize i = 0; i < MAX_SERVICES; ++i)
        if (g_services[i].active && g_services[i].node_id == node_id) return &g_services[i];
    return nullptr;
}

void init() {
    memset(g_endpoints, 0, sizeof(g_endpoints));
    memset(g_services, 0, sizeof(g_services));
    memset(g_capabilities, 0, sizeof(g_capabilities));
    g_next_handle = 0;
}

bool register_service(u32 owner, const char* name, nvmfs::NodeId node_id, Handle& handle, Error& error) {
    if (!name || !*name || strlen(name) >= SERVICE_NAME_SIZE) {
        error = Error::invalid_argument;
        return false;
    }
    for (usize i = 0; i < MAX_SERVICES; ++i) {
        if (g_services[i].active && same(g_services[i].name, name)) {
            error = Error::already_exists;
            return false;
        }
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

bool connect(u32 client, const char* name, Handle& handle, Error& error) {
    if (!name || !*name || strlen(name) >= SERVICE_NAME_SIZE) {
        error = Error::invalid_argument;
        return false;
    }
    usize service_slot = MAX_SERVICES;
    for (usize i = 0; i < MAX_SERVICES; ++i) {
        if (g_services[i].active && same(g_services[i].name, name)) { service_slot = i; break; }
    }
    if (service_slot == MAX_SERVICES) { error = Error::not_found; return false; }
    Service& service = g_services[service_slot];

    bool has_right = false;
    nvmfs::check_rights(service.node_id,
        static_cast<u8>(nvmfs::Right::connect), has_right);
    if (!has_right) { error = Error::permission_denied; return false; }

    u16 endpoint_index = 0;
    Endpoint* endpoint = allocate_endpoint(client, false, endpoint_index);
    if (!endpoint) { error = Error::table_full; return false; }
    endpoint->peer = service.endpoint;
    g_endpoints[service.endpoint].peer = endpoint_index;

    Handle new_handle;
    if (!create_capability(client, endpoint_index, RIGHT_SEND | RIGHT_RECEIVE, new_handle)) {
        memset(endpoint, 0, sizeof(Endpoint));
        g_endpoints[service.endpoint].peer = 0xFFFF;
        error = Error::table_full;
        return false;
    }
    handle = new_handle;
    error = Error::none;
    return true;
}

bool accept(u32 owner, Handle listener, Handle& connection, Error& error) {
    Capability* capability = find_capability(owner, listener);
    if (!capability || !(capability->rights & RIGHT_ACCEPT)) {
        error = Error::permission_denied;
        return false;
    }
    Service* service = nullptr;
    for (usize i = 0; i < MAX_SERVICES; ++i) {
        if (g_services[i].active && g_services[i].endpoint == capability->endpoint) {
            service = &g_services[i];
            break;
            service = &g_services[i];
            break;
        }
    }
    if (!service) { error = Error::not_found; return false; }

    bool has_right = false;
    nvmfs::check_rights(service->node_id,
        static_cast<u8>(nvmfs::Right::accept), has_right);
    if (!has_right) { error = Error::permission_denied; return false; }

    Handle new_handle;
    if (!create_capability(owner, capability->endpoint, RIGHT_SEND | RIGHT_RECEIVE, new_handle)) {
        error = Error::table_full;
        return false;
    }
    connection = new_handle;
    error = Error::none;
    return true;
}

bool send(u32 sender, Handle connection, const void* bytes, usize size, Error& error) {
    Capability* capability = find_capability(sender, connection);
    if (!capability || !(capability->rights & RIGHT_SEND)) {
        error = Error::permission_denied;
        return false;
    }
    Endpoint* endpoint = &g_endpoints[capability->endpoint];
    if (!endpoint->active || endpoint->peer == 0xFFFF) {
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

bool receive(u32 receiver, Handle connection, Message& message, Error& error) {
    Capability* capability = find_capability(receiver, connection);
    if (!capability || !(capability->rights & RIGHT_RECEIVE)) {
        error = Error::permission_denied;
        return false;
    }
    Endpoint* endpoint = &g_endpoints[capability->endpoint];
    if (!endpoint->active) { error = Error::not_found; return false; }
    if (endpoint->queue_count == 0) { error = Error::would_block; return false; }

    usize slot = endpoint->queue_head;
    message = endpoint->queue[slot];
    endpoint->queue_head = static_cast<u8>((endpoint->queue_head + 1) % QUEUE_DEPTH);
    --endpoint->queue_count;

    error = Error::none;
    return true;
}

static void cleanup_test(u32 service_owner, u32 client, Handle listener,
                         Handle client_handle, Handle server_handle, usize service_slot) {
    (void)listener;
    close_task(service_owner);
    close_task(client);
}

void close_task(u32 task_id) {
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
        if (endpoint.peer < MAX_ENDPOINTS && g_endpoints[endpoint.peer].active)
            g_endpoints[endpoint.peer].peer = 0xFFFF;
        memset(&endpoint, 0, sizeof(endpoint));
    }
}

nvmfs::NodeId service_node_id(Handle handle) {
    for (usize i = 0; i < MAX_CAPABILITIES; ++i) {
        Capability& capability = g_capabilities[i];
        if (capability.active && capability.handle == handle) {
            for (usize j = 0; j < MAX_SERVICES; ++j) {
                if (g_services[j].active && g_services[j].endpoint == capability.endpoint)
                    return g_services[j].node_id;
            }
        }
    }
    return 0;
}

bool selftest(u32& checks, Error& error) {
    static constexpr u32 SERVICE_OWNER = 0xFFFFFFF0;
    static constexpr u32 CLIENT = 0xFFFFFFF1;
    static constexpr char REQUEST[] = "ping";
    static constexpr char RESPONSE[] = "pong";
    checks = 0;

    Handle listener = 0;
    Handle client_handle = 0;
    Handle server_handle = 0;
    Error failure;
    nvmfs::NodeId test_node;
    nvmfs::Error fs_error;
    if (!nvmfs::create(nvmfs::NodeType::service, "ipc-check-node", 0, test_node, fs_error)) {
        error = Error::not_found;
        return false;
    }
    if (!register_service(SERVICE_OWNER, "ipc-check", test_node, listener, failure)) {
        error = failure;
        return false;
    }
    usize service_slot = 0;
    while (service_slot < MAX_SERVICES && !same(g_services[service_slot].name, "ipc-check"))
        ++service_slot;
    if (service_slot == MAX_SERVICES) { error = Error::not_found; return false; }
    ++checks;

    bool success = connect(CLIENT, "ipc-check", client_handle, failure);
    if (success) ++checks;
    if (success) success = accept(SERVICE_OWNER, listener, server_handle, failure);
    if (success) ++checks;

    if (success) success = send(CLIENT, client_handle, REQUEST, sizeof(REQUEST) - 1, failure);
    if (success) ++checks;
    Message message{};
    if (success) success = receive(SERVICE_OWNER, server_handle, message, failure)
        && message.sender == CLIENT && message.size == sizeof(REQUEST) - 1
        && memcmp(message.bytes, REQUEST, sizeof(REQUEST) - 1) == 0;
    if (success) ++checks;

    if (success) success = send(SERVICE_OWNER, server_handle, RESPONSE, sizeof(RESPONSE) - 1, failure);
    if (success) ++checks;
    if (success) success = receive(CLIENT, client_handle, message, failure)
        && message.sender == SERVICE_OWNER && message.size == sizeof(RESPONSE) - 1
        && memcmp(message.bytes, RESPONSE, sizeof(RESPONSE) - 1) == 0;
    if (success) ++checks;

    cleanup_test(SERVICE_OWNER, CLIENT, listener, client_handle, server_handle, service_slot);
    error = success ? Error::none : failure;
    return success;
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
    }
    return "unknown IPC error";
}

}
