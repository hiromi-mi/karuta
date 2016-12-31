// -*- C++ -*-
#ifndef _synth_method_synth_h_
#define _synth_method_synth_h_

#include "synth/insn_walker.h"

#include <map>

using std::map;

namespace synth {

class MethodSynth : public InsnWalker {
public:
  MethodSynth(ThreadSynth *thr_synth,
	      const string &method_name, ITable *tab, ResourceSet *res);
  virtual ~MethodSynth();

  bool Synth();
  MethodContext *GetContext();
  void InjectTaskEntry(IState *st);

private:
  void SynthNativeImplMethod(vm::Method *method);
  void SynthEmbeddedMethod(vm::Method *method);

  void SynthInsn(vm::Insn *insn);

  void SynthNop(vm::Insn *insn);
  void SynthNum(vm::Insn *insn);
  void SynthAssign(vm::Insn *insn);
  void SynthLoadObj(vm::Insn *insn);
  void SynthFuncall(vm::Insn *insn);
  void SynthFuncallDone(vm::Insn *insn);
  void SynthBinCalcExpr(vm::Insn *insn);
  void SynthBitInv(vm::Insn *insn);
  void SynthShiftExpr(vm::Insn *insn);
  void SynthIf(vm::Insn *insn);
  void SynthGoto(vm::Insn *insn);
  void SynthMemberAccess(vm::Insn *insn, bool is_store);
  void SynthArrayAccess(vm::Insn *insn, bool is_write, bool is_memory);
  void SynthBitRange(vm::Insn *insn);
  void SynthChannelAccess(vm::Insn *insn, bool is_write);
  void SynthConcat(vm::Insn *insn);
  void SynthPreIncDec(vm::Insn *insn);
  void SynthNative(vm::Insn *insn);

  void GenNeg(IRegister *src, IRegister *dst);
  void EmitEntryInsn(vm::Method *method);

  ThreadSynth *thr_synth_;
  const string method_name_;
  ITable *tab_;
  ResourceSet *res_;
  std::unique_ptr<MethodContext> context_;

  // VM -> Iroha mapping.
  map<vm::Register *, IRegister *> local_reg_map_;
  map<string, IRegister *> member_name_reg_map_;

  map<int, StateWrapper *> vm_insn_state_map_;

  IRegister *FindLocalVarRegister(vm::Register *vreg);
  IRegister *FindArgRegister(vm::Method *method, int nth,
			     fe::VarDecl *arg_decl);
  StateWrapper *AllocState();
  void ResolveJumps();
  void LinkStates();
  void InsnToCalcValueType(vm::Insn *insn, IValueType *vt);
};

}  // namespace synth

#endif  // _synth_method_synth_h_
