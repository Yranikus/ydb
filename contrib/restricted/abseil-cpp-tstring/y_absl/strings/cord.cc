// Copyright 2020 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "y_absl/strings/cord.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <ios>
#include <iostream>
#include <limits>
#include <ostream>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "y_absl/base/casts.h"
#include "y_absl/base/internal/raw_logging.h"
#include "y_absl/base/macros.h"
#include "y_absl/base/port.h"
#include "y_absl/container/fixed_array.h"
#include "y_absl/container/inlined_vector.h"
#include "y_absl/crc/internal/crc_cord_state.h"
#include "y_absl/strings/cord_buffer.h"
#include "y_absl/strings/escaping.h"
#include "y_absl/strings/internal/cord_data_edge.h"
#include "y_absl/strings/internal/cord_internal.h"
#include "y_absl/strings/internal/cord_rep_btree.h"
#include "y_absl/strings/internal/cord_rep_crc.h"
#include "y_absl/strings/internal/cord_rep_flat.h"
#include "y_absl/strings/internal/cordz_statistics.h"
#include "y_absl/strings/internal/cordz_update_scope.h"
#include "y_absl/strings/internal/cordz_update_tracker.h"
#include "y_absl/strings/internal/resize_uninitialized.h"
#include "y_absl/strings/str_cat.h"
#include "y_absl/strings/str_format.h"
#include "y_absl/strings/str_join.h"
#include "y_absl/strings/string_view.h"

