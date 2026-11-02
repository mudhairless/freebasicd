/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "format/config.h"

#include "i18n.h"

#include <toml++/toml.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace fbformat {

namespace {

// --- factory defaults -------------------------------------------------------
// `FormatConfig` carries its own in-class defaults (config.h), so a
// default-constructed instance is the built-in default config and the defaults
// live in exactly one place. The named preset (FBdocs) is applied on top by
// `resolveConfig`.
FormatConfig factoryDefaults() { return FormatConfig{}; }

// --- value parsers ----------------------------------------------------------
bool parseCaseStyle(std::string_view s, CaseStyle *out) {
  if (s == "Leave") {
    *out = CaseStyle::Leave;
  } else if (s == "lower") {
    *out = CaseStyle::lower;
  } else if (s == "UPPER") {
    *out = CaseStyle::UPPER;
  } else if (s == "Title") {
    *out = CaseStyle::Title;
  } else if (s == "Canonical") {
    *out = CaseStyle::Canonical;
  } else {
    return false;
  }
  return true;
}

bool parseBool(toml::node const *n, bool *out) {
  if (n == nullptr) {
    return false;
  }
  if (auto const v = n->value<bool>()) {
    *out = *v;
    return true;
  }
  return false;
}

bool parseInt(toml::node const *n, int *out) {
  if (n == nullptr) {
    return false;
  }
  if (auto const v = n->value<int64_t>()) {
    *out = static_cast<int>(*v);
    return true;
  }
  return false;
}

bool parseString(toml::node const *n, std::string *out) {
  if (n == nullptr) {
    return false;
  }
  if (auto const v = n->value<std::string>()) {
    *out = *v;
    return true;
  }
  return false;
}

// Look up a (possibly dotted) key under `tbl`; the research schema writes
// `SpacesInParens.InConditionalStatements`, which TOML stores as a nested
// table, and `at_path` accepts the dotted spelling directly. A missing key
// yields nullptr, which every `parseX` helper treats as "leave the default".
toml::node const *lookup(toml::table const &tbl, std::string_view path) {
  return tbl.at_path(path).node();
}

} // namespace

