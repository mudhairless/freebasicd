/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "preproc.h"

#include "language.h"
#include "symbols.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fblang {
namespace {

using PreprocMap = std::unordered_map<std::string, PreprocEntry>;

bool isIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isIdentChar(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }

// Keyword-form binary/unary operators of the `#if` expression grammar. These
// are never macro names to the evaluator: `#if TRUE and FALSE` means the
// operator even if a definition of `and` exists (fbc requires the same).
bool isWordOperator(std::string const &w) {
  return w == "and" || w == "or" || w == "xor" || w == "eqv" || w == "imp" ||
         w == "not" || w == "mod" || w == "shl" || w == "shr";
}

// The tail of a directive line after its word (`#`, whitespace, word).
std::string_view restAfterWord(std::string_view line) {
  std::string_view const w = preprocessorWord(line);
  std::size_t i = 1;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
    ++i;
  }
  i += w.size();
  return line.substr(i);
}

// The first identifier of a directive's tail (`#ifdef NAME ...`), suffix char
// included, or empty. `#undef` uses the same read.
std::string_view firstIdentifier(std::string_view line) {
  std::string_view const rest = restAfterWord(line);
  std::size_t i = 0;
  while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) {
    ++i;
  }
  std::size_t const b = i;
  while (i < rest.size() && isIdentChar(rest[i])) {
    ++i;
  }
  if (i < rest.size() && isSuffixChar(rest[i])) {
    ++i;
  }
  return rest.substr(b, i - b);
}

// fbc's line-continuation rule applied to a single textual tail
// (FreeBASIC.md §6 — a trailing `_` "must not follow an identifier/word
// without a space"). Scan with the lexer's word model: a letter or `_` begins
// an identifier word and absorbs later digits/underscores (`foo2_`,
// `__FB_DEBUG__` — no continuation); a digit begins a number literal, which a
// `_` never extends (`5_` continues, probed; `5_10` = the number `5` plus the
// identifier `_10`, which fbc reports as "expected identifier, found '_10'"
// when it appears where a name is wanted). Returns whether the text's last
// code character is a fresh (non-absorbed) `_` — the continuation marker.
bool trailingUnderscoreIsContinuation(std::string_view text) {
  bool inWord = false;
  bool wordIsNumber = false; // the current word began with a digit
  bool freshUnderscore = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    char const c = text[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      inWord = true;
      wordIsNumber = false;
      freshUnderscore = false;
    } else if (c == '_') {
      freshUnderscore = !inWord || wordIsNumber;
      inWord = true;
      wordIsNumber = false;
    } else if (c >= '0' && c <= '9') {
      if (!inWord) {
        inWord = true;
        wordIsNumber = true;
      }
      freshUnderscore = false;
    } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      inWord = false;
      wordIsNumber = false;
    } else {
      inWord = false;
      wordIsNumber = false;
      freshUnderscore = false;
    }
  }
  return freshUnderscore;
}

// The directive's first physical line, minus a trailing comment, whitespace,
// and a trailing `_` continuation marker. This is what a define/macro hover
// shows: `#define MAX_ITEMS 10`, `#macro say(w)`, or `#define printval(b) _`
// → `#define printval(b)`.
std::string firstLineSignature(std::string_view line) {
  std::size_t e = line.find_first_of("\r\n");
  if (e == std::string_view::npos) {
    e = line.size();
  }
  std::string s(line.substr(0, e));
  // Strip a trailing `'` comment (respecting a string literal that contains
  // one). The scan keeps the first string double-quote state, so the apostrophe
  // in `"it's"` survives.
  bool inString = false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (inString) {
      if (s[i] == '"') {
        if (i + 1 < s.size() && s[i + 1] == '"') {
          ++i;
        } else {
          inString = false;
        }
      }
      continue;
    }
    if (s[i] == '"') {
      inString = true;
    } else if (s[i] == '\'') {
      s.resize(i);
      break;
    }
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.pop_back();
  }
  // Strip a trailing continuation `_` — but only when it is one. A `_` that
  // is the tail of an identifier is part of the name (`#define MAX_` keeps
  // its underscore); a fresh `_` after a number or an operator is the marker
  // (`#define X 5_` shows `#define X 5`). FreeBASIC.md §6.
  if (trailingUnderscoreIsContinuation(s)) {
    s.pop_back();
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
      s.pop_back();
    }
  }
  return s;
}

