//===--- Conversion.h - Types for value conversion --------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Defines the Conversion class as well as ConvertingInitialization.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_LOWERING_CONVERSION_H
#define SWIFT_LOWERING_CONVERSION_H

#include "swift/Basic/ExternalUnion.h"
#include "Initialization.h"
#include "SGFContext.h"

namespace swift {
namespace Lowering {

/// An abstraction representing certain kinds of conversion that SILGen can
/// do automatically in various situations.
class Conversion {
public:
  enum KindTy {
    /// An implicit bridging conversion to a foreign type.
    BridgeToObjC,

    /// An implicit bridging conversion from a foreign type.
    BridgeFromObjC,

    /// An implicit bridging conversion for a function result.
    BridgeResultFromObjC,

    /// An erasure to Any (possibly wrapped in optional conversions).
    /// This is sortof a bridging conversion.
    AnyErasure,

    LastBridgingKind = AnyErasure,

    /// An orig-to-subst conversion.
    OrigToSubst,

    /// A subst-to-orig conversion.  These can always be annihilated.
    SubstToOrig,
  };

  static bool isBridgingKind(KindTy kind) {
    return kind <= LastBridgingKind;
  }

private:
  KindTy Kind;

  struct BridgingTypes {
    CanType OrigType;
    CanType ResultType;
    SILType LoweredResultType;
    bool IsExplicit;
  };

  struct ReabstractionTypes {
    AbstractionPattern OrigType;
    CanType SubstType;
  };

  static int getStorageIndexForKind(KindTy kind) {
    switch (kind) {
    case BridgeToObjC:
    case BridgeFromObjC:
    case BridgeResultFromObjC:
    case AnyErasure:
      return 0;

    case OrigToSubst:
    case SubstToOrig:
      return 1;
    }
    llvm_unreachable("bad kind");
  }

  ExternalUnion<KindTy, getStorageIndexForKind,
                BridgingTypes, ReabstractionTypes> Types;
  static_assert(decltype(Types)::union_is_trivially_copyable,
                "define the special members if this changes");

  Conversion(KindTy kind, CanType origType, CanType resultType,
             SILType loweredResultTy, bool isExplicit)
      : Kind(kind) {
    Types.emplaceAggregate<BridgingTypes>(kind, origType, resultType,
                                          loweredResultTy, isExplicit);
  }

  Conversion(KindTy kind, AbstractionPattern origType, CanType substType)
      : Kind(kind) {
    Types.emplaceAggregate<ReabstractionTypes>(kind, origType, substType);
  }

public:
  static Conversion getOrigToSubst(AbstractionPattern origType,
                                   CanType substType) {
    return Conversion(OrigToSubst, origType, substType);
  }

  static Conversion getSubstToOrig(AbstractionPattern origType,
                                   CanType substType) {
    return Conversion(SubstToOrig, origType, substType);
  }

  static Conversion getBridging(KindTy kind, CanType origType,
                                CanType resultType, SILType loweredResultTy,
                                bool isExplicit = false) {
    assert(isBridgingKind(kind));
    return Conversion(kind, origType, resultType, loweredResultTy, isExplicit);
  }

  KindTy getKind() const {
    return Kind;
  }

  bool isBridging() const {
    return isBridgingKind(getKind());
  }

  AbstractionPattern getReabstractionOrigType() const {
    return Types.get<ReabstractionTypes>(Kind).OrigType;
  }

  CanType getReabstractionSubstType() const {
    return Types.get<ReabstractionTypes>(Kind).SubstType;
  }

  bool isBridgingExplicit() const {
    return Types.get<BridgingTypes>(Kind).IsExplicit;
  }

  CanType getBridgingSourceType() const {
    return Types.get<BridgingTypes>(Kind).OrigType;
  }

  CanType getBridgingResultType() const {
    return Types.get<BridgingTypes>(Kind).ResultType;
  }

  SILType getBridgingLoweredResultType() const {
    return Types.get<BridgingTypes>(Kind).LoweredResultType;
  }

