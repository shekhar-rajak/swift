//===--- MemoryPromotion.cpp - Promote memory to SSA registers ------------===//
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

#define DEBUG_TYPE "memory-promotion"
#include "swift/Subsystems.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/Diagnostics.h"
#include "swift/SIL/SILBuilder.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/StringExtras.h"
using namespace swift;

STATISTIC(NumLoadPromoted, "Number of loads promoted");
STATISTIC(NumAssignRewritten, "Number of assigns rewritten");

template<typename ...ArgTypes>
static void diagnose(SILModule *M, SILLocation loc, ArgTypes... args) {
  M->getASTContext().Diags.diagnose(loc.getSourceLoc(), Diagnostic(args...));
}

//===----------------------------------------------------------------------===//
// Element Counting Logic
//===----------------------------------------------------------------------===//

namespace {
  /// Recursively computing the number of elements in a type can be exponential,
  /// and even if the number of elements is low, the presence of single-element
  /// struct wrappers (like Int!) means that computing this is non-trivial.
  ///
  /// Cache the results of the NumElements computation in a DenseMap to make
  /// this be linear in the number of types analyzed.
  class NumElementsCache {
    llvm::DenseMap<CanType, unsigned> ElementCountMap;
    
    NumElementsCache(const NumElementsCache&) = delete;
    void operator=(const NumElementsCache&) = delete;
  public:
    NumElementsCache() {}
    unsigned get(CanType T);

    /// Push the symbolic path name to the specified element number onto to
    /// specified std::string.
    void getPathStringToElement(CanType T, unsigned Element,
                                std::string &Result);

    /// Given a pointer to an aggregate type, compute the addresses of each
    /// element and add them to the ElementAddrs vector.
    void getElementAddresses(SILValue Pointer,
                             SmallVectorImpl<SILInstruction*> &ElementAddrs);
    
    /// Given an RValue of aggregate type, compute the values of the elements by
    /// emitting a series of tuple_element or struct_element instructions.
    void getElements(SILValue V, SmallVectorImpl<SILValue> &ElementVals,
                     SILLocation Loc, SILBuilder &B);
  };
} // end anonymous namespace

/// getNumElements - Return the number of elements in the flattened SILType.
/// For tuples and structs, this is the (recursive) count of the fields it
/// contains.
unsigned NumElementsCache::get(CanType T) {
  if (TupleType *TT = T->getAs<TupleType>()) {
    auto It = ElementCountMap.find(T);
    if (It != ElementCountMap.end()) return It->second;
    
    unsigned NumElements = 0;
    for (auto &Elt : TT->getFields())
      NumElements += get(Elt.getType()->getCanonicalType());
    return ElementCountMap[T] = NumElements;
  }
  
  if (T->is<StructType>() || T->is<BoundGenericStructType>()) {
    auto It = ElementCountMap.find(T);
    if (It != ElementCountMap.end()) return It->second;
    
    StructDecl *SD;
    if (auto *ST = T->getAs<StructType>())
      SD = ST->getDecl();
    else
      SD = T->castTo<BoundGenericStructType>()->getDecl();
    
    unsigned NumElements = 0;
    for (auto *VD : SD->getPhysicalFields())
      NumElements += get(VD->getType()->getCanonicalType());
    return ElementCountMap[T] = NumElements;
  }
  
  // If this isn't a tuple or struct, it is a single element.
  return 1;
}