// Parse `#define ...` / `#macro ...` into a PreprocDefine (offsets relative to
// the line). False when the tail carries no identifier.
bool parseDefineLine(std::string_view line, bool isMacro, PreprocDefine *out) {
  std::string_view const rest = restAfterWord(line);
  // `rest` starts partway into `line` (`#`, whitespace, word); the name
  // offsets must come back relative to `line`, which is what the caller adds
  // the directive's absolute offset to.
  std::size_t const restOff = line.size() - rest.size();
  std::size_t i = 0;
  while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) {
    ++i;
  }
  std::size_t const nb = i;
  if (i >= rest.size() || !isIdentStart(rest[i])) {
    return false;
  }
  ++i;
  while (i < rest.size() && isIdentChar(rest[i])) {
    ++i;
  }
  if (i < rest.size() && isSuffixChar(rest[i])) {
    ++i; // the suffix is part of the identifier's canonical key (FOO$)
  }
  std::size_t const ne = i;
  out->name = std::string(rest.substr(nb, ne - nb));
  out->key = toLowerChars(out->name);
  out->isMacro = isMacro;
  std::size_t j = i;
  while (j < rest.size() && (rest[j] == ' ' || rest[j] == '\t')) {
    ++j;
  }
  out->functionLike = j < rest.size() && rest[j] == '(';
  out->nameBeg = static_cast<std::uint32_t>(restOff + nb);
  out->nameEnd = static_cast<std::uint32_t>(restOff + i);
  out->signature = firstLineSignature(line);
  return true;
}

// The replacement text of a `#define NAME <value>` line: the raw tail after
// the name with `_` line continuations joined, `'` comments dropped, and the
// result trimmed. A function-like define is registered separately and never
// expanded, so this path only sees object-like bodies.
std::string normalizedValue(std::string_view line, std::size_t from) {
  std::string out;
  std::size_t i = from;
  while (i < line.size()) {
    char const c = line[i];
    if (c == '\'') {
      break; // trailing comment ends the value
    }
    if (c == '_') {
      // Only a fresh `_` token continues the line; an identifier tail
      // (`#define X abc_`) is ordinary replacement text (FreeBASIC.md §6).
      bool const continuation = trailingUnderscoreIsContinuation(
          std::string_view(line.data(), i + 1));
      if (continuation) {
        std::size_t j = i + 1;
        while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) {
          ++j;
        }
        if (j < line.size() && line[j] == '\'') {
          while (j < line.size() && line[j] != '\n' && line[j] != '\r') {
            ++j;
          }
        }
        if (j >= line.size() || line[j] == '\n' || line[j] == '\r') {
          // `_` (and anything between it and the newline) is a continuation.
          i = j;
          while (i < line.size() && (line[i] == '\n' || line[i] == '\r' ||
                                     line[i] == ' ' || line[i] == '\t')) {
            ++i;
          }
          out.push_back(' ');
          continue;
        }
      }
      out.push_back(c);
      ++i;
      continue;
    }
    if (c == '\n' || c == '\r') {
      out.push_back(' ');
      ++i;
      continue;
    }
    out.push_back(c);
    ++i;
  }
  std::size_t b = 0;
  std::size_t e = out.size();
  while (b < e && (out[b] == ' ' || out[b] == '\t')) {
    ++b;
  }
  while (e > b && (out[e - 1] == ' ' || out[e - 1] == '\t')) {
    --e;
  }
  return out.substr(b, e - b);
}

// --- `#if` constant-expression evaluation ---

