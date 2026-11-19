#pragma once
#include <nord/types.hpp>

namespace nord::storage::persistence {

bool init();
bool available();
bool dirty();
bool sync();
bool selftest(u32& checks);

}