bool parseFormatTable(std::string_view tomlText, FormatConfig *out,
                      std::string *styleName, std::string *inheritName) {
  toml::table root;
  try {
    root = toml::parse(tomlText);
  } catch (...) {
    return false; // malformed: caller keeps the preset/default values
  }

  if (toml::table const *const styleTbl = root["format-style"].as_table()) {
    if (styleName != nullptr) {
      parseString(lookup(*styleTbl, "style"), styleName);
    }
    if (inheritName != nullptr) {
      parseString(lookup(*styleTbl, "inherit"), inheritName);
    }
  }

  toml::table const *const f = root["format"].as_table();
  if (f == nullptr) {
    return true; // a file with no [format] table is valid; presence still
                 // counts
  }
  FormatConfig &c = *out;

  // A. file / dialect / line endings
  parseString(lookup(*f, "BasedOnStyle"), &c.basedOnStyle);
  parseString(lookup(*f, "Standard"), &c.standard);
  parseInt(lookup(*f, "ColumnLimit"), &c.columnLimit);
  parseString(lookup(*f, "LineEnding"), &c.lineEnding);
  parseBool(lookup(*f, "InsertNewlineAtEOF"), &c.insertNewlineAtEOF);
  parseBool(lookup(*f, "TrimTrailingWhitespace"), &c.trimTrailingWhitespace);
  parseBool(lookup(*f, "TrimFinalNewlines"), &c.trimFinalNewlines);
  parseBool(lookup(*f, "DisableFormat"), &c.disableFormat);

  // B. indentation / tabs
  parseInt(lookup(*f, "IndentWidth"), &c.indentWidth);
  parseInt(lookup(*f, "TabWidth"), &c.tabWidth);
  parseInt(lookup(*f, "ContinuationIndentWidth"), &c.continuationIndentWidth);
  if (std::string s; parseString(lookup(*f, "UseTab"), &s)) {
    if (s == "Never") {
      c.useTab = UseTab::Never;
    } else if (s == "ForIndentation") {
      c.useTab = UseTab::ForIndentation;
    } else if (s == "ForContinuationAndIndentation") {
      c.useTab = UseTab::ForContinuationAndIndentation;
    } else if (s == "Always") {
      c.useTab = UseTab::Always;
    }
  }
  parseBool(lookup(*f, "IndentCaseLabels"), &c.indentCaseLabels);
  if (std::string s; parseString(lookup(*f, "NamespaceIndentation"), &s)) {
    if (s == "None") {
      c.namespaceIndentation = NamespaceIndentation::None;
    } else if (s == "All") {
      c.namespaceIndentation = NamespaceIndentation::All;
    } else if (s == "Inner") {
      c.namespaceIndentation = NamespaceIndentation::Inner;
    }
  }
  parseBool(lookup(*f, "IndentAccessModifierSections"),
            &c.indentAccessModifierSections);
  if (std::string s; parseString(lookup(*f, "IndentPPDirectives"), &s)) {
    if (s == "None") {
      c.indentPPDirectives = IndentPPDirectives::None;
    } else if (s == "BeforeHash") {
      c.indentPPDirectives = IndentPPDirectives::BeforeHash;
    } else if (s == "AfterHash") {
      c.indentPPDirectives = IndentPPDirectives::AfterHash;
    }
  }
  if (std::string s; parseString(lookup(*f, "IndentGotoLabels"), &s)) {
    if (s == "None") {
      c.indentGotoLabels = IndentGotoLabels::None;
    } else if (s == "OuterIndent") {
      c.indentGotoLabels = IndentGotoLabels::OuterIndent;
    } else if (s == "InnerIndent") {
      c.indentGotoLabels = IndentGotoLabels::InnerIndent;
    }
  }
  if (std::string s; parseString(lookup(*f, "IndentExternBlock"), &s)) {
    if (s == "None") {
      c.indentExternBlock = IndentExternBlock::None;
    } else if (s == "Indent") {
      c.indentExternBlock = IndentExternBlock::Indent;
    } else if (s == "AfterExternBlock") {
      c.indentExternBlock = IndentExternBlock::AfterExternBlock;
    }
  }
  parseBool(lookup(*f, "IndentWrappedFunctionNames"),
            &c.indentWrappedFunctionNames);
  parseInt(lookup(*f, "BracedInitializerIndentWidth"),
           &c.bracedInitializerIndentWidth);

  // C. blank lines
  parseInt(lookup(*f, "MaxEmptyLinesToKeep"), &c.maxEmptyLinesToKeep);
  parseBool(lookup(*f, "KeepEmptyLines.AtEndOfFile"),
            &c.keepEmptyLinesAtEndOfFile);
  parseBool(lookup(*f, "KeepEmptyLines.AtStartOfBlock"),
            &c.keepEmptyLinesAtStartOfBlock);
  parseBool(lookup(*f, "KeepEmptyLines.AtStartOfFile"),
            &c.keepEmptyLinesAtStartOfFile);
  if (std::string s; parseString(lookup(*f, "SeparateDefinitionBlocks"), &s)) {
    if (s == "Leave") {
      c.separateDefinitionBlocks = SeparateDefinitionBlocks::Leave;
    } else if (s == "Always") {
      c.separateDefinitionBlocks = SeparateDefinitionBlocks::Always;
    } else if (s == "Never") {
      c.separateDefinitionBlocks = SeparateDefinitionBlocks::Never;
    }
  }
  if (std::string s;
      parseString(lookup(*f, "EmptyLineBeforeAccessModifier"), &s)) {
    if (s == "Never") {
      c.emptyLineBeforeAccessModifier = EmptyLineBeforeAccessModifier::Never;
    } else if (s == "Always") {
      c.emptyLineBeforeAccessModifier = EmptyLineBeforeAccessModifier::Always;
    } else if (s == "LogicalBlock") {
      c.emptyLineBeforeAccessModifier =
          EmptyLineBeforeAccessModifier::LogicalBlock;
    }
  }
  if (std::string s;
      parseString(lookup(*f, "EmptyLineAfterAccessModifier"), &s)) {
    if (s == "Never") {
      c.emptyLineAfterAccessModifier = EmptyLineAfterAccessModifier::Never;
    } else if (s == "Always") {
      c.emptyLineAfterAccessModifier = EmptyLineAfterAccessModifier::Always;
    } else if (s == "LogicalBlock") {
      c.emptyLineAfterAccessModifier =
          EmptyLineAfterAccessModifier::LogicalBlock;
    }
  }
  if (std::string s;
      parseString(lookup(*f, "WrapNamespaceBodyWithEmptyLines"), &s)) {
    if (s == "Leave") {
      c.wrapNamespaceBodyWithEmptyLines =
          WrapNamespaceBodyWithEmptyLines::Leave;
    } else if (s == "Never") {
      c.wrapNamespaceBodyWithEmptyLines =
          WrapNamespaceBodyWithEmptyLines::Never;
    } else if (s == "Always") {
      c.wrapNamespaceBodyWithEmptyLines =
          WrapNamespaceBodyWithEmptyLines::Always;
    }
  }

  // D. keyword / type / builtin case
  if (std::string s; parseString(lookup(*f, "KeywordCase"), &s)) {
    parseCaseStyle(s, &c.keywordCase);
  }
  if (std::string s; parseString(lookup(*f, "TypeNameCase"), &s)) {
    parseCaseStyle(s, &c.typeNameCase);
  }
  if (std::string s; parseString(lookup(*f, "IntrinsicCase"), &s)) {
    parseCaseStyle(s, &c.intrinsicCase);
  }
  if (std::string s; parseString(lookup(*f, "BooleanLiteralCase"), &s)) {
    parseCaseStyle(s, &c.booleanLiteralCase);
  }
  if (std::string s; parseString(lookup(*f, "IntrinsicDollarSuffix"), &s)) {
    if (s == "Leave") {
      c.intrinsicDollarSuffix = DollarSuffix::Leave;
    } else if (s == "Drop") {
      c.intrinsicDollarSuffix = DollarSuffix::Drop;
    } else if (s == "Add") {
      c.intrinsicDollarSuffix = DollarSuffix::Add;
    }
  }
  if (std::string s; parseString(lookup(*f, "IdentifierCase"), &s)) {
    parseCaseStyle(s, &c.identifierCase);
  }

  // E. literal spelling
  if (std::string s; parseString(lookup(*f, "NumericLiteralCase.Prefix"), &s)) {
    if (s == "Leave") {
      c.numericLiteralCasePrefix = NumericPrefixCase::Leave;
    } else if (s == "lower") {
      c.numericLiteralCasePrefix = NumericPrefixCase::lower;
    } else if (s == "UPPER") {
      c.numericLiteralCasePrefix = NumericPrefixCase::UPPER;
    }
  }
  if (std::string s;
      parseString(lookup(*f, "NumericLiteralCase.HexDigit"), &s)) {
    parseCaseStyle(s, &c.numericLiteralCaseHexDigit);
  }
  if (std::string s; parseString(lookup(*f, "NumericLiteralCase.Suffix"), &s)) {
    if (s == "Leave") {
      c.numericLiteralCaseSuffix = NumericSuffixCase::Leave;
    } else if (s == "lower") {
      c.numericLiteralCaseSuffix = NumericSuffixCase::lower;
    } else if (s == "UPPER") {
      c.numericLiteralCaseSuffix = NumericSuffixCase::UPPER;
    }
  }
  if (std::string s;
      parseString(lookup(*f, "NumericLiteralCase.ExponentLetter"), &s)) {
    parseCaseStyle(s, &c.numericLiteralCaseExponentLetter);
  }
  if (std::string s; parseString(lookup(*f, "FloatSuffixStyle"), &s)) {
    if (s == "Leave") {
      c.floatSuffixStyle = FloatSuffixStyle::Leave;
    } else if (s == "Sigil") {
      c.floatSuffixStyle = FloatSuffixStyle::Sigil;
    } else if (s == "Letter") {
      c.floatSuffixStyle = FloatSuffixStyle::Letter;
    }
  }
  parseBool(lookup(*f, "InsertLeadingZero"), &c.insertLeadingZero);

  // F. punctuation / operator spacing
  parseBool(lookup(*f, "SpaceBeforeAssignmentOperators"),
            &c.spaceBeforeAssignmentOperators);
  parseBool(lookup(*f, "SpaceAroundBinaryOperators"),
            &c.spaceAroundBinaryOperators);
  parseBool(lookup(*f, "SpaceAfterUnaryOperators"),
            &c.spaceAfterUnaryOperators);
  if (std::string s; parseString(lookup(*f, "SpaceAroundMemberAccess"), &s)) {
    if (s == "Leave") {
      c.spaceAroundMemberAccess = SpaceAroundMemberAccess::Leave;
    } else if (s == "None") {
      c.spaceAroundMemberAccess = SpaceAroundMemberAccess::None;
    } else if (s == "Both") {
      c.spaceAroundMemberAccess = SpaceAroundMemberAccess::Both;
    } else if (s == "After") {
      c.spaceAroundMemberAccess = SpaceAroundMemberAccess::After;
    }
  }
  parseBool(lookup(*f, "SpaceBeforeColon"), &c.spaceBeforeColon);
  parseBool(lookup(*f, "SpaceAfterStatementSeparator"),
            &c.spaceAfterStatementSeparator);
  parseBool(lookup(*f, "SpaceAfterPrintChannelHash"),
            &c.spaceAfterPrintChannelHash);
  parseInt(lookup(*f, "SpacesBeforeTrailingComments"),
           &c.spacesBeforeTrailingComments);

  // G. parens / commas / argument lists
  if (std::string s; parseString(lookup(*f, "SpaceBeforeParens"), &s)) {
    if (s == "Never") {
      c.spaceBeforeParens = SpaceBeforeParens::Never;
    } else if (s == "ControlStatements") {
      c.spaceBeforeParens = SpaceBeforeParens::ControlStatements;
    } else if (s == "FunctionDeclaration") {
      c.spaceBeforeParens = SpaceBeforeParens::FunctionDeclaration;
    } else if (s == "FunctionDefinition") {
      c.spaceBeforeParens = SpaceBeforeParens::FunctionDefinition;
    } else if (s == "FunctionCall") {
      c.spaceBeforeParens = SpaceBeforeParens::FunctionCall;
    } else if (s == "Always") {
      c.spaceBeforeParens = SpaceBeforeParens::Always;
    }
  }
  parseBool(lookup(*f, "SpaceBeforeParensOptions.AfterControlStatements"),
            &c.spaceBeforeParensAfterControlStatements);
  parseBool(lookup(*f, "SpaceBeforeParensOptions.AfterFunctionDeclarationName"),
            &c.spaceBeforeParensAfterFunctionDeclarationName);
  parseBool(lookup(*f, "SpaceBeforeParensOptions.AfterFunctionDefinitionName"),
            &c.spaceBeforeParensAfterFunctionDefinitionName);
  parseBool(lookup(*f, "SpaceBeforeParensOptions.AfterFunctionCallName"),
            &c.spaceBeforeParensAfterFunctionCallName);
  parseBool(lookup(*f, "SpacesInParens.InConditionalStatements"),
            &c.spacesInParensInConditionalStatements);
  parseBool(lookup(*f, "SpacesInParens.InEmptyParentheses"),
            &c.spacesInParensInEmptyParentheses);
  parseBool(lookup(*f, "SpacesInParens.InParameterLists"),
            &c.spacesInParensInParameterLists);
  parseBool(lookup(*f, "SpacesInParens.InCallParens"),
            &c.spacesInParensInCallParens);
  parseBool(lookup(*f, "SpacesInParens.InSubscripts"),
            &c.spacesInParensInSubscripts);
  parseBool(lookup(*f, "SpacesInParens.InCasts"), &c.spacesInParensInCasts);
  parseBool(lookup(*f, "SpaceAfterComma"), &c.spaceAfterComma);
  parseBool(lookup(*f, "SpaceBeforeComma"), &c.spaceBeforeComma);
  parseBool(lookup(*f, "SpacesInBracedInitializers"),
            &c.spacesInBracedInitializers);
  parseBool(lookup(*f, "SpaceBeforeBracedInitializer"),
            &c.spaceBeforeBracedInitializer);
  if (std::string s; parseString(lookup(*f, "RemoveRedundantParens"), &s)) {
    if (s == "Leave") {
      c.removeRedundantParens = RemoveRedundantParens::Leave;
    } else if (s == "MultipleParentheses") {
      c.removeRedundantParens = RemoveRedundantParens::MultipleParentheses;
    }
  }
  if (std::string s; parseString(lookup(*f, "CallParentheses"), &s)) {
    if (s == "Leave") {
      c.callParentheses = CallParentheses::Leave;
    } else if (s == "Add") {
      c.callParentheses = CallParentheses::Add;
    } else if (s == "Remove") {
      c.callParentheses = CallParentheses::Remove;
    }
  }
  if (toml::node const *const node = lookup(*f, "CallParenthesesExclude")) {
    if (toml::array const *const arr = node->as_array()) {
      c.callParenthesesExclude.clear();
      for (toml::node const &el : *arr) {
        if (auto const str = el.value<std::string>()) {
          c.callParenthesesExclude.push_back(*str);
        }
      }
    }
  }
  if (std::string s; parseString(lookup(*f, "NiladicCallParentheses"), &s)) {
    if (s == "Leave") {
      c.niladicCallParentheses = NiladicCallParentheses::Leave;
    } else if (s == "Add") {
      c.niladicCallParentheses = NiladicCallParentheses::Add;
    } else if (s == "Remove") {
      c.niladicCallParentheses = NiladicCallParentheses::Remove;
    }
  }

  // Graphics statements
  if (std::string s; parseString(lookup(*f, "CoordPairSeparatorSpacing"), &s)) {
    if (s == "Leave") {
      c.coordPairSeparatorSpacing = CoordPairSep::Leave;
    } else if (s == "None") {
      c.coordPairSeparatorSpacing = CoordPairSep::None;
    } else if (s == "Around") {
      c.coordPairSeparatorSpacing = CoordPairSep::Around;
    }
  }
  if (std::string s; parseString(lookup(*f, "StepPrefixSpacing"), &s)) {
    if (s == "Leave") {
      c.stepPrefixSpacing = StepPrefix::Leave;
    } else if (s == "Attached") {
      c.stepPrefixSpacing = StepPrefix::Attached;
    } else if (s == "Spaced") {
      c.stepPrefixSpacing = StepPrefix::Spaced;
    }
  }
  parseBool(lookup(*f, "SpaceBetweenEmptyArguments"),
            &c.spaceBetweenEmptyArguments);
  if (std::string s; parseString(lookup(*f, "GraphicsModeWordCase"), &s)) {
    if (s == "Leave") {
      c.graphicsModeWordCase = GraphicsModeWordCase::Leave;
    } else if (s == "lower") {
      c.graphicsModeWordCase = GraphicsModeWordCase::lower;
    } else if (s == "UPPER") {
      c.graphicsModeWordCase = GraphicsModeWordCase::UPPER;
    }
  }

  // H. declarations / procedure heads
  parseBool(lookup(*f, "CanonicalizeSpecifierOrder"),
            &c.canonicalizeSpecifierOrder);
  if (std::string s; parseString(lookup(*f, "PointerSpellingStyle"), &s)) {
    if (s == "Leave") {
      c.pointerSpellingStyle = PointerSpellingStyle::Leave;
    } else if (s == "Ptr") {
      c.pointerSpellingStyle = PointerSpellingStyle::Ptr;
    } else if (s == "Pointer") {
      c.pointerSpellingStyle = PointerSpellingStyle::Pointer;
    }
  }
  if (std::string s; parseString(lookup(*f, "ConstQualifierPosition"), &s)) {
    if (s == "Leave") {
      c.constQualifierPosition = ConstQualifierPosition::Leave;
    } else if (s == "Left") {
      c.constQualifierPosition = ConstQualifierPosition::Left;
    } else if (s == "Right") {
      c.constQualifierPosition = ConstQualifierPosition::Right;
    }
  }
  if (std::string s; parseString(lookup(*f, "DimTypePosition"), &s)) {
    if (s == "Leave") {
      c.dimTypePosition = DimTypePosition::Leave;
    } else if (s == "NameFirst") {
      c.dimTypePosition = DimTypePosition::NameFirst;
    } else if (s == "TypeFirst") {
      c.dimTypePosition = DimTypePosition::TypeFirst;
    }
  }
  if (std::string s; parseString(lookup(*f, "TypeFieldDim"), &s)) {
    if (s == "Leave") {
      c.typeFieldDim = TypeFieldDim::Leave;
    } else if (s == "RequireDim") {
      c.typeFieldDim = TypeFieldDim::RequireDim;
    } else if (s == "OmitDim") {
      c.typeFieldDim = TypeFieldDim::OmitDim;
    }
  }
  if (std::string s; parseString(lookup(*f, "NextVariableStyle"), &s)) {
    if (s == "Leave") {
      c.nextVariableStyle = NextVariableStyle::Leave;
    } else if (s == "Always") {
      c.nextVariableStyle = NextVariableStyle::Always;
    } else if (s == "Omit") {
      c.nextVariableStyle = NextVariableStyle::Omit;
    }
  }
  if (std::string s; parseString(lookup(*f, "CallKeywordStyle"), &s)) {
    if (s == "Leave") {
      c.callKeywordStyle = CallKeywordStyle::Leave;
    } else if (s == "Remove") {
      c.callKeywordStyle = CallKeywordStyle::Remove;
    }
  }
  if (std::string s; parseString(lookup(*f, "LetKeywordStyle"), &s)) {
    if (s == "Leave") {
      c.letKeywordStyle = LetKeywordStyle::Leave;
    } else if (s == "Remove") {
      c.letKeywordStyle = LetKeywordStyle::Remove;
    }
  }

  // I. records / enums / access sections
  if (std::string s; parseString(lookup(*f, "AccessSectionSpacing"), &s)) {
    if (s == "Leave") {
      c.accessSectionSpacing = AccessSectionSpacing::Leave;
    } else if (s == "ColonOnly") {
      c.accessSectionSpacing = AccessSectionSpacing::ColonOnly;
    } else if (s == "ColonAndBlank") {
      c.accessSectionSpacing = AccessSectionSpacing::ColonAndBlank;
    }
  }
  if (std::string s; parseString(lookup(*f, "FieldAlignmentSpacing"), &s)) {
    if (s == "Leave") {
      c.fieldAlignmentSpacing = FieldAlignmentSpacing::Leave;
    } else if (s == "Tight") {
      c.fieldAlignmentSpacing = FieldAlignmentSpacing::Tight;
    } else if (s == "Aligned") {
      c.fieldAlignmentSpacing = FieldAlignmentSpacing::Aligned;
    }
  }

  // J. statements / `:` separator
  if (std::string s; parseString(lookup(*f, "StatementSeparatorStyle"), &s)) {
    if (s == "Leave") {
      c.statementSeparatorStyle = StatementSeparatorStyle::Leave;
    } else if (s == "SplitSafe") {
      c.statementSeparatorStyle = StatementSeparatorStyle::SplitSafe;
    } else if (s == "SplitAll") {
      c.statementSeparatorStyle = StatementSeparatorStyle::SplitAll;
    } else if (s == "Join") {
      c.statementSeparatorStyle = StatementSeparatorStyle::Join;
    }
  }
  if (std::string s;
      parseString(lookup(*f, "AllowShortIfStatementsOnASingleLine"), &s)) {
    if (s == "Leave") {
      c.allowShortIfStatementsOnASingleLine = AllowShort::Leave;
    } else if (s == "Never") {
      c.allowShortIfStatementsOnASingleLine = AllowShort::Never;
    }
  }
  if (std::string s;
      parseString(lookup(*f, "AllowShortBlocksOnASingleLine"), &s)) {
    if (s == "Leave") {
      c.allowShortBlocksOnASingleLine = AllowShortBlock::Leave;
    } else if (s == "Never") {
      c.allowShortBlocksOnASingleLine = AllowShortBlock::Never;
    }
  }

  // K. line breaking / continuations
  if (std::string s; parseString(lookup(*f, "ContinuationStyle"), &s)) {
    if (s == "Leave") {
      c.continuationStyle = ContinuationStyle::Leave;
    } else if (s == "WrapOnly") {
      c.continuationStyle = ContinuationStyle::WrapOnly;
    } else if (s == "WrapAndJoin") {
      c.continuationStyle = ContinuationStyle::WrapAndJoin;
    }
  }
  if (std::string s; parseString(lookup(*f, "BreakBinaryOperators"), &s)) {
    if (s == "None") {
      c.breakBinaryOperators = BreakBinaryOperators::None;
    } else if (s == "NonAssignment") {
      c.breakBinaryOperators = BreakBinaryOperators::NonAssignment;
    } else if (s == "All") {
      c.breakBinaryOperators = BreakBinaryOperators::All;
    }
  }
  if (std::string s; parseString(lookup(*f, "AlignEscapedNewlines"), &s)) {
    if (s == "Left") {
      c.alignEscapedNewlines = AlignEscapedNewlines::Left;
    } else if (s == "Right") {
      c.alignEscapedNewlines = AlignEscapedNewlines::Right;
    } else if (s == "DontAlign") {
      c.alignEscapedNewlines = AlignEscapedNewlines::DontAlign;
    }
  }
  if (std::string s; parseString(lookup(*f, "AlignmentAfterOpenBracket"), &s)) {
    if (s == "Align") {
      c.alignmentAfterOpenBracket = AlignmentAfterOpenBracket::Align;
    } else if (s == "DontAlign") {
      c.alignmentAfterOpenBracket = AlignmentAfterOpenBracket::DontAlign;
    } else if (s == "AlwaysBreak") {
      c.alignmentAfterOpenBracket = AlignmentAfterOpenBracket::AlwaysBreak;
    } else if (s == "BlockIndent") {
      c.alignmentAfterOpenBracket = AlignmentAfterOpenBracket::BlockIndent;
    }
  }
  parseBool(lookup(*f, "BreakStringLiterals"), &c.breakStringLiterals);
  parseInt(lookup(*f, "PenaltyExcessCharacter"), &c.penaltyExcessCharacter);
  parseInt(lookup(*f, "PenaltyBreakBeforeFirstCallParameter"),
           &c.penaltyBreakBeforeFirstCallParameter);

  // L. alignment
  parseBool(lookup(*f, "AlignConsecutiveAssignments"),
            &c.alignConsecutiveAssignments);
  parseBool(lookup(*f, "AlignConsecutiveDeclarations"),
            &c.alignConsecutiveDeclarations);
  parseBool(lookup(*f, "AlignConsecutiveMacros"), &c.alignConsecutiveMacros);
  if (std::string s; parseString(lookup(*f, "AlignTrailingComments"), &s)) {
    if (s == "Leave") {
      c.alignTrailingComments = AlignTrailing::Leave;
    } else if (s == "Always") {
      c.alignTrailingComments = AlignTrailing::Always;
    } else if (s == "Never") {
      c.alignTrailingComments = AlignTrailing::Never;
    }
  }
  if (std::string s; parseString(lookup(*f, "AlignOperands"), &s)) {
    if (s == "DontAlign") {
      c.alignOperands = AlignOperandsStyle::DontAlign;
    } else if (s == "Align") {
      c.alignOperands = AlignOperandsStyle::Align;
    } else if (s == "AlignAfterOperator") {
      c.alignOperands = AlignOperandsStyle::AlignAfterOperator;
    }
  }
  parseBool(lookup(*f, "AlignConsecutiveCaseStatements"),
            &c.alignConsecutiveCaseStatements);
  if (std::string s; parseString(lookup(*f, "AlignArrayOfStructures"), &s)) {
    if (s == "None") {
      c.alignArrayOfStructures = AlignArrayOfStruct::None;
    } else if (s == "Left") {
      c.alignArrayOfStructures = AlignArrayOfStruct::Left;
    } else if (s == "Right") {
      c.alignArrayOfStructures = AlignArrayOfStruct::Right;
    }
  }

  // M. comments / doc comments
  if (std::string s; parseString(lookup(*f, "ReflowComments"), &s)) {
    if (s == "Never") {
      c.reflowComments = ReflowComments::Never;
    } else if (s == "NonDoc") {
      c.reflowComments = ReflowComments::NonDoc;
    } else if (s == "All") {
      c.reflowComments = ReflowComments::All;
    }
  }
  parseInt(lookup(*f, "SpacesInLineCommentPrefix.Minimum"),
           &c.spacesInLineCommentPrefixMinimum);
  parseInt(lookup(*f, "SpacesInLineCommentPrefix.Maximum"),
           &c.spacesInLineCommentPrefixMaximum);
  parseString(lookup(*f, "CommentPragmas"), &c.commentPragmas);
  if (std::string s; parseString(lookup(*f, "DocCommentPrefix"), &s)) {
    if (s == "Leave") {
      c.docCommentPrefix = DocCommentPrefix::Leave;
    } else if (s == "DoubleQuote") {
      c.docCommentPrefix = DocCommentPrefix::DoubleQuote;
    } else if (s == "Slash") {
      c.docCommentPrefix = DocCommentPrefix::Slash;
    }
  }

  // N. preprocessor / metacommands
  if (std::string s; parseString(lookup(*f, "DirectiveCase"), &s)) {
    parseCaseStyle(s, &c.directiveCase);
  }
  if (std::string s; parseString(lookup(*f, "MetacommandCase"), &s)) {
    parseCaseStyle(s, &c.metacommandCase);
  }
  parseBool(lookup(*f, "SortIncludes"), &c.sortIncludes);
  if (std::string s; parseString(lookup(*f, "IncludeBlocks"), &s)) {
    if (s == "Preserve") {
      c.includeBlocks = IncludeBlocks::Preserve;
    } else if (s == "Merge") {
      c.includeBlocks = IncludeBlocks::Merge;
    } else if (s == "Regroup") {
      c.includeBlocks = IncludeBlocks::Regroup;
    }
  }
  if (std::string s;
      parseString(lookup(*f, "DirectiveContinuationAlign"), &s)) {
    if (s == "Leave") {
      c.directiveContinuationAlign = DirectiveContAlign::Leave;
    } else if (s == "ContinuationIndent") {
      c.directiveContinuationAlign = DirectiveContAlign::ContinuationIndent;
    } else if (s == "AlignToValue") {
      c.directiveContinuationAlign = DirectiveContAlign::AlignToValue;
    }
  }

  // O. format-off control
  parseString(lookup(*f, "FormatOffPragma"), &c.formatOffPragma);
  parseString(lookup(*f, "FormatOnPragma"), &c.formatOnPragma);
  parseString(lookup(*f, "OneLineFormatOffRegex"), &c.oneLineFormatOffRegex);

  return true;
}

