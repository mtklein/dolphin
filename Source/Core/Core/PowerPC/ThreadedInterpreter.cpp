#include "ThreadedInterpreter.h"

struct ThreadedInterpreter::Inst {
    void (*fn)(struct Inst const*, ThreadedInterpreter&);
    uintptr_t arg;
};

ThreadedInterpreter::ThreadedInterpreter(Core::System& system)
    : JitBase(system), m_block_cache(*this) {

    __builtin_debugtrap();
}

ThreadedInterpreter::~ThreadedInterpreter() {}

void ThreadedInterpreter::Init() {}
void ThreadedInterpreter::Shutdown() {}
void ThreadedInterpreter::ClearCache() {}
void ThreadedInterpreter::SingleStep() {}
void ThreadedInterpreter::Run() {}

void ThreadedInterpreter::Jit(u32 addr) {}
