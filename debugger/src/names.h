#pragma once

#include <cstdint>
#include <string>

// "F$Sleep" for an OS-9 call code (the word after TRAP #0), or nullptr.
// Names from CeDImu's OS9/SystemCalls.hpp.
const char* os9_call_name(uint16_t code);

// "bus error", "TRAP #0", "on-chip IRQ 5", ... for an exception vector.
std::string vector_name(uint8_t vector);

// OS-9 TRAPs carry a function word: a TRAP takes 4 bytes in a listing.
inline bool is_trap_opcode(uint16_t opcode) { return (opcode & 0xfff0) == 0x4e40; }