// --- preset resolution ------------------------------------------------------
namespace {

// Does the file exist as a regular file?
bool isFile(fs::path const &p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

// Read a whole file, binary-exact. Returns false when it cannot be opened.
bool readFile(fs::path const &p, std::string *out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(in),
              std::istreambuf_iterator<char>());
  return true;
}

// The directories a preset name is resolved against, highest priority first:
// the config file's own `.styles/`, then the user's config dir, then the
// install's `share/fbformat/styles/` and (for a dev build) the source tree's
// `styles/`. The first `<name>.toml` found wins, which lets a project preset
// shadow a user or global one of the same names.
std::vector<fs::path> styleSearchDirs(fs::path const &configDir) {
  std::vector<fs::path> dirs;
  if (!configDir.empty()) {
    dirs.push_back(configDir / kLocalStylesDir);
  }
  if (char const *const home = std::getenv("HOME")) {
    dirs.push_back(fs::path(home) / ".config" / "fbformat" / "styles");
  }
  if (char const *const profile = std::getenv("USERPROFILE")) {
    dirs.push_back(fs::path(profile) / "fbformat" / "styles");
  }
#ifdef FBLANG_FORMAT_STYLEDIR_INSTALL
  dirs.emplace_back(FBLANG_FORMAT_STYLEDIR_INSTALL);
#endif
#ifdef FBLANG_FORMAT_STYLEDIR_BUILD
  dirs.emplace_back(FBLANG_FORMAT_STYLEDIR_BUILD);
#endif
  return dirs;
}

enum class PresetStatus { Loaded, NotFound, Broken };

// Resolve one preset by name into `cfg`, following its `inherit` chain.
// `seen` breaks cycles: a preset that (directly or transitively) names itself
// is a broken config rather than a silent stop, because half of the chain
// applied would be neither the author's style nor a default.
PresetStatus loadPresetInto(std::string const &name, fs::path const &configDir,
                            FormatConfig *cfg,
                            std::unordered_set<std::string> *seen,
                            std::string *error) {
  if (name.empty() || !seen->insert(name).second) {
    *error = fblang::trf("style inheritance cycle at %s", name);
    return PresetStatus::Broken;
  }
  fs::path found;
  for (fs::path const &dir : styleSearchDirs(configDir)) {
    fs::path const candidate = dir / (name + ".toml");
    if (isFile(candidate)) {
      found = candidate;
      break;
    }
  }
  if (found.empty()) {
    return PresetStatus::NotFound;
  }
  std::string text;
  if (!readFile(found, &text)) {
    *error = fblang::trf("cannot read %s", found.generic_string());
    return PresetStatus::Broken;
  }
  FormatConfig scratch = *cfg;
  std::string ignoreStyle;
  std::string inherit;
  if (!parseFormatTable(text, &scratch, &ignoreStyle, &inherit)) {
    *error = fblang::trf("%s is not valid TOML", found.generic_string());
    return PresetStatus::Broken;
  }
  if (!inherit.empty()) {
    // A parent is applied first, then the child's own keys — so the child
    // wins. The search dir stays the config file's own dir: "local" is
    // `<config-file-dir>/.styles/`, not relative to each preset in the chain.
    PresetStatus const parent =
        loadPresetInto(inherit, configDir, cfg, seen, error);
    if (parent != PresetStatus::Loaded) {
      if (parent == PresetStatus::NotFound) {
        *error = fblang::trf("unknown style: %s", inherit);
      }
      return parent;
    }
  }
  if (!parseFormatTable(text, cfg, nullptr, nullptr)) {
    *error = fblang::trf("%s is not valid TOML", found.generic_string());
    return PresetStatus::Broken;
  }
  cfg->basedOnStyle = name;
  return PresetStatus::Loaded;
}

} // namespace

