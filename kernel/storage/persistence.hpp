#pragma once

namespace nord::storage::persistence {

bool init();
bool available();
bool dirty();
bool sync();

}