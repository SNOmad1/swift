//===--- RequirementMachine.cpp - Generics with term rewriting ------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Module.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/Requirement.h"
#include <vector>

#include "RequirementMachine.h"

using namespace swift;
using namespace rewriting;

namespace {

/// A utility class for bulding rewrite rules from the top-level requirements
/// of a generic signature.
///
/// This also collects requirements from the transitive closure of all protocols
/// appearing on the right hand side of conformance requirements.
struct RuleBuilder {
  RewriteContext &Context;
  bool Dump;

  /// The keys are the unique protocols we've added so far. The value indicates
  /// whether the protocol's SCC is an initial component for the rewrite system.
  ///
  /// A rewrite system built from a generic signature does not have any initial
  /// protocols.
  ///
  /// A rewrite system built from a protocol SCC has the protocols of the SCC
  /// itself as initial protocols.
  ///
  /// If a protocol is an initial protocol, we use its structural requirements
  /// instead of its requirement signature as the basis of its rewrite rules.
  ///
  /// This is what breaks the cycle in requirement signature computation for a
  /// group of interdependent protocols.
  llvm::DenseMap<const ProtocolDecl *, bool> ProtocolMap;
  std::vector<const ProtocolDecl *> Protocols;

  /// New rules to add which will be marked 'permanent'. These are rules for
  /// introducing associated types, and relationships between layout,
  /// superclass and concrete type symbols. They are not eliminated by
  /// homotopy reduction, since they are always added when the rewrite system
  /// is built.
  std::vector<std::pair<MutableTerm, MutableTerm>> PermanentRules;

  /// New rules derived from requirements written by the user, which can be
  /// eliminated by homotopy reduction.
  std::vector<std::pair<MutableTerm, MutableTerm>> RequirementRules;

  CanType getConcreteSubstitutionSchema(CanType concreteType,
                                        const ProtocolDecl *proto,
                                        SmallVectorImpl<Term> &result);

  RuleBuilder(RewriteContext &ctx, bool dump) : Context(ctx), Dump(dump) {}
  void addRequirements(ArrayRef<Requirement> requirements);
  void addProtocols(ArrayRef<const ProtocolDecl *> proto);
  void addProtocol(const ProtocolDecl *proto,
                   bool initialComponent);
  void addAssociatedType(const AssociatedTypeDecl *type,
                         const ProtocolDecl *proto);
  void addRequirement(const Requirement &req,
                      const ProtocolDecl *proto);
  void collectRulesFromReferencedProtocols();
};

} // end namespace

/// Given a concrete type that may contain type parameters in structural positions,
/// collect all the structural type parameter components, and replace them all with
/// fresh generic parameters. The fresh generic parameters all have a depth of 0,
/// and the index is an index into the 'result' array.
///
/// For example, given the concrete type Foo<X.Y, Array<Z>>, this produces the
/// result type Foo<τ_0_0, Array<τ_0_1>>, with result array {X.Y, Z}.
CanType
RuleBuilder::getConcreteSubstitutionSchema(CanType concreteType,
                                           const ProtocolDecl *proto,
                                           SmallVectorImpl<Term> &result) {
  assert(!concreteType->isTypeParameter() && "Must have a concrete type here");

  if (!concreteType->hasTypeParameter())
    return concreteType;

  return CanType(concreteType.transformRec([&](Type t) -> Optional<Type> {
    if (!t->isTypeParameter())
      return None;

    unsigned index = result.size();
    result.push_back(Context.getTermForType(CanType(t), proto));

    return CanGenericTypeParamType::get(/*type sequence=*/ false,
                                        /*depth=*/0, index,
                                        Context.getASTContext());
  }));
}

void RuleBuilder::addRequirements(ArrayRef<Requirement> requirements) {
  // Collect all protocols transitively referenced from these requirements.
  for (auto req : requirements) {
    if (req.getKind() == RequirementKind::Conformance) {
      addProtocol(req.getProtocolDecl(), /*initialComponent=*/false);
    }
  }

  collectRulesFromReferencedProtocols();

  // Add rewrite rules for all top-level requirements.
  for (const auto &req : requirements)
    addRequirement(req, /*proto=*/nullptr);
}

