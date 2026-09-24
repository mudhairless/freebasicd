// i18n checks: GNU gettext wiring for log + diagnostic messages, plus the
// "FreeBASIC is never translated" invariants encoded as code.
//
//   1. Functional: with the CMake-built fblang-test catalog for the `de`
//      language (i18n-test-catalog target), binding that domain and relying on
//      LANGUAGE=de must return the translated msgstr, and trf() must replace
//      %s inside the *translated* template; unknown msgids fall back to the
//      msgid. Skipped (return 77) when no msgfmt was found at configure time.
//   2. Structural: no translatable literal in src/ may contain the word
//      `FreeBASIC` — it is a proper noun that only enters a message as a %s
//      argument — and none may contain a FreeBASIC keyword written with an
//      uppercase letter. Keywords (END, SUB, NEXT, ...) reach the user only as
//      dynamic insertions (block-closer display text, dialect names) that
//      translators never see; lowercase homographs in English prose ("for doc
//      comments") are ordinary words and stay allowed.
//   3. Freshness: every translatable literal must exist as a msgid in the
//      committed po/freebasiclsp.pot, so a new message cannot ship without the
//      template translators work from.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <libintl.h>
#include <locale.h>
#include <set>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "i18n.h"
#include "language.h"

namespace fs = std::filesystem;

using namespace fblang;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

namespace {

int failures = 0;

// True when `c` may appear inside an identifier.
bool isWordChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Decode a C string literal body (escapes as written by the source and by
// xgettext) into its plain text.
std::string decodeLiteral(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      char const e = raw[++i];
      switch (e) {
      case 'n':
        out.push_back('\n');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case '\\':
      case '"':
      case '\'':
        out.push_back(e);
        break;
      default:
        out.push_back('\\');
        out.push_back(e);
        break;
      }
    } else {
      out.push_back(raw[i]);
    }
  }
  return out;
}

struct Literal {
  std::string text; // decoded
  std::string site; // "file:offset"
};

// Every translatable literal in src/: direct `tr("..." / `trf("..." /
// `gettext("..."` calls with a literal first argument, concatenating adjacent
// C string literals (the lang-mode message is split over two source lines).
std::vector<Literal> collectLiterals() {
  std::vector<Literal> out;
  const char *const keywords[] = {"tr(", "trf(", "gettext("};
  for (fs::directory_iterator it(SRC_DIR), end; it != end; ++it) {
    fs::path const &p = it->path();
    std::string const ext = p.extension().string();
    if (ext != ".cpp" && ext != ".h") {
      continue;
    }
    std::string src;
    {
      std::ifstream in(p);
      if (!in) {
        std::printf("FAIL: cannot open %s\n", p.c_str());
        ++failures;
        continue;
      }
      src.assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
    }
    for (const char *kw : keywords) {
      size_t const kwLen = std::strlen(kw);
      for (size_t i = 0; i + kwLen <= src.size(); ++i) {
        if (src.compare(i, kwLen, kw) != 0) {
          continue;
        }
        // Must start at an identifier boundary (an attribute like `attr(` has
        // `tr(` inside it and must not count).
        if (i > 0 && isWordChar(src[i - 1])) {
          continue;
        }
        size_t pos = i + kwLen;
        if (pos >= src.size() || src[pos] != '"') {
          continue;
        }
        Literal lit;
        std::string raw;
        bool closed = false;
        while (pos < src.size() && src[pos] == '"') {
          ++pos;
          raw.clear();
          while (pos < src.size()) {
            char const c = src[pos++];
            if (c == '\\') {
              raw.push_back(c);
              if (pos < src.size()) {
                raw.push_back(src[pos++]);
              }
              continue;
            }
            if (c == '"') {
              break;
            }
            raw.push_back(c);
          }
          lit.text += decodeLiteral(raw);
          closed = true;
          while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' ||
                                      src[pos] == '\n' || src[pos] == '\r')) {
            ++pos;
          }
          if (pos >= src.size() || src[pos] != '"') {
            break;
          }
        }
        if (!closed || lit.text.empty()) {
          continue;
        }
        lit.site = p.filename().string() + ":" + std::to_string(i);
        out.push_back(std::move(lit));
      }
    }
  }
  return out;
}

