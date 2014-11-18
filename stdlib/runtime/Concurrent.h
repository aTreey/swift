//===--- Concurrent.h - Concurrent Data Structures  ------------*- C++ -*--===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_RUNTIME_CONCURRENTUTILS_H
#define SWIFT_RUNTIME_CONCURRENTUTILS_H
#include <iterator>
#include <atomic>
#include <stdint.h>

/// This is a node in an concurrent linked list.
template <class ElemTy> struct ConcurrentListNode {
  ConcurrentListNode(ElemTy Elem) : Payload(Elem), Next(nullptr) {}
  ConcurrentListNode(const ConcurrentListNode &) = delete;
  ConcurrentListNode &operator=(const ConcurrentListNode &) = delete;

  /// The element.
  ElemTy Payload;
  /// Points to the next link in the chain.
  ConcurrentListNode<ElemTy> *Next;
};

/// This is a concurrent linked list. It supports insertion at the beginning
/// of the list and traversal using iterators.
/// This is a very simple implementation of a concurrent linked list
/// using atomic operations. The 'push_front' method allocates a new link
/// and attempts to compare and swap the old head pointer with pointer to
/// the new link. This operation may fail many times if there are other
/// contending threads, but eventually the head pointer is set to the new
/// link that already points to the old head value. Notice that the more
/// difficult feature of removing links is not supported.
/// See 'push_front' for more details.
template <class ElemTy> struct ConcurrentList {
  ConcurrentList() : First(nullptr) {}
  ~ConcurrentList() {
    // Iterate over the list and delete all the nodes.
    auto Ptr = First.load(std::memory_order_acquire);
    while (Ptr) {
      auto N = Ptr->Next;
      delete Ptr;
      Ptr = N;
    }
  }

  ConcurrentList(const ConcurrentList &) = delete;
  ConcurrentList &operator=(const ConcurrentList &) = delete;

  /// A list iterator.
  struct ConcurrentListIterator {
    /// Points to the current link.
    ConcurrentListNode<ElemTy> *Ptr;
    /// C'tor.
    ConcurrentListIterator(ConcurrentListNode<ElemTy> *P) : Ptr(P) {}
    /// Move to the next element.
    ConcurrentListIterator &operator++() {
      Ptr = Ptr->Next;
      return *this;
    }
    /// Access the element.
    ElemTy &operator*() { return Ptr->Payload; }
    /// Same?
    bool operator==(const ConcurrentListIterator &o) const {
      return o.Ptr == Ptr;
    }
    /// Not the same?
    bool operator!=(const ConcurrentListIterator &o) const {
      return o.Ptr != Ptr;
    }
  };

  /// Iterator entry point.
  typedef ConcurrentListIterator iterator;
  /// Marks the beginning of the list.
  iterator begin() const {
    return ConcurrentListIterator(First.load(std::memory_order_acquire));
  }
  /// Marks the end of the list.
  iterator end() const { return ConcurrentListIterator(nullptr); }

  /// Add a new item to the list.
  void push_front(ElemTy Elem) {
    /// Allocate a new node.
    ConcurrentListNode<ElemTy> *N = new ConcurrentListNode<ElemTy>(Elem);
    // Point to the first element in the list.
    N->Next = First.load(std::memory_order_acquire);
    auto OldFirst = N->Next;
    // Try to replace the current First with the new node.
    while (!std::atomic_compare_exchange_weak_explicit(&First, &OldFirst, N,
                                               std::memory_order_release,
                                               std::memory_order_relaxed)) {
      // If we fail, update the new node to point to the new head and try to
      // insert before the new
      // first element.
      N->Next = OldFirst;
    }
  }

  /// Points to the first link in the list.
  std::atomic<ConcurrentListNode<ElemTy> *> First;
};

template <class KeyTy, class ValueTy> struct ConcurrentMapNode {
  ConcurrentMapNode(KeyTy H)
      : Payload(), Left(nullptr), Right(nullptr), Key(H) {}

  ~ConcurrentMapNode() {
    delete Left.load(std::memory_order_acquire);
    delete Right.load(std::memory_order_acquire);
  }

  ConcurrentMapNode(const ConcurrentMapNode &) = delete;
  ConcurrentMapNode &operator=(const ConcurrentMapNode &) = delete;

  ValueTy Payload;
  typedef std::atomic<ConcurrentMapNode *> EdgeTy;
  EdgeTy Left;
  EdgeTy Right;
  KeyTy Key;
};

/// A concurrent map that is implemented using a binary tree. It supports
/// concurrent insertions but does not support removals or rebalancing of
/// the tree. Much like the concurrent linked list this data structure
/// does not support the removal of nodes, which is more difficult.
/// The method findOrAllocateNode searches the binary tree in search of the
/// exact Key value. If it finds an edge that points to NULL that should
/// contain the value then it tries to compare and swap the new node into
/// place. If it looses the race to a different thread it de-allocates
/// the node and starts the search again since the new node should
/// be placed (or found) on the new link. See findOrAllocateNode for more
/// details.
template <class KeyTy, class ValueTy> class ConcurrentMap {
public:
  ConcurrentMap() : Sentinel(0) {}

  ConcurrentMap(const ConcurrentMap &) = delete;
  ConcurrentMap &operator=(const ConcurrentMap &) = delete;

  /// A Sentinel root node that contains no elements.
  typedef ConcurrentMapNode<KeyTy, ValueTy> NodeTy;
  NodeTy Sentinel;

  /// Search a for a node with key value \p. If the node does not exist then
  /// allocate a new node and add it to the tree.
  ValueTy &findOrAllocateNode(KeyTy Key) {
    return findOrAllocateNode_rec(&Sentinel, Key);
  }

private:
  static ValueTy &findOrAllocateNode_rec(NodeTy *P, KeyTy Key) {
    // Found the node we were looking for.
    if (P->Key == Key)
      return P->Payload;

    // Point to the edge we want to replace.
    typename ConcurrentMapNode<KeyTy, ValueTy>::EdgeTy *Edge = nullptr;

    // Select the edge to follow.
    if (P->Key > Key) {
      Edge = &P->Left;
    } else {
      Edge = &P->Right;
    }

    // Load the current edge value.
    ConcurrentMapNode<KeyTy, ValueTy> *CurrentVal =
      Edge->load(std::memory_order_acquire);

    // If the edge is populated the follow the edge.
    if (CurrentVal)
      return findOrAllocateNode_rec(CurrentVal, Key);

    // Allocate a new node.
    ConcurrentMapNode<KeyTy, ValueTy> *New =
        new ConcurrentMapNode<KeyTy, ValueTy>(Key);

    // Try to set a new node:
    if (std::atomic_compare_exchange_weak_explicit(Edge, &CurrentVal, New,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed)){
      // On success return the new node.
      return New->Payload;
    }

    // If failed, deallocate the node and look for a new place in the
    // tree. Some other thread may have created a new entry and we may
    // discover it, so start searching with the current node.
    delete New;
    return findOrAllocateNode_rec(P, Key);
  }
};

#endif // SWIFT_RUNTIME_CONCURRENTUTILS_H
