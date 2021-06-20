//===- FIRParser.cpp - .fir to FIRRTL dialect parser ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This implements a .fir file parser.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/FIRRTL/FIRParser.h"

#include "FIRAnnotations.h"
#include "FIRLexer.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Support/LLVM.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Translation.h"
#include "llvm/ADT/PointerEmbeddedInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopedHashTable.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace circt;
using namespace firrtl;

using llvm::SMLoc;
using llvm::SourceMgr;
using mlir::LocationAttr;

namespace json = llvm::json;

/// Return true if this is a useless temporary name produced by FIRRTL.  We
/// drop these as they don't convey semantic meaning.
static bool isUselessName(StringRef name) {
  // Ignore _T and _T_123
  if (name.startswith("_T")) {
    if (name.size() == 2)
      return true;
    return name.size() > 3 && name[2] == '_' && llvm::isDigit(name[3]);
  }

  // Ignore _GEN and _GEN_123, these are produced by Namespace.scala.
  if (name.startswith("_GEN")) {
    if (name.size() == 4)
      return true;
    return name.size() > 5 && name[4] == '_' && llvm::isDigit(name[5]);
  }
  return false;
}

/// If the specified name is a useless temporary name produced by FIRRTL, return
/// an empty string to ignore it.  Otherwise, return the argument unmodified.
static StringRef filterUselessName(StringRef name) {
  return isUselessName(name) ? "" : name;
}

/// Get an annotation with a class name field.
static DictionaryAttr getAnnotationOfClass(MLIRContext *context,
                                           StringRef classString) {
  auto id = NamedAttribute(Identifier::get("class", context),
                           StringAttr::get(context, classString));
  return DictionaryAttr::getWithSorted(context, {id});
}

/// Checks the annotations array for a matching annotation.
static bool hasAnnotation(ArrayAttr annotations, DictionaryAttr annotation) {
  return llvm::is_contained(annotations, annotation);
}

//===----------------------------------------------------------------------===//
// GlobalFIRParserState
//===----------------------------------------------------------------------===//

namespace {
/// This class refers to all of the state maintained globally by the parser,
/// such as the current lexer position.  This is separated out from the parser
/// so that individual subparsers can refer to the same state.
struct GlobalFIRParserState {
  GlobalFIRParserState(const llvm::SourceMgr &sourceMgr, MLIRContext *context,
                       FIRParserOptions options,
                       const llvm::MemoryBuffer *annotationsBuf)
      : context(context), options(options), lex(sourceMgr, context),
        curToken(lex.lexToken()), annotationsBuf(annotationsBuf),
        loIdentifier(Identifier::get("lo", context)),
        hiIdentifier(Identifier::get("hi", context)),
        amountIdentifier(Identifier::get("amount", context)),
        fieldnameIdentifier(Identifier::get("fieldname", context)),
        indexIdentifier(Identifier::get("index", context)),
        locatorFilenameCache(loIdentifier /*arbitrary non-null identifier*/) {
    dontTouchAnnotation =
        getAnnotationOfClass(context, "firrtl.transforms.DontTouchAnnotation");
    emptyArrayAttr = ArrayAttr::get(context, {});
  }

  /// The context we're parsing into.
  MLIRContext *const context;

  // Options that control the behavior of the parser.
  const FIRParserOptions options;

  /// A mapping of targets to annotations
  llvm::StringMap<ArrayAttr> annotationMap;

  /// A global identifier that can be used to link multiple annotations
  /// together.  This should be incremented on use.
  unsigned annotationID = 0;

  /// The lexer for the source file we're parsing.
  FIRLexer lex;

  /// This is the next token that hasn't been consumed yet.
  FIRToken curToken;

  /// A pointer to a buffer of annotations (the contents of a JSON
  /// file).  This is null if no annotations were provided.
  const llvm::MemoryBuffer *annotationsBuf;

  /// A mutable reference to the current CircuitTarget.
  StringRef circuitTarget;

  /// A mutable reference to the current ModuleTarget.
  StringRef moduleTarget;

  /// Cached annotation for DontTouch.
  DictionaryAttr dontTouchAnnotation;

  /// An empty array attribute.
  ArrayAttr emptyArrayAttr;

  /// Cached identifiers used in primitives.
  Identifier loIdentifier, hiIdentifier, amountIdentifier, fieldnameIdentifier;
  Identifier indexIdentifier;

  class BacktraceState {
  public:
    explicit BacktraceState(GlobalFIRParserState &state)
        : state(state), curToken(state.curToken),
          cursor(state.lex.getCursor()) {}

    void backtrack() {
      state.curToken = curToken;
      cursor.restore(state.lex);
    }

  private:
    GlobalFIRParserState &state;
    FIRToken curToken;
    FIRLexerCursor cursor;
  };

  BacktraceState getBacktrackState() { return BacktraceState(*this); }

  /// Return an FileLineColLoc for the specified location, but use a bit of
  /// caching to reduce thrasing the MLIRContext.
  FileLineColLoc getFileLineColLoc(StringRef filename, unsigned lineNo,
                                   unsigned columnNo) {
    // Check our single-entry cache for this filename.
    Identifier filenameId = locatorFilenameCache;
    if (filenameId.str() != filename) {
      // We missed!  Get the right identifier.
      locatorFilenameCache = filenameId = Identifier::get(filename, context);

      // If we miss in the filename cache, we also miss in the FileLineColLoc
      // cache.
      return fileLineColLocCache =
                 FileLineColLoc::get(filenameId, lineNo, columnNo);
    }

    // If we hit the filename cache, check the FileLineColLoc cache.
    auto result = fileLineColLocCache;
    if (result && result.getLine() == lineNo && result.getColumn() == columnNo)
      return result;

    return fileLineColLocCache =
               FileLineColLoc::get(filenameId, lineNo, columnNo);
  }

private:
  GlobalFIRParserState(const GlobalFIRParserState &) = delete;
  void operator=(const GlobalFIRParserState &) = delete;

  /// This is a single-entry cache for filenames in locators.
  Identifier locatorFilenameCache;
  /// This is a single-entry cache for FileLineCol locations.
  FileLineColLoc fileLineColLocCache;
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
// FIRParser
//===----------------------------------------------------------------------===//

namespace {
/// This class implements logic common to all levels of the parser, including
/// things like types and helper logic.
struct FIRParser {
  FIRParser(GlobalFIRParserState &state) : state(state) {}

  // Helper methods to get stuff from the parser-global state.
  GlobalFIRParserState &getState() const { return state; }
  MLIRContext *getContext() const { return state.context; }
  const llvm::SourceMgr &getSourceMgr() { return state.lex.getSourceMgr(); }

  /// Return the indentation level of the specified token.
  Optional<unsigned> getIndentation() const {
    return state.lex.getIndentation(state.curToken);
  }

  /// Return the current token the parser is inspecting.
  const FIRToken &getToken() const { return state.curToken; }
  StringRef getTokenSpelling() const { return state.curToken.getSpelling(); }

  //===--------------------------------------------------------------------===//
  // Error Handling
  //===--------------------------------------------------------------------===//

  /// Emit an error and return failure.
  InFlightDiagnostic emitError(const Twine &message = {}) {
    return emitError(state.curToken.getLoc(), message);
  }
  InFlightDiagnostic emitError(SMLoc loc, const Twine &message = {});

  //===--------------------------------------------------------------------===//
  // Location Handling
  //===--------------------------------------------------------------------===//

  class LocWithInfo;

  /// Encode the specified source location information into an attribute for
  /// attachment to the IR.
  Location translateLocation(llvm::SMLoc loc) {
    return state.lex.translateLocation(loc);
  }

  /// Parse an @info marker if present.  If so, fill in the specified Location,
  /// if not, ignore it.
  ParseResult parseOptionalInfoLocator(LocationAttr &result);

  //===--------------------------------------------------------------------===//
  // Annotation Parsing
  //===--------------------------------------------------------------------===//

  /// Parse a non-standard inline Annotation JSON blob if present.  This uses
  /// the info-like encoding of %[<JSON Blob>].
  ParseResult parseOptionalAnnotations(SMLoc &loc, StringRef &result);

  //===--------------------------------------------------------------------===//
  // Token Parsing
  //===--------------------------------------------------------------------===//

  /// If the current token has the specified kind, consume it and return true.
  /// If not, return false.
  bool consumeIf(FIRToken::Kind kind) {
    if (state.curToken.isNot(kind))
      return false;
    consumeToken(kind);
    return true;
  }

  /// Advance the current lexer onto the next token.
  ///
  /// This returns the consumed token.
  FIRToken consumeToken() {
    FIRToken consumedToken = state.curToken;
    assert(consumedToken.isNot(FIRToken::eof, FIRToken::error) &&
           "shouldn't advance past EOF or errors");
    state.curToken = state.lex.lexToken();
    return consumedToken;
  }

  /// Advance the current lexer onto the next token, asserting what the expected
  /// current token is.  This is preferred to the above method because it leads
  /// to more self-documenting code with better checking.
  ///
  /// This returns the consumed token.
  FIRToken consumeToken(FIRToken::Kind kind) {
    FIRToken consumedToken = state.curToken;
    assert(consumedToken.is(kind) && "consumed an unexpected token");
    consumeToken();
    return consumedToken;
  }

  /// Capture the current token's spelling into the specified value.  This
  /// always succeeds.
  ParseResult parseGetSpelling(StringRef &spelling) {
    spelling = getTokenSpelling();
    return success();
  }

  /// Consume the specified token if present and return success.  On failure,
  /// output a diagnostic and return failure.
  ParseResult parseToken(FIRToken::Kind expectedToken, const Twine &message);

  /// Parse a list of elements, terminated with an arbitrary token.
  ParseResult parseListUntil(FIRToken::Kind rightToken,
                             const std::function<ParseResult()> &parseElement);

  //===--------------------------------------------------------------------===//
  // Common Parser Rules
  //===--------------------------------------------------------------------===//

  /// Parse 'intLit' into the specified value.
  ParseResult parseIntLit(APInt &result, const Twine &message);
  ParseResult parseIntLit(int64_t &result, const Twine &message);

  // Parse ('<' intLit '>')? setting result to -1 if not present.
  ParseResult parseOptionalWidth(int64_t &result);

  // Parse the 'id' grammar, which is an identifier or an allowed keyword.
  ParseResult parseId(StringRef &result, const Twine &message);
  ParseResult parseId(StringAttr &result, const Twine &message);
  ParseResult parseFieldId(StringRef &result, const Twine &message);
  ParseResult parseType(FIRRTLType &result, const Twine &message);

  ParseResult parseOptionalRUW(RUWAttr &result);

  //===--------------------------------------------------------------------===//
  // Annotation Utilities
  //===--------------------------------------------------------------------===//

  /// Add annotations from a string to the internal annotation map.  Report
  /// errors using a provided source manager location and with a provided error
  /// message
  ParseResult importAnnotations(SMLoc loc, StringRef annotationsStr);

  /// Add annotations from the source manager, if an annotation file was added.
  ParseResult importAnnotationFile(SMLoc loc);

  /// Return the set of annotations for a given Target.
  ArrayAttr getAnnotations(Twine target);

  /// Returns true if the annotation list contains the DontTouchAnnotation. This
  /// method is slightly more efficient than other lookup methods, because it
  /// uses a stashed copy of the annotation for lookup.
  bool hasDontTouch(ArrayAttr annotations) {
    return hasAnnotation(annotations, state.dontTouchAnnotation);
  }

private:
  FIRParser(const FIRParser &) = delete;
  void operator=(const FIRParser &) = delete;

  /// FIRParser is subclassed and reinstantiated.  Do not add additional
  /// non-trivial state here, add it to the GlobalFIRParserState class.
  GlobalFIRParserState &state;
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Error Handling
//===----------------------------------------------------------------------===//

InFlightDiagnostic FIRParser::emitError(SMLoc loc, const Twine &message) {
  auto diag = mlir::emitError(translateLocation(loc), message);

  // If we hit a parse error in response to a lexer error, then the lexer
  // already reported the error.
  if (getToken().is(FIRToken::error))
    diag.abandon();
  return diag;
}

//===----------------------------------------------------------------------===//
// Token Parsing
//===----------------------------------------------------------------------===//

/// Consume the specified token if present and return success.  On failure,
/// output a diagnostic and return failure.
ParseResult FIRParser::parseToken(FIRToken::Kind expectedToken,
                                  const Twine &message) {
  if (consumeIf(expectedToken))
    return success();
  return emitError(message);
}

/// Parse a list of elements, terminated with an arbitrary token.
ParseResult
FIRParser::parseListUntil(FIRToken::Kind rightToken,
                          const std::function<ParseResult()> &parseElement) {

  while (!consumeIf(rightToken)) {
    if (parseElement())
      return failure();
  }
  return success();
}

//===--------------------------------------------------------------------===//
// Location Processing
//===--------------------------------------------------------------------===//

/// This helper class is used to handle Info records, which specify higher level
/// symbolic source location, that may be missing from the file.  If the higher
/// level source information is missing, we fall back to the location in the
/// .fir file.
class FIRParser::LocWithInfo {
public:
  explicit LocWithInfo(SMLoc firLoc, FIRParser *parser)
      : parser(parser), firLoc(firLoc) {}

  SMLoc getFIRLoc() const { return firLoc; }

  Location getLoc() {
    if (infoLoc.hasValue())
      return infoLoc.getValue();
    auto result = parser->translateLocation(firLoc);
    infoLoc = result;
    return result;
  }

