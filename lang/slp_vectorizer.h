#pragma once

#include "expr.h"
#include "visitor.h"

TLANG_NAMESPACE_BEGIN

#if(0)
class SLPVectorizer : public Visitor {
 public:
  std::map<Expr, Expr> scalar_to_vector;
  int group_size;

  SLPVectorizer() : Visitor(Visitor::Order::parent_first) {
  }

  void sort(Expr &expr);

  Expr run(Expr &expr, int group_size);

  void visit(Expr &expr) override;
};
#endif

TLANG_NAMESPACE_END