// Expression values are integers (fbc's pp folds everything to numbers) or
// strings (only the `"..."` literals and the string-valued `__FB_*` defines,
// which can only be compared). Truthiness is `num != 0`; comparisons yield
// FreeBASIC's -1/0 so `not`/`and` compose bitwise exactly like fbc's.
struct Value {
  bool isStr = false;
  std::int64_t num = 0;
  std::string str;
};

enum class Tk { Num, Str, Ident, Op, LParen, RParen, End };

struct Tok {
  Tk kind = Tk::End;
  std::int64_t num = 0;
  std::string s;
};

constexpr int kMaxExpansionDepth = 32;

// Tokens of a condition, with defined identifiers spliced by their replacement
// text (recursively, depth-capped). Any inability to produce a token stream —
// an unterminated string, a stray character, expansion overflow — poisons the
// whole condition: the chain becomes undecidable rather than guessed.
struct Tokenizer {
  PreprocMap const &defs;
  std::vector<Tok> out;
  bool ok = true;

  void tokenize(std::string_view s, int depth) {
    if (!ok) {
      return;
    }
    if (depth > kMaxExpansionDepth) {
      ok = false;
      return;
    }
    bool afterDefined = false; // next `(` starts a `defined(...)` argument
    bool inDefinedArg = false; // the identifier is a name, not an expansion
    std::size_t i = 0;
    while (i < s.size()) {
      char const c = s[i];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++i;
        continue;
      }
      if (c == '\'') {
        break; // comment
      }
      if (c == '(') {
        inDefinedArg = afterDefined;
        Tok t;
        t.kind = Tk::LParen;
        out.push_back(std::move(t));
        ++i;
        continue;
      }
      if (c == ')') {
        afterDefined = false;
        inDefinedArg = false;
        Tok t;
        t.kind = Tk::RParen;
        out.push_back(std::move(t));
        ++i;
        continue;
      }
      if (c == '"') {
        ++i;
        std::string v;
        bool closed = false;
        while (i < s.size()) {
          if (s[i] == '"') {
            if (i + 1 < s.size() && s[i + 1] == '"') {
              v.push_back('"');
              i += 2;
              continue;
            }
            ++i;
            closed = true;
            break;
          }
          v.push_back(s[i]);
          ++i;
        }
        afterDefined = false;
        if (!closed) {
          ok = false;
          return;
        }
        Tok t;
        t.kind = Tk::Str;
        t.s = std::move(v);
        out.push_back(std::move(t));
        continue;
      }
      if (c >= '0' && c <= '9') {
        afterDefined = false;
        if (!readDecimal(s, i)) {
          return;
        }
        continue;
      }
      if (c == '&' && i + 1 < s.size() &&
          (s[i + 1] == 'h' || s[i + 1] == 'H' || s[i + 1] == 'o' ||
           s[i + 1] == 'O' || s[i + 1] == 'b' || s[i + 1] == 'B')) {
        afterDefined = false;
        if (!readRadix(s, i)) {
          return;
        }
        continue;
      }
      if (isIdentStart(c)) {
        std::size_t b = i;
        while (i < s.size() && isIdentChar(s[i])) {
          ++i;
        }
        std::string const word = toLowerChars(s.substr(b, i - b));
        if (word == "defined") {
          afterDefined = true;
          Tok t;
          t.kind = Tk::Ident;
          t.s = word;
          out.push_back(std::move(t));
          continue;
        }
        if (inDefinedArg) {
          inDefinedArg = false;
          Tok t;
          t.kind = Tk::Ident;
          t.s = word;
          out.push_back(std::move(t));
          continue;
        }
        if (isWordOperator(word)) {
          Tok t;
          t.kind = Tk::Op;
          t.s = word;
          out.push_back(std::move(t));
          continue;
        }
        PreprocMap::const_iterator const it = defs.find(word);
        if (it != defs.end() && !it->second.functionLike &&
            !it->second.value.empty()) {
          tokenize(it->second.value, depth + 1); // splice, maybe recursively
          continue;
        }
        Tok t;
        t.kind = Tk::Ident;
        t.s = word;
        out.push_back(std::move(t));
        continue;
      }
      // Symbolic operators, longest first.
      if (i + 1 < s.size()) {
        std::string_view const two = s.substr(i, 2);
        if (two == "<>" || two == "<=" || two == ">=") {
          Tok t;
          t.kind = Tk::Op;
          t.s = std::string(two);
          out.push_back(std::move(t));
          i += 2;
          continue;
        }
      }
      if (c == '=' || c == '<' || c == '>' || c == '+' || c == '-' ||
          c == '*' || c == '/' || c == '\\' || c == '&' || c == '^') {
        Tok t;
        t.kind = Tk::Op;
        t.s.assign(1, c);
        out.push_back(std::move(t));
        ++i;
        continue;
      }
      ok = false;
      return;
    }
  }

