//===--- ParseDecl.cpp - Swift Language Parser for Declarations -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Declaration Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "swift/Parse/DelayedParsingCallbacks.h"
#include "swift/Parse/ParsedSyntaxRecorder.h"
#include "swift/Parse/ParseSILSupport.h"
#include "swift/Parse/SyntaxParsingContext.h"
#include "swift/Syntax/SyntaxKind.h"
#include "swift/Subsystems.h"
#include "swift/AST/Attr.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/DebuggerClient.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/StringExtras.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
#include <algorithm>

using namespace swift;
using namespace syntax;

namespace {
  /// A RAII object for deciding whether this DeclKind needs special
  /// treatment when parsing in the "debugger context", and implementing
  /// that treatment.  The problem arises because, when lldb
  /// uses swift to parse expressions, it needs to emulate the current
  /// frame's scope. We do that, for instance, by making a class extension
  /// and running the code in a function in that extension.
  ///
  /// This causes two kinds of issues:
  /// 1) Some DeclKinds require to be parsed in TopLevel contexts only.
  /// 2) Sometimes the debugger wants a Decl to live beyond the current
  /// function invocation, in which case it should be parsed at the
  /// file scope level so it will be set up correctly for this purpose.
  ///
  /// Creating an instance of this object will cause it to figure out
  /// whether we are in the debugger function, whether it needs to swap 
  /// the Decl that is currently being parsed.
  /// If you have created the object, instead of returning the result
  /// with makeParserResult, use the object's fixupParserResult.  If
  /// no swap has occurred, these methods will work the same.  
  /// If the decl has been moved, then Parser::markWasHandled will be
  /// called on the Decl, and you should call declWasHandledAlready
  /// before you consume the Decl to see if you actually need to
  /// consume it.
  /// If you are making one of these objects to address issue 1, call
  /// the constructor that only takes a DeclKind, and it will be moved
  /// unconditionally.  Otherwise pass in the Name and DeclKind and the
  /// DebuggerClient will be asked whether to move it or not.
  class DebuggerContextChange {
  protected:
    Parser &P;
    Identifier Name;
    SourceFile *SF;
    Optional<Parser::ContextChange> CC;
  public:
    DebuggerContextChange (Parser &P)
      : P(P), SF(nullptr) {
      if (!inDebuggerContext())
        return;
      else
        switchContext();
    }
    
    DebuggerContextChange (Parser &P, Identifier &Name, DeclKind Kind)
      : P(P), Name(Name), SF(nullptr) {
      if (!inDebuggerContext())
        return;
      bool globalize = false;
        
      DebuggerClient *debug_client = getDebuggerClient();
      if (!debug_client)
        return;
      
      globalize = debug_client->shouldGlobalize(Name, Kind);
        
      if (globalize)
        switchContext();
    }
    
    bool movedToTopLevel() {
      return CC.hasValue();
    }
    
    template <typename T>
    ParserResult<T>
    fixupParserResult(ParserResult<T> &Result) {
      ParserStatus Status = Result;
      return fixupParserResult(Status, Result.getPtrOrNull());
    }
    
    template <typename T>
    ParserResult<T>
    fixupParserResult(T *D) {
      if (CC.hasValue()) {
        swapDecl(D);
      }
      return ParserResult<T>(D);
    }
    
    template <typename T>
    ParserResult<T>
    fixupParserResult(ParserStatus Status, T *D) {
      if (CC.hasValue() && !Status.isError()) {
        // If there is an error, don't do our splicing trick,
        // just return the Decl and the status for reporting.
        swapDecl(D);
      }
      return makeParserResult(Status, D);
    }

    // The destructor doesn't need to do anything, the CC's destructor will
    // pop the context if we set it.
    ~DebuggerContextChange () {}
  protected:
  
    DebuggerClient *getDebuggerClient()
    {
      ModuleDecl *PM = P.CurDeclContext->getParentModule();
      if (!PM)
          return nullptr;
      else
           return PM->getDebugClient();
    }
    
    bool inDebuggerContext() {
      if (!P.Context.LangOpts.DebuggerSupport)
        return false;
      if (!P.CurDeclContext)
        return false;
      auto *func_decl = dyn_cast<FuncDecl>(P.CurDeclContext);
      if (!func_decl)
        return false;
        
      if (!func_decl->getAttrs().hasAttribute<LLDBDebuggerFunctionAttr>())
        return false;
      
      return true;
    }
    
    void switchContext () {
      SF = P.CurDeclContext->getParentSourceFile();
      CC.emplace (P, SF);
    }
    
    void swapDecl (Decl *D)
    {
      assert (SF);
      DebuggerClient *debug_client = getDebuggerClient();
      assert (debug_client);
      debug_client->didGlobalize(D);
      SF->Decls.push_back(D);
      P.markWasHandled(D);
    }
  };
} // end anonymous namespace

void PersistentParserState::parseMembers(IterableDeclContext *IDC) {
  SourceFile &SF = *IDC->getDecl()->getDeclContext()->getParentSourceFile();
  assert(!SF.hasInterfaceHash() &&
    "Cannot delay parsing if we care about the interface hash.");
  assert(SF.Kind != SourceFileKind::SIL && "cannot delay parsing SIL");
  unsigned BufferID = *SF.getBufferID();

  // MarkedPos is not useful for delayed parsing because we know where we should
  // jump the parser to. However, we should recover the MarkedPos here in case
  // the PersistentParserState will be used to continuously parse the rest of
  // the file linearly.
  llvm::SaveAndRestore<ParserPosition> Pos(MarkedPos, ParserPosition());

  // Lexer diaganostics have been emitted during skipping, so we disable lexer's
  // diagnostic engine here.
  Parser TheParser(BufferID, SF, /*No Lexer Diags*/nullptr, nullptr, this);
  // Disable libSyntax creation in the delayed parsing.
  TheParser.SyntaxContext->setDiscard();
  TheParser.parseDeclListDelayed(IDC);
}

/// Main entrypoint for the parser.
///
/// \verbatim
///   top-level:
///     stmt-brace-item*
///     decl-sil       [[only in SIL mode]
///     decl-sil-stage [[only in SIL mode]
/// \endverbatim
bool Parser::parseTopLevel() {
  SF.ASTStage = SourceFile::Parsing;

  // Prime the lexer.
  if (Tok.is(tok::NUM_TOKENS))
    consumeTokenWithoutFeedingReceiver();

  // Parse the body of the file.
  SmallVector<ASTNode, 128> Items;

  // If we are in SIL mode, and if the first token is the start of a sil
  // declaration, parse that one SIL function and return to the top level.  This
  // allows type declarations and other things to be parsed, name bound, and
  // type checked in batches, similar to immediate mode.  This also enforces
  // that SIL bodies can only be at the top level.
  if (Tok.is(tok::kw_sil)) {
    assert(isInSILMode() && "'sil' should only be a keyword in SIL mode");
    SIL->parseDeclSIL(*this);
  } else if (Tok.is(tok::kw_sil_stage)) {
    assert(isInSILMode() && "'sil_stage' should only be a keyword in SIL mode");
    SIL->parseDeclSILStage(*this);
  } else if (Tok.is(tok::kw_sil_vtable)) {
    assert(isInSILMode() &&
           "'sil_vtable' should only be a keyword in SIL mode");
    SIL->parseSILVTable(*this);
  } else if (Tok.is(tok::kw_sil_global)) {
    assert(isInSILMode() &&
           "'sil_global' should only be a keyword in SIL mode");
    SIL->parseSILGlobal(*this);
  } else if (Tok.is(tok::kw_sil_witness_table)) {
    assert(isInSILMode() &&
           "'sil_witness_table' should only be a keyword in SIL mode");
    SIL->parseSILWitnessTable(*this);
  } else if (Tok.is(tok::kw_sil_default_witness_table)) {
    assert(isInSILMode() &&
           "'sil_default_witness_table' should only be a keyword in SIL mode");
    SIL->parseSILDefaultWitnessTable(*this);
  } else if (Tok.is(tok::kw_sil_coverage_map)) {
    assert(isInSILMode() &&
           "'sil_coverage_map' should only be a keyword in SIL mode");
    SIL->parseSILCoverageMap(*this);
  } else if (Tok.is(tok::kw_sil_property)) {
    assert(isInSILMode() &&
           "'sil_property' should only be a keyword in SIL mode");
    SIL->parseSILProperty(*this);
  } else if (Tok.is(tok::kw_sil_scope)) {
    assert(isInSILMode() && "'sil_scope' should only be a keyword in SIL mode");
    SIL->parseSILScope(*this);
  } else {
    parseBraceItems(Items,
                    allowTopLevelCode() ? BraceItemListKind::TopLevelCode
                                        : BraceItemListKind::TopLevelLibrary);
  }
  
  // In the case of a catastrophic parse error, consume any trailing
  // #else, #elseif, or #endif and move on to the next statement or declaration
  // block.
  if (Tok.is(tok::pound_else) || Tok.is(tok::pound_elseif) ||
      Tok.is(tok::pound_endif)) {
    diagnose(Tok.getLoc(),
             diag::unexpected_conditional_compilation_block_terminator);
    consumeToken();
  }

  // If this is a Main source file, determine if we found code that needs to be
  // executed (this is used by the repl to know whether to compile and run the
  // newly parsed stuff).
  bool FoundTopLevelCodeToExecute = false;
  if (allowTopLevelCode()) {
    for (auto V : Items) {
      if (isa<TopLevelCodeDecl>(V.get<Decl*>()))
        FoundTopLevelCodeToExecute = true;
    }
  }

  // Add newly parsed decls to the module.
  for (auto Item : Items) {
    if (auto *D = Item.dyn_cast<Decl*>()) {
      assert(!isa<AccessorDecl>(D) && "accessors should not be added here");
      SF.Decls.push_back(D);
    }
  }

  // Note that the source file is fully parsed and verify it.
  SF.ASTStage = SourceFile::Parsed;
  verify(SF);

  // Next time start relexing from the beginning of the comment so that we can
  // attach it to the token.
  State->markParserPosition(getParserPosition(),
                            InPoundLineEnvironment);

  // If we are done parsing the whole file, finalize the token receiver.
  if (Tok.is(tok::eof)) {
    SyntaxContext->addToken(Tok, LeadingTrivia, TrailingTrivia);
    TokReceiver->finalize();
  }

  return FoundTopLevelCodeToExecute;
}

ParserResult<AvailableAttr> Parser::parseExtendedAvailabilitySpecList(
    SourceLoc AtLoc, SourceLoc AttrLoc, StringRef AttrName) {
  // Check 'Tok', return false if ':' or '=' cannot be found.
  // Complain if '=' is found and suggest replacing it with ": ".
  auto findAttrValueDelimiter = [&]() -> bool {
    if (!Tok.is(tok::colon)) {
      if (!Tok.is(tok::equal))
        return false;

      diagnose(Tok.getLoc(), diag::replace_equal_with_colon_for_value)
          .fixItReplace(Tok.getLoc(), ": ");
    }
    return true;
  };

  struct VersionArg {
    llvm::VersionTuple Version;
    SourceRange Range;
    SourceLoc DelimiterLoc;
    bool empty() const {
      return Version.empty();
    }
  };

  StringRef Platform = Tok.getText();

  StringRef Message, Renamed;
  VersionArg Introduced, Deprecated, Obsoleted;
  auto PlatformAgnostic = PlatformAgnosticAvailabilityKind::None;

  SyntaxParsingContext AvailabilitySpecContext(
      SyntaxContext, SyntaxKind::AvailabilitySpecList);

  bool HasUpcomingEntry = false;

  {
    SyntaxParsingContext EntryContext(SyntaxContext,
                                      SyntaxKind::AvailabilityArgument);
    consumeToken();
    if (consumeIf(tok::comma)) {
      HasUpcomingEntry = true;
    }
  }

  bool AnyAnnotations = false;
  bool AnyArgumentInvalid = false;
  int ParamIndex = 0;

  while (HasUpcomingEntry) {
    SyntaxParsingContext EntryContext(SyntaxContext,
                                      SyntaxKind::AvailabilityArgument);
    auto ArgumentLoc = Tok.getLoc();
    AnyAnnotations = true;
    StringRef ArgumentKindStr = Tok.getText();
    ParamIndex++;

    enum {
      IsMessage, IsRenamed,
      IsIntroduced, IsDeprecated, IsObsoleted,
      IsUnavailable,
      IsInvalid
    } ArgumentKind = IsInvalid;
    
    if (Tok.is(tok::identifier)) {
      ArgumentKind =
      llvm::StringSwitch<decltype(ArgumentKind)>(ArgumentKindStr)
      .Case("message", IsMessage)
      .Case("renamed", IsRenamed)
      .Case("introduced", IsIntroduced)
      .Case("deprecated", IsDeprecated)
      .Case("obsoleted", IsObsoleted)
      .Case("unavailable", IsUnavailable)
      .Default(IsInvalid);
    }

    if (ArgumentKind == IsInvalid) {
      diagnose(ArgumentLoc, diag::attr_availability_expected_option, AttrName)
          .highlight(SourceRange(ArgumentLoc));
      if (Tok.is(tok::code_complete) && CodeCompletion) {
        CodeCompletion->completeDeclAttrParam(DAK_Available, ParamIndex);
        consumeToken(tok::code_complete);
      } else {
        consumeIf(tok::identifier);
      }
      return nullptr;
    }

    consumeToken();

    auto diagnoseDuplicate = [&](bool WasEmpty) {
      if (!WasEmpty) {
        diagnose(ArgumentLoc, diag::attr_availability_invalid_duplicate,
                 ArgumentKindStr);
      }
    };

    switch (ArgumentKind) {
    case IsMessage:
    case IsRenamed: {
      // Items with string arguments.
      if (findAttrValueDelimiter()) {
        consumeToken();
      } else {
        diagnose(Tok, diag::attr_availability_expected_equal, AttrName,
                 ArgumentKindStr);
        AnyArgumentInvalid = true;
        if (peekToken().isAny(tok::r_paren, tok::comma))
          consumeToken();
        break;
      }

      if (!Tok.is(tok::string_literal)) {
        diagnose(AttrLoc, diag::attr_expected_string_literal, AttrName);
        AnyArgumentInvalid = true;
        if (peekToken().isAny(tok::r_paren, tok::comma))
          consumeToken();
        break;
      }

      auto Value = getStringLiteralIfNotInterpolated(
          AttrLoc, ("'" + ArgumentKindStr + "'").str());
      consumeToken();
      if (!Value) {
        AnyArgumentInvalid = true;
        break;
      }

      if (ArgumentKind == IsMessage) {
        diagnoseDuplicate(Message.empty());
        Message = Value.getValue();
      } else {
        ParsedDeclName parsedName = parseDeclName(Value.getValue());
        if (!parsedName) {
          diagnose(AttrLoc, diag::attr_availability_invalid_renamed, AttrName);
          AnyArgumentInvalid = true;
          break;
        }
        diagnoseDuplicate(Renamed.empty());
        Renamed = Value.getValue();
      }

      SyntaxContext->createNodeInPlace(SyntaxKind::AvailabilityLabeledArgument);

      break;
    }

    case IsDeprecated:
      if (!findAttrValueDelimiter()) {
        if (PlatformAgnostic != PlatformAgnosticAvailabilityKind::None) {
          diagnose(Tok, diag::attr_availability_unavailable_deprecated,
                   AttrName);
        }

        PlatformAgnostic = PlatformAgnosticAvailabilityKind::Deprecated;
        break;
      }
      LLVM_FALLTHROUGH;

    case IsIntroduced:
    case IsObsoleted: {
      // Items with version arguments.
      SourceLoc DelimiterLoc;
      if (findAttrValueDelimiter()) {
        DelimiterLoc = Tok.getLoc();
        consumeToken();
      } else {
        diagnose(Tok, diag::attr_availability_expected_equal, AttrName,
                 ArgumentKindStr);
        AnyArgumentInvalid = true;
        if (peekToken().isAny(tok::r_paren, tok::comma))
          consumeToken();
        break;
      }

      auto &VerArg =
          (ArgumentKind == IsIntroduced)
              ? Introduced
              : (ArgumentKind == IsDeprecated) ? Deprecated : Obsoleted;

      bool VerArgWasEmpty = VerArg.empty();
      if (parseVersionTuple(
              VerArg.Version, VerArg.Range,
              Diagnostic(diag::attr_availability_expected_version, AttrName))) {
        AnyArgumentInvalid = true;
        if (peekToken().isAny(tok::r_paren, tok::comma))
          consumeToken();
      }
      VerArg.DelimiterLoc = DelimiterLoc;
      diagnoseDuplicate(VerArgWasEmpty);

      SyntaxContext->createNodeInPlace(SyntaxKind::AvailabilityLabeledArgument);

      break;
    }

    case IsUnavailable:
      if (PlatformAgnostic != PlatformAgnosticAvailabilityKind::None) {
        diagnose(Tok, diag::attr_availability_unavailable_deprecated, AttrName);
      }

      PlatformAgnostic = PlatformAgnosticAvailabilityKind::Unavailable;
      break;

    case IsInvalid:
      llvm_unreachable("handled above");
    }

    // Parse the trailing comma
    if (consumeIf(tok::comma)) {
      HasUpcomingEntry = true;
    } else {
      HasUpcomingEntry = false;
    }
  }

  if (!AnyAnnotations) {
    diagnose(Tok.getLoc(), diag::attr_expected_comma, AttrName,
             /*isDeclModifier*/ false);
  }

  auto PlatformKind = platformFromString(Platform);

  // Treat 'swift' as a valid version-qualifying token, when
  // at least some versions were mentioned and no other
  // platform-agnostic availability spec has been provided.
  bool SomeVersion = (!Introduced.empty() ||
                      !Deprecated.empty() ||
                      !Obsoleted.empty());
  if (!PlatformKind.hasValue() &&
      (Platform == "swift" || Platform == "_PackageDescription")) {

    if (PlatformAgnostic == PlatformAgnosticAvailabilityKind::Deprecated) {
      diagnose(AttrLoc,
               diag::attr_availability_platform_agnostic_expected_deprecated_version,
               AttrName, Platform);
      return nullptr;
    }
    if (PlatformAgnostic == PlatformAgnosticAvailabilityKind::Unavailable) {
      diagnose(AttrLoc, diag::attr_availability_platform_agnostic_infeasible_option,
               "unavailable", AttrName, Platform);
      return nullptr;
    }
    assert(PlatformAgnostic == PlatformAgnosticAvailabilityKind::None);

    if (!SomeVersion) {
      diagnose(AttrLoc, diag::attr_availability_platform_agnostic_expected_option,
               AttrName, Platform);
      return nullptr;
    }

    PlatformKind = PlatformKind::none;
    PlatformAgnostic = (Platform == "swift") ?
                         PlatformAgnosticAvailabilityKind::SwiftVersionSpecific :
                         PlatformAgnosticAvailabilityKind::PackageDescriptionVersionSpecific;
  }


  if (AnyArgumentInvalid)
    return nullptr;
  if (!PlatformKind.hasValue()) {
    diagnose(AttrLoc, diag::attr_availability_unknown_platform,
           Platform, AttrName);
    return nullptr;
  }

  // Warn if any version is specified for non-specific platform '*'.
  if (Platform == "*" && SomeVersion) {
    auto diag = diagnose(AttrLoc,
        diag::attr_availability_nonspecific_platform_unexpected_version,
        AttrName);
    if (!Introduced.empty())
      diag.fixItRemove(SourceRange(Introduced.DelimiterLoc,
                                   Introduced.Range.End));
    if (!Deprecated.empty())
      diag.fixItRemove(SourceRange(Deprecated.DelimiterLoc,
                                   Deprecated.Range.End));
    if (!Obsoleted.empty())
      diag.fixItRemove(SourceRange(Obsoleted.DelimiterLoc,
                                   Obsoleted.Range.End));
    return nullptr;
  }

  auto Attr = new (Context)
  AvailableAttr(AtLoc, SourceRange(AttrLoc, Tok.getLoc()),
                PlatformKind.getValue(),
                Message, Renamed,
                Introduced.Version, Introduced.Range,
                Deprecated.Version, Deprecated.Range,
                Obsoleted.Version, Obsoleted.Range,
                PlatformAgnostic,
                /*Implicit=*/false);
  return makeParserResult(Attr);

}

bool Parser::parseSpecializeAttributeArguments(
    swift::tok ClosingBrace, bool &DiscardAttribute, Optional<bool> &Exported,
    Optional<SpecializeAttr::SpecializationKind> &Kind,
    swift::TrailingWhereClause *&TrailingWhereClause) {
  SyntaxParsingContext ContentContext(SyntaxContext,
                                      SyntaxKind::SpecializeAttributeSpecList);
  // Parse optional "exported" and "kind" labeled parameters.
  while (!Tok.is(tok::kw_where)) {
    SyntaxParsingContext ArgumentContext(SyntaxContext,
                                         SyntaxKind::LabeledSpecializeEntry);
    if (Tok.is(tok::identifier)) {
      auto ParamLabel = Tok.getText();
      if (ParamLabel != "exported" && ParamLabel != "kind") {
        diagnose(Tok.getLoc(), diag::attr_specialize_unknown_parameter_name,
                 ParamLabel);
      }
      consumeToken();
      if (!consumeIf(tok::colon)) {
        diagnose(Tok.getLoc(), diag::attr_specialize_missing_colon, ParamLabel);
        skipUntil(tok::comma, tok::kw_where);
        if (Tok.is(ClosingBrace))
          break;
        if (Tok.is(tok::kw_where)) {
          continue;
        }
        if (Tok.is(tok::comma)) {
          consumeToken();
          continue;
        }
        DiscardAttribute = true;
        return false;
      }
      if ((ParamLabel == "exported" && Exported.hasValue()) ||
          (ParamLabel == "kind" && Kind.hasValue())) {
        diagnose(Tok.getLoc(), diag::attr_specialize_parameter_already_defined,
                 ParamLabel);
      }
      if (ParamLabel == "exported") {
        bool isTrue = consumeIf(tok::kw_true);
        bool isFalse = consumeIf(tok::kw_false);
        if (!isTrue && !isFalse) {
          diagnose(Tok.getLoc(), diag::attr_specialize_expected_bool_value);
          skipUntil(tok::comma, tok::kw_where);
          if (Tok.is(ClosingBrace))
            break;
          if (Tok.is(tok::kw_where)) {
            continue;
          }
          if (Tok.is(tok::comma)) {
            consumeToken();
            continue;
          }
          DiscardAttribute = true;
          return false;
        }
        if (ParamLabel == "exported") {
          Exported = isTrue ? true : false;
        }
      }
      if (ParamLabel == "kind") {
        SourceLoc paramValueLoc;
        if (Tok.is(tok::identifier)) {
          if (Tok.getText() == "partial") {
            Kind = SpecializeAttr::SpecializationKind::Partial;
          } else if (Tok.getText() == "full") {
            Kind = SpecializeAttr::SpecializationKind::Full;
          } else {
            diagnose(Tok.getLoc(),
                     diag::attr_specialize_expected_partial_or_full);
          }
          consumeToken();
        } else if (consumeIf(tok::kw_true, paramValueLoc) ||
                   consumeIf(tok::kw_false, paramValueLoc)) {
          diagnose(paramValueLoc,
                   diag::attr_specialize_expected_partial_or_full);
        }
      }
      if (!consumeIf(tok::comma)) {
        diagnose(Tok.getLoc(), diag::attr_specialize_missing_comma);
        skipUntil(tok::comma, tok::kw_where);
        if (Tok.is(ClosingBrace))
          break;
        if (Tok.is(tok::kw_where)) {
          continue;
        }
        if (Tok.is(tok::comma)) {
          consumeToken();
          continue;
        }
        DiscardAttribute = true;
        return false;
      }
      continue;
    }
    diagnose(Tok.getLoc(),
             diag::attr_specialize_missing_parameter_label_or_where_clause);
    DiscardAttribute = true;
    return false;
  };

  // Parse the where clause.
  if (Tok.is(tok::kw_where)) {
    SourceLoc whereLoc;
    SmallVector<RequirementRepr, 4> requirements;
    bool firstTypeInComplete;
    parseGenericWhereClause(whereLoc, requirements, firstTypeInComplete,
                            /* AllowLayoutConstraints */ true);
    TrailingWhereClause =
        TrailingWhereClause::create(Context, whereLoc, requirements);
  }
  return true;
}

bool Parser::parseSpecializeAttribute(swift::tok ClosingBrace, SourceLoc AtLoc,
                                      SourceLoc Loc, SpecializeAttr *&Attr) {
  assert(ClosingBrace == tok::r_paren || ClosingBrace == tok::r_square);

  SourceLoc lParenLoc = consumeToken();
  bool DiscardAttribute = false;
  StringRef AttrName = "_specialize";

  Optional<bool> exported;
  Optional<SpecializeAttr::SpecializationKind> kind;

  TrailingWhereClause *trailingWhereClause = nullptr;

  if (!parseSpecializeAttributeArguments(ClosingBrace, DiscardAttribute,
                                         exported, kind, trailingWhereClause)) {
    return false;
  }

  // Parse the closing ')' or ']'.
  SourceLoc rParenLoc;
  if (!consumeIf(ClosingBrace, rParenLoc)) {
    if (ClosingBrace == tok::r_paren)
      diagnose(lParenLoc, diag::attr_expected_rparen, AttrName,
             /*DeclModifier=*/false);
    else if (ClosingBrace == tok::r_square)
      diagnose(lParenLoc, diag::attr_expected_rparen, AttrName,
             /*DeclModifier=*/false);
    return false;
  }
  // Not exported by default.
  if (!exported.hasValue())
    exported = false;
  // Full specialization by default.
  if (!kind.hasValue())
    kind = SpecializeAttr::SpecializationKind::Full;

  if (DiscardAttribute) {
    Attr = nullptr;
    return false;
  }
  // Store the attribute.
  Attr = SpecializeAttr::create(Context, AtLoc, SourceRange(Loc, rParenLoc),
                                trailingWhereClause, exported.getValue(),
                                kind.getValue());
  return true;
}

ParserResult<ImplementsAttr>
Parser::parseImplementsAttribute(SourceLoc AtLoc, SourceLoc Loc) {
  StringRef AttrName = "_implements";
  ParserStatus Status;

  if (Tok.isNot(tok::l_paren)) {
    diagnose(Loc, diag::attr_expected_lparen, AttrName,
             /*DeclModifier=*/false);
    Status.setIsParseError();
    return Status;
  }

  SourceLoc lParenLoc = consumeToken();

  DeclNameLoc MemberNameLoc;
  DeclName MemberName;
  ParserResult<TypeRepr> ProtocolType;
  {
    SyntaxParsingContext ContentContext(
        SyntaxContext, SyntaxKind::ImplementsAttributeArguments);
    ProtocolType = parseType();
    Status |= ProtocolType;

    if (!(Status.shouldStopParsing() || consumeIf(tok::comma))) {
      diagnose(Tok.getLoc(), diag::attr_expected_comma, AttrName,
               /*DeclModifier=*/false);
      Status.setIsParseError();
    }

    if (!Status.shouldStopParsing()) {
      MemberName =
          parseUnqualifiedDeclName(/*afterDot=*/false, MemberNameLoc,
                                   diag::attr_implements_expected_member_name,
                                   /*allowOperators=*/true,
                                   /*allowZeroArgCompoundNames=*/true);
      if (!MemberName) {
        Status.setIsParseError();
      }
    }
  }

  if (Status.isError()) {
    skipUntil(tok::r_paren);
  }

  SourceLoc rParenLoc;
  if (!consumeIf(tok::r_paren, rParenLoc)) {
    diagnose(lParenLoc, diag::attr_expected_rparen, AttrName,
             /*DeclModifier=*/false);
    Status.setIsParseError();
  }

  if (Status.isError()) {
    return Status;
  }

  return ParserResult<ImplementsAttr>(
    ImplementsAttr::create(Context, AtLoc, SourceRange(Loc, rParenLoc),
                           ProtocolType.get(), MemberName, MemberNameLoc));
}

void Parser::parseObjCSelector(SmallVector<Identifier, 4> &Names,
                               SmallVector<SourceLoc, 4> &NameLocs,
                               bool &IsNullarySelector) {
  IsNullarySelector = true;
  SyntaxParsingContext SelectorContext(SyntaxContext, SyntaxKind::ObjCSelector);
  while (true) {
    SyntaxParsingContext SelectorPieceContext(SyntaxContext,
                                              SyntaxKind::ObjCSelectorPiece);
    // Empty selector piece.
    if (Tok.is(tok::colon)) {
      Names.push_back(Identifier());
      NameLocs.push_back(Tok.getLoc());
      IsNullarySelector = false;
      consumeToken();
      continue;
    }

    // Name.
    if (Tok.is(tok::identifier) || Tok.isKeyword()) {
      Names.push_back(Context.getIdentifier(Tok.getText()));
      NameLocs.push_back(Tok.getLoc());
      consumeToken();

      // If we have a colon, consume it.
      if (Tok.is(tok::colon)) {
        consumeToken();
        IsNullarySelector = false;
        continue;
      }

      // If we see a closing parentheses, we're done.
      if (Tok.is(tok::r_paren)) {
        // If we saw more than one identifier, there's a ':'
        // missing here. Complain and pretend we saw it.
        if (Names.size() > 1) {
          diagnose(Tok, diag::attr_objc_missing_colon)
          .fixItInsertAfter(NameLocs.back(), ":");
          IsNullarySelector = false;
        }

        break;
      }

      // If we see another identifier or keyword, complain about
      // the missing colon and keep going.
      if (Tok.is(tok::identifier) || Tok.isKeyword()) {
        diagnose(Tok, diag::attr_objc_missing_colon)
        .fixItInsertAfter(NameLocs.back(), ":");
        IsNullarySelector = false;
        continue;
      }

      // We don't know what happened. Break out.
      break;
    }

    // We didn't parse anything, don't create a ObjCSelectorPiece
    SelectorPieceContext.setTransparent();
    break;
  }
}