/// Push the symbolic path name to the specified element number onto to
/// specified std::string.
void NumElementsCache::getPathStringToElement(CanType T, unsigned Element,
                                              std::string &Result) {
  // If the specified type has a single element, we're done.  Don't dive in any
  // farther even if we could.
  if (get(T) == 1) return;
  
  if (TupleType *TT = T->getAs<TupleType>()) {
    unsigned FieldNo = 0;
    for (auto &Field : TT->getFields()) {
      unsigned ElementsForField = get(Field.getType()->getCanonicalType());
      
      if (Element < ElementsForField) {
        Result += '.';
        if (Field.hasName())
          Result += Field.getName().str();
        else
          Result += llvm::utostr(FieldNo);
        return getPathStringToElement(Field.getType()->getCanonicalType(),
                                      Element, Result);
      }
      
      Element -= ElementsForField;
      
      ++FieldNo;
    }
    assert(0 && "Element number is out of range for this type!");
  }
  
  assert(T->is<StructType>() || T->is<BoundGenericStructType>());
  StructDecl *SD;
  if (auto *ST = T->getAs<StructType>())
    SD = ST->getDecl();
  else
    SD = T->castTo<BoundGenericStructType>()->getDecl();
  
  for (auto *VD : SD->getPhysicalFields()) {
    unsigned ElementsForField = get(VD->getType()->getCanonicalType());

    if (Element < ElementsForField) {
      Result += '.';
      Result += VD->getName().str();
      return getPathStringToElement(VD->getType()->getCanonicalType(),
                                    Element, Result);
    }
    
    Element -= ElementsForField;
  }
  assert(0 && "Element number is out of range for this type!");
}

/// Given a pointer to an aggregate type, compute the addresses of each
/// element and add them to the ElementAddrs vector.
void NumElementsCache::getElementAddresses(SILValue Pointer,
                               SmallVectorImpl<SILInstruction*> &ElementAddrs) {
  CanType AggType = Pointer.getType().getSwiftRValueType();
  assert(get(AggType) != 1 && "Shouldn't decompose scalars");
  
  SILInstruction *PointerInst = cast<SILInstruction>(Pointer.getDef());
  SILBuilder B(++SILBasicBlock::iterator(PointerInst));

  if (TupleType *TT = AggType->getAs<TupleType>()) {
    for (auto &Field : TT->getFields()) {
      auto ResultTy = Field.getType()->getCanonicalType();
      ElementAddrs.push_back(B.createTupleElementAddr(PointerInst->getLoc(),
                                                      Pointer,
                                                      ElementAddrs.size(),
                                  SILType::getPrimitiveAddressType(ResultTy)));
    }
    return;
  }
  
  assert(AggType->is<StructType>() || AggType->is<BoundGenericStructType>());
  StructDecl *SD;
  if (auto *ST = AggType->getAs<StructType>())
    SD = ST->getDecl();
  else
    SD = AggType->castTo<BoundGenericStructType>()->getDecl();
  
  for (auto *VD : SD->getPhysicalFields()) {
    auto ResultTy = VD->getType()->getCanonicalType();
    ElementAddrs.push_back(B.createStructElementAddr(PointerInst->getLoc(),
                                                     Pointer, VD,
                                   SILType::getPrimitiveAddressType(ResultTy)));
  }
}

/// Given an RValue of aggregate type, compute the values of the elements by
/// emitting a series of tuple_element or struct_element instructions.
void NumElementsCache::getElements(SILValue V,
                                   SmallVectorImpl<SILValue> &ElementVals,
                                   SILLocation Loc, SILBuilder &B) {
  CanType AggType = V.getType().getSwiftRValueType();
  assert(get(AggType) != 1 && "Shouldn't decompose scalars");
  
  if (TupleType *TT = AggType->getAs<TupleType>()) {
    for (auto &Field : TT->getFields()) {
      auto ResultTy = Field.getType()->getCanonicalType();
      ElementVals.push_back(B.createTupleExtract(Loc, V, ElementVals.size(),
                                 SILType::getPrimitiveObjectType(ResultTy)));
    }
    return;
  }
  
  assert(AggType->is<StructType>() ||
         AggType->is<BoundGenericStructType>());
  StructDecl *SD;
  if (auto *ST = AggType->getAs<StructType>())
    SD = ST->getDecl();
  else
    SD = AggType->castTo<BoundGenericStructType>()->getDecl();
  
  for (auto *VD : SD->getPhysicalFields()) {
    auto ResultTy = VD->getType()->getCanonicalType();
    ElementVals.push_back(B.createStructExtract(Loc, V, VD,
                                  SILType::getPrimitiveObjectType(ResultTy)));
  }
}


//===----------------------------------------------------------------------===//
// Per-Element Promotion Logic
//===----------------------------------------------------------------------===//

namespace {
  enum UseKind {
    // The instruction is a LoadInst.
    Load,

