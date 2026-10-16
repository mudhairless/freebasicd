#pragma once

#include <string>
#include <vector>

namespace fbformat {

// Format option values
enum class UseTab {
  Never,
  ForIndentation,
  ForContinuationAndIndentation,
  Always
};

enum class NamespaceIndentation { None, All, Inner };

enum class IndentPPDirectives { None, BeforeHash, AfterHash };

enum class IndentGotoLabels { None, OuterIndent, InnerIndent };

enum class IndentExternBlock { None, Indent, AfterExternBlock };

enum class SpaceBeforeParens {
  Never,
  ControlStatements,
  FunctionDeclaration,
  FunctionDefinition,
  FunctionCall,
  Always
};

enum class SpaceAroundMemberAccess { Leave, None, Both, After };

enum class PointerSpellingStyle { Leave, Ptr, Pointer };

enum class ConstQualifierPosition { Leave, Left, Right };

enum class DimTypePosition { Leave, NameFirst, TypeFirst };

enum class TypeFieldDim { Leave, RequireDim, OmitDim };

enum class NextVariableStyle { Leave, Always, Omit };

enum class CallKeywordStyle { Leave, Remove };

enum class LetKeywordStyle { Leave, Remove };

enum class BreakBinaryOperators { None, NonAssignment, All };

enum class AlignEscapedNewlines { Left, Right, DontAlign };

enum class AlignmentAfterOpenBracket {
  Align,
  DontAlign,
  AlwaysBreak,
  BlockIndent
};

enum class CaseStyle { Leave, lower, UPPER, Title, Canonical };

enum class NumericPrefixCase { Leave, lower, UPPER };
enum class NumericSuffixCase { Leave, lower, UPPER };
enum class FloatSuffixStyle { Leave, Sigil, Letter };
enum class StringSigilStyle { Preserve };
enum class GraphicsModeWordCase { Leave, lower, UPPER };

enum class StatementSeparatorStyle { Leave, SplitSafe, SplitAll, Join };

enum class AccessSectionSpacing { Leave, ColonOnly, ColonAndBlank };

enum class FieldAlignmentSpacing { Leave, Tight, Aligned };

enum class EnumTrailingComma { Leave };

enum class SeparateDefinitionBlocks { Leave, Always, Never };

enum class EmptyLineBeforeAccessModifier { Never, Always, LogicalBlock };

enum class EmptyLineAfterAccessModifier { Never, Always, LogicalBlock };

enum class WrapNamespaceBodyWithEmptyLines { Leave, Never, Always };

enum class ReflowComments { Never, NonDoc, All };

enum class RemoveRedundantParens { Leave, MultipleParentheses };

enum class CallParentheses { Leave, Add, Remove };

enum class NiladicCallParentheses { Leave, Add, Remove };

enum class ContinuationStyle { Leave, WrapOnly, WrapAndJoin };

enum class AllowShort { Leave, Never };
enum class AllowShortBlock { Leave, Never };
enum class AlignTrailing { Leave, Always, Never };
enum class AlignOperandsStyle { DontAlign, Align, AlignAfterOperator };
enum class AlignArrayOfStruct { None, Left, Right };
enum class DocCommentPrefix { Leave, DoubleQuote, Slash };
enum class IncludeBlocks { Preserve, Merge, Regroup };
enum class DirectiveContAlign { Leave, ContinuationIndent, AlignToValue };
enum class DollarSuffix { Leave, Drop, Add };
enum class CoordPairSep { Leave, None, Around };
enum class StepPrefix { Leave, Attached, Spaced };

// Every member carries its factory default, so a default-constructed
// `FormatConfig` *is* the built-in default config (PLAN M17). The defaults
// favor not rewriting token spelling — a first run fixes indentation, spacing,
// and line endings and changes no token spelling. The shipped `FBdocs` preset
// is what a document actually gets when it names no style.
struct FormatConfig {
  // A. File, dialect, and line endings
  std::string basedOnStyle;
  std::string standard = "auto";       // auto, fb, fblite, qb, deprecated
  int columnLimit = 0;                 // off: every break needs a `_`
  std::string lineEnding = "DeriveLF"; // LF, CRLF, DeriveLF, DeriveCRLF, Leave
  bool insertNewlineAtEOF = true;
  bool trimTrailingWhitespace = true;
  bool trimFinalNewlines = true;
  bool disableFormat = false;

  // B. Indentation and tabs
  int indentWidth = 4;
  int tabWidth = 4;
  UseTab useTab = UseTab::ForIndentation;
  int continuationIndentWidth = 4;
  bool indentCaseLabels = false;
  NamespaceIndentation namespaceIndentation = NamespaceIndentation::All;
  bool indentAccessModifierSections = false;
  IndentPPDirectives indentPPDirectives = IndentPPDirectives::None;
  IndentGotoLabels indentGotoLabels = IndentGotoLabels::None;
  IndentExternBlock indentExternBlock = IndentExternBlock::Indent;
  bool indentWrappedFunctionNames = false;
  int bracedInitializerIndentWidth = 4;