private:
  bool readDecimal(std::string_view s, std::size_t &i) {
    std::int64_t v = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      v = v * 10 + (s[i] - '0');
      ++i;
      any = true;
    }
    if (!any) {
      ok = false;
      return false;
    }
    Tok t;
    t.kind = Tk::Num;
    t.num = v;
    out.push_back(std::move(t));
    return true;
  }

  bool readRadix(std::string_view s, std::size_t &i) {
    ++i; // '&'
    char const base = s[i];
    ++i;
    int const radix = base == 'h' || base == 'H'   ? 16
                      : base == 'o' || base == 'O' ? 8
                                                   : 2;
    std::int64_t v = 0;
    bool any = false;
    for (;;) {
      if (i >= s.size()) {
        break;
      }
      char const d = s[i];
      int digit = -1;
      if (d >= '0' && d <= '9') {
        digit = d - '0';
      } else if (d >= 'a' && d <= 'f') {
        digit = d - 'a' + 10;
      } else if (d >= 'A' && d <= 'F') {
        digit = d - 'A' + 10;
      }
      if (digit < 0 || digit >= radix) {
        break;
      }
      v = v * radix + digit;
      ++i;
      any = true;
    }
    if (!any) {
      ok = false;
      return false;
    }
    Tok t;
    t.kind = Tk::Num;
    t.num = v;
    out.push_back(std::move(t));
    return true;
  }
};

// Precedence, loosest first, matching OpPrecedence.md's table (which the fbc
// probes confirm): XOR < IMP < EQV < OR < AND < NOT < comparisons < & <
// +/- < SHL/SHR < MOD < \ < */ < unary minus < ^.
struct Expr {
  std::vector<Tok> const &toks;
  std::size_t pos = 0;
  PreprocMap const &defs_;

  Tok const &peek() const {
    static Tok const eof;
    return pos < toks.size() ? toks[pos] : eof;
  }

  bool isOp(std::string_view op) const {
    Tok const &t = peek();
    return t.kind == Tk::Op && t.s == op;
  }

  void next() {
    if (pos < toks.size()) {
      ++pos;
    }
  }

  std::optional<Value> parse() {
    std::optional<Value> v = parseXor();
    if (!v || peek().kind != Tk::End) {
      return std::nullopt;
    }
    return v;
  }

private:
  static std::optional<Value> fail() { return std::nullopt; }

  std::optional<Value> parseXor() {
    std::optional<Value> v = parseImp();
    while (v && isOp("xor")) {
      next();
      std::optional<Value> const b = parseImp();
      if (!b) {
        return fail();
      }
      v = bitwise(v, b, [](std::int64_t a, std::int64_t c) { return a ^ c; });
    }
    return v;
  }

  std::optional<Value> parseImp() {
    std::optional<Value> v = parseEqv();
    while (v && isOp("imp")) {
      next();
      std::optional<Value> const b = parseEqv();
      if (!b) {
        return fail();
      }
      v = bitwise(v, b,
                  [](std::int64_t a, std::int64_t c) { return (~a) | c; });
    }
    return v;
  }