    // The instruction is a StoreInst.
    Store,

    /// The instruction is an Apply, this is a byref or indirect return.
    ByrefUse,

    /// This instruction is a general escape of the value, e.g. a call to a
    /// closure that captures it.
    Escape
  };

  /// ElementUses - This class keeps track of all of the uses of a single
  /// element (i.e. tuple element or struct field) of a memory object.
  typedef std::vector<std::pair<SILInstruction*, UseKind>> ElementUses;

  enum class EscapeKind {
    Unknown,
    Yes,
    No
  };

  /// LiveOutBlockState - Keep track of information about blocks that have
  /// already been analyzed.  Since this is a global analysis, we need this to
  /// cache information about different paths through the CFG.
  struct LiveOutBlockState {
    /// For this block, keep track of whether there is a path from the entry
    /// of the function to the end of the block that crosses an escape site.
    EscapeKind EscapeInfo = EscapeKind::Unknown;

    /// Keep track of whether there is a Store, ByrefUse, or Escape locally in
    /// this block.
    bool HasNonLoadUse = false;

  };
} // end anonymous namespace

namespace {
  /// ElementPromotion - This is the main heavy lifting for processing the uses
  /// of an element of an allocation.
  class ElementPromotion {
    AllocBoxInst *TheAllocBox;
    unsigned ElementNumber;
    ElementUses &Uses;
    NumElementsCache &NumElements;
    llvm::SmallDenseMap<SILBasicBlock*, LiveOutBlockState, 32> PerBlockInfo;

    /// This is the set of uses that are not loads (i.e., they are Stores,
    /// ByrefUses, and Escapes).
    llvm::SmallPtrSet<SILInstruction*, 16> NonLoadUses;

    bool HasAnyEscape = false;

    // Keep track of whether we've emitted an error.  We only emit one error per
    // element as a policy decision.
    bool HadError = false;
  public:
    ElementPromotion(AllocBoxInst *TheAllocBox, unsigned ElementNumber,
                     ElementUses &Uses, NumElementsCache &NumElements);

    void doIt();
    
  private:
    void handleLoadUse(SILInstruction *Inst);
    void handleStoreUse(SILInstruction *Inst);
    void handleByrefUse(SILInstruction *Inst);
    void handleEscape(SILInstruction *Inst);

    enum DIKind {
      DI_Yes,
      DI_No,
      DI_Partial
    };
    DIKind checkDefinitelyInit(SILInstruction *Inst, SILValue *AV = nullptr);
    
    
    void diagnoseInitError(SILInstruction *Use,
                           Diag<> DiagNoName,
                           Diag<StringRef> DiagWithName);
  };
} // end anonymous namespace

ElementPromotion::ElementPromotion(AllocBoxInst *TheAllocBox,
                                   unsigned ElementNumber, ElementUses &Uses,
                                   NumElementsCache &NumElements)
  : TheAllocBox(TheAllocBox), ElementNumber(ElementNumber), Uses(Uses),
    NumElements(NumElements) {

  // The first step of processing an element is to collect information about the
  // element into data structures we use later.
  for (auto Use : Uses) {
    // Keep track of all the uses that aren't loads.
    if (Use.second != UseKind::Load)
      NonLoadUses.insert(Use.first);

    if (Use.second == UseKind::Escape) {
      // Determine which blocks the value can escape from.  We aren't allowed to
      // promote loads in blocks reachable from an escape point.
      HasAnyEscape = true;
      auto &BBInfo = PerBlockInfo[Use.first->getParent()];
      BBInfo.EscapeInfo = EscapeKind::Yes;
      BBInfo.HasNonLoadUse = true;
    } else if (Use.second == UseKind::Store ||
               Use.second == UseKind::ByrefUse) {
      // Keep track of which blocks have local stores.  This makes scanning for
      // assignments cheaper later.
      PerBlockInfo[Use.first->getParent()].HasNonLoadUse = true;
    }
  }
}

