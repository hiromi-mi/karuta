#include "fe/method.h"

#include "base/dump_stream.h"
#include "fe/stmt.h"
#include "fe/var_decl.h"

namespace fe {

Method::Method(const string &name)
  : name_(name), args_(nullptr), returns_(nullptr), annotation_(nullptr),
    is_always_(false) {
}

void Method::Dump() const {
  DumpStream ds(cout);
  Dump(ds);
}

void Method::Dump(DumpStream &ds) const {
  for (size_t i = 0; i < stmts_.size(); ++i) {
    stmts_[i]->Dump(ds);
  }
  if (args_) {
    ds.indent();
    ds.os << "args:\n";
    ds.push_indent();
    args_->Dump(ds);
    ds.pop_indent();
  }
  if (returns_) {
    ds.indent();
    ds.os << "returns:\n";
    ds.push_indent();
    returns_->Dump(ds);
    ds.pop_indent();
  }
}

const string &Method::GetName() const {
  return name_;
}

const vector<Stmt*> &Method::GetStmts() const {
  return stmts_;
}

vector<Stmt*> &Method::GetMutableStmts() {
  return stmts_;
}

VarDeclSet *Method::GetArgs() const {
  return args_;
}

void Method::SetArgs(VarDeclSet *args) {
  args_ = args;
}

VarDeclSet *Method::GetReturns() const {
  return returns_;
}

void Method::SetReturns(VarDeclSet *returns) {
  returns_ = returns;
}

void Method::SetAnnotation(Annotation *an) {
  annotation_ = an;
}

Annotation *Method::GetAnnotation() const {
  return annotation_;
}

bool Method::GetIsAlways() const {
  return is_always_;
}

void Method::SetIsAlways(bool is_always) {
  is_always_ = is_always;
}

}  // namespace fe