  std::optional<Value> parseEqv() {
    std::optional<Value> v = parseOr();
    while (v && isOp("eqv")) {
      next();
      std::optional<Value> const b = parseOr();
      if (!b) {
        return fail();
      }
      v = bitwise(v, b,
                  [](std::int64_t a, std::int64_t c) { return ~(a ^ c); });
    }
    return v;
  }

  std::optional<Value> parseOr() {
    std::optional<Value> v = parseAnd();
    while (v && isOp("or")) {
      next();
      std::optional<Value> const b = parseAnd();
      if (!b) {
        return fail();
      }
      v = bitwise(v, b, [](std::int64_t a, std::int64_t c) { return a | c; });
    }
    return v;
  }

  std::optional<Value> parseAnd() {
    std::optional<Value> v = parseNot();
    while (v && isOp("and")) {
      next();
      std::optional<Value> const b = parseNot();
      if (!b) {
        return fail();
      }
      v = bitwise(v, b, [](std::int64_t a, std::int64_t c) { return a & c; });
    }
    return v;
  }

  std::optional<Value> parseNot() {
    if (isOp("not")) {
      next();
      std::optional<Value> const v = parseNot();
      if (!v || v->isStr) {
        return fail();
      }
      return Value{false, ~v->num, {}};
    }
    return parseCmp();
  }

  std::optional<Value> parseCmp() {
    std::optional<Value> v = parseConcat();
    while (v) {
      std::string_view op;
      Tok const &t = peek();
      if (t.kind == Tk::Op && (t.s == "=" || t.s == "<>" || t.s == "<" ||
                               t.s == "<=" || t.s == ">" || t.s == ">=")) {
        op = t.s;
      } else {
        break;
      }
      next();
      std::optional<Value> const b = parseConcat();
      if (!b) {
        return fail();
      }
      if (v->isStr != b->isStr) {
        return fail();
      }
      int cmp;
      if (v->isStr) {
        cmp = v->str == b->str ? 0 : (v->str < b->str ? -1 : 1);
      } else {
        cmp = v->num == b->num ? 0 : (v->num < b->num ? -1 : 1);
      }
      bool const truth = op == "="    ? cmp == 0
                         : op == "<>" ? cmp != 0
                         : op == "<"  ? cmp < 0
                         : op == "<=" ? cmp <= 0
                         : op == ">"  ? cmp > 0
                                      : cmp >= 0;
      v = Value{false, truth ? -1 : 0, {}};
    }
    return v;
  }

  std::optional<Value> parseConcat() {
    std::optional<Value> v = parseAdd();
    while (v && isOp("&")) {
      next();
      std::optional<Value> const b = parseAdd();
      if (!b) {
        return fail();
      }
      if (!v->isStr || !b->isStr) {
        return fail();
      }
      v = Value{true, 0, v->str + b->str};
    }
    return v;
  }

  std::optional<Value> parseAdd() {
    std::optional<Value> v = parseShift();
    while (v && (isOp("+") || isOp("-"))) {
      bool const add = isOp("+");
      next();
      std::optional<Value> const b = parseShift();
      if (!b) {
        return fail();
      }
      v = arith(add ? v->num + b->num : v->num - b->num, v->isStr, b->isStr);
      if (!v) {
        return fail();
      }
    }
    return v;
  }

  std::optional<Value> parseShift() {
    std::optional<Value> v = parseMod();
    while (v && (isOp("shl") || isOp("shr"))) {
      bool const left = isOp("shl");
      next();
      std::optional<Value> const b = parseMod();
      if (!b) {
        return fail();
      }
      if (v->isStr || b->isStr || b->num < 0 || b->num >= 64) {
        return fail();
      }
      std::uint64_t const a = static_cast<std::uint64_t>(v->num);
      v = Value{false,
                left ? static_cast<std::int64_t>(a << b->num)
                     : static_cast<std::int64_t>(v->num >> b->num),
                {}};
    }
    return v;
  }

  std::optional<Value> parseMod() {
    std::optional<Value> v = parseIDiv();
    while (v && isOp("mod")) {
      next();
      std::optional<Value> const b = parseIDiv();
      if (!b) {
        return fail();
      }
      if (v->isStr || b->isStr || b->num == 0) {
        return fail();
      }
      v = Value{false, v->num % b->num, {}};
    }
    return v;
  }