  /// Parse an @info marker if present and update our location.
  ParseResult parseOptionalInfo() {
    LocationAttr loc;
    if (failed(parser->parseOptionalInfoLocator(loc)))
      return failure();
    if (loc)
      infoLoc = loc;
    return success();
  }

private:
  FIRParser *const parser;

  /// This is the designated location in the .fir file for use when there is no
  /// @ info marker.
  SMLoc firLoc;

  /// This is the location specified by the @ marker if present.
  Optional<Location> infoLoc;
};

/// Parse an @info marker if present.  If so, fill in the specified Location,
/// if not, ignore it.
ParseResult FIRParser::parseOptionalInfoLocator(LocationAttr &result) {
  if (getToken().isNot(FIRToken::fileinfo))
    return success();

  auto loc = getToken().getLoc();

  // See if we can parse this token into a File/Line/Column record.  If not,
  // just ignore it with a warning.
  auto unknownFormat = [&]() -> ParseResult {
    mlir::emitWarning(translateLocation(loc),
                      "ignoring unknown @ info record format");
    return success();
  };

  auto spelling = getTokenSpelling();
  consumeToken(FIRToken::fileinfo);

  // The spelling of the token looks something like "@[Decoupled.scala 221:8]".
  if (!spelling.startswith("@[") || !spelling.endswith("]"))
    return unknownFormat();

  spelling = spelling.drop_front(2).drop_back(1);

  // Decode the locator in "spelling", returning the filename and filling in
  // lineNo and colNo on success.  On failure, this returns an empty filename.
  auto decodeLocator = [&](StringRef input, unsigned &resultLineNo,
                           unsigned &resultColNo) -> StringRef {
    // Split at the last space.
    auto spaceLoc = input.find_last_of(' ');
    if (spaceLoc == StringRef::npos)
      return {};

    auto filename = input.take_front(spaceLoc);
    auto lineAndColumn = input.drop_front(spaceLoc + 1);

    // Decode the line/column.  If the colon is missing, then it will be empty
    // here.
    StringRef lineStr, colStr;
    std::tie(lineStr, colStr) = lineAndColumn.split(':');

    // Decode the line number and the column number if present.
    if (lineStr.getAsInteger(10, resultLineNo))
      return {};
    if (!colStr.empty()) {
      if (colStr.front() != '{') {
        if (colStr.getAsInteger(10, resultColNo))
          return {};
      } else {
        // compound locator, just parse the first part for now
        if (colStr.drop_front().split(',').first.getAsInteger(10, resultColNo))
          return {};
      }
    }
    return filename;
  };

  // Decode the locator spelling, reporting an error if it is malformed.
  unsigned lineNo = 0, columnNo = 0;
  StringRef filename = decodeLocator(spelling, lineNo, columnNo);
  if (filename.empty())
    return unknownFormat();

  // If info locators are ignored, don't actually apply them.  We still do all
  // the verification above though.
  if (state.options.ignoreInfoLocators)
    return success();

  // Compound locators will be combined with spaces, like:
  //  @[Foo.scala 123:4 Bar.scala 309:14]
  // and at this point will be parsed as a-long-string-with-two-spaces at
  // 309:14.   We'd like to parse this into two things and represent it as an
  // MLIR fused locator, but we want to be conservatively safe for filenames
  // that have a space in it.  As such, we are careful to make sure we can
  // decode the filename/loc of the result.  If so, we accumulate results,
  // backward, in this vector.
  SmallVector<Location> extraLocs;
  auto spaceLoc = filename.find_last_of(' ');
  while (spaceLoc != StringRef::npos) {
    // Try decoding the thing before the space.  Validates that there is another
    // space and that the file/line can be decoded in that substring.
    unsigned nextLineNo = 0, nextColumnNo = 0;
    auto nextFilename =
        decodeLocator(filename.take_front(spaceLoc), nextLineNo, nextColumnNo);

    // On failure we didn't have a joined locator.
    if (nextFilename.empty())
      break;

    // On success, remember what we already parsed (Bar.Scala / 309:14), and
    // move on to the next chunk.
    auto loc = state.getFileLineColLoc(filename.drop_front(spaceLoc + 1),
                                       lineNo, columnNo);
    extraLocs.push_back(loc);
    filename = nextFilename;
    lineNo = nextLineNo;
    columnNo = nextColumnNo;
    spaceLoc = filename.find_last_of(' ');
  }

  result = state.getFileLineColLoc(filename, lineNo, columnNo);
  if (!extraLocs.empty()) {
    extraLocs.push_back(result);
    std::reverse(extraLocs.begin(), extraLocs.end());
    result = FusedLoc::get(getContext(), extraLocs);
  }
  return success();
}

//===--------------------------------------------------------------------===//
// Annotation Handling
//===--------------------------------------------------------------------===//

/// Parse a non-standard inline Annotation JSON blob if present.  This uses
/// the info-like encoding of %[<JSON Blob>].
ParseResult FIRParser::parseOptionalAnnotations(SMLoc &loc, StringRef &result) {

  if (getToken().isNot(FIRToken::inlineannotation))
    return success();

  loc = getToken().getLoc();

  result = getTokenSpelling().drop_front(2).drop_back(1);
  consumeToken(FIRToken::inlineannotation);

  return success();
}

//===--------------------------------------------------------------------===//
// Common Parser Rules
//===--------------------------------------------------------------------===//

/// intLit    ::= UnsignedInt
///           ::= SignedInt
///           ::= HexLit
///           ::= OctalLit
///           ::= BinaryLit
/// HexLit    ::= '"' 'h' ( '+' | '-' )? ( HexDigit )+ '"'
/// OctalLit  ::= '"' 'o' ( '+' | '-' )? ( OctalDigit )+ '"'
/// BinaryLit ::= '"' 'b' ( '+' | '-' )? ( BinaryDigit )+ '"'
///
ParseResult FIRParser::parseIntLit(APInt &result, const Twine &message) {
  auto spelling = getTokenSpelling();
  switch (getToken().getKind()) {
  case FIRToken::integer:
    if (spelling.getAsInteger(10, result))
      return emitError(message), failure();

    // Make sure that the returned APInt has a zero at the top so clients don't
    // confuse it with a negative number.
    if (result.isNegative())
      result = result.zext(result.getBitWidth() + 1);

    consumeToken(FIRToken::integer);
    return success();

  case FIRToken::signed_integer:
    assert(spelling[0] == '+' || spelling[0] == '-');
    if (spelling.drop_front().getAsInteger(10, result))
      return emitError(message), failure();

    // Make sure that the returned APInt has a zero at the top so clients don't
    // confuse it with a negative number.
    if (result.isNegative())
      result = result.zext(result.getBitWidth() + 1);

    if (spelling[0] == '-')
      result = -result;
    consumeToken(FIRToken::signed_integer);
    return success();
  case FIRToken::string: {
    // Drop the quotes.
    assert(spelling.front() == '"' && spelling.back() == '"');
    spelling = spelling.drop_back().drop_front();

    // Decode the base.
    unsigned base;
    switch (spelling.empty() ? ' ' : spelling.front()) {
    case 'h':
      base = 16;
      break;
    case 'o':
      base = 8;
      break;
    case 'b':
      base = 2;
      break;
    default:
      return emitError("expected base specifier (h/o/b) in integer literal"),
             failure();
    }
    spelling = spelling.drop_front();

    // Handle the optional sign.
    bool isNegative = false;
    if (!spelling.empty() && spelling.front() == '+')
      spelling = spelling.drop_front();
    else if (!spelling.empty() && spelling.front() == '-') {
      isNegative = true;
      spelling = spelling.drop_front();
    }

    // Parse the digits.
    if (spelling.empty())
      return emitError("expected digits in integer literal"), failure();

    if (spelling.getAsInteger(base, result))
      return emitError("invalid character in integer literal"), failure();

    // We just parsed the positive version of this number.  Make sure it has
    // a zero at the top so clients don't confuse it with a negative number and
    // so the negation (in the case of a negative sign) doesn't overflow.
    if (result.isNegative())
      result = result.zext(result.getBitWidth() + 1);

    if (isNegative)
      result = -result;

    consumeToken(FIRToken::string);
    return success();
  }

  default:
    return emitError("expected integer literal"), failure();
  }
}

ParseResult FIRParser::parseIntLit(int64_t &result, const Twine &message) {
  APInt value;
  auto loc = getToken().getLoc();
  if (parseIntLit(value, message))
    return failure();

  result = (int64_t)value.getLimitedValue();
  if (result != value)
    return emitError(loc, "value is too big to handle"), failure();
  return success();
}

// optional-width ::= ('<' intLit '>')?
//
// This returns with result equal to -1 if not present.
ParseResult FIRParser::parseOptionalWidth(int64_t &result) {
  if (!consumeIf(FIRToken::less))
    return result = -1, success();

  // Parse a width specifier if present.
  auto widthLoc = getToken().getLoc();
  if (parseIntLit(result, "expected width") ||
      parseToken(FIRToken::greater, "expected >"))
    return failure();

  if (result < 0)
    return emitError(widthLoc, "invalid width specifier"), failure();

  return success();
}

/// id  ::= Id | keywordAsId
///
/// Parse the 'id' grammar, which is an identifier or an allowed keyword.  On
/// success, this returns the identifier in the result attribute.
ParseResult FIRParser::parseId(StringRef &result, const Twine &message) {
  switch (getToken().getKind()) {
  // The most common case is an identifier.
  case FIRToken::identifier:
// Otherwise it may be a keyword that we're allowing in an id position.
#define TOK_KEYWORD(spelling) case FIRToken::kw_##spelling:
#include "FIRTokenKinds.def"

    // Yep, this is a valid 'id'.  Turn it into an attribute.
    result = getTokenSpelling();
    consumeToken();
    return success();

  default:
    emitError(message);
    return failure();
  }
}

ParseResult FIRParser::parseId(StringAttr &result, const Twine &message) {
  StringRef name;
  if (parseId(name, message))
    return failure();

  result = StringAttr::get(getContext(), name);
  return success();
}

/// fieldId ::= Id
///         ::= RelaxedId
///         ::= UnsignedInt
///         ::= keywordAsId
///
ParseResult FIRParser::parseFieldId(StringRef &result, const Twine &message) {
  // Handle the UnsignedInt case.
  result = getTokenSpelling();
  if (consumeIf(FIRToken::integer))
    return success();

  // FIXME: Handle RelaxedId

  // Otherwise, it must be Id or keywordAsId.
  if (parseId(result, message))
    return failure();

  return success();
}

/// type ::= 'Clock'
///      ::= 'Reset'
///      ::= 'AsyncReset'
///      ::= 'UInt' optional-width
///      ::= 'SInt' optional-width
///      ::= 'Analog' optional-width
///      ::= {' field* '}'
///      ::= type '[' intLit ']'
///
/// field: 'flip'? fieldId ':' type
///
ParseResult FIRParser::parseType(FIRRTLType &result, const Twine &message) {
  switch (getToken().getKind()) {
  default:
    return emitError(message), failure();

  case FIRToken::kw_Clock:
    consumeToken(FIRToken::kw_Clock);
    result = ClockType::get(getContext());
    break;

  case FIRToken::kw_Reset:
    consumeToken(FIRToken::kw_Reset);
    result = ResetType::get(getContext());
    break;

  case FIRToken::kw_AsyncReset:
    consumeToken(FIRToken::kw_AsyncReset);
    result = AsyncResetType::get(getContext());
    break;

  case FIRToken::kw_UInt:
  case FIRToken::kw_SInt:
  case FIRToken::kw_Analog: {
    auto kind = getToken().getKind();
    consumeToken();

    // Parse a width specifier if present.
    int64_t width;
    if (parseOptionalWidth(width))
      return failure();

    if (kind == FIRToken::kw_SInt)
      result = SIntType::get(getContext(), width);
    else if (kind == FIRToken::kw_UInt)
      result = UIntType::get(getContext(), width);
    else {
      assert(kind == FIRToken::kw_Analog);
      result = AnalogType::get(getContext(), width);
    }
    break;
  }

  case FIRToken::l_brace: {
    consumeToken(FIRToken::l_brace);

    SmallVector<BundleType::BundleElement, 4> elements;
    if (parseListUntil(FIRToken::r_brace, [&]() -> ParseResult {
          bool isFlipped = consumeIf(FIRToken::kw_flip);

          StringRef fieldName;
          FIRRTLType type;
          if (parseFieldId(fieldName, "expected bundle field name") ||
              parseToken(FIRToken::colon, "expected ':' in bundle") ||
              parseType(type, "expected bundle field type"))
            return failure();

          if (isFlipped)
            type = FlipType::get(type);

          elements.push_back({StringAttr::get(getContext(), fieldName), type});
          return success();
        }))
      return failure();
    result = BundleType::get(elements, getContext());
    break;
  }
  }

  // Handle postfix vector sizes.
  while (consumeIf(FIRToken::l_square)) {
    auto sizeLoc = getToken().getLoc();
    int64_t size;
    if (parseIntLit(size, "expected width") ||
        parseToken(FIRToken::r_square, "expected ]"))
      return failure();

    if (size < 0)
      return emitError(sizeLoc, "invalid size specifier"), failure();

    result = FVectorType::get(result, size);
  }

  return success();
}

/// ruw ::= 'old' | 'new' | 'undefined'
ParseResult FIRParser::parseOptionalRUW(RUWAttr &result) {
  switch (getToken().getKind()) {
  default:
    break;

  case FIRToken::kw_old:
    result = RUWAttr::Old;
    consumeToken(FIRToken::kw_old);
    break;
  case FIRToken::kw_new:
    result = RUWAttr::New;
    consumeToken(FIRToken::kw_new);
    break;
  case FIRToken::kw_undefined:
    result = RUWAttr::Undefined;
    consumeToken(FIRToken::kw_undefined);
    break;
  }

  return success();
}

ParseResult FIRParser::importAnnotations(SMLoc loc, StringRef annotationsStr) {

  auto annotations = json::parse(annotationsStr);
  if (auto err = annotations.takeError()) {
    handleAllErrors(std::move(err), [&](const json::ParseError &a) {
      auto diag = emitError(loc, "Failed to parse JSON Annotations");
      diag.attachNote() << a.message();
    });
    return failure();
  }

  json::Path::Root root;
  llvm::StringMap<ArrayAttr> annotationMap;
  if (!fromJSON(annotations.get(), annotationMap, root, getContext())) {
    auto diag = emitError(loc, "Invalid/unsupported annotation format");
    std::string jsonErrorMessage =
        "See inline comments for problem area in JSON:\n";
    llvm::raw_string_ostream s(jsonErrorMessage);
    root.printErrorContext(annotations.get(), s);
    diag.attachNote() << jsonErrorMessage;
    return failure();
  }

  if (!scatterCustomAnnotations(annotationMap, getContext(), state.annotationID,
                                translateLocation(loc)))
    return failure();

  for (auto a : annotationMap.keys()) {
    auto &entry = state.annotationMap[a];
    if (!entry) {
      entry = annotationMap[a];
      continue;
    }

    SmallVector<Attribute> annotationVec;
    auto arrayRef = state.annotationMap[a].getValue();
    annotationVec.append(arrayRef.begin(), arrayRef.end());
    arrayRef = annotationMap[a].getValue();
    annotationVec.append(arrayRef.begin(), arrayRef.end());
    state.annotationMap[a] = ArrayAttr::get(getContext(), annotationVec);
  }

  return success();
}

ParseResult FIRParser::importAnnotationFile(SMLoc loc) {
  if (!state.annotationsBuf)
    return success();

  ParseResult result = success();
  result = importAnnotations(loc, (state.annotationsBuf)->getBuffer());

  if (!result)
    state.annotationsBuf = nullptr;

  return result;
}

/// Return the set of annotations for a given Target.
ArrayAttr FIRParser::getAnnotations(Twine target) {
  // Early exit if no annotations exist.  This avoids the cost of
  // constructing strings representing targets if no annotation can
  // possibly exist.
  if (state.annotationMap.empty())
    return state.emptyArrayAttr;

  // Flatten the input twine into a SmallVector for lookup.
  SmallString<64> targetStr;
  target.toVector(targetStr);

  if (auto result = state.annotationMap.lookup(targetStr))
    return result;
  return state.emptyArrayAttr;
}

//===----------------------------------------------------------------------===//
// FIRScopedParser
//===----------------------------------------------------------------------===//

namespace {
/// This class is a common base class for stuff that needs local scope
/// manipulation helpers.
class FIRScopedParser : public FIRParser {
public:
  // Entries in a symbol table are either an mlir::Value for the operation that
  // defines the value or an unbundled ID tracking the index in the
  // UnbundledValues list.
  using UnbundledID = llvm::PointerEmbeddedInt<unsigned, 31>;
  using SymbolValueEntry = llvm::PointerUnion<Value, UnbundledID>;