// Split a literal into word tokens (anything else is a separator), keeping the
// original spelling so the invariant checks can see how each keyword is
// written.
std::vector<std::string> wordsOf(std::string_view s) {
  std::vector<std::string> out;
  std::string cur;
  for (char const c : s) {
    if (isWordChar(c)) {
      cur.push_back(c);
    } else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) {
    out.push_back(cur);
  }
  return out;
}

bool hasUppercase(std::string_view s) {
  return std::any_of(s.begin(), s.end(),
                     [](char c) { return c >= 'A' && c <= 'Z'; });
}

// msgid values from a .pot file: lines starting `msgid "..."` plus any quoted
// continuation lines (xgettext wraps long msgids as `msgid ""` followed by
// quoted fragments), decoded with the same escaping the source literals use.
// The catalog header block (the very first `msgid ""`) is skipped.
std::set<std::string> potMsgids() {
  std::set<std::string> out;
  std::ifstream in(FBLANG_POT);
  if (!in) {
    std::printf("FAIL: cannot open pot %s\n", FBLANG_POT);
    ++failures;
    return out;
  }
  std::string cur; // msgid under assembly ("" when its block is the header)
  bool inMsgid = false; // inside a msgid block, collecting continuation lines
  size_t blockNo = 0;   // block 0 is the catalog header, not a message
  std::string line;
  while (std::getline(in, line)) {
    std::string_view l(line);
    // strip a trailing CR from CRLF files
    if (!l.empty() && l.back() == '\r') {
      l.remove_suffix(1);
    }
    if (l.rfind("msgid ", 0) == 0) {
      if (!cur.empty()) {
        out.insert(cur);
      }
      std::string_view const body = l.substr(std::strlen("msgid "));
      cur.clear();
      if (body.size() >= 2 && body.front() == '"' && body.back() == '"') {
        cur = decodeLiteral(body.substr(1, body.size() - 2));
      }
      inMsgid = true;
      ++blockNo;
      continue;
    }
    if (inMsgid && l.size() >= 2 && l.front() == '"' && l.back() == '"') {
      // Wrapped msgid: append the fragment, unless this is the header block.
      if (blockNo > 1) {
        cur += decodeLiteral(l.substr(1, l.size() - 2));
      }
      continue;
    }
    // Anything else (msgstr, flags, comments) ends the current msgid block.
    inMsgid = false;
  }
  if (!cur.empty()) {
    out.insert(cur);
  }
  return out;
}

void TestInitI18nSmoke() {
  // Binds the real domain to the build/install catalog tree; with no
  // translation installed tr() falls back to the msgid.
  fblang::initI18n();
  CHECK(std::string(fblang::tr("workspace root: %s")) == "workspace root: %s");
  CHECK(fblang::trf("Expected '%s'", "END SUB") == "Expected 'END SUB'");
}

