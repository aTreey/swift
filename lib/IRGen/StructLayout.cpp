//===--- StructLayout.cpp - Layout of structures -------------------------===//
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
//
//  This file implements algorithms for laying out structures.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ErrorHandling.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"

#include "FixedTypeInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "StructLayout.h"
#include "TypeInfo.h"

using namespace swift;
using namespace irgen;

/// Does this layout kind require a heap header?
static bool requiresHeapHeader(LayoutKind kind) {
  switch (kind) {
  case LayoutKind::NonHeapObject: return false;
  case LayoutKind::HeapObject: return true;
  }
  llvm_unreachable("bad layout kind!");
}

/// Return the size of the standard heap header.
Size irgen::getHeapHeaderSize(IRGenModule &IGM) {
  return IGM.getPointerSize() * 2;
}

/// Add the fields for the standard heap header to the given layout.
void irgen::addHeapHeaderToLayout(IRGenModule &IGM,
                                  Size &size, Alignment &align,
                                  SmallVectorImpl<llvm::Type*> &fields) {
  assert(size.isZero() && align.isOne() && fields.empty());
  size = getHeapHeaderSize(IGM);
  align = IGM.getPointerAlignment();
  fields.push_back(IGM.RefCountedStructTy);
}

/// Perform structure layout on the given types.
StructLayout::StructLayout(IRGenModule &IGM, LayoutKind layoutKind,
                           LayoutStrategy strategy,
                           ArrayRef<const TypeInfo *> types,
                           llvm::StructType *typeToFill) {
  Elements.reserve(types.size());

  // Fill in the Elements array.
  for (auto type : types)
    Elements.push_back(ElementLayout::getIncomplete(*type));

  assert(typeToFill == nullptr || typeToFill->isOpaque());

  StructLayoutBuilder builder(IGM);

  // Add the heap header if necessary.
  if (requiresHeapHeader(layoutKind)) {
    builder.addHeapHeader();
  }

  bool nonEmpty = builder.addFields(Elements, strategy);

  // Special-case: there's nothing to store.
  // In this case, produce an opaque type;  this tends to cause lovely
  // assertions.
  if (!nonEmpty) {
    assert(!builder.empty() == requiresHeapHeader(layoutKind));
    MinimumAlign = Alignment(1);
    MinimumSize = Size(0);
    Ty = (typeToFill ? typeToFill : IGM.OpaquePtrTy->getElementType());
  } else {
    MinimumAlign = builder.getAlignment();
    MinimumSize = builder.getSize();
    if (typeToFill) {
      builder.setAsBodyOfStruct(typeToFill);
      Ty = typeToFill;
    } else {
      Ty = builder.getAsAnonStruct();
    }
  }
  assert(typeToFill == nullptr || Ty == typeToFill);
}

llvm::Value *StructLayout::emitSize(IRGenFunction &IGF) const {
  assert(isFixedLayout());
  return IGF.IGM.getSize(getSize());
}

llvm::Value *StructLayout::emitAlign(IRGenFunction &IGF) const {
  assert(isFixedLayout());
  return IGF.IGM.getSize(getAlignment().asSize());
}

/// Bitcast an arbitrary pointer to be a pointer to this type.
Address StructLayout::emitCastTo(IRGenFunction &IGF,
                                 llvm::Value *ptr,
                                 const llvm::Twine &name) const {
  llvm::Value *addr =
    IGF.Builder.CreateBitCast(ptr, getType()->getPointerTo(), name);
  return Address(addr, getAlignment());
}

Address ElementLayout::project(IRGenFunction &IGF, Address baseAddr,
                               const llvm::Twine &suffix) const {
  switch (getKind()) {
  case Kind::Empty:
    return getType().getUndefAddress();

  case Kind::Fixed:
    return IGF.Builder.CreateStructGEP(baseAddr,
                                       getStructIndex(),
                                       getByteOffset(),
                                 baseAddr.getAddress()->getName() + suffix);
  }
  llvm_unreachable("bad element layout kind");
}

void StructLayoutBuilder::addHeapHeader() {
  assert(StructFields.empty() && "adding heap header at a non-zero offset");
  CurSize = getHeapHeaderSize(IGM);
  CurAlignment = IGM.getPointerAlignment();
  StructFields.push_back(IGM.RefCountedStructTy);
}

bool StructLayoutBuilder::addFields(llvm::MutableArrayRef<ElementLayout> elts,
                                    LayoutStrategy strategy) {
  // Track whether we've added any storage to our layout.
  bool addedStorage = false;

  // Loop through the elements.  The only valid field in each element
  // is Type; StructIndex and ByteOffset need to be laid out.
  for (auto &elt : elts) {
    auto &eltTI = elt.getType();

    auto isPOD = eltTI.isPOD(ResilienceScope::Local);

    // If the element type is empty, it adds nothing.
    if (eltTI.isKnownEmpty()) {
      elt.completeEmpty(isPOD);
      continue;
    }

    // Anything else we do at least potentially adds storage requirements.
    addedStorage = true;

    // FIXME: handle resilient/dependently-sized types
    auto &fixedEltTI = cast<FixedTypeInfo>(eltTI);

    // TODO: consider using different layout rules.
    // If the rules are changed so that fields aren't necessarily laid
    // out sequentially, the computation of InstanceStart in the
    // RO-data will need to be fixed.

    // The struct alignment is the max of the alignment of the fields.
    CurAlignment = std::max(CurAlignment, fixedEltTI.getFixedAlignment());

    // If the current tuple size isn't a multiple of the field's
    // required alignment, we need to pad out.
    Alignment eltAlignment = fixedEltTI.getFixedAlignment();
    if (Size offsetFromAlignment = CurSize % eltAlignment) {
      unsigned paddingRequired
        = eltAlignment.getValue() - offsetFromAlignment.getValue();
      assert(paddingRequired != 0);

      // We don't actually need to uglify the IR unless the natural
      // alignment of the IR type for the field isn't good enough.
      Alignment fieldIRAlignment(
          IGM.DataLayout.getABITypeAlignment(eltTI.StorageType));
      assert(fieldIRAlignment <= eltAlignment);
      if (fieldIRAlignment != eltAlignment) {
        auto paddingTy = llvm::ArrayType::get(IGM.Int8Ty, paddingRequired);
        StructFields.push_back(paddingTy);
      }

      // Regardless, the storage size goes up.
      CurSize += Size(paddingRequired);
    }

    // Set the element's offset and field-index.
    elt.completeFixed(isPOD, CurSize, StructFields.size());

    StructFields.push_back(eltTI.getStorageType());
    CurSize += fixedEltTI.getFixedSize();
  }

  return addedStorage;
}

/// Produce the current fields as an anonymous structure.
llvm::StructType *StructLayoutBuilder::getAsAnonStruct() const {
  return llvm::StructType::get(IGM.getLLVMContext(), StructFields);
}

/// Set the current fields as the body of the given struct type.
void StructLayoutBuilder::setAsBodyOfStruct(llvm::StructType *type) const {
  assert(type->isOpaque());
  type->setBody(StructFields);
}