// Walk up from `startDir` to the filesystem root, returning the first
// `dir/<name>` that exists as a regular file, or an empty path.
fs::path findUpwards(fs::path const &startDir, std::string_view name) {
  std::error_code ec;
  fs::path dir = fs::absolute(startDir, ec);
  if (ec) {
    dir = startDir;
  }
  while (!dir.empty()) {
    fs::path const candidate = dir / name;
    if (isFile(candidate)) {
      return candidate;
    }
    fs::path const next = dir.parent_path();
    if (next == dir) {
      break;
    }
    dir = next;
  }
  return {};
}

namespace {

// Find the config file that governs `filePath`: `.fbformat` walking up wins
// outright over `freebasicd.toml`, which is also walked up (the server's config
// file carries [format] tables too).
fs::path discoverConfigFile(std::string const &filePath) {
  fs::path probe = fs::path(filePath).parent_path();
  if (probe.empty()) {
    probe = fs::current_path();
  }
  fs::path chosen = findUpwards(probe, kFormatFileName);
  if (chosen.empty()) {
    chosen = findUpwards(probe, "freebasicd.toml");
  }
  return chosen;
}

// The preset a file names, plus the fact that it named one at all. `inherit`
// without `style` is the plan's single-parent spelling and resolves the same
// way.
std::string namedStyle(std::string const &fileStyle,
                       std::string const &fileInherit) {
  return !fileStyle.empty() ? fileStyle : fileInherit;
}

} // namespace