  std::optional<Value> parseIDiv() {
    std::optional<Value> v = parseMul();
    while (v && isOp("\\")) {
      next();
      std::optional<Value> const b = parseMul();
      if (!b) {
        return fail();
      }
      if (v->isStr || b->isStr || b->num == 0) {
        return fail();
      }
      v = Value{false, v->num / b->num, {}};
    }
    return v;
  }

  std::optional<Value> parseMul() {
    std::optional<Value> v = parseUnary();
    while (v && (isOp("*") || isOp("/"))) {
      bool const mul = isOp("*");
      next();
      std::optional<Value> const b = parseUnary();
      if (!b) {
        return fail();
      }
      if (v->isStr || b->isStr || (!mul && b->num == 0)) {
        return fail();
      }
      v = Value{false, mul ? v->num * b->num : v->num / b->num, {}};
    }
    return v;
  }

  std::optional<Value> parseUnary() {
    if (isOp("-")) {
      next();
      std::optional<Value> const v = parseUnary();
      if (!v || v->isStr) {
        return fail();
      }
      return Value{false, -v->num, {}};
    }
    return parsePow();
  }

  std::optional<Value> parsePow() {
    std::optional<Value> v = parsePrimary();
    while (v && isOp("^")) {
      next();
      std::optional<Value> const b = parseUnary();
      if (!b) {
        return fail();
      }
      if (v->isStr || b->isStr) {
        return fail();
      }
      double const r =
          std::pow(static_cast<double>(v->num), static_cast<double>(b->num));
      if (!std::isfinite(r)) {
        return fail();
      }
      v = Value{false, static_cast<std::int64_t>(r), {}};
    }
    return v;
  }

  std::optional<Value> parsePrimary() {
    Tok const &t = peek();
    if (t.kind == Tk::Num) {
      next();
      return Value{false, t.num, {}};
    }
    if (t.kind == Tk::Str) {
      next();
      return Value{true, 0, t.s};
    }
    if (t.kind == Tk::LParen) {
      next();
      std::optional<Value> v = parseXor();
      if (!v || peek().kind != Tk::RParen) {
        return fail();
      }
      next();
      return v;
    }
    if (t.kind == Tk::Ident && t.s == "defined") {
      next();
      if (peek().kind != Tk::LParen) {
        return fail();
      }
      next();
      if (peek().kind != Tk::Ident) {
        return fail();
      }
      // The arg was left unexpanded by the tokenizer: it is a plain name.
      bool const d = ownsKey(peek().s);
      next();
      if (peek().kind != Tk::RParen) {
        return fail();
      }
      next();
      return Value{false, d ? -1 : 0, {}};
    }
    if (t.kind == Tk::Ident) {
      // An identifier the tokenizer left alone is a macro name, an empty
      // define, or an unknown name — nothing the evaluator can trust.
      return fail();
    }
    return fail();
  }

  // `bitwise` and `arith` exist solely to split the lambdas above from the
  // option-channel plumbing; a failing operand reaches `fail()` and the chain
  // is undecidable.
  static std::optional<Value>
  bitwise(std::optional<Value> const &a, std::optional<Value> const &b,
          std::int64_t (*op)(std::int64_t, std::int64_t)) {
    if (!a || !b || a->isStr || b->isStr) {
      return std::nullopt;
    }
    return Value{false, op(a->num, b->num), {}};
  }

  static std::optional<Value> arith(std::int64_t r, bool aStr, bool bStr) {
    if (aStr || bStr) {
      return std::nullopt;
    }
    return Value{false, r, {}};
  }

  // `defined(name)`: defined means keyed — value, empty, function-like, all
  // count. `defs_` is the same map the tokenizer spliced from.
  bool ownsKey(std::string_view key) const {
    return defs_.count(std::string(key)) != 0;
  }
};

} // namespace