  ManagedValue emit(SILGenFunction &SGF, SILLocation loc,
                    ManagedValue source, SGFContext ctxt) const;

  Optional<Conversion> tryPeepholeOptionalInjection() const;

  void dump() const LLVM_ATTRIBUTE_USED;
  void print(llvm::raw_ostream &out) const;
};

bool canPeepholeConversions(SILGenFunction &SGF,
                            const Conversion &outerConversion,
                            const Conversion &innerConversion);

ManagedValue emitPeepholedConversions(SILGenFunction &SGF, SILLocation loc,
                                      const Conversion &outerConversion,
                                      const Conversion &innerConversion,
                                      SGFContext C,
                   llvm::function_ref<ManagedValue(SGFContext)> produceValue);

/// An initialization where we ultimately want to apply a conversion to
/// the value before completing the initialization.
///
/// Value generators may call getAsConversion() to check whether an
/// Initialization is one of these.  If so, they may call either
/// tryPeephole or setConvertedValue.
class ConvertingInitialization final : public Initialization {
private:
  enum StateTy {
    /// Nothing has happened.
    Uninitialized,

    /// The converted value has been set.
    Initialized,

    /// finishInitialization has been called.
    Finished,

    /// The converted value has been extracted.
    Extracted
  };

  StateTy State;

  /// The conversion that needs to be applied to the formal value.
  Conversion TheConversion;

  /// The converted value, set if the initializing code calls tryPeephole,
  /// setReabstractedValue, or copyOrInitValueInto.
  ManagedValue Value;
  SGFContext FinalContext;

  StateTy getState() const {
    return State;
  }

public:
  ConvertingInitialization(Conversion conversion, SGFContext finalContext)
    : State(Uninitialized), TheConversion(conversion),
      FinalContext(finalContext) {}

  /// Return the conversion to apply to the unconverted value.
  const Conversion &getConversion() const {
    return TheConversion;
  }

  /// Return the context into which to emit the converted value.
  SGFContext getFinalContext() const {
    return FinalContext;
  }

  // The three ways to perform this initialization:

  /// Set the unconverted value for this initialization.
  void copyOrInitValueInto(SILGenFunction &SGF, SILLocation loc,
                           ManagedValue value, bool isInit) override;

  /// Given that the result of the given expression needs to sequentially
  /// undergo the the given conversion and then this conversion, attempt to
  /// peephole the result.  If successful, the value will be set in this
  /// initialization.  The initialization will not yet be finished.
  bool tryPeephole(SILGenFunction &SGF, Expr *E, Conversion innerConversion);
  bool tryPeephole(SILGenFunction &SGF, SILLocation loc,
                   Conversion innerConversion,
                   llvm::function_ref<ManagedValue(SGFContext)> generate);
  bool tryPeephole(SILGenFunction &SGF, SILLocation loc, ManagedValue value,
                   Conversion innerConversion);

  /// Set the converted value for this initialization.
  void setConvertedValue(ManagedValue value) {
    assert(getState() == Uninitialized);
    assert(!value.isInContext() || FinalContext.getEmitInto());
    Value = value;
    State = Initialized;
  }

  /// Given the unconverted result, i.e. the result of emitting a
  /// value formally of the unconverted type with this initialization
  /// as the SGFContext, produce the converted result.
  ///
  /// If this initialization was initialized, the unconverted result
  /// must be ManagedValue::forInContext(), and vice-versa.
  ///
  /// The result of this function may itself be
  /// ManagedValue::forInContext() if this Initialization was created
  /// with an SGFContext which contains another Initialization.
  ManagedValue finishEmission(SILGenFunction &SGF, SILLocation loc,
                              ManagedValue formalResult);

  // Implement to make the cast work.
  ConvertingInitialization *getAsConversion() override {
    return this;
  }

  // Bookkeeping.
  void finishInitialization(SILGenFunction &SGF) override {
    assert(getState() == Initialized);
    State = Finished;
  }
};

} // end namespace Lowering
} // end namespace swift

#endif