  using SymbolTable =
      llvm::ScopedHashTable<Identifier, std::pair<SMLoc, SymbolValueEntry>,
                            DenseMapInfo<Identifier>, llvm::BumpPtrAllocator>;

  using UnbundledValueEntry = SmallVector<std::pair<Attribute, Value>>;
  using UnbundledValuesList = std::vector<UnbundledValueEntry>;

  using MemoryScopeTable =
      llvm::ScopedHashTable<Identifier,
                            std::pair<SymbolTable::ScopeTy *, Operation *>,
                            DenseMapInfo<Identifier>, llvm::BumpPtrAllocator>;

  FIRScopedParser(GlobalFIRParserState &state, SymbolTable &symbolTable,
                  UnbundledValuesList &unbundledValues,
                  MemoryScopeTable &memoryScopeTable)
      : FIRParser(state), symbolTable(symbolTable),
        unbundledValues(unbundledValues), memoryScopeTable(memoryScopeTable) {}

  /// Add a symbol entry with the specified name, returning failure if the name
  /// is already defined.
  ParseResult addSymbolEntry(StringRef name, SymbolValueEntry entry, SMLoc loc);
  ParseResult addSymbolEntry(StringRef name, Value value, SMLoc loc) {
    return addSymbolEntry(name, SymbolValueEntry(value), loc);
  }

  /// Resolved a symbol table entry to a value.  Emission of error is optional.
  ParseResult resolveSymbolEntry(Value &result, SymbolValueEntry &entry,
                                 SMLoc loc, bool fatal = true);

  /// Resolved a symbol table entry if it is an expanded bundle e.g. from an
  /// instance.  Emission of error is optional.
  ParseResult resolveSymbolEntry(Value &result, SymbolValueEntry &entry,
                                 StringRef field, SMLoc loc);

  /// Look up the specified name, emitting an error and returning failure if the
  /// name is unknown.
  ParseResult lookupSymbolEntry(SymbolValueEntry &result, StringRef name,
                                SMLoc loc);

  /// This symbol table holds the names of ports, wires, and other local decls.
  /// This is scoped because conditional statements introduce subscopes.
  SymbolTable &symbolTable;

  /// This contains one entry for each value in FIRRTL that is represented as a
  /// bundle type in the FIRRTL spec but for which we represent as an exploded
  /// set of elements in the FIRRTL dialect.
  UnbundledValuesList &unbundledValues;

  /// Chisel is producing mports with invalid scopes.  To work around the bug,
  /// we need to keep track of the scope (in the `symbolTable`) of each memory.
  /// This keeps track of this.
  MemoryScopeTable &memoryScopeTable;

  /// Return the current modulet target, e.g., "~Foo|Bar".
  StringRef getModuleTarget() { return getState().moduleTarget; }
};
} // end anonymous namespace

/// Add a symbol entry with the specified name, returning failure if the name
/// is already defined.
ParseResult FIRScopedParser::addSymbolEntry(StringRef name,
                                            SymbolValueEntry entry, SMLoc loc) {
  auto nameId = Identifier::get(name, getContext());
  auto prev = symbolTable.lookup(nameId);
  if (prev.first.isValid()) {
    emitError(loc, "redefinition of name '" + name.str() + "'")
            .attachNote(translateLocation(prev.first))
        << "previous definition here";
    return failure();
  }

  symbolTable.insert(nameId, {loc, entry});
  return success();
}

/// Look up the specified name, emitting an error and returning null if the
/// name is unknown.
ParseResult FIRScopedParser::lookupSymbolEntry(SymbolValueEntry &result,
                                               StringRef name, SMLoc loc) {
  auto prev = symbolTable.lookup(Identifier::get(name, getContext()));
  if (!prev.first.isValid())
    return emitError(loc, "use of unknown declaration '" + name.str() + "'"),
           failure();
  result = prev.second;
  assert(result && "name in symbol table without definition");
  return success();
}

ParseResult FIRScopedParser::resolveSymbolEntry(Value &result,
                                                SymbolValueEntry &entry,
                                                SMLoc loc, bool fatal) {
  if (!entry.is<Value>()) {
    if (fatal)
      emitError(loc, "bundle value should only be used from subfield");
    return failure();
  }
  result = entry.get<Value>();
  return success();
}

ParseResult FIRScopedParser::resolveSymbolEntry(Value &result,
                                                SymbolValueEntry &entry,
                                                StringRef fieldName,
                                                SMLoc loc) {
  if (!entry.is<UnbundledID>()) {
    emitError(loc, "value should not be used from subfield");
    return failure();
  }

  auto fieldAttr = StringAttr::get(getContext(), fieldName);

  unsigned unbundledId = entry.get<UnbundledID>() - 1;
  assert(unbundledId < unbundledValues.size());
  UnbundledValueEntry &ubEntry = unbundledValues[unbundledId];
  for (auto elt : ubEntry) {
    if (elt.first == fieldAttr) {
      result = elt.second;
      break;
    }
  }
  if (!result) {
    emitError(loc, "use of invalid field name '")
        << fieldName << "' on bundle value";
    return failure();
  }

  return success();
}

//===----------------------------------------------------------------------===//
// FIRModuleContext
//===----------------------------------------------------------------------===//

namespace {
/// This struct provides context information that is global to the module we're
/// currently parsing into.
struct FIRModuleContext {
  // The expression-oriented nature of firrtl syntax produces tons of constant
  // nodes which are obviously redundant.  Instead of literally producing them
  // in the parser, do an implicit CSE to reduce parse time and silliness in the
  // resulting IR.
  llvm::DenseMap<std::pair<Attribute, Type>, Value> constantCache;
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
// FIRStmtParser
//===----------------------------------------------------------------------===//

namespace {
/// This class is used when building expression nodes for a statement: we need
/// to parse a bunch of expressions and build MLIR operations for them, and then
/// we see the locator that specifies the location for those operations
/// afterward.
///
/// It is wasteful in time and memory to create a bunch of temporary FileLineCol
/// location's that point into the .fir file when they're destined to get
/// overwritten by a location specified by a Locator.  To avoid this, we create
/// all of the operations with a temporary location on them, then remember the
/// [Operation*, SMLoc] pair for the newly created operation.
///
/// At the end of the operation we'll see a Locator (or not).  If we see a
/// locator, we apply it to all the operations we've parsed and we're done.  If
/// not, we lazily create the locators in the .fir file.
struct LazyLocationListener : public OpBuilder::Listener {
  LazyLocationListener(OpBuilder &builder) : builder(builder) {
    assert(builder.getListener() == nullptr);
    builder.setListener(this);
  }

  ~LazyLocationListener() {
    assert(subOps.empty() && "didn't process parsed operations");
    assert(builder.getListener() == this);
    builder.setListener(nullptr);
  }

  void startStatement() {
    assert(!isActive && "Already processing a statement");
    isActive = true;
  }

  /// This is called when done with each statement.  This applies the locations
  /// to each statement.
  void endStatement(FIRScopedParser &parser) {
    assert(isActive && "Not parsing a statement");

    // If we have a symbolic location, apply it to any subOps specified.
    if (infoLoc) {
      for (auto opAndSMLoc : subOps)
        opAndSMLoc.first->setLoc(infoLoc);
    } else {
      // If we don't, translate all the individual SMLoc's to Location objects
      // in the .fir file.
      for (auto opAndSMLoc : subOps)
        opAndSMLoc.first->setLoc(parser.translateLocation(opAndSMLoc.second));
    }

    // Reset our state.
    isActive = false;
    infoLoc = LocationAttr();
    currentSMLoc = SMLoc();
    subOps.clear();
  }

  /// Specify the location to be used for the next operations that are created.
  void setLoc(SMLoc loc) { currentSMLoc = loc; }

  /// When a @Info locator is parsed, this method captures it.
  void setInfoLoc(LocationAttr loc) {
    assert(!infoLoc && "Info location multiply specified");
    infoLoc = loc;
  }

  // Notification handler for when an operation is inserted into the builder.
  /// `op` is the operation that was inserted.
  void notifyOperationInserted(Operation *op) override {
    assert(currentSMLoc != SMLoc() && "No .fir file location specified");
    assert(isActive && "Not parsing a statement");
    subOps.push_back({op, currentSMLoc});
  }

private:
  /// This is set to true while parsing a statement.  It is used for assertions.
  bool isActive = false;

  /// This is the current position in the source file that the next operation
  /// will be parsed into.
  SMLoc currentSMLoc;

  /// This is the @ location attribute for the current statement, or null if
  /// not set.
  LocationAttr infoLoc;

  /// This is the builder we're installed into.
  OpBuilder &builder;

  /// This is the set of operations we've enqueued along with their location in
  /// the source file.
  SmallVector<std::pair<Operation *, SMLoc>, 8> subOps;

  void operator=(const LazyLocationListener &) = delete;
  LazyLocationListener(const LazyLocationListener &) = delete;
};
} // end anonymous namespace

namespace {
/// This class implements logic and state for parsing statements, suites, and
/// similar module body constructs.
struct FIRStmtParser : public FIRScopedParser {
  explicit FIRStmtParser(Block &blockToInsertInto,
                         FIRScopedParser &parentParser,
                         FIRModuleContext &moduleContext)
      : FIRScopedParser(parentParser.getState(), parentParser.symbolTable,
                        parentParser.unbundledValues,
                        parentParser.memoryScopeTable),
        builder(mlir::ImplicitLocOpBuilder::atBlockEnd(
            UnknownLoc::get(getContext()), &blockToInsertInto)),
        locationProcessor(this->builder), moduleContext(moduleContext) {}

  ParseResult parseSimpleStmt(unsigned stmtIndent);
  ParseResult parseSimpleStmtBlock(unsigned indent);

private:
  ParseResult parseSimpleStmtImpl(unsigned stmtIndent);

  /// Return the input operand if it has passive type, otherwise convert to
  /// a passive-typed value and return that.
  Value convertToPassive(Value input);

  /// Attach invalid values to every element of the value.
  void emitInvalidate(Value val, Flow flow);

  // The FIRRTL specification describes Invalidates as a statement with
  // implicit connect semantics.  The FIRRTL dialect models it as a primitive
  // that returns an "Invalid Value", followed by an explicit connect to make
  // the representation simpler and more consistent.
  void emitInvalidate(Value val) { emitInvalidate(val, foldFlow(val)); }

