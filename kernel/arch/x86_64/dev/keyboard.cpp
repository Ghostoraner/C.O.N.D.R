#include <arch/x86_64/dev/keyboard.hpp>
#include <arch/x86_64/cpu/idt.hpp>
#include <arch/x86_64/cpu/scheduler.hpp>
#include <arch/x86_64/io.hpp>
#include <lib/print.hpp>

namespace nord::arch::keyboard {

static constexpr u16 DATA_PORT = 0x60;
static constexpr u16 STATUS_PORT = 0x64;
static constexpr u32 MAX_WAITERS = 8;

static bool g_shift = false;
static bool g_caps_lock = false;
static bool g_extended = false;
static Event g_events[64];
static u8 g_head = 0;
static u8 g_tail = 0;
static u64 g_dropped_events = 0;

// Tasks blocked in a read. Every event wakes all of them; each one re-checks
// focus and the queue, so each event is consumed by exactly one reader.
static u32 g_waiters[MAX_WAITERS];

// Focus owner: 0 = kernel console (shell), otherwise a user task id.
static u32 g_focus = 0;

static char translate(u8 code, bool shifted);

static bool enqueue(Event event) {
    const u8 head = __atomic_load_n(&g_head, __ATOMIC_RELAXED);
    const u8 next = static_cast<u8>((head + 1) & 63);
    if (next == __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE)) {
        ++g_dropped_events;
        kprintf("[keyboard] dropped event: ring buffer full (%lu dropped)\n", g_dropped_events);
        return false;
    }
    g_events[head] = event;
    __atomic_store_n(&g_head, next, __ATOMIC_RELEASE);
    return true;
}

static bool queue_has_event() {
    return __atomic_load_n(&g_tail, __ATOMIC_RELAXED) != __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
}