void ElementPromotion::diagnoseInitError(SILInstruction *Use,
                                         Diag<> DiagNoName,
                                         Diag<StringRef> DiagWithName) {
  HadError = true;

  // If the definition is a declaration, try to reconstruct a name and
  // optionally an access path to the uninitialized element.
  if (ValueDecl *VD =
        dyn_cast_or_null<ValueDecl>(TheAllocBox->getLoc().getAs<Decl>())) {
    std::string Name = VD->getName().str();
    
    // If the overall memory allocation is an aggregate of multiple elements
    // (i.e. a struct or tuple), then dive in to explain *which* element is
    // being used uninitialized.
    CanType AllocTy = TheAllocBox->getElementType().getSwiftRValueType();
    NumElements.getPathStringToElement(AllocTy, ElementNumber, Name);
    
    diagnose(Use->getModule(), Use->getLoc(), DiagWithName, Name);
    
  } else {
    // Otherwise, emit the diagnostic with no name or path information.
    diagnose(Use->getModule(), Use->getLoc(), DiagNoName);
  }

  // Provide context as note diagnostics.

  // TODO: The QoI could be improved in many different ways here.  For example,
  // We could give some path information where the use was uninitialized, like
  // the static analyzer.
  diagnose(Use->getModule(), TheAllocBox->getLoc(),
           diag::variable_defined_here);
}


void ElementPromotion::doIt() {
  // With any escapes tallied up, we can work through all the uses, checking
  // for definitive initialization, promoting loads, rewriting assigns, and
  // performing other tasks.
  for (auto Use : Uses) {
    switch (Use.second) {
    case UseKind::Load:     handleLoadUse(Use.first); break;
    case UseKind::Store:    handleStoreUse(Use.first); break;
    case UseKind::ByrefUse: handleByrefUse(Use.first); break;
    case UseKind::Escape:   handleEscape(Use.first); break;
    }

    if (HadError) break;
  }
}

/// Given a load (i.e., a LoadInst or CopyAddr), determine whether the loaded
/// value is definitely assigned or not.  If not, produce a diagnostic.  If so,
/// attempt to promote the value into SSA form.
void ElementPromotion::handleLoadUse(SILInstruction *Inst) {
  SILValue Result;

  // If this is a Load (not a CopyAddr or LoadWeak), we try to compute the
  // loaded value as an SSA register.  Otherwise, we don't ask for an available
  // value to avoid constructing SSA for the value.
  auto DI = checkDefinitelyInit(Inst, isa<LoadInst>(Inst) ? &Result : nullptr);
  if (DI == DI_Yes) {
    // If the value is definitely initialized, check to see if this is a load
    // that we have a value available for.  If so, we can replace the load now.
    //
    // Don't transform aggregate loads and stores.  We are operating
    // elementwise, so just because this element of the aggregate is
    // fullfilled by the load doesn't mean the other lanes are.
    if (Result && NumElements.get(Inst->getType(0).getSwiftType()) == 1 &&
        Inst->getType(0) == Result.getType()) {
      SILValue(Inst, 0).replaceAllUsesWith(Result);
      Inst->eraseFromParent();
      ++NumLoadPromoted;
    }
    return;
  }

  // Otherwise, this is a use of an uninitialized value.  Emit a diagnostic.
  diagnoseInitError(Inst, diag::variable_used_before_initialized,
                    diag::variable_n_used_before_initialized);
}