ResolvedConfig resolveConfig(std::string const &filePath,
                             ConfigQuery const &q) {
  ResolvedConfig r;

  fs::path chosen;
  if (!q.configFile.empty()) {
    chosen = fs::path(q.configFile);
    if (!isFile(chosen)) {
      r.ok = false;
      r.error = fblang::trf("cannot read %s", q.configFile);
      return r;
    }
  } else if (!filePath.empty()) {
    chosen = discoverConfigFile(filePath);
  }

  std::string text;
  std::string fileStyle;
  std::string fileInherit;
  if (!chosen.empty()) {
    r.foundConfigFile = true;
    r.configFile = chosen.generic_string();
    if (!readFile(chosen, &text)) {
      r.ok = false;
      r.error = fblang::trf("cannot read %s", r.configFile);
      return r;
    }
    FormatConfig scratch;
    if (!parseFormatTable(text, &scratch, &fileStyle, &fileInherit)) {
      r.ok = false;
      r.error = fblang::trf("%s is not valid TOML", r.configFile);
      return r;
    }
    // `.fbformat` shadows the server's config file in the same directory. The
    // notice is a warning the caller logs; a library never writes stderr.
    if (chosen.filename() == kFormatFileName &&
        isFile(chosen.parent_path() / "freebasicd.toml")) {
      r.warnings.push_back(fblang::trf(
          "%s: .fbformat overrides freebasicd.toml; its formatting settings "
          "are ignored",
          r.configFile));
    }
  }

  fs::path const configDir = chosen.empty() ? fs::path() : chosen.parent_path();
  std::string const style =
      !q.style.empty() ? q.style : namedStyle(fileStyle, fileInherit);

  FormatConfig cfg;
  std::unordered_set<std::string> seen;
  std::string error;
  if (!style.empty()) {
    PresetStatus const ps =
        loadPresetInto(style, configDir, &cfg, &seen, &error);
    if (ps != PresetStatus::Loaded) {
      r.ok = false;
      r.error = ps == PresetStatus::NotFound
                    ? fblang::trf("unknown style: %s", style)
                    : error;
      return r;
    }
  } else if (loadPresetInto("FBdocs", configDir, &cfg, &seen, &error) !=
             PresetStatus::Loaded) {
    // The implicit default has a built-in fallback (PLAN M17 §6): a missing
    // install preset degrades to the factory values, which change no token
    // spelling. An explicitly named style never gets this treatment.
    cfg = factoryDefaults();
  }

  // The config file's own keys override the preset it named.
  if (!chosen.empty() && !parseFormatTable(text, &cfg, nullptr, nullptr)) {
    r.ok = false;
    r.error = fblang::trf("%s is not valid TOML", r.configFile);
    return r;
  }
  r.config = cfg;
  return r;
}

