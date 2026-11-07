#include <shell/custom_shell.hpp>
#include <arch/x86_64/cpu/scheduler.hpp>
#include <arch/x86_64/dev/keyboard.hpp>
#include <arch/x86_64/dev/timer.hpp>
#include <arch/x86_64/io.hpp>
#include <drivers/video/framebuffer.hpp>
#include <fs/nvmfs.hpp>
#include <ipc/ipc.hpp>
#include <lib/print.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <storage/persistence.hpp>
#include <test/test.hpp>
#include <nord/types.hpp>

namespace nord::shell {

static constexpr usize LINE_CAPACITY = 256;
static constexpr usize TOKEN_CAPACITY = 8;
static nvmfs::NodeId g_context;

struct TokenList { char* value[TOKEN_CAPACITY]; usize count; };

static bool equal(const char* left, const char* right) {
    return left && right && strcmp(left, right) == 0;
}

static bool tokenize(char* line, TokenList& tokens) {
    tokens.count = 0;
    char* read = line;
    char* write = line;
    while (*read) {
        while (*read == ' ' || *read == '\t') ++read;
        if (!*read) break;
        if (tokens.count == TOKEN_CAPACITY) return false;
        tokens.value[tokens.count++] = write;
        char quote = 0;
        if (*read == '"' || *read == '\'') quote = *read++;
        while (*read) {
            if (quote) {
                if (*read == quote) { ++read; quote = 0; continue; }
                if (*read == '\\' && read[1]) ++read;
                *write++ = *read++;
                continue;
            }
            if (*read == ' ' || *read == '\t') break;
            if (*read == '"' || *read == '\'') { quote = *read++; continue; }
            if (*read == '\\' && read[1]) ++read;
            *write++ = *read++;
        }
        if (quote) return false;
        if (*read == ' ' || *read == '\t') ++read;
        *write++ = '\0';
    }
    return true;
}

static bool parse_id(const char* text, nvmfs::NodeId& id) {
    if (!text || !*text) return false;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    if (!*text) return false;
    u64 value = 0;
    for (; *text; ++text) {
        u8 digit;
        if (*text >= '0' && *text <= '9') digit = static_cast<u8>(*text - '0');
        else if (*text >= 'a' && *text <= 'f') digit = static_cast<u8>(*text - 'a' + 10);
        else if (*text >= 'A' && *text <= 'F') digit = static_cast<u8>(*text - 'A' + 10);
        else return false;
        if (value > (~static_cast<u64>(0) - digit) / 16) return false;
        value = value * 16 + digit;
    }
    id = value;
    return true;
}

static void error(const char* command, nvmfs::Error code) {
    kprintf("%s: %s\n", command, nvmfs::error_text(code));
}

static void print_node(const nvmfs::Node& node) {
    kprintf("%#lx  %s  %s", node.id, nvmfs::type_name(node.type), node.name);
    if (node.stream.size) kprintf("  [%lu B]", node.stream.size);
    kprintf("\n");
}

static bool parse_tag_query(char* text, char*& key, char*& value) {
    if (!text) return false;
    char* split = text;
    while (*split && *split != '=') ++split;
    if (!*split || split == text || !split[1]) return false;
    *split = '\0';
    key = text;
    value = split + 1;
    return true;
}

static void print_help() {
    kprintf("C.O.N.D.R commands:\n");
    kprintf("  vault [NAME]       list/create vault\n");
    kprintf("  ls [ID]            list objects in current/container vault\n");
    kprintf("  cd ID|..           enter container / go to parent container\n");
    kprintf("  pwd                print current object context\n");
    kprintf("  mkdir NAME         create directory object\n");
    kprintf("  touch NAME         create file object\n");
    kprintf("  new TYPE NAME      create any node type\n");
    kprintf("  find KEY=VALUE     search the tag index\n");
    kprintf("  stat ID | rm ID    inspect/delete object\n");
    kprintf("  tag ID KEY VALUE   add tag; untag removes it\n");
    kprintf("  put ID \"TEXT\" | add ID \"TEXT\" | get ID\n");
    kprintf("  sys | check | ver | reboot | help\n");
    kprintf("  ps                 list kernel tasks\n");
}

static void command_ps() {
    for (usize i = 0; i < arch::scheduler::count(); ++i) {
        arch::scheduler::TaskInfo task{};
        if (!arch::scheduler::at(i, task)) continue;
        const char* state = task.state == arch::scheduler::State::running ? "running"
            : task.state == arch::scheduler::State::ready ? "ready" : "exited";
        kprintf("%u  %s  %s  switches=%lu\n", task.id, state, task.name, task.switches);
    }
}

static void list_container(nvmfs::NodeId container) {
    nvmfs::Directory directory{container};
    usize cursor = 0;
    nvmfs::Node* node = nullptr;
    bool any = false;
    while ((node = nvmfs::directory_at(directory, cursor))) {
        print_node(*node);
        any = true;
    }
    if (!any) kprintf("(empty space)\n");
}

static void command_vault(const TokenList& tokens) {
    if (tokens.count == 1) {
        for (usize i = 0; i < nvmfs::count(); ++i) {
            nvmfs::Node* node = nvmfs::at(i);
            if (node && node->type == nvmfs::NodeType::vault) print_node(*node);
        }
        return;
    }
    if (tokens.count != 2) { kprintf("usage: vault [NAME]\n"); return; }
    nvmfs::NodeId id;
    nvmfs::Error failure;
    if (!nvmfs::create(nvmfs::NodeType::vault, tokens.value[1], g_context, id, failure))
        error("vault", failure);
    else kprintf("vault created: %#lx\n", id);
}

static void command_cd(const TokenList& tokens) {
    if (tokens.count != 2) { kprintf("usage: cd ID|..\n"); return; }
    if (equal(tokens.value[1], "..")) {
        nvmfs::Node* current = nvmfs::find(g_context);
        g_context = current ? current->container_id : 0;
        return;
    }
    nvmfs::NodeId id;
    if (!parse_id(tokens.value[1], id)) { kprintf("cd: invalid object ID\n"); return; }
    nvmfs::Node* node = nvmfs::find(id);
    if (!node || (node->type != nvmfs::NodeType::vault
        && node->type != nvmfs::NodeType::directory)) {
        kprintf("cd: target is not a container\n");
        return;
    }
    g_context = id;
}

static void command_create(const TokenList& tokens, nvmfs::NodeType type) {
    if (tokens.count != 2) { kprintf("usage: %s NAME\n", tokens.value[0]); return; }
    nvmfs::NodeId id;
    nvmfs::Error failure;
    if (!nvmfs::create(type, tokens.value[1], g_context, id, failure)) error(tokens.value[0], failure);
    else kprintf("created: %#lx\n", id);
}

static void command_find(const TokenList& tokens) {
    char* key = nullptr;
    char* value = nullptr;
    if (tokens.count != 2 || !parse_tag_query(tokens.value[1], key, value)) {
        kprintf("usage: find KEY=VALUE\n");
        return;
    }
    usize cursor = 0;
    nvmfs::NodeId id;
    bool any = false;
    while (nvmfs::find_tag(key, value, cursor, id)) {
        nvmfs::Node* node = nvmfs::find(id);
        if (node) { print_node(*node); any = true; }
    }
    if (!any) kprintf("no object tagged %s=%s\n", key, value);
}

static void command_stat(const TokenList& tokens) {
    nvmfs::NodeId id;
    if (tokens.count != 2 || !parse_id(tokens.value[1], id)) { kprintf("usage: stat ID\n"); return; }
    nvmfs::Node* node = nvmfs::find(id);
    if (!node) { kprintf("stat: object not found\n"); return; }
    print_node(*node);
    usize cursor = 0;
    nvmfs::TagView tag{};
    while (nvmfs::tag_at(cursor, tag))
        if (tag.node_id == id) kprintf("  %s=%s\n", tag.key, tag.value);
}

static void command_remove(const TokenList& tokens) {
    nvmfs::NodeId id;
    nvmfs::Error failure;
    if (tokens.count != 2 || !parse_id(tokens.value[1], id)) { kprintf("usage: rm ID\n"); return; }
    if (!nvmfs::erase(id, failure)) error("rm", failure);
    else kprintf("object removed\n");
}

static void command_tag(const TokenList& tokens, bool remove) {
    nvmfs::NodeId id;
    nvmfs::Error failure;
    if (tokens.count != 4 || !parse_id(tokens.value[1], id)) {
        kprintf("usage: %s ID KEY VALUE\n", tokens.value[0]);
        return;
    }
    const bool success = remove
        ? nvmfs::remove_tag(id, tokens.value[2], tokens.value[3], failure)
        : nvmfs::add_tag(id, tokens.value[2], tokens.value[3], failure);
    if (!success) error(tokens.value[0], failure);
    else kprintf("tag %s\n", remove ? "removed" : "indexed");
}

static void command_stream(const TokenList& tokens, bool append) {
    nvmfs::NodeId id;
    nvmfs::Error failure;
    if (tokens.count != 3 || !parse_id(tokens.value[1], id)) {
        kprintf("usage: %s ID \"TEXT\"\n", tokens.value[0]);
        return;
    }
    const usize size = strlen(tokens.value[2]);
    if (!nvmfs::write(id, tokens.value[2], size, append, failure)) error(tokens.value[0], failure);
    else kprintf("stream %s: %lu bytes\n", append ? "extended" : "written", size);
}

static void command_get(const TokenList& tokens) {
    nvmfs::NodeId id;
    if (tokens.count != 2 || !parse_id(tokens.value[1], id)) { kprintf("usage: get ID\n"); return; }
    nvmfs::Node* node = nvmfs::find(id);
    if (!node) { kprintf("get: object not found\n"); return; }
    for (usize i = 0; i < node->stream.size; ++i) kprintf("%c", static_cast<char>(node->stream.bytes[i]));
    if (node->stream.size && node->stream.bytes[node->stream.size - 1] != '\n') kprintf("\n");
}

static void command_sys() {
    u64 nodes, tags, streams, stream_limit, heap_used, heap_capacity;
    nvmfs::stats(nodes, tags, streams, stream_limit);
    mm::heap::stats(heap_used, heap_capacity);
    kprintf("C.O.N.D.R.  Command Operating Node Data Router\n");
    kprintf("kernel=x86_64 boot=Limine\n");
    kprintf("timer_ticks=%lu uptime=%lu s\n", arch::timer::ticks(), arch::timer::ticks() / 100);
    kprintf("NVMFS nodes=%lu tags=%lu streams=%lu/%lu bytes\n", nodes, tags, streams, stream_limit);
    kprintf("heap=%lu/%lu bytes\n", heap_used, heap_capacity);
}

static void command_reboot() {
    kprintf("Restarting C.O.N.D.R...\n");
    arch::cli();
    for (u32 attempt = 0; attempt < 100000; ++attempt)
        if ((arch::inb(0x64) & 0x02) == 0) break;
    arch::outb(0x64, 0xFE);
    arch::halt_forever();
}

static void execute(const char* input) {
    char line[LINE_CAPACITY];
    const usize length = strlen(input);
    if (length >= sizeof(line)) { kprintf("input too long\n"); return; }
    memcpy(line, input, length + 1);
    TokenList tokens{};
    if (!tokenize(line, tokens)) { kprintf("parse error: unmatched quote or too many arguments\n"); return; }
    if (!tokens.count) return;
    const char* command = tokens.value[0];

    if (equal(command, "help")) { print_help(); return; }
    if (equal(command, "ps")) { command_ps(); return; }
    if (equal(command, "vault")) { command_vault(tokens); return; }
    if (equal(command, "ls")) {
        nvmfs::NodeId container = g_context;
        if (tokens.count > 2 || (tokens.count == 2 && !parse_id(tokens.value[1], container))) {
            kprintf("usage: ls [ID]\n"); return;
        }
        list_container(container);
        return;
    }
    if (equal(command, "cd")) { command_cd(tokens); return; }
    if (equal(command, "pwd")) {
        nvmfs::Node* node = nvmfs::find(g_context);
        if (!node) kprintf("condr:/\n");
        else kprintf("condr:/%s [%#lx]\n", node->name, node->id);
        return;
    }
    if (equal(command, "mkdir")) { command_create(tokens, nvmfs::NodeType::directory); return; }
    if (equal(command, "touch")) { command_create(tokens, nvmfs::NodeType::file); return; }
    if (equal(command, "new")) {
        if (tokens.count != 3) { kprintf("usage: new TYPE NAME\n"); return; }
        nvmfs::NodeType type;
        if (!nvmfs::parse_type(tokens.value[1], type)) { kprintf("new: invalid type\n"); return; }
        nvmfs::NodeId id;
        nvmfs::Error failure;
        if (!nvmfs::create(type, tokens.value[2], g_context, id, failure)) error("new", failure);
        else kprintf("created: %#lx\n", id);
        return;
    }
    if (equal(command, "find")) { command_find(tokens); return; }
    if (equal(command, "stat")) { command_stat(tokens); return; }
    if (equal(command, "rm")) { command_remove(tokens); return; }
    if (equal(command, "tag")) { command_tag(tokens, false); return; }
    if (equal(command, "untag")) { command_tag(tokens, true); return; }
    if (equal(command, "put")) { command_stream(tokens, false); return; }
    if (equal(command, "add")) { command_stream(tokens, true); return; }
    if (equal(command, "get")) { command_get(tokens); return; }
    if (equal(command, "sys")) { command_sys(); return; }
    if (equal(command, "check")) {
        u32 checks;
        nvmfs::Error failure;
        if (nvmfs::selftest(checks, failure)) kprintf("NVMFS check: %u passed\n", checks);
        else error("check", failure);
        return;
    }
    if (equal(command, "test")) {
        u32 checks;
        nord::ipc::Error ipc_failure;
        nord::nvmfs::Error nvmfs_failure;
        if (nord::test::TestSuite::run_all()) {
            kprintf("[test] All tests passed!\n");
        } else {
            kprintf("[test] Some tests failed.\n");
        }
        return;
    }
    if (equal(command, "memtest")) {
        kprintf("[memtest] Not fully implemented yet.\n");
        return;
    }
    if (equal(command, "storagetest")) {
        kprintf("[storage] Not fully implemented yet.\n");
        return;
    }
    if (equal(command, "ver")) { kprintf("C.O.N.D.R. 0.1-dev x86_64\n"); return; }
    if (equal(command, "reboot")) { command_reboot(); return; }
    kprintf("%s: unknown command (type help)\n", command);
}

[[noreturn]] void custom_shell_run() {
    char line[LINE_CAPACITY];
    usize length = 0;
    line[0] = '\0';
    kprintf("\nC.O.N.D.R. object console. Type help.\n");
    kprintf("condr:/# ");
    for (;;) {
        const arch::keyboard::Event event = arch::keyboard::get_event();
        if (event.key != arch::keyboard::Key::character) continue;
        const char character = event.character;
        if (character == '\n' || character == '\r') {
            kprintf("\n");
            line[length] = '\0';
            execute(line);
            length = 0;
            line[0] = '\0';
            nvmfs::Node* context = nvmfs::find(g_context);
            if (!context) kprintf("condr:/# ");
            else kprintf("condr:/%s# ", context->name);
        } else if (character == '\b') {
            if (length) { --length; kprintf("\b \b"); }
        } else if (character >= 0x20 && character <= 0x7e && length + 1 < sizeof(line)) {
            line[length++] = character;
            kprintf("%c", character);
        }
    }
}

}