  // C. Blank lines
  int maxEmptyLinesToKeep = 2;
  bool keepEmptyLinesAtEndOfFile = true;
  bool keepEmptyLinesAtStartOfBlock = true;
  bool keepEmptyLinesAtStartOfFile = true;
  SeparateDefinitionBlocks separateDefinitionBlocks =
      SeparateDefinitionBlocks::Leave;
  EmptyLineBeforeAccessModifier emptyLineBeforeAccessModifier =
      EmptyLineBeforeAccessModifier::Never;
  EmptyLineAfterAccessModifier emptyLineAfterAccessModifier =
      EmptyLineAfterAccessModifier::Never;
  WrapNamespaceBodyWithEmptyLines wrapNamespaceBodyWithEmptyLines =
      WrapNamespaceBodyWithEmptyLines::Leave;

  // D. Keyword/case
  CaseStyle keywordCase = CaseStyle::Leave;
  CaseStyle typeNameCase = CaseStyle::Leave;
  CaseStyle intrinsicCase = CaseStyle::Leave;
  CaseStyle booleanLiteralCase = CaseStyle::Leave;
  DollarSuffix intrinsicDollarSuffix = DollarSuffix::Leave;
  CaseStyle identifierCase = CaseStyle::Leave; // Leave only

  // E. Literals
  NumericPrefixCase numericLiteralCasePrefix = NumericPrefixCase::Leave;
  CaseStyle numericLiteralCaseHexDigit = CaseStyle::Leave;
  NumericSuffixCase numericLiteralCaseSuffix = NumericSuffixCase::Leave;
  CaseStyle numericLiteralCaseExponentLetter = CaseStyle::Leave;
  FloatSuffixStyle floatSuffixStyle = FloatSuffixStyle::Leave;
  bool insertLeadingZero = false;
  StringSigilStyle stringSigilStyle = StringSigilStyle::Preserve;

  // F/G. Spacing/operators/parens
  bool spaceBeforeAssignmentOperators = true;
  bool spaceAroundBinaryOperators = true;
  bool spaceAfterUnaryOperators = false;
  SpaceAroundMemberAccess spaceAroundMemberAccess =
      SpaceAroundMemberAccess::None;
  bool spaceBeforeColon = false;
  bool spaceAfterStatementSeparator = true;
  bool spaceAfterPrintChannelHash = true;
  int spacesBeforeTrailingComments = 1;

  // Parens
  SpaceBeforeParens spaceBeforeParens = SpaceBeforeParens::Never;
  bool spaceBeforeParensAfterControlStatements = false;
  bool spaceBeforeParensAfterFunctionDeclarationName = false;
  bool spaceBeforeParensAfterFunctionDefinitionName = false;
  bool spaceBeforeParensAfterFunctionCallName = false;
  bool spacesInParensInConditionalStatements = false;
  bool spacesInParensInEmptyParentheses = false;
  bool spacesInParensInParameterLists = false;
  bool spacesInParensInCallParens = false;
  bool spacesInParensInSubscripts = false;
  bool spacesInParensInCasts = false;
  bool spaceAfterComma = true;
  bool spaceBeforeComma = false;
  bool spacesInBracedInitializers = true;
  bool spaceBeforeBracedInitializer = false;
  RemoveRedundantParens removeRedundantParens = RemoveRedundantParens::Leave;
  CallParentheses callParentheses = CallParentheses::Leave;
  // Graphics statements whose argument grammar has no parenthesized form, so
  // `CallParentheses = "Add"` must skip them (research §G).
  std::vector<std::string> callParenthesesExclude = {
      "Circle", "Draw", "Get", "Line", "Paint", "Preset",
      "PReset", "Pset", "Put", "View", "Window"};
  NiladicCallParentheses niladicCallParentheses = NiladicCallParentheses::Leave;

  // Graphics
  CoordPairSep coordPairSeparatorSpacing = CoordPairSep::Leave;
  StepPrefix stepPrefixSpacing = StepPrefix::Leave;
  bool spaceBetweenEmptyArguments = false;
  GraphicsModeWordCase graphicsModeWordCase = GraphicsModeWordCase::Leave;

  // H. Declarations
  bool canonicalizeSpecifierOrder = false;
  PointerSpellingStyle pointerSpellingStyle = PointerSpellingStyle::Leave;
  ConstQualifierPosition constQualifierPosition = ConstQualifierPosition::Leave;
  DimTypePosition dimTypePosition = DimTypePosition::Leave;
  TypeFieldDim typeFieldDim = TypeFieldDim::Leave;
  NextVariableStyle nextVariableStyle = NextVariableStyle::Leave;
  CallKeywordStyle callKeywordStyle = CallKeywordStyle::Leave;
  LetKeywordStyle letKeywordStyle = LetKeywordStyle::Leave;

