// Writes the generated editor grammars (`editors/*`) from the catalog. The
// emit logic lives in the shared `grammar_emitter` module so `grammar_checks`
// byte-diffs exactly what this tool would write.
//
//   gen_grammar [output-dir]     # default: editors
#include "grammar_emitter.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char **argv) {
  std::filesystem::path const outDir = argc > 1 ? argv[1] : "editors";

  std::error_code ec;
  std::filesystem::create_directories(outDir, ec);
  if (ec) {
    std::fprintf(stderr, "gen_grammar: cannot create %s: %s\n",
                 outDir.string().c_str(), ec.message().c_str());
    return 1;
  }

  for (fbgrammar::GeneratedFile const &file : fbgrammar::generate()) {
    std::filesystem::path const path = outDir / file.relativePath;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      std::fprintf(stderr, "gen_grammar: cannot open %s\n",
                   path.string().c_str());
      return 1;
    }
    out << file.content;
    if (!out) {
      std::fprintf(stderr, "gen_grammar: cannot write %s\n",
                   path.string().c_str());
      return 1;
    }
  }
  return 0;
}
