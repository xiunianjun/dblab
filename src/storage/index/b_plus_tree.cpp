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
    if (root->IsLeafPage()) {
      // guard and root is now pointing to the same page, so it is safe here
      auto leaf = guard.As<LeafPage>();
      BUSTUB_ASSERT(leaf->IsLeafPage(), "leaf should be a leaf page!");
      for (int i = 0; i < leaf->GetSize(); i ++) { // for leaf page, traverse the key map begin with 0
        if (comparator_(key, leaf->KeyAt(i)) == 0) { // get target
          result->push_back(static_cast<ValueType>(leaf->ValueAt(i)));
          return true;
        }
      }
      return false; // not get target
    }

    bool flag = false;
    for (int i = 1; i < root->GetSize(); i ++) { // for internal page, traverse the key map begin with 1
      if (comparator_(key, root->KeyAt(i)) < 0) { // if target < current key, target is in the lefthand of key
        guard = bpm_->FetchPageBasic(root->ValueAt(i - 1));
        root = guard.AsMut<InternalPage>(); // goto left child
        flag = true;
        break;
      }
    }
    if (!flag) {
      guard = bpm_->FetchPageBasic(root->ValueAt(root->GetSize() - 1));
      root = guard.AsMut<InternalPage>(); // goto most right child
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

  // get write page of root
  ctx.header_page_ = std::move(bpm_->FetchPageWrite(header_page_id_));
  // get root page id
  auto header_page = ctx.header_page_.value().AsMut<BPlusTreeHeaderPage>();
  ctx.root_page_id_ = header_page->root_page_id_;

  // b+ tree is empty
  if (ctx.root_page_id_ == INVALID_PAGE_ID) {
    bpm_->NewPageGuarded(&(ctx.root_page_id_));
    header_page->root_page_id_ = ctx.root_page_id_;
    
    // create new root
    auto guard = bpm_->FetchPageWrite(ctx.root_page_id_);
    LeafPage* root = guard.AsMut<LeafPage>(); // a leaf node initially
    root->Init(leaf_max_size_);
    // insert new value
    root->IncreaseSize(1);
    root->SetKeyAt(0, key);
    root->SetValueAt(0, value);
    ctx.header_page_ = std::nullopt;
    return true;
  }

  BUSTUB_ASSERT(ctx.root_page_id_ != INVALID_PAGE_ID, "root page id should be valid.");

  // get root page
  auto guard = bpm_->FetchPageWrite(ctx.root_page_id_);
  InternalPage* root = guard.AsMut<InternalPage>();
  ctx.write_set_.push_back(std::move(guard)); 
  
  /* step1 get target leaf node */
  while (true) {
    if (root->IsLeafPage()) {
      break;
    }
    bool flag = false;
    for (int i = 1; i < root->GetSize(); i ++) { // for internal page, traverse the key map begin with 1
      if (comparator_(key, root->KeyAt(i)) < 0) { // if target < current key, target is in the lefthand of key
        guard = std::move(bpm_->FetchPageWrite(root->ValueAt(i - 1)));
        root = guard.AsMut<InternalPage>(); // goto left child
        ctx.write_set_.push_back(std::move(guard));
        flag = true;
        break;
      }
    }
    if (!flag) {
      guard = std::move(bpm_->FetchPageWrite(root->ValueAt(root->GetSize() - 1)));
      root = guard.AsMut<InternalPage>(); // goto most right child
      ctx.write_set_.push_back(std::move(guard));
    }
  }

  auto leaf_guard = std::move(ctx.write_set_.back());
  auto leaf = leaf_guard.AsMut<LeafPage>();
  if (!(ctx.write_set_.empty())) {
    ctx.write_set_.pop_back();  // the target leaf was pushed to ctx above
  }

  // check is here first
  for (int i = 0; i < leaf->GetSize(); i++) { // for leaf page, traverse the key map begin with 0
    BUSTUB_ASSERT(leaf->IsLeafPage(), "leaf should be a leaf page!");
    if (comparator_(key, leaf->KeyAt(i)) == 0) { // related key is here
      ctx.write_set_.clear();
      ctx.header_page_ = std::nullopt;
      return false;
    }
  }

  bool should_split = (leaf->GetSize() == leaf->GetMaxSize());
  bool insert_small_than_tmp_key, special;
  int m, middle, idx;
  KeyType tmp_key, insert_key, swap;
  page_id_t page_id, insert_val;
  WritePageGuard new_page_guard;
  InternalPage* new_page;
  InternalPage* insert_page;
  
  /* step2 split the target leaf node */
  // Though I think there must be a waiy to merge this case with the below while(),
  // I finally decide not to do a merging for a more clear code.
  if (leaf->GetSize() == leaf->GetMaxSize()) {
    m = leaf->GetSize();
    BUSTUB_ASSERT(m > 0, "leaf size must bigger than zero!");
    tmp_key = leaf->KeyAt((m + 1) / 2);
    insert_key = tmp_key;

    /* split to two nodes first */
    // create new node
    bpm_->NewPageGuarded(&page_id);
    insert_val = page_id;
    auto new_page_guard = bpm_->FetchPageWrite(page_id);
    auto new_page = new_page_guard.AsMut<LeafPage>();
    new_page->Init(leaf_max_size_);
    // allocate the second half of the old node to the new node evenly
    new_page->IncreaseSize(m / 2);
    idx = 0;
    for (int i = (m + 1) / 2; i < m; i ++) { // the next page of the split point will give to the key0 of the new node
      new_page->SetKeyAt(idx, leaf->KeyAt(i));
      new_page->SetValueAt(idx, leaf->ValueAt(i));
      idx ++;
    }
    // shrink old node
    // for that the new root should also remain in the new node, minus m/2 is sufficient here
    leaf->IncreaseSize(-m/2);
    // link in the leaf iterator
    new_page->SetNextPageId(leaf->GetNextPageId());
    leaf->SetNextPageId(page_id);
      
    if (comparator_(key, tmp_key) > 0) {
      // insert into new node finally
      leaf = std::move(new_page);
      leaf_guard = std::move(new_page_guard);
    }
    
    if(ctx.write_set_.empty()) {  // root should be splited
      goto ROOT_SPLIT;
    }

    // get parent
    guard = std::move(ctx.write_set_.back());
    ctx.write_set_.pop_back();

    root = guard.AsMut<InternalPage>();
  }

  if (!should_split) {
    goto INSERTION_END;
  }

  // split up
  while(true) {
    if (root->GetSize() < root->GetMaxSize()) {
      // insert into parent node
      BUSTUB_ASSERT(root->IsLeafPage() == false, "root must not be leaf page in the up split");
      BUSTUB_ASSERT(root->GetSize() > 1, "root must have at least one element!");
      for (int i = 1; i <= root->GetSize(); i++) { // remember that root must be internal node here
        if (i != root->GetSize() && comparator_(insert_key, root->KeyAt(i)) > 0) {
          continue;
        }
        // insert into (i - 1)th position
        root->IncreaseSize(1);
        for (int j = root->GetSize() - 1; j >= i + 1; j --) {
          root->SetKeyAt(j, root->KeyAt(j-1));
          root->SetValueAt(j, root->ValueAt(j-1));
        }
        root->SetKeyAt(i, insert_key);
        root->SetValueAt(i, insert_val);  // remember to point to new page
        break;
      }
      goto INSERTION_END;
    }

    // split node
    m = root->GetSize();
    BUSTUB_ASSERT(m > 0, "leaf size must bigger than zero!");

    special = false;
    middle = (m + 1) / 2;
    tmp_key = root->KeyAt(middle);
    insert_small_than_tmp_key = (comparator_(insert_key, tmp_key) < 0);
    if (insert_small_than_tmp_key) {
      middle = m / 2;
      tmp_key = root->KeyAt(middle);
      if (comparator_(insert_key, tmp_key) >= 0) {
        special = true;
        swap = insert_key;
        insert_key = tmp_key;
        tmp_key = swap;
      }
    }

    // split to two nodes first
    // create new node
    bpm_->NewPageGuarded(&page_id);
    new_page_guard = bpm_->FetchPageWrite(page_id);
    new_page = new_page_guard.AsMut<InternalPage>();
    new_page->Init(internal_max_size_);

    if (!special)
      new_page->SetValueAt(0, root->ValueAt(middle));
    else {
      new_page->SetValueAt(0, insert_val);
      insert_val = root->ValueAt(middle);
    }
    idx = 1;
    for (int i = middle + 1; i < m; i ++) { // the next page of the split point will give to the key0 of the new node
      new_page->IncreaseSize(1);
      new_page->SetKeyAt(idx, root->KeyAt(i));
      new_page->SetValueAt(idx, root->ValueAt(i));
      idx ++;
    }
    // shrink old node
    root->IncreaseSize(-(idx));// remember that key0 is null

    insert_page = std::move(root);
    if (!insert_small_than_tmp_key) {
      insert_page = std::move(new_page);
    }

    for (int i = 1; i <= insert_page->GetSize(); i ++) {
      if (i != insert_page->GetSize() && comparator_(insert_key, insert_page->KeyAt(i)) > 0) {
        continue;
      }
      // insert into (i - 1)th position
      insert_page->IncreaseSize(1);
      BUSTUB_ASSERT(insert_page->GetSize() > 1, "insert_page must have at least one element!");
      for (int j = insert_page->GetSize() - 1; j >= i + 1; j --) {
        insert_page->SetKeyAt(j, insert_page->KeyAt(j - 1));
        insert_page->SetValueAt(j, insert_page->ValueAt(j - 1));
      }
      insert_page->SetKeyAt(i, insert_key);
      insert_page->SetValueAt(i, insert_val);
      break;
    }

    if (ctx.write_set_.empty()) {
      break;
    }

    // need to split
    // get parent of the "root"
    auto parent_guard = std::move(ctx.write_set_.back());
    auto parent = parent_guard.AsMut<InternalPage>();
    ctx.write_set_.pop_back();

    root = parent;
    guard = std::move(parent_guard);
    insert_key = tmp_key;
    insert_val = page_id;
  }

ROOT_SPLIT:
  {
    // create and init new root
    page_id_t new_root_page_id;
    bpm_->NewPageGuarded(&new_root_page_id);
    auto new_root_guard = bpm_->FetchPageWrite(new_root_page_id);
    auto new_root = new_root_guard.AsMut<InternalPage>();
    new_root->Init(internal_max_size_);
    // remember to set key0 pointing to old root
    new_root->SetValueAt(0, ctx.root_page_id_);

    auto header_page = ctx.header_page_.value().AsMut<BPlusTreeHeaderPage>();
    header_page->root_page_id_ = new_root_page_id;
    ctx.root_page_id_ = new_root_page_id;

    new_root->IncreaseSize(1);

    // insert into parent node
    new_root->SetKeyAt(1, tmp_key);
    new_root->SetValueAt(1, page_id);
  }
  
INSERTION_END:
  // insert the target
  BUSTUB_ASSERT(leaf->IsLeafPage(), "leaf should be a leaf page!");
  for (int i = 0; i <= leaf->GetSize(); i ++) {
    if (i != leaf->GetSize() && comparator_(key, leaf->KeyAt(i)) > 0) {
      continue;
    }
    // insert into (i - 1)th position
    leaf->IncreaseSize(1);
    for (int j = leaf->GetSize() - 1; j >= i + 1; j --) {
      leaf->SetKeyAt(j, leaf->KeyAt(j - 1));
      leaf->SetValueAt(j, leaf->ValueAt(j - 1));
    }
    leaf->SetKeyAt(i, key);
    leaf->SetValueAt(i, value);
    break;
  }

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
  // get write page of root
  ctx.header_page_ = std::move(bpm_->FetchPageWrite(header_page_id_));
  // get root page id
  auto header_page = ctx.header_page_.value().AsMut<BPlusTreeHeaderPage>();
  ctx.root_page_id_ = header_page->root_page_id_;

  // b+ tree is empty
  if (ctx.root_page_id_ == INVALID_PAGE_ID) {
    return ;
  }

  BUSTUB_ASSERT(ctx.root_page_id_ != INVALID_PAGE_ID, "root page id should be valid.");

  // get root page
  auto guard = bpm_->FetchPageWrite(ctx.root_page_id_);
  InternalPage* root = guard.AsMut<InternalPage>();
  ctx.write_set_.push_back(std::move(guard)); 
  
  /* step1 get target leaf node */
  while (true) {
    if (root->IsLeafPage()) {
      break;
    }
    bool flag = false;
    int i;
    for (i = 1; i < root->GetSize(); i ++) { // for internal page, traverse the key map begin with 1
      if (comparator_(key, root->KeyAt(i)) < 0) { // if target < current key, target is in the lefthand of key
        guard = std::move(bpm_->FetchPageWrite(root->ValueAt(i - 1)));
        root = guard.AsMut<InternalPage>(); // goto left child
        ctx.write_set_.push_back(std::move(guard));
        flag = true;
        break;
      }
    }
    if (!flag) {
      guard = std::move(bpm_->FetchPageWrite(root->ValueAt(root->GetSize() - 1)));
      root = guard.AsMut<InternalPage>(); // goto most right child
      ctx.write_set_.push_back(std::move(guard));
    }
    ctx.position_set_.push_back(i - 1);
  }

  auto leaf_guard = std::move(ctx.write_set_.back());
  auto leaf = leaf_guard.AsMut<LeafPage>();
  if (!(ctx.write_set_.empty())) {
    ctx.write_set_.pop_back();  // the target leaf was pushed to ctx above
  }

  // delete key
  for (int i = 0; i < leaf->GetSize(); i ++) { // for leaf page, traverse the key map begin with 0
    if (comparator_(key, leaf->KeyAt(i)) == 0) { // related key is here
      // delete key
      for (int j = i + 1; j < leaf->GetSize(); j ++) {
        leaf->SetKeyAt(j - 1, leaf->KeyAt(j));
        leaf->SetValueAt(j - 1, leaf->ValueAt(j));
      }
      leaf->IncreaseSize(-1);
      break;
    }
  }

  if (leaf->GetSize() >= leaf->GetMinSize())  return ;
  if (ctx.write_set_.empty()) {
    if (leaf->GetSize() > 0)  return ;
    // switch to an empty tree
    auto header_page = ctx.header_page_.value().AsMut<BPlusTreeHeaderPage>();
    header_page->root_page_id_ = INVALID_PAGE_ID;
    ctx.root_page_id_ = INVALID_PAGE_ID;
    return ;
  }
  
  /* 
    Should do a merge or a steal. 
    If do a merge, we should delete related key in the parent, and merge up till
    reaching the root;
    If do a steal, we should update related key in the parent, and update up till
    reaching the root.
  */

  /*
    For that steal is more simple, we first check whether it can do a steal first.
    We steal the node whose size is biggest between the next and the prev node.
    If the prev size is bigger, we only update self key in parent.
    If the next size is bigger, we update both self key and next key n parent.
    After that, we trace back and update all the parent nodes which contains the 
    target key.
  */
  // get parent
  auto parent_guard = std::move(ctx.write_set_.back());
  auto parent = parent_guard.AsMut<InternalPage>();
  ctx.write_set_.pop_back();
  auto root_guard = std::move(parent_guard);
  root = root_guard.AsMut<InternalPage>();

  WritePageGuard next_guard, prev_guard;
  LeafPage* next_leaf_page = nullptr;
  LeafPage* prev_leaf_page = nullptr;
  int leaf_position = ctx.position_set_.back();
  ctx.position_set_.pop_back();
  int internal_position = 0;
  KeyType update_key;

  int bigger_size = 0;
  if (leaf_position == root->GetSize() - 1) {
    // rightest, has no next
    prev_guard = bpm_->FetchPageWrite(root->ValueAt(leaf_position - 1));
    prev_leaf_page = prev_guard.AsMut<LeafPage>();
    bigger_size = prev_leaf_page->GetSize();
  } else if (leaf_position == 0) {
    // leftest, has no prev
    next_guard = bpm_->FetchPageWrite(root->ValueAt(leaf_position + 1));
    next_leaf_page = next_guard.AsMut<LeafPage>();
    bigger_size = next_leaf_page->GetSize();
  } else {
    prev_guard = bpm_->FetchPageWrite(root->ValueAt(leaf_position - 1));
    prev_leaf_page = prev_guard.AsMut<LeafPage>();
    next_guard = bpm_->FetchPageWrite(root->ValueAt(leaf_position + 1));
    next_leaf_page = next_guard.AsMut<LeafPage>();
    bigger_size= std::max(next_leaf_page->GetSize(), prev_leaf_page->GetSize());
  }

  BUSTUB_ASSERT(next_leaf_page != nullptr || prev_leaf_page != nullptr, "must have at least one company.");
  if (bigger_size - 1 < leaf->GetMinSize()) {
    goto MERGE_NODE;
  }
  
  // should not merge, just steal an element from bigger leaf and update the parent node.

  leaf->IncreaseSize(1);
  if (next_leaf_page == nullptr || (prev_leaf_page != nullptr && next_leaf_page->GetSize() <= prev_leaf_page->GetSize())) {
    // steal one element from prev
    for (int j = 1; j < leaf->GetSize(); j ++) {
      leaf->SetKeyAt(j, leaf->KeyAt(j - 1));
      leaf->SetValueAt(j, leaf->ValueAt(j - 1));
    }
    leaf->SetKeyAt(0, prev_leaf_page->KeyAt(prev_leaf_page->GetSize() - 1));
    leaf->SetValueAt(0, prev_leaf_page->ValueAt(prev_leaf_page->GetSize() - 1));
    prev_leaf_page->IncreaseSize(-1);
  } else {
    // steal one element from next
    leaf->SetKeyAt(leaf->GetSize() - 1, next_leaf_page->KeyAt(0));
    leaf->SetValueAt(leaf->GetSize() - 1, next_leaf_page->ValueAt(0));
    for (int j = 1; j < next_leaf_page->GetSize(); j ++) {
      next_leaf_page->SetKeyAt(j - 1, next_leaf_page->KeyAt(j));
      next_leaf_page->SetValueAt(j - 1, next_leaf_page->ValueAt(j));
    }
    next_leaf_page->IncreaseSize(-1);

    // update next key
    root->SetKeyAt(leaf_position + 1, next_leaf_page->KeyAt(0));
  }
  if (leaf_position > 0)
    root->SetKeyAt(leaf_position, leaf->KeyAt(0));

  update_key = leaf->KeyAt(0);

  // update parent
  while(!ctx.write_set_.empty()) {
    // get parent
    parent_guard = std::move(ctx.write_set_.back());
    parent = parent_guard.AsMut<InternalPage>();
    ctx.write_set_.pop_back();
    internal_position = ctx.position_set_.back();
    ctx.position_set_.pop_back();

    if (comparator_(parent->KeyAt(internal_position), key) == 0) {
      auto tmp_guard = bpm_->FetchPageRead(parent->ValueAt(internal_position));
      parent->SetKeyAt(internal_position, update_key);
    }
  }

  ctx.write_set_.clear();
  ctx.header_page_ = std::nullopt;
  return ;

MERGE_NODE:
  /* 
    Need to merge with one of the node. It is more simple to try to merge the left node 
    first. So the strategy:
    1. Pick the prev node to merge. (If leaf is most left, pick next node)
    2. Update delete-key. (for prev, it's leaf[0]; for next, it's right key, and need 
      to update self)
    3. Go up till reaching root. Do:
      1. delete delete-key.
      2. pick merging or stealing like above.
        1. if merge, update delete-key, go up;
        2. if steal, break to do update and has no need to go up.
    4. Remember to deal with edge case: root.
  */
  WritePageGuard merge_to_guard, merge_from_guard;
  LeafPage* merge_to_page;
  LeafPage* merge_from_page;
  KeyType delete_key;

  if (leaf_position != 0) {
    merge_to_guard = std::move(prev_guard);
    merge_from_guard = std::move(leaf_guard);
    delete_key = root->KeyAt(leaf_position);
  } else {
    merge_to_guard = std::move(leaf_guard);
    merge_from_guard = std::move(next_guard);
    delete_key = root->KeyAt(leaf_position + 1);
  }
  merge_to_page = merge_to_guard.AsMut<LeafPage>();
  merge_from_page = merge_from_guard.AsMut<LeafPage>();

  int idx = merge_to_page->GetSize();

  for (int i = 0; i < merge_from_page->GetSize(); i ++) {
    merge_to_page->IncreaseSize(1);
    merge_to_page->SetKeyAt(idx, merge_from_page->KeyAt(i));
    merge_to_page->SetValueAt(idx, merge_from_page->ValueAt(i));
    idx ++;
  }

  update_key = merge_to_page->KeyAt(0);
  merge_to_page->SetNextPageId(merge_from_page->GetNextPageId());

  merge_from_page->IncreaseSize(merge_from_page->GetSize());
  merge_to_guard.Drop();
  merge_from_guard.Drop();

  while(true) {
    // delete key
    bool is_find = false;
    for (int i = 1; i < root->GetSize(); i ++) { // for leaf page, traverse the key map begin with 0
      if (comparator_(delete_key, root->KeyAt(i)) == 0) { // related key is here
        // delete key
        for (int j = i + 1; j < root->GetSize(); j ++) {
          root->SetKeyAt(j - 1, root->KeyAt(j));
          root->SetValueAt(j - 1, root->ValueAt(j));
        }
        root->IncreaseSize(-1);
        is_find = true;
        break;
      }
    }
    BUSTUB_ASSERT(is_find, "deleted key must be in [1, size - 1].");

    if (ctx.write_set_.empty()) {
      if (root->GetSize() == 1) {
        // change root!
        auto header_page = ctx.header_page_.value().AsMut<BPlusTreeHeaderPage>();
        header_page->root_page_id_ = root->ValueAt(0);
        ctx.root_page_id_ = root->ValueAt(0);
      }
      break;
    }

    parent_guard = std::move(ctx.write_set_.back());
    parent = parent_guard.AsMut<InternalPage>();
    ctx.write_set_.pop_back();
    internal_position = ctx.position_set_.back();
    ctx.position_set_.pop_back();

    InternalPage* next_internal_page = nullptr;
    InternalPage* prev_internal_page = nullptr;

    int bigger_size = 0;
    if (internal_position == parent->GetSize() - 1) {
      // rightest, has no next
      prev_guard = bpm_->FetchPageWrite(parent->ValueAt(internal_position - 1));
      prev_internal_page = prev_guard.AsMut<InternalPage>();
      bigger_size = prev_internal_page->GetSize();
    } else if (internal_position == 0) {
      // leftest, has no prev
      next_guard = bpm_->FetchPageWrite(parent->ValueAt(internal_position + 1));
      next_internal_page = next_guard.AsMut<InternalPage>();
      bigger_size = next_internal_page->GetSize();
    } else {
      prev_guard = bpm_->FetchPageWrite(parent->ValueAt(internal_position - 1));
      prev_internal_page = prev_guard.AsMut<InternalPage>();
      next_guard = bpm_->FetchPageWrite(parent->ValueAt(internal_position + 1));
      next_internal_page = next_guard.AsMut<InternalPage>();
      bigger_size= std::max(next_internal_page->GetSize(), prev_internal_page->GetSize());
    }

    BUSTUB_ASSERT(next_internal_page != nullptr || prev_internal_page != nullptr, "must have at least one company.");
    
    if (bigger_size - 1 < root->GetMinSize()) {
      // merge!
      InternalPage* merge_to_page;
      InternalPage* merge_from_page;

      if (internal_position != 0) {
        merge_to_guard = std::move(prev_guard);
        merge_from_guard = std::move(root_guard);
        delete_key = parent->KeyAt(internal_position);
      } else {
        merge_to_guard = std::move(root_guard);
        merge_from_guard = std::move(next_guard);
        delete_key = parent->KeyAt(internal_position + 1);
      }
      merge_to_page = merge_to_guard.AsMut<InternalPage>();
      merge_from_page = merge_from_guard.AsMut<InternalPage>();

      int idx = merge_to_page->GetSize();
      merge_to_page->IncreaseSize(1);
      merge_to_page->SetKeyAt(idx, delete_key);
      merge_to_page->SetValueAt(idx, merge_from_page->ValueAt(0));
      idx ++;

      for (int i = 1; i < merge_from_page->GetSize(); i ++) {
        merge_to_page->IncreaseSize(1);
        merge_to_page->SetKeyAt(idx, merge_from_page->KeyAt(i));
        merge_to_page->SetValueAt(idx, merge_from_page->ValueAt(i));
        idx ++;
      }

      merge_from_page->IncreaseSize(merge_from_page->GetSize());
      merge_to_guard.Drop();
      merge_from_guard.Drop();

      root_guard = std::move(parent_guard);
      root = root_guard.AsMut<InternalPage>();
      continue;
    }
    
    // should not merge, just steal an element from bigger leaf and update the parent node.
    // This case doesn't need to update up the new key of the prev or the next node, 
    // because it just update the second key or after of the parent node, and won't 
    // affect the grandparent.
    if (next_internal_page == nullptr || (prev_internal_page != nullptr && next_internal_page->GetSize() <= prev_internal_page->GetSize())) {
      // steal one element from prev
      if (comparator_(delete_key, parent->KeyAt(internal_position)) == 0) // TODO
        BUSTUB_ASSERT(false, "the deleted key has no chance to be in position 0.");
      
      root->IncreaseSize(1);
      for (int j = 2; j < root->GetSize(); j ++) {
        root->SetKeyAt(j, root->KeyAt(j - 1));
        root->SetValueAt(j, root->ValueAt(j - 1));
      }
      root->SetKeyAt(1, parent->KeyAt(internal_position));
      root->SetValueAt(1, root->ValueAt(0));
      root->SetValueAt(0, prev_internal_page->ValueAt(prev_internal_page->GetSize() - 1));
      parent->SetKeyAt(internal_position, prev_internal_page->KeyAt(prev_internal_page->GetSize() - 1));
      prev_internal_page->IncreaseSize(-1);
    } else {
      // steal one element from next
      root->IncreaseSize(1);
      root->SetKeyAt(root->GetSize() - 1, parent->KeyAt(internal_position + 1));
      root->SetValueAt(root->GetSize() - 1, next_internal_page->ValueAt(0));
      next_internal_page->SetValueAt(0, next_internal_page->ValueAt(1));
      parent->SetKeyAt(internal_position + 1, next_internal_page->KeyAt(1));
      for (int j = 2; j < next_internal_page->GetSize(); j ++) {
        next_internal_page->SetKeyAt(j - 1, next_internal_page->KeyAt(j));
        next_internal_page->SetValueAt(j - 1, next_internal_page->ValueAt(j));
      }
      next_internal_page->IncreaseSize(-1);
    }

    // update parent
    while(!ctx.write_set_.empty()) {
      // get parent
      parent_guard = std::move(ctx.write_set_.back());
      parent = parent_guard.AsMut<InternalPage>();
      ctx.write_set_.pop_back();
      internal_position = ctx.position_set_.back();
      ctx.position_set_.pop_back();

      if (comparator_(parent->KeyAt(internal_position), key) == 0) {
        auto tmp_guard = bpm_->FetchPageRead(parent->ValueAt(internal_position));
        parent->SetKeyAt(internal_position, update_key);
      }
    }

    break;
  }

  ctx.write_set_.clear();
  ctx.header_page_ = std::nullopt;
  
  return ;
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the leftmost leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin() -> INDEXITERATOR_TYPE { 
  // get root page
  BasicPageGuard guard = bpm_->FetchPageBasic(header_page_id_);
  auto header_page = guard.As<BPlusTreeHeaderPage>();
  auto res_pgid = header_page->root_page_id_;
  if (header_page->root_page_id_ == INVALID_PAGE_ID) {
    return INDEXITERATOR_TYPE(bpm_, INVALID_PAGE_ID);
  }
  
  guard = bpm_->FetchPageBasic(header_page->root_page_id_);
  InternalPage* root = guard.AsMut<InternalPage>();

  while (true) {
    if (root->IsLeafPage()) {
      return INDEXITERATOR_TYPE(bpm_, res_pgid);
    }

    res_pgid = root->ValueAt(0);
    guard = bpm_->FetchPageBasic(root->ValueAt(0));
    root = guard.AsMut<InternalPage>();
  }

  // theorily unreachable
  BUSTUB_ASSERT(false, "b+ tree Begin() wrong!\n");
  return INDEXITERATOR_TYPE(bpm_, INVALID_PAGE_ID);
}

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType &key) -> INDEXITERATOR_TYPE {
  auto it = Begin();
  while (!(it.IsEnd() || comparator_((*it).first, key) == 0)) {
    ++ it;
  }
  return it;
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End() -> INDEXITERATOR_TYPE { 
  return INDEXITERATOR_TYPE(bpm_, INVALID_PAGE_ID);
}

/**
 * @return Page id of the root of this tree
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId() -> page_id_t {
  auto guard = std::move(bpm_->FetchPageWrite(header_page_id_));
  // get root page id
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