void ElementPromotion::handleStoreUse(SILInstruction *Inst) {
  // Generally, we don't need to do anything for stores, since this analysis is
  // use-driven.  However, we *do* need to decide if "assignments" are stores,
  // initializations, or ambiguous and then rewrite them.  As such, we look at
  // AssignInst and "assignment" CopyAddr's.  We ignore initialize copy_addrs,
  // relying on SILGen to only produce them when known correct.
  if (isa<AssignInst>(Inst))
    ;
  else if (auto CA = dyn_cast<CopyAddrInst>(Inst)) {
    if (CA->isInitializationOfDest()) return;
  } else if (auto SW = dyn_cast<StoreWeakInst>(Inst)) {
    if (SW->isInitializationOfDest()) return;
  } else {
    return;
  }

  SILType StoredType = Inst->getOperand(0).getType();

  bool HasTrivialType = false;
  if (isa<AssignInst>(Inst))
    HasTrivialType = Inst->getModule()->
      Types.getTypeLowering(StoredType).isTrivial();

  // Check to see if the value is known-initialized here or not.  If the assign
  // has non-trivial type, then we're interested in using any live-in value that
  // is available.
  SILValue IncomingVal;
  auto DI = checkDefinitelyInit(Inst, HasTrivialType ? nullptr : &IncomingVal);

  // If it is initialized on some paths, but not others, then we have an
  // inconsistent initialization error.
  if (DI == DI_Partial) {
    diagnoseInitError(Inst, diag::variable_initialized_on_some_paths,
                      diag::variable_n_initialized_on_some_paths);
    return;
  }

  // If this is a copy_addr or store_weak, we just set the initialization bit
  // depending on what we find.
  if (auto *CA = dyn_cast<CopyAddrInst>(Inst)) {
    CA->setIsInitializationOfDest(IsInitialization_t(DI == DI_No));
    return;
  }
  if (auto *SW = dyn_cast<StoreWeakInst>(Inst)) {
    SW->setIsInitializationOfDest(IsInitialization_t(DI == DI_No));
    return;
  }

  assert(isa<AssignInst>(Inst));

  ++NumAssignRewritten;
  SILBuilder B(Inst);

  // "unowned" assignments are expanded to unowned operations.
  bool isOwned = true;
  if (auto *RST = StoredType.getSwiftRValueType()->getAs<ReferenceStorageType>())
    isOwned = RST->getOwnership() != Ownership::Unowned;


  // Otherwise, if it has trivial type, we can always just replace the
  // assignment with a store.  If it has non-trivial type and is an
  // initialization, we can also replace it with a store.
  if (HasTrivialType || DI == DI_No) {
    auto NewStore =
      B.createStore(Inst->getLoc(), Inst->getOperand(0), Inst->getOperand(1));

    // Non-trivial values must be retained, since the box owns them.
    if (HasTrivialType)
      ;
    else if (isOwned)
      B.createRetainInst(Inst->getLoc(), Inst->getOperand(0));
    else
      B.createUnownedRetain(Inst->getLoc(), Inst->getOperand(0));

    NonLoadUses.insert(NewStore);
    NonLoadUses.erase(Inst);
    Inst->eraseFromParent();
    return;
  }

  // Otherwise, we need to replace the assignment with the full
  // load/store/retain/release dance.  If we have a live-in value available, we
  // can use that instead of doing a reload.
  if (!IncomingVal)
    IncomingVal = B.createLoad(Inst->getLoc(), Inst->getOperand(1));

  if (isOwned)
    B.createRetainInst(Inst->getLoc(), Inst->getOperand(0));
  else
    B.createUnownedRetain(Inst->getLoc(), Inst->getOperand(0));

  auto NewStore =
    B.createStore(Inst->getLoc(), Inst->getOperand(0), Inst->getOperand(1));

  if (isOwned)
    B.createReleaseInst(Inst->getLoc(), IncomingVal);
  else
    B.createUnownedRelease(Inst->getLoc(), IncomingVal);

  NonLoadUses.insert(NewStore);
  NonLoadUses.erase(Inst);
  Inst->eraseFromParent();
}

/// Given a byref use (an Apply), determine whether the loaded
/// value is definitely assigned or not.  If not, produce a diagnostic.
void ElementPromotion::handleByrefUse(SILInstruction *Inst) {
  auto DI = checkDefinitelyInit(Inst);
  if (DI == DI_Yes)
    return;

  // Otherwise, this is a use of an uninitialized value.  Emit a diagnostic.
  diagnoseInitError(Inst, diag::variable_byref_before_initialized,
                    diag::variable_n_byref_before_initialized);
}

void ElementPromotion::handleEscape(SILInstruction *Inst) {
  auto DI = checkDefinitelyInit(Inst);
  if (DI == DI_Yes)
    return;

  // Otherwise, this is a use of an uninitialized value.  Emit a diagnostic.
  diagnoseInitError(Inst, diag::variable_escape_before_initialized,
                    diag::variable_n_escape_before_initialized);
}