ResolvedConfig configForStyle(std::string const &name,
                              std::string const &configDir) {
  ResolvedConfig r;
  std::string error;
  std::unordered_set<std::string> seen;
  if (name.empty()) {
    r.ok = false;
    r.error = fblang::trf("unknown style: %s", name);
    return r;
  }
  PresetStatus const ps =
      loadPresetInto(name, fs::path(configDir), &r.config, &seen, &error);
  if (ps != PresetStatus::Loaded) {
    r.ok = false;
    r.error = ps == PresetStatus::NotFound
                  ? fblang::trf("unknown style: %s", name)
                  : error;
  }
  return r;
}

FormatConfig applyClientOptions(FormatConfig const &base, int tabSize,
                                bool insertSpaces, bool trimTrailingWhitespace,
                                bool insertFinalNewline,
                                bool trimFinalNewlines) {
  FormatConfig cfg = base;
  if (tabSize > 0) {
    cfg.tabWidth = tabSize;
    cfg.indentWidth = tabSize;
  }
  cfg.useTab = insertSpaces ? UseTab::Never : UseTab::ForIndentation;
  cfg.trimTrailingWhitespace = trimTrailingWhitespace;
  cfg.insertNewlineAtEOF = insertFinalNewline;
  cfg.trimFinalNewlines = trimFinalNewlines;
  return cfg;
}

} // namespace fbformat