void TestLiteralInvariants() {
  std::vector<std::string_view> const reserved = reservedWords();
  for (Literal const &lit : collectLiterals()) {
    for (std::string const &word : wordsOf(lit.text)) {
      std::string wordLower = word;
      for (char &c : wordLower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (wordLower == "freebasic") {
        std::printf("  %s: literal contains the proper noun FreeBASIC:\n"
                    "    \"%s\"\n",
                    lit.site.c_str(), lit.text.c_str());
        CHECK(false);
        continue;
      }
      bool const keyword = std::find(reserved.begin(), reserved.end(),
                                     wordLower) != reserved.end() ||
                           isBuiltinType(wordLower);
      // An uppercase *spelling of the keyword itself* inside a translatable
      // literal is a keyword emitted as message text — it would be translated.
      // Keywords must reach messages as trf() arguments instead. Lowercase
      // homographs in English prose ("not valid", "for doc comments") are
      // ordinary words and stay allowed.
      if (keyword && hasUppercase(word)) {
        std::printf("  %s: literal embeds the keyword '%s' (uppercase):\n"
                    "    \"%s\"\n",
                    lit.site.c_str(), word.c_str(), lit.text.c_str());
        CHECK(false);
      }
    }
  }
}

void TestPotFreshness() {
  std::vector<Literal> const lits = collectLiterals();
  CHECK(!lits.empty());
  std::set<std::string> const pot = potMsgids();
  CHECK(!pot.empty());
  for (Literal const &lit : lits) {
    if (pot.find(lit.text) == pot.end()) {
      std::printf("  %s: literal missing from po/freebasiclsp.pot:\n"
                  "    \"%s\"\n    (run: cmake --build build --target "
                  "po-template, then update-po)\n",
                  lit.site.c_str(), lit.text.c_str());
      CHECK(false);
    }
  }
}

// Returns false when the runtime catalog is unavailable (no msgfmt at
// configure time, or a C-ish default message locale), so main() can report
// SKIP via return code 77.
bool TestGettextRoundTrip() {
#ifdef FBLANG_TEST_LOCALE_DIR
  // The de catalog builds to <build>/i18n-test/de/LC_MESSAGES/fblang-test.mo.
  (void)bindtextdomain("fblang-test", FBLANG_TEST_LOCALE_DIR);
  (void)bind_textdomain_codeset("fblang-test", "UTF-8");
  (void)textdomain("fblang-test");
#ifdef _WIN32
  (void)_putenv_s("LANGUAGE", "de");
#else
  (void)setenv("LANGUAGE", "de", 1);
#endif
  // glibc's gettext consults LANGUAGE only when LC_MESSAGES is a real
  // language locale; under C/POSIX (all of LC_ALL=C, C.UTF-8, POSIX) it is
  // ignored. Activate the environment's default message locale and bail out
  // (SKIP) when that default is C-ish, which keeps the check green on
  // minimal containers while still exercising the full runtime path on
  // ordinary machines.
  if (setlocale(LC_MESSAGES, "") == nullptr) {
    std::printf("note: setlocale(LC_MESSAGES, \"\") failed; skipping the "
                "functional round-trip\n");
    return false;
  }
  char const *msg = setlocale(LC_MESSAGES, nullptr);
  if (msg == nullptr || msg[0] == '\0' || msg[0] == 'C' || msg[0] == 'P') {
    std::printf("note: default LC_MESSAGES \"%s\" ignores LANGUAGE; "
                "skipping the functional round-trip\n",
                msg != nullptr ? msg : "(null)");
    return false;
  }

  CHECK(std::string(fblang::tr("hello %s")) == "Grüße aus dem Test: %s");
  CHECK(fblang::trf("hello %s", "Welt") == "Grüße aus dem Test: Welt");
  CHECK(std::string(fblang::tr("not translated yet")) == "not translated yet");
  return true;
#else
  return false;
#endif
}

// ctest convention: 77 marks the suite as skipped (SKIP_RETURN_CODE in
// CMakeLists.txt), used when the runtime catalog or locale cannot exercise the
// functional part.
constexpr int kSkipExitCode = 77;

} // namespace

int main() {
  TestInitI18nSmoke();
  TestLiteralInvariants();
  TestPotFreshness();
  bool const functional = TestGettextRoundTrip();

  if (failures != 0) {
    std::printf("i18n_checks: FAIL\n");
    return 1;
  }
  if (!functional) {
    std::printf("i18n_checks: SKIP (functional gettext round-trip "
                "unavailable: no msgfmt at configure time, or the default "
                "message locale is C-ish)\n");
    return kSkipExitCode;
  }
  std::printf("i18n_checks: PASS\n");
  return 0;
}