static void flush_queue() {
    __atomic_store_n(&g_tail, __atomic_load_n(&g_head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

static void wake_all_waiters() {
    for (u32 i = 0; i < MAX_WAITERS; ++i) {
        if (g_waiters[i]) scheduler::wake(g_waiters[i]);
    }
}

static bool add_waiter(u32 task_id) {
    u32 free_slot = MAX_WAITERS;
    for (u32 i = 0; i < MAX_WAITERS; ++i) {
        if (g_waiters[i] == task_id) return true;
        if (!g_waiters[i] && free_slot == MAX_WAITERS) free_slot = i;
    }
    if (free_slot == MAX_WAITERS) return false;
    g_waiters[free_slot] = task_id;
    return true;
}

static void remove_waiter(u32 task_id) {
    for (u32 i = 0; i < MAX_WAITERS; ++i) {
        if (g_waiters[i] == task_id) g_waiters[i] = 0;
    }
}

static bool decode(u8 scan, Event& event) {
    if (scan == 0xE0 || scan == 0xE1) {
        g_extended = true;
        return false;
    }
    const bool released = (scan & 0x80) != 0;
    const u8 code = scan & 0x7F;
    if (g_extended) {
        g_extended = false;
        if (released) return false;
        switch (code) {
        case 0x48: event = {Key::up, 0}; return true;
        case 0x50: event = {Key::down, 0}; return true;
        case 0x4B: event = {Key::left, 0}; return true;
        case 0x4D: event = {Key::right, 0}; return true;
        case 0x47: event = {Key::home, 0}; return true;
        case 0x4F: event = {Key::end, 0}; return true;
        case 0x53: event = {Key::delete_key, 0}; return true;
        default: return false;
        }
    }
    if (code == 0x2A || code == 0x36) {
        g_shift = !released;
        return false;
    }
    if (code == 0x3A && !released) {
        g_caps_lock = !g_caps_lock;
        return false;
    }
    if (released) return false;

    const bool is_letter = (code >= 0x10 && code <= 0x19)
        || (code >= 0x1E && code <= 0x26)
        || (code >= 0x2C && code <= 0x32);
    const bool shifted = is_letter ? (g_shift ^ g_caps_lock) : g_shift;
    const char character = translate(code, shifted);
    if (character == 0) return false;
    event = {Key::character, character};
    return true;
}

void handle_irq(Registers*) {
    while (inb(STATUS_PORT) & 0x01) {
        Event event{};
        if (decode(inb(DATA_PORT), event) && enqueue(event)) wake_all_waiters();
    }
}

bool try_get_event(Event& event) {
    const u8 tail = __atomic_load_n(&g_tail, __ATOMIC_RELAXED);
    if (tail == __atomic_load_n(&g_head, __ATOMIC_ACQUIRE)) return false;
    event = g_events[tail];
    __atomic_store_n(&g_tail, static_cast<u8>((tail + 1) & 63), __ATOMIC_RELEASE);
    return true;
}

// Blocks `task` until an event is available to `owner`. `owner` is 0 for the
// console or the task id of a user program holding focus.
static Event wait_event(u32 task_id, u32 owner) {
    Event event{};
    for (;;) {
        if (g_focus == owner && try_get_event(event)) return event;

        arch::cli();
        // Check and register in one critical section: an IRQ after this point
        // finds the task in the waiter list and wakes it, so no wakeup is lost.
        if (g_focus == owner && queue_has_event()) {
            arch::sti();
            continue;
        }
        if (!add_waiter(task_id)) {
            // Waiter list is full: back off and retry.
            arch::sti();
            scheduler::yield();
            continue;
        }
        scheduler::block_current();   // returns with interrupts enabled
        remove_waiter(task_id);
    }
}

Event get_event() {
    return wait_event(scheduler::current_id(), 0);
}

bool try_getc(char& character) {
    Event event{};
    while (try_get_event(event)) {
        if (event.key == Key::character) {
            character = event.character;
            return true;
        }
    }
    return false;
}

char getc() {
    Event event{};
    do { event = get_event(); } while (event.key != Key::character);
    return event.character;
}

bool request_focus(u32 task_id) {
    if (!task_id) return false;
    if (g_focus == task_id) return true;
    if (g_focus != 0) return false;          // another program owns the keyboard
    g_focus = task_id;
    flush_queue();                           // keys typed earlier belong to the console
    wake_all_waiters();
    return true;
}

bool release_focus(u32 task_id) {
    if (!task_id || g_focus != task_id) return false;
    g_focus = 0;
    flush_queue();
    wake_all_waiters();
    return true;
}

u32 focus_owner() {
    return g_focus;
}

bool read_char_for_task(u32 task_id, char& character) {
    if (!task_id || g_focus != task_id) return false;
    for (;;) {
        const Event event = wait_event(task_id, task_id);
        if (event.key == Key::character) {
            character = event.character;
            return true;
        }
    }
}

void clear_waiter(u32 task_id) {
    remove_waiter(task_id);
    if (g_focus == task_id) {
        g_focus = 0;
        flush_queue();
        wake_all_waiters();
    }
}

void init() {
    idt::set_handler(0x21, handle_irq);
    outb(STATUS_PORT, 0xAE);
    while (inb(STATUS_PORT) & 0x01) (void)inb(DATA_PORT);
    g_shift = false;
    g_caps_lock = false;
    g_extended = false;
    g_head = 0;
    g_tail = 0;
    g_dropped_events = 0;
    for (u32 i = 0; i < MAX_WAITERS; ++i) g_waiters[i] = 0;
    g_focus = 0;
}

static char translate(u8 code, bool shifted) {
    if (code >= 0x10 && code <= 0x19) {
        static constexpr char normal[] = "qwertyuiop";
        static constexpr char upper[] = "QWERTYUIOP";
        return shifted ? upper[code - 0x10] : normal[code - 0x10];
    }
    if (code >= 0x1E && code <= 0x26) {
        static constexpr char normal[] = "asdfghjkl";
        static constexpr char upper[] = "ASDFGHJKL";
        return shifted ? upper[code - 0x1E] : normal[code - 0x1E];
    }
    if (code >= 0x2C && code <= 0x32) {
        static constexpr char normal[] = "zxcvbnm";
        static constexpr char upper[] = "ZXCVBNM";
        return shifted ? upper[code - 0x2C] : normal[code - 0x2C];
    }

    switch (code) {
    case 0x02: return shifted ? '!' : '1';
    case 0x03: return shifted ? '@' : '2';
    case 0x04: return shifted ? '#' : '3';
    case 0x05: return shifted ? '$' : '4';
    case 0x06: return shifted ? '%' : '5';
    case 0x07: return shifted ? '^' : '6';
    case 0x08: return shifted ? '&' : '7';
    case 0x09: return shifted ? '*' : '8';
    case 0x0A: return shifted ? '(' : '9';
    case 0x0B: return shifted ? ')' : '0';
    case 0x0C: return shifted ? '_' : '-';
    case 0x0D: return shifted ? '+' : '=';
    case 0x1A: return shifted ? '{' : '[';
    case 0x1B: return shifted ? '}' : ']';
    case 0x27: return shifted ? ':' : ';';
    case 0x28: return shifted ? '"' : '\'';
    case 0x29: return shifted ? '~' : '`';
    case 0x2B: return shifted ? '|' : '\\';
    case 0x33: return shifted ? '<' : ',';
    case 0x34: return shifted ? '>' : '.';
    case 0x35: return shifted ? '?' : '/';
    case 0x39: return ' ';
    case 0x0E: return '\b';
    case 0x1C: return '\n';
    case 0x0F: return '\t';
    default: return 0;
    }
}

bool test_enqueue(Event event) {
    return enqueue(event);
}

u64 test_dropped_count() {
    return g_dropped_events;
}

bool test_add_waiter(u32 task_id) {
    return add_waiter(task_id);
}

void test_remove_waiter(u32 task_id) {
    remove_waiter(task_id);
}

u64 test_waiter_count() {
    u64 count = 0;
    for (u32 i = 0; i < MAX_WAITERS; ++i) {
        if (g_waiters[i]) ++count;
    }
    return count;
}

}