void RuleBuilder::addProtocols(ArrayRef<const ProtocolDecl *> protos) {
  // Collect all protocols transitively referenced from this connected component
  // of the protocol dependency graph.
  for (auto proto : protos) {
    addProtocol(proto, /*initialComponent=*/true);
  }

  collectRulesFromReferencedProtocols();
}

/// For an associated type T in a protocol P, we add a rewrite rule:
///
///   [P].T => [P:T]
///
/// Intuitively, this means "if a type conforms to P, it has a nested type
/// named T".
void RuleBuilder::addAssociatedType(const AssociatedTypeDecl *type,
                                    const ProtocolDecl *proto) {
  MutableTerm lhs;
  lhs.add(Symbol::forProtocol(proto, Context));
  lhs.add(Symbol::forName(type->getName(), Context));

  MutableTerm rhs;
  rhs.add(Symbol::forAssociatedType(proto, type->getName(), Context));

  PermanentRules.emplace_back(lhs, rhs);
}

/// Lowers a generic requirement to a rewrite rule.
///
/// If \p proto is null, this is a generic requirement from the top-level
/// generic signature. The added rewrite rule will be rooted in a generic
/// parameter symbol.
///
/// If \p proto is non-null, this is a generic requirement in the protocol's
/// requirement signature. The added rewrite rule will be rooted in a
/// protocol symbol.
void RuleBuilder::addRequirement(const Requirement &req,
                                 const ProtocolDecl *proto) {
  if (Dump) {
    llvm::dbgs() << "+ ";
    req.dump(llvm::dbgs());
    llvm::dbgs() << "\n";
  }

  // Compute the left hand side.
  auto subjectType = CanType(req.getFirstType());
  auto subjectTerm = Context.getMutableTermForType(subjectType, proto);

  // Compute the right hand side.
  MutableTerm constraintTerm;

  switch (req.getKind()) {
  case RequirementKind::Conformance: {
    // A conformance requirement T : P becomes a rewrite rule
    //
    //   T.[P] == T
    //
    // Intuitively, this means "any type ending with T conforms to P".
    auto *proto = req.getProtocolDecl();

    constraintTerm = subjectTerm;
    constraintTerm.add(Symbol::forProtocol(proto, Context));
    break;
  }

  case RequirementKind::Superclass: {
    // A superclass requirement T : C<X, Y> becomes a rewrite rule
    //
    //   T.[superclass: C<X, Y>] => T
    //
    // Together with a rewrite rule
    //
    //   [superclass: C<X, Y>].[layout: L] => [superclass: C<X, Y>]
    //
    // Where 'L' is either AnyObject or _NativeObject, depending on the
    // ancestry of C.
    //
    // The second rule is marked permanent. Completion will derive a new
    // rule as a consequence of these two rules:
    //
    //   T.[layout: L] => T
    //
    // The new rule will be marked redundant by homotopy reduction since
    // it is a consequence of the other two rules.
    auto otherType = CanType(req.getSecondType());

    // Build the symbol [superclass: C<X, Y>].
    SmallVector<Term, 1> substitutions;
    otherType = getConcreteSubstitutionSchema(otherType, proto,
                                              substitutions);
    auto superclassSymbol = Symbol::forSuperclass(otherType, substitutions,
                                                  Context);

    {
      // Build the symbol [layout: L].
      auto layout =
        LayoutConstraint::getLayoutConstraint(
          otherType->getClassOrBoundGenericClass()->usesObjCObjectModel()
            ? LayoutConstraintKind::Class
            : LayoutConstraintKind::NativeClass,
          Context.getASTContext());
      auto layoutSymbol = Symbol::forLayout(layout, Context);

      MutableTerm layoutSubjectTerm;
      layoutSubjectTerm.add(superclassSymbol);

      MutableTerm layoutConstraintTerm = layoutSubjectTerm;
      layoutConstraintTerm.add(layoutSymbol);

      // Add the rule [superclass: C<X, Y>].[layout: L] => [superclass: C<X, Y>].
      PermanentRules.emplace_back(layoutConstraintTerm,
                                  layoutSubjectTerm);
    }

    // Build the term T.[superclass: C<X, Y>].
    constraintTerm = subjectTerm;
    constraintTerm.add(superclassSymbol);
    break;
  }

  case RequirementKind::Layout: {
    // A layout requirement T : L becomes a rewrite rule
    //
    //   T.[layout: L] == T
    constraintTerm = subjectTerm;
    constraintTerm.add(Symbol::forLayout(req.getLayoutConstraint(),
                                         Context));
    break;
  }

  case RequirementKind::SameType: {
    auto otherType = CanType(req.getSecondType());

    if (!otherType->isTypeParameter()) {
      // A concrete same-type requirement T == C<X, Y> becomes a
      // rewrite rule
      //
      //   T.[concrete: C<X, Y>] => T
      SmallVector<Term, 1> substitutions;
      otherType = getConcreteSubstitutionSchema(otherType, proto,
                                                substitutions);

      constraintTerm = subjectTerm;
      constraintTerm.add(Symbol::forConcreteType(otherType, substitutions,
                                                 Context));
      break;
    }

    constraintTerm = Context.getMutableTermForType(otherType, proto);
    break;
  }
  }

  RequirementRules.emplace_back(subjectTerm, constraintTerm);
}