/// getStoredValueFrom - Given a may store to the stack slot we're promoting,
/// return the value being stored.
static SILValue getStoredValueFrom(SILInstruction *I) {
  if (auto *SI = dyn_cast<StoreInst>(I))
    return SI->getOperand(0);
  if (auto *AI = dyn_cast<AssignInst>(I))
    return AI->getOperand(0);
  // TODO: Should we support store forwarding of weak pointers?
  return SILValue();
}

/// The specified instruction is a use of the element.  Determine whether the
/// element is definitely initialized at this point or not.  If the value is
/// initialized on some paths, but not others, this returns a partial result.
///
/// In addition to computing whether a value is definitely initialized or not,
/// if AV is non-null, this function can return the currently live value in some
/// cases.
ElementPromotion::DIKind
ElementPromotion::checkDefinitelyInit(SILInstruction *Inst, SILValue *AV) {
  // If there is a store in the current block, scan the block to see if the
  // store is before or after the load.  If it is before, it produces the value
  // we are looking for.
  if (PerBlockInfo[Inst->getParent()].HasNonLoadUse) {
    for (SILBasicBlock::iterator BBI = Inst, E = Inst->getParent()->begin();
         BBI != E;) {
      SILInstruction *TheInst = --BBI;
      if (NonLoadUses.count(TheInst)) {
        if (AV) *AV = getStoredValueFrom(TheInst);

        return DI_Yes;
      }

      // If we found the allocation itself, then we are loading something that
      // is not defined at all yet.
      if (TheInst == TheAllocBox)
        return DI_No;
    }
  }

  // FIXME: Need to do cross-block analysis.

  return DI_Partial;
}


//===----------------------------------------------------------------------===//
//                          Top Level Driver
//===----------------------------------------------------------------------===//

namespace {
  class ElementUseCollector {
    SmallVectorImpl<ElementUses> &Uses;
    NumElementsCache &NumElements;
  public:
    ElementUseCollector(SmallVectorImpl<ElementUses> &Uses,
                        NumElementsCache &NumElements)
      : Uses(Uses), NumElements(NumElements) {
    }

    /// This is the main entry point for the use walker.
    void collectUses(SILValue Pointer, unsigned BaseElt);
    
  private:
    void addElementUses(unsigned BaseElt, SILType UseTy,
                        SILInstruction *User, UseKind Kind);
    void collectElementAddressUses(SILInstruction *ElementPtr,unsigned BaseElt);
  };
  
  
} // end anonymous namespace

/// addElementUses - An operation (e.g. load, store, byref use, etc) on a value
/// acts on all of the aggregate elements in that value.  For example, a load
/// of $*(Int,Int) is a use of both Int elements of the tuple.  This is a helper
/// to keep the Uses data structure up to date for aggregate uses.
void ElementUseCollector::addElementUses(unsigned BaseElt, SILType UseTy,
                                         SILInstruction *User, UseKind Kind) {
  for (unsigned i = 0, e = NumElements.get(UseTy.getSwiftRValueType());
       i != e; ++i)
    Uses[BaseElt+i].push_back({ User, Kind });
}

/// Given a tuple_element_addr or struct_element_addr, compute the new BaseElt
/// implicit in the selected member, and recursively add uses of the
/// instruction.
void ElementUseCollector::collectElementAddressUses(SILInstruction *ElementPtr,
                                                    unsigned BaseElt) {
  auto RValueType = ElementPtr->getOperand(0).getType().getSwiftRValueType();
  
  // tuple_element_addr P, 42 indexes into the current element.  Recursively
  // process its uses with the adjusted element number.
  if (auto *TEAI = dyn_cast<TupleElementAddrInst>(ElementPtr)) {
    unsigned FieldNo = TEAI->getFieldNo();
    auto *TT = RValueType->castTo<TupleType>();
    unsigned NewBaseElt = BaseElt;
    for (unsigned i = 0; i != FieldNo; ++i) {
      CanType EltTy = TT->getElementType(i)->getCanonicalType();
      NewBaseElt += NumElements.get(EltTy);
    }
    
    collectUses(SILValue(TEAI, 0), NewBaseElt);
    return;
  }
  
  // struct_element_addr P, #field indexes into the current element.
  // Recursively process its uses with the adjusted element number.
  auto *SEAI = cast<StructElementAddrInst>(ElementPtr);
  VarDecl *Field = SEAI->getField();
  
  StructDecl *SD;
  if (auto *ST = RValueType->getAs<StructType>())
    SD = ST->getDecl();
  else
    SD = RValueType->castTo<BoundGenericStructType>()->getDecl();
    
  unsigned NewBaseElt = BaseElt;
  for (auto *VD : SD->getPhysicalFields()) {
    if (VD == Field) break;
    NewBaseElt += NumElements.get(VD->getType()->getCanonicalType());
  }
  
  collectUses(SILValue(SEAI, 0), NewBaseElt);
}


