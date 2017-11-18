// -*- C++ -*-
#ifndef _fe_expr_h_
#define _fe_expr_h_

#include "fe/common.h"
#include "fe/nodecode.h"
#include "numeric/numeric_type.h"  // from iroha

namespace fe {

class Expr {
public:
  explicit Expr(NodeCode type);

  enum NodeCode GetType() const;

  void Dump();
  void Dump(DumpStream &os);

  iroha::Numeric num_;
  sym_t sym_;
  string str_;

  Expr *func_;
  Expr *args_;
  Expr *lhs_;
  Expr *rhs_;

private:
  NodeCode type_;
};

// mainly for multiple var decls (e.g. var x, y int).
class ExprSet {
public:
  void Dump(DumpStream &os);
  vector<Expr*> exprs;
};

}  // namespace fe

#endif  // _fe_expr_h_