/// Record information about a protocol if we have no seen it yet.
void RuleBuilder::addProtocol(const ProtocolDecl *proto,
                              bool initialComponent) {
  if (ProtocolMap.count(proto) > 0)
    return;

  ProtocolMap[proto] = initialComponent;
  Protocols.push_back(proto);
}

void RuleBuilder::collectRulesFromReferencedProtocols() {
  unsigned i = 0;
  while (i < Protocols.size()) {
    auto *proto = Protocols[i++];
    for (auto *depProto : proto->getProtocolDependencies()) {
      addProtocol(depProto, /*initialComponent=*/false);
    }
  }

  // Add rewrite rules for each protocol.
  for (auto *proto : Protocols) {
    if (Dump) {
      llvm::dbgs() << "protocol " << proto->getName() << " {\n";
    }

    MutableTerm lhs;
    lhs.add(Symbol::forProtocol(proto, Context));
    lhs.add(Symbol::forProtocol(proto, Context));

    MutableTerm rhs;
    rhs.add(Symbol::forProtocol(proto, Context));

    PermanentRules.emplace_back(lhs, rhs);

    for (auto *assocType : proto->getAssociatedTypeMembers())
      addAssociatedType(assocType, proto);

    for (auto *inheritedProto : Context.getInheritedProtocols(proto)) {
      for (auto *assocType : inheritedProto->getAssociatedTypeMembers())
        addAssociatedType(assocType, proto);
    }

    // If this protocol is part of the initial connected component, we're
    // building requirement signatures for all protocols in this component,
    // and so we must start with the structural requirements.
    //
    // Otherwise, we should either already have a requirement signature, or
    // we can trigger the computation of the requirement signatures of the
    // next component recursively.
    if (ProtocolMap[proto]) {
      for (auto req : proto->getStructuralRequirements())
        addRequirement(req.req.getCanonical(), proto);
    } else {
      for (auto req : proto->getRequirementSignature())
        addRequirement(req.getCanonical(), proto);
    }

    if (Dump) {
      llvm::dbgs() << "}\n";
    }
  }
}