bool Parser::parseNewDeclAttribute(DeclAttributes &Attributes, SourceLoc AtLoc,
                                   DeclAttrKind DK) {
  // Ok, it is a valid attribute, eat it, and then process it.
  StringRef AttrName = Tok.getText();
  SourceLoc Loc = consumeToken();

  bool DiscardAttribute = false;

  // Diagnose duplicated attributes.
  const DeclAttribute *DuplicateAttribute = nullptr;
  if (!DeclAttribute::allowMultipleAttributes(DK))
    if ((DuplicateAttribute = Attributes.getAttribute(DK))) {
      // Delay issuing the diagnostic until we parse the attribute.
      DiscardAttribute = true;
    }
 
  // If this is a SIL-only attribute, reject it.
  if ((DeclAttribute::getOptions(DK) & DeclAttribute::SILOnly) != 0 &&
      !isInSILMode()) {
    diagnose(Loc, diag::only_allowed_in_sil, AttrName);
    DiscardAttribute = true;
  }

  if (Context.LangOpts.Target.isOSBinFormatCOFF()) {
    if (DK == DAK_WeakLinked) {
      diagnose(Loc, diag::attr_unsupported_on_target, AttrName,
               Context.LangOpts.Target.str());
      DiscardAttribute = true;
    }
  }

  // Filled in during parsing.  If there is a duplicate
  // diagnostic this can be used for better error presentation.
  SourceRange AttrRange;

  switch (DK) {
  case DAK_Count:
    llvm_unreachable("DAK_Count should not appear in parsing switch");

  case DAK_RawDocComment:
  case DAK_ObjCBridged:
  case DAK_RestatedObjCConformance:
  case DAK_SynthesizedProtocol:
  case DAK_ClangImporterSynthesizedType:
  case DAK_Custom:
    llvm_unreachable("virtual attributes should not be parsed "
                     "by attribute parsing code");
  case DAK_SetterAccess:
    llvm_unreachable("handled by DAK_AccessControl");

#define SIMPLE_DECL_ATTR(_, CLASS, ...) \
  case DAK_##CLASS: \
    if (!DiscardAttribute) \
      Attributes.add(new (Context) CLASS##Attr(AtLoc, Loc)); \
    break;
#include "swift/AST/Attr.def"

  case DAK_Effects: {
    if (!consumeIf(tok::l_paren)) {
      diagnose(Loc, diag::attr_expected_lparen, AttrName,
               DeclAttribute::isDeclModifier(DK));      return false;
    }

    if (Tok.isNot(tok::identifier)) {
      diagnose(Loc, diag::effects_attribute_expect_option, AttrName);
      return false;
    }

    EffectsKind kind;
    if (Tok.getText() == "readonly")
      kind = EffectsKind::ReadOnly;
    else if (Tok.getText() == "readnone")
      kind = EffectsKind::ReadNone;
    else if (Tok.getText() == "readwrite")
      kind = EffectsKind::ReadWrite;
    else if (Tok.getText() == "releasenone")
      kind = EffectsKind::ReleaseNone;
    else {
      diagnose(Loc, diag::effects_attribute_unknown_option,
               Tok.getText(), AttrName);
      return false;
    }
    AttrRange = SourceRange(Loc, Tok.getRange().getStart());
    consumeToken(tok::identifier);

    if (!consumeIf(tok::r_paren)) {
      diagnose(Loc, diag::attr_expected_rparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    if (!DiscardAttribute)
      Attributes.add(new (Context) EffectsAttr(AtLoc, AttrRange, kind));
    break;
  }

  case DAK_Inline: {
    if (!consumeIf(tok::l_paren)) {
      diagnose(Loc, diag::attr_expected_lparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    if (Tok.isNot(tok::identifier)) {
      diagnose(Loc, diag::optimization_attribute_expect_option, AttrName,
               "none");
      return false;
    }

    InlineKind kind;
    if (Tok.getText() == "never")
      kind = InlineKind::Never;
    else if (Tok.getText() == "__always")
      kind = InlineKind::Always;
    else {
      diagnose(Loc, diag::optimization_attribute_unknown_option,
               Tok.getText(), AttrName);
      return false;
    }
    consumeToken(tok::identifier);
    AttrRange = SourceRange(Loc, Tok.getRange().getStart());
    
    if (!consumeIf(tok::r_paren)) {
      diagnose(Loc, diag::attr_expected_rparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    if (!DiscardAttribute)
      Attributes.add(new (Context) InlineAttr(AtLoc, AttrRange, kind));

    break;
  }

  case DAK_Optimize: {
    if (!consumeIf(tok::l_paren)) {
      diagnose(Loc, diag::attr_expected_lparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    if (Tok.isNot(tok::identifier)) {
      diagnose(Loc, diag::optimization_attribute_expect_option, AttrName,
               "speed");
      return false;
    }

    OptimizationMode optMode = OptimizationMode::NotSet;
    if (Tok.getText() == "none")
      optMode = OptimizationMode::NoOptimization;
    else if (Tok.getText() == "speed")
      optMode = OptimizationMode::ForSpeed;
    else if (Tok.getText() == "size")
      optMode = OptimizationMode::ForSize;
    else {
      diagnose(Loc, diag::optimization_attribute_unknown_option,
               Tok.getText(), AttrName);
      return false;
    }
    consumeToken(tok::identifier);
    AttrRange = SourceRange(Loc, Tok.getRange().getStart());

    if (!consumeIf(tok::r_paren)) {
      diagnose(Loc, diag::attr_expected_rparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    if (!DiscardAttribute)
      Attributes.add(new (Context) OptimizeAttr(AtLoc, AttrRange, optMode));

    break;
  }

  case DAK_ReferenceOwnership: {
    // Handle weak/unowned/unowned(unsafe).
    auto Kind = AttrName == "weak" ? ReferenceOwnership::Weak
                                   : ReferenceOwnership::Unowned;
    SourceLoc EndLoc = Loc;

    if (Kind == ReferenceOwnership::Unowned && Tok.is(tok::l_paren)) {
      // Parse an optional specifier after unowned.
      SourceLoc lp = consumeToken(tok::l_paren);
      if (Tok.is(tok::identifier) && Tok.getText() == "safe") {
        consumeToken();
      } else if (Tok.is(tok::identifier) && Tok.getText() == "unsafe") {
        consumeToken();
        Kind = ReferenceOwnership::Unmanaged;
      } else {
        diagnose(Tok, diag::attr_unowned_invalid_specifier);
        consumeIf(tok::identifier);
      }

      SourceLoc rp;
      parseMatchingToken(tok::r_paren, rp, diag::attr_unowned_expected_rparen,
                         lp);
      EndLoc = rp;
    }

    if (!DiscardAttribute)
      Attributes.add(
          new (Context) ReferenceOwnershipAttr(SourceRange(Loc, EndLoc), Kind));
    break;
  }

  case DAK_AccessControl: {

    // Diagnose using access control in a local scope, which isn't meaningful.
    if (CurDeclContext->isLocalContext()) {
      diagnose(Loc, diag::attr_only_at_non_local_scope, AttrName);
    }

    AccessLevel access = llvm::StringSwitch<AccessLevel>(AttrName)
      .Case("private", AccessLevel::Private)
      .Case("fileprivate", AccessLevel::FilePrivate)
      .Case("internal", AccessLevel::Internal)
      .Case("public", AccessLevel::Public)
      .Case("open", AccessLevel::Open);

    if (!consumeIf(tok::l_paren)) {
      // Normal access control attribute.
      AttrRange = Loc;
      DuplicateAttribute = Attributes.getAttribute<AccessControlAttr>();
      if (!DuplicateAttribute)
        Attributes.add(new (Context) AccessControlAttr(AtLoc, Loc, access));
      break;
    }

    // Parse the subject.
    if (Tok.isContextualKeyword("set")) {
      consumeToken();
    } else {
      diagnose(Loc, diag::attr_access_expected_set, AttrName);
      // Minimal recovery: if there's a single token and then an r_paren,
      // consume them both. If there's just an r_paren, consume that.
      if (!consumeIf(tok::r_paren)) {
        if (Tok.isNot(tok::l_paren) && peekToken().is(tok::r_paren)) {
          consumeToken();
          consumeToken(tok::r_paren);
        }
      }
      return false;
    }

    AttrRange = SourceRange(Loc, Tok.getLoc());

    if (!consumeIf(tok::r_paren)) {
      diagnose(Loc, diag::attr_expected_rparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    DuplicateAttribute = Attributes.getAttribute<SetterAccessAttr>();
    if (!DuplicateAttribute) {
      Attributes.add(new (Context) SetterAccessAttr(AtLoc, AttrRange, access));
    }

    break;
  }

  case DAK_CDecl:
  case DAK_SILGenName: {
    if (!consumeIf(tok::l_paren)) {
      diagnose(Loc, diag::attr_expected_lparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    if (Tok.isNot(tok::string_literal)) {
      diagnose(Loc, diag::attr_expected_string_literal, AttrName);
      return false;
    }

    Optional<StringRef> AsmName = getStringLiteralIfNotInterpolated(
        Loc, ("'" + AttrName + "'").str());

    consumeToken(tok::string_literal);

    if (AsmName.hasValue())
      AttrRange = SourceRange(Loc, Tok.getRange().getStart());
    else
      DiscardAttribute = true;

    if (!consumeIf(tok::r_paren)) {
      diagnose(Loc, diag::attr_expected_rparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    // Diagnose using @_silgen_name in a local scope.  These don't
    // actually work.
    if (CurDeclContext->isLocalContext()) {
      // Emit an error, but do not discard the attribute.  This enables
      // better recovery in the parser.
      diagnose(Loc, diag::attr_only_at_non_local_scope, AttrName);
    }

    if (!DiscardAttribute) {
      if (DK == DAK_SILGenName)
        Attributes.add(new (Context) SILGenNameAttr(AsmName.getValue(), AtLoc,
                                                AttrRange, /*Implicit=*/false));
      else if (DK == DAK_CDecl)
        Attributes.add(new (Context) CDeclAttr(AsmName.getValue(), AtLoc,
                                               AttrRange, /*Implicit=*/false));
      else
        llvm_unreachable("out of sync with switch");
    }

    break;
  }
  
  case DAK_Alignment: {
    if (!consumeIf(tok::l_paren)) {
      diagnose(Loc, diag::attr_expected_lparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }
    
    if (Tok.isNot(tok::integer_literal)) {
      diagnose(Loc, diag::alignment_must_be_positive_integer);
      return false;
    }
    
    StringRef alignmentText = Tok.getText();
    unsigned alignmentValue;
    if (alignmentText.getAsInteger(0, alignmentValue)) {
      diagnose(Loc, diag::alignment_must_be_positive_integer);
      return false;
    }
    
    consumeToken(tok::integer_literal);
    
    auto range = SourceRange(Loc, Tok.getRange().getStart());
    
    if (!consumeIf(tok::r_paren)) {
      diagnose(Loc, diag::attr_expected_rparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    Attributes.add(new (Context) AlignmentAttr(alignmentValue, AtLoc, range,
                                               /*implicit*/ false));
    
    break;
  }
  
  case DAK_SwiftNativeObjCRuntimeBase: {
    if (!consumeIf(tok::l_paren)) {
      diagnose(Loc, diag::attr_expected_lparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    if (Tok.isNot(tok::identifier)) {
      diagnose(Loc, diag::swift_native_objc_runtime_base_must_be_identifier);
      return false;
    }
    
    Identifier name;
    consumeIdentifier(&name);
    
    auto range = SourceRange(Loc, Tok.getRange().getStart());

    if (!consumeIf(tok::r_paren)) {
      diagnose(Loc, diag::attr_expected_rparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }
    
    Attributes.add(new (Context) SwiftNativeObjCRuntimeBaseAttr(name,
                                            AtLoc, range, /*implicit*/ false));
    break;
  }
  
  case DAK_Semantics: {
    if (!consumeIf(tok::l_paren)) {
      diagnose(Loc, diag::attr_expected_lparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    if (Tok.isNot(tok::string_literal)) {
      diagnose(Loc, diag::attr_expected_string_literal, AttrName);
      return false;
    }

    auto Value = getStringLiteralIfNotInterpolated(
        Loc, ("'" + AttrName + "'").str());

    consumeToken(tok::string_literal);

    if (Value.hasValue())
      AttrRange = SourceRange(Loc, Tok.getRange().getStart());
    else
      DiscardAttribute = true;

    if (!consumeIf(tok::r_paren)) {
      diagnose(Loc, diag::attr_expected_rparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    // Diagnose using @_semantics in a local scope.  These don't
    // actually work.
    if (CurDeclContext->isLocalContext()) {
      // Emit an error, but do not discard the attribute.  This enables
      // better recovery in the parser.
      diagnose(Loc, diag::attr_only_at_non_local_scope, AttrName);
    }

    if (!DiscardAttribute)
      Attributes.add(new (Context) SemanticsAttr(Value.getValue(), AtLoc,
                                                 AttrRange,
                                                 /*Implicit=*/false));
    break;
  }

  case DAK_Available: {
    if (!consumeIf(tok::l_paren)) {
      diagnose(Loc, diag::attr_expected_lparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    // platform:
    //   *
    //   identifier
    if (!Tok.is(tok::identifier) &&
        !(Tok.isAnyOperator() && Tok.getText() == "*")) {
      if (Tok.is(tok::code_complete) && CodeCompletion) {
        CodeCompletion->completeDeclAttrParam(DAK_Available, 0);
        consumeToken(tok::code_complete);
      }
      diagnose(Tok.getLoc(), diag::attr_availability_platform, AttrName)
        .highlight(SourceRange(Tok.getLoc()));
      consumeIf(tok::r_paren);
      return false;
    }

    // Delay processing of platform until later, after we have
    // parsed more of the attribute.
    StringRef Platform = Tok.getText();

    if (Platform != "*" &&
        peekToken().isAny(tok::integer_literal, tok::floating_literal)) {
      // We have the short form of available: @available(iOS 8.0.1, *)
      SmallVector<AvailabilitySpec *, 5> Specs;
      ParserStatus Status = parseAvailabilitySpecList(Specs);

      if (Status.isError())
        return false;

      AttrRange = SourceRange(Loc, Tok.getLoc());
      // For each platform version spec in the spec list, create an
      // implicit AvailableAttr for the platform with the introduced
      // version from the spec. For example, if we have
      //   @available(iOS 8.0, OSX 10.10, *):
      // we will synthesize:
      //  @available(iOS, introduced: 8.0)
      //  @available(OSX, introduced: 10.10)
      //
      // Similarly if we have a language version spec or PackageDescription
      // version in the spec list, create an implicit AvailableAttr
      // with the specified version as the introduced argument. 
      // For example, if we have
      //   @available(swift 3.1)
      // we will synthesize
      //   @available(swift, introduced: 3.1)
      // or, if we have
      //   @available(_PackageDescription 4.2)
      // we will synthesize
      //   @available(_PackageDescription, introduced: 4.2)

      for (auto *Spec : Specs) {
        PlatformKind Platform;
        llvm::VersionTuple Version;
        SourceRange VersionRange;
        PlatformAgnosticAvailabilityKind PlatformAgnostic;

        if (auto *PlatformVersionSpec =
            dyn_cast<PlatformVersionConstraintAvailabilitySpec>(Spec)) {
          Platform = PlatformVersionSpec->getPlatform();
          Version = PlatformVersionSpec->getVersion();
          VersionRange = PlatformVersionSpec->getVersionSrcRange();
          PlatformAgnostic = PlatformAgnosticAvailabilityKind::None;

        } else if (auto *PlatformAgnosticVersionSpec =
                   dyn_cast<PlatformAgnosticVersionConstraintAvailabilitySpec>(Spec)) {
          Platform = PlatformKind::none;
          Version = PlatformAgnosticVersionSpec->getVersion();
          VersionRange = PlatformAgnosticVersionSpec->getVersionSrcRange();
          PlatformAgnostic = PlatformAgnosticVersionSpec->isLanguageVersionSpecific() ?
                               PlatformAgnosticAvailabilityKind::SwiftVersionSpecific :
                               PlatformAgnosticAvailabilityKind::PackageDescriptionVersionSpecific;

        } else {
          continue;
        }

        Attributes.add(new (Context)
                       AvailableAttr(AtLoc, AttrRange,
                                     Platform,
                                     /*Message=*/StringRef(),
                                     /*Rename=*/StringRef(),
                                     /*Introduced=*/Version,
                                     /*IntroducedRange=*/VersionRange,
                                     /*Deprecated=*/llvm::VersionTuple(),
                                     /*DeprecatedRange=*/SourceRange(),
                                     /*Obsoleted=*/llvm::VersionTuple(),
                                     /*ObsoletedRange=*/SourceRange(),
                                     PlatformAgnostic,
                                     /*Implicit=*/false));
      }

      if (!consumeIf(tok::r_paren)) {
        diagnose(Tok.getLoc(), diag::attr_expected_rparen, AttrName,
                 DeclAttribute::isDeclModifier(DK));
        return false;
      }

      break;
    }

    auto AvailabilityAttr = parseExtendedAvailabilitySpecList(AtLoc, Loc,
                                                              AttrName);
    DiscardAttribute |= AvailabilityAttr.isParseError();

    if (!consumeIf(tok::r_paren)) {
      if (!DiscardAttribute) {
        diagnose(Tok.getLoc(), diag::attr_expected_rparen, AttrName,
                 DeclAttribute::isDeclModifier(DK));
      }
      return false;
    }

    if (!DiscardAttribute) {
      Attributes.add(AvailabilityAttr.get());
    } else {
      return false;
    }
    break;
  }
  case DAK_PrivateImport: {
    // Parse the leading '('.
    if (Tok.isNot(tok::l_paren)) {
      diagnose(Loc, diag::attr_expected_lparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }
    SourceLoc LParenLoc = consumeToken(tok::l_paren);
    Optional<StringRef> filename;
    {
      SyntaxParsingContext ContentContext(
          SyntaxContext, SyntaxKind::NamedAttributeStringArgument);

      // Parse 'sourceFile'.
      if (Tok.getText() != "sourceFile") {
        diagnose(LParenLoc, diag::attr_private_import_expected_sourcefile);
        return false;
      }
      auto ForLoc = consumeToken();

      // Parse ':'.
      if (Tok.getKind() != tok::colon) {
        diagnose(ForLoc, diag::attr_private_import_expected_colon);
        return false;
      }
      auto ColonLoc = consumeToken(tok::colon);

      // Parse '"'function-name'"'
      if (Tok.isNot(tok::string_literal)) {
        diagnose(ColonLoc, diag::attr_private_import_expected_sourcefile_name);
        return false;
      }
      filename = getStringLiteralIfNotInterpolated(Loc, "_private");
      if (!filename.hasValue()) {
        diagnose(ColonLoc, diag::attr_private_import_expected_sourcefile_name);
        return false;
      }
      consumeToken(tok::string_literal);
    }
    // Parse the matching ')'.
    SourceLoc RParenLoc;
    bool Invalid = parseMatchingToken(tok::r_paren, RParenLoc,
                                      diag::attr_private_import_expected_rparen,
                                      LParenLoc);
    if (Invalid)
      return false;
    auto *attr = PrivateImportAttr::create(Context, AtLoc, Loc, LParenLoc,
                                           *filename, RParenLoc);
    Attributes.add(attr);

    break;
  }
  case DAK_ObjC: {
    // Unnamed @objc attribute.
    if (Tok.isNot(tok::l_paren)) {
      auto attr = ObjCAttr::createUnnamed(Context, AtLoc, Loc);
      Attributes.add(attr);
      break;
    }

    // Parse the leading '('.
    SourceLoc LParenLoc = consumeToken(tok::l_paren);

    // Parse the names, with trailing colons (if there are present) and populate
    // the inout parameters
    SmallVector<Identifier, 4> Names;
    SmallVector<SourceLoc, 4> NameLocs;
    bool NullarySelector = true;
    parseObjCSelector(Names, NameLocs, NullarySelector);

    // Parse the matching ')'.
    SourceLoc RParenLoc;
    bool Invalid = parseMatchingToken(tok::r_paren, RParenLoc,
                                      diag::attr_objc_expected_rparen,
                                      LParenLoc);

    ObjCAttr *attr;
    if (Names.empty()) {
      // When there are no names, recover as if there were no parentheses.
      if (!Invalid)
        diagnose(LParenLoc, diag::attr_objc_empty_name);
      attr = ObjCAttr::createUnnamed(Context, AtLoc, Loc);
    } else if (NullarySelector) {
      // When we didn't see a colon, this is a nullary name.
      assert(Names.size() == 1 && "Forgot to set sawColon?");
      attr = ObjCAttr::createNullary(Context, AtLoc, Loc, LParenLoc,
                                     NameLocs.front(), Names.front(),
                                     RParenLoc);
    } else {
      // When we did see a colon, this is a selector.
      attr = ObjCAttr::createSelector(Context, AtLoc, Loc, LParenLoc,
                                      NameLocs, Names, RParenLoc);
    }
    Attributes.add(attr);
    break;
  }
  case DAK_ObjCRuntimeName: {
    if (!consumeIf(tok::l_paren)) {
      diagnose(Loc, diag::attr_expected_lparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    if (Tok.isNot(tok::identifier)) {
      diagnose(Loc, diag::objc_runtime_name_must_be_identifier);
      return false;
    }

    auto name = Tok.getText();

    consumeToken(tok::identifier);

    auto range = SourceRange(Loc, Tok.getRange().getStart());

    if (!consumeIf(tok::r_paren)) {
      diagnose(Loc, diag::attr_expected_rparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    Attributes.add(new (Context) ObjCRuntimeNameAttr(name, AtLoc, range,
                                                     /*implicit*/ false));
    break;
  }


  case DAK_DynamicReplacement: {
    // Parse the leading '('.
    if (Tok.isNot(tok::l_paren)) {
      diagnose(Loc, diag::attr_expected_lparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    SourceLoc LParenLoc = consumeToken(tok::l_paren);
    DeclName replacedFunction;
    {
      SyntaxParsingContext ContentContext(
          SyntaxContext, SyntaxKind::NamedAttributeStringArgument);

      // Parse 'for'.
      if (Tok.getText() != "for") {
        diagnose(Loc, diag::attr_dynamic_replacement_expected_for);
        return false;
      }
      auto ForLoc = consumeToken();

      // Parse ':'.
      if (Tok.getText() != ":") {
        diagnose(ForLoc, diag::attr_dynamic_replacement_expected_colon);
        return false;
      }
      consumeToken(tok::colon);
      {
        SyntaxParsingContext ContentContext(SyntaxContext,
                                            SyntaxKind::DeclName);

        DeclNameLoc loc;
        replacedFunction = parseUnqualifiedDeclName(
            true, loc, diag::attr_dynamic_replacement_expected_function,
            /*allowOperators*/ true, /*allowZeroArgCompoundNames*/ true,
            /*allowDeinitAndSubscript*/ true);
      }
    }

    // Parse the matching ')'.
    SourceLoc RParenLoc;
    bool Invalid = parseMatchingToken(
        tok::r_paren, RParenLoc, diag::attr_dynamic_replacement_expected_rparen,
        LParenLoc);
    if (Invalid) {
      return false;
    }


    DynamicReplacementAttr *attr = DynamicReplacementAttr::create(
        Context, AtLoc, Loc, LParenLoc, replacedFunction, RParenLoc);
    Attributes.add(attr);
    break;
  }

  case DAK_Specialize: {
    if (Tok.isNot(tok::l_paren)) {
      diagnose(Loc, diag::attr_expected_lparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }
    SpecializeAttr *Attr;
    if (!parseSpecializeAttribute(tok::r_paren, AtLoc, Loc, Attr))
      return false;

    Attributes.add(Attr);
    break;
    }

  case DAK_Implements: {
    ParserResult<ImplementsAttr> Attr = parseImplementsAttribute(AtLoc, Loc);
    if (Attr.isNonNull()) {
      Attributes.add(Attr.get());
    }
    break;
  }

  case DAK_ProjectedValueProperty: {
    if (!consumeIf(tok::l_paren)) {
      diagnose(Loc, diag::attr_expected_lparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    if (Tok.isNot(tok::identifier)) {
      diagnose(Loc, diag::projection_value_property_not_identifier);
      return false;
    }

    Identifier name;
    consumeIdentifier(&name, /*allowDollarIdentifier=*/true);

    auto range = SourceRange(Loc, Tok.getRange().getStart());

    if (!consumeIf(tok::r_paren)) {
      diagnose(Loc, diag::attr_expected_rparen, AttrName,
               DeclAttribute::isDeclModifier(DK));
      return false;
    }

    Attributes.add(new (Context) ProjectedValuePropertyAttr(
        name, AtLoc, range, /*implicit*/ false));
    break;
  }
  }

  if (DuplicateAttribute) {
    diagnose(Loc, diag::duplicate_attribute, DeclAttribute::isDeclModifier(DK))
      .highlight(AttrRange);
    diagnose(DuplicateAttribute->getLocation(),
             diag::previous_attribute,
             DeclAttribute::isDeclModifier(DK))
      .highlight(DuplicateAttribute->getRange());
  }

    // If this is a decl modifier spelled with an @, emit an error and remove it
  // with a fixit.
  if (AtLoc.isValid() && DeclAttribute::isDeclModifier(DK))
    diagnose(AtLoc, diag::cskeyword_not_attribute, AttrName).fixItRemove(AtLoc);
  
  return false;
}

bool Parser::parseVersionTuple(llvm::VersionTuple &Version,
                               SourceRange &Range,
                               const Diagnostic &D) {
  SyntaxParsingContext VersionContext(SyntaxContext, SyntaxKind::VersionTuple);
  // A version number is either an integer (8), a float (8.1), or a
  // float followed by a dot and an integer (8.1.0).
  if (!Tok.isAny(tok::integer_literal, tok::floating_literal)) {
    diagnose(Tok, D);
    return true;
  }

  SourceLoc StartLoc = Tok.getLoc();
  
  if (Tok.is(tok::integer_literal)) {
    unsigned major = 0;
    if (Tok.getText().getAsInteger(10, major)) {
      // Maybe the literal was in hex. Reject that.
      diagnose(Tok, D);
      consumeToken();
      return true;
    }
    Version = llvm::VersionTuple(major);
    Range = SourceRange(StartLoc, Tok.getLoc());
    consumeToken();
    return false;
  }

  unsigned major = 0, minor = 0;
  StringRef majorPart, minorPart;
  std::tie(majorPart, minorPart) = Tok.getText().split('.');
  if (majorPart.getAsInteger(10, major) || minorPart.getAsInteger(10, minor)) {
    // Reject things like 0.1e5 and hex literals.
    diagnose(Tok, D);
    consumeToken();
    return true;
  }

  Range = SourceRange(StartLoc, Tok.getLoc());
  consumeToken();
  
  if (consumeIf(tok::period)) {
    unsigned micro = 0;
    if (!Tok.is(tok::integer_literal) ||
        Tok.getText().getAsInteger(10, micro)) {
      // Reject things like 0.1e5 and hex literals.
      diagnose(Tok, D);
      if (Tok.is(tok::integer_literal) ||
          peekToken().isAny(tok::r_paren, tok::comma))
        consumeToken();
      return true;
    }
    
    Range = SourceRange(StartLoc, Tok.getLoc());
    consumeToken();
    
    Version = llvm::VersionTuple(major, minor, micro);
  } else {
    Version = llvm::VersionTuple(major, minor);
  }

  return false;
}

/// Check whether the attributes have already established an initializer
/// context within the given set of attributes.
static PatternBindingInitializer *findAttributeInitContent(
    DeclAttributes &Attributes) {
  for (auto custom : Attributes.getAttributes<CustomAttr>()) {
    if (auto initContext = custom->getInitContext())
      return initContext;
  }

  return nullptr;
}

/// \verbatim
///   attribute:
///     '_silgen_name' '(' identifier ')'
///     'semantics' '(' identifier ')'
///     'infix' '=' numeric_constant
///     'unary'
///     'stdlib'
///     'weak'
///     'inout'
///     'unowned'
///     'unowned' '(' 'safe' ')'
///     'unowned' '(' 'unsafe' ')'
///     'noreturn'
///     'optional'
///     'mutating'
///     ( 'private' | 'internal' | 'public' )
///     ( 'private' | 'internal' | 'public' ) '(' 'set' ')'
///     'requires_stored_property_inits'
/// \endverbatim
///
/// Note that various attributes (like mutating, weak, and unowned) are parsed
/// but rejected since they have context-sensitive keywords.
///
ParserStatus Parser::parseDeclAttribute(DeclAttributes &Attributes, SourceLoc AtLoc) {
  // If this not an identifier, the attribute is malformed.
  if (Tok.isNot(tok::identifier) &&
      Tok.isNot(tok::kw_in) &&
      Tok.isNot(tok::kw_inout)) {

    if (Tok.is(tok::code_complete)) {
      if (CodeCompletion) {
        // If the next token is not on the same line, this attribute might be
        // starting new declaration instead of adding attribute to existing
        // decl.
        auto isIndependent = peekToken().isAtStartOfLine();
        CodeCompletion->completeDeclAttrBeginning(isInSILMode(), isIndependent);
      }
      consumeToken(tok::code_complete);
      return makeParserCodeCompletionStatus();
    }

    diagnose(Tok, diag::expected_attribute_name);
    return makeParserError();
  }

  // If the attribute follows the new representation, switch
  // over to the alternate parsing path.
  DeclAttrKind DK = DeclAttribute::getAttrKindFromString(Tok.getText());
  
  auto checkInvalidAttrName = [&](StringRef invalidName,
                                  StringRef correctName,
                                  DeclAttrKind kind,
                                  Optional<Diag<StringRef, StringRef>> diag = None) {
    if (DK == DAK_Count && Tok.getText() == invalidName) {
      DK = kind;

      if (diag) {
        diagnose(Tok, *diag, invalidName, correctName)
            .fixItReplace(Tok.getLoc(), correctName);
      }
    }
  };

  // Check if attr is availability, and suggest available instead
  checkInvalidAttrName("availability", "available", DAK_Available, diag::attr_renamed);

  // Check if attr is inlineable, and suggest inlinable instead
  checkInvalidAttrName("inlineable", "inlinable", DAK_Inlinable, diag::attr_name_close_match);

  // In Swift 5 and above, these become hard errors. In Swift 4.2, emit a
  // warning for compatibility. Otherwise, don't diagnose at all.
  if (Context.isSwiftVersionAtLeast(5)) {
    checkInvalidAttrName("_versioned", "usableFromInline", DAK_UsableFromInline, diag::attr_renamed);
    checkInvalidAttrName("_inlineable", "inlinable", DAK_Inlinable, diag::attr_renamed);
  } else if (Context.isSwiftVersionAtLeast(4, 2)) {
    checkInvalidAttrName("_versioned", "usableFromInline", DAK_UsableFromInline, diag::attr_renamed_warning);
    checkInvalidAttrName("_inlineable", "inlinable", DAK_Inlinable, diag::attr_renamed_warning);
  } else {
    checkInvalidAttrName("_versioned", "usableFromInline", DAK_UsableFromInline);
    checkInvalidAttrName("_inlineable", "inlinable", DAK_Inlinable);
  }

  // Other names of property wrappers...
  checkInvalidAttrName("propertyDelegate", "propertyWrapper",
                       DAK_PropertyWrapper, diag::attr_renamed_warning);
  checkInvalidAttrName("_propertyWrapper", "propertyWrapper",
                       DAK_PropertyWrapper, diag::attr_renamed_warning);

  if (DK == DAK_Count && Tok.getText() == "warn_unused_result") {
    // The behavior created by @warn_unused_result is now the default. Emit a
    // Fix-It to remove.
    SourceLoc attrLoc = consumeToken();

    // @warn_unused_result with no arguments.
    if (Tok.isNot(tok::l_paren)) {
      diagnose(AtLoc, diag::attr_warn_unused_result_removed)
        .fixItRemove(SourceRange(AtLoc, attrLoc));

      // Recovered.
      return makeParserSuccess();
    }

    // @warn_unused_result with arguments.
    SourceLoc lParenLoc = consumeToken();
    skipUntil(tok::r_paren);

    // Parse the closing ')'.
    SourceLoc rParenLoc;
    if (Tok.isNot(tok::r_paren)) {
      parseMatchingToken(tok::r_paren, rParenLoc,
                         diag::attr_warn_unused_result_expected_rparen,
                         lParenLoc);
    }
    if (Tok.is(tok::r_paren)) {
      rParenLoc = consumeToken();
    }

    diagnose(AtLoc, diag::attr_warn_unused_result_removed)
      .fixItRemove(SourceRange(AtLoc, rParenLoc));

    // Recovered.
    return makeParserSuccess();
  }

  if (DK != DAK_Count && !DeclAttribute::shouldBeRejectedByParser(DK)) {
    parseNewDeclAttribute(Attributes, AtLoc, DK);
    return makeParserSuccess();
  }

  if (TypeAttributes::getAttrKindFromString(Tok.getText()) != TAK_Count)
    diagnose(Tok, diag::type_attribute_applied_to_decl);
  else if (Tok.isContextualKeyword("unknown")) {
    diagnose(Tok, diag::unknown_attribute, "unknown");
  } else {
    // Change the context to create a custom attribute syntax.
    SyntaxContext->setCreateSyntax(SyntaxKind::CustomAttribute);
    // Parse a custom attribute.
    auto type = parseType(diag::expected_type);
    if (type.hasCodeCompletion() || type.isNull()) {
      if (Tok.is(tok::l_paren))
        skipSingle();

      return ParserStatus(type);
    }

    // Parse the optional arguments.
    SourceLoc lParenLoc, rParenLoc;
    SmallVector<Expr *, 2> args;
    SmallVector<Identifier, 2> argLabels;
    SmallVector<SourceLoc, 2> argLabelLocs;
    Expr *trailingClosure = nullptr;
    bool hasInitializer = false;
    ParserStatus status;

    // If we're not in a local context, we'll need a context to parse
    // initializers into (should we have one).  This happens for properties
    // and global variables in libraries.
    PatternBindingInitializer *initContext = nullptr;

    if (Tok.isFollowingLParen()) {
      if (peekToken().is(tok::code_complete)) {
        consumeToken(tok::l_paren);
        if (CodeCompletion) {
          auto typeE = new (Context) TypeExpr(type.get());
          auto CCE = new (Context) CodeCompletionExpr(Tok.getLoc());
          CodeCompletion->completePostfixExprParen(typeE, CCE);
        }
        consumeToken(tok::code_complete);
        skipUntil(tok::r_paren);
        consumeIf(tok::r_paren);
        status.setHasCodeCompletion();
      } else {
        // If we have no local context to parse the initial value into, create
        // one for the PBD we'll eventually create.  This allows us to have
        // reasonable DeclContexts for any closures that may live inside of
        // initializers.
        Optional<ParseFunctionBody> initParser;
        if (!CurDeclContext->isLocalContext()) {
          initContext = findAttributeInitContent(Attributes);
          if (!initContext)
            initContext =
                new (Context) PatternBindingInitializer(CurDeclContext);

          initParser.emplace(*this, initContext);
        }
        status |= parseExprList(tok::l_paren, tok::r_paren,
                                /*isPostfix=*/false, /*isExprBasic=*/true,
                                lParenLoc, args, argLabels, argLabelLocs,
                                rParenLoc, trailingClosure,
                                SyntaxKind::FunctionCallArgumentList);
        assert(!trailingClosure && "Cannot parse a trailing closure here");
        hasInitializer = true;
      }
    }

    // Form the attribute.
    auto attr = CustomAttr::create(Context, AtLoc, type.get(), hasInitializer,
                                   initContext, lParenLoc, args, argLabels,
                                   argLabelLocs, rParenLoc);
    Attributes.add(attr);
    return status;
  }

  // Recover by eating @foo(...) when foo is not known.
  consumeToken();
  if (Tok.is(tok::l_paren))
    skipSingle();

  return makeParserError();
}

bool Parser::canParseTypeAttribute() {
  TypeAttributes attrs; // ignored
  return !parseTypeAttribute(attrs, /*justChecking*/ true);
}

/// \verbatim
///   attribute-type:
///     'noreturn'
/// \endverbatim
///
/// \param justChecking - if true, we're just checking whether we
///   canParseTypeAttribute; don't emit any diagnostics, and there's
///   no need to actually record the attribute
bool Parser::parseTypeAttribute(TypeAttributes &Attributes, bool justChecking) {
  // If this not an identifier, the attribute is malformed.
  if (Tok.isNot(tok::identifier) &&
      // These are keywords that we accept as attribute names.
      Tok.isNot(tok::kw_in) && Tok.isNot(tok::kw_inout)) {
    if (!justChecking)
      diagnose(Tok, diag::expected_attribute_name);
    return true;
  }
  
  // Determine which attribute it is, and diagnose it if unknown.
  TypeAttrKind attr = TypeAttributes::getAttrKindFromString(Tok.getText());

  if (attr == TAK_Count) {
    if (justChecking) return true;

    auto declAttrID = DeclAttribute::getAttrKindFromString(Tok.getText());
    if (declAttrID == DAK_Count) {
      // Not a decl or type attribute.
      diagnose(Tok, diag::unknown_attribute, Tok.getText());
    } else {
      // Otherwise this is a valid decl attribute so they should have put it on
      // the decl instead of the type.

      // If this is the first attribute, and if we are on a simple decl, emit a
      // fixit to move the attribute.  Otherwise, we don't have the location of
      // the @ sign, or we don't have confidence that the fixit will be right.
      if (!Attributes.empty() || StructureMarkers.empty() ||
          StructureMarkers.back().Kind != StructureMarkerKind::Declaration ||
          StructureMarkers.back().Loc.isInvalid() ||
          peekToken().is(tok::equal)) {
        diagnose(Tok, diag::decl_attribute_applied_to_type);
      } else {
        // Otherwise, this is the first type attribute and we know where the
        // declaration is.  Emit the same diagnostic, but include a fixit to
        // move the attribute.  Unfortunately, we don't have enough info to add
        // the attribute to DeclAttributes.
        diagnose(Tok, diag::decl_attribute_applied_to_type)
          .fixItRemove(SourceRange(Attributes.AtLoc, Tok.getLoc()))
          .fixItInsert(StructureMarkers.back().Loc,
                       "@" + Tok.getText().str()+" ");
      }
    }
    
    // Recover by eating @foo(...) when foo is not known.
    consumeToken();
    SyntaxParsingContext TokListContext(SyntaxContext, SyntaxKind::TokenList);

    if (Tok.is(tok::l_paren) && getEndOfPreviousLoc() == Tok.getLoc()) {
      BacktrackingScope backtrack(*this);
      skipSingle();
      // If we found '->', or 'throws' after paren, it's likely a parameter
      // of function type.
      if (Tok.isNot(tok::arrow, tok::kw_throws, tok::kw_rethrows,
                    tok::kw_throw))
        backtrack.cancelBacktrack();
    }
    return true;
  }
  
  // Ok, it is a valid attribute, eat it, and then process it.
  StringRef Text = Tok.getText();
  SourceLoc Loc = consumeToken();
  
  StringRef conventionName;
  StringRef witnessMethodProtocol;

  if (attr == TAK_convention) {
    SourceLoc LPLoc;
    if (!consumeIfNotAtStartOfLine(tok::l_paren)) {
      if (!justChecking)
        diagnose(Tok, diag::convention_attribute_expected_lparen);
      return true;
    }

    if (Tok.isNot(tok::identifier)) {
      if (!justChecking)
        diagnose(Tok, diag::convention_attribute_expected_name);
      return true;
    }

    conventionName = Tok.getText();
    consumeToken(tok::identifier);

    if (conventionName == "witness_method") {
      if (Tok.isNot(tok::colon)) {
        if (!justChecking)
          diagnose(Tok,
                   diag::convention_attribute_witness_method_expected_colon);
        return true;
      }
      consumeToken(tok::colon);
      if (Tok.isNot(tok::identifier)) {
        if (!justChecking)
          diagnose(Tok,
                   diag::convention_attribute_witness_method_expected_protocol);
        return true;
      }

      witnessMethodProtocol = Tok.getText();
      consumeToken(tok::identifier);
    }

    // Parse the ')'.  We can't use parseMatchingToken if we're in
    // just-checking mode.
    if (justChecking && Tok.isNot(tok::r_paren))
      return true;

    SourceLoc RPLoc;
    parseMatchingToken(tok::r_paren, RPLoc,
                       diag::convention_attribute_expected_rparen,
                       LPLoc);
  }


  // In just-checking mode, we only need to consume the tokens, and we don't
  // want to do any other analysis.
  if (justChecking)
    return false;

  // Diagnose duplicated attributes.
  if (Attributes.has(attr)) {
    diagnose(Loc, diag::duplicate_attribute, /*isModifier=*/false);
    return false;
  }

  // Handle any attribute-specific processing logic.
  switch (attr) {
  default: break;
  case TAK_autoclosure:
  case TAK_escaping:
  case TAK_noescape:
    break;

  case TAK_out:
  case TAK_in:
  case TAK_owned:
  case TAK_unowned_inner_pointer:
  case TAK_guaranteed:
  case TAK_autoreleased:
  case TAK_callee_owned:
  case TAK_callee_guaranteed:
  case TAK_objc_metatype:
    if (!isInSILMode()) {
      diagnose(Loc, diag::only_allowed_in_sil, Text);
      return false;
    }
    break;
    
  // Ownership attributes.
  case TAK_sil_weak:
  case TAK_sil_unowned:
    if (!isInSILMode()) {
      diagnose(Loc, diag::only_allowed_in_sil, Text);
      return false;
    }
      
    if (Attributes.hasOwnership()) {
      diagnose(Loc, diag::duplicate_attribute, /*isModifier*/false);
      return false;
    }
    break;

  // 'inout' attribute.
  case TAK_inout:
    if (!isInSILMode()) {
      diagnose(Loc, diag::inout_not_attribute);
      return false;
    }
    break;
      
  case TAK_opened: {
    if (!isInSILMode()) {
      diagnose(Loc, diag::only_allowed_in_sil, "opened");
      return false;
    }

    // Parse the opened existential ID string in parens
    SourceLoc beginLoc = Tok.getLoc(), idLoc, endLoc;
    if (consumeIfNotAtStartOfLine(tok::l_paren)) {
      if (Tok.is(tok::string_literal)) {
        UUID openedID;
        idLoc = Tok.getLoc();
        auto literalText = Tok.getText().slice(1, Tok.getText().size() - 1);
        llvm::SmallString<UUID::StringBufferSize> text(literalText);
        if (auto openedID = UUID::fromString(text.c_str())) {
          Attributes.OpenedID = openedID;
        } else {
          diagnose(Tok, diag::opened_attribute_id_value);
        }
        consumeToken();
      } else {
        diagnose(Tok, diag::opened_attribute_id_value);
      }
      parseMatchingToken(tok::r_paren, endLoc,
                         diag::opened_attribute_expected_rparen,
                         beginLoc);
    } else {
      diagnose(Tok, diag::opened_attribute_expected_lparen);
    }

    break;
  }

  // Convention attribute.
  case TAK_convention:
    Attributes.convention = conventionName;
    Attributes.conventionWitnessMethodProtocol = witnessMethodProtocol;
    break;
      
  case TAK__opaqueReturnTypeOf: {
    // Parse the mangled decl name and index.
    auto beginLoc = Tok.getLoc();
    if (!consumeIfNotAtStartOfLine(tok::l_paren)) {
      diagnose(Tok, diag::attr_expected_lparen, "_opaqueReturnTypeOf", false);
      return true;
    }
    
    if (!Tok.is(tok::string_literal)) {
      diagnose(Tok, diag::opened_attribute_id_value);
      return true;
    }
    auto mangling = Tok.getText().slice(1, Tok.getText().size() - 1);
    consumeToken(tok::string_literal);
    
    if (!Tok.is(tok::comma)) {
      diagnose(Tok, diag::attr_expected_comma, "_opaqueReturnTypeOf", false);
      return true;
    }
    consumeToken(tok::comma);
    
    if (!Tok.is(tok::integer_literal)) {
      diagnose(Tok, diag::attr_expected_string_literal, "_opaqueReturnTypeOf");
      return true;
    }
    
    unsigned index;
    if (Tok.getText().getAsInteger(10, index)) {
      diagnose(Tok, diag::attr_expected_string_literal, "_opaqueReturnTypeOf");
      return true;
    }
    consumeToken(tok::integer_literal);
    
    SourceLoc endLoc;
    parseMatchingToken(tok::r_paren, endLoc,
                       diag::expected_rparen_expr_list,
                       beginLoc);

    Attributes.setOpaqueReturnTypeOf(mangling, index);
    break;
  }
  }

  Attributes.setAttr(attr, Loc);
  return false;
}

/// \verbatim
///   attribute-list:
///     /*empty*/
///     attribute-list-clause attribute-list
///   attribute-list-clause:
///     '@' attribute
/// \endverbatim
ParserStatus Parser::parseDeclAttributeList(DeclAttributes &Attributes) {
  if (Tok.isNot(tok::at_sign))
    return makeParserSuccess();

  ParserStatus Status;
  SyntaxParsingContext AttrListCtx(SyntaxContext, SyntaxKind::AttributeList);
  do {
    SyntaxParsingContext AttrCtx(SyntaxContext, SyntaxKind::Attribute);
    SourceLoc AtLoc = consumeToken();
    Status |= parseDeclAttribute(Attributes, AtLoc);
  } while (Tok.is(tok::at_sign));
  return Status;
}

/// \verbatim
///   modifier-list
///     /* empty */
//      modifier modifier-list
//    modifier
//      'private'
//      'private' '(' 'set' ')'
//      'fileprivate'
//      'fileprivate' '(' 'set' )'
//      'internal'
//      'internal' '(' 'set' ')'
//      'public'
//      'open'
//      'weak'
//      'unowned'
//      'unowned' '(' 'safe' ')'
//      'unowned' '(' 'unsafe' ')'
//      'optional'
//      'required'
//      'lazy'
//      'final'
//      'dynamic'
//      'prefix'
//      'postfix'
//      'infix'
//      'override'
//      'mutating
//      'nonmutating'
//      '__consuming'
//      'convenience'
bool Parser::parseDeclModifierList(DeclAttributes &Attributes,
                                   SourceLoc &StaticLoc,
                                   StaticSpellingKind &StaticSpelling) {
  SyntaxParsingContext ListContext(SyntaxContext, SyntaxKind::ModifierList);
  bool isError = false;
  bool hasModifier = false;
  while (true) {
    switch (Tok.getKind()) {

    case tok::kw_private:
    case tok::kw_fileprivate:
    case tok::kw_internal:
    case tok::kw_public: {
      SyntaxParsingContext ModContext(SyntaxContext, SyntaxKind::DeclModifier);
      // We still model these specifiers as attributes.
      isError |=
          parseNewDeclAttribute(Attributes, /*AtLoc=*/{}, DAK_AccessControl);
      hasModifier = true;
      continue;
    }

    // Context sensitive keywords.
    case tok::identifier: {
      if (Tok.isEscapedIdentifier())
        break;

      DeclAttrKind Kind = llvm::StringSwitch<DeclAttrKind>(Tok.getText())
#define CONTEXTUAL_CASE(KW, CLASS) .Case(#KW, DAK_##CLASS)
#define CONTEXTUAL_DECL_ATTR(KW, CLASS, ...) CONTEXTUAL_CASE(KW, CLASS)
#define CONTEXTUAL_DECL_ATTR_ALIAS(KW, CLASS) CONTEXTUAL_CASE(KW, CLASS)
#define CONTEXTUAL_SIMPLE_DECL_ATTR(KW, CLASS, ...) CONTEXTUAL_CASE(KW, CLASS)
#include <swift/AST/Attr.def>
#undef CONTEXTUAL_CASE
        .Default(DAK_Count);

      if (Kind == DAK_Count)
        break;

      SyntaxParsingContext ModContext(SyntaxContext,
                                      SyntaxKind::DeclModifier);
      isError |= parseNewDeclAttribute(Attributes, /*AtLoc=*/{}, Kind);
      hasModifier = true;
      continue;
    }

    case tok::kw_static: {
      // 'static' is not handled as an attribute in AST.
      if (StaticLoc.isValid()) {
        diagnose(Tok, diag::decl_already_static,
                 StaticSpellingKind::KeywordStatic)
            .highlight(StaticLoc)
            .fixItRemove(Tok.getLoc());
      } else {
        StaticLoc = Tok.getLoc();
        StaticSpelling = StaticSpellingKind::KeywordStatic;
      }
      SyntaxParsingContext ModContext(SyntaxContext, SyntaxKind::DeclModifier);
      consumeToken(tok::kw_static);
      hasModifier = true;
      continue;
    }

    case tok::kw_class: {
      // If 'class' is a modifier on another decl kind, like var or func,
      // then treat it as a modifier.
      {
        BacktrackingScope Scope(*this);
        consumeToken(tok::kw_class);
        // When followed by an 'override' or CC token inside a class,
        // treat 'class' as a modifier; in the case of a following CC
        // token, we cannot be sure there is no intention to override
        // or witness something static.
        if (isStartOfDecl() || (isa<ClassDecl>(CurDeclContext) &&
                                (Tok.is(tok::code_complete) ||
                                 Tok.getRawText().equals("override")))) {
          /* We're OK */
        } else {
          // This 'class' is a real ClassDecl introducer.
          break;
        }
      }
      if (StaticLoc.isValid()) {
        diagnose(Tok, diag::decl_already_static,
                 StaticSpellingKind::KeywordClass)
            .highlight(StaticLoc)
            .fixItRemove(Tok.getLoc());
      } else {
        StaticLoc = Tok.getLoc();
        StaticSpelling = StaticSpellingKind::KeywordClass;
      }
      SyntaxParsingContext ModContext(SyntaxContext, SyntaxKind::DeclModifier);
      consumeToken(tok::kw_class);
      hasModifier = true;
      continue;
    }

    case tok::unknown:
      // Eat an invalid token in decl modifier context. Error tokens are
      // diagnosed by the lexer, so we don't need to emit another diagnostic.
      consumeToken(tok::unknown);
      hasModifier = true;
      continue;

    default:
      break;
    }

    // If we don't have any modifiers, don't bother to construct an empty list.
    if (!hasModifier)
      ListContext.setTransparent();

    // If we 'break' out of the switch, modifier list has ended.
    return isError;
  }
}

/// This is the internal implementation of \c parseTypeAttributeList,
/// which we expect to be inlined to handle the common case of an absent
/// attribute list.
///
/// \verbatim
///   attribute-list:
///     /*empty*/
///     attribute-list-clause attribute-list
///     'inout' attribute-list-clause attribute-list
///     '__shared' attribute-list-clause attribute-list
///     '__owned' attribute-list-clause attribute-list
///     'some' attribute-list-clause attribute-list
///   attribute-list-clause:
///     '@' attribute
///     '@' attribute attribute-list-clause
/// \endverbatim
bool Parser::parseTypeAttributeListPresent(ParamDecl::Specifier &Specifier,
                                           SourceLoc &SpecifierLoc,
                                           TypeAttributes &Attributes) {
  Specifier = ParamDecl::Specifier::Default;
  while (Tok.is(tok::kw_inout) ||
         (Tok.is(tok::identifier) &&
          (Tok.getRawText().equals("__shared") ||
           Tok.getRawText().equals("__owned")))) {
    if (SpecifierLoc.isValid()) {
      diagnose(Tok, diag::parameter_specifier_repeated)
        .fixItRemove(SpecifierLoc);
    } else {
      if (Tok.is(tok::kw_inout)) {
        Specifier = ParamDecl::Specifier::InOut;
      } else if (Tok.is(tok::identifier)) {
        if (Tok.getRawText().equals("__shared")) {
          Specifier = ParamDecl::Specifier::Shared;
        } else if (Tok.getRawText().equals("__owned")) {
          Specifier = ParamDecl::Specifier::Owned;
        }
      }
    }
    SpecifierLoc = consumeToken();
  }

  SyntaxParsingContext AttrListCtx(SyntaxContext, SyntaxKind::AttributeList);
  while (Tok.is(tok::at_sign)) {
    if (Attributes.AtLoc.isInvalid())
      Attributes.AtLoc = Tok.getLoc();
    SyntaxParsingContext AttrCtx(SyntaxContext, SyntaxKind::Attribute);
    consumeToken();
    if (parseTypeAttribute(Attributes))
      return true;
  }
  
  return false;
}

static bool isStartOfOperatorDecl(const Token &Tok, const Token &Tok2) {
  return Tok.isContextualKeyword("operator") &&
         (Tok2.isContextualKeyword("prefix") ||
          Tok2.isContextualKeyword("postfix") ||
          Tok2.isContextualKeyword("infix"));
}

/// Diagnose issues with fixity attributes, if any.
static void diagnoseOperatorFixityAttributes(Parser &P,
                                             DeclAttributes &Attrs,
                                             const Decl *D) {
  auto isFixityAttr = [](DeclAttribute *attr){
    DeclAttrKind kind = attr->getKind();
    return attr->isValid() && (kind == DAK_Prefix ||
                               kind == DAK_Infix ||
                               kind == DAK_Postfix);
  };
  
  SmallVector<DeclAttribute *, 3> fixityAttrs;
  std::copy_if(Attrs.begin(), Attrs.end(),
               std::back_inserter(fixityAttrs), isFixityAttr);
  std::reverse(fixityAttrs.begin(), fixityAttrs.end());
  
  for (auto it = fixityAttrs.begin(); it != fixityAttrs.end(); ++it) {
    if (it != fixityAttrs.begin()) {
      auto *attr = *it;
      P.diagnose(attr->getLocation(), diag::mutually_exclusive_attrs,
                 attr->getAttrName(), fixityAttrs.front()->getAttrName(),
                 attr->isDeclModifier())
      .fixItRemove(attr->getRange());
      attr->setInvalid();
    }
  }
  
  // Operator declarations must specify a fixity.
  if (auto *OD = dyn_cast<OperatorDecl>(D)) {
    if (fixityAttrs.empty()) {
      P.diagnose(OD->getOperatorLoc(), diag::operator_decl_no_fixity);
    }
  }
  // Infix operator is only allowed on operator declarations, not on func.
  else if (isa<FuncDecl>(D)) {
    if (auto *attr = Attrs.getAttribute<InfixAttr>()) {
      P.diagnose(attr->getLocation(), diag::invalid_infix_on_func)
        .fixItRemove(attr->getLocation());
      attr->setInvalid();
    }
  } else {
    llvm_unreachable("unexpected decl kind?");
  }
}

static unsigned skipUntilMatchingRBrace(Parser &P, bool &HasPoundDirective,
                                        SyntaxParsingContext *&SyntaxContext) {
  HasPoundDirective = false;
  bool isRootCtx = SyntaxContext->isRoot();
  SyntaxParsingContext BlockItemListContext(SyntaxContext,
                                            SyntaxKind::CodeBlockItemList);
  if (isRootCtx) {
    BlockItemListContext.setTransparent();
  }
  SyntaxParsingContext BlockItemContext(SyntaxContext,
                                        SyntaxKind::CodeBlockItem);
  SyntaxParsingContext BodyContext(SyntaxContext, SyntaxKind::TokenList);
  unsigned OpenBraces = 1;
  while (OpenBraces != 0 && P.Tok.isNot(tok::eof)) {
    HasPoundDirective |= P.Tok.isAny(tok::pound_sourceLocation, tok::pound_line,
      tok::pound_if, tok::pound_else, tok::pound_endif, tok::pound_elseif);
    if (P.consumeIf(tok::l_brace)) {
      OpenBraces++;
      continue;
    }
    if (OpenBraces == 1 && P.Tok.is(tok::r_brace))
      break;
    if (P.consumeIf(tok::r_brace)) {
      OpenBraces--;
      continue;
    }
    P.consumeToken();
  }
  return OpenBraces;
}

bool swift::isKeywordPossibleDeclStart(const Token &Tok) {
  switch (Tok.getKind()) {
  case tok::at_sign:
  case tok::kw_associatedtype:
  case tok::kw_case:
  case tok::kw_class:
  case tok::kw_deinit:
  case tok::kw_enum:
  case tok::kw_extension:
  case tok::kw_fileprivate:
  case tok::kw_func:
  case tok::kw_import:
  case tok::kw_init:
  case tok::kw_internal:
  case tok::kw_let:
  case tok::kw_operator:
  case tok::kw_precedencegroup:
  case tok::kw_private:
  case tok::kw_protocol:
  case tok::kw_public:
  case tok::kw_static:
  case tok::kw_struct:
  case tok::kw_subscript:
  case tok::kw_typealias:
  case tok::kw_var:
  case tok::pound_if:
  case tok::pound_warning:
  case tok::pound_error:
  case tok::identifier:
  case tok::pound_sourceLocation:
    return true;
  case tok::pound_line:
    // #line at the start of the line is a directive, but it's deprecated.
    // #line within a line is an expression.
    return Tok.isAtStartOfLine();

  case tok::kw_try:
    // 'try' is not a valid way to start a decl, but we special-case 'try let'
    // and 'try var' for better recovery.
    return true;
  default:
    return false;
  }
}

/// Given a current token of 'unowned', check to see if it is followed by a
/// "(safe)" or "(unsafe)" specifier.
static bool isParenthesizedUnowned(Parser &P) {
  assert(P.Tok.getText() == "unowned" && P.peekToken().is(tok::l_paren) &&
         "Invariant violated");
  
  // Look ahead to parse the parenthesized expression.
  Parser::BacktrackingScope Backtrack(P);
  P.consumeToken(tok::identifier);
  P.consumeToken(tok::l_paren);
  return P.Tok.is(tok::identifier) && P.peekToken().is(tok::r_paren) &&
          (P.Tok.getText() == "safe" || P.Tok.getText() == "unsafe");
}

  
bool Parser::isStartOfDecl() {
  // If this is obviously not the start of a decl, then we're done.
  if (!isKeywordPossibleDeclStart(Tok)) return false;

  // When 'init' appears inside another 'init', it's likely the user wants to
  // invoke an initializer but forgets to prefix it with 'self.' or 'super.'
  // Otherwise, expect 'init' to be the start of a declaration (and complain
  // when the expectation is not fulfilled).
  if (Tok.is(tok::kw_init)) {
    return !isa<ConstructorDecl>(CurDeclContext);
  }

  // Similarly, when 'case' appears inside a function, it's probably a switch
  // case, not an enum case declaration.
  if (Tok.is(tok::kw_case)) {
    return !isa<AbstractFunctionDecl>(CurDeclContext);
  }

  // The protocol keyword needs more checking to reject "protocol<Int>".
  if (Tok.is(tok::kw_protocol)) {
    const Token &Tok2 = peekToken();
    return !Tok2.isAnyOperator() || !Tok2.getText().equals("<");
  }

  // The 'try' case is only for simple local recovery, so we only bother to
  // check 'let' and 'var' right now.
  if (Tok.is(tok::kw_try))
    return peekToken().isAny(tok::kw_let, tok::kw_var);
  
  // Look through attribute list, because it may be an *type* attribute list.
  if (Tok.is(tok::at_sign)) {
    BacktrackingScope backtrack(*this);
    while (consumeIf(tok::at_sign)) {
      // If not identifier or code complete token, consider '@' as an incomplete
      // attribute.
      if (Tok.isNot(tok::identifier, tok::code_complete))
        continue;
      consumeToken();
      // Eat paren after attribute name; e.g. @foo(x)
      if (consumeIf(tok::l_paren)) {
        while (Tok.isNot(tok::r_brace, tok::eof, tok::pound_endif)) {
          if (consumeIf(tok::r_paren)) break;
          skipSingle();
        }
      }
    }
    // If this attribute is the last element in the block,
    // consider it is a start of incomplete decl.
    if (Tok.isAny(tok::r_brace, tok::eof, tok::pound_endif))
      return true;

    return isStartOfDecl();
  }

  // Otherwise, the only hard case left is the identifier case.
  if (Tok.isNot(tok::identifier)) return true;

  // If this is an operator declaration, handle it.
  const Token &Tok2 = peekToken();
  if (isStartOfOperatorDecl(Tok, Tok2))
    return true;
    
  // If this can't possibly be a contextual keyword, then this identifier is
  // not interesting.  Bail out.
  if (!Tok.isContextualDeclKeyword())
    return false;
      
  // If it might be, we do some more digging.

  // If this is 'unowned', check to see if it is valid.
  if (Tok.getText() == "unowned" && Tok2.is(tok::l_paren) &&
      isParenthesizedUnowned(*this)) {
    Parser::BacktrackingScope Backtrack(*this);
    consumeToken(tok::identifier);
    consumeToken(tok::l_paren);
    consumeToken(tok::identifier);
    consumeToken(tok::r_paren);
    return isStartOfDecl();
  }

  // If the next token is obviously not the start of a decl, bail early.
  if (!isKeywordPossibleDeclStart(Tok2))
    return false;
  
  // Otherwise, do a recursive parse.
  Parser::BacktrackingScope Backtrack(*this);
  consumeToken(tok::identifier);
  return isStartOfDecl();
}

void Parser::consumeDecl(ParserPosition BeginParserPosition,
                         ParseDeclOptions Flags,
                         bool IsTopLevel) {
  SourceLoc CurrentLoc = Tok.getLoc();

  SourceLoc EndLoc = PreviousLoc;
  backtrackToPosition(BeginParserPosition);
  SourceLoc BeginLoc = Tok.getLoc();

  State->delayDecl(PersistentParserState::DelayedDeclKind::Decl, Flags.toRaw(),
                   CurDeclContext, {BeginLoc, EndLoc},
                   BeginParserPosition.PreviousLoc);

  while (SourceMgr.isBeforeInBuffer(Tok.getLoc(), CurrentLoc))
    consumeToken();

  if (IsTopLevel) {
    // Skip the rest of the file to prevent the parser from constructing the
    // AST for it.  Forward references are not allowed at the top level.
    while (Tok.isNot(tok::eof))
      consumeToken();
  }
}

void Parser::setLocalDiscriminator(ValueDecl *D) {
  // If we're not in a local context, this is unnecessary.
  if (!CurLocalContext || !D->getDeclContext()->isLocalContext())
    return;

  if (auto TD = dyn_cast<TypeDecl>(D))
    if (!getScopeInfo().isInactiveConfigBlock())
      SF.LocalTypeDecls.insert(TD);

  Identifier name = D->getBaseName().getIdentifier();
  unsigned discriminator = CurLocalContext->claimNextNamedDiscriminator(name);
  D->setLocalDiscriminator(discriminator);
}

void Parser::setLocalDiscriminatorToParamList(ParameterList *PL) {
  for (auto P : *PL) {
    if (!P->hasName() || P->isImplicit())
      continue;
    setLocalDiscriminator(P);
  }
}

void Parser::delayParseFromBeginningToHere(ParserPosition BeginParserPosition,
                                           ParseDeclOptions Flags) {
  auto CurLoc = Tok.getLoc();
  backtrackToPosition(BeginParserPosition);
  SourceLoc BeginLoc = Tok.getLoc();
  SourceLoc EndLoc = CurLoc;
  State->delayDecl(PersistentParserState::DelayedDeclKind::Decl,
                   Flags.toRaw(),
                   CurDeclContext, {BeginLoc, EndLoc},
                   BeginParserPosition.PreviousLoc);

  while (Tok.isNot(tok::eof))
    consumeToken();
}

/// Parse a single syntactic declaration and return a list of decl
/// ASTs.  This can return multiple results for var decls that bind to multiple
/// values, structs that define a struct decl and a constructor, etc.
///
/// \verbatim
///   decl:
///     decl-typealias
///     decl-extension
///     decl-let
///     decl-var
///     decl-class
///     decl-func
///     decl-enum
///     decl-struct
///     decl-import
///     decl-operator
/// \endverbatim
ParserResult<Decl>
Parser::parseDecl(ParseDeclOptions Flags,
                  bool IsAtStartOfLineOrPreviousHadSemi,
                  llvm::function_ref<void(Decl*)> Handler) {
  ParserPosition BeginParserPosition;
  if (isCodeCompletionFirstPass())
    BeginParserPosition = getParserPosition();

  if (Tok.is(tok::pound_if)) {
    auto IfConfigResult = parseIfConfig(
      [&](SmallVectorImpl<ASTNode> &Decls, bool IsActive) {
        Optional<Scope> scope;
        if (!IsActive)
          scope.emplace(this, getScopeInfo().getCurrentScope()->getKind(),
                        /*inactiveConfigBlock=*/true);

        ParserStatus Status;
        bool PreviousHadSemi = true;
        SyntaxParsingContext DeclListCtx(SyntaxContext,
                                         SyntaxKind::MemberDeclList);
        while (Tok.isNot(tok::pound_else, tok::pound_endif, tok::pound_elseif,
                         tok::eof)) {
          if (Tok.is(tok::r_brace)) {
            diagnose(Tok.getLoc(),
                      diag::unexpected_rbrace_in_conditional_compilation_block);
            // If we see '}', following declarations don't look like belong to
            // the current decl context; skip them.
            skipUntilConditionalBlockClose();
            break;
          }
          Status |= parseDeclItem(PreviousHadSemi, Flags,
                                  [&](Decl *D) {Decls.emplace_back(D);});
        }
      });
    if (IfConfigResult.hasCodeCompletion() && isCodeCompletionFirstPass()) {
      consumeDecl(BeginParserPosition, Flags,
                  CurDeclContext->isModuleScopeContext());
      return makeParserError();
    }

    if (auto ICD = IfConfigResult.getPtrOrNull()) {
      // The IfConfigDecl is ahead of its members in source order.
      Handler(ICD);
      // Copy the active members into the entries list.
      for (auto activeMember : ICD->getActiveClauseElements()) {
        auto *D = activeMember.get<Decl*>();
        if (isa<IfConfigDecl>(D))
          // Don't hoist nested '#if'.
          continue;
        Handler(D);
      }
    }
    return IfConfigResult;
  }
  if (Tok.isAny(tok::pound_warning, tok::pound_error)) {
    auto Result = parseDeclPoundDiagnostic();
    if (Result.isNonNull())
      Handler(Result.get());
    return Result;
  }

  SyntaxParsingContext DeclParsingContext(SyntaxContext,
                                          SyntaxContextKind::Decl);

  // Note that we're parsing a declaration.
  StructureMarkerRAII ParsingDecl(*this, Tok.getLoc(),
                                  StructureMarkerKind::Declaration);

  // Parse attributes.
  DeclAttributes Attributes;
  if (Tok.hasComment())
    Attributes.add(new (Context) RawDocCommentAttr(Tok.getCommentRange()));
  ParserStatus AttrStatus = parseDeclAttributeList(Attributes);

  // Parse modifiers.
  // Keep track of where and whether we see a contextual keyword on the decl.
  SourceLoc StaticLoc;
  StaticSpellingKind StaticSpelling = StaticSpellingKind::None;
  parseDeclModifierList(Attributes, StaticLoc, StaticSpelling);

  // We emit diagnostics for 'try let ...' in parseDeclVar().
  SourceLoc tryLoc;
  if (Tok.is(tok::kw_try) && peekToken().isAny(tok::kw_let, tok::kw_var))
    tryLoc = consumeToken(tok::kw_try);

  ParserResult<Decl> DeclResult;

  // Save the original token, in case code-completion needs it.
  auto OrigTok = Tok;
  bool MayNeedOverrideCompletion = false;

  auto parseLetOrVar = [&](bool HasLetOrVarKeyword) {
    // Collect all modifiers into a modifier list.
    DeclParsingContext.setCreateSyntax(SyntaxKind::VariableDecl);
    llvm::SmallVector<Decl *, 4> Entries;
    DeclResult = parseDeclVar(Flags, Attributes, Entries, StaticLoc,
                              StaticSpelling, tryLoc, HasLetOrVarKeyword);
    StaticLoc = SourceLoc(); // we handled static if present.
    MayNeedOverrideCompletion = true;
    if (DeclResult.hasCodeCompletion() && isCodeCompletionFirstPass())
      return;
    std::for_each(Entries.begin(), Entries.end(), Handler);
    if (auto *D = DeclResult.getPtrOrNull())
      markWasHandled(D);
  };

  auto parseFunc = [&](bool HasFuncKeyword) {
    // Collect all modifiers into a modifier list.
    DeclParsingContext.setCreateSyntax(SyntaxKind::FunctionDecl);
    DeclResult = parseDeclFunc(StaticLoc, StaticSpelling, Flags, Attributes,
                               HasFuncKeyword);
    StaticLoc = SourceLoc(); // we handled static if present.
    MayNeedOverrideCompletion = true;
  };

  switch (Tok.getKind()) {
  case tok::kw_import:
    DeclParsingContext.setCreateSyntax(SyntaxKind::ImportDecl);
    DeclResult = parseDeclImport(Flags, Attributes);
    break;
  case tok::kw_extension:
    DeclParsingContext.setCreateSyntax(SyntaxKind::ExtensionDecl);
    DeclResult = parseDeclExtension(Flags, Attributes);
    break;
  case tok::kw_let:
  case tok::kw_var: {
    parseLetOrVar(/*HasLetOrVarKeyword=*/true);
    break;
  }
  case tok::kw_typealias:
    DeclParsingContext.setCreateSyntax(SyntaxKind::TypealiasDecl);
    DeclResult = parseDeclTypeAlias(Flags, Attributes);
    MayNeedOverrideCompletion = true;
    break;
  case tok::kw_associatedtype:
    DeclParsingContext.setCreateSyntax(SyntaxKind::AssociatedtypeDecl);
    DeclResult = parseDeclAssociatedType(Flags, Attributes);
    break;
  case tok::kw_enum:
    DeclParsingContext.setCreateSyntax(SyntaxKind::EnumDecl);
    DeclResult = parseDeclEnum(Flags, Attributes);
    break;
  case tok::kw_case: {
    llvm::SmallVector<Decl *, 4> Entries;
    DeclParsingContext.setCreateSyntax(SyntaxKind::EnumCaseDecl);
    DeclResult = parseDeclEnumCase(Flags, Attributes, Entries);
    if (DeclResult.hasCodeCompletion() && isCodeCompletionFirstPass())
      break;
    std::for_each(Entries.begin(), Entries.end(), Handler);
    if (auto *D = DeclResult.getPtrOrNull())
      markWasHandled(D);
    break;
  }
  case tok::kw_class:
    DeclParsingContext.setCreateSyntax(SyntaxKind::ClassDecl);
    DeclResult = parseDeclClass(Flags, Attributes);
    break;
  case tok::kw_struct:
    DeclParsingContext.setCreateSyntax(SyntaxKind::StructDecl);
    DeclResult = parseDeclStruct(Flags, Attributes);
    break;
  case tok::kw_init:
    DeclParsingContext.setCreateSyntax(SyntaxKind::InitializerDecl);
    DeclResult = parseDeclInit(Flags, Attributes);
    break;
  case tok::kw_deinit:
    DeclParsingContext.setCreateSyntax(SyntaxKind::DeinitializerDecl);
    DeclResult = parseDeclDeinit(Flags, Attributes);
    break;
  case tok::kw_operator:
    DeclParsingContext.setCreateSyntax(SyntaxKind::OperatorDecl);
    DeclResult = parseDeclOperator(Flags, Attributes);
    break;
  case tok::kw_precedencegroup:
    DeclParsingContext.setCreateSyntax(SyntaxKind::PrecedenceGroupDecl);
    DeclResult = parseDeclPrecedenceGroup(Flags, Attributes);
    break;
  case tok::kw_protocol:
    DeclParsingContext.setCreateSyntax(SyntaxKind::ProtocolDecl);
    DeclResult = parseDeclProtocol(Flags, Attributes);
    break;
  case tok::kw_func:
    parseFunc(/*HasFuncKeyword=*/true);
    break;
  case tok::kw_subscript: {
    DeclParsingContext.setCreateSyntax(SyntaxKind::SubscriptDecl);
    llvm::SmallVector<Decl *, 4> Entries;
    DeclResult = parseDeclSubscript(StaticLoc, StaticSpelling, Flags,
                                    Attributes, Entries);
    StaticLoc = SourceLoc(); // we handled static if present.
    if (DeclResult.hasCodeCompletion() && isCodeCompletionFirstPass())
      break;
    std::for_each(Entries.begin(), Entries.end(), Handler);
    MayNeedOverrideCompletion = true;
    if (auto *D = DeclResult.getPtrOrNull())
      markWasHandled(D);
    break;
  }

  case tok::code_complete:
    MayNeedOverrideCompletion = true;
    DeclResult = makeParserError();
    // Handled below.
    break;
  case tok::pound:
    if (Tok.isAtStartOfLine() &&
        peekToken().is(tok::code_complete) &&
        Tok.getLoc().getAdvancedLoc(1) == peekToken().getLoc()) {
      consumeToken();
      if (CodeCompletion)
        CodeCompletion->completeAfterPoundDirective();
      consumeToken(tok::code_complete);
      DeclResult = makeParserCodeCompletionResult<Decl>();
      break;
    }
    LLVM_FALLTHROUGH;

  case tok::pound_if:
  case tok::pound_sourceLocation:
  case tok::pound_line:
  case tok::pound_warning:
  case tok::pound_error:
    // We see some attributes right before these pounds.
    // TODO: Emit dedicated errors for them.
    LLVM_FALLTHROUGH;

  // Obvious nonsense.
  default:

    if (Flags.contains(PD_HasContainerType) &&
        IsAtStartOfLineOrPreviousHadSemi) {

      // Emit diagnostics if we meet an identifier/operator where a declaration
      // is expected, perhaps the user forgot the 'func' or 'var' keyword.
      //
      // Must not confuse it with trailing closure syntax, so we only
      // recover in contexts where there can be no statements.

      const bool IsProbablyVarDecl =
          Tok.isIdentifierOrUnderscore() &&
          peekToken().isAny(tok::colon, tok::equal, tok::comma);

      const bool IsProbablyTupleDecl =
          Tok.is(tok::l_paren) && peekToken().isIdentifierOrUnderscore();

      if (IsProbablyVarDecl || IsProbablyTupleDecl) {

        DescriptiveDeclKind DescriptiveKind;

        switch (StaticSpelling) {
        case StaticSpellingKind::None:
          DescriptiveKind = DescriptiveDeclKind::Property;
          break;
        case StaticSpellingKind::KeywordStatic:
          DescriptiveKind = DescriptiveDeclKind::StaticProperty;
          break;
        case StaticSpellingKind::KeywordClass:
          llvm_unreachable("kw_class is only parsed as a modifier if it's "
                           "followed by a keyword");
        }

        diagnose(Tok.getLoc(), diag::expected_keyword_in_decl, "var",
                 DescriptiveKind)
            .fixItInsert(Tok.getLoc(), "var ");
        parseLetOrVar(/*HasLetOrVarKeyword=*/false);
        break;
      }

      const bool IsProbablyFuncDecl =
          Tok.isIdentifierOrUnderscore() || Tok.isAnyOperator();

      if (IsProbablyFuncDecl) {

        DescriptiveDeclKind DescriptiveKind;

        if (Tok.isAnyOperator()) {
          DescriptiveKind = DescriptiveDeclKind::OperatorFunction;
        } else {
          switch (StaticSpelling) {
          case StaticSpellingKind::None:
            DescriptiveKind = DescriptiveDeclKind::Method;
            break;
          case StaticSpellingKind::KeywordStatic:
            DescriptiveKind = DescriptiveDeclKind::StaticMethod;
            break;
          case StaticSpellingKind::KeywordClass:
            llvm_unreachable("kw_class is only parsed as a modifier if it's "
                             "followed by a keyword");
          }
        }

        diagnose(Tok.getLoc(), diag::expected_keyword_in_decl, "func",
                 DescriptiveKind)
            .fixItInsert(Tok.getLoc(), "func ");
        parseFunc(/*HasFuncKeyword=*/false);
        break;
      }
    }

    diagnose(Tok, diag::expected_decl);

    if (CurDeclContext) {
      if (auto nominal = dyn_cast<NominalTypeDecl>(CurDeclContext)) {
        diagnose(nominal->getLoc(), diag::note_in_decl_extension, false,
                 nominal->getName());
      } else if (auto extension = dyn_cast<ExtensionDecl>(CurDeclContext)) {
        if (auto repr = extension->getExtendedTypeLoc().getTypeRepr()) {
          if (auto idRepr = dyn_cast<IdentTypeRepr>(repr)) {
            diagnose(extension->getLoc(), diag::note_in_decl_extension, true,
                     idRepr->getComponentRange().front()->getIdentifier());
          }
        }
      }
    }
  }

  if (DeclResult.isParseError() && Tok.is(tok::code_complete)) {
    if (MayNeedOverrideCompletion && CodeCompletion) {
      // If we need to complete an override, collect the keywords already
      // specified so that we do not duplicate them in code completion
      // strings.
      SmallVector<StringRef, 3> Keywords;
      SourceLoc introducerLoc;
      switch (OrigTok.getKind()) {
      case tok::kw_func:
      case tok::kw_subscript:
      case tok::kw_var:
      case tok::kw_let:
      case tok::kw_typealias:
        Keywords.push_back(OrigTok.getText());
        introducerLoc = OrigTok.getLoc();
        break;
      default:
        // Other tokens are already accounted for.
        break;
      }
      if (StaticSpelling == StaticSpellingKind::KeywordStatic) {
        Keywords.push_back(getTokenText(tok::kw_static));
      } else if (StaticSpelling == StaticSpellingKind::KeywordClass) {
        Keywords.push_back(getTokenText(tok::kw_class));
      }
      for (auto attr : Attributes) {
        Keywords.push_back(attr->getAttrName());
      }
      CodeCompletion->completeNominalMemberBeginning(Keywords,
                                                     introducerLoc);
    }

    DeclResult = makeParserCodeCompletionStatus();
    consumeToken(tok::code_complete);
  }

  if (AttrStatus.hasCodeCompletion()) {
    if (CodeCompletion) {
      Optional<DeclKind> DK;
      if (DeclResult.isNonNull())
        DK = DeclResult.get()->getKind();
      CodeCompletion->setAttrTargetDeclKind(DK);
    } else {
      delayParseFromBeginningToHere(BeginParserPosition, Flags);
      return makeParserError();
    }
  }

  if (DeclResult.hasCodeCompletion() && isCodeCompletionFirstPass() &&
      !CurDeclContext->isModuleScopeContext() &&
      !isa<TopLevelCodeDecl>(CurDeclContext)) {
    // Only consume non-toplevel decls.
    consumeDecl(BeginParserPosition, Flags, /*IsTopLevel=*/false);

    return makeParserError();
  }

  if (auto SF = CurDeclContext->getParentSourceFile()) {
    if (!getScopeInfo().isInactiveConfigBlock()) {
      for (auto Attr : Attributes) {
        if (isa<ObjCAttr>(Attr) ||
            /* Pre Swift 5 dymamic implied @objc */
            (!Context.LangOpts.isSwiftVersionAtLeast(5) &&
             isa<DynamicAttr>(Attr)))
          SF->AttrsRequiringFoundation.insert(Attr);
      }
    }
  }

  if (DeclResult.isNonNull()) {
    Decl *D = DeclResult.get();
    if (!declWasHandledAlready(D))
      Handler(DeclResult.get());
  }

  if (!DeclResult.isParseError()) {
    // If we parsed 'class' or 'static', but didn't handle it above, complain
    // about it.
    if (StaticLoc.isValid())
      diagnose(DeclResult.get()->getLoc(), diag::decl_not_static,
               StaticSpelling)
          .fixItRemove(SourceRange(StaticLoc));
  }

  return DeclResult;
}

void Parser::parseDeclListDelayed(IterableDeclContext *IDC) {
  auto DelayedState = State->takeDelayedDeclListState(IDC);
  assert(DelayedState.get() && "should have delayed state");

  auto BeginParserPosition = getParserPosition(DelayedState->BodyPos);
  auto EndLexerState = L->getStateForEndOfTokenLoc(DelayedState->BodyEnd);

  // ParserPositionRAII needs a primed parser to restore to.
  if (Tok.is(tok::NUM_TOKENS))
    consumeTokenWithoutFeedingReceiver();

  // Ensure that we restore the parser state at exit.
  ParserPositionRAII PPR(*this);

  // Create a lexer that cannot go past the end state.
  Lexer LocalLex(*L, BeginParserPosition.LS, EndLexerState);

  // Temporarily swap out the parser's current lexer with our new one.
  llvm::SaveAndRestore<Lexer *> T(L, &LocalLex);

  // Rewind to the beginning of the decl.
  restoreParserPosition(BeginParserPosition);

  // Re-enter the lexical scope.
  Scope S(this, DelayedState->takeScope());
  ContextChange CC(*this, DelayedState->ParentContext);
  Decl *D = const_cast<Decl*>(IDC->getDecl());
  SourceLoc LBLoc = consumeToken(tok::l_brace);
  SourceLoc RBLoc;
  Diag<> Id;
  switch (D->getKind()) {
  case DeclKind::Extension: Id = diag::expected_rbrace_extension; break;
  case DeclKind::Enum: Id = diag::expected_rbrace_enum; break;
  case DeclKind::Protocol: Id = diag::expected_rbrace_protocol; break;
  case DeclKind::Class: Id = diag::expected_rbrace_class; break;
  case DeclKind::Struct: Id = diag::expected_rbrace_struct; break;
  default:
    llvm_unreachable("Bad iterable decl context kinds.");
  }
  if (auto *ext = dyn_cast<ExtensionDecl>(D)) {
    parseDeclList(ext->getBraces().Start, RBLoc, Id,
                  ParseDeclOptions(DelayedState->Flags),
                  ext);
    ext->setBraces({LBLoc, RBLoc});
  } else {
    auto *ntd = cast<NominalTypeDecl>(D);
    parseDeclList(ntd->getBraces().Start, RBLoc, Id,
                  ParseDeclOptions(DelayedState->Flags),
                  ntd);
    ntd->setBraces({LBLoc, RBLoc});
  }
}

void Parser::parseDeclDelayed() {
  auto DelayedState = State->takeDelayedDeclState();
  assert(DelayedState.get() && "should have delayed state");

  auto BeginParserPosition = getParserPosition(DelayedState->BodyPos);
  auto EndLexerState = L->getStateForEndOfTokenLoc(DelayedState->BodyEnd);

  // ParserPositionRAII needs a primed parser to restore to.
  if (Tok.is(tok::NUM_TOKENS))
    consumeTokenWithoutFeedingReceiver();

  // Ensure that we restore the parser state at exit.
  ParserPositionRAII PPR(*this);

  // Create a lexer that cannot go past the end state.
  Lexer LocalLex(*L, BeginParserPosition.LS, EndLexerState);

  // Temporarily swap out the parser's current lexer with our new one.
  llvm::SaveAndRestore<Lexer *> T(L, &LocalLex);

  // Rewind to the beginning of the decl.
  restoreParserPosition(BeginParserPosition);

  // Re-enter the lexical scope.
  Scope S(this, DelayedState->takeScope());
  ContextChange CC(*this, DelayedState->ParentContext);

  parseDecl(ParseDeclOptions(DelayedState->Flags),
            /*IsAtStartOfLineOrPreviousHadSemi=*/true,
            [&](Decl *D) {
    if (auto *parent = DelayedState->ParentContext) {
      if (auto *NTD = dyn_cast<NominalTypeDecl>(parent)) {
        NTD->addMember(D);
      } else if (auto *ED = dyn_cast<ExtensionDecl>(parent)) {
        ED->addMember(D);
      } else if (auto *SF = dyn_cast<SourceFile>(parent)) {
        SF->Decls.push_back(D);
      }
    }
  });
}

/// Parse an 'import' declaration, doing no token skipping on error.
///
/// \verbatim
///   decl-import:
///     'import' attribute-list import-kind? import-path
///   import-kind:
///     'typealias'
///     'struct'
///     'class'
///     'enum'
///     'protocol'
///     'var'
///     'func'
///   import-path:
///     any-identifier ('.' any-identifier)*
/// \endverbatim
ParserResult<ImportDecl> Parser::parseDeclImport(ParseDeclOptions Flags,
                                                 DeclAttributes &Attributes) {
  SourceLoc ImportLoc = consumeToken(tok::kw_import);
  DebuggerContextChange DCC (*this);

  if (!CodeCompletion && !DCC.movedToTopLevel() && !(Flags & PD_AllowTopLevel)) {
    diagnose(ImportLoc, diag::decl_inner_scope);
    return nullptr;
  }

  ImportKind Kind = ImportKind::Module;
  SourceLoc KindLoc;
  if (Tok.isKeyword()) {
    switch (Tok.getKind()) {
    case tok::kw_typealias:
      Kind = ImportKind::Type;
      break;
    case tok::kw_struct:
      Kind = ImportKind::Struct;
      break;
    case tok::kw_class:
      Kind = ImportKind::Class;
      break;
    case tok::kw_enum:
      Kind = ImportKind::Enum;
      break;
    case tok::kw_protocol:
      Kind = ImportKind::Protocol;
      break;
    case tok::kw_var:
    case tok::kw_let:
      Kind = ImportKind::Var;
      break;
    case tok::kw_func:
      Kind = ImportKind::Func;
      break;
    default:
      diagnose(Tok, diag::expected_identifier_in_decl, "import");
      diagnose(Tok, diag::keyword_cant_be_identifier, Tok.getText());
      diagnose(Tok, diag::backticks_to_escape);
      return nullptr;
    }
    KindLoc = consumeToken();
  }

  std::vector<std::pair<Identifier, SourceLoc>> ImportPath;
  bool HasNext;
  do {
    SyntaxParsingContext AccessCompCtx(SyntaxContext,
                                       SyntaxKind::AccessPathComponent);
    if (Tok.is(tok::code_complete)) {
      consumeToken();
      if (CodeCompletion) {
        CodeCompletion->completeImportDecl(ImportPath);
      }
      return makeParserCodeCompletionStatus();
    }
    ImportPath.push_back(std::make_pair(Identifier(), Tok.getLoc()));
    if (parseAnyIdentifier(ImportPath.back().first,
                           diag::expected_identifier_in_decl, "import"))
      return nullptr;
    HasNext = consumeIf(tok::period);
  } while (HasNext);

  // Collect all access path components to an access path.
  SyntaxContext->collectNodesInPlace(SyntaxKind::AccessPath);

  if (Tok.is(tok::code_complete)) {
    // We omit the code completion token if it immediately follows the module
    // identifiers.
    auto BufferId = SourceMgr.getCodeCompletionBufferID();
    auto IdEndOffset = SourceMgr.getLocOffsetInBuffer(ImportPath.back().second,
      BufferId) + ImportPath.back().first.str().size();
    auto CCTokenOffset = SourceMgr.getLocOffsetInBuffer(SourceMgr.
      getCodeCompletionLoc(), BufferId);
    if (IdEndOffset == CCTokenOffset) {
      consumeToken();
    }
  }

  if (Kind != ImportKind::Module && ImportPath.size() == 1) {
    diagnose(ImportPath.front().second, diag::decl_expected_module_name);
    return nullptr;
  }

  auto *ID = ImportDecl::create(Context, CurDeclContext, ImportLoc, Kind,
                                KindLoc, ImportPath);
  ID->getAttrs() = Attributes;
  return DCC.fixupParserResult(ID);
}

/// Parse an inheritance clause.
///
/// \verbatim
///   inheritance:
///      ':' inherited (',' inherited)*
///
///   inherited:
///     'class'
///     type-identifier
/// \endverbatim
ParserStatus Parser::parseInheritance(SmallVectorImpl<TypeLoc> &Inherited,
                                      bool allowClassRequirement,
                                      bool allowAnyObject) {
  SyntaxParsingContext InheritanceContext(SyntaxContext,
                                          SyntaxKind::TypeInheritanceClause);

  Scope S(this, ScopeKind::InheritanceClause);
  consumeToken(tok::colon);

  SyntaxParsingContext TypeListContext(SyntaxContext,
                                       SyntaxKind::InheritedTypeList);
  SourceLoc classRequirementLoc;

  ParserStatus Status;
  SourceLoc prevComma;
  bool HasNextType;
  do {
    SyntaxParsingContext TypeContext(SyntaxContext, SyntaxKind::InheritedType);
    SWIFT_DEFER {
      // Check for a ',', which indicates that there are more protocols coming.
      HasNextType = consumeIf(tok::comma, prevComma);
    };
    // Parse the 'class' keyword for a class requirement.
    if (Tok.is(tok::kw_class)) {
      SyntaxParsingContext ClassTypeContext(SyntaxContext,
                                            SyntaxKind::ClassRestrictionType);
      // If we aren't allowed to have a class requirement here, complain.
      auto classLoc = consumeToken();
      if (!allowClassRequirement) {
        diagnose(classLoc, diag::unexpected_class_constraint);

        // Note that it makes no sense to suggest fixing
        // 'struct S : class' to 'struct S : AnyObject' for
        // example; in that case we just complain about
        // 'class' being invalid here.
        if (allowAnyObject) {
          diagnose(classLoc, diag::suggest_anyobject)
            .fixItReplace(classLoc, "AnyObject");
        }
        continue;
      }

      // If we already saw a class requirement, complain.
      if (classRequirementLoc.isValid()) {
        diagnose(classLoc, diag::redundant_class_requirement)
          .highlight(classRequirementLoc)
          .fixItRemove(SourceRange(prevComma, classLoc));
        continue;
      }

      // If the class requirement was not the first requirement, complain.
      if (!Inherited.empty()) {
        SourceLoc properLoc = Inherited[0].getSourceRange().Start;
        diagnose(classLoc, diag::late_class_requirement)
          .fixItInsert(properLoc, "class, ")
          .fixItRemove(SourceRange(prevComma, classLoc));
      }

      // Record the location of the 'class' keyword.
      classRequirementLoc = classLoc;

      // Add 'AnyObject' to the inherited list.
      Inherited.push_back(
        new (Context) SimpleIdentTypeRepr(classLoc,
                                          Context.getIdentifier("AnyObject")));
      continue;
    }

    auto ParsedTypeResult = parseType();
    Status |= ParsedTypeResult;

    // Record the type if its a single type.
    if (ParsedTypeResult.isNonNull())
      Inherited.push_back(ParsedTypeResult.get());
  } while (HasNextType);

  return Status;
}

enum class TokenProperty {
  None,
  StartsWithLess,
};

static ParserStatus
parseIdentifierDeclName(Parser &P, Identifier &Result, SourceLoc &Loc,
                        StringRef DeclKindName, tok ResyncT1, tok ResyncT2,
                        tok ResyncT3, tok ResyncT4,
                        TokenProperty ResyncP1) {
  if (P.Tok.is(tok::identifier)) {
    Loc = P.consumeIdentifier(&Result);

    // We parsed an identifier for the declaration. If we see another
    // identifier, it might've been a single identifier that got broken by a
    // space or newline accidentally.
    if (P.Tok.isIdentifierOrUnderscore() && !P.Tok.isContextualDeclKeyword())
      P.diagnoseConsecutiveIDs(Result.str(), Loc, DeclKindName);

    // Return success anyway
    return makeParserSuccess();
  }

  P.checkForInputIncomplete();

  if (P.Tok.is(tok::integer_literal) || P.Tok.is(tok::floating_literal) ||
      (P.Tok.is(tok::unknown) && isdigit(P.Tok.getText()[0]))) {
    // Using numbers for identifiers is a common error for beginners, so it's
    // worth handling this in a special way.
    P.diagnose(P.Tok, diag::number_cant_start_decl_name, DeclKindName);

    // Pretend this works as an identifier, which shouldn't be observable since
    // actual uses of it will hit random other errors, e.g. `1()` won't be
    // callable.
    Result = P.Context.getIdentifier(P.Tok.getText());
    Loc = P.Tok.getLoc();
    P.consumeToken();

    // We recovered, so this is a success.
    return makeParserSuccess();
  }

  if (P.Tok.isKeyword()) {
    P.diagnose(P.Tok, diag::keyword_cant_be_identifier, P.Tok.getText());
    P.diagnose(P.Tok, diag::backticks_to_escape)
      .fixItReplace(P.Tok.getLoc(), "`" + P.Tok.getText().str() + "`");

    // Recover if the next token is one of the expected tokens.
    auto Next = P.peekToken();
    if (Next.isAny(ResyncT1, ResyncT2, ResyncT3, ResyncT4) ||
        (ResyncP1 != TokenProperty::None && P.startsWithLess(Next))) {
      llvm::SmallString<32> Name(P.Tok.getText());
      // Append an invalid character so that nothing can resolve to this name.
      Name += "#";
      Result = P.Context.getIdentifier(Name.str());
      Loc = P.Tok.getLoc();
      P.consumeToken();
      // Return success because we recovered.
      return makeParserSuccess();
    }
    return makeParserError();
  }

  P.diagnose(P.Tok, diag::expected_identifier_in_decl, DeclKindName);
  return makeParserError();
}

static ParserStatus
parseIdentifierDeclName(Parser &P, Identifier &Result, SourceLoc &L,
                        StringRef DeclKindName, tok ResyncT1, tok ResyncT2) {
  return parseIdentifierDeclName(P, Result, L, DeclKindName, ResyncT1, ResyncT2,
                                 tok::NUM_TOKENS, tok::NUM_TOKENS,
                                 TokenProperty::None);
}

static ParserStatus
parseIdentifierDeclName(Parser &P, Identifier &Result, SourceLoc &L,
                        StringRef DeclKindName, tok ResyncT1, tok ResyncT2,
                        tok ResyncT3, tok ResyncT4) {
  return parseIdentifierDeclName(P, Result, L, DeclKindName, ResyncT1, ResyncT2,
                                 ResyncT3, ResyncT4, TokenProperty::None);
}

static ParserStatus
parseIdentifierDeclName(Parser &P, Identifier &Result, SourceLoc &L,
                        StringRef DeclKindName, tok ResyncT1, tok ResyncT2,
                        TokenProperty ResyncP1) {
  return parseIdentifierDeclName(P, Result, L, DeclKindName, ResyncT1, ResyncT2,
                                 tok::NUM_TOKENS, tok::NUM_TOKENS, ResyncP1);
}

static ParserStatus
parseIdentifierDeclName(Parser &P, Identifier &Result, SourceLoc &L,
                        StringRef DeclKindName, tok ResyncT1, tok ResyncT2,
                        tok ResyncT3, TokenProperty ResyncP1) {
  return parseIdentifierDeclName(P, Result, L, DeclKindName, ResyncT1, ResyncT2,
                                 ResyncT3, tok::NUM_TOKENS, ResyncP1);
}

/// Add a fix-it to remove the space in consecutive identifiers.
/// Add a camel-cased option if it is different than the first option.
void Parser::diagnoseConsecutiveIDs(StringRef First, SourceLoc FirstLoc,
                                    StringRef DeclKindName) {
  assert(Tok.isAny(tok::identifier, tok::kw__));

  diagnose(Tok, diag::repeated_identifier, DeclKindName);
  auto Second = Tok.getText();
  auto SecondLoc = consumeToken();

  SourceRange FixRange(FirstLoc, SecondLoc);
  // Provide two fix-its: a direct concatenation of the two identifiers
  // and a camel-cased version.
  //
  auto DirectConcatenation = First.str() + Second.str();
  diagnose(SecondLoc, diag::join_identifiers)
    .fixItReplace(FixRange, DirectConcatenation);

  SmallString<8> CapitalizedScratch;
  auto Capitalized = camel_case::toSentencecase(Second,
                                                CapitalizedScratch);
  if (Capitalized != Second) {
    auto CamelCaseConcatenation = First.str() + Capitalized.str();
    diagnose(SecondLoc, diag::join_identifiers_camel_case)
      .fixItReplace(FixRange, CamelCaseConcatenation);
  }
}

/// Parse a Decl item in decl list.
ParserStatus Parser::parseDeclItem(bool &PreviousHadSemi,
                                   Parser::ParseDeclOptions Options,
                                   llvm::function_ref<void(Decl*)> handler) {
  if (Tok.is(tok::semi)) {
    // Consume ';' without preceding decl.
    diagnose(Tok, diag::unexpected_separator, ";")
      .fixItRemove(Tok.getLoc());
    consumeToken();
    // Return success because we already recovered.
    return makeParserSuccess();
  }

  // If the previous declaration didn't have a semicolon and this new
  // declaration doesn't start a line, complain.
  const bool IsAtStartOfLineOrPreviousHadSemi =
      PreviousHadSemi || Tok.isAtStartOfLine() || Tok.is(tok::unknown);
  if (!IsAtStartOfLineOrPreviousHadSemi) {
    auto endOfPrevious = getEndOfPreviousLoc();
    diagnose(endOfPrevious, diag::declaration_same_line_without_semi)
      .fixItInsert(endOfPrevious, ";");
  }

  if (Tok.isAny(tok::pound_sourceLocation, tok::pound_line)) {
    auto LineDirectiveStatus = parseLineDirective(Tok.is(tok::pound_line));
    if (LineDirectiveStatus.isError())
      skipUntilDeclRBrace(tok::semi, tok::pound_endif);
    return LineDirectiveStatus;
  }

  ParserResult<Decl> Result;
  SyntaxParsingContext DeclContext(SyntaxContext,
                                   SyntaxKind::MemberDeclListItem);
  if (loadCurrentSyntaxNodeFromCache()) {
    return ParserStatus();
  }
  Result = parseDecl(Options, IsAtStartOfLineOrPreviousHadSemi, handler);
  if (Result.isParseError())
    skipUntilDeclRBrace(tok::semi, tok::pound_endif);
  SourceLoc SemiLoc;
  PreviousHadSemi = consumeIf(tok::semi, SemiLoc);
  if (PreviousHadSemi && Result.isNonNull())
    Result.get()->TrailingSemiLoc = SemiLoc;
  return Result;
}

/// Parse the members in a struct/class/enum/protocol/extension.
///
/// \verbatim
///    decl* '}'
/// \endverbatim
bool Parser::parseDeclList(SourceLoc LBLoc, SourceLoc &RBLoc,
                           Diag<> ErrorDiag, ParseDeclOptions Options,
                           IterableDeclContext *IDC) {
  ParserStatus Status;
  bool PreviousHadSemi = true;
  {
    SyntaxParsingContext ListContext(SyntaxContext, SyntaxKind::MemberDeclList);
    while (Tok.isNot(tok::r_brace)) {
      Status |= parseDeclItem(PreviousHadSemi, Options,
                              [&](Decl *D) { IDC->addMember(D); });
      if (Tok.isAny(tok::eof, tok::pound_endif, tok::pound_else,
                    tok::pound_elseif)) {
        IsInputIncomplete = true;
        break;
      }
    }
  }
  if (parseMatchingToken(tok::r_brace, RBLoc, ErrorDiag, LBLoc)) {
    // Synthesize an r_brace syntax node if the token is absent
    SyntaxContext->synthesize(tok::r_brace, RBLoc);
  }

  // Increase counter.
  if (auto *stat = Context.Stats) {
    stat->getFrontendCounters().NumIterableDeclContextParsed ++;
  }
  // If we found the closing brace, then the caller should not care if there
  // were errors while parsing inner decls, because we recovered.
  return !RBLoc.isValid();
}

bool Parser::canDelayMemberDeclParsing() {
  // If explicitly disabled, respect the flag.
  if (!DelayBodyParsing)
    return false;
  // Recovering parser status later for #sourceLocation is not-trivial and
  // it may not worth it.
  if (InPoundLineEnvironment)
    return false;

  // Skip until the matching right curly bracket; if we find a pound directive,
  // we can't lazily parse.
  BacktrackingScope BackTrack(*this);
  bool HasPoundDirective;
  skipUntilMatchingRBrace(*this, HasPoundDirective, SyntaxContext);
  if (!HasPoundDirective)
    BackTrack.cancelBacktrack();
  return !BackTrack.willBacktrack();
}

bool Parser::delayParsingDeclList(SourceLoc LBLoc, SourceLoc &RBLoc,
                                  SourceLoc PosBeforeLB,
                                  ParseDeclOptions Options,
                                  IterableDeclContext *IDC) {
  bool error = false;

  if (Tok.is(tok::r_brace)) {
    RBLoc = consumeToken();
  } else {
    RBLoc = Tok.getLoc();
    error = true;
  }
  State->delayDeclList(IDC, Options.toRaw(), CurDeclContext, { LBLoc, RBLoc },
                        PosBeforeLB);
  return error;
}

/// Parse an 'extension' declaration.
///
/// \verbatim
///   extension:
///    'extension' attribute-list type inheritance? where-clause?
///        '{' decl* '}'
/// \endverbatim
ParserResult<ExtensionDecl>
Parser::parseDeclExtension(ParseDeclOptions Flags, DeclAttributes &Attributes) {
  SourceLoc ExtensionLoc = consumeToken(tok::kw_extension);
  
  DebuggerContextChange DCC (*this);

  // Parse the type being extended.
  ParserStatus status;
  ParserResult<TypeRepr> extendedType = parseType(diag::extension_type_expected);
  status |= extendedType;

  // Parse optional inheritance clause.
  SmallVector<TypeLoc, 2> Inherited;
  if (Tok.is(tok::colon))
    status |= parseInheritance(Inherited,
                               /*allowClassRequirement=*/false,
                               /*allowAnyObject=*/false);

  // Parse the optional where-clause.
  TrailingWhereClause *trailingWhereClause = nullptr;
  if (Tok.is(tok::kw_where)) {
    SourceLoc whereLoc;
    SmallVector<RequirementRepr, 4> requirements;
    bool firstTypeInComplete;
    auto whereStatus = parseGenericWhereClause(whereLoc, requirements,
                                               firstTypeInComplete);
    if (whereStatus.isSuccess()) {
      trailingWhereClause = TrailingWhereClause::create(Context, whereLoc,
                                                        requirements);
    } else if (whereStatus.hasCodeCompletion()) {
      if (CodeCompletion && firstTypeInComplete) {
        CodeCompletion->completeGenericParams(extendedType.getPtrOrNull());
      } else
        return makeParserCodeCompletionResult<ExtensionDecl>();
    }
  }

  ExtensionDecl *ext = ExtensionDecl::create(Context, ExtensionLoc,
                                             extendedType.getPtrOrNull(),
                                             Context.AllocateCopy(Inherited),
                                             CurDeclContext,
                                             trailingWhereClause);
  ext->getAttrs() = Attributes;

  SyntaxParsingContext BlockContext(SyntaxContext, SyntaxKind::MemberDeclBlock);
  SourceLoc LBLoc, RBLoc;

  auto PosBeforeLB = Tok.getLoc();
  if (parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_extension)) {
    LBLoc = PreviousLoc;
    RBLoc = LBLoc;
    status.setIsParseError();
  } else {
    ContextChange CC(*this, ext);
    Scope S(this, ScopeKind::Extension);
    ParseDeclOptions Options(PD_HasContainerType | PD_InExtension);
    if (canDelayMemberDeclParsing()) {
      if (delayParsingDeclList(LBLoc, RBLoc, PosBeforeLB, Options, ext))
        status.setIsParseError();
    } else {
      if (parseDeclList(LBLoc, RBLoc, diag::expected_rbrace_extension,
                        Options, ext))
        status.setIsParseError();
    }

    // Don't propagate the code completion bit from members: we cannot help
    // code completion inside a member decl, and our callers cannot do
    // anything about it either.  But propagate the error bit.
  }
  ext->setBraces({LBLoc, RBLoc});
  if (!DCC.movedToTopLevel() && !(Flags & PD_AllowTopLevel)) {
    diagnose(ExtensionLoc, diag::decl_inner_scope);
    status.setIsParseError();

    // Tell the type checker not to touch this extension.
    ext->setInvalid();
  }

  return DCC.fixupParserResult(status, ext);
}

ParserResult<PoundDiagnosticDecl> Parser::parseDeclPoundDiagnostic() {
  bool isError = Tok.is(tok::pound_error);
  SyntaxParsingContext LocalContext(SyntaxContext, 
    isError ? SyntaxKind::PoundErrorDecl : SyntaxKind::PoundWarningDecl);
  SourceLoc startLoc = 
    consumeToken(isError ? tok::pound_error : tok::pound_warning);

  SourceLoc lParenLoc = Tok.getLoc();
  bool hadLParen = consumeIf(tok::l_paren);

  if (!Tok.is(tok::string_literal)) {
    // Catch #warning(oops, forgot the quotes)
    SourceLoc wordsStartLoc = Tok.getLoc();

    skipUntilTokenOrEndOfLine(tok::r_paren);

    SourceLoc wordsEndLoc = getEndOfPreviousLoc();

    auto diag = diagnose(wordsStartLoc, 
                          diag::pound_diagnostic_expected_string, isError);
    if (wordsEndLoc != wordsStartLoc) {
      diag.fixItInsert(wordsStartLoc, hadLParen ? "\"" : "(\"")
          .fixItInsert(wordsEndLoc, Tok.is(tok::r_paren) ? "\"" : "\")");
    }

    // Consume the right paren to finish the decl, if it's there.
    consumeIf(tok::r_paren);

    return makeParserError();
  }

  auto string = parseExprStringLiteral();
  if (string.isNull())
    return makeParserError();

  auto messageExpr = string.get();

  SourceLoc rParenLoc = Tok.getLoc();
  bool hadRParen = consumeIf(tok::r_paren);

  if (!Tok.isAtStartOfLine() && Tok.isNot(tok::eof)) {
    diagnose(Tok.getLoc(),
             diag::extra_tokens_pound_diagnostic_directive, isError);
    return makeParserError();
  }

  if (!hadLParen && !hadRParen) {
    // Catch if the user forgot parentheses around the string, e.g.
    // #warning "foo"
    diagnose(lParenLoc, diag::pound_diagnostic_expected_parens, isError)
      .highlight(messageExpr->getSourceRange())
      .fixItInsert(messageExpr->getStartLoc(), "(")
      .fixItInsertAfter(messageExpr->getEndLoc(), ")");
    return makeParserError();
  } else if (hadRParen && !hadLParen) {
    // Catch if the user forgot a left paren before the string, e.g.
    // #warning "foo")
    diagnose(messageExpr->getStartLoc(), diag::pound_diagnostic_expected,
             "(", isError)
      .fixItInsert(messageExpr->getStartLoc(), "(");
    return makeParserError();
  } else if (hadLParen && !hadRParen) {
    // Catch if the user forgot a right paren after the string, e.g.
    // #warning("foo"
    diagnose(messageExpr->getEndLoc(), diag::pound_diagnostic_expected,
             ")", isError)
      .fixItInsertAfter(messageExpr->getEndLoc(), ")");
    return makeParserError();
  }

  if (messageExpr->getKind() == ExprKind::InterpolatedStringLiteral) {
    diagnose(messageExpr->getStartLoc(), diag::pound_diagnostic_interpolation,
             isError)
      .highlight(messageExpr->getSourceRange());
    return makeParserError();
  }

  ParserStatus Status;
  return makeParserResult(Status,
    new (Context) PoundDiagnosticDecl(CurDeclContext, isError,
                                      startLoc, rParenLoc,
                                      cast<StringLiteralExpr>(messageExpr)));
}

ParserStatus Parser::parseLineDirective(bool isLine) {
  SyntaxParsingContext PoundSourceLocation(SyntaxContext,
                                           SyntaxKind::PoundSourceLocation);
  SourceLoc Loc = consumeToken();
  if (isLine) {
    diagnose(Loc, diag::line_directive_style_deprecated)
        .fixItReplace(Loc, "#sourceLocation");
  }
  bool WasInPoundLineEnvironment = InPoundLineEnvironment;
  if (WasInPoundLineEnvironment) {
    SourceMgr.closeVirtualFile(Loc);
    InPoundLineEnvironment = false;
  }

  
  unsigned StartLine = 0;
  Optional<StringRef> Filename;
  if (!isLine) {
    // #sourceLocation()
    // #sourceLocation(file: "foo", line: 42)
    if (parseToken(tok::l_paren, diag::sourceLocation_expected, "("))
      return makeParserError();

    // Handle the "reset" form.
    if (consumeIf(tok::r_paren)) {
      if (!WasInPoundLineEnvironment) {
        diagnose(Tok, diag::unexpected_line_directive);
        return makeParserError();
      }
      return makeParserSuccess();
    }

    {
      SyntaxParsingContext Args(SyntaxContext,
                                SyntaxKind::PoundSourceLocationArgs);

      if (parseSpecificIdentifier("file", diag::sourceLocation_expected,
                                  "file:") ||
          parseToken(tok::colon, diag::sourceLocation_expected, ":"))
        return makeParserError();

      if (Tok.isNot(tok::string_literal)) {
        diagnose(Tok, diag::expected_line_directive_name);
        return makeParserError();
      }

      Filename =
          getStringLiteralIfNotInterpolated(Loc, "'#sourceLocation'");
      if (!Filename.hasValue())
        return makeParserError();
      consumeToken(tok::string_literal);

      if (parseToken(tok::comma, diag::sourceLocation_expected, ",") ||
          parseSpecificIdentifier("line", diag::sourceLocation_expected,
                                  "line:") ||
          parseToken(tok::colon, diag::sourceLocation_expected, ":"))
        return makeParserError();

      if (Tok.isNot(tok::integer_literal)) {
        diagnose(Tok, diag::expected_line_directive_number);
        return makeParserError();
      }
      if (Tok.getText().getAsInteger(0, StartLine)) {
        diagnose(Tok, diag::expected_line_directive_number);
        return makeParserError();
      }
      if (StartLine == 0) {
        diagnose(Tok, diag::line_directive_line_zero);
        return makeParserError();
      }
      consumeToken(tok::integer_literal);
    }

    if (Tok.isNot(tok::r_paren)) {
      diagnose(Tok, diag::sourceLocation_expected, ")");
      return makeParserError();
    }
  } else {  // Legacy #line syntax.
  
    // #line\n returns to the main buffer.
    if (Tok.isAtStartOfLine()) {
      if (!WasInPoundLineEnvironment) {
        diagnose(Tok, diag::unexpected_line_directive);
        return makeParserError();
      }
      return makeParserSuccess();
    }

    // #line 42 "file.swift"\n
    if (Tok.isNot(tok::integer_literal)) {
      diagnose(Tok, diag::expected_line_directive_number);
      return makeParserError();
    }
    if (Tok.getText().getAsInteger(0, StartLine)) {
      diagnose(Tok, diag::expected_line_directive_number);
      return makeParserError();
    }
    if (StartLine == 0) {
      diagnose(Tok, diag::line_directive_line_zero);
      return makeParserError();
    }
    consumeToken(tok::integer_literal);

    if (Tok.isNot(tok::string_literal)) {
      diagnose(Tok, diag::expected_line_directive_name);
      return makeParserError();
    }
    
    Filename = getStringLiteralIfNotInterpolated(Loc, "'#line'");
    if (!Filename.hasValue())
      return makeParserError();
  }

  const char *LastTokTextEnd = Tok.getText().end();

  // Skip over trailing whitespace and a single \n to the start of the next
  // line.
  while (*LastTokTextEnd == ' ' || *LastTokTextEnd == '\t')
    ++LastTokTextEnd;
  SourceLoc nextLineStartLoc = Lexer::getSourceLoc(LastTokTextEnd);
  
  if (*LastTokTextEnd == '\n')
    nextLineStartLoc = nextLineStartLoc.getAdvancedLoc(1);
  else {
    diagnose(Tok.getLoc(), diag::extra_tokens_line_directive);
    return makeParserError();
  }
  
  int LineOffset = StartLine - SourceMgr.getLineNumber(nextLineStartLoc);
 
  // Create a new virtual file for the region started by the #line marker.
  bool isNewFile = SourceMgr.openVirtualFile(nextLineStartLoc,
                                             Filename.getValue(), LineOffset);
  assert(isNewFile);(void)isNewFile;

  // Lexing of next token must be deferred until after virtual file setup.
  consumeToken(isLine ? tok::string_literal : tok::r_paren);

  InPoundLineEnvironment = true;
  return makeParserSuccess();
}

/// Parse a typealias decl.
///
/// \verbatim
///   decl-typealias:
///     'typealias' identifier generic-params? '=' type requirement-clause?
/// \endverbatim
ParserResult<TypeDecl> Parser::
parseDeclTypeAlias(Parser::ParseDeclOptions Flags, DeclAttributes &Attributes) {
  ParserPosition startPosition = getParserPosition();
  llvm::Optional<SyntaxParsingContext> TmpCtxt;
  TmpCtxt.emplace(SyntaxContext);
  TmpCtxt->setTransparent();

  SourceLoc TypeAliasLoc = consumeToken(tok::kw_typealias);
  SourceLoc EqualLoc;
  Identifier Id;
  SourceLoc IdLoc;
  ParserStatus Status;

  Status |= parseIdentifierDeclName(*this, Id, IdLoc, "typealias",
                                    tok::colon, tok::equal);
  if (Status.isError())
    return nullptr;
    
  DebuggerContextChange DCC(*this, Id, DeclKind::TypeAlias);

  Optional<Scope> GenericsScope;
  GenericsScope.emplace(this, ScopeKind::Generics);

  // Parse a generic parameter list if it is present.
  GenericParamList *genericParams = nullptr;
  if (startsWithLess(Tok)) {
    auto Result = parseGenericParameters();
    if (Result.hasCodeCompletion() && !CodeCompletion)
      return makeParserCodeCompletionStatus();
    genericParams = Result.getPtrOrNull();

    if (!genericParams) {
      // If the parser returned null, it is an already diagnosed parse error.
    } else if (!genericParams->getRequirements().empty()) {
      // Reject a where clause.
      diagnose(genericParams->getWhereLoc(),
               diag::associated_type_generic_parameter_list)
          .highlight(genericParams->getWhereClauseSourceRange());
    }
  }

  if (Flags.contains(PD_InProtocol) && !genericParams && !Tok.is(tok::equal)) {
    TmpCtxt->setBackTracking();
    TmpCtxt.reset();
    // If we're in a protocol and don't see an '=' this looks like leftover Swift 2
    // code intending to be an associatedtype.
    backtrackToPosition(startPosition);
    return parseDeclAssociatedType(Flags, Attributes);
  }
  TmpCtxt.reset();

  auto *TAD = new (Context) TypeAliasDecl(TypeAliasLoc, EqualLoc, Id, IdLoc,
                                          /*genericParams*/nullptr,
                                          CurDeclContext);
  setLocalDiscriminator(TAD);
  ParserResult<TypeRepr> UnderlyingTy;

  if (Tok.is(tok::colon) || Tok.is(tok::equal)) {
    ContextChange CC(*this, TAD);

    SyntaxParsingContext InitCtx(SyntaxContext,
                                 SyntaxKind::TypeInitializerClause);
    if (Tok.is(tok::colon)) {
      // It is a common mistake to write "typealias A : Int" instead of = Int.
      // Recognize this and produce a fixit.
      diagnose(Tok, diag::expected_equal_in_typealias)
          .fixItReplace(Tok.getLoc(), " = ");
      EqualLoc = consumeToken(tok::colon);
    } else {
      EqualLoc = consumeToken(tok::equal);
    }

    UnderlyingTy = parseType(diag::expected_type_in_typealias);
    TAD->setTypeEndLoc(PreviousLoc);
    Status |= UnderlyingTy;
  }

  TAD->getUnderlyingTypeLoc() = UnderlyingTy.getPtrOrNull();
  TAD->getAttrs() = Attributes;

  // Parse a 'where' clause if present, adding it to our GenericParamList.
  if (Tok.is(tok::kw_where)) {
    ContextChange CC(*this, TAD);
    Status |= parseFreestandingGenericWhereClause(genericParams);
  }

  // Set after parsing the where clause, which might create genericParams.
  TAD->setGenericParams(genericParams);

  if (UnderlyingTy.isNull()) {
    // If there is an attempt to do code completion
    // inside of typealias type, let's just return
    // because we've seen required '=' token.
    if (EqualLoc.isInvalid()) {
      diagnose(Tok, diag::expected_equal_in_typealias);
      Status.setIsParseError();
      return Status;
    }
  }

  // Exit the scope introduced for the generic parameters.
  GenericsScope.reset();

  addToScope(TAD);
  return DCC.fixupParserResult(Status, TAD);
}

/// Parse an associatedtype decl.
///
/// \verbatim
///   decl-associatedtype:
///     'associatedtype' identifier inheritance? ('=' type)? where-clause?
/// \endverbatim

ParserResult<TypeDecl> Parser::parseDeclAssociatedType(Parser::ParseDeclOptions Flags,
                                                       DeclAttributes &Attributes) {
  SourceLoc AssociatedTypeLoc;
  ParserStatus Status;
  Identifier Id;
  SourceLoc IdLoc;
  
  // Look for 'typealias' here and diagnose a fixit because parseDeclTypeAlias can
  // ask us to fix up leftover Swift 2 code intending to be an associatedtype.
  if (Tok.is(tok::kw_typealias)) {
    AssociatedTypeLoc = consumeToken(tok::kw_typealias);
    diagnose(AssociatedTypeLoc, diag::typealias_inside_protocol_without_type)
        .fixItReplace(AssociatedTypeLoc, "associatedtype");
  } else {
    AssociatedTypeLoc = consumeToken(tok::kw_associatedtype);
  }
  
  Status = parseIdentifierDeclName(*this, Id, IdLoc, "associatedtype",
                                   tok::colon, tok::equal);
  if (Status.isError())
    return nullptr;
  
  DebuggerContextChange DCC(*this, Id, DeclKind::AssociatedType);
  
  // Reject generic parameters with a specific error.
  if (startsWithLess(Tok)) {
    // Introduce a throwaway scope to capture the generic parameters. We
    // don't want them visible anywhere!
    Scope S(this, ScopeKind::Generics);

    if (auto genericParams = parseGenericParameters().getPtrOrNull()) {
      diagnose(genericParams->getLAngleLoc(),
               diag::associated_type_generic_parameter_list)
      .fixItRemove(genericParams->getSourceRange());
    }
  }
  
  // Parse optional inheritance clause.
  // FIXME: Allow class requirements here.
  SmallVector<TypeLoc, 2> Inherited;
  if (Tok.is(tok::colon))
    Status |= parseInheritance(Inherited,
                               /*allowClassRequirement=*/false,
                               /*allowAnyObject=*/true);
  
  ParserResult<TypeRepr> UnderlyingTy;
  if (Tok.is(tok::equal)) {
    SyntaxParsingContext InitContext(SyntaxContext,
                                     SyntaxKind::TypeInitializerClause);
    consumeToken(tok::equal);
    UnderlyingTy = parseType(diag::expected_type_in_associatedtype);
    Status |= UnderlyingTy;
    if (UnderlyingTy.isNull())
      return Status;
  }

  TrailingWhereClause *TrailingWhere = nullptr;
  // Parse a 'where' clause if present.
  if (Tok.is(tok::kw_where)) {
    auto whereStatus = parseProtocolOrAssociatedTypeWhereClause(
        TrailingWhere, /*isProtocol=*/false);
    Status |= whereStatus;
    if (whereStatus.hasCodeCompletion() && !CodeCompletion) {
      // Trigger delayed parsing, no need to continue.
      return whereStatus;
    }
  }

  if (!Flags.contains(PD_InProtocol)) {
    diagnose(AssociatedTypeLoc, diag::associatedtype_outside_protocol)
        .fixItReplace(AssociatedTypeLoc, "typealias");
    Status.setIsParseError();
    return Status;
  }

  auto assocType = new (Context)
      AssociatedTypeDecl(CurDeclContext, AssociatedTypeLoc, Id, IdLoc,
                         UnderlyingTy.getPtrOrNull(), TrailingWhere);
  assocType->getAttrs() = Attributes;
  if (!Inherited.empty())
    assocType->setInherited(Context.AllocateCopy(Inherited));
  addToScope(assocType);
  return makeParserResult(Status, assocType);
}

/// This function creates an accessor function (with no body) for a computed
/// property or subscript.
static AccessorDecl *createAccessorFunc(SourceLoc DeclLoc,
                                    ParameterList *param,
                                    GenericParamList *GenericParams,
                                    ParameterList *Indices,
                                    TypeLoc ElementTy,
                                    SourceLoc StaticLoc,
                                    Parser::ParseDeclOptions Flags,
                                    AccessorKind Kind,
                                    AbstractStorageDecl *storage,
                                    Parser *P, SourceLoc AccessorKeywordLoc) {
  // First task, set up the value argument list.  This is the "newValue" name
  // (for setters) followed by the index list (for subscripts).  For
  // non-subscript getters, this degenerates down to "()".
  //
  // We put the 'newValue' argument before the subscript index list as a
  // micro-optimization for Objective-C thunk generation.
  ParameterList *ValueArg;
  {
    SmallVector<ParamDecl*, 2> ValueArgElements;
    SourceLoc StartLoc, EndLoc;
    if (param) {
      assert(param->size() == 1 &&
             "Should only have a single parameter in the list");
      ValueArgElements.push_back(param->get(0));
      StartLoc = param->getStartLoc();
      EndLoc = param->getEndLoc();
    }

    if (Indices) {
      // Create parameter declarations corresponding to each of the
      // parameter declarations from the subscript declaration.
      for (ParamDecl *storageParam : *Indices) {
        // Clone the parameter.  Do not clone the parameter type;
        // this will be filled in by the type-checker.
        auto accessorParam =
          new (P->Context) ParamDecl(storageParam->getSpecifier(),
                                     storageParam->getSpecifierLoc(),
                                     storageParam->getArgumentNameLoc(),
                                     storageParam->getArgumentName(),
                                     storageParam->getNameLoc(),
                                     storageParam->getName(),
                                     P->CurDeclContext);
        accessorParam->setVariadic(storageParam->isVariadic());
        accessorParam->setAutoClosure(storageParam->isAutoClosure());

        // The cloned parameter is implicit.
        accessorParam->setImplicit();

        // It has no default arguments; these will be always be taken
        // from the subscript declaration.
        accessorParam->setDefaultArgumentKind(DefaultArgumentKind::None);

        ValueArgElements.push_back(accessorParam);
      }

      if (StartLoc.isInvalid()) {
        StartLoc = Indices->getStartLoc();
        EndLoc = Indices->getEndLoc();
      }
    }

    ValueArg = ParameterList::create(P->Context, StartLoc, ValueArgElements,
                                     EndLoc);
  }

  // The typechecker will always fill this in.
  TypeLoc ReturnType;

  // Start the function.
  auto *D = AccessorDecl::create(P->Context,
                                 /*FIXME FuncLoc=*/DeclLoc,
                                 AccessorKeywordLoc,
                                 Kind, storage,
                                 StaticLoc, StaticSpellingKind::None,
                                 /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(),
                                 (GenericParams
                                  ? GenericParams->clone(P->CurDeclContext)
                                  : nullptr),
                                 ValueArg, ReturnType,
                                 P->CurDeclContext);

  return D;
}

static ParamDecl *createSetterAccessorArgument(SourceLoc nameLoc,
                                               Identifier name,
                                               AccessorKind accessorKind,
                                               Parser &P, TypeLoc elementType) {
  // Add the parameter. If no name was specified, the name defaults to
  // 'value'.
  bool isNameImplicit = name.empty();
  if (isNameImplicit) {
    const char *implName =
      accessorKind == AccessorKind::DidSet ? "oldValue" : "newValue";
    name = P.Context.getIdentifier(implName);
  }

  auto result = new (P.Context)
      ParamDecl(ParamDecl::Specifier::Default, SourceLoc(), SourceLoc(),
                Identifier(), nameLoc, name, P.CurDeclContext);
  if (isNameImplicit)
    result->setImplicit();

  // AST Walker shouldn't go into the type recursively.
  result->setIsTypeLocImplicit(true);

  if (auto *repr = elementType.getTypeRepr()) {
    if (repr->getKind() ==
        TypeReprKind::ImplicitlyUnwrappedOptional) {
      result->getAttrs().add(
          new (P.Context) ImplicitlyUnwrappedOptionalAttr(/* implicit= */ true));
    }
  }

  return result;
}

/// Parse a "(value)" specifier for "set" or "willSet" if present.  Create a
/// parameter list to represent the spelled argument or return null if none is
/// present.
static ParameterList *parseOptionalAccessorArgument(SourceLoc SpecifierLoc,
                                                    Parser &P,
                                                    AccessorKind Kind,
                                                    TypeLoc ElementTy) {
  // 'set' and 'willSet' have a (value) parameter, 'didSet' takes an (oldValue)
  // parameter and 'get' and always takes a () parameter.
  if (Kind != AccessorKind::Set && Kind != AccessorKind::WillSet &&
      Kind != AccessorKind::DidSet)
    return nullptr;

  SourceLoc StartLoc, NameLoc, EndLoc;
  Identifier Name;

  // If the SpecifierLoc is invalid, then the caller just wants us to synthesize
  // the default, not actually try to parse something.
  if (SpecifierLoc.isValid() && P.Tok.is(tok::l_paren)) {
    SyntaxParsingContext ParamCtx(P.SyntaxContext, SyntaxKind::AccessorParameter);
    StartLoc = P.consumeToken(tok::l_paren);
    if (P.Tok.isNot(tok::identifier)) {
      P.diagnose(P.Tok, diag::expected_accessor_parameter_name,
                 Kind == AccessorKind::Set ? 0 :
                 Kind == AccessorKind::WillSet ? 1 : 2);
      P.skipUntil(tok::r_paren, tok::l_brace);
      if (P.Tok.is(tok::r_paren))
        EndLoc = P.consumeToken();
      else
        EndLoc = StartLoc;
    } else {
      // We have a name.
      NameLoc = P.consumeIdentifier(&Name);

      auto DiagID =
         Kind == AccessorKind::Set ? diag::expected_rparen_set_name :
         Kind == AccessorKind::WillSet ? diag::expected_rparen_willSet_name :
          diag::expected_rparen_didSet_name;
      
      // Look for the closing ')'.
      P.parseMatchingToken(tok::r_paren, EndLoc, DiagID, StartLoc);
    }
  }

  if (Name.empty()) NameLoc = SpecifierLoc;
  auto param = createSetterAccessorArgument(NameLoc, Name, Kind, P, ElementTy);
  return ParameterList::create(P.Context, StartLoc, param, EndLoc);
}

static unsigned skipBracedBlock(Parser &P,
                                SyntaxParsingContext *&SyntaxContext) {
  SyntaxParsingContext CodeBlockContext(SyntaxContext, SyntaxKind::CodeBlock);
  P.consumeToken(tok::l_brace);
  bool HasPoundDirectives;
  unsigned OpenBraces = skipUntilMatchingRBrace(P, HasPoundDirectives,
                                                SyntaxContext);
  if (P.consumeIf(tok::r_brace))
    OpenBraces--;
  return OpenBraces;
}

/// Returns a descriptive name for the given accessor/addressor kind.
static StringRef getAccessorNameForDiagnostic(AccessorKind accessorKind,
                                              bool article) {
  switch (accessorKind) {
  case AccessorKind::Get:
    return article ? "a getter" : "getter";
  case AccessorKind::Set:
    return article ? "a setter" : "setter";
  case AccessorKind::Address:
    return article ? "an addressor" : "addressor";
  case AccessorKind::MutableAddress:
    return article ? "a mutable addressor" : "mutable addressor";
  case AccessorKind::Read:
    return article ? "a 'read' accessor" : "'read' accessor";
  case AccessorKind::Modify:
    return article ? "a 'modify' accessor" : "'modify' accessor";
  case AccessorKind::WillSet:
    return "'willSet'";
  case AccessorKind::DidSet:
    return "'didSet'";
  }
  llvm_unreachable("bad accessor kind");  
}

static StringRef getAccessorNameForDiagnostic(AccessorDecl *accessor,
                                              bool article) {
  return getAccessorNameForDiagnostic(accessor->getAccessorKind(),
                                      article);
}

static void diagnoseRedundantAccessors(Parser &P, SourceLoc loc,
                                       AccessorKind accessorKind,
                                       bool isSubscript,
                                       AccessorDecl *previous) {
  assert(accessorKind == previous->getAccessorKind());

  P.diagnose(loc, diag::duplicate_accessor,
             unsigned(isSubscript),
             getAccessorNameForDiagnostic(previous, /*article*/ true));
  P.diagnose(previous->getLoc(), diag::previous_accessor,
             getAccessorNameForDiagnostic(previous, /*article*/ false),
             /*already*/ true);
}

static bool isAllowedInLimitedSyntax(AccessorKind kind) {
  switch (kind) {
  case AccessorKind::Get:
  case AccessorKind::Set:
    return true;

  case AccessorKind::Address:
  case AccessorKind::MutableAddress:
  case AccessorKind::WillSet:
  case AccessorKind::DidSet:
  case AccessorKind::Read:
  case AccessorKind::Modify:
    return false;
  }
  llvm_unreachable("bad accessor kind");
}

struct Parser::ParsedAccessors {
  SourceLoc LBLoc, RBLoc;
  SmallVector<AccessorDecl*, 16> Accessors;

#define ACCESSOR(ID) AccessorDecl *ID = nullptr;
#include "swift/AST/AccessorKinds.def"

  void record(Parser &P, AbstractStorageDecl *storage, bool invalid);
  void classify(Parser &P, AbstractStorageDecl *storage, bool invalid);

  /// Add an accessor.  If there's an existing accessor of this kind,
  /// return it.  The new accessor is still remembered but will be
  /// ignored.
  AccessorDecl *add(AccessorDecl *accessor);

  /// Find the first accessor that's not an observing accessor.
  AccessorDecl *findFirstNonObserver() {
    for (auto accessor : Accessors) {
      if (!accessor->isObservingAccessor())
        return accessor;
    }
    return nullptr;
  }

  /// Find the first accessor that can be used to perform mutation.
  AccessorDecl *findFirstMutator() const {
    if (Set) return Set;
    if (Modify) return Modify;
    if (MutableAddress) return MutableAddress;
    return nullptr;
  }
};

static bool parseAccessorIntroducer(Parser &P,
                                    DeclAttributes &Attributes,
                                    AccessorKind &Kind,
                                    SourceLoc &Loc) {
  assert(Attributes.isEmpty());
  P.parseDeclAttributeList(Attributes);

  // Parse the contextual keywords for 'mutating' and 'nonmutating' before
  // get and set.
  {
    SyntaxParsingContext ModifierCtx(P.SyntaxContext, SyntaxKind::DeclModifier);

    if (P.Tok.isContextualKeyword("mutating")) {
      P.parseNewDeclAttribute(Attributes, /*AtLoc*/ {}, DAK_Mutating);
    } else if (P.Tok.isContextualKeyword("nonmutating")) {
      P.parseNewDeclAttribute(Attributes, /*AtLoc*/ {}, DAK_NonMutating);
    } else if (P.Tok.isContextualKeyword("__consuming")) {
      P.parseNewDeclAttribute(Attributes, /*AtLoc*/ {}, DAK_Consuming);
    } else {
      ModifierCtx.setTransparent();
    }
  }

  if (!P.Tok.is(tok::identifier) || P.Tok.isEscapedIdentifier()) {
    return true;
  }
#define SUPPRESS_ARTIFICIAL_ACCESSORS 1
#define ACCESSOR_KEYWORD(KEYWORD)
#define SINGLETON_ACCESSOR(ID, KEYWORD)                                        \
  else if (P.Tok.getRawText() == #KEYWORD) {                                   \
    Kind = AccessorKind::ID;                                                   \
  }
#include "swift/AST/AccessorKinds.def"
  else {
    return true;
  }
  P.Tok.setKind(tok::contextual_keyword);
  Loc = P.consumeToken();
  return false;
}

ParserStatus Parser::parseGetSet(ParseDeclOptions Flags,
                                 GenericParamList *GenericParams,
                                 ParameterList *Indices,
                                 TypeLoc ElementTy, ParsedAccessors &accessors,
                                 AbstractStorageDecl *storage,
                                 SourceLoc StaticLoc) {
  assert(Tok.is(tok::l_brace));

  // Properties in protocols use a very limited syntax.
  // SIL mode and parseable interfaces use the same syntax.
  // Otherwise, we have a normal var or subscript declaration and we need
  // parse the full complement of specifiers, along with their bodies.
  bool parsingLimitedSyntax = Flags.contains(PD_InProtocol) ||
                              SF.Kind == SourceFileKind::SIL;

  SyntaxParsingContext AccessorListCtx(SyntaxContext,
                                       SyntaxKind::AccessorBlock);

  // If the body is completely empty, preserve it. This is at best a getter with
  // an implicit fallthrough off the end.
  if (peekToken().is(tok::r_brace)) {
    accessors.LBLoc = consumeToken(tok::l_brace);
    // Give syntax node an empty accessor list.
    SourceLoc listLoc = accessors.LBLoc.getAdvancedLoc(1);
    SyntaxContext->addSyntax(
      ParsedSyntaxRecorder::makeBlankAccessorList(listLoc, *SyntaxContext));
    accessors.RBLoc = consumeToken(tok::r_brace);

    // In the limited syntax, fall out and let the caller handle it.
    if (parsingLimitedSyntax)
      return makeParserSuccess();

    diagnose(accessors.RBLoc, diag::computed_property_no_accessors,
             /*subscript*/ Indices != nullptr);
    return makeParserError();
  }

  auto parseImplicitGetter = [&]() {
    assert(Tok.is(tok::l_brace));
    accessors.LBLoc = Tok.getLoc();
    auto getter =
        createAccessorFunc(Tok.getLoc(), /*ValueNamePattern*/ nullptr,
                           GenericParams, Indices, ElementTy, StaticLoc, Flags,
                           AccessorKind::Get, storage, this,
                           /*AccessorKeywordLoc*/ SourceLoc());
    accessors.add(getter);
    parseAbstractFunctionBody(getter);
    accessors.RBLoc = getter->getEndLoc();
  };

  // Prepare backtracking for implicit getter.
  Optional<BacktrackingScope> backtrack;
  backtrack.emplace(*this);

  bool Invalid = false;
  bool accessorHasCodeCompletion = false;
  bool IsFirstAccessor = true;
  accessors.LBLoc = consumeToken(tok::l_brace);
  while (!Tok.isAny(tok::r_brace, tok::eof)) {
    Optional<SyntaxParsingContext> AccessorCtx;
    AccessorCtx.emplace(SyntaxContext, SyntaxKind::AccessorDecl);

    // Parse introducer if possible.
    DeclAttributes Attributes;
    AccessorKind Kind = AccessorKind::Get;
    SourceLoc Loc;
    bool NotAccessor = parseAccessorIntroducer(
        *this, Attributes, Kind, Loc);
    if (NotAccessor) {
      AccessorCtx->setTransparent();
      AccessorCtx.reset();

      if (Tok.is(tok::code_complete)) {
        if (CodeCompletion) {
          CodeCompletionExpr *CCE = nullptr;
          if (IsFirstAccessor && !parsingLimitedSyntax) {
            // If CC token is the first token after '{', it might be implicit
            // getter. Set up dummy accessor as the decl context to populate
            // 'self' decl.

            // FIXME: if there is already code inside the body, we should fall
            // through to parseImplicitGetter and handle the completion there so
            // that we can differentiate a single-expression body from the first
            // expression in a multi-statement body.
            auto getter = createAccessorFunc(
                accessors.LBLoc, /*ValueNamePattern*/ nullptr, GenericParams,
                Indices, ElementTy, StaticLoc, Flags, AccessorKind::Get,
                storage, this, /*AccessorKeywordLoc*/ SourceLoc());
            CCE = new (Context) CodeCompletionExpr(Tok.getLoc());
            getter->setBodyParsed(BraceStmt::create(Context, Tok.getLoc(),
                                                    ASTNode(CCE), Tok.getLoc(),
                                                    /*implicit*/ true));
            accessors.add(getter);
            CodeCompletion->setParsedDecl(getter);
          } else {
            CodeCompletion->setParsedDecl(storage);
          }
          CodeCompletion->completeAccessorBeginning(CCE);
        }
        consumeToken(tok::code_complete);
        accessorHasCodeCompletion = true;
        break;
      }

      // parsingLimitedSyntax mode cannot have a body.
      if (parsingLimitedSyntax) {
        diagnose(Tok, diag::expected_getset_in_protocol);
        Invalid = true;
        break;
      }

      // Cannot have an implicit getter after other accessor.
      if (!IsFirstAccessor) {
        diagnose(Tok, diag::expected_accessor_kw);
        skipUntil(tok::r_brace);
        // Don't signal an error since we recovered.
        break;
      }

      // This is an implicit getter. Cancel accessor contexts, backtrack to '{'
      // position.
      backtrack.reset();
      AccessorListCtx.setTransparent();
      parseImplicitGetter();
      return makeParserSuccess();
    }
    IsFirstAccessor = false;

    // For now, immediately reject illegal accessors in protocols just to
    // avoid having to deal with them everywhere.
    if (parsingLimitedSyntax && !isAllowedInLimitedSyntax(Kind)) {
      diagnose(Loc, diag::expected_getset_in_protocol);
      continue;
    }

    // 'set' and 'willSet' can have an optional name.  This isn't valid in a
    // protocol, but we parse and then reject it for better QoI.
    //
    //     set-name    ::= '(' identifier ')'
    if (parsingLimitedSyntax && Tok.is(tok::l_paren)) {
      diagnose(Loc, diag::protocol_setter_name);
    }
    auto *ValueNamePattern =
        parseOptionalAccessorArgument(Loc, *this, Kind, ElementTy);

    // Set up a function declaration.
    auto accessor = createAccessorFunc(Loc, ValueNamePattern, GenericParams,
                                       Indices, ElementTy, StaticLoc, Flags,
                                       Kind, storage, this, Loc);
    accessor->getAttrs() = Attributes;

    // Collect this accessor and detect conflicts.
    if (auto existingAccessor = accessors.add(accessor)) {
      diagnoseRedundantAccessors(*this, Loc, Kind,
                                 /*subscript*/Indices != nullptr,
                                 existingAccessor);
    }

    // There's no body in the limited syntax.
    if (parsingLimitedSyntax)
      continue;

    // It's okay not to have a body if there's an external asm name.
    if (!Tok.is(tok::l_brace)) {
      // Accessors don't need bodies in parseable interfaces
      if (SF.Kind == SourceFileKind::Interface)
        continue;
      // _silgen_name'd accessors don't need bodies.
      if (!Attributes.hasAttribute<SILGenNameAttr>()) {
        diagnose(Tok, diag::expected_lbrace_accessor,
                 getAccessorNameForDiagnostic(accessor, /*article*/ false));
        Invalid = true;
        break;
      }
      continue;
    }

    parseAbstractFunctionBody(accessor);
  }
  backtrack->cancelBacktrack();
  backtrack.reset();
  // Collect all explicit accessors to a list.
  AccessorListCtx.collectNodesInPlace(SyntaxKind::AccessorList);
  // Parse the final '}'.
  if (Invalid)
    skipUntil(tok::r_brace);

  parseMatchingToken(tok::r_brace, accessors.RBLoc,
                     diag::expected_rbrace_in_getset, accessors.LBLoc);
  if (accessorHasCodeCompletion)
    return makeParserCodeCompletionStatus();
  return Invalid ? makeParserError() : makeParserSuccess();
}

/// Parse the brace-enclosed getter and setter for a variable.
ParserResult<VarDecl>
Parser::parseDeclVarGetSet(Pattern *pattern, ParseDeclOptions Flags,
                           SourceLoc StaticLoc,
                           StaticSpellingKind StaticSpelling,
                           SourceLoc VarLoc, bool hasInitializer,
                           const DeclAttributes &Attributes,
                           SmallVectorImpl<Decl *> &Decls) {
  bool Invalid = false;
  
  // The grammar syntactically requires a simple identifier for the variable
  // name. Complain if that isn't what we got. But for recovery purposes,
  // make an effort to look through other things anyway.
  VarDecl *PrimaryVar = nullptr;
  bool primaryVarIsWellFormed = true;
  {
    Pattern *cur = pattern;
    TypedPattern *previousTyped = nullptr;
    while (true) {
      if (auto typed = dyn_cast<TypedPattern>(cur)) {
        if (previousTyped) primaryVarIsWellFormed = false;
        previousTyped = typed;
        cur = typed->getSubPattern();
      } else if (auto paren = dyn_cast<ParenPattern>(cur)) {
        primaryVarIsWellFormed = false;
        cur = paren->getSubPattern();
      } else if (auto var = dyn_cast<VarPattern>(cur)) {
        primaryVarIsWellFormed = false;
        cur = var->getSubPattern();
      } else {
        break;
      }
    }

    if (auto named = dyn_cast<NamedPattern>(cur)) {
      PrimaryVar = named->getDecl();
    }
  }

  if (!PrimaryVar || !primaryVarIsWellFormed) {
    diagnose(pattern->getLoc(), diag::getset_nontrivial_pattern);
    Invalid = true;
  }

  TypeLoc TyLoc;
  if (auto *TP = dyn_cast<TypedPattern>(pattern)) {
    TyLoc = TP->getTypeLoc();
  } else if (!PrimaryVar) {
    TyLoc = TypeLoc::withoutLoc(ErrorType::get(Context));
  }

  // Create a fake VarDecl and PBD so that we don't have to weaken the
  // formation rule that an AccessorDecl always has a VarDecl.
  VarDecl *storage = PrimaryVar;
  if (!storage) {
    storage = new (Context) VarDecl(StaticLoc.isValid(),
                                    VarDecl::Introducer::Var,
                                    /*is capture list*/ false,
                                    VarLoc, Identifier(),
                                    CurDeclContext);
    storage->setImplicit(true);
    storage->setInvalid(true);

    Pattern *pattern =
      TypedPattern::createImplicit(Context, new (Context) NamedPattern(storage),
                                   ErrorType::get(Context));
    PatternBindingEntry entry(pattern, /*EqualLoc*/ SourceLoc(),
                              /*Init*/ nullptr, /*InitContext*/ nullptr);
    auto binding = PatternBindingDecl::create(Context, StaticLoc,
                                              StaticSpelling,
                                              VarLoc, entry, CurDeclContext);
    binding->setInvalid(true);
    storage->setParentPatternBinding(binding);

    Decls.push_back(binding);
    Decls.push_back(storage);
  }

  // Parse getter and setter.
  ParsedAccessors accessors;
  auto AccessorStatus = parseGetSet(Flags, /*GenericParams=*/nullptr,
                                    /*Indices=*/nullptr, TyLoc, accessors,
                                    storage, StaticLoc);
  if (AccessorStatus.hasCodeCompletion())
    return makeParserCodeCompletionStatus();
  if (AccessorStatus.isError())
    Invalid = true;

  // If we have an invalid case, bail out now.
  if (!PrimaryVar)
    return nullptr;

  if (!TyLoc.hasLocation()) {
    if (accessors.Get || accessors.Set || accessors.Address ||
        accessors.MutableAddress) {
      SourceLoc locAfterPattern = pattern->getLoc().getAdvancedLoc(
        pattern->getBoundName().getLength());
      diagnose(pattern->getLoc(), diag::computed_property_missing_type)
        .fixItInsert(locAfterPattern, ": <# Type #>");
      Invalid = true;
    }
  }

  // Reject accessors on 'let's after parsing them (for better recovery).
  if (PrimaryVar->isLet() && !Attributes.hasAttribute<HasStorageAttr>()) {
    Diag<> DiagID;
    if (accessors.WillSet || accessors.DidSet)
      DiagID = diag::let_cannot_be_observing_property;
    else if (accessors.Address || accessors.MutableAddress)
      DiagID = diag::let_cannot_be_addressed_property;
    else
      DiagID = diag::let_cannot_be_computed_property;

    diagnose(accessors.LBLoc, DiagID).fixItReplace(VarLoc, "var");
    Invalid = true;
  }

  accessors.record(*this, PrimaryVar, Invalid);

  return makeParserResult(PrimaryVar);
}

/// Add the given accessor to the collection of parsed accessors.  If
/// it's the first accessor of its kind, remember it for that purpose
/// and return null; otherwise, return the existing accessor.
AccessorDecl *Parser::ParsedAccessors::add(AccessorDecl *accessor) {
  Accessors.push_back(accessor);

  switch (accessor->getAccessorKind()) {
#define ACCESSOR(ID)                      \
  case AccessorKind::ID:                  \
    if (ID) {                             \
      return ID;                          \
    } else {                              \
      ID = accessor;                      \
      return nullptr;                     \
    }
#include "swift/AST/AccessorKinds.def"
  }
  llvm_unreachable("bad accessor kind");
}

/// Record a bunch of parsed accessors into the given abstract storage decl.
void Parser::ParsedAccessors::record(Parser &P, AbstractStorageDecl *storage,
                                     bool invalid) {
  classify(P, storage, invalid);
  storage->setAccessors(LBLoc, Accessors, RBLoc);
}

static void flagInvalidAccessor(AccessorDecl *func) {
  if (func) {
    func->setInvalid();
  }
}

static void diagnoseConflictingAccessors(Parser &P, AccessorDecl *first,
                                         AccessorDecl *&second) {
  if (!second) return;
  P.diagnose(second->getLoc(), diag::conflicting_accessor,
             isa<SubscriptDecl>(first->getStorage()),
             getAccessorNameForDiagnostic(second, /*article*/ true),
             getAccessorNameForDiagnostic(first, /*article*/ true));
  P.diagnose(first->getLoc(), diag::previous_accessor,
             getAccessorNameForDiagnostic(first, /*article*/ false),
             /*already*/ false);
  flagInvalidAccessor(second);
}

template <class... DiagArgs>
static void diagnoseAndIgnoreObservers(Parser &P,
                                       Parser::ParsedAccessors &accessors,
                                       Diag<unsigned, DiagArgs...> diagnostic,
                        typename std::enable_if<true, DiagArgs>::type... args) {
  if (auto &accessor = accessors.WillSet) {
    P.diagnose(accessor->getLoc(), diagnostic, /*willSet*/ 0, args...);
    flagInvalidAccessor(accessor);
  }
  if (auto &accessor = accessors.DidSet) {
    P.diagnose(accessor->getLoc(), diagnostic, /*didSet*/ 1, args...);
    flagInvalidAccessor(accessor);
  }
}

void Parser::ParsedAccessors::classify(Parser &P, AbstractStorageDecl *storage,
                                       bool invalid) {
  // If there was a problem parsing accessors, mark all parsed accessors
  // as invalid to avoid tripping up later invariants.
  // We also want to avoid diagnose missing accessors if something
  // was invalid.
  if (invalid) {
    for (auto accessor : Accessors) {
      flagInvalidAccessor(accessor);
    }
  }

  // The observing accessors have very specific restrictions.
  // Prefer to ignore them.
  if (WillSet || DidSet) {
    // For now, we don't support the observing accessors on subscripts.
    if (isa<SubscriptDecl>(storage)) {
      diagnoseAndIgnoreObservers(P, *this,
                                 diag::observing_accessor_in_subscript);

    // The observing accessors cannot be combined with other accessors.
    } else if (auto nonObserver = findFirstNonObserver()) {
      diagnoseAndIgnoreObservers(P, *this,
                   diag::observing_accessor_conflicts_with_accessor,
                   getAccessorNameForDiagnostic(nonObserver, /*article*/ true));
    }
  }

  // Okay, observers are out of the way.

  // 'get', 'read', and a non-mutable addressor are all exclusive.
  if (Get) {
    diagnoseConflictingAccessors(P, Get, Read);
    diagnoseConflictingAccessors(P, Get, Address);
  } else if (Read) {
    diagnoseConflictingAccessors(P, Read, Address);
  } else if (Address) {
    // Nothing can go wrong.

  // If there's a writing accessor of any sort, there must also be a
  // reading accessor.
  } else if (auto mutator = findFirstMutator()) {
    if (!invalid) {
      P.diagnose(mutator->getLoc(),
                 // Don't mention the more advanced accessors if the user
                 // only provided a setter without a getter.
                 (MutableAddress || Modify)
                   ? diag::missing_reading_accessor
                   : diag::missing_getter,
                 isa<SubscriptDecl>(storage),
                 getAccessorNameForDiagnostic(mutator, /*article*/ true));
    }

  // Subscripts always have to have some sort of accessor; they can't be
  // purely stored.
  } else if (isa<SubscriptDecl>(storage)) {
    if (!invalid) {
      P.diagnose(LBLoc, diag::subscript_without_get);
    }
  }

  // A mutable addressor is exclusive with 'set' and 'modify', but
  // 'set' and 'modify' can appear together.
  if (Set) {
    diagnoseConflictingAccessors(P, Set, MutableAddress);
  } else if (Modify) {
    diagnoseConflictingAccessors(P, Modify, MutableAddress);
  }
}


/// Parse a 'var' or 'let' declaration, doing no token skipping on error.
ParserResult<PatternBindingDecl>
Parser::parseDeclVar(ParseDeclOptions Flags,
                     DeclAttributes &Attributes,
                     SmallVectorImpl<Decl *> &Decls,
                     SourceLoc StaticLoc,
                     StaticSpellingKind StaticSpelling,
                     SourceLoc TryLoc,
                     bool HasLetOrVarKeyword) {
  assert(StaticLoc.isInvalid() || StaticSpelling != StaticSpellingKind::None);

  if (StaticLoc.isValid()) {
    if (!Flags.contains(PD_HasContainerType)) {
      diagnose(Tok, diag::static_var_decl_global_scope, StaticSpelling)
          .fixItRemove(StaticLoc);
      StaticLoc = SourceLoc();
      StaticSpelling = StaticSpellingKind::None;
    } else if (Flags.contains(PD_InStruct) || Flags.contains(PD_InEnum) ||
               Flags.contains(PD_InProtocol)) {
      if (StaticSpelling == StaticSpellingKind::KeywordClass)
        diagnose(Tok, diag::class_var_not_in_class, 
                 Flags.contains(PD_InProtocol))
            .fixItReplace(StaticLoc, "static");
    }
  }

  bool isLet = HasLetOrVarKeyword && Tok.is(tok::kw_let);
  assert(!HasLetOrVarKeyword || Tok.getKind() == tok::kw_let ||
         Tok.getKind() == tok::kw_var);

  SourceLoc VarLoc = HasLetOrVarKeyword ? consumeToken() : Tok.getLoc();

  // If this is a var in the top-level of script/repl source file, wrap the
  // PatternBindingDecl in a TopLevelCodeDecl, since it represents executable
  // code.  The VarDecl and any accessor decls (for computed properties) go in
  // CurDeclContext.
  //
  TopLevelCodeDecl *topLevelDecl = nullptr;
  if (allowTopLevelCode() && CurDeclContext->isModuleScopeContext()) {
    // The body of topLevelDecl will get set later.
    topLevelDecl = new (Context) TopLevelCodeDecl(CurDeclContext);
  }

  bool HasAccessors = false;  // Syntactically has accessor {}'s.
  ParserStatus Status;

  unsigned NumDeclsInResult = Decls.size();
  
  // In var/let decl with multiple patterns, accumulate them all in this list
  // so we can build our singular PatternBindingDecl at the end.
  SmallVector<PatternBindingEntry, 4> PBDEntries;
  auto BaseContext = CurDeclContext;

  // No matter what error path we take, make sure the
  // PatternBindingDecl/TopLevel code block are added.
  auto makeResult =
    [&](ParserStatus Status) -> ParserResult<PatternBindingDecl> {

    // If we didn't parse any patterns, don't create the pattern binding decl.
    if (PBDEntries.empty())
      return Status;
    
    // Now that we've parsed all of our patterns, initializers and accessors, we
    // can finally create our PatternBindingDecl to represent the
    // pattern/initializer pairs.
    auto PBD = PatternBindingDecl::create(Context, StaticLoc, StaticSpelling,
                                          VarLoc, PBDEntries, BaseContext);

    // Wire up any initializer contexts we needed.
    for (unsigned i : indices(PBDEntries)) {
      if (auto initContext = PBDEntries[i].getInitContext())
        cast<PatternBindingInitializer>(initContext)->setBinding(PBD, i);
    }

    // If we're setting up a TopLevelCodeDecl, configure it by setting up the
    // body that holds PBD and we're done.  The TopLevelCodeDecl is already set
    // up in Decls to be returned to caller.
    if (topLevelDecl) {
      PBD->setDeclContext(topLevelDecl);
      auto range = PBD->getSourceRange();
      topLevelDecl->setBody(BraceStmt::create(Context, range.Start,
                                              ASTNode(PBD), range.End, true));
      Decls.insert(Decls.begin()+NumDeclsInResult, topLevelDecl);
      return makeParserResult(Status, PBD);
    }

    // Otherwise return the PBD in "Decls" to the caller.  We add it at a
    // specific spot to get it in before any accessors, which SILGen seems to
    // want.
    Decls.insert(Decls.begin()+NumDeclsInResult, PBD);

    // Always return the result for PBD.
    return makeParserResult(Status, PBD);
  };
  SyntaxParsingContext PBListCtx(SyntaxContext, SyntaxKind::PatternBindingList);
  bool HasNext;
  do {
    SyntaxParsingContext PatternBindingCtx(SyntaxContext,
                                           SyntaxKind::PatternBinding);
    Pattern *pattern;
    {
      // In our recursive parse, remember that we're in a var/let pattern.
      llvm::SaveAndRestore<decltype(InVarOrLetPattern)>
      T(InVarOrLetPattern, isLet ? IVOLP_InLet : IVOLP_InVar);

      auto patternRes = parseTypedPattern();
      if (patternRes.hasCodeCompletion())
        return makeResult(makeParserCodeCompletionStatus());
      if (patternRes.isNull())
        return makeResult(makeParserError());

      pattern = patternRes.get();
    }
    
    bool hasOpaqueReturnTy = false;
    if (auto typedPattern = dyn_cast<TypedPattern>(pattern)) {
      hasOpaqueReturnTy =
                        isa<OpaqueReturnTypeRepr>(typedPattern->getTypeRepr());
    }
    auto sf = CurDeclContext->getParentSourceFile();
    
    // Configure all vars with attributes, 'static' and parent pattern.
    pattern->forEachVariable([&](VarDecl *VD) {
      VD->setStatic(StaticLoc.isValid());
      VD->getAttrs() = Attributes;
      setLocalDiscriminator(VD);
      Decls.push_back(VD);
      if (hasOpaqueReturnTy && sf) {
        sf->addUnvalidatedDeclWithOpaqueResultType(VD);
      }
    });

    // Check whether we have already established an initializer context.
    PatternBindingInitializer *initContext =
      findAttributeInitContent(Attributes);

    // Remember this pattern/init pair for our ultimate PatternBindingDecl. The
    // Initializer will be added later when/if it is parsed.
    PBDEntries.push_back({pattern, /*EqualLoc*/ SourceLoc(), /*Init*/ nullptr,
                          initContext});

    Expr *PatternInit = nullptr;
    
    // Parse an initializer if present.
    if (Tok.is(tok::equal)) {
      SyntaxParsingContext InitCtx(SyntaxContext, SyntaxKind::InitializerClause);
      // If we're not in a local context, we'll need a context to parse initializers
      // into (should we have one).  This happens for properties and global
      // variables in libraries.

      // Record the variables that we're trying to initialize.  This allows us
      // to cleanly reject "var x = x" when "x" isn't bound to an enclosing
      // decl (even though names aren't injected into scope when the initializer
      // is parsed).
      SmallVector<VarDecl *, 4> Vars;
      Vars.append(DisabledVars.begin(), DisabledVars.end());
      pattern->collectVariables(Vars);
      
      llvm::SaveAndRestore<decltype(DisabledVars)>
      RestoreCurVars(DisabledVars, Vars);

      llvm::SaveAndRestore<decltype(DisabledVarReason)>
      RestoreReason(DisabledVarReason, diag::var_init_self_referential);
      
      // If we have no local context to parse the initial value into, create one
      // for the PBD we'll eventually create.  This allows us to have reasonable
      // DeclContexts for any closures that may live inside of initializers.
      if (!CurDeclContext->isLocalContext() && !topLevelDecl && !initContext)
        initContext = new (Context) PatternBindingInitializer(CurDeclContext);

      // If we're using a local context (either a TopLevelCodeDecl or a
      // PatternBindingContext) install it now so that CurDeclContext is set
      // right when parsing the initializer.
      Optional<ParseFunctionBody> initParser;
      Optional<ContextChange> topLevelParser;
      if (topLevelDecl)
        topLevelParser.emplace(*this, topLevelDecl,
                               &State->getTopLevelContext());
      if (initContext)
        initParser.emplace(*this, initContext);

      
      SourceLoc EqualLoc = consumeToken(tok::equal);
      PBDEntries.back().setEqualLoc(EqualLoc);

      ParserResult<Expr> init = parseExpr(diag::expected_init_value);
      PBDEntries.back().setOriginalInit(init.getPtrOrNull());

      // If this Pattern binding was not supposed to have an initializer, but it
      // did, diagnose this and remove it.
      if (Flags & PD_DisallowInit && init.isNonNull()) {
        diagnose(EqualLoc, diag::disallowed_init);
        init = nullptr;
      }
      
      // Otherwise, if this pattern binding *was* supposed (or allowed) to have
      // an initializer, but it was a parse error, replace it with ErrorExpr so
      // that downstream clients know that it was present (well, at least the =
      // was present).  This silences downstream diagnostics checking to make
      // sure that some PBD's that require initializers actually had them.
      if (!(Flags & PD_DisallowInit) && init.isNull())
        init = makeParserResult(init, new (Context) ErrorExpr(EqualLoc));
      
      
      // Remember this init for the PatternBindingDecl.
      PatternInit = init.getPtrOrNull();
      PBDEntries.back().setInit(PatternInit);

      // If we set up an initialization context for a property or module-level
      // global, record it.
      PBDEntries.back().setInitContext(initContext);

      if (init.hasCodeCompletion()) {
        Status |= init;
        // If we are doing second pass of code completion, we don't want to
        // suddenly cut off parsing and throw away the declaration.
        if (isCodeCompletionFirstPass())
          return makeResult(makeParserCodeCompletionStatus());
      }

      if (init.isNull())
        return makeResult(makeParserError());
    }
    
    // If we syntactically match the second decl-var production, with a
    // var-get-set clause, parse the var-get-set clause.
    if (Tok.is(tok::l_brace)) {
      HasAccessors = true;
      auto boundVar =
          parseDeclVarGetSet(pattern, Flags, StaticLoc, StaticSpelling, VarLoc,
                             PatternInit != nullptr, Attributes, Decls);
      if (boundVar.hasCodeCompletion())
        return makeResult(makeParserCodeCompletionStatus());
    }
    
    // Add all parsed vardecls to this scope.
    addPatternVariablesToScope(pattern);
    
    // Propagate back types for simple patterns, like "var A, B : T".
    if (auto *TP = dyn_cast<TypedPattern>(pattern)) {
      if (isa<NamedPattern>(TP->getSubPattern()) && PatternInit == nullptr) {
        for (unsigned i = PBDEntries.size() - 1; i != 0; --i) {
          Pattern *PrevPat = PBDEntries[i-1].getPattern();
          if (!isa<NamedPattern>(PrevPat) || PBDEntries[i-1].getInit())
            break;
          if (HasAccessors) {
            // FIXME -- offer a fixit to explicitly specify the type
            diagnose(PrevPat->getLoc(), diag::getset_cannot_be_implied);
            Status.setIsParseError();
          }

          TypedPattern *NewTP = new (Context) TypedPattern(PrevPat,
                                                           TP->getTypeRepr());
          NewTP->setPropagatedType();
          PBDEntries[i-1].setPattern(NewTP);
        }
      }
    }
     HasNext = consumeIf(tok::comma);
  } while (HasNext);

  if (HasAccessors && PBDEntries.size() > 1) {
    diagnose(VarLoc, diag::disallowed_var_multiple_getset);
    Status.setIsParseError();
  }

  if (TryLoc.isValid()) {
    auto inFlightDiag = diagnose(TryLoc, diag::try_on_var_let);

    if (PBDEntries.size() == 1 && PBDEntries.front().getInit() &&
        !isa<ErrorExpr>(PBDEntries.front().getInit())) {
      auto *init = PBDEntries.front().getInit();
      inFlightDiag.fixItRemoveChars(TryLoc, VarLoc);
      inFlightDiag.fixItInsert(init->getStartLoc(), "try ");

      // Note: We can't use TryLoc here because it's outside the PBD source
      // range.
      PBDEntries.front().setInit(new (Context) TryExpr(init->getStartLoc(),
                                                       init));
    }
  }

  return makeResult(Status);
}

void Parser::consumeAbstractFunctionBody(AbstractFunctionDecl *AFD,
                                         const DeclAttributes &Attrs) {
  auto BeginParserPosition = getParserPosition();
  SourceRange BodyRange;
  BodyRange.Start = Tok.getLoc();

  // Consume the '{', and find the matching '}'.
  unsigned OpenBraces = skipBracedBlock(*this, SyntaxContext);
  if (OpenBraces != 0 && Tok.isNot(tok::code_complete)) {
    assert(Tok.is(tok::eof));
    // We hit EOF, and not every brace has a pair.  Recover by searching
    // for the next decl except variable decls and cutting off before
    // that point.
    backtrackToPosition(BeginParserPosition);
    consumeToken(tok::l_brace);
    while (Tok.is(tok::kw_var) || Tok.is(tok::kw_let) ||
           (Tok.isNot(tok::eof) && !isStartOfDecl())) {
      consumeToken();
    }
  }

  BodyRange.End = PreviousLoc;

  if (DelayedParseCB &&
      DelayedParseCB->shouldDelayFunctionBodyParsing(*this, AFD, Attrs,
                                                     BodyRange)) {
    State->delayFunctionBodyParsing(AFD, BodyRange,
                                    BeginParserPosition.PreviousLoc);
    AFD->setBodyDelayed(BodyRange);
  } else {
    AFD->setBodySkipped(BodyRange);
  }
}

/// Parse a 'func' declaration, returning null on error.  The caller
/// handles this case and does recovery as appropriate.
///
/// \verbatim
///   decl-func:
///     attribute-list? ('static' | 'class')? 'mutating'? 'func' 
///               any-identifier generic-params? func-signature where-clause?
///               stmt-brace?
/// \endverbatim
///
/// \note The caller of this method must ensure that the next token is 'func'.
ParserResult<FuncDecl> Parser::parseDeclFunc(SourceLoc StaticLoc,
                                             StaticSpellingKind StaticSpelling,
                                             ParseDeclOptions Flags,
                                             DeclAttributes &Attributes,
                                             bool HasFuncKeyword) {
  assert(StaticLoc.isInvalid() || StaticSpelling != StaticSpellingKind::None);

  if (StaticLoc.isValid()) {
    if (!Flags.contains(PD_HasContainerType)) {
      // Reject static functions at global scope.
      diagnose(Tok, diag::static_func_decl_global_scope, StaticSpelling)
          .fixItRemove(StaticLoc);
      StaticLoc = SourceLoc();
      StaticSpelling = StaticSpellingKind::None;
    } else if (Flags.contains(PD_InStruct) || Flags.contains(PD_InEnum) ||
               Flags.contains(PD_InProtocol)) {
      if (StaticSpelling == StaticSpellingKind::KeywordClass) {
        diagnose(Tok, diag::class_func_not_in_class,
                 Flags.contains(PD_InProtocol))
            .fixItReplace(StaticLoc, "static");

        StaticSpelling = StaticSpellingKind::KeywordStatic;
      }
    }
  }

  SourceLoc FuncLoc =
      HasFuncKeyword ? consumeToken(tok::kw_func) : Tok.getLoc();

  // Parse function name.
  Identifier SimpleName;
  SourceLoc NameLoc;
  if (Tok.isAnyOperator() || Tok.isAny(tok::exclaim_postfix, tok::amp_prefix)) {
    // If the name is an operator token that ends in '<' and the following token
    // is an identifier, split the '<' off as a separate token. This allows
    // things like 'func ==<T>(x:T, y:T) {}' to parse as '==' with generic type
    // variable '<T>' as expected.
    auto NameStr = Tok.getText();
    if (NameStr.size() > 1 && NameStr.back() == '<' &&
        peekToken().is(tok::identifier)) {
      NameStr = NameStr.slice(0, NameStr.size() - 1);
    }
    SimpleName = Context.getIdentifier(NameStr);
    NameLoc = consumeStartingCharacterOfCurrentToken(tok::oper_binary_spaced,
                                                     NameStr.size());
    // Within a protocol, recover from a missing 'static'.
    if (Flags & PD_InProtocol) {
      switch (StaticSpelling) {
      case StaticSpellingKind::None: {
        diagnose(NameLoc, diag::operator_static_in_protocol, SimpleName.str())
            .fixItInsert(FuncLoc, "static ");
        StaticSpelling = StaticSpellingKind::KeywordStatic;
        break;
      }

      case StaticSpellingKind::KeywordStatic:
        // Okay, this is correct.
        break;

      case StaticSpellingKind::KeywordClass:
        llvm_unreachable("should have been fixed above");
      }
    }
  } else {
    // This non-operator path is quite accepting of what tokens might be a name,
    // because we're aggressive about recovering/providing good diagnostics for
    // beginners.
    auto NameStatus = parseIdentifierDeclName(
        *this, SimpleName, NameLoc, "function", tok::l_paren, tok::arrow,
        tok::l_brace, TokenProperty::StartsWithLess);
    if (NameStatus.isError())
      return nullptr;
  }

  DebuggerContextChange DCC(*this, SimpleName, DeclKind::Func);
  
  // Parse the generic-params, if present.
  Optional<Scope> GenericsScope;
  GenericsScope.emplace(this, ScopeKind::Generics);
  GenericParamList *GenericParams;
  bool SignatureHasCodeCompletion = false;
  auto GenericParamResult = maybeParseGenericParams();
  GenericParams = GenericParamResult.getPtrOrNull();
  SignatureHasCodeCompletion |= GenericParamResult.hasCodeCompletion();
  if (SignatureHasCodeCompletion && !CodeCompletion)
    return makeParserCodeCompletionStatus();

  DefaultArgumentInfo DefaultArgs;
  TypeRepr *FuncRetTy = nullptr;
  DeclName FullName;
  ParameterList *BodyParams;
  SourceLoc throwsLoc;
  bool rethrows;
  ParserStatus SignatureStatus =
      parseFunctionSignature(SimpleName, FullName, BodyParams, DefaultArgs,
                             throwsLoc, rethrows, FuncRetTy);

  SignatureHasCodeCompletion |= SignatureStatus.hasCodeCompletion();
  if (SignatureStatus.hasCodeCompletion() && !CodeCompletion) {
    // Trigger delayed parsing, no need to continue.
    return SignatureStatus;
  }

  diagnoseWhereClauseInGenericParamList(GenericParams);

  // Create the decl for the func and add it to the parent scope.
  auto *FD = FuncDecl::create(Context, StaticLoc, StaticSpelling,
                              FuncLoc, FullName, NameLoc,
                              /*Throws=*/throwsLoc.isValid(), throwsLoc,
                              /*GenericParams=*/nullptr,
                              BodyParams, FuncRetTy,
                              CurDeclContext);

  // Let the source file track the opaque return type mapping, if any.
  if (FuncRetTy && isa<OpaqueReturnTypeRepr>(FuncRetTy)) {
    if (auto sf = CurDeclContext->getParentSourceFile()) {
      sf->addUnvalidatedDeclWithOpaqueResultType(FD);
    }
  }
  
  // Parse a 'where' clause if present, adding it to our GenericParamList.
  if (Tok.is(tok::kw_where)) {
    ContextChange CC(*this, FD);

    auto whereStatus = parseFreestandingGenericWhereClause(GenericParams);
    SignatureHasCodeCompletion |= whereStatus.hasCodeCompletion();
    if (whereStatus.hasCodeCompletion() && !CodeCompletion) {
      // Trigger delayed parsing, no need to continue.
      return whereStatus;
    }
  }

  FD->setGenericParams(GenericParams);
  
  // Protocol method arguments may not have default values.
  if (Flags.contains(PD_InProtocol) && DefaultArgs.HasDefaultArgument) {
    diagnose(FuncLoc, diag::protocol_method_argument_init);
    return nullptr;
  }

  // Add the 'rethrows' attribute.
  if (rethrows) {
    Attributes.add(new (Context) RethrowsAttr(throwsLoc));
  }

  diagnoseOperatorFixityAttributes(*this, Attributes, FD);
  // Add the attributes here so if we need them while parsing the body
  // they are available.
  FD->getAttrs() = Attributes;

  // Pass the function signature to code completion.
  if (SignatureHasCodeCompletion)
    CodeCompletion->setParsedDecl(FD);

  DefaultArgs.setFunctionContext(FD, FD->getParameters());
  setLocalDiscriminator(FD);

  if (Flags.contains(PD_InProtocol)) {
    if (Tok.is(tok::l_brace)) {
      diagnose(Tok, diag::protocol_method_with_body);
      skipSingle();
    }
  } else {
    parseAbstractFunctionBody(FD);
  }

  // Exit the scope introduced for the generic parameters.
  GenericsScope.reset();

  addToScope(FD);
  return DCC.fixupParserResult(FD);
}

/// Parse function body into \p AFD.
void Parser::parseAbstractFunctionBody(AbstractFunctionDecl *AFD) {
  Scope S(this, ScopeKind::FunctionBody);

  // Enter the arguments for the function into a new function-body scope.  We
  // need this even if there is no function body to detect argument name
  // duplication.
  if (auto *P = AFD->getImplicitSelfDecl())
    addToScope(P);
  addParametersToScope(AFD->getParameters());

   // Establish the new context.
  ParseFunctionBody CC(*this, AFD);
  setLocalDiscriminatorToParamList(AFD->getParameters());

  if (!Tok.is(tok::l_brace)) {
    checkForInputIncomplete();
    return;
  }

  if (IsParsingInterfaceTokens) {
    // Record the curly braces but nothing inside.
    SF.recordInterfaceToken("{");
    SF.recordInterfaceToken("}");
  }
  llvm::SaveAndRestore<bool> T(IsParsingInterfaceTokens, false);

  if (isDelayedParsingEnabled()) {
    consumeAbstractFunctionBody(AFD, AFD->getAttrs());
    return;
  }

  if (Context.Stats)
    Context.Stats->getFrontendCounters().NumFunctionsParsed++;

  ParserResult<BraceStmt> Body = parseBraceItemList(diag::invalid_diagnostic);
  if (!Body.isNull()) {
    BraceStmt * BS = Body.get();
    AFD->setBodyParsed(BS);

    // If the body consists of a single expression, turn it into a return
    // statement.
    //
    // But don't do this transformation during code completion, as the source
    // may be incomplete and the type mismatch in return statement will just
    // confuse the type checker.
    if (!Body.hasCodeCompletion() && BS->getNumElements() == 1) {
      auto Element = BS->getElement(0);
      if (auto *stmt = Element.dyn_cast<Stmt *>()) {
        if (isa<FuncDecl>(AFD)) {
          if (auto *returnStmt = dyn_cast<ReturnStmt>(stmt)) {
            if (!returnStmt->hasResult()) {
              auto returnExpr = TupleExpr::createEmpty(Context,
                                                       SourceLoc(),
                                                       SourceLoc(),
                                                       /*implicit*/true);
              returnStmt->setResult(returnExpr);
              AFD->setHasSingleExpressionBody();
              AFD->setSingleExpressionBody(returnExpr);
            }
          }
        }
      } else if (auto *E = Element.dyn_cast<Expr *>()) {
        if (auto SE = dyn_cast<SequenceExpr>(E->getSemanticsProvidingExpr())) {
          if (SE->getNumElements() > 1 && isa<AssignExpr>(SE->getElement(1))) {
            // This is an assignment.  We don't want to implicitly return 
            // it.
            return;
          }
        }
        if (auto F = dyn_cast<FuncDecl>(AFD)) {
          auto RS = new (Context) ReturnStmt(SourceLoc(), E);
          BS->setElement(0, RS); 
          AFD->setHasSingleExpressionBody();
          AFD->setSingleExpressionBody(E);
        } else if (auto *F = dyn_cast<ConstructorDecl>(AFD)) {
          if (F->getFailability() != OTK_None && isa<NilLiteralExpr>(E)) {
            // If it's a nil literal, just insert return.  This is the only 
            // legal thing to return.
            auto RS = new (Context) ReturnStmt(E->getStartLoc(), E);
            BS->setElement(0, RS); 
            AFD->setHasSingleExpressionBody();
            AFD->setSingleExpressionBody(E);
          }
        }
      }
    }
  }
}

bool Parser::parseAbstractFunctionBodyDelayed(AbstractFunctionDecl *AFD) {
  assert(!AFD->getBody() && "function should not have a parsed body");
  assert(AFD->getBodyKind() == AbstractFunctionDecl::BodyKind::Unparsed &&
         "function body should be delayed");

  auto FunctionParserState = State->takeFunctionBodyState(AFD);
  assert(FunctionParserState.get() && "should have a valid state");

  auto BeginParserPosition = getParserPosition(FunctionParserState->BodyPos);
  auto EndLexerState = L->getStateForEndOfTokenLoc(AFD->getEndLoc());

  // ParserPositionRAII needs a primed parser to restore to.
  if (Tok.is(tok::NUM_TOKENS))
    consumeTokenWithoutFeedingReceiver();

  // Ensure that we restore the parser state at exit.
  ParserPositionRAII PPR(*this);

  // Create a lexer that cannot go past the end state.
  Lexer LocalLex(*L, BeginParserPosition.LS, EndLexerState);

  // Temporarily swap out the parser's current lexer with our new one.
  llvm::SaveAndRestore<Lexer *> T(L, &LocalLex);

  // Rewind to '{' of the function body.
  restoreParserPosition(BeginParserPosition);

  // Re-enter the lexical scope.
  Scope S(this, FunctionParserState->takeScope());
  ParseFunctionBody CC(*this, AFD);
  setLocalDiscriminatorToParamList(AFD->getParameters());

  ParserResult<BraceStmt> Body =
      parseBraceItemList(diag::func_decl_without_brace);
  if (Body.isNull()) {
    // FIXME: Should do some sort of error recovery here?
    return true;
  } else {
    AFD->setBodyParsed(Body.get());
  }

  return false;
}

/// Parse a 'enum' declaration, returning true (and doing no token
/// skipping) on error.
///
/// \verbatim
///   decl-enum:
///      'enum' attribute-list identifier generic-params? inheritance?
///          where-clause? '{' decl-enum-body '}'
///   decl-enum-body:
///      decl*
/// \endverbatim
ParserResult<EnumDecl> Parser::parseDeclEnum(ParseDeclOptions Flags,
                                             DeclAttributes &Attributes) {
  SourceLoc EnumLoc = consumeToken(tok::kw_enum);

  Identifier EnumName;
  SourceLoc EnumNameLoc;
  ParserStatus Status;

  Status |= parseIdentifierDeclName(*this, EnumName, EnumNameLoc, "enum",
                                    tok::colon, tok::l_brace,
                                    TokenProperty::StartsWithLess);
  if (Status.isError())
    return nullptr;

  DebuggerContextChange DCC(*this, EnumName, DeclKind::Enum);
  
  // Parse the generic-params, if present.
  GenericParamList *GenericParams = nullptr;
  {
    Scope S(this, ScopeKind::Generics);
    auto Result = maybeParseGenericParams();
    GenericParams = Result.getPtrOrNull();
    if (Result.hasCodeCompletion())
      return makeParserCodeCompletionStatus();
  }

  EnumDecl *ED = new (Context) EnumDecl(EnumLoc, EnumName, EnumNameLoc,
                                        { }, nullptr, CurDeclContext);
  setLocalDiscriminator(ED);
  ED->getAttrs() = Attributes;

  ContextChange CC(*this, ED);

  // Parse optional inheritance clause within the context of the enum.
  if (Tok.is(tok::colon)) {
    SmallVector<TypeLoc, 2> Inherited;
    Status |= parseInheritance(Inherited,
                               /*allowClassRequirement=*/false,
                               /*allowAnyObject=*/false);
    ED->setInherited(Context.AllocateCopy(Inherited));
  }

  diagnoseWhereClauseInGenericParamList(GenericParams);
  
  // Parse a 'where' clause if present, adding it to our GenericParamList.
  if (Tok.is(tok::kw_where)) {
    auto whereStatus = parseFreestandingGenericWhereClause(GenericParams);
    Status |= whereStatus;
    if (whereStatus.hasCodeCompletion() && !CodeCompletion) {
      // Trigger delayed parsing, no need to continue.
      return whereStatus;
    }
  }

  ED->setGenericParams(GenericParams);

  SyntaxParsingContext BlockContext(SyntaxContext, SyntaxKind::MemberDeclBlock);
  SourceLoc LBLoc, RBLoc;
  SourceLoc PosBeforeLB = Tok.getLoc();
  if (parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_enum)) {
    LBLoc = PreviousLoc;
    RBLoc = LBLoc;
    Status.setIsParseError();
  } else {
    Scope S(this, ScopeKind::EnumBody);
    ParseDeclOptions Options(PD_HasContainerType | PD_AllowEnumElement | PD_InEnum);
    if (canDelayMemberDeclParsing()) {
      if (delayParsingDeclList(LBLoc, RBLoc, PosBeforeLB, Options, ED))
        Status.setIsParseError();
    } else {
      if (parseDeclList(LBLoc, RBLoc, diag::expected_rbrace_enum,
                        Options, ED))
        Status.setIsParseError();
    }
  }

  ED->setBraces({LBLoc, RBLoc});

  addToScope(ED);

  return DCC.fixupParserResult(Status, ED);
}

/// Parse a 'case' of an enum.
///
/// \verbatim
///   enum-case:
///      identifier type-tuple?
///   decl-enum-element:
///      'case' attribute-list enum-case (',' enum-case)*
/// \endverbatim
ParserResult<EnumCaseDecl>
Parser::parseDeclEnumCase(ParseDeclOptions Flags,
                          DeclAttributes &Attributes,
                          llvm::SmallVectorImpl<Decl *> &Decls) {
  ParserStatus Status;
  SourceLoc CaseLoc = consumeToken(tok::kw_case);

  // Parse comma-separated enum elements.
  SmallVector<EnumElementDecl*, 4> Elements;
  
  SourceLoc CommaLoc;
  for (;;) {
    SyntaxParsingContext ElementContext(SyntaxContext,
                                        SyntaxKind::EnumCaseElement);
    Identifier Name;
    SourceLoc NameLoc;

    // Consume an extraneous '.' so we can recover the case name.
    SourceLoc DotLoc;
    consumeIf(tok::period_prefix, DotLoc);

    // Handle the likely case someone typed 'case X, case Y'.
    if (Tok.is(tok::kw_case) && CommaLoc.isValid()) {
      diagnose(Tok, diag::expected_identifier_after_case_comma);
      Status.setIsParseError();
      return Status;
    }

    if (Tok.is(tok::identifier)) {
      Status |= parseIdentifierDeclName(*this, Name, NameLoc, "enum 'case'",
                                        tok::l_paren, tok::kw_case, tok::colon,
                                        tok::r_brace);
      assert(Status.isSuccess());
      if (DotLoc.isValid())
        diagnose(DotLoc, diag::enum_case_dot_prefix)
          .fixItRemove(DotLoc);
    } else {
      NameLoc = CaseLoc;
      bool NameIsKeyword = Tok.isKeyword();
      SourceLoc TokLoc = Tok.getLoc();
      StringRef TokText = Tok.getText();

      // For recovery, see if the user typed something resembling a switch
      // "case" label.
      llvm::SaveAndRestore<decltype(InVarOrLetPattern)>
      T(InVarOrLetPattern, Parser::IVOLP_InMatchingPattern);
      parseMatchingPattern(/*isExprBasic*/false);

      if (consumeIf(tok::colon)) {
        diagnose(CaseLoc, diag::case_outside_of_switch, "case");
        Status.setIsParseError();
        return Status;
      }
      if (CommaLoc.isValid()) {
        diagnose(Tok, diag::expected_identifier_after_case_comma);
        Status.setIsParseError();
        return Status;
      }
      if (NameIsKeyword) {
        diagnose(TokLoc, diag::keyword_cant_be_identifier, TokText);
        diagnose(TokLoc, diag::backticks_to_escape)
          .fixItReplace(TokLoc, "`" + TokText.str() + "`");
      } else {
        diagnose(CaseLoc, diag::expected_identifier_in_decl, "enum 'case'");
      }
    }

    // See if there's a following argument type.
    ParserResult<ParameterList> ArgParams;
    SmallVector<Identifier, 4> argumentNames;
    DefaultArgumentInfo DefaultArgs;
    if (Tok.isFollowingLParen()) {
      ArgParams = parseSingleParameterClause(ParameterContextKind::EnumElement,
                                             &argumentNames, &DefaultArgs);
      if (ArgParams.isNull() || ArgParams.hasCodeCompletion())
        return ParserStatus(ArgParams);
    }

    // See if there's a raw value expression.
    SourceLoc EqualsLoc;
    ParserResult<Expr> RawValueExpr;
    LiteralExpr *LiteralRawValueExpr = nullptr;
    if (Tok.is(tok::equal)) {
      SyntaxParsingContext InitContext(SyntaxContext,
                                       SyntaxKind::InitializerClause);

      EqualsLoc = consumeToken();
      {
        CodeCompletionCallbacks::InEnumElementRawValueRAII
            InEnumElementRawValue(CodeCompletion);
        if (!CurLocalContext) {
          // A local context is needed for parsing closures. We want to parse
          // them anyways for proper diagnosis.
          LocalContext tempContext{};
          CurLocalContext = &tempContext;
          RawValueExpr = parseExpr(diag::expected_expr_enum_case_raw_value);
          CurLocalContext = nullptr;
        } else {
          RawValueExpr = parseExpr(diag::expected_expr_enum_case_raw_value);
        }
      }
      if (RawValueExpr.hasCodeCompletion()) {
        Status.setHasCodeCompletion();
        return Status;
      }
      if (RawValueExpr.isNull()) {
        Status.setIsParseError();
        return Status;
      }
      // The raw value must be syntactically a simple literal.
      LiteralRawValueExpr = dyn_cast<LiteralExpr>(RawValueExpr.getPtrOrNull());
      if (!LiteralRawValueExpr
          || isa<InterpolatedStringLiteralExpr>(LiteralRawValueExpr)) {
        diagnose(RawValueExpr.getPtrOrNull()->getLoc(),
                 diag::nonliteral_enum_case_raw_value);
        LiteralRawValueExpr = nullptr;
      }
    }
    
    // For recovery, again make sure the user didn't try to spell a switch
    // case label:
    // 'case Identifier:' or
    // 'case Identifier where ...:'
    if (Tok.is(tok::colon) || Tok.is(tok::kw_where)) {
      diagnose(CaseLoc, diag::case_outside_of_switch, "case");
      skipUntilDeclRBrace();
      Status.setIsParseError();
      return Status;
    }
    
    
    // Create the element.
    DeclName FullName;
    if (ArgParams.isNull()) {
      FullName = Name;
    } else {
      FullName = DeclName(Context, Name, argumentNames);
    }
    auto *result = new (Context) EnumElementDecl(NameLoc, FullName,
                                                 ArgParams.getPtrOrNull(),
                                                 EqualsLoc,
                                                 LiteralRawValueExpr,
                                                 CurDeclContext);

    DefaultArgs.setFunctionContext(result, result->getParameterList());

    if (NameLoc == CaseLoc) {
      result->setImplicit(); // Parse error
    }

    result->getAttrs() = Attributes;
    Elements.push_back(result);
    
    // Continue through the comma-separated list.
    if (!Tok.is(tok::comma))
      break;
    CommaLoc = consumeToken(tok::comma);
  }
  SyntaxContext->collectNodesInPlace(SyntaxKind::EnumCaseElementList);
  
  if (!(Flags & PD_AllowEnumElement)) {
    diagnose(CaseLoc, diag::disallowed_enum_element);
    // Don't add the EnumElementDecls unless the current context
    // is allowed to have EnumElementDecls.
    Status.setIsParseError();
    return Status;
  }

  // Create and insert the EnumCaseDecl containing all the elements.
  auto TheCase = EnumCaseDecl::create(CaseLoc, Elements, CurDeclContext);
  Decls.push_back(TheCase);
  
  // Insert the element decls.
  std::copy(Elements.begin(), Elements.end(), std::back_inserter(Decls));
  return makeParserResult(Status, TheCase);
}

/// Parse a 'struct' declaration, returning true (and doing no token
/// skipping) on error.
///
/// \verbatim
///   decl-struct:
///      'struct' attribute-list identifier generic-params? inheritance?
///          where-clause? '{' decl-struct-body '}
///   decl-struct-body:
///      decl*
/// \endverbatim
ParserResult<StructDecl> Parser::parseDeclStruct(ParseDeclOptions Flags,
                                                 DeclAttributes &Attributes) {
  SourceLoc StructLoc = consumeToken(tok::kw_struct);
  
  Identifier StructName;
  SourceLoc StructNameLoc;
  ParserStatus Status;

  Status |= parseIdentifierDeclName(*this, StructName, StructNameLoc, "struct",
                                    tok::colon, tok::l_brace,
                                    TokenProperty::StartsWithLess);
  if (Status.isError())
    return nullptr;

  DebuggerContextChange DCC (*this, StructName, DeclKind::Struct);
  
  // Parse the generic-params, if present.
  GenericParamList *GenericParams = nullptr;
  {
    Scope S(this, ScopeKind::Generics);
    auto Result = maybeParseGenericParams();
    GenericParams = Result.getPtrOrNull();
    if (Result.hasCodeCompletion())
      return makeParserCodeCompletionStatus();
  }

  StructDecl *SD = new (Context) StructDecl(StructLoc, StructName,
                                            StructNameLoc,
                                            { },
                                            nullptr,
                                            CurDeclContext);
  setLocalDiscriminator(SD);
  SD->getAttrs() = Attributes;

  ContextChange CC(*this, SD);

  // Parse optional inheritance clause within the context of the struct.
  if (Tok.is(tok::colon)) {
    SmallVector<TypeLoc, 2> Inherited;
    Status |= parseInheritance(Inherited,
                               /*allowClassRequirement=*/false,
                               /*allowAnyObject=*/false);
    SD->setInherited(Context.AllocateCopy(Inherited));
  }

  diagnoseWhereClauseInGenericParamList(GenericParams);

  // Parse a 'where' clause if present, adding it to our GenericParamList.
  if (Tok.is(tok::kw_where)) {
    auto whereStatus = parseFreestandingGenericWhereClause(GenericParams);
    Status |= whereStatus;
    if (whereStatus.hasCodeCompletion() && !CodeCompletion) {
      // Trigger delayed parsing, no need to continue.
      return whereStatus;
    }
  }

  SD->setGenericParams(GenericParams);
  // Make the entities of the struct as a code block.
  SyntaxParsingContext BlockContext(SyntaxContext, SyntaxKind::MemberDeclBlock);
  SourceLoc LBLoc, RBLoc;
  SourceLoc PosBeforeLB = Tok.getLoc();
  if (parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_struct)) {
    LBLoc = PreviousLoc;
    RBLoc = LBLoc;
    Status.setIsParseError();
  } else {
    // Parse the body.
    Scope S(this, ScopeKind::StructBody);
    ParseDeclOptions Options(PD_HasContainerType | PD_InStruct);
    if (canDelayMemberDeclParsing()) {
      if (delayParsingDeclList(LBLoc, RBLoc, PosBeforeLB, Options, SD))
        Status.setIsParseError();
    } else {
      if (parseDeclList(LBLoc, RBLoc, diag::expected_rbrace_struct,
                        Options,SD))
        Status.setIsParseError();
    }
  }

  SD->setBraces({LBLoc, RBLoc});

  addToScope(SD);

  return DCC.fixupParserResult(Status, SD);
}

/// Parse a 'class' declaration, doing no token skipping on error.
///
/// \verbatim
///   decl-class:
///      'class' attribute-list identifier generic-params? inheritance?
///          where-clause? '{' decl-class-body '}
///   decl-class-body:
///      decl*
/// \endverbatim
ParserResult<ClassDecl> Parser::parseDeclClass(ParseDeclOptions Flags,
                                               DeclAttributes &Attributes) {
  SourceLoc ClassLoc = consumeToken(tok::kw_class);

  Identifier ClassName;
  SourceLoc ClassNameLoc;
  ParserStatus Status;

  Status |= parseIdentifierDeclName(*this, ClassName, ClassNameLoc, "class",
                                    tok::colon, tok::l_brace,
                                    TokenProperty::StartsWithLess);
  if (Status.isError())
    return nullptr;

  DebuggerContextChange DCC (*this, ClassName, DeclKind::Class);
  
  // Parse the generic-params, if present.
  GenericParamList *GenericParams = nullptr;
  {
    Scope S(this, ScopeKind::Generics);
    auto Result = maybeParseGenericParams();
    GenericParams = Result.getPtrOrNull();
    if (Result.hasCodeCompletion())
      return makeParserCodeCompletionStatus();
  }

  // Create the class.
  ClassDecl *CD = new (Context) ClassDecl(ClassLoc, ClassName, ClassNameLoc,
                                          { }, nullptr, CurDeclContext);
  setLocalDiscriminator(CD);
  CD->getAttrs() = Attributes;

  ContextChange CC(*this, CD);

  // Parse optional inheritance clause within the context of the class.
  if (Tok.is(tok::colon)) {
    SmallVector<TypeLoc, 2> Inherited;
    Status |= parseInheritance(Inherited,
                               /*allowClassRequirement=*/false,
                               /*allowAnyObject=*/false);
    CD->setInherited(Context.AllocateCopy(Inherited));
  
  // Parse python style inheritance clause and replace parentheses with a colon
  } else if (Tok.is(tok::l_paren)) {
    bool isParenStyleInheritance = false;
    {
      BacktrackingScope backtrack(*this);
      consumeToken(tok::l_paren);
      isParenStyleInheritance = canParseType() &&
        Tok.isAny(tok::r_paren, tok::kw_where, tok::l_brace, tok::eof);
    }
    if(isParenStyleInheritance) {
      SourceLoc LParenLoc = consumeToken(tok::l_paren);
      auto TypeResult = parseType();
      if (TypeResult.isNull()) {
        Status.setIsParseError();
        return Status;
      }
      SourceLoc RParenLoc;
      consumeIf(tok::r_paren, RParenLoc);
      diagnose(LParenLoc, diag::expected_colon_class)
        .fixItReplace(LParenLoc, ": ")
        .fixItRemove(RParenLoc);
    }
  } 

  diagnoseWhereClauseInGenericParamList(GenericParams);

  // Parse a 'where' clause if present, adding it to our GenericParamList.
  if (Tok.is(tok::kw_where)) {
    auto whereStatus = parseFreestandingGenericWhereClause(GenericParams);
    Status |= whereStatus;
    if (whereStatus.hasCodeCompletion() && !CodeCompletion) {
      // Trigger delayed parsing, no need to continue.
      return whereStatus;
    }
  }

  CD->setGenericParams(GenericParams);

  SyntaxParsingContext BlockContext(SyntaxContext, SyntaxKind::MemberDeclBlock);
  SourceLoc LBLoc, RBLoc;
  auto PosBeforeLB = Tok.getLoc();
  if (parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_class)) {
    LBLoc = PreviousLoc;
    RBLoc = LBLoc;
    Status.setIsParseError();
  } else {
    // Parse the body.
    Scope S(this, ScopeKind::ClassBody);
    ParseDeclOptions Options(PD_HasContainerType | PD_AllowDestructor |
                             PD_InClass);
    if (canDelayMemberDeclParsing()) {
      if (delayParsingDeclList(LBLoc, RBLoc, PosBeforeLB, Options, CD))
        Status.setIsParseError();
    } else {
      if (parseDeclList(LBLoc, RBLoc, diag::expected_rbrace_class,
                        Options, CD))
        Status.setIsParseError();
    }
  }

  CD->setBraces({LBLoc, RBLoc});

  addToScope(CD);

  return DCC.fixupParserResult(Status, CD);
}

/// Parse a 'protocol' declaration, doing no token skipping on error.
///
/// \verbatim
///   decl-protocol:
///      protocol-head '{' protocol-member* '}'
///
///   protocol-head:
///     'protocol' attribute-list identifier inheritance? 
///
///   protocol-member:
///      decl-func
///      decl-var-simple
///      decl-typealias
/// \endverbatim
ParserResult<ProtocolDecl> Parser::
parseDeclProtocol(ParseDeclOptions Flags, DeclAttributes &Attributes) {
  SourceLoc ProtocolLoc = consumeToken(tok::kw_protocol);
  
  SourceLoc NameLoc;
  Identifier ProtocolName;
  ParserStatus Status;

  Status |= parseIdentifierDeclName(*this, ProtocolName, NameLoc, "protocol",
                                    tok::colon, tok::l_brace);
  if (Status.isError())
    return nullptr;

  // Protocols don't support generic parameters, but people often want them and
  // we want to have good error recovery if they try them out.  Parse them and
  // produce a specific diagnostic if present.
  if (startsWithLess(Tok)) {
    diagnose(Tok, diag::generic_arguments_protocol);
    Scope S(this, ScopeKind::Generics);
    maybeParseGenericParams();
  }

  DebuggerContextChange DCC (*this);
  
  // Parse optional inheritance clause.
  SmallVector<TypeLoc, 4> InheritedProtocols;
  SourceLoc colonLoc;
  if (Tok.is(tok::colon)) {
    colonLoc = Tok.getLoc();
    Status |= parseInheritance(InheritedProtocols,
                               /*allowClassRequirement=*/true,
                               /*allowAnyObject=*/true);
  }

  TrailingWhereClause *TrailingWhere = nullptr;
  // Parse a 'where' clause if present.
  if (Tok.is(tok::kw_where)) {
    auto whereStatus = parseProtocolOrAssociatedTypeWhereClause(
        TrailingWhere, /*isProtocol=*/true);
    if (whereStatus.shouldStopParsing())
      return whereStatus;
  }

  ProtocolDecl *Proto = new (Context)
      ProtocolDecl(CurDeclContext, ProtocolLoc, NameLoc, ProtocolName,
                   Context.AllocateCopy(InheritedProtocols), TrailingWhere);
  // No need to setLocalDiscriminator: protocols can't appear in local contexts.

  Proto->getAttrs() = Attributes;

  ContextChange CC(*this, Proto);
  Scope ProtocolBodyScope(this, ScopeKind::ProtocolBody);

  // Parse the body.
  {
    SyntaxParsingContext BlockContext(SyntaxContext, SyntaxKind::MemberDeclBlock);
    SourceLoc LBraceLoc;
    SourceLoc RBraceLoc;
    SourceLoc PosBeforeLB = Tok.getLoc();
    if (parseToken(tok::l_brace, LBraceLoc, diag::expected_lbrace_protocol)) {
      LBraceLoc = PreviousLoc;
      RBraceLoc = LBraceLoc;
      Status.setIsParseError();
    } else {
      // Parse the members.
      ParseDeclOptions Options(PD_HasContainerType |
                               PD_DisallowInit |
                               PD_InProtocol);
      if (canDelayMemberDeclParsing()) {
        if (delayParsingDeclList(LBraceLoc, RBraceLoc, PosBeforeLB, Options, Proto))
          Status.setIsParseError();
      } else {
        if (parseDeclList(LBraceLoc, RBraceLoc, diag::expected_rbrace_protocol,
                          Options, Proto))
          Status.setIsParseError();
      }
    }

    // Install the protocol elements.
    Proto->setBraces({LBraceLoc, RBraceLoc});
  }
  
  return DCC.fixupParserResult(Status, Proto);
}

/// Parse a 'subscript' declaration.
///
/// \verbatim
///   decl-subscript:
///     subscript-head get-set
///   subscript-head
///     attribute-list? 'subscript' parameter-clause '->' type
/// \endverbatim
ParserResult<SubscriptDecl>
Parser::parseDeclSubscript(SourceLoc StaticLoc,
                           StaticSpellingKind StaticSpelling,
                           ParseDeclOptions Flags,
                           DeclAttributes &Attributes,
                           SmallVectorImpl<Decl *> &Decls) {
  assert(StaticLoc.isInvalid() || StaticSpelling != StaticSpellingKind::None);
  
  if (StaticLoc.isValid()) {
    if (Flags.contains(PD_InStruct) || Flags.contains(PD_InEnum) ||
               Flags.contains(PD_InProtocol)) {
      if (StaticSpelling == StaticSpellingKind::KeywordClass) {
        diagnose(Tok, diag::class_subscript_not_in_class,
                 Flags.contains(PD_InProtocol))
        .fixItReplace(StaticLoc, "static");
        
        StaticSpelling = StaticSpellingKind::KeywordStatic;
      }
    }
  }
  
  ParserStatus Status;
  SourceLoc SubscriptLoc = consumeToken(tok::kw_subscript);

  // Diagnose 'subscript' with name.
  if (Tok.is(tok::identifier) &&
      (peekToken().is(tok::l_paren) || startsWithLess(peekToken()))) {
    diagnose(Tok, diag::subscript_has_name)
      .fixItRemove(Tok.getLoc());
    consumeToken(tok::identifier);
  }

  // Parse the generic-params, if present.
  Optional<Scope> GenericsScope;
  GenericsScope.emplace(this, ScopeKind::Generics);
  GenericParamList *GenericParams;
  bool SignatureHasCodeCompletion = false;

  auto Result = maybeParseGenericParams();
  GenericParams = Result.getPtrOrNull();
  SignatureHasCodeCompletion |= Result.hasCodeCompletion();

  if (SignatureHasCodeCompletion && !CodeCompletion)
    return makeParserCodeCompletionStatus();

  // Parse the parameter list.
  DefaultArgumentInfo DefaultArgs;
  SmallVector<Identifier, 4> argumentNames;
  ParserResult<ParameterList> Indices
    = parseSingleParameterClause(ParameterContextKind::Subscript,
                                 &argumentNames, &DefaultArgs);
  Status |= Indices;

  SignatureHasCodeCompletion |= Indices.hasCodeCompletion();
  if (SignatureHasCodeCompletion && !CodeCompletion)
    return makeParserCodeCompletionStatus();
  
  SourceLoc ArrowLoc;
  ParserResult<TypeRepr> ElementTy;
  {
    SyntaxParsingContext ReturnCtxt(SyntaxContext, SyntaxKind::ReturnClause);

    // '->'
    if (!consumeIf(tok::arrow, ArrowLoc)) {
      if (!Indices.isParseError())
        diagnose(Tok, diag::expected_arrow_subscript);
      Status.setIsParseError();
    }

    if (!ArrowLoc.isValid() &&
        (Indices.isNull() || Indices.get()->size() == 0)) {
      // This doesn't look much like a subscript, so let regular recovery take
      // care of it.
      return Status;
    }

    // type
    ElementTy = parseDeclResultType(diag::expected_type_subscript);
    Status |= ElementTy;
    SignatureHasCodeCompletion |= ElementTy.hasCodeCompletion();
    if (SignatureHasCodeCompletion && !CodeCompletion) {
      return makeParserCodeCompletionStatus();
    }
    if (ElementTy.isNull()) {
      // Always set an element type.
      ElementTy = makeParserResult(ElementTy, new (Context) ErrorTypeRepr());
    }
  }

  diagnoseWhereClauseInGenericParamList(GenericParams);

  // Build an AST for the subscript declaration.
  DeclName name = DeclName(Context, DeclBaseName::createSubscript(),
                           argumentNames);
  auto *Subscript = new (Context) SubscriptDecl(name,
                                                StaticLoc, StaticSpelling,
                                                SubscriptLoc, Indices.get(),
                                                ArrowLoc, ElementTy.get(),
                                                CurDeclContext,
                                                nullptr);
  Subscript->getAttrs() = Attributes;
  
  // Let the source file track the opaque return type mapping, if any.
  if (ElementTy.get() && isa<OpaqueReturnTypeRepr>(ElementTy.get())) {
    if (auto sf = CurDeclContext->getParentSourceFile()) {
      sf->addUnvalidatedDeclWithOpaqueResultType(Subscript);
    }
  }

  DefaultArgs.setFunctionContext(Subscript, Subscript->getIndices());

  // Parse a 'where' clause if present, adding it to our GenericParamList.
  if (Tok.is(tok::kw_where)) {
    ContextChange CC(*this, Subscript);

    auto whereStatus = parseFreestandingGenericWhereClause(GenericParams);
    SignatureHasCodeCompletion |= whereStatus.hasCodeCompletion();
    if (whereStatus.hasCodeCompletion() && !CodeCompletion) {
      // Trigger delayed parsing, no need to continue.
      return whereStatus;
    }
  }

  Subscript->setGenericParams(GenericParams);

  // Pass the function signature to code completion.
  if (SignatureHasCodeCompletion && CodeCompletion) {
    CodeCompletion->setParsedDecl(Subscript);
  }

  Decls.push_back(Subscript);

  // '{'
  // Parse getter and setter.
  ParsedAccessors accessors;
  if (Tok.isNot(tok::l_brace)) {
    // Subscript declarations must always have at least a getter, so they need
    // to be followed by a {.
    if (!Status.isError()) {
      if (Flags.contains(PD_InProtocol)) {
        diagnose(Tok, diag::expected_lbrace_subscript_protocol)
            .fixItInsertAfter(ElementTy.get()->getEndLoc(), " { get <#set#> }");
      } else {
        diagnose(Tok, diag::expected_lbrace_subscript);
      }
      Status.setIsParseError();
    }
  } else {
    Status |= parseGetSet(Flags, GenericParams,
                          Indices.get(), ElementTy.get(),
                          accessors, Subscript, StaticLoc);
  }

  // Now that it's been parsed, set the end location.
  Subscript->setEndLoc(PreviousLoc);

  bool Invalid = false;
  // Reject 'subscript' functions outside of type decls
  if (!(Flags & PD_HasContainerType)) {
    diagnose(SubscriptLoc, diag::subscript_decl_wrong_scope);
    Invalid = true;
  }

  accessors.record(*this, Subscript, (Invalid || !Status.isSuccess()));

  // No need to setLocalDiscriminator because subscripts cannot
  // validly appear outside of type decls.
  return makeParserResult(Status, Subscript);
}

ParserResult<ConstructorDecl>
Parser::parseDeclInit(ParseDeclOptions Flags, DeclAttributes &Attributes) {
  assert(Tok.is(tok::kw_init));
  SourceLoc ConstructorLoc = consumeToken();
  OptionalTypeKind Failability = OTK_None;
  SourceLoc FailabilityLoc;

  const bool ConstructorsNotAllowed = !(Flags & PD_HasContainerType);

  // Reject constructors outside of types.
  if (ConstructorsNotAllowed) {
    diagnose(Tok, diag::initializer_decl_wrong_scope);
  }

  // Parse the '!' or '?' for a failable initializer.
  if (Tok.isAny(tok::exclaim_postfix, tok::sil_exclamation) ||
      (Tok.isAnyOperator() && Tok.getText() == "!")) {
    Failability = OTK_ImplicitlyUnwrappedOptional;
    FailabilityLoc = consumeToken();
  } else if (Tok.isAny(tok::question_postfix, tok::question_infix)) {
    Failability = OTK_Optional;
    FailabilityLoc = consumeToken();
  }

  // Reject named 'init'. e.g. 'init withString(string: str)'.
  if (Tok.is(tok::identifier) &&
      (peekToken().is(tok::l_paren) || startsWithLess(peekToken()))) {
    diagnose(Tok, diag::initializer_has_name)
      .fixItRemove(Tok.getLoc());
    consumeToken(tok::identifier);
  }

  // Parse the generic-params, if present.
  Scope S(this, ScopeKind::Generics);
  auto GPResult = maybeParseGenericParams();
  GenericParamList *GenericParams = GPResult.getPtrOrNull();
  if (GPResult.hasCodeCompletion())
    return makeParserCodeCompletionStatus();

  // Parse the parameters.
  DefaultArgumentInfo DefaultArgs;
  llvm::SmallVector<Identifier, 4> namePieces;
  bool SignatureHasCodeCompletion = false;
  ParserResult<ParameterList> Params
    = parseSingleParameterClause(ParameterContextKind::Initializer,
                                 &namePieces, &DefaultArgs);

  SignatureHasCodeCompletion |= Params.hasCodeCompletion();
  if (Params.hasCodeCompletion() && !CodeCompletion) {
    // Trigger delayed parsing, no need to continue.
    return makeParserCodeCompletionStatus();
  }

  // Protocol initializer arguments may not have default values.
  if (Flags.contains(PD_InProtocol) && DefaultArgs.HasDefaultArgument) {
    diagnose(ConstructorLoc, diag::protocol_init_argument_init);
    return nullptr;
  }

  // Parse 'throws' or 'rethrows'.
  SourceLoc throwsLoc;
  if (consumeIf(tok::kw_throws, throwsLoc)) {
    // okay
  } else if (consumeIf(tok::kw_rethrows, throwsLoc)) {
    Attributes.add(new (Context) RethrowsAttr(throwsLoc));
  }

  diagnoseWhereClauseInGenericParamList(GenericParams);

  DeclName FullName(Context, DeclBaseName::createConstructor(), namePieces);
  auto *CD = new (Context) ConstructorDecl(FullName, ConstructorLoc,
                                           Failability, FailabilityLoc,
                                           throwsLoc.isValid(), throwsLoc,
                                           Params.get(), nullptr,
                                           CurDeclContext);
  CD->getAttrs() = Attributes;

  // Parse a 'where' clause if present, adding it to our GenericParamList.
  if (Tok.is(tok::kw_where)) {
    ContextChange(*this, CD);

    auto whereStatus = parseFreestandingGenericWhereClause(GenericParams);
    SignatureHasCodeCompletion |= whereStatus.hasCodeCompletion();
    if (whereStatus.hasCodeCompletion() && !CodeCompletion) {
      // Trigger delayed parsing, no need to continue.
      return whereStatus;
    }
  }

  CD->setGenericParams(GenericParams);

  // No need to setLocalDiscriminator.

  DefaultArgs.setFunctionContext(CD, CD->getParameters());

  // Pass the function signature to code completion.
  if (SignatureHasCodeCompletion)
    CodeCompletion->setParsedDecl(CD);

  if (ConstructorsNotAllowed || Params.isParseError()) {
    // Tell the type checker not to touch this constructor.
    CD->setInvalid();
  }

  if (Flags.contains(PD_InProtocol)) {
    if (Tok.is(tok::l_brace)) {
      diagnose(Tok, diag::protocol_init_with_body);
      skipSingle();
    }
  } else {
    parseAbstractFunctionBody(CD);
  }

  return makeParserResult(CD);
}

ParserResult<DestructorDecl> Parser::
parseDeclDeinit(ParseDeclOptions Flags, DeclAttributes &Attributes) {
  SourceLoc DestructorLoc = consumeToken(tok::kw_deinit);

  // Parse extraneous parentheses and remove them with a fixit.
  auto skipParameterListIfPresent = [this] {
    SourceLoc LParenLoc;
    if (!consumeIf(tok::l_paren, LParenLoc))
      return;
    SourceLoc RParenLoc;
    skipUntil(tok::r_paren);

    if (Tok.is(tok::r_paren)) {
      SourceLoc RParenLoc = consumeToken();
      diagnose(LParenLoc, diag::destructor_params)
        .fixItRemove(SourceRange(LParenLoc, RParenLoc));
    } else {
      diagnose(Tok, diag::opened_destructor_expected_rparen);
      diagnose(LParenLoc, diag::opening_paren);
    }
  };

  // '{'
  if (!Tok.is(tok::l_brace)) {
    switch (SF.Kind) {
    case SourceFileKind::Interface:
    case SourceFileKind::SIL:
      // It's okay to have no body for SIL code or parseable interfaces.
      break;
    case SourceFileKind::Library:
    case SourceFileKind::Main:
    case SourceFileKind::REPL:
      if (Tok.is(tok::identifier)) {
        diagnose(Tok, diag::destructor_has_name).fixItRemove(Tok.getLoc());
        consumeToken();
      }
      skipParameterListIfPresent();
      if (Tok.is(tok::l_brace))
        break;

      diagnose(Tok, diag::expected_lbrace_destructor);
      return nullptr;
    }
  }

  auto *DD = new (Context) DestructorDecl(DestructorLoc, CurDeclContext);
  parseAbstractFunctionBody(DD);

  DD->getAttrs() = Attributes;

  // Reject 'destructor' functions outside of classes
  if (!(Flags & PD_AllowDestructor)) {
    diagnose(DestructorLoc, diag::destructor_decl_outside_class);

    // Tell the type checker not to touch this destructor.
    DD->setInvalid();
  }

  return makeParserResult(DD);
}

ParserResult<OperatorDecl> 
Parser::parseDeclOperator(ParseDeclOptions Flags, DeclAttributes &Attributes) {
  SourceLoc OperatorLoc = consumeToken(tok::kw_operator);
  bool AllowTopLevel = Flags.contains(PD_AllowTopLevel);

  if (!Tok.isAnyOperator() && !Tok.is(tok::exclaim_postfix)) {
    // A common error is to try to define an operator with something in the
    // unicode plane considered to be an operator, or to try to define an
    // operator like "not".  Diagnose this specifically.
    if (Tok.is(tok::identifier))
      diagnose(Tok, diag::identifier_when_expecting_operator,
               Context.getIdentifier(Tok.getText()));
    else
      diagnose(Tok, diag::expected_operator_name_after_operator);

    // To improve recovery, check to see if we have a { right after this token.
    // If so, swallow until the end } to avoid tripping over the body of the
    // malformed operator decl.
    if (peekToken().is(tok::l_brace)) {
      consumeToken();
      skipSingle();
    }

    return nullptr;
  }

  DebuggerContextChange DCC (*this);

  Identifier Name = Context.getIdentifier(Tok.getText());
  SourceLoc NameLoc = consumeToken();

  if (Attributes.hasAttribute<PostfixAttr>()) {
    if (!Name.empty() && (Name.get()[0] == '?' || Name.get()[0] == '!'))
      diagnose(NameLoc, diag::expected_operator_name_after_operator);      
  }

  auto Result = parseDeclOperatorImpl(OperatorLoc, Name, NameLoc, Attributes);

  if (!DCC.movedToTopLevel() && !AllowTopLevel) {
    diagnose(OperatorLoc, diag::operator_decl_inner_scope);
    return nullptr;
  }

  return DCC.fixupParserResult(Result);
}

ParserResult<OperatorDecl>
Parser::parseDeclOperatorImpl(SourceLoc OperatorLoc, Identifier Name,
                              SourceLoc NameLoc, DeclAttributes &Attributes) {
  bool isPrefix = Attributes.hasAttribute<PrefixAttr>();
  bool isInfix = Attributes.hasAttribute<InfixAttr>();
  bool isPostfix = Attributes.hasAttribute<PostfixAttr>();

  // Parse (or diagnose) a specified precedence group and/or
  // designated protocol. These both look like identifiers, so we
  // parse them both as identifiers here and sort it out in type
  // checking.
  SourceLoc colonLoc;
  SmallVector<Identifier, 4> identifiers;
  SmallVector<SourceLoc, 4> identifierLocs;
  if (Tok.is(tok::colon)) {
    SyntaxParsingContext GroupCtxt(SyntaxContext,
                                   SyntaxKind::OperatorPrecedenceAndTypes);
    colonLoc = consumeToken();
    if (Tok.is(tok::code_complete)) {
      if (CodeCompletion && !isPrefix && !isPostfix) {
        CodeCompletion->completeInPrecedenceGroup(
          SyntaxKind::PrecedenceGroupRelation);
      }
      consumeToken();

      return makeParserCodeCompletionResult<OperatorDecl>();
    }

    if (Context.LangOpts.EnableOperatorDesignatedTypes) {
      if (Tok.is(tok::identifier)) {
        SyntaxParsingContext GroupCtxt(SyntaxContext,
                                       SyntaxKind::IdentifierList);

        Identifier name;
        identifierLocs.push_back(consumeIdentifier(&name));
        identifiers.push_back(name);

        while (Tok.is(tok::comma)) {
          auto comma = consumeToken();

          if (Tok.is(tok::identifier)) {
            Identifier name;
            identifierLocs.push_back(consumeIdentifier(&name));
            identifiers.push_back(name);
          } else {
            if (Tok.isNot(tok::eof)) {
              auto otherTokLoc = consumeToken();
              diagnose(otherTokLoc, diag::operator_decl_expected_type);
            } else {
              diagnose(comma, diag::operator_decl_trailing_comma);
            }
          }
        }
      }
    } else if (Tok.is(tok::identifier)) {
      SyntaxParsingContext GroupCtxt(SyntaxContext,
                                     SyntaxKind::IdentifierList);

      identifiers.push_back(Context.getIdentifier(Tok.getText()));
      identifierLocs.push_back(consumeToken(tok::identifier));

      if (isPrefix || isPostfix) {
        diagnose(colonLoc, diag::precedencegroup_not_infix)
            .fixItRemove({colonLoc, identifierLocs.back()});
      }
      // Nothing to complete here, simply consume the token.
      if (Tok.is(tok::code_complete))
        consumeToken();
    }
  }

  // Diagnose deprecated operator body syntax `operator + { ... }`.
  SourceLoc lBraceLoc;
  if (consumeIf(tok::l_brace, lBraceLoc)) {
    if (isInfix && !Tok.is(tok::r_brace)) {
      diagnose(lBraceLoc, diag::deprecated_operator_body_use_group);
    } else {
      auto Diag = diagnose(lBraceLoc, diag::deprecated_operator_body);
      if (Tok.is(tok::r_brace)) {
        SourceLoc lastGoodLoc =
            !identifierLocs.empty() ? identifierLocs.back() : SourceLoc();
        if (lastGoodLoc.isInvalid())
          lastGoodLoc = NameLoc;
        SourceLoc lastGoodLocEnd = Lexer::getLocForEndOfToken(SourceMgr,
                                                              lastGoodLoc);
        SourceLoc rBraceEnd = Lexer::getLocForEndOfToken(SourceMgr, Tok.getLoc());
        Diag.fixItRemoveChars(lastGoodLocEnd, rBraceEnd);
      }
    }

    skipUntilDeclRBrace();
    (void) consumeIf(tok::r_brace);
  }

  OperatorDecl *res;
  if (Attributes.hasAttribute<PrefixAttr>())
    res = new (Context)
        PrefixOperatorDecl(CurDeclContext, OperatorLoc, Name, NameLoc,
                           Context.AllocateCopy(identifiers),
                           Context.AllocateCopy(identifierLocs));
  else if (Attributes.hasAttribute<PostfixAttr>())
    res = new (Context)
        PostfixOperatorDecl(CurDeclContext, OperatorLoc, Name, NameLoc,
                            Context.AllocateCopy(identifiers),
                            Context.AllocateCopy(identifierLocs));
  else
    res = new (Context)
        InfixOperatorDecl(CurDeclContext, OperatorLoc, Name, NameLoc, colonLoc,
                          Context.AllocateCopy(identifiers),
                          Context.AllocateCopy(identifierLocs));

  diagnoseOperatorFixityAttributes(*this, Attributes, res);

  res->getAttrs() = Attributes;
  return makeParserResult(res);
}

ParserResult<PrecedenceGroupDecl>
Parser::parseDeclPrecedenceGroup(ParseDeclOptions flags,
                                 DeclAttributes &attributes) {
  SourceLoc precedenceGroupLoc = consumeToken(tok::kw_precedencegroup);
  DebuggerContextChange DCC (*this);

  if (!CodeCompletion && !DCC.movedToTopLevel() && !(flags & PD_AllowTopLevel))
  {
    diagnose(precedenceGroupLoc, diag::decl_inner_scope);
    return nullptr;
  }

  Identifier name;
  SourceLoc nameLoc;
  if (parseIdentifier(name, nameLoc, diag::expected_precedencegroup_name)) {
    // If the identifier is missing or a keyword or something, try to
    // skip the entire body.
    if (consumeIf(tok::l_brace)) {
      skipUntilDeclRBrace();
      (void) consumeIf(tok::r_brace);
    } else if (Tok.isNot(tok::eof) && peekToken().is(tok::l_brace)) {
      consumeToken();
      skipBracedBlock(*this, SyntaxContext);
    }
    return nullptr;
  }

  SourceLoc lbraceLoc, rbraceLoc;
  SourceLoc associativityKeywordLoc, associativityValueLoc;
  SourceLoc assignmentKeywordLoc, assignmentValueLoc;
  SourceLoc higherThanKeywordLoc, lowerThanKeywordLoc;
  SmallVector<PrecedenceGroupDecl::Relation, 4> higherThan, lowerThan;
  Associativity associativity = Associativity::None;
  bool assignment = false;
  bool invalid = false;
  bool hasCodeCompletion = false;

  // Helper functions.
  auto create = [&] {
    auto result = PrecedenceGroupDecl::create(CurDeclContext,
                                              precedenceGroupLoc,
                                              nameLoc, name, lbraceLoc,
                                              associativityKeywordLoc,
                                              associativityValueLoc,
                                              associativity,
                                              assignmentKeywordLoc,
                                              assignmentValueLoc,
                                              assignment,
                                              higherThanKeywordLoc, higherThan,
                                              lowerThanKeywordLoc, lowerThan,
                                              rbraceLoc);
    result->getAttrs() = attributes;
    return result;
  };
  auto createInvalid = [&](bool hasCodeCompletion) {
    // Use the last consumed token location as the rbrace to satisfy
    // the AST invariant about a decl's source range including all of
    // its components.
    if (!rbraceLoc.isValid()) rbraceLoc = PreviousLoc;

    auto result = create();
    result->setInvalid();
    if (hasCodeCompletion)
      return makeParserCodeCompletionResult(result);
    return makeParserErrorResult(result);
  };

  // Expect the body to start here.
  if (!consumeIf(tok::l_brace, lbraceLoc)) {
    diagnose(Tok, diag::expected_precedencegroup_lbrace);
    return createInvalid(/*hasCodeCompletion*/false);
  }
  // Empty body.
  if (Tok.is(tok::r_brace)) {
    // Create empty attribute list.
    SyntaxParsingContext(SyntaxContext,
                         SyntaxKind::PrecedenceGroupAttributeList);
    rbraceLoc = consumeToken(tok::r_brace);
    return makeParserResult(create());
  }

  auto abortBody = [&](bool hasCodeCompletion = false) {
    skipUntilDeclRBrace();
    (void) consumeIf(tok::r_brace, rbraceLoc);
    return createInvalid(hasCodeCompletion);
  };

  auto parseAttributePrefix = [&](SourceLoc &attrKeywordLoc) {
    auto attrName = Tok.getText(); 
    if (attrKeywordLoc.isValid()) {
      diagnose(Tok, diag::precedencegroup_attribute_redeclared, attrName);
      // We want to continue parsing after this.
      invalid = true;
    }
    attrKeywordLoc = consumeToken(tok::identifier);
    if (!consumeIf(tok::colon)) {
      diagnose(Tok, diag::expected_precedencegroup_attribute_colon, attrName);
      // Try to recover by allowing the colon to be missing.
    }
  };

  auto checkCodeCompletion = [&](SyntaxKind SK) -> bool {
    if (Tok.is(tok::code_complete)) {
      if (CodeCompletion)
        CodeCompletion->completeInPrecedenceGroup(SK);
      consumeToken();
      return true;
    }
    return false;
  };

  // Skips the CC token if it comes without spacing.
  auto skipUnspacedCodeCompleteToken = [&]() -> bool {
    if (Tok.is(tok::code_complete) && getEndOfPreviousLoc() == Tok.getLoc()) {
       consumeToken();
      return true;
    }
    return false;
  };

  // Parse the attributes in the body.
  while (Tok.isNot(tok::r_brace)) {
    if (checkCodeCompletion(SyntaxKind::PrecedenceGroupAttributeList)) {
      hasCodeCompletion = true;
      continue;
    } else if (Tok.isNot(tok::identifier)) {
      diagnose(Tok, diag::expected_precedencegroup_attribute);
      return abortBody();
    }
    auto attrName = Tok.getText();

    if (attrName == "associativity") {
      SyntaxParsingContext AttrCtxt(SyntaxContext,
                                    SyntaxKind::PrecedenceGroupAssociativity);
      // "associativity" is considered as a contextual keyword.
      TokReceiver->registerTokenKindChange(Tok.getLoc(),
                                           tok::contextual_keyword);
      parseAttributePrefix(associativityKeywordLoc);

      if (checkCodeCompletion(SyntaxKind::PrecedenceGroupAssociativity))
        return abortBody(/*hasCodeCompletion*/true);

      if (Tok.isNot(tok::identifier)) {
        diagnose(Tok, diag::expected_precedencegroup_associativity);
        return abortBody();
      }

      auto parsedAssociativity
        = llvm::StringSwitch<Optional<Associativity>>(Tok.getText())
          .Case("none", Associativity::None)
          .Case("left", Associativity::Left)
          .Case("right", Associativity::Right)
          .Default(None);

      if (!parsedAssociativity) {
        diagnose(Tok, diag::expected_precedencegroup_associativity);
        parsedAssociativity = Associativity::None;
        invalid = true;
      } else {
        // "left", "right" or "none" are considered contextual keywords.
        TokReceiver->registerTokenKindChange(Tok.getLoc(),
                                             tok::contextual_keyword);
      }
      associativity = *parsedAssociativity;
      associativityValueLoc = consumeToken();

      if (skipUnspacedCodeCompleteToken())
        return abortBody(/*hasCodeCompletion*/true);
      continue;
    }
    
    if (attrName == "assignment") {
      SyntaxParsingContext AttrCtxt(SyntaxContext,
                                    SyntaxKind::PrecedenceGroupAssignment);
      parseAttributePrefix(assignmentKeywordLoc);

      // "assignment" is considered as a contextual keyword.
      TokReceiver->registerTokenKindChange(assignmentKeywordLoc,
                                           tok::contextual_keyword);
      if (checkCodeCompletion(SyntaxKind::PrecedenceGroupAssignment))
        return abortBody(/*hasCodeCompletion*/true);

      if (consumeIf(tok::kw_true, assignmentValueLoc)) {
        assignment = true;
      } else if (consumeIf(tok::kw_false, assignmentValueLoc)) {
        assignment = false;
      } else {
        diagnose(Tok, diag::expected_precedencegroup_assignment);
        return abortBody();
      }
      if (skipUnspacedCodeCompleteToken())
        return abortBody(/*hasCodeCompletion*/true);
      continue;
    }

    bool isLowerThan = false;
    if (attrName == "higherThan" ||
        (isLowerThan = (attrName == "lowerThan"))) {
      SyntaxParsingContext AttrCtxt(SyntaxContext,
                                    SyntaxKind::PrecedenceGroupRelation);
      // "lowerThan" and "higherThan" are contextual keywords.
      TokReceiver->registerTokenKindChange(Tok.getLoc(),
                                           tok::contextual_keyword);
      parseAttributePrefix(isLowerThan ? lowerThanKeywordLoc
                                       : higherThanKeywordLoc);
      auto &relations = (isLowerThan ? lowerThan : higherThan);

      do {
        SyntaxParsingContext NameCtxt(SyntaxContext,
                                      SyntaxKind::PrecedenceGroupNameElement);
        if (checkCodeCompletion(SyntaxKind::PrecedenceGroupRelation)) {
          return abortBody(/*hasCodeCompletion*/true);
        }

        if (Tok.isNot(tok::identifier)) {
          diagnose(Tok, diag::expected_precedencegroup_relation, attrName);
          return abortBody();
        }
        Identifier name;
        SourceLoc nameLoc = consumeIdentifier(&name);
        relations.push_back({nameLoc, name, nullptr});

        if (skipUnspacedCodeCompleteToken())
          return abortBody(/*hasCodeCompletion*/true);
        if (!consumeIf(tok::comma))
          break;
      } while (true);
      SyntaxContext->collectNodesInPlace(SyntaxKind::PrecedenceGroupNameList);
      continue;
    }

    diagnose(Tok, diag::unknown_precedencegroup_attribute, attrName);
    return abortBody();
  }
  SyntaxContext->collectNodesInPlace(SyntaxKind::PrecedenceGroupAttributeList);
  rbraceLoc = consumeToken(tok::r_brace);

  auto result = create();
  if (invalid) result->setInvalid();
  if (hasCodeCompletion)
    return makeParserCodeCompletionResult(result);
  return makeParserResult(result);
}
