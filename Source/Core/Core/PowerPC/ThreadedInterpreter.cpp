#include "Core/PowerPC/ThreadedInterpreter.h"

#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/HW/CPU.h"
#include "Core/System.h"

struct ThreadedInterpreter::Inst {
    void (*fn)(const Inst[], ThreadedInterpreter&, Interpreter&);
    uintptr_t data;

    static void Return(const Inst[], ThreadedInterpreter&, Interpreter&) {
        return;
    }

#define next return inst[1].fn(inst+1,ti,i)

    static void WritePC(const Inst inst[], ThreadedInterpreter& ti, Interpreter& i) {
        ti.m_ppc_state. pc = inst->data;
        ti.m_ppc_state.npc = inst->data + 4;
        next;
    }

    static void WriteBrokenBlockNPC(const Inst inst[], ThreadedInterpreter& ti, Interpreter& i) {
        ti.m_ppc_state.npc = inst->data;
        next;
    }

    static void HLEFunction(const Inst inst[], ThreadedInterpreter& ti, Interpreter& i) {
        Interpreter::HLEFunction(i, inst->data);
        next;
    }

    static void Interp(const Inst inst[], ThreadedInterpreter& ti, Interpreter& i) {
        Interpreter::RunInterpreterOp(i, inst->data);
        next;
    }

    static void EndBlock(const Inst inst[], ThreadedInterpreter& ti, Interpreter& i) {
        ti.m_ppc_state.pc = ti.m_ppc_state.npc;
        ti.m_ppc_state.downcount -= inst->data;
        PowerPC::UpdatePerformanceMonitor(inst->data,0,0, ti.m_ppc_state);
        next;
    }

    static void UpdateLS(const Inst inst[], ThreadedInterpreter& ti, Interpreter& i) {
        PowerPC::UpdatePerformanceMonitor(0,inst->data,0, ti.m_ppc_state);
        next;
    }

    static void UpdateFP(const Inst inst[], ThreadedInterpreter& ti, Interpreter& i) {
        PowerPC::UpdatePerformanceMonitor(0,0,inst->data, ti.m_ppc_state);
        next;
    }

    static void CheckFPU(const Inst inst[], ThreadedInterpreter& ti, Interpreter& i) {
        if (!ti.m_ppc_state.msr.FP) {
            ti.m_ppc_state.Exceptions |= EXCEPTION_FPU_UNAVAILABLE;
            ti.m_system.GetPowerPC().CheckExceptions();
            ti.m_ppc_state.downcount -= inst->data;
            return;
        }
        next;
    }

    static void CheckDSI(const Inst inst[], ThreadedInterpreter& ti, Interpreter& i) {
        if (ti.m_ppc_state.Exceptions & EXCEPTION_DSI) {
            ti.m_system.GetPowerPC().CheckExceptions();
            ti.m_ppc_state.downcount -= inst->data;
            return;
        }
        next;
    }

    static void CheckPE(const Inst inst[], ThreadedInterpreter& ti, Interpreter& i) {
        if (ti.m_ppc_state.Exceptions & EXCEPTION_PROGRAM) {
            ti.m_system.GetPowerPC().CheckExceptions();
            ti.m_ppc_state.downcount -= inst->data;
            return;
        }
        next;
    }

    static void CheckBreakpoint(const Inst inst[], ThreadedInterpreter& ti, Interpreter& i) {
        ti.m_system.GetPowerPC().CheckBreakPoints();
        if (ti.m_system.GetCPU().GetState() != CPU::State::Running) {
            ti.m_ppc_state.downcount -= inst->data;
            return;
        }
        next;
    }

    static void CheckIdle(const Inst inst[], ThreadedInterpreter& ti, Interpreter& i) {
        if (ti.m_ppc_state.npc == inst->data) {
            ti.m_system.GetCoreTiming().Idle();
        }
        next;
    }


#undef next
};

ThreadedInterpreter::ThreadedInterpreter(Core::System& system)
    : JitBase(system), block_cache(*this) {}

ThreadedInterpreter::~ThreadedInterpreter() = default;

void ThreadedInterpreter::Init() {
    RefreshConfig();
    jo.enableBlocklink = false;

    block_cache.Init();

    code_block.m_stats = &js.st;
    code_block.m_gpa   = &js.gpa;
    code_block.m_fpa   = &js.fpa;

    inst.reserve(CODE_SIZE / sizeof(Inst));
}

void ThreadedInterpreter::Shutdown() {
    block_cache.Shutdown();
}

void ThreadedInterpreter::ClearCache() {
    RefreshConfig();
    block_cache.Clear();
    inst.clear();
}

void ThreadedInterpreter::ExecuteOneBlock() {
    if (const u8 *found = block_cache.Dispatch()) {
        const Inst *cached = reinterpret_cast<const Inst*>(found);
        cached->fn(cached, *this, m_system.GetInterpreter());
    } else {
        Jit(m_ppc_state.pc);
    }
}