Preprocessor::Preprocessor() {
  // Resolve `defs_` by copying each seed in the constructor body rather than
  // delegating to a helper that would need the member set first.
  std::unordered_map<std::string, PreprocEntry> seed;
  auto def = [&seed](std::string_view name, std::string_view value,
                     bool functionLike = false) {
    seed[toLowerChars(name)] = PreprocEntry{functionLike, std::string(value)};
  };

  // fbc 1.10.2 facts, probed on linux-x86_64 (FreeBASIC.md §12 records that
  // the values describe this build): version, dialect, and runtime defaults.
  def("__FB_VER_MAJOR__", "1");
  def("__FB_VER_MINOR__", "10");
  def("__FB_VER_PATCH__", "2");
  def("__FB_VERSION__", "\"1.10.2\"");
  def("__FB_LANG__", "\"fb\"");
  def("__FB_FPMODE__", "\"precise\"");
  def("__FB_FPU__", "\"x87\"");
  def("__FB_ASM__", "\"intel\"");
  def("__FB_BACKEND__", "\"gcc\"");
  // Compile-mode flags, defaulted to a plain `fbc file.bas` run.
  def("__FB_DEBUG__", "0");
  def("__FB_MT__", "0");
  def("__FB_GUI__", "0");
  def("__FB_ERR__", "0");
  def("__FB_GCC__", "-1");
  def("__FB_OUT_EXE__", "-1");
  def("__FB_OUT_DLL__", "0");
  def("__FB_OUT_LIB__", "0");
  def("__FB_OUT_OBJ__", "0");
  def("__FB_OPTION_BYVAL__", "0");
  def("__FB_OPTION_DYNAMIC__", "0");
  def("__FB_OPTION_ESCAPE__", "0");
  def("__FB_OPTION_GOSUB__", "0");
  def("__FB_OPTION_EXPLICIT__", "-1");
  def("__FB_OPTION_PRIVATE__", "0");
  def("__FB_OPTIMIZE__", "0");
  def("__FB_VECTORIZE__", "0");
  // Predefined constants.
  def("TRUE", "-1");
  def("FALSE", "0");
  // Platform flags are defined-empty: usable through `#ifdef`/`defined()` only
  // (`#if __FB_LINUX__` is fbc error 20). The set describes the host this
  // server runs on — the same truth fbc would probe.
#if defined(_WIN32)
  def("__FB_WIN32__", "");
#elif defined(__APPLE__)
  def("__FB_DARWIN__", "");
  def("__FB_UNIX__", "");
#elif defined(__linux__)
  def("__FB_LINUX__", "");
  def("__FB_UNIX__", "");
#elif defined(__FreeBSD__)
  def("__FB_FREEBSD__", "");
  def("__FB_UNIX__", "");
#elif defined(__NetBSD__)
  def("__FB_NETBSD__", "");
  def("__FB_UNIX__", "");
#elif defined(__OpenBSD__)
  def("__FB_OPENBSD__", "");
  def("__FB_UNIX__", "");
#endif
#if defined(_WIN64) || defined(__LP64__) || defined(_LP64)
  def("__FB_64BIT__", "");
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
  def("__FB_X86__", "");
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__) ||         \
    defined(_M_ARM)
  def("__FB_ARM__", "");
#elif defined(__powerpc64__) || defined(__powerpc__)
  def("__FB_PPC__", "");
#endif
  // __FB_MAIN__ is defined-empty: fbc sets it for a standalone module, and the
  // server has no "main module" concept, so a program-shaped source sees it
  // reachable and a library module's guard is the documented divergence
  // (FreeBASIC.md §12).
  def("__FB_MAIN__", "");
  defines_ = std::move(seed);
}

PreprocEntry const *Preprocessor::entryFor(std::string_view name) const {
  auto const it = defines_.find(toLowerChars(name));
  return it == defines_.end() ? nullptr : &it->second;
}

bool Preprocessor::isDefined(std::string_view name) const {
  return defines_.count(toLowerChars(name)) != 0;
}

