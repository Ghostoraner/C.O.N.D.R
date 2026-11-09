#pragma once
#include <arch/x86_64/cpu/idt.hpp>

namespace nord::arch::keyboard {

enum class Key : unsigned char {
    character,
    up,
    down,
    left,
    right,
    home,
    end,
    delete_key,
};

struct Event {
    Key key;
    char character;
};

void init();
void handle_irq(Registers* registers);

// Console (kernel shell) input. Blocks while the console does not hold focus.
Event get_event();
bool try_get_event(Event& event);
char getc();
bool try_getc(char& character);

// User-mode input. A task must take focus with request_focus() first.
bool request_focus(u32 task_id);
bool release_focus(u32 task_id);
u32 focus_owner();                              // 0 = console
bool read_char_for_task(u32 task_id, char& character);

// Called when a task exits: removes it from the waiter list and releases focus.
void clear_waiter(u32 task_id);

u64 dropped_events();

bool test_enqueue(Event event);
u64 test_dropped_count();
bool test_add_waiter(u32 task_id);
void test_remove_waiter(u32 task_id);
u64 test_waiter_count();

}