  /// Parse an @info marker if present and inform locationProcessor about it.
  ParseResult parseOptionalInfo() {
    LocationAttr loc;
    if (failed(parseOptionalInfoLocator(loc)))
      return failure();
    locationProcessor.setInfoLoc(loc);
    return success();
  }

  // Exp Parsing
  ParseResult parseExpImpl(Value &result, const Twine &message,
                           bool isLeadingStmt);
  ParseResult parseExp(Value &result, const Twine &message) {
    return parseExpImpl(result, message, /*isLeadingStmt:*/ false);
  }
  ParseResult parseExpLeadingStmt(Value &result, const Twine &message) {
    return parseExpImpl(result, message, /*isLeadingStmt:*/ true);
  }

  ParseResult parseOptionalExpPostscript(Value &result);
  ParseResult parsePostFixFieldId(Value &result);
  ParseResult parsePostFixIntSubscript(Value &result);
  ParseResult parsePostFixDynamicSubscript(Value &result);
  ParseResult parsePrimExp(Value &result);
  ParseResult parseIntegerLiteralExp(Value &result);

  Optional<ParseResult> parseExpWithLeadingKeyword(FIRToken keyword);

  // Stmt Parsing
  ParseResult parseAttach();
  ParseResult parseMemPort(MemDirAttr direction);
  ParseResult parsePrintf();
  ParseResult parseSkip();
  ParseResult parseStop();
  ParseResult parseAssert();
  ParseResult parseAssume();
  ParseResult parseCover();
  ParseResult parseWhen(unsigned whenIndent);
  ParseResult parseLeadingExpStmt(Value lhs);

  // Declarations
  ParseResult parseInstance();
  ParseResult parseCMem();
  ParseResult parseSMem();
  ParseResult parseMem(unsigned memIndent);
  ParseResult parseNode();
  ParseResult parseWire();
  ParseResult parseRegister(unsigned regIndent);

  // The builder to build into.
  ImplicitLocOpBuilder builder;
  LazyLocationListener locationProcessor;

  // Extra information maintained across a module.
  FIRModuleContext &moduleContext;
};

} // end anonymous namespace

/// Attach invalid values to every element of the value.
void FIRStmtParser::emitInvalidate(Value val, Flow flow) {
  // Strip an outer flip.  This is associated with an output port, but this
  // was already included in flow calculation.
  auto tpe = val.getType().cast<FIRRTLType>();
  if (auto flip = tpe.dyn_cast<FlipType>())
    tpe = flip.getElementType();

  // Recurse until we hit leaves.  Connect any leaves which have sink or
  // duplex flow.
  //
  // TODO: This is very similar to connect expansion in the LowerTypes pass
  // works.  Find a way to unify this with methods common to LowerTypes or to
  // have LowerTypes to the actual work here, e.g., emitting a partial connect
  // to only the leaf sources.
  TypeSwitch<FIRRTLType>(tpe)
      .Case<BundleType>([&](auto tpe) {
        for (auto element : tpe.getElements()) {
          auto subfield = builder.create<SubfieldOp>(val, element.name);
          emitInvalidate(subfield,
                         subfield.isFieldFlipped() ? swapFlow(flow) : flow);
        }
      })
      .Case<FVectorType>([&](auto tpe) {
        auto tpex = tpe.getElementType();
        for (size_t i = 0, e = tpe.getNumElements(); i != e; ++i)
          emitInvalidate(builder.create<SubindexOp>(tpex, val, i), flow);
      })
      // Drop invalidation of analog.
      .Case<AnalogType>([](auto) {})
      // Invalidate any sink or duplex flow ground types.
      .Default([&](auto tpe) {
        if (flow != Flow::Source)
          builder.create<ConnectOp>(val, builder.create<InvalidValueOp>(tpe));
      });
}

/// Return the input operand if it has passive type, otherwise convert to
/// a passive-typed value and return that.
Value FIRStmtParser::convertToPassive(Value input) {
  auto inType = input.getType().cast<FIRRTLType>();
  if (inType.isPassive() && foldFlow(input) != Flow::Sink)
    return input;

  return builder.create<AsPassivePrimOp>(inType.getPassiveType(), input);
}

//===-------------------------------
// FIRStmtParser Expression Parsing.

/// Parse the 'exp' grammar, returning all of the suboperations in the
/// specified vector, and the ultimate SSA value in value.
///
///  exp ::= id    // Ref
///      ::= prim
///      ::= integer-literal-exp
///      ::= exp '.' fieldId
///      ::= exp '[' intLit ']'
/// XX   ::= exp '.' DoubleLit // TODO Workaround for #470
///      ::= exp '[' exp ']'
///
///
/// If 'isLeadingStmt' is true, then this is being called to parse the first
/// expression in a statement.  We can handle some weird cases due to this if
/// we end up parsing the whole statement.  In that case we return success, but
/// set the 'result' value to null.
ParseResult FIRStmtParser::parseExpImpl(Value &result, const Twine &message,
                                        bool isLeadingStmt) {
  switch (getToken().getKind()) {

    // Handle all the primitive ops: primop exp* intLit*  ')'
#define TOK_LPKEYWORD(SPELLING) case FIRToken::lp_##SPELLING:
#include "FIRTokenKinds.def"
    if (parsePrimExp(result))
      return failure();
    break;

  case FIRToken::kw_UInt:
  case FIRToken::kw_SInt:
    if (parseIntegerLiteralExp(result))
      return failure();
    break;

    // Otherwise there are a bunch of keywords that are treated as identifiers
    // try them.
  case FIRToken::identifier: // exp ::= id
  default: {
    StringRef name;
    auto loc = getToken().getLoc();
    SymbolValueEntry symtabEntry;
    if (parseId(name, message) || lookupSymbolEntry(symtabEntry, name, loc))
      return failure();

    // If we looked up a normal value, then we're done.
    if (!resolveSymbolEntry(result, symtabEntry, loc, false))
      break;

    assert(symtabEntry.is<UnbundledID>() && "should be an instance");

    // Otherwise we referred to an implicitly bundled value.  We *must* be in
    // the midst of processing a field ID reference or 'is invalid'.  If not,
    // this is an error.
    if (isLeadingStmt && consumeIf(FIRToken::kw_is)) {
      if (parseToken(FIRToken::kw_invalid, "expected 'invalid'") ||
          parseOptionalInfo())
        return failure();

      locationProcessor.setLoc(loc);
      // Invalidate all of the results of the bundled value.
      unsigned unbundledId = symtabEntry.get<UnbundledID>() - 1;
      assert(unbundledId < unbundledValues.size());
      UnbundledValueEntry &ubEntry = unbundledValues[unbundledId];
      for (auto elt : ubEntry)
        emitInvalidate(elt.second);

      // Signify that we parsed the whole statement.
      result = Value();
      return success();
    }

    // Handle the normal "instance.x" reference.
    StringRef fieldName;
    if (parseToken(FIRToken::period, "expected '.' in field reference") ||
        parseFieldId(fieldName, "expected field name") ||
        resolveSymbolEntry(result, symtabEntry, fieldName, loc))
      return failure();
    break;
  }
  }

  return parseOptionalExpPostscript(result);
}

/// Parse the postfix productions of expression after the leading expression
/// has been parsed.
///
///  exp ::= exp '.' fieldId
///      ::= exp '[' intLit ']'
/// XX   ::= exp '.' DoubleLit // TODO Workaround for #470
///      ::= exp '[' exp ']'
ParseResult FIRStmtParser::parseOptionalExpPostscript(Value &result) {

  // Handle postfix expressions.
  while (1) {
    // Subfield: exp ::= exp '.' fieldId
    if (consumeIf(FIRToken::period)) {
      if (parsePostFixFieldId(result))
        return failure();

      continue;
    }

    // Subindex: exp ::= exp '[' intLit ']' | exp '[' exp ']'
    if (consumeIf(FIRToken::l_square)) {
      if (getToken().isAny(FIRToken::integer, FIRToken::string)) {
        if (parsePostFixIntSubscript(result))
          return failure();
        continue;
      }
      if (parsePostFixDynamicSubscript(result))
        return failure();

      continue;
    }

    return success();
  }
}

/// exp ::= exp '.' fieldId
///
/// The "exp '.'" part of the production has already been parsed.
///
ParseResult FIRStmtParser::parsePostFixFieldId(Value &result) {
  auto loc = getToken().getLoc();
  StringRef fieldName;
  if (parseFieldId(fieldName, "expected field name"))
    return failure();

  // Make sure the field name matches up with the input value's type and
  // compute the result type for the expression.
  NamedAttribute attrs = {getState().fieldnameIdentifier,
                          builder.getStringAttr(fieldName)};
  auto resultType = SubfieldOp::inferReturnType({result}, attrs, {});
  if (!resultType) {
    // Emit the error at the right location.  translateLocation is expensive.
    (void)SubfieldOp::inferReturnType({result}, attrs, translateLocation(loc));
    return failure();
  }

  // Create the result operation.
  locationProcessor.setLoc(loc);
  auto op = builder.create<SubfieldOp>(resultType, result, attrs);
  result = op.getResult();
  return success();
}

/// exp ::= exp '[' intLit ']'
///
/// The "exp '['" part of the production has already been parsed.
///
ParseResult FIRStmtParser::parsePostFixIntSubscript(Value &result) {
  auto loc = getToken().getLoc();
  int64_t indexNo;
  if (parseIntLit(indexNo, "expected index") ||
      parseToken(FIRToken::r_square, "expected ']'"))
    return failure();

  if (indexNo < 0)
    return emitError(loc, "invalid index specifier"), failure();

  // Make sure the index expression is valid and compute the result type for the
  // expression.
  // TODO: This should ideally be folded into a `tryCreate` method on the
  // builder (https://llvm.discourse.group/t/3504).
  NamedAttribute attrs = {getState().indexIdentifier,
                          builder.getI32IntegerAttr(indexNo)};
  auto resultType = SubindexOp::inferReturnType({result}, attrs, {});
  if (!resultType) {
    // Emit the error at the right location.  translateLocation is expensive.
    (void)SubindexOp::inferReturnType({result}, attrs, translateLocation(loc));
    return failure();
  }

  // Create the result operation.
  locationProcessor.setLoc(loc);
  auto op = builder.create<SubindexOp>(resultType, result, attrs);
  result = op.getResult();
  return success();
}

/// exp ::= exp '[' exp ']'
///
/// The "exp '['" part of the production has already been parsed.
///
ParseResult FIRStmtParser::parsePostFixDynamicSubscript(Value &result) {
  auto loc = getToken().getLoc();
  Value index;
  if (parseExp(index, "expected subscript index expression") ||
      parseToken(FIRToken::r_square, "expected ']' in subscript"))
    return failure();

  // If the index expression is a flip type, strip it off.
  auto indexType = index.getType().cast<FIRRTLType>();
  indexType = indexType.getPassiveType();
  locationProcessor.setLoc(loc);
  index = convertToPassive(index);

  // Make sure the index expression is valid and compute the result type for the
  // expression.
  auto resultType = SubaccessOp::inferReturnType({result, index}, {}, {});
  if (!resultType) {
    // Emit the error at the right location.  translateLocation is expensive.
    (void)SubaccessOp::inferReturnType({result, index}, {},
                                       translateLocation(loc));
    return failure();
  }

  // Create the result operation.
  auto op = builder.create<SubaccessOp>(resultType, result, index);
  result = op.getResult();
  return success();
}

/// prim ::= primop exp* intLit*  ')'
ParseResult FIRStmtParser::parsePrimExp(Value &result) {
  auto kind = getToken().getKind();
  auto loc = getToken().getLoc();
  consumeToken();

  // Parse the operands and constant integer arguments.
  SmallVector<Value, 4> operands;
  SmallVector<int64_t, 4> integers;
  if (parseListUntil(FIRToken::r_paren, [&]() -> ParseResult {
        // Handle the integer constant case if present.
        if (getToken().isAny(FIRToken::integer, FIRToken::signed_integer,
                             FIRToken::string)) {
          integers.push_back(0);
          return parseIntLit(integers.back(), "expected integer");
        }

        // Otherwise it must be a value operand.  These must all come before the
        // integers.
        if (!integers.empty())
          return emitError("expected more integer constants"), failure();

        Value operand;
        if (parseExp(operand, "expected expression in primitive operand"))
          return failure();

        locationProcessor.setLoc(loc);
        operand = convertToPassive(operand);

        operands.push_back(operand);
        return success();
      }))
    return failure();

  locationProcessor.setLoc(loc);

  SmallVector<FIRRTLType, 4> opTypes;
  for (auto v : operands)
    opTypes.push_back(v.getType().cast<FIRRTLType>());

  unsigned numOperandsExpected;
  SmallVector<Identifier, 2> attrNames;

  // Get information about the primitive in question.
  switch (kind) {
  default:
    emitError(loc, "primitive not supported yet");
    return failure();
  case FIRToken::lp_validif:
    numOperandsExpected = 2;
    break;
#define TOK_LPKEYWORD_PRIM(SPELLING, CLASS, NUMOPERANDS)                       \
  case FIRToken::lp_##SPELLING:                                                \
    numOperandsExpected = NUMOPERANDS;                                         \
    break;
#include "FIRTokenKinds.def"
  }
  // Don't add code here, we want these two switch statements to be fused by
  // the compiler.
  switch (kind) {
  default:
    break;
  case FIRToken::lp_bits:
    attrNames.push_back(getState().hiIdentifier); // "hi"
    attrNames.push_back(getState().loIdentifier); // "lo"
    break;
  case FIRToken::lp_head:
  case FIRToken::lp_pad:
  case FIRToken::lp_shl:
  case FIRToken::lp_shr:
  case FIRToken::lp_tail:
    attrNames.push_back(getState().amountIdentifier);
    break;
  }

  if (operands.size() != numOperandsExpected) {
    assert(numOperandsExpected <= 3);
    const char *numberName[] = {"zero", "one", "two", "three"};
    const char *optionalS = &"s"[numOperandsExpected == 1];
    return emitError(loc, "operation requires ")
           << numberName[numOperandsExpected] << " operand" << optionalS;
  }

  if (integers.size() != attrNames.size()) {
    emitError(loc, "expected ") << attrNames.size() << " constant arguments";
    return failure();
  }

  NamedAttrList attrs;
  for (size_t i = 0, e = attrNames.size(); i != e; ++i)
    attrs.append(attrNames[i], builder.getI32IntegerAttr(integers[i]));

  switch (kind) {
  default:
    emitError(loc, "primitive not supported yet");
    return failure();

#define TOK_LPKEYWORD_PRIM(SPELLING, CLASS, NUMOPERANDS)                       \
  case FIRToken::lp_##SPELLING: {                                              \
    auto resultTy = CLASS::inferReturnType(operands, attrs, {});               \
    if (!resultTy) {                                                           \
      /* only call translateLocation on an error case, it is expensive. */     \
      (void)CLASS::validateAndInferReturnType(operands, attrs,                 \
                                              translateLocation(loc));         \
      return failure();                                                        \
    }                                                                          \
    result = builder.create<CLASS>(resultTy, operands, attrs);                 \
    return success();                                                          \
  }
#include "FIRTokenKinds.def"

  // Expand `validif(a, b)` expressions to `mux(a, b, invalidvalue)`, since we
  // do not provide an operation for `validif`. See docs/RationaleFIRRTL.md for
  // more details on this.
  case FIRToken::lp_validif: {
    if (opTypes.size() != 2 || !integers.empty()) {
      emitError(loc, "operation requires two operands and no constants");
      return failure();
    }
    auto lhsUInt = opTypes[0].dyn_cast<UIntType>();
    if (!lhsUInt) {
      emitError(loc, "first operand should have UInt type");
      return failure();
    }
    auto lhsWidth = lhsUInt.getWidthOrSentinel();
    if (lhsWidth != -1 && lhsWidth != 1) {
      emitError(loc, "first operand should have 'uint<1>' type");
      return failure();
    }

    // We always skip emitting the validif mux because it always folds to the
    // non-invalid operand.
    result = operands[1];
    return success();
  }
  }