void RequirementMachine::verify(const MutableTerm &term) const {
#ifndef NDEBUG
  // If the term is in the generic parameter domain, ensure we have a valid
  // generic parameter.
  if (term.begin()->getKind() == Symbol::Kind::GenericParam) {
    auto *genericParam = term.begin()->getGenericParam();
    TypeArrayView<GenericTypeParamType> genericParams = getGenericParams();
    auto found = std::find(genericParams.begin(),
                           genericParams.end(),
                           genericParam);
    if (found == genericParams.end()) {
      llvm::errs() << "Bad generic parameter in " << term << "\n";
      dump(llvm::errs());
      abort();
    }
  }

  MutableTerm erased;

  // First, "erase" resolved associated types from the term, and try
  // to simplify it again.
  for (auto symbol : term) {
    if (erased.empty()) {
      switch (symbol.getKind()) {
      case Symbol::Kind::Protocol:
      case Symbol::Kind::GenericParam:
        erased.add(symbol);
        continue;

      case Symbol::Kind::AssociatedType:
        erased.add(Symbol::forProtocol(symbol.getProtocols()[0], Context));
        break;

      case Symbol::Kind::Name:
      case Symbol::Kind::Layout:
      case Symbol::Kind::Superclass:
      case Symbol::Kind::ConcreteType:
        llvm::errs() << "Bad initial symbol in " << term << "\n";
        abort();
        break;
      }
    }

    switch (symbol.getKind()) {
    case Symbol::Kind::Name:
      assert(!erased.empty());
      erased.add(symbol);
      break;

    case Symbol::Kind::AssociatedType:
      erased.add(Symbol::forName(symbol.getName(), Context));
      break;

    case Symbol::Kind::Protocol:
    case Symbol::Kind::GenericParam:
    case Symbol::Kind::Layout:
    case Symbol::Kind::Superclass:
    case Symbol::Kind::ConcreteType:
      llvm::errs() << "Bad interior symbol " << symbol << " in " << term << "\n";
      abort();
      break;
    }
  }

  MutableTerm simplified = erased;
  System.simplify(simplified);

  // We should end up with the same term.
  if (simplified != term) {
    llvm::errs() << "Term verification failed\n";
    llvm::errs() << "Initial term:    " << term << "\n";
    llvm::errs() << "Erased term:     " << erased << "\n";
    llvm::errs() << "Simplified term: " << simplified << "\n";
    llvm::errs() << "\n";
    dump(llvm::errs());
    abort();
  }
#endif
}

void RequirementMachine::dump(llvm::raw_ostream &out) const {
  out << "Requirement machine for ";
  if (Sig)
    out << Sig;
  else if (!Params.empty()) {
    out << "fresh signature ";
    for (auto paramTy : Params)
      out << " " << Type(paramTy);
  } else {
    assert(!Protos.empty());
    out << "protocols [";
    for (auto *proto : Protos) {
      out << " " << proto->getName();
    }
    out << " ]";
  }
  out << "\n";

  System.dump(out);
  Map.dump(out);

  out << "Conformance access paths: {\n";
  for (auto pair : ConformanceAccessPaths) {
    out << "- " << pair.first.first << " : ";
    out << pair.first.second->getName() << " => ";
    pair.second.print(out);
    out << "\n";
  }
  out << "}\n";
}

RequirementMachine::RequirementMachine(RewriteContext &ctx)
    : Context(ctx), System(ctx), Map(System) {
  auto &langOpts = ctx.getASTContext().LangOpts;
  Dump = langOpts.DumpRequirementMachine;
  RequirementMachineStepLimit = langOpts.RequirementMachineStepLimit;
  RequirementMachineDepthLimit = langOpts.RequirementMachineDepthLimit;
  Stats = ctx.getASTContext().Stats;
}

RequirementMachine::~RequirementMachine() {}

/// Build a requirement machine for the requirements of a generic signature.
///
/// This must only be called exactly once, before any other operations are
/// performed on this requirement machine.
void RequirementMachine::initWithGenericSignature(CanGenericSignature sig) {
  Sig = sig;
  Params.append(sig.getGenericParams().begin(),
                sig.getGenericParams().end());

  PrettyStackTraceGenericSignature debugStack("building rewrite system for", sig);

  auto &ctx = Context.getASTContext();
  auto *Stats = ctx.Stats;

  if (Stats)
    ++Stats->getFrontendCounters().NumRequirementMachines;

  FrontendStatsTracer tracer(Stats, "build-rewrite-system");

  if (Dump) {
    llvm::dbgs() << "Adding generic signature " << sig << " {\n";
  }

  // Collect the top-level requirements, and all transtively-referenced
  // protocol requirement signatures.
  RuleBuilder builder(Context, Dump);
  builder.addRequirements(sig.getRequirements());

  // Add the initial set of rewrite rules to the rewrite system.
  System.initialize(/*recordLoops=*/false,
                    std::move(builder.PermanentRules),
                    std::move(builder.RequirementRules));

  computeCompletion(RewriteSystem::DisallowInvalidRequirements);

  if (Dump) {
    llvm::dbgs() << "}\n";
  }
}