  // I. Records/enums
  AccessSectionSpacing accessSectionSpacing = AccessSectionSpacing::Leave;
  FieldAlignmentSpacing fieldAlignmentSpacing = FieldAlignmentSpacing::Leave;
  EnumTrailingComma enumTrailingComma = EnumTrailingComma::Leave;

  // J. Statements
  StatementSeparatorStyle statementSeparatorStyle =
      StatementSeparatorStyle::Leave;
  AllowShort allowShortIfStatementsOnASingleLine = AllowShort::Leave;
  AllowShortBlock allowShortBlocksOnASingleLine = AllowShortBlock::Leave;
  bool ifThenSpacing = true;

  // K. Line breaking
  ContinuationStyle continuationStyle = ContinuationStyle::Leave;
  BreakBinaryOperators breakBinaryOperators = BreakBinaryOperators::None;
  AlignEscapedNewlines alignEscapedNewlines = AlignEscapedNewlines::Right;
  AlignmentAfterOpenBracket alignmentAfterOpenBracket =
      AlignmentAfterOpenBracket::Align;
  bool breakStringLiterals = false;
  int penaltyExcessCharacter = 100;
  int penaltyBreakBeforeFirstCallParameter = 19;

  // L. Alignment
  bool alignConsecutiveAssignments = false;
  bool alignConsecutiveDeclarations = false;
  bool alignConsecutiveMacros = false;
  AlignTrailing alignTrailingComments = AlignTrailing::Leave;
  AlignOperandsStyle alignOperands = AlignOperandsStyle::DontAlign;
  bool alignConsecutiveCaseStatements = false;
  AlignArrayOfStruct alignArrayOfStructures = AlignArrayOfStruct::None;

  // M. Comments
  ReflowComments reflowComments = ReflowComments::Never;
  int spacesInLineCommentPrefixMinimum = 1;
  int spacesInLineCommentPrefixMaximum = -1;
  std::string commentPragmas = "^[[:space:]]*\\$"; // regex
  DocCommentPrefix docCommentPrefix = DocCommentPrefix::Leave;

  // N. Preprocessor/metacommands
  CaseStyle directiveCase = CaseStyle::Leave;
  CaseStyle metacommandCase = CaseStyle::Leave;
  bool macroBodyFormattingPreserve = true; // the only value (invariant)
  bool sortIncludes = false;
  IncludeBlocks includeBlocks = IncludeBlocks::Preserve;
  bool alignConsecutiveMacrosPP = false;
  DirectiveContAlign directiveContinuationAlign = DirectiveContAlign::Leave;

  // O. Format control
  std::string formatOffPragma = "fbformat: off";
  std::string formatOnPragma = "fbformat: on";
  std::string oneLineFormatOffRegex;

  // Style inheritance
  std::string inherit;
};

// The name of the standalone formatter's config file, discovered by walking up
// the directory tree from the document (like `.clang-format`).
inline constexpr char const *kFormatFileName = ".fbformat";

// The directory (beside a config file) whose `<name>.toml` files shadow other
// presets of the same name.
inline constexpr char const *kLocalStylesDir = ".styles";

// Parse a TOML document's `[format]` table into `out`, overlaying only the keys
// the document actually sets (the caller seeds `out` with the preset/default
// values). Unknown keys and wrong-typed values are ignored, so a malformed
// value never degrades a session below its default. `styleName` and
// `inheritName` receive the `[format-style]` values when present. Returns
// false only when the document is not valid TOML.
bool parseFormatTable(std::string_view tomlText, FormatConfig *out,
                      std::string *styleName, std::string *inheritName);

// Resolve the effective configuration for a document path:
//   1. the `[format]` keys of `.fbformat` (walking up), else of
//      `freebasicd.toml` at the discovered root, else nothing;
//   2. the `[format-style].style` preset, its `inherit` chain, and `FBdocs`
//      when neither names one;
//   3. the built-in factory defaults for anything still unset.
// Client `FormattingOptions` are deliberately not part of this: they apply only
// when *no* config file exists, through `applyClientOptions`.
FormatConfig loadConfigForPath(std::string const &filePath);

// Apply client `FormattingOptions` to a config that came from no config file.
// `insertSpaces == false` means tabs (UseTab::ForIndentation), `true` means
// spaces (UseTab::Never); `tabSize` feeds both TabWidth and IndentWidth.
FormatConfig applyClientOptions(FormatConfig const &base, int tabSize,
                                bool insertSpaces, bool trimTrailingWhitespace,
                                bool insertFinalNewline,
                                bool trimFinalNewlines);

} // namespace fbformat