bool Preprocessor::outerActive() const {
  for (Frame const &f : frames_) {
    if (!f.curActive) {
      return false;
    }
  }
  return true;
}

bool Preprocessor::active() const { return !inMacroBody_ && outerActive(); }

bool Preprocessor::evaluateCondition(std::string_view text,
                                     bool *decided) const {
  *decided = false;
  Tokenizer tz{defines_};
  tz.tokenize(text, 0);
  if (!tz.ok) {
    return false;
  }
  Expr p{tz.out, 0, defines_};
  std::optional<Value> const v = p.parse();
  if (!v || v->isStr) {
    return false;
  }
  *decided = true;
  return v->num != 0;
}

PreprocFeedResult Preprocessor::feed(std::string_view line) {
  static const std::string_view kEndmacro = "endmacro";
  std::string const w = toLowerChars(preprocessorWord(line));
  PreprocFeedResult r;

  // A `#macro` body is opaque text: only `#endmacro` exits it, and nothing
  // else inside — a `#if`, a nested `#define` — takes effect.
  if (inMacroBody_) {
    if (w == kEndmacro) {
      inMacroBody_ = false;
    }
    return r;
  }

  if (w == "if" || w == "ifdef" || w == "ifndef") {
    Frame f;
    f.parentActive = outerActive();
    bool decided = true;
    bool truth = false;
    if (w == "ifdef") {
      truth = isDefined(firstIdentifier(line));
    } else if (w == "ifndef") {
      truth = !isDefined(firstIdentifier(line));
    } else {
      truth = evaluateCondition(restAfterWord(line), &decided);
    }
    f.undecided = !decided;
    f.taken = decided && truth;
    f.curActive = f.parentActive && decided && truth;
    frames_.push_back(f);
    return r;
  }

  if (w == "elseif" || w == "elseifdef" || w == "elseifndef") {
    if (frames_.empty()) {
      r.strayCloser = true;
      return r;
    }
    Frame &f = frames_.back();
    if (f.undecided || f.taken) {
      f.curActive = false; // chain dead or an earlier arm already lived
      return r;
    }
    bool truth = false;
    bool decided = true;
    if (w == "elseifdef") {
      truth = isDefined(firstIdentifier(line));
    } else if (w == "elseifndef") {
      truth = !isDefined(firstIdentifier(line));
    } else {
      decided = evaluateCondition(restAfterWord(line), &truth);
    }
    if (!decided) {
      f.undecided = true;
      f.curActive = false;
    } else {
      f.taken = truth;
      f.curActive = f.parentActive && truth;
    }
    return r;
  }

  if (w == "else") {
    if (frames_.empty()) {
      r.strayCloser = true;
      return r;
    }
    Frame &f = frames_.back();
    if (f.undecided) {
      f.curActive = false;
    } else if (!f.taken) {
      f.taken = true;
      f.curActive = f.parentActive;
    } else {
      f.curActive = false;
    }
    return r;
  }

  if (w == "endif") {
    if (frames_.empty()) {
      r.strayCloser = true;
      return r;
    }
    frames_.pop_back();
    return r;
  }

  // Everything below only happens in reachable code (an unreachable region's
  // defines never take effect, matching fbc).
  if (!outerActive()) {
    return r;
  }

  if (w == "define" || w == "macro") {
    PreprocDefine d;
    if (parseDefineLine(line, w == "macro", &d)) {
      PreprocEntry e;
      e.functionLike = d.functionLike || d.isMacro;
      if (!e.functionLike) {
        e.value = normalizedValue(line, d.nameEnd);
      }
      defines_[d.key] = std::move(e);
      if (w == "macro") {
        inMacroBody_ = true;
      }
      r.define = std::move(d);
    }
    return r;
  }

  if (w == "undef") {
    std::string_view const name = firstIdentifier(line);
    if (!name.empty()) {
      defines_.erase(toLowerChars(name));
    }
    return r;
  }

  return r; // include, pragma, print, ... — nothing for the preprocessor
}

} // namespace fblang