void ElementUseCollector::collectUses(SILValue Pointer, unsigned BaseElt) {
  assert(Pointer.getType().isAddress() &&
         "Walked through the pointer to the value?");
  SILType PointeeType = Pointer.getType().getObjectType();

  /// This keeps track of instructions in the use list that touch multiple
  /// elements and should be scalarized.  This is done as a second phase to
  /// avoid invalidating the use iterator.
  ///
  SmallVector<SILInstruction*, 4> UsesToScalarize;
  
  for (auto UI : Pointer.getUses()) {
    auto *User = cast<SILInstruction>(UI->getUser());

    // These show up as uses but aren't significant for this analysis.
    if (isa<DeallocStackInst>(User) ||
        isa<RetainInst>(User) ||
        isa<ReleaseInst>(User) ||
        isa<DeallocRefInst>(User))
      continue;
    
    // Instructions that compute a subelement are handled by a helper.
    if (isa<TupleElementAddrInst>(User) ||
        isa<StructElementAddrInst>(User)) {
      collectElementAddressUses(User, BaseElt);
      continue;
    }
    
    // Loads are a use of the value.
    if (isa<LoadInst>(User) || isa<LoadWeakInst>(User)) {
      if (NumElements.get(PointeeType.getSwiftRValueType()) == 1)
        Uses[BaseElt].push_back({User, UseKind::Load});
      else {
        assert(!isa<LoadWeakInst>(User) &&
               "Aggregate load_weak shouldn't happen");
        UsesToScalarize.push_back(User);
      }
      continue;
    }

    // Stores *to* the allocation are writes.  Stores *of* it is an escape.
    if ((isa<StoreInst>(User) || isa<AssignInst>(User) ||
         isa<StoreWeakInst>(User)) &&
        UI->getOperandNumber() == 1) {
      if (NumElements.get(PointeeType.getSwiftRValueType()) == 1)
        Uses[BaseElt].push_back({ User, UseKind::Store });
      else {
        assert(!isa<StoreWeakInst>(User) &&
               "Aggregate store_weak shouldn't happen");
        UsesToScalarize.push_back(User);
      }
      continue;
    }

    if (isa<CopyAddrInst>(User)) {
      // If this is the source of the copy_addr, then this is a load.  If it is
      // the destination, then this is a store.
      auto Kind = UI->getOperandNumber() == 0 ? UseKind::Load : UseKind::Store;
      addElementUses(BaseElt, PointeeType, User, Kind);
      continue;
    }
    
    // Initializations are definitions of the whole thing.  This is currently
    // used in constructors and should go away someday.
    if (isa<InitializeVarInst>(User)) {
      addElementUses(BaseElt, PointeeType, User, UseKind::Store);
      continue;
    }

    // The apply instruction does not capture the pointer when it is passed
    // through [byref] arguments or for indirect returns.  Byref arguments are
    // treated as uses and may-store's, but an indirect return is treated as a
    // full store.
    //
    // Note that partial_apply instructions always close over their argument.
    //
    if (auto *Apply = dyn_cast<ApplyInst>(User)) {
      SILType FnTy = Apply->getOperand(0).getType();
      SILFunctionTypeInfo *FTI = FnTy.getFunctionTypeInfo(*Apply->getModule());
      unsigned ArgumentNumber = UI->getOperandNumber()-1;

      // If this is an indirect return slot, it is a store.
      if (ArgumentNumber == 0 && FTI->hasIndirectReturn()) {
        addElementUses(BaseElt, PointeeType, User, UseKind::Store);
        continue;
      }

      // Otherwise, check for [byref].
      Type ArgTy = FTI->getSwiftArgumentType(ArgumentNumber);
      if (ArgTy->is<LValueType>()) {
        addElementUses(BaseElt, PointeeType, User, UseKind::ByrefUse);
        continue;
      }

      // Otherwise, it is an escape.
    }

     // Otherwise, the use is something complicated, it escapes.
    addElementUses(BaseElt, PointeeType, User, UseKind::Escape);
  }

  // Now that we've walked all of the immediate uses, scalarize any elements
  // that we need to for canonicalization or analysis reasons.
  if (!UsesToScalarize.empty()) {
    SmallVector<SILInstruction*, 4> ElementAddrs;
    NumElements.getElementAddresses(Pointer, ElementAddrs);
    
    SmallVector<SILValue, 4> ElementTmps;
    for (auto *User : UsesToScalarize) {
      SILBuilder B(User);
      ElementTmps.clear();

      // Scalarize LoadInst
      if (auto *LI = dyn_cast<LoadInst>(User)) {
        for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i)
          ElementTmps.push_back(B.createLoad(LI->getLoc(), ElementAddrs[i]));
        
        SILInstruction *Result;
        if (LI->getType().is<TupleType>())
          Result = B.createTuple(LI->getLoc(), LI->getType(), ElementTmps);
        else
          Result = B.createStruct(LI->getLoc(), LI->getType(), ElementTmps);

        
        SILValue(LI,0).replaceAllUsesWith(Result);
        LI->eraseFromParent();
        continue;
      }
      
      // Scalarize AssignInst
      if (auto *AI = dyn_cast<AssignInst>(User)) {
        NumElements.getElements(AI->getOperand(0), ElementTmps, AI->getLoc(),B);

        for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i)
          B.createAssign(AI->getLoc(), ElementTmps[i], ElementAddrs[i]);
        AI->eraseFromParent();
        continue;
      }
      
      // Scalarize StoreInst
      auto *SI = cast<StoreInst>(User);
      NumElements.getElements(SI->getOperand(0), ElementTmps, SI->getLoc(), B);
      
      for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i)
        B.createStore(SI->getLoc(), ElementTmps[i], ElementAddrs[i]);
      SI->eraseFromParent();
    }
    
    // Now that we've scalarized some stuff, recurse down into the newly created
    // element address computations to recursively process it.  This can cause
    // further scalarization.
    for (auto EltPtr : ElementAddrs)
      collectElementAddressUses(EltPtr, BaseElt);
  }
}