/// Build a requirement machine for the structural requirements of a set
/// of protocols, which are understood to form a strongly-connected component
/// (SCC) of the protocol dependency graph.
///
/// This must only be called exactly once, before any other operations are
/// performed on this requirement machine.
void RequirementMachine::initWithProtocols(ArrayRef<const ProtocolDecl *> protos) {
  Protos = protos;

  auto &ctx = Context.getASTContext();
  auto *Stats = ctx.Stats;

  if (Stats)
    ++Stats->getFrontendCounters().NumRequirementMachines;

  FrontendStatsTracer tracer(Stats, "build-rewrite-system");

  if (Dump) {
    llvm::dbgs() << "Adding protocols";
    for (auto *proto : protos) {
      llvm::dbgs() << " " << proto->getName();
    }
    llvm::dbgs() << " {\n";
  }

  RuleBuilder builder(Context, Dump);
  builder.addProtocols(protos);

  // Add the initial set of rewrite rules to the rewrite system.
  System.initialize(/*recordLoops=*/true,
                    std::move(builder.PermanentRules),
                    std::move(builder.RequirementRules));

  // FIXME: Only if the protocols were written in source, though.
  computeCompletion(RewriteSystem::AllowInvalidRequirements);

  if (Dump) {
    llvm::dbgs() << "}\n";
  }
}

/// Build a requirement machine from a set of generic parameters and
/// (possibly non-canonical or non-minimal) structural requirements.
void RequirementMachine::initWithAbstractRequirements(
    ArrayRef<GenericTypeParamType *> genericParams,
    ArrayRef<Requirement> requirements) {
  Params.append(genericParams.begin(), genericParams.end());

  auto &ctx = Context.getASTContext();
  auto *Stats = ctx.Stats;

  if (Stats)
    ++Stats->getFrontendCounters().NumRequirementMachines;

  FrontendStatsTracer tracer(Stats, "build-rewrite-system");

  if (Dump) {
    llvm::dbgs() << "Adding generic parameters:";
    for (auto *paramTy : genericParams)
      llvm::dbgs() << " " << Type(paramTy);
    llvm::dbgs() << "\n";
  }

  // Collect the top-level requirements, and all transtively-referenced
  // protocol requirement signatures.
  RuleBuilder builder(Context, Dump);
  builder.addRequirements(requirements);

  // Add the initial set of rewrite rules to the rewrite system.
  System.initialize(/*recordLoops=*/true,
                    std::move(builder.PermanentRules),
                    std::move(builder.RequirementRules));

  computeCompletion(RewriteSystem::AllowInvalidRequirements);

  if (Dump) {
    llvm::dbgs() << "}\n";
  }
}

/// Attempt to obtain a confluent rewrite system using the completion
/// procedure.
void RequirementMachine::computeCompletion(RewriteSystem::ValidityPolicy policy) {
  while (true) {
    // First, run the Knuth-Bendix algorithm to resolve overlapping rules.
    auto result = System.computeConfluentCompletion(
        RequirementMachineStepLimit,
        RequirementMachineDepthLimit);

    if (Stats) {
      Stats->getFrontendCounters()
          .NumRequirementMachineCompletionSteps += result.second;
    }

    // Check for failure.
    auto checkCompletionResult = [&]() {
      switch (result.first) {
      case CompletionResult::Success:
        break;

      case CompletionResult::MaxIterations:
        llvm::errs() << "Generic signature " << Sig
                     << " exceeds maximum completion step count\n";
        System.dump(llvm::errs());
        abort();

      case CompletionResult::MaxDepth:
        llvm::errs() << "Generic signature " << Sig
                     << " exceeds maximum completion depth\n";
        System.dump(llvm::errs());
        abort();
      }
    };

    checkCompletionResult();

    // Check invariants.
    System.verifyRewriteRules(policy);

    // Build the property map, which also performs concrete term
    // unification; if this added any new rules, run the completion
    // procedure again.
    result = Map.buildPropertyMap(
        RequirementMachineStepLimit,
        RequirementMachineDepthLimit);

    if (Stats) {
      Stats->getFrontendCounters()
        .NumRequirementMachineUnifiedConcreteTerms += result.second;
    }

    checkCompletionResult();

    // If buildPropertyMap() added new rules, we run another round of
    // Knuth-Bendix, and build the property map again.
    if (result.second == 0)
      break;
  }

  if (Dump) {
    dump(llvm::dbgs());
  }

  assert(!Complete);
  Complete = true;
}

bool RequirementMachine::isComplete() const {
  return Complete;
}