  llvm_unreachable("all cases should return");
}

/// integer-literal-exp ::= 'UInt' optional-width '(' intLit ')'
///                     ::= 'SInt' optional-width '(' intLit ')'
ParseResult FIRStmtParser::parseIntegerLiteralExp(Value &result) {
  bool isSigned = getToken().is(FIRToken::kw_SInt);
  auto loc = getToken().getLoc();
  consumeToken();

  // Parse a width specifier if present.
  int64_t width;
  APInt value;
  if (parseOptionalWidth(width) ||
      parseToken(FIRToken::l_paren, "expected '(' in integer expression") ||
      parseIntLit(value, "expected integer value") ||
      parseToken(FIRToken::r_paren, "expected ')' in integer expression"))
    return failure();

  if (width == 0)
    return emitError(loc, "zero bit constants are not allowed"), failure();

  // Construct an integer attribute of the right width.
  auto type = IntType::get(builder.getContext(), isSigned, width);

  IntegerType::SignednessSemantics signedness;
  if (type.isSigned()) {
    signedness = IntegerType::Signed;
    if (width != -1) {
      // Check for overlow if we are truncating bits.
      if (unsigned(width) < value.getBitWidth() &&
          value.getNumSignBits() <= value.getBitWidth() - width) {
        return emitError(loc, "initializer too wide for declared width"),
               failure();
      }

      value = value.sextOrTrunc(width);
    }
  } else {
    signedness = IntegerType::Unsigned;
    if (width != -1) {
      // Check for overlow if we are truncating bits.
      if (unsigned(width) < value.getBitWidth() &&
          value.countLeadingZeros() < value.getBitWidth() - width) {
        return emitError(loc, "initializer too wide for declared width"),
               failure();
      }
      value = value.zextOrTrunc(width);
    }
  }

  Type attrType =
      IntegerType::get(type.getContext(), value.getBitWidth(), signedness);
  auto attr = builder.getIntegerAttr(attrType, value);

  // Check to see if we've already created this constant.  If so, reuse it.
  auto &entry = moduleContext.constantCache[{attr, type}];
  if (entry) {
    // If we already had an entry, reuse it.
    result = entry;
    return success();
  }

  // Make sure to insert constants at the top level of the module to maintain
  // dominance.
  OpBuilder::InsertPoint savedIP;

  auto parentOp = builder.getInsertionBlock()->getParentOp();
  if (!isa<FModuleOp>(parentOp)) {
    savedIP = builder.saveInsertionPoint();
    while (!isa<FModuleOp>(parentOp)) {
      builder.setInsertionPoint(parentOp);
      parentOp = builder.getInsertionBlock()->getParentOp();
    }
  }

  locationProcessor.setLoc(loc);
  auto op = builder.create<ConstantOp>(type, value);
  entry = op;
  result = op;

  if (savedIP.isSet())
    builder.setInsertionPoint(savedIP.getBlock(), savedIP.getPoint());
  return success();
}

/// The .fir grammar has the annoying property where:
/// 1) some statements start with keywords
/// 2) some start with an expression
/// 3) it allows the 'reference' expression to either be an identifier or a
///    keyword.
///
/// One example of this is something like, where this is not a register decl:
///   reg <- thing
///
/// Solving this requires lookahead to the second token.  We handle it by
///  factoring the lookahead inline into the code to keep the parser fast.
///
/// As such, statements that start with a leading keyword call this method to
/// check to see if the keyword they consumed was actually the start of an
/// expression.  If so, they parse the expression-based statement and return the
/// parser result.  If not, they return None and the statement is parsed like
/// normal.
Optional<ParseResult>
FIRStmtParser::parseExpWithLeadingKeyword(FIRToken keyword) {
  switch (getToken().getKind()) {
  default:
    // This isn't part of an expression, and isn't part of a statement.
    return None;

  case FIRToken::period:     // exp `.` identifier
  case FIRToken::l_square:   // exp `[` index `]`
  case FIRToken::kw_is:      // exp is invalid
  case FIRToken::less_equal: // exp <= thing
  case FIRToken::less_minus: // exp <- thing
    break;
  }

  Value lhs;
  SymbolValueEntry symtabEntry;
  auto loc = keyword.getLoc();

  if (lookupSymbolEntry(symtabEntry, keyword.getSpelling(), loc))
    return ParseResult(failure());

  // If we have a '.', we might have a symbol or an expanded port.  If we
  // resolve to a symbol, use that, otherwise check for expanded bundles of
  // other ops.
  // Non '.' ops take the plain symbol path.
  if (resolveSymbolEntry(lhs, symtabEntry, loc, false)) {
    // Ok if the base name didn't resolve by itself, it might be part of an
    // expanded dot reference.  That doesn't work then we fail.
    if (!consumeIf(FIRToken::period))
      return ParseResult(failure());

    StringRef fieldName;
    if (parseFieldId(fieldName, "expected field name") ||
        resolveSymbolEntry(lhs, symtabEntry, fieldName, loc))
      return ParseResult(failure());
  }

  // Parse any further trailing things like "mem.x.y".
  if (parseOptionalExpPostscript(lhs))
    return ParseResult(failure());

  return parseLeadingExpStmt(lhs);
}
//===-----------------------------
// FIRStmtParser Statement Parsing

/// simple_stmt_block ::= simple_stmt*
ParseResult FIRStmtParser::parseSimpleStmtBlock(unsigned indent) {
  while (true) {
    // The outer level parser can handle these tokens.
    if (getToken().isAny(FIRToken::eof, FIRToken::error))
      return success();

    auto subIndent = getIndentation();
    if (!subIndent.hasValue())
      return emitError("expected statement to be on its own line"), failure();

    if (subIndent.getValue() <= indent)
      return success();

    // Let the statement parser handle this.
    if (parseSimpleStmt(subIndent.getValue()))
      return failure();
  }
}

ParseResult FIRStmtParser::parseSimpleStmt(unsigned stmtIndent) {
  locationProcessor.startStatement();
  auto result = parseSimpleStmtImpl(stmtIndent);
  locationProcessor.endStatement(*this);
  return result;
}

/// simple_stmt ::= stmt
///
/// stmt ::= attach
///      ::= memport
///      ::= printf
///      ::= skip
///      ::= stop
///      ::= when
///      ::= leading-exp-stmt
///
/// stmt ::= instance
///      ::= cmem | smem | mem
///      ::= node | wire
///      ::= register
///
ParseResult FIRStmtParser::parseSimpleStmtImpl(unsigned stmtIndent) {
  switch (getToken().getKind()) {
  // Statements.
  case FIRToken::kw_attach:
    return parseAttach();
  case FIRToken::kw_infer:
    return parseMemPort(MemDirAttr::Infer);
  case FIRToken::kw_read:
    return parseMemPort(MemDirAttr::Read);
  case FIRToken::kw_write:
    return parseMemPort(MemDirAttr::Write);
  case FIRToken::kw_rdwr:
    return parseMemPort(MemDirAttr::ReadWrite);
  case FIRToken::lp_printf:
    return parsePrintf();
  case FIRToken::kw_skip:
    return parseSkip();
  case FIRToken::lp_stop:
    return parseStop();
  case FIRToken::lp_assert:
    return parseAssert();
  case FIRToken::lp_assume:
    return parseAssume();
  case FIRToken::lp_cover:
    return parseCover();
  case FIRToken::kw_when:
    return parseWhen(stmtIndent);
  default: {
    // Statement productions that start with an expression.
    Value lhs;
    if (parseExpLeadingStmt(lhs, "unexpected token in module"))
      return failure();
    // We use parseExp in a special mode that can complete the entire stmt at
    // once in unusual cases.  If this happened, then we are done.
    if (!lhs)
      return success();

    return parseLeadingExpStmt(lhs);
  }

    // Declarations
  case FIRToken::kw_inst:
    return parseInstance();
  case FIRToken::kw_cmem:
    return parseCMem();
  case FIRToken::kw_smem:
    return parseSMem();
  case FIRToken::kw_mem:
    return parseMem(stmtIndent);
  case FIRToken::kw_node:
    return parseNode();
  case FIRToken::kw_wire:
    return parseWire();
  case FIRToken::kw_reg:
    return parseRegister(stmtIndent);
  }
}

/// attach ::= 'attach' '(' exp+ ')' info?
ParseResult FIRStmtParser::parseAttach() {
  auto startTok = consumeToken(FIRToken::kw_attach);

  // If this was actually the start of a connect or something else handle that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  if (parseToken(FIRToken::l_paren, "expected '(' after attach"))
    return failure();

  SmallVector<Value, 4> operands;
  do {
    operands.push_back({});
    if (parseExp(operands.back(), "expected operand in attach"))
      return failure();
  } while (!consumeIf(FIRToken::r_paren));

  if (parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  builder.create<AttachOp>(operands);
  return success();
}

/// stmt ::= mdir 'mport' id '=' id '[' exp ']' exp info?
/// mdir ::= 'infer' | 'read' | 'write' | 'rdwr'
///
ParseResult FIRStmtParser::parseMemPort(MemDirAttr direction) {
  auto mdirIndent = getIndentation();
  auto startTok = consumeToken();
  auto startLoc = startTok.getLoc();

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  StringRef memName;
  SymbolValueEntry memorySym;
  Value memory, indexExp, clock;
  if (parseToken(FIRToken::kw_mport, "expected 'mport' in memory port") ||
      parseId(id, "expected result name") ||
      parseToken(FIRToken::equal, "expected '=' in memory port") ||
      parseId(memName, "expected memory name") ||
      lookupSymbolEntry(memorySym, memName, startLoc) ||
      resolveSymbolEntry(memory, memorySym, startLoc) ||
      parseToken(FIRToken::l_square, "expected '[' in memory port") ||
      parseExp(indexExp, "expected index expression") ||
      parseToken(FIRToken::r_square, "expected ']' in memory port") ||
      parseExp(clock, "expected clock expression") || parseOptionalInfo())
    return failure();

  auto memVType = memory.getType().dyn_cast<FVectorType>();
  if (!memVType)
    return emitError(startLoc, "memory should have vector type");
  auto resultType = memVType.getElementType();

  ArrayAttr annotations = getAnnotations(getModuleTarget() + ">" + id);
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  locationProcessor.setLoc(startLoc);
  auto result = builder.create<MemoryPortOp>(
      resultType, memory, indexExp, clock, direction, name, annotations);

  // TODO(firrtl scala bug): If the next operation is a skip, just eat it if it
  // is at the same indent level as us.  This is a horrible hack on top of the
  // following hack to work around a Scala bug.
  auto nextIndent = getIndentation();
  if (getToken().is(FIRToken::kw_skip) && mdirIndent.hasValue() &&
      nextIndent.hasValue() && mdirIndent.getValue() == nextIndent.getValue()) {
    // End the location for the MemPort, and start the location processing for
    // the skip op.
    locationProcessor.endStatement(*this);
    locationProcessor.startStatement();
    if (parseSkip())
      return failure();

    nextIndent = getIndentation();
  }

  // TODO(firrtl scala bug): Chisel is creating invalid IR where the mports
  // are logically defining their name at the scope of the memory itself.  This
  // is a problem for us, because the index expression and clock may be defined
  // in a nested expression.  We can't really tell when this is happening
  //  without unbounded lookahead either.
  //
  // Fortunately, this seems to happen in very idiomatic cases, where the
  // mport happens at the end of a when block.  We detect this situation by
  // seeing if the next statement is indented less than our memport - if so,
  // this is the last statement at the end of the 'when' block.   Trigger a
  // hacky workaround just in this case.
  if (mdirIndent.hasValue() && nextIndent.hasValue() &&
      mdirIndent.getValue() > nextIndent.getValue()) {
    auto memNameId = Identifier::get(memName, getContext());

    // To make this even more gross, we have no efficient way to figure out
    // what scope a value lives in our scoped hash table.  We keep a shadow
    // table to track this.
    auto scopeAndOperation = memoryScopeTable.lookup(memNameId);
    if (!scopeAndOperation.first) {
      emitError(startLoc, "unknown memory '") << memNameId << "'";
      return failure();
    }

    // If we need to inject this name into a parent scope, then we have to do
    // some IR hackery.  Create a wire for the id name right before
    // the mem in question, inject its name into that scope, then connect
    // the output of the mport to it.
    if (scopeAndOperation.first != symbolTable.getCurScope()) {
      WireOp wireHack;
      /* inject the wire into an outer scope*/ {
        auto insertPoint = builder.saveInsertionPoint();
        builder.setInsertionPoint(scopeAndOperation.second);
        wireHack = builder.create<WireOp>(result.getType());
        builder.restoreInsertionPoint(insertPoint);
      }
      builder.create<ConnectOp>(wireHack, result);

      // Inject this the wire's name into the same scope as the memory.
      symbolTable.insertIntoScope(scopeAndOperation.first,
                                  Identifier::get(id, getContext()),
                                  {startLoc, SymbolValueEntry(wireHack)});
      return success();
    }
  }

  return addSymbolEntry(id, result, startLoc);
}

/// printf ::= 'printf(' exp exp StringLit exp* ')' info?
ParseResult FIRStmtParser::parsePrintf() {
  auto startTok = consumeToken(FIRToken::lp_printf);

  Value clock, condition;
  StringRef formatString;
  if (parseExp(clock, "expected clock expression in printf") ||
      parseExp(condition, "expected condition in printf") ||
      parseGetSpelling(formatString) ||
      parseToken(FIRToken::string, "expected format string in printf"))
    return failure();

  SmallVector<Value, 4> operands;
  while (!consumeIf(FIRToken::r_paren)) {
    operands.push_back({});
    if (parseExp(operands.back(), "expected operand in printf"))
      return failure();
  }

  if (parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());

  auto formatStrUnescaped = FIRToken::getStringValue(formatString);
  StringRef formatStringRef(formatStrUnescaped);
  if (formatStringRef.startswith("cover:")) {
    APInt constOne(1, 1, false);
    auto constTrue =
        builder.create<ConstantOp>(UIntType::get(getContext(), 1), constOne);
    builder.create<CoverOp>(clock, condition, constTrue,
                            builder.getStringAttr(formatStrUnescaped));
    return success();
  }
  if (formatStringRef.startswith("assert:")) {
    APInt constOne(1, 1, false);
    auto constTrue =
        builder.create<ConstantOp>(UIntType::get(getContext(), 1), constOne);
    builder.create<AssertOp>(clock, condition, constTrue,
                             builder.getStringAttr(formatStrUnescaped));
    return success();
  }
  if (formatStringRef.startswith("assume:")) {
    APInt constOne(1, 1, false);
    auto constTrue =
        builder.create<ConstantOp>(UIntType::get(getContext(), 1), constOne);
    builder.create<AssumeOp>(clock, condition, constTrue,
                             builder.getStringAttr(formatStrUnescaped));
    return success();
  }

  builder.create<PrintFOp>(clock, condition,
                           builder.getStringAttr(formatStrUnescaped), operands);
  return success();
}

/// skip ::= 'skip' info?
ParseResult FIRStmtParser::parseSkip() {
  auto startTok = consumeToken(FIRToken::kw_skip);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  if (parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  builder.create<SkipOp>();
  return success();
}

/// stop ::= 'stop(' exp exp intLit ')' info?
ParseResult FIRStmtParser::parseStop() {
  auto startTok = consumeToken(FIRToken::lp_stop);

  Value clock, condition;
  int64_t exitCode;
  if (parseExp(clock, "expected clock expression in 'stop'") ||
      parseExp(condition, "expected condition in 'stop'") ||
      parseIntLit(exitCode, "expected exit code in 'stop'") ||
      parseToken(FIRToken::r_paren, "expected ')' in 'stop'") ||
      parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  builder.create<StopOp>(clock, condition, builder.getI32IntegerAttr(exitCode));
  return success();
}

/// assert ::= 'assert(' exp exp exp StringLit ')' info?
ParseResult FIRStmtParser::parseAssert() {
  auto startTok = consumeToken(FIRToken::lp_assert);

  Value clock, predicate, enable;
  StringRef message;
  if (parseExp(clock, "expected clock expression in 'assert'") ||
      parseExp(predicate, "expected predicate in 'assert'") ||
      parseExp(enable, "expected enable in 'assert'") ||
      parseGetSpelling(message) ||
      parseToken(FIRToken::string, "expected message in 'assert'") ||
      parseToken(FIRToken::r_paren, "expected ')' in 'assert'") ||
      parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  auto messageUnescaped = FIRToken::getStringValue(message);
  builder.create<AssertOp>(clock, predicate, enable,
                           builder.getStringAttr(messageUnescaped));
  return success();
}

/// assume ::= 'assume(' exp exp exp StringLit ')' info?
ParseResult FIRStmtParser::parseAssume() {
  auto startTok = consumeToken(FIRToken::lp_assume);

  Value clock, predicate, enable;
  StringRef message;
  if (parseExp(clock, "expected clock expression in 'assume'") ||
      parseExp(predicate, "expected predicate in 'assume'") ||
      parseExp(enable, "expected enable in 'assume'") ||
      parseGetSpelling(message) ||
      parseToken(FIRToken::string, "expected message in 'assume'") ||
      parseToken(FIRToken::r_paren, "expected ')' in 'assume'") ||
      parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  auto messageUnescaped = FIRToken::getStringValue(message);
  builder.create<AssumeOp>(clock, predicate, enable,
                           builder.getStringAttr(messageUnescaped));
  return success();
}

/// cover ::= 'cover(' exp exp exp StringLit ')' info?
ParseResult FIRStmtParser::parseCover() {
  auto startTok = consumeToken(FIRToken::lp_cover);

  Value clock, predicate, enable;
  StringRef message;
  if (parseExp(clock, "expected clock expression in 'cover'") ||
      parseExp(predicate, "expected predicate in 'cover'") ||
      parseExp(enable, "expected enable in 'cover'") ||
      parseGetSpelling(message) ||
      parseToken(FIRToken::string, "expected message in 'cover'") ||
      parseToken(FIRToken::r_paren, "expected ')' in 'cover'") ||
      parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  auto messageUnescaped = FIRToken::getStringValue(message);
  builder.create<CoverOp>(clock, predicate, enable,
                          builder.getStringAttr(messageUnescaped));
  return success();
}

/// when  ::= 'when' exp ':' info? suite? ('else' ( when | ':' info? suite?)
/// )? suite ::= simple_stmt | INDENT simple_stmt+ DEDENT
ParseResult FIRStmtParser::parseWhen(unsigned whenIndent) {
  auto startTok = consumeToken(FIRToken::kw_when);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  Value condition;
  if (parseExp(condition, "expected condition in 'when'") ||
      parseToken(FIRToken::colon, "expected ':' in when") ||
      parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  condition = convertToPassive(condition);

  // Create the IR representation for the when.
  auto whenStmt = builder.create<WhenOp>(condition, /*createElse*/ false);

  // This is a function to parse a suite body.
  auto parseSuite = [&](Block &blockToInsertInto) -> ParseResult {
    // Declarations within the suite are scoped to within the suite.
    SymbolTable::ScopeTy suiteScope(symbolTable);
    MemoryScopeTable::ScopeTy suiteMemoryScope(memoryScopeTable);

    // After parsing the when region, we can release any new entries in
    // unbundledValues since the symbol table entries that refer to them will be
    // gone.
    struct UnbundledValueRestorer {
      UnbundledValuesList &list;
      size_t startingSize;
      UnbundledValueRestorer(UnbundledValuesList &list) : list(list) {
        startingSize = list.size();
      }
      ~UnbundledValueRestorer() { list.resize(startingSize); }
    } x(unbundledValues);

    // We parse the substatements into their own parser, so they get inserted
    // into the specified 'when' region.
    FIRStmtParser subParser(blockToInsertInto, *this, moduleContext);

    // Figure out whether the body is a single statement or a nested one.
    auto stmtIndent = getIndentation();

    // Parsing a single statment is straightforward.
    if (!stmtIndent.hasValue())
      return subParser.parseSimpleStmt(whenIndent);

    if (stmtIndent.getValue() <= whenIndent)
      return emitError("statement must be indented more than 'when'"),
             failure();

    // Parse a block of statements that are indented more than the when.
    return subParser.parseSimpleStmtBlock(whenIndent);
  };

  // Parse the 'then' body into the 'then' region.
  if (parseSuite(whenStmt.getThenBlock()))
    return failure();

  // If the else is present, handle it otherwise we're done.
  if (getToken().isNot(FIRToken::kw_else))
    return success();

  // If the 'else' is less indented than the when, then it must belong to some
  // containing 'when'.
  auto elseIndent = getIndentation();
  if (elseIndent.hasValue() && elseIndent.getValue() < whenIndent)
    return success();

  consumeToken(FIRToken::kw_else);

  // Create an else block to parse into.
  whenStmt.createElseRegion();

  // If we have the ':' form, then handle it.

  // Syntactic shorthand 'else when'. This uses the same indentation level as
  // the outer 'when'.
  if (getToken().is(FIRToken::kw_when)) {
    // We create a sub parser for the else block.
    FIRStmtParser subParser(whenStmt.getElseBlock(), *this, moduleContext);
    return subParser.parseSimpleStmt(whenIndent);
  }

  // Parse the 'else' body into the 'else' region.
  LocationAttr elseLoc; // ignore the else locator.
  if (parseToken(FIRToken::colon, "expected ':' after 'else'") ||
      parseOptionalInfoLocator(elseLoc) || parseSuite(whenStmt.getElseBlock()))
    return failure();

  // TODO(firrtl spec): There is no reason for the 'else :' grammar to take an
  // info.  It doesn't appear to be generated either.
  return success();
}

/// leading-exp-stmt ::= exp '<=' exp info?
///                  ::= exp '<-' exp info?
///                  ::= exp 'is' 'invalid' info?
ParseResult FIRStmtParser::parseLeadingExpStmt(Value lhs) {
  auto loc = getToken().getLoc();

  // If 'is' grammar is special.
  if (consumeIf(FIRToken::kw_is)) {
    if (parseToken(FIRToken::kw_invalid, "expected 'invalid'") ||
        parseOptionalInfo())
      return failure();

    locationProcessor.setLoc(loc);
    emitInvalidate(lhs);
    return success();
  }

  auto kind = getToken().getKind();
  if (getToken().isNot(FIRToken::less_equal, FIRToken::less_minus))
    return emitError("expected '<=', '<-', or 'is' in statement"), failure();
  consumeToken();

  Value rhs;
  if (parseExp(rhs, "unexpected token in statement") || parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(loc);

  if (kind == FIRToken::less_equal) {
    // Some operations, dshl for example, have implicit truncations, even in lo
    // firrtl.  Chisel will also use connects as partial connects to do
    // truncation.  Handle truncations as partial connects, which allow
    // truncation.
    auto lhsPType = lhs.getType().cast<FIRRTLType>().getPassiveType();
    auto rhsPType = rhs.getType().cast<FIRRTLType>().getPassiveType();
    if (lhsPType != rhsPType && lhsPType.getBitWidthOrSentinel() >= 0 &&
        lhsPType.getBitWidthOrSentinel() < rhsPType.getBitWidthOrSentinel()) {
      builder.create<PartialConnectOp>(lhs, rhs);
    } else {
      builder.create<ConnectOp>(lhs, rhs);
    }
  } else {
    assert(kind == FIRToken::less_minus && "unexpected kind");
    builder.create<PartialConnectOp>(lhs, rhs);
  }
  return success();
}

//===-------------------------------
// FIRStmtParser Declaration Parsing

/// instance ::= 'inst' id 'of' id info?
ParseResult FIRStmtParser::parseInstance() {
  auto startTok = consumeToken(FIRToken::kw_inst);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  StringRef moduleName;
  if (parseId(id, "expected instance name") ||
      parseToken(FIRToken::kw_of, "expected 'of' in instance") ||
      parseId(moduleName, "expected module name") || parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());

  // Look up the module that is being referenced.
  auto circuit =
      builder.getBlock()->getParentOp()->getParentOfType<CircuitOp>();
  auto referencedModule = circuit.lookupSymbol(moduleName);
  if (!referencedModule) {
    emitError(startTok.getLoc(),
              "use of undefined module name '" + moduleName + "' in instance");
    return failure();
  }

  SmallVector<ModulePortInfo> modulePorts = getModulePortInfo(referencedModule);

  // Make a bundle of the inputs and outputs of the specified module.
  SmallVector<Type, 4> resultTypes;
  resultTypes.reserve(modulePorts.size());

  for (auto port : modulePorts) {
    auto portType = port.type;
    if (port.direction == Direction::Input)
      portType = FlipType::get(portType);
    resultTypes.push_back(portType);
  }
  ArrayAttr annotations = getAnnotations(getModuleTarget() + ">" + id);
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  auto result =
      builder.create<InstanceOp>(resultTypes, moduleName, name, annotations);

  // Since we are implicitly unbundling the instance results, we need to keep
  // track of the mapping from bundle fields to results in the unbundledValues
  // data structure.  Build our entry now.
  UnbundledValueEntry unbundledValueEntry;
  unbundledValueEntry.reserve(modulePorts.size());
  for (size_t i = 0, e = modulePorts.size(); i != e; ++i)
    unbundledValueEntry.push_back({modulePorts[i].name, result.getResult(i)});

  // Add it to unbundledValues and add an entry to the symbol table to remember
  // it.
  unbundledValues.push_back(std::move(unbundledValueEntry));
  auto entryId = UnbundledID(unbundledValues.size());
  return addSymbolEntry(id, entryId, startTok.getLoc());
}

/// cmem ::= 'cmem' id ':' type info?
ParseResult FIRStmtParser::parseCMem() {
  // TODO(firrtl spec) cmem is completely undocumented.
  auto startTok = consumeToken(FIRToken::kw_cmem);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  FIRRTLType type;
  if (parseId(id, "expected cmem name") ||
      parseToken(FIRToken::colon, "expected ':' in cmem") ||
      parseType(type, "expected cmem type") || parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());

  ArrayAttr annotations = getAnnotations(getModuleTarget() + ">" + id);
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  auto result = builder.create<CMemOp>(type, name, annotations);

  // Remember that this memory is in this symbol table scope.
  // TODO(chisel bug): This should be removed along with memoryScopeTable.
  memoryScopeTable.insert(Identifier::get(id, getContext()),
                          {symbolTable.getCurScope(), result.getOperation()});

  return addSymbolEntry(id, result, startTok.getLoc());
}

/// smem ::= 'smem' id ':' type ruw? info?
ParseResult FIRStmtParser::parseSMem() {
  // TODO(firrtl spec) smem is completely undocumented.
  auto startTok = consumeToken(FIRToken::kw_smem);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  FIRRTLType type;
  RUWAttr ruw = RUWAttr::Undefined;

  if (parseId(id, "expected cmem name") ||
      parseToken(FIRToken::colon, "expected ':' in cmem") ||
      parseType(type, "expected cmem type") || parseOptionalRUW(ruw) ||
      parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());

  ArrayAttr annotations = getAnnotations(getModuleTarget() + ">" + id);
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  auto result = builder.create<SMemOp>(type, ruw, name, annotations);

  // Remember that this memory is in this symbol table scope.
  // TODO(chisel bug): This should be removed along with memoryScopeTable.
  memoryScopeTable.insert(Identifier::get(id, getContext()),
                          {symbolTable.getCurScope(), result.getOperation()});

  return addSymbolEntry(id, result, startTok.getLoc());
}

/// mem ::= 'mem' id ':' info? INDENT memField* DEDENT
/// memField ::= 'data-type' '=>' type NEWLINE
/// 	       ::= 'depth' '=>' intLit NEWLINE
/// 	       ::= 'read-latency' '=>' intLit NEWLINE
/// 	       ::= 'write-latency' '=>' intLit NEWLINE
/// 	       ::= 'read-under-write' '=>' ruw NEWLINE
/// 	       ::= 'reader' '=>' id+ NEWLINE
/// 	       ::= 'writer' '=>' id+ NEWLINE
/// 	       ::= 'readwriter' '=>' id+ NEWLINE
ParseResult FIRStmtParser::parseMem(unsigned memIndent) {
  auto startTok = consumeToken(FIRToken::kw_mem);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  if (parseId(id, "expected mem name") ||
      parseToken(FIRToken::colon, "expected ':' in mem") || parseOptionalInfo())
    return failure();

  FIRRTLType type;
  int64_t depth = -1, readLatency = -1, writeLatency = -1;
  RUWAttr ruw = RUWAttr::Undefined;

  SmallVector<std::pair<StringAttr, BundleType>, 4> ports;

  // Parse all the memfield records, which are indented more than the mem.
  while (1) {
    auto nextIndent = getIndentation();
    if (!nextIndent.hasValue() || nextIndent.getValue() <= memIndent)
      break;

    auto spelling = getTokenSpelling();
    if (parseToken(FIRToken::identifier, "unexpected token in 'mem'") ||
        parseToken(FIRToken::equal_greater, "expected '=>' in 'mem'"))
      return failure();

    if (spelling == "data-type") {
      if (type)
        return emitError("'mem' type specified multiple times"), failure();

      if (parseType(type, "expected type in data-type declaration"))
        return failure();
      continue;
    }
    if (spelling == "depth") {
      if (parseIntLit(depth, "expected integer in depth specification"))
        return failure();
      continue;
    }
    if (spelling == "read-latency") {
      if (parseIntLit(readLatency, "expected integer latency"))
        return failure();
      continue;
    }
    if (spelling == "write-latency") {
      if (parseIntLit(writeLatency, "expected integer latency"))
        return failure();
      continue;
    }
    if (spelling == "read-under-write") {
      if (getToken().isNot(FIRToken::kw_old, FIRToken::kw_new,
                           FIRToken::kw_undefined))
        return emitError("expected specifier"), failure();

      if (parseOptionalRUW(ruw))
        return failure();
      continue;
    }

    MemOp::PortKind portKind;
    if (spelling == "reader")
      portKind = MemOp::PortKind::Read;
    else if (spelling == "writer")
      portKind = MemOp::PortKind::Write;
    else if (spelling == "readwriter")
      portKind = MemOp::PortKind::ReadWrite;
    else
      return emitError("unexpected field in 'mem' declaration"), failure();

    StringRef portName;
    if (parseId(portName, "expected port name"))
      return failure();
    ports.push_back({builder.getStringAttr(portName),
                     MemOp::getTypeForPort(depth, type, portKind)});

    while (!getIndentation().hasValue()) {
      if (parseId(portName, "expected port name"))
        return failure();
      ports.push_back({builder.getStringAttr(portName),
                       MemOp::getTypeForPort(depth, type, portKind)});
    }
  }

  // The FIRRTL dialect requires mems to have at least one port.  Since portless
  // mems can never be referenced, it is always safe to drop them.
  if (ports.empty())
    return success();

  // Canonicalize the ports into alphabetical order.
  // TODO: Move this into MemOp construction/canonicalization.
  llvm::array_pod_sort(ports.begin(), ports.end(),
                       [](const std::pair<StringAttr, BundleType> *lhs,
                          const std::pair<StringAttr, BundleType> *rhs) -> int {
                         return lhs->first.getValue().compare(
                             rhs->first.getValue());
                       });

  SmallVector<Attribute, 4> resultNames;
  SmallVector<Type, 4> resultTypes;
  for (auto p : ports) {
    resultNames.push_back(p.first);
    resultTypes.push_back(FlipType::get(p.second));
  }

  ArrayAttr annotations = getAnnotations(getModuleTarget() + ">" + id);
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  locationProcessor.setLoc(startTok.getLoc());

  auto result = builder.create<MemOp>(
      resultTypes, readLatency, writeLatency, depth, ruw,
      builder.getArrayAttr(resultNames), name, annotations);

  UnbundledValueEntry unbundledValueEntry;
  unbundledValueEntry.reserve(result.getNumResults());
  for (size_t i = 0, e = result.getNumResults(); i != e; ++i)
    unbundledValueEntry.push_back({resultNames[i], result.getResult(i)});

  unbundledValues.push_back(std::move(unbundledValueEntry));
  auto entryID = UnbundledID(unbundledValues.size());

  // Remember that this memory is in this symbol table scope.
  // TODO(chisel bug): This should be removed along with memoryScopeTable.
  memoryScopeTable.insert(Identifier::get(id, getContext()),
                          {symbolTable.getCurScope(), result.getOperation()});

  return addSymbolEntry(id, entryID, startTok.getLoc());
}

/// node ::= 'node' id '=' exp info?
ParseResult FIRStmtParser::parseNode() {
  auto startTok = consumeToken(FIRToken::kw_node);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  Value initializer;
  if (parseId(id, "expected node name") ||
      parseToken(FIRToken::equal, "expected '=' in node") ||
      parseExp(initializer, "expected expression for node") ||
      parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());

  // Error out in the following conditions:
  //
  //   1. Node type is Analog (at the top level)
  //   2. Node type is not passive under an optional outer flip
  //      (analog field is okay)
  //
  // Note: (1) is more restictive than normal NodeOp verification, but
  // this is added to align with the SFC. (2) is less restrictive than
  // the SFC to accomodate for situations where the node is something
  // weird like a module output or an instance input. This one
  // situation is cleaned up with 'convertToPassive' following.
  auto initializerType = initializer.getType().cast<FIRRTLType>();
  if (initializerType.isa<AnalogType>() ||
      (!initializerType.isPassive() && !initializerType.isa<FlipType>())) {
    emitError(startTok.getLoc())
        << "Node cannot be analog and must be passive or passive under a flip "
        << initializer.getType();
    return failure();
  }

  // If the node type isn't passive (it contains an outer flip), then make it
  // passive.
  initializer = convertToPassive(initializer);

  ArrayAttr annotations = getAnnotations(getModuleTarget() + ">" + id);

  // Ignore useless names like _T.
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  // The entire point of a node declaration is to carry a name.  If it got
  // dropped, then we don't even need to create a result unless it is annotated.
  Value result;
  if (!name.empty() || !annotations.empty()) {
    result = builder.create<NodeOp>(initializer.getType(), initializer, name,
                                    annotations);
  } else
    result = initializer;
  return addSymbolEntry(id, result, startTok.getLoc());
}

/// wire ::= 'wire' id ':' type info?
ParseResult FIRStmtParser::parseWire() {
  auto startTok = consumeToken(FIRToken::kw_wire);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  FIRRTLType type;
  if (parseId(id, "expected wire name") ||
      parseToken(FIRToken::colon, "expected ':' in wire") ||
      parseType(type, "expected wire type") || parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  ArrayAttr annotations = getAnnotations(getModuleTarget() + ">" + id);
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  auto result = builder.create<WireOp>(type, name, annotations);
  return addSymbolEntry(id, result, startTok.getLoc());
}

/// register    ::= 'reg' id ':' type exp ('with' ':' reset_block)? info?
///
/// reset_block ::= INDENT simple_reset info? NEWLINE DEDENT
///             ::= '(' simple_reset ')'
///
/// simple_reset ::= simple_reset0
///              ::= '(' simple_reset0 ')'
///
/// simple_reset0:  'reset' '=>' '(' exp exp ')'
///
ParseResult FIRStmtParser::parseRegister(unsigned regIndent) {
  auto startTok = consumeToken(FIRToken::kw_reg);

  // If this was actually the start of a connect or something else handle
  // that.
  if (auto isExpr = parseExpWithLeadingKeyword(startTok))
    return isExpr.getValue();

  StringRef id;
  FIRRTLType type;
  Value clock;

  // TODO(firrtl spec): info? should come after the clock expression before
  // the 'with'.
  if (parseId(id, "expected reg name") ||
      parseToken(FIRToken::colon, "expected ':' in reg") ||
      parseType(type, "expected reg type") ||
      parseExp(clock, "expected expression for register clock"))
    return failure();

  // Parse the 'with' specifier if present.
  Value resetSignal, resetValue;
  if (consumeIf(FIRToken::kw_with)) {
    if (parseToken(FIRToken::colon, "expected ':' in reg"))
      return failure();

    // TODO(firrtl spec): Simplify the grammar for register reset logic.
    // Why allow multiple ambiguous parentheses?  Why rely on indentation at
    // all?

    // This implements what the examples have in practice.
    bool hasExtraLParen = consumeIf(FIRToken::l_paren);

    auto indent = getIndentation();
    if (!indent.hasValue() || indent.getValue() <= regIndent)
      if (!hasExtraLParen)
        return emitError("expected indented reset specifier in reg"), failure();

    if (parseToken(FIRToken::kw_reset, "expected 'reset' in reg") ||
        parseToken(FIRToken::equal_greater, "expected => in reset specifier") ||
        parseToken(FIRToken::l_paren, "expected '(' in reset specifier") ||
        parseExp(resetSignal, "expected expression for reset signal"))
      return failure();

    // The Scala implementation of FIRRTL represents registers without resets
    // as a self referential register... and the pretty printer doesn't print
    // the right form. Recognize that this is happening and treat it as a
    // register without a reset for compatibility.
    // TODO(firrtl scala impl): pretty print registers without resets right.
    if (getTokenSpelling() == id) {
      consumeToken();
      if (parseToken(FIRToken::r_paren, "expected ')' in reset specifier"))
        return failure();
      resetSignal = Value();
    } else {
      if (parseExp(resetValue, "expected expression for reset value") ||
          parseToken(FIRToken::r_paren, "expected ')' in reset specifier"))
        return failure();
    }

    if (hasExtraLParen &&
        parseToken(FIRToken::r_paren, "expected ')' in reset specifier"))
      return failure();
  }

  // Finally, handle the last info if present, providing location info for the
  // clock expression.
  if (parseOptionalInfo())
    return failure();

  locationProcessor.setLoc(startTok.getLoc());
  clock = convertToPassive(clock);
  if (resetSignal)
    resetSignal = convertToPassive(resetSignal);
  if (resetValue)
    resetValue = convertToPassive(resetValue);

  ArrayAttr annotations = getAnnotations(getModuleTarget() + ">" + id);
  auto name = hasDontTouch(annotations) ? id : filterUselessName(id);

  Value result;
  if (resetSignal)
    result = builder.create<RegResetOp>(type, clock, resetSignal, resetValue,
                                        name, annotations);
  else
    result = builder.create<RegOp>(type, clock, name, annotations);

  return addSymbolEntry(id, result, startTok.getLoc());
}

//===----------------------------------------------------------------------===//
// FIRModuleParser
//===----------------------------------------------------------------------===//

namespace {
/// This class implements logic and state for parsing module bodies.
struct FIRModuleParser : public FIRScopedParser {
  explicit FIRModuleParser(GlobalFIRParserState &state, CircuitOp circuit)
      : FIRScopedParser(state, symbolTable, unbundledValueList,
                        memoryScopeTable),
        circuit(circuit), firstScope(symbolTable),
        firstMemoryScope(memoryScopeTable) {}

  ParseResult parseModule(unsigned indent);

private:
  using PortInfoAndLoc = std::pair<ModulePortInfo, SMLoc>;
  ParseResult parsePortList(SmallVectorImpl<PortInfoAndLoc> &result,
                            unsigned indent);

  CircuitOp circuit;
  SymbolTable symbolTable;
  SymbolTable::ScopeTy firstScope;

  UnbundledValuesList unbundledValueList;

  MemoryScopeTable memoryScopeTable;
  MemoryScopeTable::ScopeTy firstMemoryScope;
};

} // end anonymous namespace

/// pohwist ::= port*
/// port     ::= dir id ':' type info? NEWLINE
/// dir      ::= 'input' | 'output'
///
ParseResult
FIRModuleParser::parsePortList(SmallVectorImpl<PortInfoAndLoc> &result,
                               unsigned indent) {
  // Parse any ports.
  while (getToken().isAny(FIRToken::kw_input, FIRToken::kw_output) &&
         // Must be nested under the module.
         getIndentation() > indent) {

    // We need one token lookahead to resolve the ambiguity between:
    // output foo             ; port
    // output <= input        ; identifier expression
    // output.thing <= input  ; identifier expression
    auto backtrackState = getState().getBacktrackState();

    bool isOutput = getToken().is(FIRToken::kw_output);
    consumeToken();

    // If we have something that isn't a keyword then this must be an
    // identifier, not an input/output marker.
    if (!getToken().is(FIRToken::identifier) && !getToken().isKeyword()) {
      backtrackState.backtrack();
      break;
    }

    StringAttr name;
    FIRRTLType type;
    LocWithInfo info(getToken().getLoc(), this);
    if (parseId(name, "expected port name") ||
        parseToken(FIRToken::colon, "expected ':' in port definition") ||
        parseType(type, "expected a type in port declaration") ||
        info.parseOptionalInfo())
      return failure();

    AnnotationSet annotations(
        getAnnotations(getModuleTarget() + ">" + name.getValue()));

    // FIXME: We should persist the info loc into the IR, not just the name
    // and type.
    result.push_back({{name, type, direction::get(isOutput), annotations},
                      info.getFIRLoc()});
  }

  return success();
}

/// Parse an external or present module depending on what token we have.
///
/// module ::= 'module' id ':' info? INDENT pohwist simple_stmt_block DEDENT
/// module ::=
///        'extmodule' id ':' info? INDENT pohwist defname? parameter* DEDENT
/// defname   ::= 'defname' '=' id NEWLINE
///
/// parameter ::= 'parameter' id '=' intLit NEWLINE
/// parameter ::= 'parameter' id '=' StringLit NEWLINE
/// parameter ::= 'parameter' id '=' floatingpoint NEWLINE
/// parameter ::= 'parameter' id '=' RawString NEWLINE
ParseResult FIRModuleParser::parseModule(unsigned indent) {
  bool isExtModule = getToken().is(FIRToken::kw_extmodule);
  consumeToken();
  StringAttr name;
  SmallVector<PortInfoAndLoc, 4> portListAndLoc;

  LocWithInfo info(getToken().getLoc(), this);
  if (parseId(name, "expected module name"))
    return failure();

  auto moduleTarget = (getState().circuitTarget + "|" + name.getValue()).str();
  getState().moduleTarget = moduleTarget;

  if (parseToken(FIRToken::colon, "expected ':' in module definition") ||
      info.parseOptionalInfo() || parsePortList(portListAndLoc, indent))
    return failure();

  auto builder = circuit.getBodyBuilder();

  // Create the module.
  SmallVector<ModulePortInfo, 4> portList;
  portList.reserve(portListAndLoc.size());
  for (auto &elt : portListAndLoc)
    portList.push_back(elt.first);

  // If this is a normal module, parse the body into an FModuleOp.
  if (!isExtModule) {
    ArrayAttr annotations = getAnnotations(moduleTarget);
    auto fmodule =
        builder.create<FModuleOp>(info.getLoc(), name, portList, annotations);

    // Install all of the ports into the symbol table, associated with their
    // block arguments.
    auto argIt = fmodule.args_begin();
    for (auto &entry : portListAndLoc) {
      if (addSymbolEntry(entry.first.getName(), *argIt, entry.second))
        return failure();
      ++argIt;
    }

    FIRModuleContext moduleContext;
    FIRStmtParser stmtParser(*fmodule.getBodyBlock(), *this, moduleContext);

    // Parse the moduleBlock.
    return stmtParser.parseSimpleStmtBlock(indent);
  }

  // Otherwise, handle extmodule specific features like parameters.

  // Add all the ports to the symbol table even though there are no SSA values
  // for arguments in an external module.  This detects multiple definitions
  // of the same name.
  for (auto &entry : portListAndLoc) {
    if (addSymbolEntry(entry.first.getName(), Value(), entry.second))
      return failure();
  }

  // Parse a defname if present.
  // TODO(firrtl spec): defname isn't documented at all, what is it?
  StringRef defName;
  if (consumeIf(FIRToken::kw_defname)) {
    if (parseToken(FIRToken::equal, "expected '=' in defname") ||
        parseId(defName, "expected defname name"))
      return failure();
  }

  NamedAttrList parameters;
  SmallPtrSet<Identifier, 8> seenNames;

  // Parse the parameter list.
  while (consumeIf(FIRToken::kw_parameter)) {
    auto loc = getToken().getLoc();
    StringRef paramName;
    if (parseId(paramName, "expected parameter name") ||
        parseToken(FIRToken::equal, "expected '=' in parameter"))
      return failure();

    Attribute value;
    switch (getToken().getKind()) {
    default:
      return emitError("expected parameter value"), failure();

    case FIRToken::integer:
    case FIRToken::signed_integer: {
      APInt result;
      if (parseIntLit(result, "invalid integer parameter"))
        return failure();

      value = builder.getIntegerAttr(
          builder.getIntegerType(result.getBitWidth()), result);
      break;
    }
    case FIRToken::string: {
      // Drop the quotes and unescape.
      value = builder.getStringAttr(getToken().getStringValue());
      consumeToken(FIRToken::string);
      break;
    }

    case FIRToken::floatingpoint:
      double v;
      if (!llvm::to_float(getTokenSpelling(), v))
        return emitError("invalid float parameter syntax"), failure();

      value = builder.getF64FloatAttr(v);
      consumeToken(FIRToken::floatingpoint);
      break;
    }

    auto nameId = builder.getIdentifier(paramName);
    if (!seenNames.insert(nameId).second)
      return emitError(loc, "redefinition of parameter '" + paramName + "'");
    parameters.append(nameId, value);
  }

  ArrayAttr annotations = getAnnotations(moduleTarget);
  auto fmodule = builder.create<FExtModuleOp>(info.getLoc(), name, portList,
                                              defName, annotations);

  if (!parameters.empty())
    fmodule->setAttr("parameters",
                     DictionaryAttr::get(getContext(), parameters));

  return success();
}

//===----------------------------------------------------------------------===//
// FIRCircuitParser
//===----------------------------------------------------------------------===//

namespace {
/// This class implements the outer level of the parser, including things
/// like circuit and module.
struct FIRCircuitParser : public FIRParser {
  explicit FIRCircuitParser(GlobalFIRParserState &state, ModuleOp mlirModule)
      : FIRParser(state), mlirModule(mlirModule) {}

  ParseResult parseCircuit();

private:
  ModuleOp mlirModule;
};

} // end anonymous namespace

/// file ::= circuit
/// circuit ::= 'circuit' id ':' info? INDENT module* DEDENT EOF
///
ParseResult FIRCircuitParser::parseCircuit() {
  auto indent = getIndentation();
  if (!indent.hasValue())
    return emitError("'circuit' must be first token on its line"), failure();
  unsigned circuitIndent = indent.getValue();

  LocWithInfo info(getToken().getLoc(), this);
  StringAttr name;
  SMLoc inlineAnnotationsLoc;
  StringRef inlineAnnotations;

  // A file must contain a top level `circuit` definition.
  if (parseToken(FIRToken::kw_circuit,
                 "expected a top-level 'circuit' definition") ||
      parseId(name, "expected circuit name") ||
      parseToken(FIRToken::colon, "expected ':' in circuit definition") ||
      parseOptionalAnnotations(inlineAnnotationsLoc, inlineAnnotations) ||
      info.parseOptionalInfo())
    return failure();

  auto circuitTarget = "~" + name.getValue().str();
  getState().circuitTarget = circuitTarget;

  // Deal with any inline annotations, if they exist.  These are processed first
  // to place any annotations from an annotation file *after* the inline
  // annotations.  While arbitrary, this makes the annotation file have "append"
  // semantics.
  if (!inlineAnnotations.empty())
    if (importAnnotations(inlineAnnotationsLoc, inlineAnnotations))
      return failure();

  // Deal with the annotation file if one was specified
  if (importAnnotationFile(info.getFIRLoc()))
    return failure();

  OpBuilder b(mlirModule.getBodyRegion());

  // Get annotations associated with this circuit. These are either:
  //   1. Annotations with no target (which we use "~" to identify)
  //   2. Annotations targeting the circuit, e.g., "~Foo"
  ArrayAttr annotations = getAnnotations("~");
  ArrayAttr circuitAnnot = getAnnotations(circuitTarget);
  if (!circuitAnnot.empty()) {
    // Merge these arrays if both present.
    if (annotations.empty())
      annotations = circuitAnnot;
    else {
      SmallVector<Attribute> elements;
      elements.append(annotations.begin(), annotations.end());
      elements.append(circuitAnnot.begin(), circuitAnnot.end());
      annotations = b.getArrayAttr(elements);
    }
  }

  // Create the top-level circuit op in the MLIR module.
  auto circuit = b.create<CircuitOp>(info.getLoc(), name, annotations);

  // Parse any contained modules.
  while (true) {
    switch (getToken().getKind()) {
    // If we got to the end of the file, then we're done.
    case FIRToken::eof:
      return success();

    // If we got an error token, then the lexer already emitted an error,
    // just stop.  We could introduce error recovery if there was demand for
    // it.
    case FIRToken::error:
      return failure();

    default:
      emitError("unexpected token in circuit");
      return failure();

    case FIRToken::kw_module:
    case FIRToken::kw_extmodule: {
      auto indent = getIndentation();
      if (!indent.hasValue())
        return emitError("'module' must be first token on its line"), failure();
      unsigned moduleIndent = indent.getValue();

      if (moduleIndent <= circuitIndent)
        return emitError("module should be indented more"), failure();

      if (FIRModuleParser(getState(), circuit).parseModule(moduleIndent))
        return failure();
      break;
    }
    }
  }
}

//===----------------------------------------------------------------------===//
// Driver
//===----------------------------------------------------------------------===//

// Parse the specified .fir file into the specified MLIR context.
OwningModuleRef circt::firrtl::importFIRRTL(SourceMgr &sourceMgr,
                                            MLIRContext *context,
                                            FIRParserOptions options) {
  auto sourceBuf = sourceMgr.getMemoryBuffer(sourceMgr.getMainFileID());
  const llvm::MemoryBuffer *annotationsBuf = nullptr;
  if (sourceMgr.getNumBuffers() > 1)
    annotationsBuf = sourceMgr.getMemoryBuffer(sourceMgr.getMainFileID() + 1);

  context->loadDialect<FIRRTLDialect>();

  // This is the result module we are parsing into.
  OwningModuleRef module(ModuleOp::create(
      FileLineColLoc::get(context, sourceBuf->getBufferIdentifier(), /*line=*/0,
                          /*column=*/0)));

  GlobalFIRParserState state(sourceMgr, context, options, annotationsBuf);
  if (FIRCircuitParser(state, *module).parseCircuit())
    return nullptr;

  // Make sure the parse module has no other structural problems detected by
  // the verifier.
  if (failed(verify(*module)))
    return {};

  return module;
}

void circt::firrtl::registerFromFIRRTLTranslation() {
  static mlir::TranslateToMLIRRegistration fromFIR(
      "import-firrtl", [](llvm::SourceMgr &sourceMgr, MLIRContext *context) {
        return importFIRRTL(sourceMgr, context);
      });
}