static void optimizeAllocBox(AllocBoxInst *ABI, NumElementsCache &NumElements) {
  // Set up the datastructure used to collect the uses of the alloc_box.  The
  // uses are bucketed up into the elements of the allocation that are being
  // used.  This matters for element-wise tuples and fragile structs.
  SmallVector<ElementUses, 1> Uses;
  Uses.resize(NumElements.get(ABI->getElementType().getSwiftRValueType()));

  // Walk the use list of the pointer, collecting them into the Uses array.
  ElementUseCollector(Uses, NumElements).collectUses(SILValue(ABI, 1), 0);

  // Process each scalar value in the uses array individually.
  unsigned EltNo = 0;
  for (auto &Elt : Uses)
    ElementPromotion(ABI, EltNo++, Elt, NumElements).doIt();
}


/// performSILMemoryPromotion - Promote alloc_box uses into SSA registers and
/// perform definitive initialization analysis.
void swift::performSILMemoryPromotion(SILModule *M) {
  NumElementsCache NumElements;
  for (auto &Fn : *M) {
    for (auto &BB : Fn) {
      auto I = BB.begin(), E = BB.end();
      while (I != E) {
        auto *ABI = dyn_cast<AllocBoxInst>(I);
        if (ABI == nullptr) {
          ++I;
          continue;
        }

        optimizeAllocBox(ABI, NumElements);

        // Carefully move iterator to avoid invalidation problems.
        ++I;
        if (ABI->use_empty())
          ABI->eraseFromParent();
      }
    }
  }
}


