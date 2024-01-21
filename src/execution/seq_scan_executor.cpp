//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// seq_scan_executor.cpp
//
// Identification: src/execution/seq_scan_executor.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/seq_scan_executor.h"

namespace bustub {

SeqScanExecutor::SeqScanExecutor(ExecutorContext *exec_ctx, const SeqScanPlanNode *plan)
    : AbstractExecutor(exec_ctx), plan_(plan) {}

void SeqScanExecutor::Init() {
  table_iterator_ =
      std::make_unique<TableIterator>(exec_ctx_->GetCatalog()->GetTable(plan_->GetTableOid())->table_->MakeIterator());
}

auto SeqScanExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  TupleMeta tuple_meta;
  do {
    if (table_iterator_->IsEnd()) {
      return EXECUTOR_EXHAUSTED;
    }

    auto tuple_pair = table_iterator_->GetTuple();
    tuple_meta = tuple_pair.first;
    *tuple = tuple_pair.second;
    *rid = table_iterator_->GetRID();
    ++(*table_iterator_);
  } while (tuple_meta.is_deleted_);

  return EXECUTOR_ACTIVE;
}

}  // namespace bustub
