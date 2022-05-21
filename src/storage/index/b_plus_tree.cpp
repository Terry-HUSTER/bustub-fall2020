//===----------------------------------------------------------------------===//
//
//                         CMU-DB Project (15-445/645)
//                         ***DO NO SHARE PUBLICLY***
//
// Identification: src/index/b_plus_tree.cpp
//
// Copyright (c) 2018, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <string>

#include "common/exception.h"
#include "common/rid.h"
#include "storage/index/b_plus_tree.h"
#include "storage/page/header_page.h"

namespace bustub {
INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, BufferPoolManager *buffer_pool_manager, const KeyComparator &comparator,
                          int leaf_max_size, int internal_max_size)
    : index_name_(std::move(name)),
      root_page_id_(INVALID_PAGE_ID),
      buffer_pool_manager_(buffer_pool_manager),
      comparator_(comparator),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {
  // LOG_DEBUG("b+ tree init, leaf max %d internal max %d", leaf_max_size, internal_max_size);
}

/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::IsEmpty() const { return root_page_id_ == INVALID_PAGE_ID; }
/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::GetValue(const KeyType &key, std::vector<ValueType> *result, Transaction *transaction) {
  mutex_.lock();
  Page *page = FindLeafPage(key, false);
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  ValueType value;
  bool found = leaf->Lookup(key, &value, comparator_);
  buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
  mutex_.unlock();
  if (!found) {
    return false;
  } else {
    result->push_back(value);
    return true;
  }
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
bool BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value, Transaction *transaction) {
  mutex_.lock();
  if (root_page_id_ == INVALID_PAGE_ID) {
    StartNewTree(key, value);
    mutex_.unlock();
    return true;
  } else {
    bool ret = InsertIntoLeaf(key, value, transaction);
    mutex_.unlock();
    return ret;
  }
}
/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::StartNewTree(const KeyType &key, const ValueType &value) {
  page_id_t page_id;
  auto *page = buffer_pool_manager_->NewPage(&page_id);
  if (page == nullptr) {
    throw Exception(ExceptionType::OUT_OF_MEMORY, "alloc new root page fail");
  }
  // 第一个根节点必然是叶子节点
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  leaf->Init(page_id, INVALID_PAGE_ID, leaf_max_size_);
  root_page_id_ = page_id;
  UpdateRootPageId(true);
  leaf->Insert(key, value, comparator_);
  buffer_pool_manager_->UnpinPage(page_id, true);
}

/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immdiately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::InsertIntoLeaf(const KeyType &key, const ValueType &value, Transaction *transaction) {
  // 注释说只支持 unique key，所以如果 key 已存在则直接返回
  auto *page = FindLeafPage(key);
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  int old_size = leaf->GetSize();
  int size = leaf->Insert(key, value, comparator_);
  if (size == old_size) {
    // 插入失败，已存在重复 key
    LOG_WARN("dup key %ld", key.ToString());
    buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
    return false;
  } else {
    // 插入成功，递归检查分裂
    if (leaf->GetSize() >= leaf->GetMaxSize()) {
      Split(leaf);
    }
    buffer_pool_manager_->UnpinPage(page->GetPageId(), true);
    // LOG_DEBUG("insert key %ld succ", key.ToString());
    return true;
  }
}

/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
N *BPLUSTREE_TYPE::Split(N *node) {
  // 满了才能分裂
  assert(node->GetSize() == node->GetMaxSize());
  page_id_t right_page_id;
  auto *right_page = buffer_pool_manager_->NewPage(&right_page_id);
  if (right_page == nullptr) {
    throw Exception(ExceptionType::OUT_OF_MEMORY, "alloc new internal page fail");
  }

  // 泛型好啊，不管 leaf 还是 internal 都可用 N 表示
  auto *right_node = reinterpret_cast<N *>(right_page->GetData());
  if (node->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(node);
    right_node->Init(right_page_id, node->GetParentPageId(), leaf_max_size_);
    leaf->MoveHalfTo((LeafPage *)right_node);
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(node);
    right_node->Init(right_page_id, node->GetParentPageId(), internal_max_size_);
    internal->MoveHalfTo((InternalPage *)right_node, buffer_pool_manager_);
  }
  // 分裂后把 middle key 插入到 parent 中
  InsertIntoParent(node, right_node->KeyAt(0), right_node);
  // 哪个函数里申请，就在哪个函数里释放
  buffer_pool_manager_->UnpinPage(right_page_id, true);
  return right_node;
}

