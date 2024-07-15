#pragma once

#include "Common/CommonTypes.h"
#include "Core/PowerPC/CachedInterpreter/InterpreterBlockCache.h"
#include "Core/PowerPC/JitCommon/JitBase.h"

class ThreadedInterpreter : public JitBase {
public:
  explicit ThreadedInterpreter(Core::System& system);
  ~ThreadedInterpreter();

  ThreadedInterpreter(const ThreadedInterpreter& ) = delete;
  ThreadedInterpreter(ThreadedInterpreter&&) = delete;
  ThreadedInterpreter& operator=(const ThreadedInterpreter& ) = delete;
  ThreadedInterpreter& operator=(ThreadedInterpreter&&) = delete;

  void Init() override;
  void Shutdown() override;
  void ClearCache() override;
  void SingleStep() override;
  void Run() override;
  void Jit(u32) override;

  JitBaseBlockCache* GetBlockCache() override { return &m_block_cache; }
  bool HandleFault(uintptr_t, SContext*) override { return false; }
  const char* GetName() const override { return "Threaded Interpreter"; }
  const CommonAsmRoutinesBase* GetAsmRoutines() override { return nullptr; }

private:
  struct Inst;

  BlockCache m_block_cache;
  std::vector<Inst> m_inst;
};
