#pragma once

#include "config.h"
#include <cstdint>

#if !OS(SCARLET) || !ENABLE(C_LOOP) || ENABLE(JIT) || ENABLE(WEBASSEMBLY)
#error "The script proof requires the instrumented native Scarlet C-loop interpreter"
#endif

// Defined at the real C-loop bytecode gate in JavaScriptCore/llint/LLIntThunks.cpp.
extern "C" uint64_t scarlet_jsc_execution_entry_count();
