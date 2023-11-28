#include <sstream>
#include <string>

#include "common/exception.h"
#include "common/logger.h"
#include "common/rid.h"
#include "storage/index/b_plus_tree.h"

namespace bustub {

INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, page_id_t header_page_id, BufferPoolManager *buffer_pool_manager,
                          const KeyComparator &comparator, int leaf_max_size, int internal_max_size)
    : index_name_(std::move(name)),
      bpm_(buffer_pool_manager),
      comparator_(std::move(comparator)),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size),
      header_page_id_(header_page_id) {
  // record the root page
  WritePageGuard guard = bpm_->FetchPageWrite(header_page_id_);
  auto header_page = guard.AsMut<BPlusTreeHeaderPage>();
  header_page->root_page_id_ = INVALID_PAGE_ID;
}

/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const -> bool {
  ReadPageGuard guard = bpm_->FetchPageRead(header_page_id_);
  auto header_page = guard.As<BPlusTreeHeaderPage>();
  if (header_page->root_page_id_ == INVALID_PAGE_ID) {
    return true;
  }
  
  guard = bpm_->FetchPageRead(header_page->root_page_id_);
  const InternalPage* root_page = guard.As<InternalPage>();

  if (root_page->IsLeafPage()) {
    return (root_page->GetSize() == 0);
  }
  return root_page->GetSize() <= 1; // TODO: Can root be one value internal page?
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetValue(const KeyType &key, std::vector<ValueType> *result, Transaction *txn) -> bool {
  BasicPageGuard guard = bpm_->FetchPageBasic(header_page_id_);
  auto header_page = guard.As<BPlusTreeHeaderPage>();
  if (header_page->root_page_id_ == INVALID_PAGE_ID) {
    return false;
  }
  
  guard = bpm_->FetchPageBasic(header_page->root_page_id_);
  InternalPage* root = guard.AsMut<InternalPage>();

  while (true) {
    if (root->IsLeafPage()) { // 如果到了leaf这一层
      // 此时guard跟root指向的是同一页，这样做是安全的
      auto leaf = guard.As<LeafPage>();
      for (int i = 0; i < leaf->GetSize(); i ++) { // 对于leaf page，需要从0开始遍历key map
        if (comparator_(key, leaf->KeyAt(i)) == 0) { // 找到target
          result->push_back(static_cast<ValueType>(leaf->ValueAt(i)));
          return true;
        }
      }
      return false; // 未找到target
    }

    // TODO: 这里可以合起来，不用flag判断。但我现在有点懒，之后再干。
    bool flag = false;
    for (int i = 1; i < root->GetSize(); i ++) { // 对于internal page，需要从1开始遍历key map
      if (comparator_(key, root->KeyAt(i)) < 0) { // 如果target < 当前key，说明target在该key左边
        guard = bpm_->FetchPageBasic(root->ValueAt(i - 1));
        root = guard.AsMut<InternalPage>(); // 去左孩子处
        flag = true;
        break;
      }
    }
    if (!flag) {
      guard = bpm_->FetchPageBasic(root->ValueAt(root->GetSize() - 1));
      root = guard.AsMut<InternalPage>();// 去最右孩子处
    }
  }
  return false;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value, Transaction *txn) -> bool {
  // Declaration of context instance.
  Context ctx;

  // 获取header page的写锁
  ctx.header_page_ = std::move(bpm_->FetchPageWrite(header_page_id_));
  // 获取root page id
  auto header_page = ctx.header_page_.value().AsMut<BPlusTreeHeaderPage>();
  ctx.root_page_id_ = header_page->root_page_id_;

  // b+树为空
  if (ctx.root_page_id_ == INVALID_PAGE_ID) {
    // TODO: current tree empty，应该新申请一页，update root page id
    bpm_->NewPageGuarded(&(ctx.root_page_id_));
    header_page->root_page_id_ = ctx.root_page_id_;
    
    // 新建root
    auto guard = bpm_->FetchPageWrite(ctx.root_page_id_);
    LeafPage* root = guard.AsMut<LeafPage>();// 注意，初始根节点为叶结点，之后键的根节点才为非叶结点。
    root->Init(leaf_max_size_);
    // 插入新值
    root->IncreaseSize(1);
    root->SetKeyAt(0, key);
    root->SetValueAt(0, value);

    // 手动释放，不过应该没必要
    ctx.header_page_ = std::nullopt;
    return true;
  }

  BUSTUB_ASSERT(ctx.root_page_id_ != INVALID_PAGE_ID, "root page id should be valid.");

  // 获取root page
  auto guard = bpm_->FetchPageWrite(ctx.root_page_id_);
  InternalPage* root = guard.AsMut<InternalPage>();
  ctx.write_set_.push_back(std::move(guard)); 
 
  // 分裂思想：旧结点连线无需改变，只需增加一个父新结点+其与新节点之间的连线即可
  
  // 获取目标叶结点
  while (true) {
    if (root->IsLeafPage()) { // 如果到了leaf这一层
      break;
    }
    bool flag = false;
    for (int i = 1; i < root->GetSize(); i++) { // 对于internal page，需要从1开始遍历key map
      if (comparator_(key, root->KeyAt(i)) < 0) { // 如果target<当前key，说明target在该key左边
        guard = std::move(bpm_->FetchPageWrite(root->ValueAt(i - 1)));
        root = guard.AsMut<InternalPage>(); // 去左孩子处
        ctx.write_set_.push_back(std::move(guard));
        flag = true;
        break;
      }
    }
    if (!flag) {
      guard = std::move(bpm_->FetchPageWrite(root->ValueAt(root->GetSize() - 1)));
      root = guard.AsMut<InternalPage>();// 去最右孩子处
      ctx.write_set_.push_back(std::move(guard));
    }
  }

  auto leaf_guard = std::move(ctx.write_set_.back());
  auto leaf = leaf_guard.AsMut<LeafPage>();
  if (!(ctx.write_set_.empty())) {
    ctx.write_set_.pop_back();// 上面的迭代过程最后会把目标叶结点push进去，所以要先把pop出来
  }

  // 先检查是否已存在再插入
  for (int i = 0; i < leaf->GetSize(); i++) { // 对于leaf page，需要从0开始遍历key map
    if (comparator_(key, leaf->KeyAt(i)) == 0) { // 对应key已存在
      ctx.write_set_.clear();
      ctx.header_page_ = std::nullopt;
      return false;
    }
  }

  // 需要进行切分
  bool should_split = (leaf->GetSize() == leaf->GetMaxSize());
  KeyType tmp_key;
  page_id_t page_id;
  int m;

  KeyType insert_key;
  page_id_t insert_val;

  if (leaf->GetSize() == leaf->GetMaxSize()) {
    m = leaf->GetSize();

    // 记住m/2上取整的key
    tmp_key = leaf->KeyAt((m + 1) / 2);
    insert_key = tmp_key;

    /* 先分成两个结点 */

    // 创建新结点,记得map为<key,page_id>
    bpm_->NewPageGuarded(&page_id);
    insert_val = page_id;
    auto new_page_guard = bpm_->FetchPageWrite(page_id);
    auto new_page = new_page_guard.AsMut<LeafPage>();
    new_page->Init(leaf_max_size_);
    // 将旧结点的后半部分（m/2下取整）匀给新结点
    new_page->IncreaseSize(m / 2);
    int idx = 0;
    for (int i = (m + 1) / 2; i < m; i ++) { // 对于那个要被移到父节点处的那个东西，它本来牵着的page_id会被转移到新结点的key0处
      new_page->SetKeyAt(idx, leaf->KeyAt(i));
      new_page->SetValueAt(idx, leaf->ValueAt(i));
      idx ++;
    }
    // 缩小旧结点
    // 由于新root的结点也要保留在旧结点中，所以这里减m/2就行
    leaf->IncreaseSize(-m/2);
    // 在链表中连起来
    new_page->SetNextPageId(leaf->GetNextPageId());
    leaf->SetNextPageId(page_id);
    // 最终向新节点插入new record，所以我们修改了leaf变量的引用
    leaf = std::move(new_page);

    // 获取当前leaf结点的父节点root
    WritePageGuard root_guard;
    
    if(ctx.write_set_.empty()) {
      // 创建并初始化根节点
      page_id_t new_root_page_id;
      bpm_->NewPageGuarded(&new_root_page_id);
      auto new_root_guard = bpm_->FetchPageWrite(new_root_page_id);
      auto new_root = new_root_guard.AsMut<InternalPage>();
      new_root->Init(internal_max_size_);

      new_root->SetValueAt(0, ctx.root_page_id_);// 注意，设置key0对于value指向旧结点

      auto header_page = ctx.header_page_.value().AsMut<BPlusTreeHeaderPage>();
      header_page->root_page_id_ = new_root_page_id;
      ctx.root_page_id_ = new_root_page_id;

      new_root->IncreaseSize(1);

      // 将中间部分的那个结点插入到父亲中
      new_root->SetKeyAt(1, insert_key);
      new_root->SetValueAt(1, insert_val);

      should_split = false;
    } else {
      // 否则，置为back()，并且pop_back()。
      root_guard = std::move(ctx.write_set_.back()); // 弹出父亲
      ctx.write_set_.pop_back();

      root = root_guard.AsMut<InternalPage>();
    }
  }

  for (int i = 0; i <= leaf->GetSize(); i ++) {
    if (i != leaf->GetSize() && comparator_(key, leaf->KeyAt(i)) > 0) {
      continue;
    }
    // 插入到i-1的位置
    leaf->IncreaseSize(1);
    for (int j = leaf->GetSize() - 1; j >= i + 1; j --) {
      leaf->SetKeyAt(j, leaf->KeyAt(j - 1));
      leaf->SetValueAt(j, leaf->ValueAt(j - 1));
    }
    leaf->SetKeyAt(i, key);
    leaf->SetValueAt(i, value);
    break;
  }

  if (!should_split) {
    goto INSERTION_END;
  }

  // 向上生长处理
  do {
    if (root->GetSize() < root->GetMaxSize()) {
      // 将中间部分的那个结点copy插入到父亲中
      for (int i = 1; i <= root->GetSize(); i++) { // 注意，root必为internal node，所以需要从1开始遍历
        if (i != root->GetSize() && comparator_(insert_key, root->KeyAt(i)) > 0) {
          continue;
        }
        // 插入到i-1的位置
        root->IncreaseSize(1);
        for (int j = root->GetSize() - 1; j >= i + 1; j --) {
          root->SetKeyAt(j, root->KeyAt(j-1));
          root->SetValueAt(j, root->ValueAt(j-1));
        }
        root->SetKeyAt(i, insert_key);
        root->SetValueAt(i, insert_val);// 注意指向new leaf page
        break;
      }
      goto INSERTION_END;
    }

    if (ctx.write_set_.empty()) {
      break;
    }

    /* 需要进行分裂 */
    // 获取当前结点root的parent
    auto parent_guard = std::move(ctx.write_set_.back());
    InternalPage* parent = parent_guard.AsMut<InternalPage>();
    ctx.write_set_.pop_back();

    // 分割结点
    m = root->GetSize();
    // 记住m/2上取整的key
    KeyType tmp_key = root->KeyAt((m + 1) / 2);

    // 先分成两个结点
    // 创建新结点,记得map为<key,page_id>
    page_id_t page_id;
    bpm_->NewPageGuarded(&page_id);
    auto new_page_guard = bpm_->FetchPageWrite(page_id);
    auto new_page = new_page_guard.AsMut<InternalPage>();
    new_page->Init(internal_max_size_);
    // 将旧结点的后半部分匀给新结点
    new_page->IncreaseSize(m / 2);// key0指向空
    int idx = 1; // 注意，从1开始
    for (int i = (m + 1) / 2; i < m; i ++) { // 对于那个要被移到父节点处的那个东西，它本来牵着的page_id会被转移到新结点的key0处
      new_page->SetKeyAt(idx, root->KeyAt(i));
      new_page->SetValueAt(idx, root->ValueAt(i));
      idx ++;
    }
    // 缩小旧结点
    root->IncreaseSize(-m/2);

    for (int i = 1; i <= new_page->GetSize(); i ++) {
      if (i != new_page->GetSize() && comparator_(insert_key, new_page->KeyAt(i)) > 0) {
        continue;
      }
      // 插入到i-1的位置
      new_page->IncreaseSize(1);
      for (int j = new_page->GetSize() - 1; j >= i + 1; j --) {
        new_page->SetKeyAt(j, new_page->KeyAt(j - 1));
        new_page->SetValueAt(j, new_page->ValueAt(j - 1));
      }
      new_page->SetKeyAt(i, insert_key);
      new_page->SetValueAt(i, insert_val);
      break;
    }

    root = parent;
    insert_key = tmp_key;
    insert_val = page_id;
  } while (!(ctx.write_set_.empty()));

  if(root->GetSize() == root->GetMaxSize()) {
    // root should be splited
    // 创建并初始化根节点
    page_id_t new_root_page_id;
    bpm_->NewPageGuarded(&new_root_page_id);
    auto new_root_guard = bpm_->FetchPageWrite(new_root_page_id);
    auto new_root = new_root_guard.AsMut<InternalPage>();
    new_root->Init(internal_max_size_);

    new_root->SetValueAt(0, ctx.root_page_id_);// 注意，设置key0对于value指向旧结点

    auto header_page = ctx.header_page_.value().AsMut<BPlusTreeHeaderPage>();
    header_page->root_page_id_ = new_root_page_id;
    ctx.root_page_id_ = new_root_page_id;

    new_root->IncreaseSize(1);
    // 分割结点
    // 创建新结点,记得map为<key,page_id>
    m = root->GetSize();

    // 记住m/2上取整的key和value
    KeyType tmp_key = root->KeyAt((m + 1) / 2);

    page_id_t page_id;
    bpm_->NewPageGuarded(&page_id);
    auto new_page_guard = bpm_->FetchPageWrite(page_id);
    auto new_page = new_page_guard.AsMut<InternalPage>();
    new_page->Init(internal_max_size_);
    // 将旧结点的后半部分匀给新结点.这个+1为第0个key的占位符
    new_page->IncreaseSize(m / 2);// key0指向空
    int idx = 1;
    for (int i = (m + 1) / 2; i < m; i ++) { // 对于那个要被移到父节点处的那个东西，它本来牵着的page_id会被转移到新结点的key0处
      new_page->SetKeyAt(idx, root->KeyAt(i));
      new_page->SetValueAt(idx, root->ValueAt(i));
      idx ++;
    }
    // 缩小旧结点
    root->IncreaseSize(-m/2);

    for (int i = 1; i <= new_page->GetSize(); i ++) {
      if (i != new_page->GetSize() && comparator_(insert_key, new_page->KeyAt(i)) > 0) {
        continue;
      }
      // 插入到i-1的位置
      new_page->IncreaseSize(1);
      for (int j = new_page->GetSize() - 1; j >= i + 1; j --) {
        new_page->SetKeyAt(j, new_page->KeyAt(j - 1));
        new_page->SetValueAt(j, new_page->ValueAt(j - 1));
      }
      new_page->SetKeyAt(i, insert_key);
      new_page->SetValueAt(i, insert_val);
      break;
    }

    // 将中间部分的那个结点插入到父亲中
    // new_root->SetKeyAt(new_page->GetSize()-1,tmp_key);
    new_root->SetKeyAt(1, tmp_key);
    new_root->SetValueAt(1, page_id);
  }

INSERTION_END:
  // 释放header page的写锁
  ctx.write_set_.clear();
  ctx.header_page_ = std::nullopt;
  return true;
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType &key, Transaction *txn) {
  // Declaration of context instance.
  Context ctx;
  (void)ctx;
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the leftmost leaf page first, then construct
 * index iterator
 * @return : index iterator
 思路：找到最左叶结点对应的pageid然后return一个new的iterator对象。
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin() -> INDEXITERATOR_TYPE { 
  // 获取root page
  BasicPageGuard guard = bpm_->FetchPageBasic(header_page_id_);
  auto header_page = guard.As<BPlusTreeHeaderPage>();
  auto res_pgid = header_page->root_page_id_;
  if (header_page->root_page_id_ == INVALID_PAGE_ID) {
    return INDEXITERATOR_TYPE(bpm_, INVALID_PAGE_ID); // TODO: 错误处理有待斟酌
  }
  
  guard = bpm_->FetchPageBasic(header_page->root_page_id_);
  InternalPage* root = guard.AsMut<InternalPage>();

  while (true) {
    if (root->IsLeafPage()) { // 如果到了leaf这一层
      return INDEXITERATOR_TYPE(bpm_, res_pgid);
    }

    res_pgid = root->ValueAt(0);
    guard = bpm_->FetchPageBasic(root->ValueAt(0));
    root = guard.AsMut<InternalPage>();
  }

  BUSTUB_ASSERT(false, "b+ tree Begin() wrong!\n");

  // theorily unreachable
  return INDEXITERATOR_TYPE(bpm_, INVALID_PAGE_ID); // TODO: 错误处理有待斟酌
}

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 思路：找到key对应的pageid然后return一个new的iterator对象。
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType &key) -> INDEXITERATOR_TYPE {
  // 想了想还是直接从Begin开始迭代更帅
  auto it = Begin();
  while (!(it.IsEnd() || comparator_((*it).first, key) == 0)) {
    ++ it;
  }
  return it;

  // // 获取root page
  // BasicPageGuard guard = bpm_->FetchPageBasic(header_page_id_);
  // auto header_page = guard.As<BPlusTreeHeaderPage>();
  // auto res_pgid = header_page->root_page_id_;
  // if (header_page->root_page_id_ == INVALID_PAGE_ID) {
  //   return INDEXITERATOR_TYPE(bpm_, INVALID_PAGE_ID); // TODO: 错误处理有待斟酌
  // }
  
  // guard = bpm_->FetchPageBasic(header_page->root_page_id_);
  // InternalPage* root = guard.AsMut<InternalPage>();

  // while (true) {
  //   if (root->IsLeafPage()) { // 如果到了leaf这一层
  //     // 此时guard跟root指向的是同一页，这样做是安全的
  //     auto leaf = guard.As<LeafPage>();
  //     for (int i = 0; i < leaf->GetSize(); i ++) { // 对于leaf page，需要从0开始遍历key map
  //       if (comparator_(key, leaf->KeyAt(i)) == 0) { // 找到target
  //         return INDEXITERATOR_TYPE(bpm_, res_pgid, i);
  //       }
  //     }
  //     // 未找到target
  //     return INDEXITERATOR_TYPE(bpm_, INVALID_PAGE_ID); // TODO: 错误处理有待斟酌
  //   }

  //   bool flag = false;
  //   for (int i = 1; i < root->GetSize(); i ++) { // 对于internal page，需要从1开始遍历key map
  //     if (comparator_(key, root->KeyAt(i)) < 0) { // 如果target < 当前key，说明target在该key左边
  //       res_pgid = root->ValueAt(i - 1);
  //       guard = bpm_->FetchPageBasic(root->ValueAt(i - 1));
  //       root = guard.AsMut<InternalPage>(); // 去左孩子处
  //       flag = true;
  //       break;
  //     }
  //   }
  //   if (!flag) {
  //     res_pgid = root->ValueAt(i - 1);
  //     guard = bpm_->FetchPageBasic(root->ValueAt(root->GetSize() - 1));
  //     root = guard.AsMut<InternalPage>();// 去最右孩子处
  //   }
  // }

  // BUSTUB_ASSERT(false, "b+ tree Begin() wrong!\n");

  // // theorily unreachable
  // return INDEXITERATOR_TYPE(bpm_, INVALID_PAGE_ID); // TODO: 错误处理有待斟酌
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End() -> INDEXITERATOR_TYPE { 
  return INDEXITERATOR_TYPE(bpm_, INVALID_PAGE_ID); // TODO: 错误处理有待斟酌
}

/**
 * @return Page id of the root of this tree
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId() -> page_id_t {
  auto guard = std::move(bpm_->FetchPageWrite(header_page_id_));
  // 获取root page id
  auto header_page = guard.AsMut<BPlusTreeHeaderPage>();
  return header_page->root_page_id_; 
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/

/*
 * This method is used for test only
 * Read data from file and insert one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertFromFile(const std::string &file_name, Transaction *txn) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;

    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    Insert(index_key, rid, txn);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string &file_name, Transaction *txn) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;
    KeyType index_key;
    index_key.SetFromInteger(key);
    Remove(index_key, txn);
  }
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Print(BufferPoolManager *bpm) {
  auto root_page_id = GetRootPageId();
  auto guard = bpm->FetchPageBasic(root_page_id);
  PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::PrintTree(page_id_t page_id, const BPlusTreePage *page) {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<const LeafPage *>(page);
    std::cout << "Leaf Page: " << page_id << "\tNext: " << leaf->GetNextPageId() << std::endl;

    // Print the contents of the leaf page.
    std::cout << "Contents: ";
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i);
      if ((i + 1) < leaf->GetSize()) {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;

  } else {
    auto *internal = reinterpret_cast<const InternalPage *>(page);
    std::cout << "Internal Page: " << page_id << std::endl;

    // Print the contents of the internal page.
    std::cout << "Contents: ";
    for (int i = 1; i < internal->GetSize(); i++) {
      // std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i);
      std::cout << internal->KeyAt(i);
      if ((i + 1) < internal->GetSize()) {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      auto guard = bpm_->FetchPageBasic(internal->ValueAt(i));
      PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
    }
  }
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Draw(BufferPoolManager *bpm, const std::string &outf) {
  if (IsEmpty()) {
    LOG_WARN("Drawing an empty tree");
    return;
  }

  std::ofstream out(outf);
  out << "digraph G {" << std::endl;
  auto root_page_id = GetRootPageId();
  auto guard = bpm->FetchPageBasic(root_page_id);
  ToGraph(guard.PageId(), guard.template As<BPlusTreePage>(), out);
  out << "}" << std::endl;
  out.close();
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToGraph(page_id_t page_id, const BPlusTreePage *page, std::ofstream &out) {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<const LeafPage *>(page);
    // Print node name
    out << leaf_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << page_id << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">"
        << "max_size=" << leaf->GetMaxSize() << ",min_size=" << leaf->GetMinSize() << ",size=" << leaf->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      out << "<TD>" << leaf->KeyAt(i) << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << page_id << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << page_id << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }
  } else {
    auto *inner = reinterpret_cast<const InternalPage *>(page);
    // Print node name
    out << internal_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << page_id << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">"
        << "max_size=" << inner->GetMaxSize() << ",min_size=" << inner->GetMinSize() << ",size=" << inner->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) {
        out << inner->KeyAt(i);
      } else {
        out << " ";
      }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_guard = bpm_->FetchPageBasic(inner->ValueAt(i));
      auto child_page = child_guard.template As<BPlusTreePage>();
      ToGraph(child_guard.PageId(), child_page, out);
      if (i > 0) {
        auto sibling_guard = bpm_->FetchPageBasic(inner->ValueAt(i - 1));
        auto sibling_page = sibling_guard.template As<BPlusTreePage>();
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_guard.PageId() << " " << internal_prefix
              << child_guard.PageId() << "};\n";
        }
      }
      out << internal_prefix << page_id << ":p" << child_guard.PageId() << " -> ";
      if (child_page->IsLeafPage()) {
        out << leaf_prefix << child_guard.PageId() << ";\n";
      } else {
        out << internal_prefix << child_guard.PageId() << ";\n";
      }
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::DrawBPlusTree() -> std::string {
  if (IsEmpty()) {
    return "()";
  }

  PrintableBPlusTree p_root = ToPrintableBPlusTree(GetRootPageId());
  std::ostringstream out_buf;
  p_root.Print(out_buf);

  return out_buf.str();
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::ToPrintableBPlusTree(page_id_t root_id) -> PrintableBPlusTree {
  auto root_page_guard = bpm_->FetchPageBasic(root_id);
  auto root_page = root_page_guard.template As<BPlusTreePage>();
  PrintableBPlusTree proot;

  if (root_page->IsLeafPage()) {
    auto leaf_page = root_page_guard.template As<LeafPage>();
    proot.keys_ = leaf_page->ToString();
    proot.size_ = proot.keys_.size() + 4;  // 4 more spaces for indent

    return proot;
  }

  // draw internal page
  auto internal_page = root_page_guard.template As<InternalPage>();
  proot.keys_ = internal_page->ToString();
  proot.size_ = 0;
  for (int i = 0; i < internal_page->GetSize(); i++) {
    page_id_t child_id = internal_page->ValueAt(i);
    PrintableBPlusTree child_node = ToPrintableBPlusTree(child_id);
    proot.size_ += child_node.size_;
    proot.children_.push_back(child_node);
  }

  return proot;
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;

template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;

template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;

template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;

template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
