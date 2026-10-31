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
Event get_event();
bool try_get_event(Event& event);
char getc();
bool try_getc(char& character);

}