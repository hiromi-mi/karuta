// -*- C++ -*-
#ifndef _compiler_expr_compiler_h_
#define _compiler_expr_compiler_h_

#include "compiler/common.h"

#include "fe/nodecode.h"
#include "vm/value.h"
#include "vm/opcode.h"

namespace compiler {

class ExprCompiler {
public:
  ExprCompiler(Compiler *compiler);

  vm::Register *CompileExpr(fe::Expr *expr);
  vm::Register *CompileSymExpr(fe::Expr *expr);
  static void FlattenCommas(fe::Expr *expr, vector<fe::Expr*> *commas);

private:
  vm::Register *CompileAssign(fe::Expr *expr);
  vm::Register *CompileAssignToArray(vm::Insn *insn, fe::Expr *lhs,
				     vm::Register *rhs_reg);
  vm::Register *CompileAssignToElmRef(vm::Insn *insn, fe::Expr *lhs,
				      vm::Register *rhs_reg);
  vm::Register *CompileAssignToSym(vm::Insn *insn, fe::Expr *lhs,
				   vm::Register *rhs_reg);
  vm::Register *CompileMemberSym(fe::Expr *expr);
  vm::Register *CompileArrayRef(fe::Expr *expr);
  vm::Register *CompileAssignToUniopRef(vm::Insn *insn, fe::Expr *lhs,
					vm::Register *rhs_reg);
  vm::Register *CompileComma(fe::Expr *expr);
  vm::Register *CompileElmRef(fe::Expr *expr);
  vm::Register *CompileFuncallExpr(vm::Register *obj, fe::Expr *expr);
  vm::Register *CompileMultiValueFuncall(vm::Register *obj,
					 fe::Expr *funcall,
					 fe::Expr *lhs);
  vm::Register *CompileSimpleExpr(fe::Expr *expr);
  vm::Register *CompileRef(fe::Expr *expr);
  vm::Register *CompileRefLhsExpr(fe::Expr *lhs_expr, vm::Insn *insn);
  vm::Register *CompileTriTerm(fe::Expr *expr);

  vm::Value::ValueType GetVariableType(sym_t name);
  vm::OpCode GetOpCodeFromExpr(fe::Expr *expr);
  vm::Register *UpdateModifyOp(fe::NodeCode type, fe::Expr *lhs_expr,
			       vm::Register *rhs_reg);
  vm::Register *MayRewriteOperator(vm::Insn *orig_insn);
  void PropagateRegisterType(vm::Insn *insn,
			     vm::Register *lhs, vm::Register *rhs,
			     vm::RegisterType *t);
  vm::Register *EmitFuncallDone(vm::Insn *call_insn, fe::Expr *lhs);

  Compiler *compiler_;
};

}  // namespace compiler

#endif  // _compiler_expr_compiler_h_