/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method
 * @param   key
 * @param   new_node      returned page from split() method
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertIntoParent(BPlusTreePage *old_node, const KeyType &key, BPlusTreePage *new_node,
                                      Transaction *transaction) {
  // old_node: split 后的 left child
  // new_node: split 后的 right child
  if (old_node->IsRootPage()) {
    // 特殊的 split：把 root 也撑爆了，创造一个新 root
    page_id_t root_page_id;
    auto *page = buffer_pool_manager_->NewPage(&root_page_id);
    if (page == nullptr) {
      LOG_ERROR("alloc new root page fail");
      abort();
    }
    auto *internal = reinterpret_cast<InternalPage *>(page->GetData());
    internal->Init(root_page_id, INVALID_PAGE_ID, internal_max_size_);
    // 一个变成 left child，一个变成 right child，key 即 right[0]，无 key，但有 value
    // 可以用 1 2 3 4 5 6 这组数据观察下
    // https://www.cs.usfca.edu/~galles/visualization/BPlusTree.html
    internal->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());
    // 这里摔了一跤：新增 root 后要更新 parent_page_id
    old_node->SetParentPageId(root_page_id);
    new_node->SetParentPageId(root_page_id);
    root_page_id_ = root_page_id;
    UpdateRootPageId(false);
    buffer_pool_manager_->UnpinPage(root_page_id, true);
  } else {
    // 普通的 split：把 right 的 page id 登记到 parent
    page_id_t parent_page_id = old_node->GetParentPageId();
    auto *page = buffer_pool_manager_->FetchPage(parent_page_id);
    auto *parent = reinterpret_cast<InternalPage *>(page->GetData());
    parent->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
    if (parent->GetSize() >= parent->GetMaxSize()) {
      // 父节点满了，递归分裂
      Split(parent);
    }
    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
  }
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immdiately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType &key, Transaction *transaction) {
  mutex_.lock();
  // LOG_DEBUG("REMOVE(%ld)", key.ToString());
  // 删除的逻辑完全参考自 《数据库系统概念》B+ Tree 那节的几幅伪代码图片
  if (root_page_id_ == INVALID_PAGE_ID) {
    return;
  }
  Page *page = FindLeafPage(key);
  if (page == nullptr) {
    throw Exception(ExceptionType::OUT_OF_MEMORY, "FindLeafPage fetch page fail");
  }
  auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  assert(leaf->IsLeafPage());
  DeleteEntry(node, key, transaction);

  // buffer_pool_manager_->UnpinPage(page->GetPageId(), true);
  mutex_.unlock();
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::DeleteEntry(BPlusTreePage *node, KeyType key, Transaction *transaction) {
  // 先删除指定 key
  if (node->IsLeafPage()) {
    LeafPage *leaf = reinterpret_cast<LeafPage *>(node);
    leaf->RemoveAndDeleteRecord(key, comparator_);
  } else {
    InternalPage *internal = reinterpret_cast<InternalPage *>(node);
    auto value = internal->Lookup(key, comparator_);
    int value_idx = internal->ValueIndex(value);
    internal->Remove(value_idx);
  }

  if (node->IsRootPage()) {
    // 根节点特别处理
    if (AdjustRoot(node)) {
      // 有了新根节点后删除当前节点
      buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
      // LOG_DEBUG("delete page becase adjust root %d", node->GetPageId());
      buffer_pool_manager_->DeletePage(node->GetPageId());
    }
    return;
  } else if (node->GetSize() < node->GetMinSize()) {
    // 非根节点则分为合并和重新调整两种情况
    page_id_t parent_id = node->GetParentPageId();
    if (parent_id == INVALID_PAGE_ID) {
      LOG_ERROR("should never go here!");
      abort();
    }
    auto *parent_page = buffer_pool_manager_->FetchPage(parent_id);
    if (parent_page == nullptr) {
      throw Exception(ExceptionType::OUT_OF_MEMORY, "DeleteEntry fetch parent_page fail");
    }
    auto *parent_node = reinterpret_cast<BPlusTreePage *>(parent_page->GetData());
    auto *parent_internal = reinterpret_cast<InternalPage *>(parent_page->GetData());
    int idx = parent_internal->ValueIndex(node->GetPageId());
    int sibling_idx = 0;
    // 取出一个兄弟节点，先检查前，再检查后
    if (idx == 0) {
      // 没有前兄弟，只能取后兄弟
      sibling_idx = 1;
    } else {
      // 前面有兄弟，则取前兄弟
      sibling_idx = idx - 1;
    }
    // middle key 即 parent 中两个节点中后者的 key
    int middle_idx = std::max(idx, sibling_idx);
    KeyType middle_key = parent_internal->KeyAt(middle_idx);

    // 实例化兄弟节点
    page_id_t sibling_page_id = parent_internal->ValueAt(sibling_idx);
    // LOG_DEBUG("parent pid %d node pid %d sibling pid %d", parent_node->GetPageId(), node->GetPageId(), sibling_page_id);
    auto *sibling_page = buffer_pool_manager_->FetchPage(sibling_page_id);
    auto *sibling_node = reinterpret_cast<BPlusTreePage *>(sibling_page->GetData());
    if (node->GetSize() + sibling_node->GetSize() <= node->GetMaxSize()) {
      // 是否可以吞并兄弟，对应 Coalesce 函数
      // 后合并到前, idx 为前， sibling_idx 为后
      if (idx > sibling_idx) {
        std::swap(sibling_node, node);
      }
      // LOG_DEBUG("MoveAllTo");
      if (!node->IsLeafPage()) {
        ((InternalPage *)sibling_node)->MoveAllTo((InternalPage *)node, middle_key, buffer_pool_manager_);
      } else {
        ((LeafPage *)sibling_node)->MoveAllTo((LeafPage *)node);
      }

      // 释放 node 和 sibling node
      buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
      buffer_pool_manager_->UnpinPage(sibling_node->GetPageId(), true);
      // LOG_DEBUG("delete page becase merge, lose %d win %d size %d max %d", sibling_node->GetPageId(), node->GetPageId(),
                // node->GetSize(), node->GetMaxSize());
      buffer_pool_manager_->DeletePage(sibling_node->GetPageId());

      // 删除掉 parent 中指向后者的 key/value
      DeleteEntry(parent_node, middle_key, transaction);
    } else {
      // 不能吞并兄弟，则重新分布，从兄弟中借一个元素
      // 对应 Redistribute 函数
      if (sibling_idx < idx) {
        // 从前兄弟末尾取一个
        if (node->IsLeafPage()) {
          ((LeafPage *)sibling_node)->MoveLastToFrontOf((LeafPage *)node);
        } else {
          ((InternalPage *)sibling_node)->MoveLastToFrontOf((InternalPage *)node, middle_key, buffer_pool_manager_);
        }
        // LOG_DEBUG("MoveLastToFrontOf %d: %d => %d: %d", sibling_page_id, sibling_node->GetSize(), node->GetPageId(), node->GetSize());

        // 更新 parent 的 middle key
        parent_internal->SetKeyAt(middle_idx, middle_key);
      } else {
        // 从后兄弟开头取一个
        // LOG_DEBUG("MoveFirstToEndOf");
        if (node->IsLeafPage()) {
          ((LeafPage *)sibling_node)->MoveFirstToEndOf((LeafPage *)node);
        } else {
          ((InternalPage *)sibling_node)->MoveFirstToEndOf((InternalPage *)node, middle_key, buffer_pool_manager_);
        }

        // 更新 parent 的 middle key
        parent_internal->SetKeyAt(middle_idx, middle_key);
      }

      // TODO: 似乎不完全是脏页
      buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
      buffer_pool_manager_->UnpinPage(sibling_node->GetPageId(), true);
    }
  }
}

/*
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
bool BPLUSTREE_TYPE::CoalesceOrRedistribute(N *node, Transaction *transaction) {
  throw std::runtime_error("unimplemented");
  return false;
}

/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @return  true means parent node should be deleted, false means no deletion
 * happend
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
bool BPLUSTREE_TYPE::Coalesce(N **neighbor_node, N **node,
                              BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> **parent, int index,
                              Transaction *transaction) {
  throw std::runtime_error("unimplemented");
  return false;
}

/*
 * Redistribute key & value pairs from one page to its sibling page. If index ==
 * 0, move sibling page's first key & value pair into end of input "node",
 * otherwise move sibling page's last key & value pair into head of input
 * "node".
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 */
INDEX_TEMPLATE_ARGUMENTS
template <typename N>
void BPLUSTREE_TYPE::Redistribute(N *neighbor_node, N *node, int index) {
  throw std::runtime_error("unimplemented");
}
/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within coalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happend
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::AdjustRoot(BPlusTreePage *old_root_node) {
  if (!old_root_node->IsLeafPage() && old_root_node->GetSize() == 1) {
    // 1. internal node && size == 1，相当于无用节点，删除之，将其孩子作为新的 root
    // 踩坑：误把 == 1 写成 == 0
    auto *internal = reinterpret_cast<InternalPage *>(old_root_node);
    root_page_id_ = internal->ValueAt(0);
    Page *page = buffer_pool_manager_->FetchPage(root_page_id_);
    auto *child_node = reinterpret_cast<BPlusTreePage *>(page->GetData());
    child_node->SetParentPageId(INVALID_PAGE_ID);
    UpdateRootPageId(0);
    return true;
  } else if (old_root_node->IsLeafPage() && old_root_node->GetSize() == 0) {
    // 2. leaf node && size == 0, 直接删除
    // 把唯一叶子删掉后整个 Tree 都空了
    root_page_id_ = INVALID_PAGE_ID;
    UpdateRootPageId(0);
    return true;
  }
  return false;
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the leaftmost leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE BPLUSTREE_TYPE::begin() {
  auto *page = FindLeafPage(KeyType{}, true);
  return INDEXITERATOR_TYPE(buffer_pool_manager_, page, 0);
}

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE BPLUSTREE_TYPE::Begin(const KeyType &key) {
  auto *leaf_page = FindLeafPage(key);
  LeafPage *leaf_node = reinterpret_cast<LeafPage *>(leaf_page->GetData());
  auto idx = leaf_node->KeyIndex(key, comparator_);
  return INDEXITERATOR_TYPE(buffer_pool_manager_, leaf_page, idx);
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE BPLUSTREE_TYPE::end() {
  // TODO: 优化成 O(log N) 复杂度
  // LOG_DEBUG("call end()");
  auto iter = begin();
  while (!iter.isEnd()) ++iter;
  return iter;
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*
 * Find leaf page containing particular key, if leftMost flag == true, find
 * the left most leaf page
 */
INDEX_TEMPLATE_ARGUMENTS
Page *BPLUSTREE_TYPE::FindLeafPage(const KeyType &key, bool leftMost) {
  // 自 root 向下查询，直到 leaf
  page_id_t page_id = root_page_id_;
  while (true) {
    Page *page = buffer_pool_manager_->FetchPage(page_id);
    if (page == nullptr) {
      throw Exception(ExceptionType::OUT_OF_MEMORY, "FindLeafPage fetch page fail");
    }
    auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());
    // leaf 节点: 直接返回。因为还要用，暂且不 unpin
    if (node->IsLeafPage()) {
      return page;
    }
    // internal 节点：二分定位 child page id
    auto *internal = reinterpret_cast<InternalPage *>(node);
    page_id = leftMost ? internal->ValueAt(0) : internal->Lookup(key, comparator_);
    buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
  }
}

/*
 * Update/Insert root page id in header page(where page_id = 0, header_page is
 * defined under include/page/header_page.h)
 * Call this method everytime root page id is changed.
 * @parameter: insert_record      defualt value is false. When set to true,
 * insert a record <index_name, root_page_id> into header page instead of
 * updating it.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::UpdateRootPageId(int insert_record) {
  HeaderPage *header_page = static_cast<HeaderPage *>(buffer_pool_manager_->FetchPage(HEADER_PAGE_ID));
  if (insert_record != 0) {
    // create a new record<index_name + root_page_id> in header_page
    header_page->InsertRecord(index_name_, root_page_id_);
  } else {
    // update root_page_id in header_page
    header_page->UpdateRecord(index_name_, root_page_id_);
  }
  buffer_pool_manager_->UnpinPage(HEADER_PAGE_ID, true);
  // LOG_DEBUG("set root page %d", root_page_id_);
}

/*
 * This method is used for test only
 * Read data from file and insert one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertFromFile(const std::string &file_name, Transaction *transaction) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;

    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    Insert(index_key, rid, transaction);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string &file_name, Transaction *transaction) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;
    KeyType index_key;
    index_key.SetFromInteger(key);
    Remove(index_key, transaction);
  }
}

/**
 * This method is used for debug only, You don't  need to modify
 * @tparam KeyType
 * @tparam ValueType
 * @tparam KeyComparator
 * @param page
 * @param bpm
 * @param out
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToGraph(BPlusTreePage *page, BufferPoolManager *bpm, std::ofstream &out) const {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    LeafPage *leaf = reinterpret_cast<LeafPage *>(page);
    // Print node name
    out << leaf_prefix << leaf->GetPageId();
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << leaf->GetPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">"
        << "max_size=" << leaf->GetMaxSize() << ",min_size=" << leaf->GetMinSize() << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      out << "<TD>" << leaf->KeyAt(i) << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << leaf->GetPageId() << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << leaf->GetPageId() << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }

    // Print parent links if there is a parent
    if (leaf->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << leaf->GetParentPageId() << ":p" << leaf->GetPageId() << " -> " << leaf_prefix
          << leaf->GetPageId() << ";\n";
    }
  } else {
    InternalPage *inner = reinterpret_cast<InternalPage *>(page);
    // Print node name
    out << internal_prefix << inner->GetPageId();
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << inner->GetPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">"
        << "max_size=" << inner->GetMaxSize() << ",min_size=" << inner->GetMinSize() << "</TD></TR>\n";
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
    // Print Parent link
    if (inner->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << inner->GetParentPageId() << ":p" << inner->GetPageId() << " -> " << internal_prefix
          << inner->GetPageId() << ";\n";
    }
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(child_page, bpm, out);
      if (i > 0) {
        auto sibling_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i - 1))->GetData());
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_page->GetPageId() << " " << internal_prefix
              << child_page->GetPageId() << "};\n";
        }
        bpm->UnpinPage(sibling_page->GetPageId(), false);
      }
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

/**
 * This function is for debug only, you don't need to modify
 * @tparam KeyType
 * @tparam ValueType
 * @tparam KeyComparator
 * @param page
 * @param bpm
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToString(BPlusTreePage *page, BufferPoolManager *bpm) const {
  if (page->IsLeafPage()) {
    LeafPage *leaf = reinterpret_cast<LeafPage *>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId()
              << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  } else {
    InternalPage *internal = reinterpret_cast<InternalPage *>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(internal->ValueAt(i))->GetData()), bpm);
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;
template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;
template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;
template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;
template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