namespace y_absl {
Y_ABSL_NAMESPACE_BEGIN

using ::y_absl::cord_internal::CordRep;
using ::y_absl::cord_internal::CordRepBtree;
using ::y_absl::cord_internal::CordRepCrc;
using ::y_absl::cord_internal::CordRepExternal;
using ::y_absl::cord_internal::CordRepFlat;
using ::y_absl::cord_internal::CordRepSubstring;
using ::y_absl::cord_internal::CordzUpdateTracker;
using ::y_absl::cord_internal::InlineData;
using ::y_absl::cord_internal::kMaxFlatLength;
using ::y_absl::cord_internal::kMinFlatLength;

using ::y_absl::cord_internal::kInlinedVectorSize;
using ::y_absl::cord_internal::kMaxBytesToCopy;

static void DumpNode(CordRep* rep, bool include_data, std::ostream* os,
                     int indent = 0);
static bool VerifyNode(CordRep* root, CordRep* start_node,
                       bool full_validation);

static inline CordRep* VerifyTree(CordRep* node) {
  // Verification is expensive, so only do it in debug mode.
  // Even in debug mode we normally do only light validation.
  // If you are debugging Cord itself, you should define the
  // macro EXTRA_CORD_VALIDATION, e.g. by adding
  // --copt=-DEXTRA_CORD_VALIDATION to the blaze line.
#ifdef EXTRA_CORD_VALIDATION
  assert(node == nullptr || VerifyNode(node, node, /*full_validation=*/true));
#else   // EXTRA_CORD_VALIDATION
  assert(node == nullptr || VerifyNode(node, node, /*full_validation=*/false));
#endif  // EXTRA_CORD_VALIDATION
  static_cast<void>(&VerifyNode);

  return node;
}

static CordRepFlat* CreateFlat(const char* data, size_t length,
                               size_t alloc_hint) {
  CordRepFlat* flat = CordRepFlat::New(length + alloc_hint);
  flat->length = length;
  memcpy(flat->Data(), data, length);
  return flat;
}

// Creates a new flat or Btree out of the specified array.
// The returned node has a refcount of 1.
static CordRep* NewBtree(const char* data, size_t length, size_t alloc_hint) {
  if (length <= kMaxFlatLength) {
    return CreateFlat(data, length, alloc_hint);
  }
  CordRepFlat* flat = CreateFlat(data, kMaxFlatLength, 0);
  data += kMaxFlatLength;
  length -= kMaxFlatLength;
  auto* root = CordRepBtree::Create(flat);
  return CordRepBtree::Append(root, {data, length}, alloc_hint);
}

// Create a new tree out of the specified array.
// The returned node has a refcount of 1.
static CordRep* NewTree(const char* data, size_t length, size_t alloc_hint) {
  if (length == 0) return nullptr;
  return NewBtree(data, length, alloc_hint);
}

namespace cord_internal {

void InitializeCordRepExternal(y_absl::string_view data, CordRepExternal* rep) {
  assert(!data.empty());
  rep->length = data.size();
  rep->tag = EXTERNAL;
  rep->base = data.data();
  VerifyTree(rep);
}

}  // namespace cord_internal

// Creates a CordRep from the provided string. If the string is large enough,
// and not wasteful, we move the string into an external cord rep, preserving
// the already allocated string contents.
// Requires the provided string length to be larger than `kMaxInline`.
static CordRep* CordRepFromString(TString&& src) {
  assert(src.length() > cord_internal::kMaxInline);
  if (
      // String is short: copy data to avoid external block overhead.
      src.size() <= kMaxBytesToCopy ||
      // String is wasteful: copy data to avoid pinning too much unused memory.
      src.size() < src.capacity() / 2
  ) {
    return NewTree(src.data(), src.size(), 0);
  }

  struct StringReleaser {
    void operator()(y_absl::string_view /* data */) {}
    TString data;
  };
  const y_absl::string_view original_data = src;
  auto* rep =
      static_cast<::y_absl::cord_internal::CordRepExternalImpl<StringReleaser>*>(
          y_absl::cord_internal::NewExternalRep(original_data,
                                              StringReleaser{std::move(src)}));
  // Moving src may have invalidated its data pointer, so adjust it.
  rep->base = rep->template get<0>().data.data();
  return rep;
}

// --------------------------------------------------------------------
// Cord::InlineRep functions

#ifdef Y_ABSL_INTERNAL_NEED_REDUNDANT_CONSTEXPR_DECL
constexpr unsigned char Cord::InlineRep::kMaxInline;
#endif

inline void Cord::InlineRep::set_data(const char* data, size_t n) {
  static_assert(kMaxInline == 15, "set_data is hard-coded for a length of 15");
  data_.set_inline_data(data, n);
}

inline char* Cord::InlineRep::set_data(size_t n) {
  assert(n <= kMaxInline);
  ResetToEmpty();
  set_inline_size(n);
  return data_.as_chars();
}

inline void Cord::InlineRep::reduce_size(size_t n) {
  size_t tag = inline_size();
  assert(tag <= kMaxInline);
  assert(tag >= n);
  tag -= n;
  memset(data_.as_chars() + tag, 0, n);
  set_inline_size(tag);
}

inline void Cord::InlineRep::remove_prefix(size_t n) {
  cord_internal::SmallMemmove(data_.as_chars(), data_.as_chars() + n,
                              inline_size() - n);
  reduce_size(n);
}

// Returns `rep` converted into a CordRepBtree.
// Directly returns `rep` if `rep` is already a CordRepBtree.
static CordRepBtree* ForceBtree(CordRep* rep) {
  return rep->IsBtree()
             ? rep->btree()
             : CordRepBtree::Create(cord_internal::RemoveCrcNode(rep));
}

void Cord::InlineRep::AppendTreeToInlined(CordRep* tree,
                                          MethodIdentifier method) {
  assert(!is_tree());
  if (!data_.is_empty()) {
    CordRepFlat* flat = MakeFlatWithExtraCapacity(0);
    tree = CordRepBtree::Append(CordRepBtree::Create(flat), tree);
  }
  EmplaceTree(tree, method);
}

void Cord::InlineRep::AppendTreeToTree(CordRep* tree, MethodIdentifier method) {
  assert(is_tree());
  const CordzUpdateScope scope(data_.cordz_info(), method);
  tree = CordRepBtree::Append(ForceBtree(data_.as_tree()), tree);
  SetTree(tree, scope);
}

void Cord::InlineRep::AppendTree(CordRep* tree, MethodIdentifier method) {
  assert(tree != nullptr);
  assert(tree->length != 0);
  assert(!tree->IsCrc());
  if (data_.is_tree()) {
    AppendTreeToTree(tree, method);
  } else {
    AppendTreeToInlined(tree, method);
  }
}

void Cord::InlineRep::PrependTreeToInlined(CordRep* tree,
                                           MethodIdentifier method) {
  assert(!is_tree());
  if (!data_.is_empty()) {
    CordRepFlat* flat = MakeFlatWithExtraCapacity(0);
    tree = CordRepBtree::Prepend(CordRepBtree::Create(flat), tree);
  }
  EmplaceTree(tree, method);
}

void Cord::InlineRep::PrependTreeToTree(CordRep* tree,
                                        MethodIdentifier method) {
  assert(is_tree());
  const CordzUpdateScope scope(data_.cordz_info(), method);
  tree = CordRepBtree::Prepend(ForceBtree(data_.as_tree()), tree);
  SetTree(tree, scope);
}

void Cord::InlineRep::PrependTree(CordRep* tree, MethodIdentifier method) {
  assert(tree != nullptr);
  assert(tree->length != 0);
  assert(!tree->IsCrc());
  if (data_.is_tree()) {
    PrependTreeToTree(tree, method);
  } else {
    PrependTreeToInlined(tree, method);
  }
}

// Searches for a non-full flat node at the rightmost leaf of the tree. If a
// suitable leaf is found, the function will update the length field for all
// nodes to account for the size increase. The append region address will be
// written to region and the actual size increase will be written to size.
static inline bool PrepareAppendRegion(CordRep* root, char** region,
                                       size_t* size, size_t max_length) {
  if (root->IsBtree() && root->refcount.IsOne()) {
    Span<char> span = root->btree()->GetAppendBuffer(max_length);
    if (!span.empty()) {
      *region = span.data();
      *size = span.size();
      return true;
    }
  }

  CordRep* dst = root;
  if (!dst->IsFlat() || !dst->refcount.IsOne()) {
    *region = nullptr;
    *size = 0;
    return false;
  }

  const size_t in_use = dst->length;
  const size_t capacity = dst->flat()->Capacity();
  if (in_use == capacity) {
    *region = nullptr;
    *size = 0;
    return false;
  }

  const size_t size_increase = std::min(capacity - in_use, max_length);
  dst->length += size_increase;

  *region = dst->flat()->Data() + in_use;
  *size = size_increase;
  return true;
}

void Cord::InlineRep::AssignSlow(const Cord::InlineRep& src) {
  assert(&src != this);
  assert(is_tree() || src.is_tree());
  auto constexpr method = CordzUpdateTracker::kAssignCord;
  if (Y_ABSL_PREDICT_TRUE(!is_tree())) {
    EmplaceTree(CordRep::Ref(src.as_tree()), src.data_, method);
    return;
  }

  CordRep* tree = as_tree();
  if (CordRep* src_tree = src.tree()) {
    // Leave any existing `cordz_info` in place, and let MaybeTrackCord()
    // decide if this cord should be (or remains to be) sampled or not.
    data_.set_tree(CordRep::Ref(src_tree));
    CordzInfo::MaybeTrackCord(data_, src.data_, method);
  } else {
    CordzInfo::MaybeUntrackCord(data_.cordz_info());
    data_ = src.data_;
  }
  CordRep::Unref(tree);
}

void Cord::InlineRep::UnrefTree() {
  if (is_tree()) {
    CordzInfo::MaybeUntrackCord(data_.cordz_info());
    CordRep::Unref(tree());
  }
}

// --------------------------------------------------------------------
// Constructors and destructors

Cord::Cord(y_absl::string_view src, MethodIdentifier method)
    : contents_(InlineData::kDefaultInit) {
  const size_t n = src.size();
  if (n <= InlineRep::kMaxInline) {
    contents_.set_data(src.data(), n);
  } else {
    CordRep* rep = NewTree(src.data(), n, 0);
    contents_.EmplaceTree(rep, method);
  }
}

template <typename T, Cord::EnableIfString<T>>
Cord::Cord(T&& src) : contents_(InlineData::kDefaultInit) {
  if (src.size() <= InlineRep::kMaxInline) {
    contents_.set_data(src.data(), src.size());
  } else {
    CordRep* rep = CordRepFromString(std::forward<T>(src));
    contents_.EmplaceTree(rep, CordzUpdateTracker::kConstructorString);
  }
}

template Cord::Cord(TString&& src);

// The destruction code is separate so that the compiler can determine
// that it does not need to call the destructor on a moved-from Cord.
void Cord::DestroyCordSlow() {
  assert(contents_.is_tree());
  CordzInfo::MaybeUntrackCord(contents_.cordz_info());
  CordRep::Unref(VerifyTree(contents_.as_tree()));
}

// --------------------------------------------------------------------
// Mutators

void Cord::Clear() {
  if (CordRep* tree = contents_.clear()) {
    CordRep::Unref(tree);
  }
}

Cord& Cord::AssignLargeString(TString&& src) {
  auto constexpr method = CordzUpdateTracker::kAssignString;
  assert(src.size() > kMaxBytesToCopy);
  CordRep* rep = CordRepFromString(std::move(src));
  if (CordRep* tree = contents_.tree()) {
    CordzUpdateScope scope(contents_.cordz_info(), method);
    contents_.SetTree(rep, scope);
    CordRep::Unref(tree);
  } else {
    contents_.EmplaceTree(rep, method);
  }
  return *this;
}

Cord& Cord::operator=(y_absl::string_view src) {
  auto constexpr method = CordzUpdateTracker::kAssignString;
  const char* data = src.data();
  size_t length = src.size();
  CordRep* tree = contents_.tree();
  if (length <= InlineRep::kMaxInline) {
    // Embed into this->contents_, which is somewhat subtle:
    // - MaybeUntrackCord must be called before Unref(tree).
    // - MaybeUntrackCord must be called before set_data() clobbers cordz_info.
    // - set_data() must be called before Unref(tree) as it may reference tree.
    if (tree != nullptr) CordzInfo::MaybeUntrackCord(contents_.cordz_info());
    contents_.set_data(data, length);
    if (tree != nullptr) CordRep::Unref(tree);
    return *this;
  }
  if (tree != nullptr) {
    CordzUpdateScope scope(contents_.cordz_info(), method);
    if (tree->IsFlat() && tree->flat()->Capacity() >= length &&
        tree->refcount.IsOne()) {
      // Copy in place if the existing FLAT node is reusable.
      memmove(tree->flat()->Data(), data, length);
      tree->length = length;
      VerifyTree(tree);
      return *this;
    }
    contents_.SetTree(NewTree(data, length, 0), scope);
    CordRep::Unref(tree);
  } else {
    contents_.EmplaceTree(NewTree(data, length, 0), method);
  }
  return *this;
}

// TODO(sanjay): Move to Cord::InlineRep section of file.  For now,
// we keep it here to make diffs easier.
void Cord::InlineRep::AppendArray(y_absl::string_view src,
                                  MethodIdentifier method) {
  MaybeRemoveEmptyCrcNode();
  if (src.empty()) return;  // memcpy(_, nullptr, 0) is undefined.

  size_t appended = 0;
  CordRep* rep = tree();
  const CordRep* const root = rep;
  CordzUpdateScope scope(root ? cordz_info() : nullptr, method);
  if (root != nullptr) {
    rep = cord_internal::RemoveCrcNode(rep);
    char* region;
    if (PrepareAppendRegion(rep, &region, &appended, src.size())) {
      memcpy(region, src.data(), appended);
    }
  } else {
    // Try to fit in the inline buffer if possible.
    size_t inline_length = inline_size();
    if (src.size() <= kMaxInline - inline_length) {
      // Append new data to embedded array
      set_inline_size(inline_length + src.size());
      memcpy(data_.as_chars() + inline_length, src.data(), src.size());
      return;
    }

    // Allocate flat to be a perfect fit on first append exceeding inlined size.
    // Subsequent growth will use amortized growth until we reach maximum flat
    // size.
    rep = CordRepFlat::New(inline_length + src.size());
    appended = std::min(src.size(), rep->flat()->Capacity() - inline_length);
    memcpy(rep->flat()->Data(), data_.as_chars(), inline_length);
    memcpy(rep->flat()->Data() + inline_length, src.data(), appended);
    rep->length = inline_length + appended;
  }

  src.remove_prefix(appended);
  if (src.empty()) {
    CommitTree(root, rep, scope, method);
    return;
  }

  // TODO(b/192061034): keep legacy 10% growth rate: consider other rates.
  rep = ForceBtree(rep);
  const size_t min_growth = std::max<size_t>(rep->length / 10, src.size());
  rep = CordRepBtree::Append(rep->btree(), src, min_growth - src.size());

  CommitTree(root, rep, scope, method);
}

inline CordRep* Cord::TakeRep() const& {
  return CordRep::Ref(contents_.tree());
}

inline CordRep* Cord::TakeRep() && {
  CordRep* rep = contents_.tree();
  contents_.clear();
  return rep;
}

template <typename C>
inline void Cord::AppendImpl(C&& src) {
  auto constexpr method = CordzUpdateTracker::kAppendCord;

  contents_.MaybeRemoveEmptyCrcNode();
  if (src.empty()) return;

  if (empty()) {
    // Since destination is empty, we can avoid allocating a node,
    if (src.contents_.is_tree()) {
      // by taking the tree directly
      CordRep* rep =
          cord_internal::RemoveCrcNode(std::forward<C>(src).TakeRep());
      contents_.EmplaceTree(rep, method);
    } else {
      // or copying over inline data
      contents_.data_ = src.contents_.data_;
    }
    return;
  }

  // For short cords, it is faster to copy data if there is room in dst.
  const size_t src_size = src.contents_.size();
  if (src_size <= kMaxBytesToCopy) {
    CordRep* src_tree = src.contents_.tree();
    if (src_tree == nullptr) {
      // src has embedded data.
      contents_.AppendArray({src.contents_.data(), src_size}, method);
      return;
    }
    if (src_tree->IsFlat()) {
      // src tree just has one flat node.
      contents_.AppendArray({src_tree->flat()->Data(), src_size}, method);
      return;
    }
    if (&src == this) {
      // ChunkIterator below assumes that src is not modified during traversal.
      Append(Cord(src));
      return;
    }
    // TODO(mec): Should we only do this if "dst" has space?
    for (y_absl::string_view chunk : src.Chunks()) {
      Append(chunk);
    }
    return;
  }

  // Guaranteed to be a tree (kMaxBytesToCopy > kInlinedSize)
  CordRep* rep = cord_internal::RemoveCrcNode(std::forward<C>(src).TakeRep());
  contents_.AppendTree(rep, CordzUpdateTracker::kAppendCord);
}

static CordRep::ExtractResult ExtractAppendBuffer(CordRep* rep,
                                                  size_t min_capacity) {
  switch (rep->tag) {
    case cord_internal::BTREE:
      return CordRepBtree::ExtractAppendBuffer(rep->btree(), min_capacity);
    default:
      if (rep->IsFlat() && rep->refcount.IsOne() &&
          rep->flat()->Capacity() - rep->length >= min_capacity) {
        return {nullptr, rep};
      }
      return {rep, nullptr};
  }
}

static CordBuffer CreateAppendBuffer(InlineData& data, size_t block_size,
                                     size_t capacity) {
  // Watch out for overflow, people can ask for size_t::max().
  const size_t size = data.inline_size();
  const size_t max_capacity = std::numeric_limits<size_t>::max() - size;
  capacity = (std::min)(max_capacity, capacity) + size;
  CordBuffer buffer =
      block_size ? CordBuffer::CreateWithCustomLimit(block_size, capacity)
                 : CordBuffer::CreateWithDefaultLimit(capacity);
  cord_internal::SmallMemmove(buffer.data(), data.as_chars(), size);
  buffer.SetLength(size);
  data = {};
  return buffer;
}

CordBuffer Cord::GetAppendBufferSlowPath(size_t block_size, size_t capacity,
                                         size_t min_capacity) {
  auto constexpr method = CordzUpdateTracker::kGetAppendBuffer;
  CordRep* tree = contents_.tree();
  if (tree != nullptr) {
    CordzUpdateScope scope(contents_.cordz_info(), method);
    CordRep::ExtractResult result = ExtractAppendBuffer(tree, min_capacity);
    if (result.extracted != nullptr) {
      contents_.SetTreeOrEmpty(result.tree, scope);
      return CordBuffer(result.extracted->flat());
    }
    return block_size ? CordBuffer::CreateWithCustomLimit(block_size, capacity)
                      : CordBuffer::CreateWithDefaultLimit(capacity);
  }
  return CreateAppendBuffer(contents_.data_, block_size, capacity);
}

void Cord::Append(const Cord& src) {
  AppendImpl(src);
}

void Cord::Append(Cord&& src) {
  AppendImpl(std::move(src));
}

template <typename T, Cord::EnableIfString<T>>
void Cord::Append(T&& src) {
  if (src.size() <= kMaxBytesToCopy) {
    Append(y_absl::string_view(src));
  } else {
    CordRep* rep = CordRepFromString(std::forward<T>(src));
    contents_.AppendTree(rep, CordzUpdateTracker::kAppendString);
  }
}

template void Cord::Append(TString&& src);

void Cord::Prepend(const Cord& src) {
  contents_.MaybeRemoveEmptyCrcNode();
  if (src.empty()) return;

  CordRep* src_tree = src.contents_.tree();
  if (src_tree != nullptr) {
    CordRep::Ref(src_tree);
    contents_.PrependTree(cord_internal::RemoveCrcNode(src_tree),
                          CordzUpdateTracker::kPrependCord);
    return;
  }

  // `src` cord is inlined.
  y_absl::string_view src_contents(src.contents_.data(), src.contents_.size());
  return Prepend(src_contents);
}

void Cord::PrependArray(y_absl::string_view src, MethodIdentifier method) {
  contents_.MaybeRemoveEmptyCrcNode();
  if (src.empty()) return;  // memcpy(_, nullptr, 0) is undefined.

  if (!contents_.is_tree()) {
    size_t cur_size = contents_.inline_size();
    if (cur_size + src.size() <= InlineRep::kMaxInline) {
      // Use embedded storage.
      InlineData data;
      data.set_inline_size(cur_size + src.size());
      memcpy(data.as_chars(), src.data(), src.size());
      memcpy(data.as_chars() + src.size(), contents_.data(), cur_size);
      contents_.data_ = data;
      return;
    }
  }
  CordRep* rep = NewTree(src.data(), src.size(), 0);
  contents_.PrependTree(rep, method);
}

void Cord::AppendPrecise(y_absl::string_view src, MethodIdentifier method) {
  assert(!src.empty());
  assert(src.size() <= cord_internal::kMaxFlatLength);
  if (contents_.remaining_inline_capacity() >= src.size()) {
    const size_t inline_length = contents_.inline_size();
    contents_.set_inline_size(inline_length + src.size());
    memcpy(contents_.data_.as_chars() + inline_length, src.data(), src.size());
  } else {
    contents_.AppendTree(CordRepFlat::Create(src), method);
  }
}

void Cord::PrependPrecise(y_absl::string_view src, MethodIdentifier method) {
  assert(!src.empty());
  assert(src.size() <= cord_internal::kMaxFlatLength);
  if (contents_.remaining_inline_capacity() >= src.size()) {
    const size_t cur_size = contents_.inline_size();
    InlineData data;
    data.set_inline_size(cur_size + src.size());
    memcpy(data.as_chars(), src.data(), src.size());
    memcpy(data.as_chars() + src.size(), contents_.data(), cur_size);
    contents_.data_ = data;
  } else {
    contents_.PrependTree(CordRepFlat::Create(src), method);
  }
}

template <typename T, Cord::EnableIfString<T>>
inline void Cord::Prepend(T&& src) {
  if (src.size() <= kMaxBytesToCopy) {
    Prepend(y_absl::string_view(src));
  } else {
    CordRep* rep = CordRepFromString(std::forward<T>(src));
    contents_.PrependTree(rep, CordzUpdateTracker::kPrependString);
  }
}

template void Cord::Prepend(TString&& src);

void Cord::RemovePrefix(size_t n) {
  Y_ABSL_INTERNAL_CHECK(n <= size(),
                      y_absl::StrCat("Requested prefix size ", n,
                                   " exceeds Cord's size ", size()));
  contents_.MaybeRemoveEmptyCrcNode();
  CordRep* tree = contents_.tree();
  if (tree == nullptr) {
    contents_.remove_prefix(n);
  } else {
    auto constexpr method = CordzUpdateTracker::kRemovePrefix;
    CordzUpdateScope scope(contents_.cordz_info(), method);
    tree = cord_internal::RemoveCrcNode(tree);
    if (n >= tree->length) {
      CordRep::Unref(tree);
      tree = nullptr;
    } else if (tree->IsBtree()) {
      CordRep* old = tree;
      tree = tree->btree()->SubTree(n, tree->length - n);
      CordRep::Unref(old);
    } else if (tree->IsSubstring() && tree->refcount.IsOne()) {
      tree->substring()->start += n;
      tree->length -= n;
    } else {
      CordRep* rep = CordRepSubstring::Substring(tree, n, tree->length - n);
      CordRep::Unref(tree);
      tree = rep;
    }
    contents_.SetTreeOrEmpty(tree, scope);
  }
}

void Cord::RemoveSuffix(size_t n) {
  Y_ABSL_INTERNAL_CHECK(n <= size(),
                      y_absl::StrCat("Requested suffix size ", n,
                                   " exceeds Cord's size ", size()));
  contents_.MaybeRemoveEmptyCrcNode();
  CordRep* tree = contents_.tree();
  if (tree == nullptr) {
    contents_.reduce_size(n);
  } else {
    auto constexpr method = CordzUpdateTracker::kRemoveSuffix;
    CordzUpdateScope scope(contents_.cordz_info(), method);
    tree = cord_internal::RemoveCrcNode(tree);
    if (n >= tree->length) {
      CordRep::Unref(tree);
      tree = nullptr;
    } else if (tree->IsBtree()) {
      tree = CordRepBtree::RemoveSuffix(tree->btree(), n);
    } else if (!tree->IsExternal() && tree->refcount.IsOne()) {
      assert(tree->IsFlat() || tree->IsSubstring());
      tree->length -= n;
    } else {
      CordRep* rep = CordRepSubstring::Substring(tree, 0, tree->length - n);
      CordRep::Unref(tree);
      tree = rep;
    }
    contents_.SetTreeOrEmpty(tree, scope);
  }
}

Cord Cord::Subcord(size_t pos, size_t new_size) const {
  Cord sub_cord;
  size_t length = size();
  if (pos > length) pos = length;
  if (new_size > length - pos) new_size = length - pos;
  if (new_size == 0) return sub_cord;

  CordRep* tree = contents_.tree();
  if (tree == nullptr) {
    sub_cord.contents_.set_data(contents_.data() + pos, new_size);
    return sub_cord;
  }

  if (new_size <= InlineRep::kMaxInline) {
    sub_cord.contents_.set_inline_size(new_size);
    char* dest = sub_cord.contents_.data_.as_chars();
    Cord::ChunkIterator it = chunk_begin();
    it.AdvanceBytes(pos);
    size_t remaining_size = new_size;
    while (remaining_size > it->size()) {
      cord_internal::SmallMemmove(dest, it->data(), it->size());
      remaining_size -= it->size();
      dest += it->size();
      ++it;
    }
    cord_internal::SmallMemmove(dest, it->data(), remaining_size);
    return sub_cord;
  }

  tree = cord_internal::SkipCrcNode(tree);
  if (tree->IsBtree()) {
    tree = tree->btree()->SubTree(pos, new_size);
  } else {
    tree = CordRepSubstring::Substring(tree, pos, new_size);
  }
  sub_cord.contents_.EmplaceTree(tree, contents_.data_,
                                 CordzUpdateTracker::kSubCord);
  return sub_cord;
}

// --------------------------------------------------------------------
// Comparators

namespace {

int ClampResult(int memcmp_res) {
  return static_cast<int>(memcmp_res > 0) - static_cast<int>(memcmp_res < 0);
}

int CompareChunks(y_absl::string_view* lhs, y_absl::string_view* rhs,
                  size_t* size_to_compare) {
  size_t compared_size = std::min(lhs->size(), rhs->size());
  assert(*size_to_compare >= compared_size);
  *size_to_compare -= compared_size;

  int memcmp_res = ::memcmp(lhs->data(), rhs->data(), compared_size);
  if (memcmp_res != 0) return memcmp_res;

  lhs->remove_prefix(compared_size);
  rhs->remove_prefix(compared_size);

  return 0;
}

// This overload set computes comparison results from memcmp result. This
// interface is used inside GenericCompare below. Differet implementations
// are specialized for int and bool. For int we clamp result to {-1, 0, 1}
// set. For bool we just interested in "value == 0".
template <typename ResultType>
ResultType ComputeCompareResult(int memcmp_res) {
  return ClampResult(memcmp_res);
}
template <>
bool ComputeCompareResult<bool>(int memcmp_res) {
  return memcmp_res == 0;
}

}  // namespace

// Helper routine. Locates the first flat or external chunk of the Cord without
// initializing the iterator, and returns a string_view referencing the data.
inline y_absl::string_view Cord::InlineRep::FindFlatStartPiece() const {
  if (!is_tree()) {
    return y_absl::string_view(data_.as_chars(), data_.inline_size());
  }

  CordRep* node = cord_internal::SkipCrcNode(tree());
  if (node->IsFlat()) {
    return y_absl::string_view(node->flat()->Data(), node->length);
  }

  if (node->IsExternal()) {
    return y_absl::string_view(node->external()->base, node->length);
  }

  if (node->IsBtree()) {
    CordRepBtree* tree = node->btree();
    int height = tree->height();
    while (--height >= 0) {
      tree = tree->Edge(CordRepBtree::kFront)->btree();
    }
    return tree->Data(tree->begin());
  }

  // Get the child node if we encounter a SUBSTRING.
  size_t offset = 0;
  size_t length = node->length;
  assert(length != 0);

  if (node->IsSubstring()) {
    offset = node->substring()->start;
    node = node->substring()->child;
  }

  if (node->IsFlat()) {
    return y_absl::string_view(node->flat()->Data() + offset, length);
  }

  assert(node->IsExternal() && "Expect FLAT or EXTERNAL node here");

  return y_absl::string_view(node->external()->base + offset, length);
}

void Cord::SetCrcCordState(crc_internal::CrcCordState state) {
  auto constexpr method = CordzUpdateTracker::kSetExpectedChecksum;
  if (empty()) {
    contents_.MaybeRemoveEmptyCrcNode();
    CordRep* rep = CordRepCrc::New(nullptr, std::move(state));
    contents_.EmplaceTree(rep, method);
  } else if (!contents_.is_tree()) {
    CordRep* rep = contents_.MakeFlatWithExtraCapacity(0);
    rep = CordRepCrc::New(rep, std::move(state));
    contents_.EmplaceTree(rep, method);
  } else {
    const CordzUpdateScope scope(contents_.data_.cordz_info(), method);
    CordRep* rep = CordRepCrc::New(contents_.data_.as_tree(), std::move(state));
    contents_.SetTree(rep, scope);
  }
}

void Cord::SetExpectedChecksum(uint32_t crc) {
  // Construct a CrcCordState with a single chunk.
  crc_internal::CrcCordState state;
  state.mutable_rep()->prefix_crc.push_back(
      crc_internal::CrcCordState::PrefixCrc(size(), y_absl::crc32c_t{crc}));
  SetCrcCordState(std::move(state));
}

const crc_internal::CrcCordState* Cord::MaybeGetCrcCordState() const {
  if (!contents_.is_tree() || !contents_.tree()->IsCrc()) {
    return nullptr;
  }
  return &contents_.tree()->crc()->crc_cord_state;
}

y_absl::optional<uint32_t> Cord::ExpectedChecksum() const {
  if (!contents_.is_tree() || !contents_.tree()->IsCrc()) {
    return y_absl::nullopt;
  }
  return static_cast<uint32_t>(
      contents_.tree()->crc()->crc_cord_state.Checksum());
}

inline int Cord::CompareSlowPath(y_absl::string_view rhs, size_t compared_size,
                                 size_t size_to_compare) const {
  auto advance = [](Cord::ChunkIterator* it, y_absl::string_view* chunk) {
    if (!chunk->empty()) return true;
    ++*it;
    if (it->bytes_remaining_ == 0) return false;
    *chunk = **it;
    return true;
  };

  Cord::ChunkIterator lhs_it = chunk_begin();

  // compared_size is inside first chunk.
  y_absl::string_view lhs_chunk =
      (lhs_it.bytes_remaining_ != 0) ? *lhs_it : y_absl::string_view();
  assert(compared_size <= lhs_chunk.size());
  assert(compared_size <= rhs.size());
  lhs_chunk.remove_prefix(compared_size);
  rhs.remove_prefix(compared_size);
  size_to_compare -= compared_size;  // skip already compared size.

  while (advance(&lhs_it, &lhs_chunk) && !rhs.empty()) {
    int comparison_result = CompareChunks(&lhs_chunk, &rhs, &size_to_compare);
    if (comparison_result != 0) return comparison_result;
    if (size_to_compare == 0) return 0;
  }

  return static_cast<int>(rhs.empty()) - static_cast<int>(lhs_chunk.empty());
}

inline int Cord::CompareSlowPath(const Cord& rhs, size_t compared_size,
                                 size_t size_to_compare) const {
  auto advance = [](Cord::ChunkIterator* it, y_absl::string_view* chunk) {
    if (!chunk->empty()) return true;
    ++*it;
    if (it->bytes_remaining_ == 0) return false;
    *chunk = **it;
    return true;
  };

  Cord::ChunkIterator lhs_it = chunk_begin();
  Cord::ChunkIterator rhs_it = rhs.chunk_begin();

  // compared_size is inside both first chunks.
  y_absl::string_view lhs_chunk =
      (lhs_it.bytes_remaining_ != 0) ? *lhs_it : y_absl::string_view();
  y_absl::string_view rhs_chunk =
      (rhs_it.bytes_remaining_ != 0) ? *rhs_it : y_absl::string_view();
  assert(compared_size <= lhs_chunk.size());
  assert(compared_size <= rhs_chunk.size());
  lhs_chunk.remove_prefix(compared_size);
  rhs_chunk.remove_prefix(compared_size);
  size_to_compare -= compared_size;  // skip already compared size.

  while (advance(&lhs_it, &lhs_chunk) && advance(&rhs_it, &rhs_chunk)) {
    int memcmp_res = CompareChunks(&lhs_chunk, &rhs_chunk, &size_to_compare);
    if (memcmp_res != 0) return memcmp_res;
    if (size_to_compare == 0) return 0;
  }

  return static_cast<int>(rhs_chunk.empty()) -
         static_cast<int>(lhs_chunk.empty());
}

inline y_absl::string_view Cord::GetFirstChunk(const Cord& c) {
  if (c.empty()) return {};
  return c.contents_.FindFlatStartPiece();
}
inline y_absl::string_view Cord::GetFirstChunk(y_absl::string_view sv) {
  return sv;
}

// Compares up to 'size_to_compare' bytes of 'lhs' with 'rhs'. It is assumed
// that 'size_to_compare' is greater that size of smallest of first chunks.
template <typename ResultType, typename RHS>
ResultType GenericCompare(const Cord& lhs, const RHS& rhs,
                          size_t size_to_compare) {
  y_absl::string_view lhs_chunk = Cord::GetFirstChunk(lhs);
  y_absl::string_view rhs_chunk = Cord::GetFirstChunk(rhs);

  size_t compared_size = std::min(lhs_chunk.size(), rhs_chunk.size());
  assert(size_to_compare >= compared_size);
  int memcmp_res = ::memcmp(lhs_chunk.data(), rhs_chunk.data(), compared_size);
  if (compared_size == size_to_compare || memcmp_res != 0) {
    return ComputeCompareResult<ResultType>(memcmp_res);
  }

  return ComputeCompareResult<ResultType>(
      lhs.CompareSlowPath(rhs, compared_size, size_to_compare));
}

bool Cord::EqualsImpl(y_absl::string_view rhs, size_t size_to_compare) const {
  return GenericCompare<bool>(*this, rhs, size_to_compare);
}

bool Cord::EqualsImpl(const Cord& rhs, size_t size_to_compare) const {
  return GenericCompare<bool>(*this, rhs, size_to_compare);
}

template <typename RHS>
inline int SharedCompareImpl(const Cord& lhs, const RHS& rhs) {
  size_t lhs_size = lhs.size();
  size_t rhs_size = rhs.size();
  if (lhs_size == rhs_size) {
    return GenericCompare<int>(lhs, rhs, lhs_size);
  }
  if (lhs_size < rhs_size) {
    auto data_comp_res = GenericCompare<int>(lhs, rhs, lhs_size);
    return data_comp_res == 0 ? -1 : data_comp_res;
  }

  auto data_comp_res = GenericCompare<int>(lhs, rhs, rhs_size);
  return data_comp_res == 0 ? +1 : data_comp_res;
}

int Cord::Compare(y_absl::string_view rhs) const {
  return SharedCompareImpl(*this, rhs);
}

int Cord::CompareImpl(const Cord& rhs) const {
  return SharedCompareImpl(*this, rhs);
}

bool Cord::EndsWith(y_absl::string_view rhs) const {
  size_t my_size = size();
  size_t rhs_size = rhs.size();

  if (my_size < rhs_size) return false;

  Cord tmp(*this);
  tmp.RemovePrefix(my_size - rhs_size);
  return tmp.EqualsImpl(rhs, rhs_size);
}

bool Cord::EndsWith(const Cord& rhs) const {
  size_t my_size = size();
  size_t rhs_size = rhs.size();

  if (my_size < rhs_size) return false;

  Cord tmp(*this);
  tmp.RemovePrefix(my_size - rhs_size);
  return tmp.EqualsImpl(rhs, rhs_size);
}

// --------------------------------------------------------------------
// Misc.

Cord::operator TString() const {
  TString s;
  y_absl::CopyCordToString(*this, &s);
  return s;
}

void CopyCordToString(const Cord& src, TString* dst) {
  if (!src.contents_.is_tree()) {
    src.contents_.CopyTo(dst);
  } else {
    y_absl::strings_internal::STLStringResizeUninitialized(dst, src.size());
    src.CopyToArraySlowPath(&(*dst)[0]);
  }
}

void Cord::CopyToArraySlowPath(char* dst) const {
  assert(contents_.is_tree());
  y_absl::string_view fragment;
  if (GetFlatAux(contents_.tree(), &fragment)) {
    memcpy(dst, fragment.data(), fragment.size());
    return;
  }
  for (y_absl::string_view chunk : Chunks()) {
    memcpy(dst, chunk.data(), chunk.size());
    dst += chunk.size();
  }
}

Cord Cord::ChunkIterator::AdvanceAndReadBytes(size_t n) {
  Y_ABSL_HARDENING_ASSERT(bytes_remaining_ >= n &&
                        "Attempted to iterate past `end()`");
  Cord subcord;
  auto constexpr method = CordzUpdateTracker::kCordReader;

  if (n <= InlineRep::kMaxInline) {
    // Range to read fits in inline data. Flatten it.
    char* data = subcord.contents_.set_data(n);
    while (n > current_chunk_.size()) {
      memcpy(data, current_chunk_.data(), current_chunk_.size());
      data += current_chunk_.size();
      n -= current_chunk_.size();
      ++*this;
    }
    memcpy(data, current_chunk_.data(), n);
    if (n < current_chunk_.size()) {
      RemoveChunkPrefix(n);
    } else if (n > 0) {
      ++*this;
    }
    return subcord;
  }

  if (btree_reader_) {
    size_t chunk_size = current_chunk_.size();
    if (n <= chunk_size && n <= kMaxBytesToCopy) {
      subcord = Cord(current_chunk_.substr(0, n), method);
      if (n < chunk_size) {
        current_chunk_.remove_prefix(n);
      } else {
        current_chunk_ = btree_reader_.Next();
      }
    } else {
      CordRep* rep;
      current_chunk_ = btree_reader_.Read(n, chunk_size, rep);
      subcord.contents_.EmplaceTree(rep, method);
    }
    bytes_remaining_ -= n;
    return subcord;
  }

  // Short circuit if reading the entire data edge.
  assert(current_leaf_ != nullptr);
  if (n == current_leaf_->length) {
    bytes_remaining_ = 0;
    current_chunk_ = {};
    CordRep* tree = CordRep::Ref(current_leaf_);
    subcord.contents_.EmplaceTree(VerifyTree(tree), method);
    return subcord;
  }

  // From this point on, we need a partial substring node.
  // Get pointer to the underlying flat or external data payload and
  // compute data pointer and offset into current flat or external.
  CordRep* payload = current_leaf_->IsSubstring()
                         ? current_leaf_->substring()->child
                         : current_leaf_;
  const char* data = payload->IsExternal() ? payload->external()->base
                                           : payload->flat()->Data();
  const size_t offset = static_cast<size_t>(current_chunk_.data() - data);

  auto* tree = CordRepSubstring::Substring(payload, offset, n);
  subcord.contents_.EmplaceTree(VerifyTree(tree), method);
  bytes_remaining_ -= n;
  current_chunk_.remove_prefix(n);
  return subcord;
}

char Cord::operator[](size_t i) const {
  Y_ABSL_HARDENING_ASSERT(i < size());
  size_t offset = i;
  const CordRep* rep = contents_.tree();
  if (rep == nullptr) {
    return contents_.data()[i];
  }
  rep = cord_internal::SkipCrcNode(rep);
  while (true) {
    assert(rep != nullptr);
    assert(offset < rep->length);
    if (rep->IsFlat()) {
      // Get the "i"th character directly from the flat array.
      return rep->flat()->Data()[offset];
    } else if (rep->IsBtree()) {
      return rep->btree()->GetCharacter(offset);
    } else if (rep->IsExternal()) {
      // Get the "i"th character from the external array.
      return rep->external()->base[offset];
    } else {
      // This must be a substring a node, so bypass it to get to the child.
      assert(rep->IsSubstring());
      offset += rep->substring()->start;
      rep = rep->substring()->child;
    }
  }
}

y_absl::string_view Cord::FlattenSlowPath() {
  assert(contents_.is_tree());
  size_t total_size = size();
  CordRep* new_rep;
  char* new_buffer;

  // Try to put the contents into a new flat rep. If they won't fit in the
  // biggest possible flat node, use an external rep instead.
  if (total_size <= kMaxFlatLength) {
    new_rep = CordRepFlat::New(total_size);
    new_rep->length = total_size;
    new_buffer = new_rep->flat()->Data();
    CopyToArraySlowPath(new_buffer);
  } else {
    new_buffer = std::allocator<char>().allocate(total_size);
    CopyToArraySlowPath(new_buffer);
    new_rep = y_absl::cord_internal::NewExternalRep(
        y_absl::string_view(new_buffer, total_size), [](y_absl::string_view s) {
          std::allocator<char>().deallocate(const_cast<char*>(s.data()),
                                            s.size());
        });
  }
  CordzUpdateScope scope(contents_.cordz_info(), CordzUpdateTracker::kFlatten);
  CordRep::Unref(contents_.as_tree());
  contents_.SetTree(new_rep, scope);
  return y_absl::string_view(new_buffer, total_size);
}

/* static */ bool Cord::GetFlatAux(CordRep* rep, y_absl::string_view* fragment) {
  assert(rep != nullptr);
  if (rep->length == 0) {
    *fragment = y_absl::string_view();
    return true;
  }
  rep = cord_internal::SkipCrcNode(rep);
  if (rep->IsFlat()) {
    *fragment = y_absl::string_view(rep->flat()->Data(), rep->length);
    return true;
  } else if (rep->IsExternal()) {
    *fragment = y_absl::string_view(rep->external()->base, rep->length);
    return true;
  } else if (rep->IsBtree()) {
    return rep->btree()->IsFlat(fragment);
  } else if (rep->IsSubstring()) {
    CordRep* child = rep->substring()->child;
    if (child->IsFlat()) {
      *fragment = y_absl::string_view(
          child->flat()->Data() + rep->substring()->start, rep->length);
      return true;
    } else if (child->IsExternal()) {
      *fragment = y_absl::string_view(
          child->external()->base + rep->substring()->start, rep->length);
      return true;
    } else if (child->IsBtree()) {
      return child->btree()->IsFlat(rep->substring()->start, rep->length,
                                    fragment);
    }
  }
  return false;
}

/* static */ void Cord::ForEachChunkAux(
    y_absl::cord_internal::CordRep* rep,
    y_absl::FunctionRef<void(y_absl::string_view)> callback) {
  assert(rep != nullptr);
  if (rep->length == 0) return;
  rep = cord_internal::SkipCrcNode(rep);

  if (rep->IsBtree()) {
    ChunkIterator it(rep), end;
    while (it != end) {
      callback(*it);
      ++it;
    }
    return;
  }

  // This is a leaf node, so invoke our callback.
  y_absl::cord_internal::CordRep* current_node = cord_internal::SkipCrcNode(rep);
  y_absl::string_view chunk;
  bool success = GetFlatAux(current_node, &chunk);
  assert(success);
  if (success) {
    callback(chunk);
  }
}

static void DumpNode(CordRep* rep, bool include_data, std::ostream* os,
                     int indent) {
  const int kIndentStep = 1;
  y_absl::InlinedVector<CordRep*, kInlinedVectorSize> stack;
  y_absl::InlinedVector<int, kInlinedVectorSize> indents;
  for (;;) {
    *os << std::setw(3) << rep->refcount.Get();
    *os << " " << std::setw(7) << rep->length;
    *os << " [";
    if (include_data) *os << static_cast<void*>(rep);
    *os << "]";
    *os << " " << std::setw(indent) << "";
    bool leaf = false;
    if (rep == nullptr) {
      *os << "NULL\n";
      leaf = true;
    } else if (rep->IsCrc()) {
      *os << "CRC crc=" << rep->crc()->crc_cord_state.Checksum() << "\n";
      indent += kIndentStep;
      rep = rep->crc()->child;
    } else if (rep->IsSubstring()) {
      *os << "SUBSTRING @ " << rep->substring()->start << "\n";
      indent += kIndentStep;
      rep = rep->substring()->child;
    } else {  // Leaf or ring
      leaf = true;
      if (rep->IsExternal()) {
        *os << "EXTERNAL [";
        if (include_data)
          *os << y_absl::CEscape(TString(rep->external()->base, rep->length));
        *os << "]\n";
      } else if (rep->IsFlat()) {
        *os << "FLAT cap=" << rep->flat()->Capacity() << " [";
        if (include_data)
          *os << y_absl::CEscape(TString(rep->flat()->Data(), rep->length));
        *os << "]\n";
      } else {
        CordRepBtree::Dump(rep, /*label=*/ "", include_data, *os);
      }
    }
    if (leaf) {
      if (stack.empty()) break;
      rep = stack.back();
      stack.pop_back();
      indent = indents.back();
      indents.pop_back();
    }
  }
  Y_ABSL_INTERNAL_CHECK(indents.empty(), "");
}

static TString ReportError(CordRep* root, CordRep* node) {
  std::ostringstream buf;
  buf << "Error at node " << node << " in:";
  DumpNode(root, true, &buf);
  return TString(buf.str());
}

static bool VerifyNode(CordRep* root, CordRep* start_node,
                       bool /* full_validation */) {
  y_absl::InlinedVector<CordRep*, 2> worklist;
  worklist.push_back(start_node);
  do {
    CordRep* node = worklist.back();
    worklist.pop_back();

    Y_ABSL_INTERNAL_CHECK(node != nullptr, ReportError(root, node));
    if (node != root) {
      Y_ABSL_INTERNAL_CHECK(node->length != 0, ReportError(root, node));
      Y_ABSL_INTERNAL_CHECK(!node->IsCrc(), ReportError(root, node));
    }

    if (node->IsFlat()) {
      Y_ABSL_INTERNAL_CHECK(node->length <= node->flat()->Capacity(),
                          ReportError(root, node));
    } else if (node->IsExternal()) {
      Y_ABSL_INTERNAL_CHECK(node->external()->base != nullptr,
                          ReportError(root, node));
    } else if (node->IsSubstring()) {
      Y_ABSL_INTERNAL_CHECK(
          node->substring()->start < node->substring()->child->length,
          ReportError(root, node));
      Y_ABSL_INTERNAL_CHECK(node->substring()->start + node->length <=
                              node->substring()->child->length,
                          ReportError(root, node));
    } else if (node->IsCrc()) {
      Y_ABSL_INTERNAL_CHECK(
          node->crc()->child != nullptr || node->crc()->length == 0,
          ReportError(root, node));
      if (node->crc()->child != nullptr) {
        Y_ABSL_INTERNAL_CHECK(node->crc()->length == node->crc()->child->length,
                            ReportError(root, node));
        worklist.push_back(node->crc()->child);
      }
    }
  } while (!worklist.empty());
  return true;
}

std::ostream& operator<<(std::ostream& out, const Cord& cord) {
  for (y_absl::string_view chunk : cord.Chunks()) {
    out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
  }
  return out;
}

namespace strings_internal {
size_t CordTestAccess::FlatOverhead() { return cord_internal::kFlatOverhead; }
size_t CordTestAccess::MaxFlatLength() { return cord_internal::kMaxFlatLength; }
size_t CordTestAccess::FlatTagToLength(uint8_t tag) {
  return cord_internal::TagToLength(tag);
}
uint8_t CordTestAccess::LengthToTag(size_t s) {
  Y_ABSL_INTERNAL_CHECK(s <= kMaxFlatLength, y_absl::StrCat("Invalid length ", s));
  return cord_internal::AllocatedSizeToTag(s + cord_internal::kFlatOverhead);
}
size_t CordTestAccess::SizeofCordRepExternal() {
  return sizeof(CordRepExternal);
}
size_t CordTestAccess::SizeofCordRepSubstring() {
  return sizeof(CordRepSubstring);
}
}  // namespace strings_internal
Y_ABSL_NAMESPACE_END
}  // namespace y_absl
