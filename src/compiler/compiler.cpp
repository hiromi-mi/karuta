// Compiles a parse tree into vm instructions.
#include "compiler/compiler.h"

#include "pool.h"
#include "status.h"
#include "fe/expr.h"
#include "fe/method.h"
#include "fe/stmt.h"
#include "fe/var_decl.h"
#include "vm/insn.h"
#include "vm/insn_annotator.h"
#include "vm/method.h"
#include "vm/object.h"
#include "vm/opcode.h"
#include "vm/value.h"
#include "vm/vm.h"

#include <map>
using std::map;

namespace compiler {

bool Compiler::dbg_bytecode_;

class VarScope {
public:
  // local name -> Register.
  map<sym_t, vm::Register*> local_regs_;
};

static void FlattenCommas(fe::Expr *expr, vector<fe::Expr*> *commas) {
  if (!expr) {
    return;
  }
  if (expr->type_ == fe::BINOP_COMMA) {
    FlattenCommas(expr->lhs_, commas);
    commas->push_back(expr->rhs_);
  } else {
    commas->push_back(expr);
  }
}

vm::Value::ValueType Compiler::GetVariableType(sym_t name) {
  vm::Register *reg = LookupLocalVar(name);
  if (reg) {
    return reg->type_.value_type_;
  } else {
    vm::Value *value = obj_->LookupValue(name, false);
    if (!value) {
      Status::os(Status::USER)
	<< "'" << sym_cstr(name) << "' is not a member of the object";
      MessageFlush::Get(Status::USER);
      return vm::Value::NONE;
    }
    return value->type_;
  }
}

// static
vm::Method *Compiler::CompileMethod(vm::VM *vm, vm::Object *obj,
				    const fe::Method *parse_tree,
				    vm::Method *method) {
  if (method && !method->parse_tree_) {
    // native method.
    return method;
  }
  std::unique_ptr<Compiler> compiler(new Compiler(vm, obj, parse_tree));
  return compiler->Compile(method);
}

void Compiler::SetByteCodeDebug(bool enable) {
  dbg_bytecode_ = enable;
}

Compiler::Compiler(vm::VM *vm, vm::Object *obj, const fe::Method *parse_tree)
  : vm_(vm), obj_(obj), tree_(parse_tree), last_queued_insn_(nullptr) {
}

vm::Method *Compiler::Compile(vm::Method *method) {
  method_ = method;
  if (!method_) {
    // top level.
    method_ = vm_->NewMethod(true /* toplevel */);
  }
  if (method_->insns_.size() > 0) {
    // already compiled.
    return method_;
  }

  PushScope();
  method_->parse_tree_ = tree_;

  SetupArgumentRegisters();
  SetupReturnRegisters();

  for (size_t i = 0; i < tree_->stmts_.size(); ++i) {
    CompileStmt(tree_->stmts_[i]);
    CompilePreIncDec();
    FlushPendingInsns();
    CompilePostIncDec();
  }
  PopScope();
  AddLabel(sym_return);
  // If a jump targets the label at the last insn.
  EmitNop();
  FlushPendingInsns();
  ResolveLabels();
  vm::InsnAnnotator::AnnotateMethod(vm_, obj_, method_);
  if (dbg_bytecode_) {
    // Top level result will be output after execution.
    if (!method_->IsTopLevel()) {
      method_->Dump();
    }
  }

  return method_;
}

void Compiler::AddLabel(sym_t label) {
  label_insn_map_[label] = last_queued_insn_;
}

void Compiler::ResolveLabels() {
  for (size_t i = 0; i < method_->insns_.size(); ++i) {
    vm::Insn *insn = method_->insns_[i];
    if (insn->op_ == vm::OP_GOTO) {
      if (insn->insn_stmt_) {
	insn->jump_target_ = InsnIndexFromLabel(insn->insn_stmt_->sym_);
      } else if (insn->label_) {
	insn->jump_target_ = InsnIndexFromLabel(insn->label_);
      } else {
	insn->jump_target_ = InsnIndexFromLabel(sym_return);
      }
    }
    if (insn->op_ == vm::OP_IF) {
      if (insn->insn_stmt_) {
	// generated by if-stmt.
	insn->jump_target_ = InsnIndexFromLabel(insn->insn_stmt_->label_f_);
      } else {
	// generated by tri term-expr.
	insn->jump_target_ = InsnIndexFromLabel(insn->label_);
      }
    }
  }
}

void Compiler::EmitNop() {
  vm::Insn *insn = new vm::Insn;
  insn->op_ = vm::OP_NOP;
  EmitInsn(insn);
}

void Compiler::CompilePreIncDec() {
  DoCompilePrePostIncDec(false, &pre_inc_dec_exprs_);
}

void Compiler::CompilePostIncDec() {
  DoCompilePrePostIncDec(true, &post_inc_dec_exprs_);
}

void Compiler::DoCompilePrePostIncDec(bool is_post, vector<fe::Expr*> *exprs) {
  for (size_t i = 0; i < exprs->size(); ++i) {
    CompileIncDecExpr(exprs->at(i));
  }
  if (is_post) {
    post_inc_dec_exprs_.clear();
  } else {
    pre_inc_dec_exprs_.clear();
  }
}

void Compiler::CompileIncDecExpr(fe::Expr *expr) {
  vm::Insn *insn = new vm::Insn;
  if (expr->type_ == fe::UNIOP_PRE_INC ||
      expr->type_ == fe::UNIOP_POST_INC) {
    insn->op_ = vm::OP_PRE_INC;
  } else {
    insn->op_ = vm::OP_PRE_DEC;
  }
  vm::Register *reg = CompileSymExpr(expr->args_);
  if (!reg) {
    Status::os(Status::USER) << "Invalid inc/dec";
    MessageFlush::Get(Status::USER);
    return;
  }
  insn->dst_regs_.push_back(reg);
  insn->src_regs_.push_back(reg);
  EmitInsnNow(insn);
}

void Compiler::FlushPendingInsns() {
  for (size_t i = 0; i < pending_insns_.size(); ++i) {
    vm::Insn *insn = pending_insns_[i];
    method_->insns_.push_back(insn);
  }
  pending_insns_.clear();
}

void Compiler::SetupArgumentRegisters() {
  if (!tree_->args_) {
    return;
  }
  SetupDeclSetRegisters(*tree_->args_, nullptr);
}

void Compiler::SetupReturnRegisters() {
  if (!tree_->returns_) {
    return;
  }
  SetupDeclSetRegisters(*tree_->returns_, &method_->return_types_);
}

void Compiler::SetupDeclSetRegisters(fe::VarDeclSet &vds,
				     vector<vm::RegisterType> *types) {
  for (size_t i = 0; i < vds.decls.size(); ++i) {
    fe::VarDecl *decl = vds.decls[i];
    vm::Register *reg = AllocRegister();
    if (decl->GetNameExpr()) {
      reg->orig_name_ = decl->GetNameExpr()->sym_;
    }
    VarScope *scope = CurrentScope();
    if (reg->orig_name_) {
      scope->local_regs_[reg->orig_name_] = reg;
    }
    vm::InsnAnnotator::AnnotateByDecl(vm_, decl, reg);
    if (types) {
      types->push_back(reg->type_);
    }
  }
}

vm::Register *Compiler::GetNthReturnRegister(int nth) {
  if (tree_->args_) {
    nth += tree_->args_->decls.size();
  }
  return method_->method_regs_[nth];
}

void Compiler::PushScope() {
  VarScope *scope = new VarScope;
  bindings_.push_back(scope);
}

void Compiler::PopScope() {
  VarScope *scope = *(bindings_.rbegin());
  delete scope;
  bindings_.pop_back();
}

VarScope *Compiler::CurrentScope() {
  return *(bindings_.rbegin());
}

void Compiler::EmitInsn(vm::Insn *insn) {
  pending_insns_.push_back(insn);
  last_queued_insn_ = insn;
}

void Compiler::EmitInsnNow(vm::Insn *insn) {
  method_->insns_.push_back(insn);
  last_queued_insn_ = insn;
}

void Compiler::CompileStmt(fe::Stmt *stmt) {
  switch (stmt->type_) {
  case fe::STMT_EXPR:
    CompileExpr(stmt->expr_);
    break;
  case fe::STMT_FUNCDECL:
    CompileFuncDecl(stmt);
    break;
  case fe::STMT_IMPORT:
    CompileImportStmt(stmt);
    break;
  case fe::STMT_SPAWN:
    CompileSpawnStmt(stmt);
    break;
  case fe::STMT_VARDECL:
    CompileVarDeclStmt(stmt);
    break;
  case fe::STMT_IF:
    CompileIfStmt(stmt);
    break;
  case fe::STMT_GOTO:
    CompileGoto(stmt);
    break;
  case fe::STMT_LABEL:
    CompileLabel(stmt);
    break;
  case fe::STMT_PUSH_BINDING:
    PushScope();
    break;
  case fe::STMT_POP_BINDING:
    PopScope();
    break;
  case fe::STMT_RETURN:
    CompileReturn(stmt);
    break;
  case fe::STMT_THREAD_DECL:
    CompileThreadDecl(stmt);
    break;
  case fe::STMT_CHANNEL_DECL:
    CompileChannelDecl(stmt);
    break;
  case fe::STMT_MAILBOX_DECL:
    CompileMailboxDecl(stmt);
    break;
  default:
    CHECK(false) << "Unknown stmt:" << fe::NodeName(stmt->type_);
    break;
  }
}

void Compiler::CompileGoto(fe::Stmt *stmt) {
  vm::Insn *insn = new vm::Insn;
  insn->op_ = vm::OP_GOTO;
  insn->insn_stmt_ = stmt;
  EmitInsn(insn);
}

void Compiler::CompileReturn(fe::Stmt *stmt) {
  size_t num_returns = method_->GetNumReturnRegisters();
  vector<fe::Expr*> value_exprs;
  FlattenCommas(stmt->expr_, &value_exprs);
  CHECK(num_returns == value_exprs.size());
  vector<vm::Register *> regs;
  for (size_t i = 0; i < value_exprs.size(); ++i) {
    regs.push_back(CompileExpr(value_exprs[i]));
  }
  for (size_t i = 0; i < value_exprs.size(); ++i) {
    vm::Insn *insn = new vm::Insn;
    insn->op_ = vm::OP_ASSIGN;
    insn->insn_expr_ = value_exprs[i];
    insn->dst_regs_.push_back(GetNthReturnRegister(i));
    insn->src_regs_.push_back(GetNthReturnRegister(i));
    insn->src_regs_.push_back(regs[i]);
    EmitInsn(insn);
  }

  vm::Insn *insn = new vm::Insn;
  insn->op_ = vm::OP_GOTO;
  insn->insn_stmt_ = nullptr;
  EmitInsn(insn);
}

void Compiler::CompileLabel(fe::Stmt *stmt) {
  AddLabel(stmt->sym_);
}

void Compiler::CompileIfStmt(fe::Stmt *stmt) {
  vm::Insn *insn = new vm::Insn;
  insn->op_ = vm::OP_IF;
  insn->src_regs_.push_back(CompileExpr(stmt->expr_));
  insn->insn_stmt_ = stmt;
  EmitInsn(insn);
}

void Compiler::CompileVarDeclStmt(fe::Stmt *stmt) {
  fe::Expr *var_expr = stmt->decl_->GetNameExpr();
  vm::Register *rhs_val = nullptr;
  if (stmt->decl_->GetInitialVal() != nullptr) {
    rhs_val = CompileExpr(stmt->decl_->GetInitialVal());
  }

  if (var_expr->type_ == fe::EXPR_ELM_SYM_REF) {
    CHECK(IsTopLevel());
    if (rhs_val) {
      vm::InsnAnnotator::AnnotateByDecl(vm_, stmt->decl_, rhs_val);
    }
    CompileMemberDeclStmt(stmt, var_expr, vm::OP_VARDECL, rhs_val);
    return;
  }

  // local variable.
  CHECK(var_expr->type_ == fe::EXPR_SYM);
  sym_t name = var_expr->sym_;
  vm::Register *reg = AllocRegister();
  reg->orig_name_ = name;
  VarScope *scope = CurrentScope();
  scope->local_regs_[name] = reg;
  vm::InsnAnnotator::AnnotateByDecl(vm_, stmt->decl_, reg);
  if (stmt->decl_->GetObjectName() != sym_null) {
    if (IsTopLevel()) {
      // Set type object later.
      vm::Insn *insn = new vm::Insn;
      insn->op_ = vm::OP_SET_TYPE_OBJECT;
      insn->dst_regs_.push_back(reg);
      insn->insn_stmt_ = stmt;
      EmitInsn(insn);
    } else {
      CHECK(reg->type_object_ != nullptr);
    }
  }

  if (rhs_val) {
    SimpleAssign(rhs_val, reg);
  }
}

void Compiler::CompileThreadDecl(fe::Stmt *stmt) {
  fe::Expr *var_expr = stmt->expr_->lhs_;
  CHECK(var_expr->type_ == fe::EXPR_ELM_SYM_REF);
  CompileMemberDeclStmt(stmt, var_expr, vm::OP_THREAD_DECL, nullptr);
}

void Compiler::CompileChannelDecl(fe::Stmt *stmt) {
  fe::Expr *var_expr = stmt->expr_;
  CHECK(var_expr->type_ == fe::EXPR_ELM_SYM_REF);
  CompileMemberDeclStmt(stmt, var_expr, vm::OP_CHANNEL_DECL, nullptr);
}
                  
void Compiler::CompileMailboxDecl(fe::Stmt *stmt) {
  fe::Expr *var_expr = stmt->expr_;
  CHECK(var_expr->type_ == fe::EXPR_ELM_SYM_REF);
  CompileMemberDeclStmt(stmt, var_expr, vm::OP_MAILBOX_DECL, nullptr);
}

void Compiler::CompileMemberDeclStmt(fe::Stmt *stmt, fe::Expr *var_expr,
				     vm::OpCode op, vm::Register *initial_val) {
  CHECK(IsTopLevel());
  vm::Register *obj_reg = CompilePathHead(var_expr);
  // Object manipulation. do in the executor.
  vm::Insn *insn = new vm::Insn;
  insn->op_ = op;
  insn->insn_stmt_ = stmt;
  insn->obj_reg_ = obj_reg;
  if (op == vm::OP_THREAD_DECL) {
    insn->label_ = stmt->expr_->func_->func_->sym_;
  }
  if (op == vm::OP_CHANNEL_DECL ||
      op == vm::OP_MAILBOX_DECL) {
    insn->label_ = stmt->expr_->sym_;
  }
  EmitInsn(insn);
  if (op == vm::OP_VARDECL && initial_val) {
    insn = new vm::Insn;
    insn->op_ = vm::OP_MEMBER_WRITE;
    insn->label_ = var_expr->sym_;
    insn->obj_reg_ = obj_reg;
    insn->src_regs_.push_back(initial_val);
    insn->src_regs_.push_back(obj_reg);
    insn->dst_regs_.push_back(insn->src_regs_[0]);
    EmitInsn(insn);
  }
}

void Compiler::CompileImportStmt(fe::Stmt *stmt) {
  vm::Insn *insn = new vm::Insn;
  insn->op_ = vm::OP_IMPORT;
  insn->insn_stmt_ = stmt;
  EmitInsn(insn);
}

void Compiler::CompileSpawnStmt(fe::Stmt *stmt) {
  vm::Insn *insn = new vm::Insn;
  insn->op_ = vm::OP_SPAWN;
  insn->insn_expr_ = stmt->expr_;
  EmitInsn(insn);
}

vm::Register *Compiler::CompileExpr(fe::Expr *expr) {
  if (expr->type_ == fe::EXPR_SYM) {
    return CompileSymExpr(expr);
  }
  if (expr->type_ == fe::EXPR_FUNCALL) {
    return CompileFuncallExpr(nullptr, expr);
  }
  if (expr->type_ == fe::UNIOP_POST_INC ||
      expr->type_ == fe::UNIOP_POST_DEC) {
    post_inc_dec_exprs_.push_back(expr);
    return CompileExpr(expr->args_);
  }
  if (expr->type_ == fe::UNIOP_PRE_INC ||
      expr->type_ == fe::UNIOP_PRE_DEC) {
    pre_inc_dec_exprs_.push_back(expr);
    return CompileExpr(expr->args_);
  }
  if (expr->type_ == fe::BINOP_ASSIGN ||
      expr->type_ == fe::BINOP_ADD_ASSIGN ||
      expr->type_ == fe::BINOP_SUB_ASSIGN ||
      expr->type_ == fe::BINOP_MUL_ASSIGN ||
      expr->type_ == fe::BINOP_LSHIFT_ASSIGN ||
      expr->type_ == fe::BINOP_RSHIFT_ASSIGN ||
      expr->type_ == fe::BINOP_AND_ASSIGN ||
      expr->type_ == fe::BINOP_XOR_ASSIGN ||
      expr->type_ == fe::BINOP_OR_ASSIGN) {
    return CompileAssign(expr);
  }
  if (expr->type_ == fe::BINOP_ARRAY_REF) {
    return CompileArrayRef(expr);
  }
  if (expr->type_ == fe::BINOP_COMMA) {
    return CompileComma(expr);
  }
  if (expr->type_ == fe::BINOP_ELM_REF) {
    return CompileElmRef(expr);
  }
  if (expr->type_ == fe::EXPR_TRI_TERM) {
    return CompileTriTerm(expr);
  }
  if (expr->type_ == fe::UNIOP_REF) {
    return CompileRef(expr);
  }

  // Simple 1 dst reg cases.
  vm::Register *reg = AllocRegister();
  vm::Insn *insn = new vm::Insn;
  insn->op_ = GetOpCodeFromExpr(expr);
  insn->insn_expr_ = expr;
  insn->dst_regs_.push_back(reg);
  switch (insn->op_) {
  case vm::OP_ADD:
  case vm::OP_SUB:
  case vm::OP_MUL:
  case vm::OP_GT:
  case vm::OP_LT:
  case vm::OP_GTE:
  case vm::OP_LTE:
  case vm::OP_EQ:
  case vm::OP_NE:
  case vm::OP_LAND:
  case vm::OP_LOR:
  case vm::OP_AND:
  case vm::OP_OR:
  case vm::OP_XOR:
  case vm::OP_CONCAT:
  case vm::OP_LSHIFT:
  case vm::OP_RSHIFT:
    {
      vm::Register *lhs = CompileExpr(expr->lhs_);
      vm::Register *rhs = CompileExpr(expr->rhs_);
      if (!lhs || !rhs) {
	return nullptr;
      }
      insn->src_regs_.push_back(lhs);
      insn->src_regs_.push_back(rhs);
    }
    break;
  case vm::OP_NUM:
    {
      // using same register for src/dst.
      reg->type_.value_type_ = vm::Value::NUM;
      reg->initial_num_ = expr->num_;
      reg->type_.is_const_ = true;
      // TODO: Handle the width sepcified with the number.
      // e.g. 32'd0
      reg->type_.width_ = numeric::Numeric::ValueWidth(expr->num_);
      reg->SetIsDeclaredType(true);
      insn->src_regs_.push_back(reg);
    }
    break;
  case vm::OP_STR:
    {
      reg->type_.value_type_ = vm::Value::OBJECT;
      reg->SetIsDeclaredType(true);
    }
    break;
  case vm::OP_BIT_INV:
  case vm::OP_LOGIC_INV:
  case vm::OP_PLUS:
  case vm::OP_MINUS:
    {
      vm::Register *val = CompileExpr(expr->args_);
      insn->src_regs_.push_back(val);
    }
    break;
  case vm::OP_BIT_RANGE:
    {
      vm::Register *val = CompileExpr(expr->args_);
      vm::Register *msb = CompileExpr(expr->lhs_);
      vm::Register *lsb = CompileExpr(expr->rhs_);
      insn->src_regs_.push_back(val);
      insn->src_regs_.push_back(msb);
      insn->src_regs_.push_back(lsb);
    }
    break;
  default:
    CHECK(false) << "Unknown expr:" << fe::NodeName(expr->type_);
    break;
  }
  EmitInsn(insn);
  return reg;
}

vm::OpCode Compiler::GetOpCodeFromExpr(fe::Expr *expr) {
  switch (expr->type_) {
  case fe::BINOP_ADD: return vm::OP_ADD;
  case fe::BINOP_SUB: return vm::OP_SUB;
  case fe::BINOP_MUL: return vm::OP_MUL;
  case fe::BINOP_GT: return vm::OP_GT;
  case fe::BINOP_LT: return vm::OP_LT;
  case fe::BINOP_GTE: return vm::OP_GTE;
  case fe::BINOP_LTE: return vm::OP_LTE;
  case fe::BINOP_EQ: return vm::OP_EQ;
  case fe::BINOP_NE: return vm::OP_NE;
  case fe::BINOP_LAND: return vm::OP_LAND;
  case fe::BINOP_LOR: return vm::OP_LOR;
  case fe::BINOP_AND: return vm::OP_AND;
  case fe::BINOP_OR: return vm::OP_OR;
  case fe::BINOP_XOR: return vm::OP_XOR;
  case fe::BINOP_CONCAT: return vm::OP_CONCAT;
  case fe::BINOP_LSHIFT: return vm::OP_LSHIFT;
  case fe::BINOP_RSHIFT: return vm::OP_RSHIFT;
  case fe::EXPR_NUM: return vm::OP_NUM;
  case fe::EXPR_STR: return vm::OP_STR;
  case fe::UNIOP_BIT_INV: return vm::OP_BIT_INV;
  case fe::UNIOP_LOGIC_INV: return vm::OP_LOGIC_INV;
  case fe::UNIOP_PLUS: return vm::OP_PLUS;
  case fe::UNIOP_MINUS: return vm::OP_MINUS;
  case fe::EXPR_BIT_RANGE: return vm::OP_BIT_RANGE;
  default:
    CHECK(false) << "Unknown expr:" << fe::NodeName(expr->type_);
  }
  return vm::OP_INVALID;
}


vm::Register *Compiler::CompileArrayRef(fe::Expr *expr) {
  vm::Register *index = CompileExpr(expr->rhs_);
  vm::Insn *insn = new vm::Insn;
  insn->op_ = vm::OP_ARRAY_READ;
  insn->src_regs_.push_back(index);
  insn->insn_expr_ = expr->lhs_;
  insn->dst_regs_.push_back(AllocRegister());

  vm::Register *array_reg = LookupLocalVar(expr->lhs_->sym_);
  if (array_reg) {
    // Local array
    insn->src_regs_.push_back(array_reg);
  } else {
    insn->obj_reg_ = CompileExpr(expr->lhs_);
  }

  EmitInsn(insn);
  return insn->dst_regs_[0];
}

vm::Register *Compiler::CompileComma(fe::Expr *expr) {
  vector<fe::Expr*> value_exprs;
  FlattenCommas(expr, &value_exprs);
  vm::Register *reg = nullptr;
  for (size_t i = 0; i < value_exprs.size(); ++i) {
    reg = CompileExpr(value_exprs[i]);
  }
  // Use the last value.
  return reg;
}

vm::Register *Compiler::CompileRef(fe::Expr *expr) {
  vm::Register *reg = AllocRegister();
  vm::Insn *insn = new vm::Insn;
  insn->insn_expr_ = expr;
  insn->dst_regs_.push_back(reg);
  if (IsTopLevel()) {
    insn->op_ = vm::OP_GENERIC_READ;
    CHECK(expr->args_->type_ == fe::EXPR_SYM);
    vm::Register *src = LookupLocalVar(expr->args_->sym_);
    if (src) {
      insn->src_regs_.push_back(src);
    } else {
      insn->obj_reg_ = EmitLoadObj(expr->args_->sym_);
    }
    insn->label_ = expr->args_->sym_;
  } else {
    vm::Value::ValueType type = GetVariableType(expr->args_->sym_);
    if (type == vm::Value::OBJECT) {
      insn->op_ = vm::OP_CHANNEL_READ;
      insn->obj_reg_ = EmitLoadObj(expr->args_->sym_);
    } else if (type == vm::Value::NUM) {
      insn->op_ = vm::OP_MEMORY_READ;
      vm::Register *val = CompileExpr(expr->args_);
      insn->src_regs_.push_back(val);
    } else {
      CHECK(false);
    }
  }
  EmitInsn(insn);
  return reg;
}

vm::Register *Compiler::CompileTriTerm(fe::Expr *expr) {
  vm::Register *cond = CompileExpr(expr->args_);
  vm::Register *res = AllocRegister();
  sym_t f_label = sym_alloc_tmp_sym("_f");
  vm::Insn *if_insn = new vm::Insn;
  if_insn->op_ = vm::OP_IF;
  if_insn->src_regs_.push_back(cond);
  if_insn->label_ = f_label;
  EmitInsn(if_insn);
  // LHS.
  vm::Register *lhs = CompileExpr(expr->lhs_);
  SimpleAssign(lhs, res);
  // Go to join.
  sym_t join_label = sym_alloc_tmp_sym("_join");
  vm::Insn *jump_insn = new vm::Insn;
  jump_insn->op_ = vm::OP_GOTO;
  jump_insn->label_ = join_label;
  EmitInsn(jump_insn);
  // RHS.
  AddLabel(f_label);
  vm::Register *rhs = CompileExpr(expr->rhs_);
  SimpleAssign(rhs, res);
  // Join.
  AddLabel(join_label);
  return res;
}

void Compiler::SimpleAssign(vm::Register *src, vm::Register *dst) {
  vm::Insn *insn = new vm::Insn;
  insn->op_ = vm::OP_ASSIGN;
  insn->src_regs_.push_back(dst);
  insn->src_regs_.push_back(src);
  insn->dst_regs_.push_back(dst);
  EmitInsn(insn);
}

vm::Register *Compiler::CompileElmRef(fe::Expr *expr) {
  vm::Register *obj_reg = CompileExpr(expr->lhs_);
  fe::Expr *rhs = expr->rhs_;
  if (rhs->type_ == fe::EXPR_SYM) {
    vm::Insn *ref_insn = new vm::Insn;
    ref_insn->op_ = vm::OP_MEMBER_READ;
    ref_insn->insn_expr_ = expr;
    ref_insn->label_ = expr->rhs_->sym_;
    vm::Register *res_reg = AllocRegister();
    res_reg->orig_name_ = expr->rhs_->sym_;
    ref_insn->dst_regs_.push_back(res_reg);
    ref_insn->src_regs_.push_back(obj_reg);
    EmitInsn(ref_insn);
    return res_reg;
  } else if (rhs->type_ == fe::EXPR_FUNCALL) {
    return CompileFuncallExpr(obj_reg, rhs);
  }
  CHECK(false);
  return nullptr;
}

vm::Register *Compiler::CompileAssign(fe::Expr *expr) {
  // Special pattern.
  // (x, y) = f(...)
  if (expr->rhs_->type_ == fe::EXPR_FUNCALL &&
      expr->lhs_->type_ == fe::BINOP_COMMA) {
    return CompileMultiValueFuncall(nullptr, expr->rhs_, expr->lhs_);
  }

  vm::Insn *insn = new vm::Insn;

  // RHS.
  vm::Register *rhs_reg = CompileExpr(expr->rhs_);
  if (!rhs_reg) {
    return nullptr;
  }
  if (expr->type_ != fe::BINOP_ASSIGN) {
    // now rhs corresponds lhs op= rhs.
    // update to lhs op rhs.
    rhs_reg = UpdateModifyOp(expr->type_, expr->lhs_, rhs_reg);
  }

  if (expr->lhs_->type_ == fe::UNIOP_REF) {
    return CompileAssignToUniopRef(insn, expr->lhs_, rhs_reg);
  } else if (expr->lhs_->type_ == fe::BINOP_ELM_REF) {
    return CompileAssignToElmRef(insn, expr->lhs_, rhs_reg);
  } else if (expr->lhs_->type_ == fe::BINOP_ARRAY_REF) {
    return CompileAssignToArray(insn, expr->lhs_, rhs_reg);
  } else if (expr->lhs_->type_ == fe::EXPR_SYM) {
    return CompileAssignToSym(insn, expr->lhs_, rhs_reg);
  } else {
    CHECK(false);
  }
  return nullptr;
}

vm::Register *Compiler::CompileAssignToArray(vm::Insn *insn, fe::Expr *lhs,
					     vm::Register *rhs_reg) {
  // SRC: INDEX RHS_REG {LOCAL_ARRAY}
  // DST: RHS_REG
  // OBJ: ARRAY
  vm::Register *local_array = nullptr;
  insn->op_ = vm::OP_ARRAY_WRITE;
  // index
  vm::Register *index_reg = CompileExpr(lhs->rhs_);
  fe::Expr *array_expr = lhs->lhs_;
  insn->insn_expr_ = array_expr;

  if (array_expr->type_ == fe::EXPR_SYM) {
    vm::Value *value = obj_->LookupValue(array_expr->sym_, false);
    if (!value) {
      local_array = LookupLocalVar(array_expr->sym_);
      CHECK(local_array || IsTopLevel()) << "undeclared local array";
    }
  }
  insn->src_regs_.push_back(index_reg);
  insn->src_regs_.push_back(rhs_reg);
  if (local_array) {
    insn->src_regs_.push_back(local_array);
  } else {
    if (array_expr->type_ == fe::EXPR_SYM) {
      insn->obj_reg_ = EmitMemberLoad(EmitLoadObj(nullptr), array_expr->sym_);
    } else {
      insn->obj_reg_ = CompileElmRef(array_expr);
    }
  }
  insn->dst_regs_.push_back(insn->src_regs_[1]);
  EmitInsn(insn);
  return insn->dst_regs_[0];
}

vm::Register *Compiler::CompileAssignToElmRef(vm::Insn *insn, fe::Expr *lhs,
					      vm::Register *rhs_reg) {
  vm::Register *lhs_obj = CompileExpr(lhs->lhs_);
  CHECK(lhs->rhs_->type_ == fe::EXPR_SYM);
  insn->op_ = vm::OP_MEMBER_WRITE;
  insn->label_ = lhs->rhs_->sym_;
  insn->obj_reg_ = lhs_obj;
  insn->src_regs_.push_back(rhs_reg);
  insn->src_regs_.push_back(lhs_obj);
  insn->dst_regs_.push_back(insn->src_regs_[0]);
  EmitInsn(insn);
  return insn->dst_regs_[0];
}

vm::Register *Compiler::CompileAssignToSym(vm::Insn *insn, fe::Expr *lhs,
					   vm::Register *rhs_reg) {
  // local var
  //  SRC: LHS_REG, RHS_REG
  //  DST: LHS_REG
  // member var
  //  SRC: RHS_REG, OBJ_REG
  //  DST: RHS_REG
  vm::Register *lhs_reg = LookupLocalVar(lhs->sym_);
  vm::Register *obj_reg = nullptr;
  insn->label_ = lhs->sym_;
  if (lhs_reg) {
    insn->op_ = vm::OP_ASSIGN;
    insn->src_regs_.push_back(lhs_reg);
  } else {
    obj_reg = EmitLoadObj(nullptr);
    insn->op_ = vm::OP_MEMBER_WRITE;
  }
  insn->src_regs_.push_back(rhs_reg);
  insn->dst_regs_.push_back(insn->src_regs_[0]);
  if (obj_reg) {
    insn->src_regs_.push_back(obj_reg);
  }
  EmitInsn(insn);
  return insn->dst_regs_[0];
}

vm::Register *Compiler::CompileAssignToUniopRef(vm::Insn *insn, fe::Expr *lhs,
						vm::Register *rhs_reg) {
  // LHS can be either an address or a channel.
  vm::Register *lhs_reg = CompileRefLhsExpr(lhs, insn);
  if (lhs_reg) {
    insn->src_regs_.push_back(lhs_reg);
  }
  insn->src_regs_.push_back(rhs_reg);
  insn->dst_regs_.push_back(insn->src_regs_[0]);
  EmitInsn(insn);
  return rhs_reg;
}

vm::Register *Compiler::CompileRefLhsExpr(fe::Expr *lhs_expr, vm::Insn *insn) {
  insn->insn_expr_ = lhs_expr;
  // Does't support: *(x.y) = ...; for now.
  CHECK(lhs_expr->args_->type_ == fe::EXPR_SYM)
    << fe::NodeName(lhs_expr->type_);
  sym_t name = lhs_expr->args_->sym_;
  if (IsTopLevel()) {
    insn->op_ = vm::OP_GENERIC_WRITE;
    vm::Register *src = LookupLocalVar(lhs_expr->args_->sym_);
    if (src) {
      insn->src_regs_.push_back(src);
    } else {
      insn->obj_reg_ = EmitLoadObj(lhs_expr->args_->sym_);
    }
    insn->label_ = name;
    return nullptr;
  } else {
    // Determines if it is a channel or memory.
    vm::Value::ValueType type = GetVariableType(name);
    if (type == vm::Value::OBJECT) {
      insn->op_ = vm::OP_CHANNEL_WRITE;
      insn->obj_reg_ = EmitLoadObj(lhs_expr->args_->sym_);
      return AllocRegister();
    } else if (type == vm::Value::NUM) {
      insn->op_ = vm::OP_MEMORY_WRITE;
      return CompileExpr(lhs_expr->args_);
    } else {
      CHECK(false);
    }
  }
  return nullptr;
}


vm::Register *Compiler::UpdateModifyOp(fe::NodeCode type, fe::Expr *lhs_expr,
				       vm::Register *rhs_reg) {
  vm::Register *lhs_reg = CompileExpr(lhs_expr);
  vm::Insn *insn = new vm::Insn;
  if (type == fe::BINOP_ADD_ASSIGN) {
    insn->op_ = vm::OP_ADD;
  } else if (type == fe::BINOP_SUB_ASSIGN) {
    insn->op_ = vm::OP_SUB;
  } else if (type == fe::BINOP_MUL_ASSIGN) {
    insn->op_ = vm::OP_MUL;
  } else if (type == fe::BINOP_LSHIFT_ASSIGN) {
    insn->op_ = vm::OP_LSHIFT;
  } else if (type == fe::BINOP_RSHIFT_ASSIGN) {
    insn->op_ = vm::OP_RSHIFT;
  } else if (type == fe::BINOP_AND_ASSIGN) {
    insn->op_ = vm::OP_AND;
  } else if (type == fe::BINOP_OR_ASSIGN) {
    insn->op_ = vm::OP_OR;
  } else if (type == fe::BINOP_XOR_ASSIGN) {
    insn->op_ = vm::OP_XOR;
  } else {
    CHECK(false);
  }
  vm::Register *reg = AllocRegister();
  insn->dst_regs_.push_back(reg);
  insn->src_regs_.push_back(lhs_reg);
  insn->src_regs_.push_back(rhs_reg);
  EmitInsn(insn);
  return reg;
}

vm::Register *Compiler::LookupLocalVar(sym_t name) {
  for (int i = bindings_.size() - 1; i >= 0; --i) {
    VarScope *scope = bindings_[i];
    vm::Register *reg = scope->local_regs_[name];
    if (reg) {
      return reg;
    }
  }
  return nullptr;
}

vm::Register *Compiler::CompileSymExpr(fe::Expr *expr) {
  sym_t name = expr->sym_;
  vm::Register *reg = LookupLocalVar(name);
  if (reg) {
    return reg;
  }

  return CompileMemberSym(expr);
}

vm::Register *Compiler::CompileMemberSym(fe::Expr *expr) {
  vm::Register *obj_reg = EmitLoadObj(nullptr);

  vm::Insn *ref_insn = new vm::Insn;
  ref_insn->src_regs_.push_back(obj_reg);
  ref_insn->op_ = vm::OP_MEMBER_READ;
  ref_insn->insn_expr_ = expr;
  ref_insn->label_ = expr->sym_;
  vm::Register *reg = AllocRegister();
  reg->orig_name_ = expr->sym_;
  ref_insn->dst_regs_.push_back(reg);
  EmitInsn(ref_insn);

  vm::Value *value = obj_->LookupValue(expr->sym_, false);
  if (value) {
    vm::InsnAnnotator::AnnotateByValue(value, reg);
  }

  return reg;
}

void Compiler::CompileFuncDecl(fe::Stmt *stmt) {
  vm::Insn *insn = new vm::Insn;
  insn->op_ = vm::OP_FUNCDECL;
  insn->label_ = stmt->expr_->sym_;
  insn->insn_stmt_ = stmt;
  insn->obj_reg_ = CompilePathHead(stmt->expr_);
  EmitInsn(insn);
}

vm::Register *Compiler::CompilePathHead(fe::Expr *path_elem) {
  if (path_elem->type_ == fe::EXPR_SYM) {
    return EmitLoadObj(nullptr);
  }
  return TraverseMemberPath(path_elem->args_);
}

vm::Register *Compiler::TraverseMemberPath(fe::Expr *path_elem) {
  if (path_elem->type_ == fe::EXPR_SYM) {
    vm::Register *obj_reg = LookupLocalVar(path_elem->sym_);
    if (obj_reg == nullptr) {
      obj_reg = EmitLoadObj(path_elem->sym_);
    }
    return obj_reg;
  }
  vm::Register *obj_reg = TraverseMemberPath(path_elem->args_);
  return EmitMemberLoad(obj_reg, path_elem->sym_);
}

vm::Register *Compiler::CompileFuncallExpr(vm::Register *obj, fe::Expr *expr) {
  return CompileMultiValueFuncall(obj, expr, nullptr);
}

vm::Register *Compiler::CompileMultiValueFuncall(vm::Register *obj,
						 fe::Expr *funcall,
						 fe::Expr *lhs) {
  vm::Insn *call_insn = new vm::Insn;
  call_insn->op_ = vm::OP_FUNCALL;
  call_insn->insn_expr_ = funcall;
  if (obj == nullptr) {
    call_insn->obj_reg_ = CompilePathHead(funcall->func_);
  } else {
    call_insn->obj_reg_ = obj;
  }
  call_insn->label_ = funcall->func_->sym_;

  vector<fe::Expr*> args;
  FlattenCommas(funcall->args_, &args);
  for (size_t i = 0; i < args.size(); ++i) {
    vm::Register *reg = CompileExpr(args[i]);
    if (!reg) {
      return nullptr;
    }
    call_insn->src_regs_.push_back(reg);
  }
  EmitInsn(call_insn);

  vm::Insn *done_insn = new vm::Insn;
  done_insn->op_ = vm::OP_FUNCALL_DONE;
  done_insn->insn_expr_ = funcall;
  done_insn->obj_reg_ = call_insn->obj_reg_;
  done_insn->label_ = call_insn->label_;
  if (lhs) {
    vector<fe::Expr*> values;
    FlattenCommas(lhs, &values);
    for (size_t i = 0; i < values.size(); ++i) {
      vm::Register *reg = CompileSymExpr(values[i]);
      done_insn->dst_regs_.push_back(reg);
    }
    EmitInsn(done_insn);
    return nullptr;
  } else {
    // This can be a dummy value, if the callee return void.
    vm::Register *reg = AllocRegister();
    reg->type_.value_type_ = vm::Value::NUM;
    done_insn->dst_regs_.push_back(reg);
    EmitInsn(done_insn);
    return reg;
  }
}

vm::Register *Compiler::AllocRegister() {
  vm::Register *reg = new vm::Register;
  reg->id_ = method_->method_regs_.size();
  method_->method_regs_.push_back(reg);
  return reg;
}

int Compiler::InsnIndexFromLabel(sym_t label) {
  vm::Insn *insn = label_insn_map_[label];
  CHECK(insn) << sym_cstr(label);
  // TODO(yusuke): don't do a linear search.
  for (size_t i = 0; i < method_->insns_.size(); ++i) {
    if (method_->insns_[i] == insn) {
      return i + 1;
    }
  }
  CHECK(false);
  return -1;
}

vm::Register *Compiler::EmitLoadObj(sym_t label) {
  vm::Insn *obj_insn = new vm::Insn;
  obj_insn->op_ = vm::OP_LOAD_OBJ;
  vm::Register *obj_reg = AllocRegister();
  obj_insn->dst_regs_.push_back(obj_reg);
  obj_insn->label_ = label;
  EmitInsn(obj_insn);
  return obj_reg;
}

vm::Register *Compiler::EmitMemberLoad(vm::Register *obj_reg, sym_t m) {
  vm::Insn *insn = new vm::Insn;
  insn->src_regs_.push_back(obj_reg);
  insn->op_ = vm::OP_MEMBER_READ;
  insn->label_ = m;
  vm::Register *value_reg = AllocRegister();
  insn->dst_regs_.push_back(value_reg);
  EmitInsn(insn);
  return value_reg;
}

bool Compiler::IsTopLevel() const {
  return method_->IsTopLevel();
}

}  // namespace compiler