void ThreadedInterpreter::SingleStep() {
    m_system.GetCoreTiming().Advance();
    ExecuteOneBlock();
}

void ThreadedInterpreter::Run() {
    for (auto& cpu = m_system.GetCPU(); cpu.GetState() == CPU::State::Running; ) {
        m_system.GetCoreTiming().Advance();
        do {
            ExecuteOneBlock();
        } while (m_ppc_state.downcount > 0 && cpu.GetState() == CPU::State::Running);
    }
}

void ThreadedInterpreter::Jit(u32) {
    // TODO: CachedInterpreter::Jit() ignores its argument?!

    if (inst.size() >= CODE_SIZE / sizeof(Inst) - 0x1000
            || SConfig::GetInstance().bJITNoBlockCache) {
        ClearCache();
    }

    const u32 npc = analyzer.Analyze(m_ppc_state.pc, &code_block,
                                     &m_code_buffer, m_code_buffer.size());

    if (code_block.m_memory_exception) {
        m_ppc_state.npc = npc;
        m_ppc_state.Exceptions |= EXCEPTION_ISI;
        m_system.GetPowerPC().CheckExceptions();
        WARN_LOG_FMT(POWERPC, "ISI execption at {:#010x}", npc);
        return;
    }

    JitBlock *b = block_cache.AllocateBlock(m_ppc_state.pc);

    js.blockStart              = m_ppc_state.pc;
    js.firstFPInstructionFound = false;
    js.fifoBytesSinceCheck     = 0;
    js.downcountAmount         = 0;
    js.numLoadStoreInst        = 0;
    js.numFloatingPointInst    = 0;
    js.curBlock                = b;

    b->normalEntry =
    b->near_begin  = reinterpret_cast<u8*>(inst.data() + inst.size());

    auto push_end_block = [&]{
        inst.push_back({Inst::EndBlock, js.downcountAmount});
        if (js.numLoadStoreInst)     { inst.push_back({Inst::UpdateLS, js.numLoadStoreInst    }); }
        if (js.numFloatingPointInst) { inst.push_back({Inst::UpdateFP, js.numFloatingPointInst}); }
    };

    for (u32 i = 0; i < code_block.m_num_instructions; i++) {
        PPCAnalyst::CodeOp& op = m_code_buffer[i];

        js.downcountAmount += op.opinfo->num_cycles;
        if (op.opinfo->flags & FL_LOADSTORE) { js.numLoadStoreInst++; }
        if (op.opinfo->flags & FL_USE_FPU  ) { js.numFloatingPointInst++; }

        if (const auto result = HLE::TryReplaceFunction(m_ppc_symbol_db, op.address,
                                                        PowerPC::CoreMode::JIT)) {
            inst.push_back({Inst::WritePC    , op.address});
            inst.push_back({Inst::HLEFunction, result.hook_index});

            if (result.type == HLE::HookType::Replace) {
                inst.push_back({Inst::EndBlock, js.downcountAmount});
                inst.push_back({Inst::Return, 0});
                break;
            }
        }

        if (!op.skip) {
            const bool
                check_fpu = (op.opinfo->flags & FL_USE_FPU)   && !js.firstFPInstructionFound,
                 endblock = (op.opinfo->flags & FL_ENDBLOCK),
                 memcheck = (op.opinfo->flags & FL_LOADSTORE) && jo.memcheck,
                 check_pe = !endblock && ShouldHandleFPExceptionForInstruction(&op),
               breakpoint = m_enable_debugging
                         && m_system.GetPowerPC().GetBreakPoints().IsAddressBreakPoint(op.address);

            if (breakpoint || check_fpu || endblock || memcheck || check_pe) {
                inst.push_back({Inst::WritePC, op.address});
            }
            if (breakpoint) { inst.push_back({Inst::CheckBreakpoint, js.downcountAmount}); }
            if (check_fpu)  { inst.push_back({Inst::CheckFPU       , js.downcountAmount});
                              js.firstFPInstructionFound = true;                           }

            // TODO: improve, obviously
            inst.push_back({Inst::Interp, op.inst.hex});

            if (memcheck           ) { inst.push_back({Inst::CheckDSI , js.downcountAmount}); }
            if (check_pe           ) { inst.push_back({Inst::CheckPE  , js.downcountAmount}); }
            if (op.branchIsIdleLoop) { inst.push_back({Inst::CheckIdle, js.blockStart     }); }
            if (endblock           ) { push_end_block(); }
        }
    }

    if (code_block.m_broken) {
        inst.push_back({Inst::WriteBrokenBlockNPC, npc});
        push_end_block();
    }

    inst.push_back({Inst::Return, 0});

    b->near_end     = reinterpret_cast<u8*>(inst.data() + inst.size());
    b->codeSize     = b->near_end - b->normalEntry;
    b->originalSize = code_block.m_num_instructions;
    b->far_begin    = nullptr;
    b->far_end      = nullptr;

    block_cache.FinalizeBlock(*b, jo.enableBlocklink, code_block.m_physical_addresses);
}
