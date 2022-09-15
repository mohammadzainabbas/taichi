#include "taichi/ir/ir.h"
#include "taichi/ir/statements.h"
#include "taichi/ir/transforms.h"
#include "taichi/ir/visitors.h"
#include "taichi/system/profiler.h"

TLANG_NAMESPACE_BEGIN

class Scalarize : public IRVisitor {
 public:
  Scalarize(IRNode *node) {
    allow_undefined_visitor = true;
    invoke_default_visitor = false;
    node->accept(this);
  }

  /*
    "val" of StoreStmt should have already been replaced by a MatrixInitStmt in
    former scalarization.

    Before:
      StoreStmt(TensorType<4 x i32>* dest, TensorType<4 x i32> val)

    After:
      addr0 = PtrOffsetStmt(TensorType<4 x i32>* dest, 0)
      addr1 = PtrOffsetStmt(TensorType<4 x i32>* dest, 1)
      addr2 = PtrOffsetStmt(TensorType<4 x i32>* dest, 2)
      addr2 = PtrOffsetStmt(TensorType<4 x i32>* dest, 3)

      StoreStmt(i32* addr0, i32 val->cast<MatrixInitStmt>()->val[0])
      StoreStmt(i32* addr1, i32 val->cast<MatrixInitStmt>()->val[1])
      StoreStmt(i32* addr2, i32 val->cast<MatrixInitStmt>()->val[2])
      StoreStmt(i32* addr3, i32 val->cast<MatrixInitStmt>()->val[3])
  */
  template <typename T>
  void scalarize_store_stmt(T *stmt) {
    auto dest_dtype = stmt->dest->ret_type.ptr_removed();
    auto val_dtype = stmt->val->ret_type;
    if (dest_dtype->template is<TensorType>() &&
        val_dtype->template is<TensorType>()) {
      // Needs scalarize
      auto dest_tensor_type = dest_dtype->template as<TensorType>();
      auto val_tensor_type = val_dtype->template as<TensorType>();
      TI_ASSERT(dest_tensor_type->get_shape() == val_tensor_type->get_shape());
      TI_ASSERT(dest_tensor_type->get_element_type() ==
                val_tensor_type->get_element_type());

      TI_ASSERT(stmt->val->template is<MatrixInitStmt>());
      auto matrix_init_stmt = stmt->val->template as<MatrixInitStmt>();

      int num_elements = val_tensor_type->get_num_elements();
      for (int i = 0; i < num_elements; i++) {
        auto const_stmt = std::make_unique<ConstStmt>(
            TypedConstant(stmt->val->ret_type.get_element_type(), i));

        auto ptr_offset_stmt =
            std::make_unique<PtrOffsetStmt>(stmt->dest, const_stmt.get());
        auto scalarized_stmt = std::make_unique<T>(ptr_offset_stmt.get(),
                                                   matrix_init_stmt->values[i]);

        stmt->insert_before_me(std::move(const_stmt));
        stmt->insert_before_me(std::move(ptr_offset_stmt));
        stmt->insert_before_me(std::move(scalarized_stmt));
      }
      stmt->parent->erase(stmt);
    }
  }

  void visit(Block *stmt_list) override {
    for (auto &stmt : stmt_list->statements) {
      stmt->accept(this);
    }
  }

  void visit(IfStmt *if_stmt) override {
    if (if_stmt->true_statements)
      if_stmt->true_statements->accept(this);
    if (if_stmt->false_statements) {
      if_stmt->false_statements->accept(this);
    }
  }

  void visit(WhileStmt *stmt) override {
    stmt->body->accept(this);
  }

  void visit(RangeForStmt *for_stmt) override {
    for_stmt->body->accept(this);
  }

  void visit(StructForStmt *for_stmt) override {
    for_stmt->body->accept(this);
  }

  void visit(MeshForStmt *for_stmt) override {
    for_stmt->body->accept(this);
  }

  void visit(OffloadedStmt *stmt) override {
    stmt->all_blocks_accept(this);
  }

  void visit(GlobalStoreStmt *stmt) override {
    scalarize_store_stmt<GlobalStoreStmt>(stmt);
  }

  void visit(LocalStoreStmt *stmt) override {
    scalarize_store_stmt<LocalStoreStmt>(stmt);
  }
};

namespace irpass {

void scalarize(IRNode *root) {
  TI_AUTO_PROF;
  Scalarize scalarize_pass(root);

  /* TODO(zhanlue): Remove redundant MatrixInitStmt
    Scalarize pass will generate temporary MatrixInitStmts, which are only used
    as rvalues. Remove these MatrixInitStmts since it's no longer needed.
  */
}

}  // namespace irpass

TLANG_NAMESPACE_END