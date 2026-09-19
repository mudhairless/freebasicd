#include "index.h"

#include "resolve.h"
#include "symbols.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fblang {

namespace {

constexpr auto kRescanDebounce = std::chrono::milliseconds(300);

// Skip directories that never hold authored sources.
bool isSkippedDir(std::filesystem::path const &name) {
  std::string const n = toLowerChars(name.string());
  if (!n.empty() && n[0] == '.') {
    return true;
  }
  return n == "build" || n == "bin" || n == "obj";
}

bool hasSourceSuffix(std::filesystem::path const &p) {
  std::string const ext = toLowerChars(p.extension().string());
  return ext == ".bas" || ext == ".bi";
}

bool readTextFile(std::filesystem::path const &p, std::string *out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(in),
              std::istreambuf_iterator<char>());
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Normalization helpers
// ---------------------------------------------------------------------------

std::string normalizePath(std::filesystem::path const &path) {
  std::error_code ec;
  std::filesystem::path abs = std::filesystem::absolute(path, ec);
  if (ec) {
    abs = path;
  }
  std::string s = abs.lexically_normal().string();
#ifdef _WIN32
  for (char &c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
#endif
  return s;
}

bool statFile(std::filesystem::path const &path, std::uint64_t *mtime,
              std::uint64_t *size) {
  std::error_code ec;
  std::filesystem::file_status const st = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::is_regular_file(st)) {
    return false;
  }
  auto lm = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return false;
  }
  if (mtime != nullptr) {
    *mtime = static_cast<std::uint64_t>(lm.time_since_epoch().count());
  }
  if (size != nullptr) {
    *size = std::filesystem::file_size(path, ec);
    if (ec) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Free functions (M5)
// ---------------------------------------------------------------------------

// System FreeBASIC headers ship next to the compiler install: Windows keeps
// them in an `inc` child of the executable's folder, POSIX in
// `<prefix>/include/freebasic` (i.e. `<exeDir>/../include/freebasic`).
std::filesystem::path
systemIncludeDirForExecutable(std::filesystem::path const &fbcExeDir) {
#ifdef _WIN32
  return fbcExeDir / "inc";
#else
  return fbcExeDir.parent_path() / "include" / "freebasic";
#endif
}

std::optional<std::filesystem::path>
findFbcExecutableDir(std::string const &pathEnv) {
  std::vector<std::string> dirs;
  {
    char const sep =
#ifdef _WIN32
        ';';
#else
        ':';
#endif
    std::size_t start = 0;
    while (start <= pathEnv.size()) {
      std::size_t const pos = pathEnv.find(sep, start);
      std::string const dir = pathEnv.substr(
          start, pos == std::string::npos ? std::string::npos : pos - start);
      if (!dir.empty()) {
        dirs.push_back(dir);
      }
      if (pos == std::string::npos) {
        break;
      }
      start = pos + 1;
    }
  }

#ifdef _WIN32
  static constexpr char const *const kExecutables[] = {"fbc.exe", "fbc32.exe",
                                                       "fbc64.exe"};
#else
  static constexpr char const *const kExecutables[] = {"fbc"};
#endif
  for (std::string const &dir : dirs) {
    for (char const *name : kExecutables) {
      std::filesystem::path const candidate = std::filesystem::path(dir) / name;
      std::error_code ec;
      if (!std::filesystem::is_regular_file(candidate, ec)) {
        continue;
      }
#ifndef _WIN32
      std::error_code pec;
      auto const perms = std::filesystem::status(candidate, pec).permissions();
      bool const executable =
          !pec && (perms & (std::filesystem::perms::owner_exec |
                            std::filesystem::perms::group_exec |
                            std::filesystem::perms::others_exec)) !=
                      std::filesystem::perms::none;
      if (!executable) {
        continue;
      }
#endif
      // Resolve symlinks so the include dir tracks the real install, not
      // a PATH shim (e.g. /usr/bin/fbc -> /opt/fb/bin/fbc).
      std::filesystem::path resolved = candidate;
      std::error_code cec;
      if (std::filesystem::path const canon =
              std::filesystem::canonical(candidate, cec);
          !cec) {
        resolved = canon;
      }
      return resolved.parent_path();
    }
  }
  return std::nullopt;
}

std::filesystem::path defaultSystemIncludeDir() {
  // Magic static: computed once on first use; guaranteed thread-safe.
  static std::filesystem::path const cached = [] {
    char const *env = std::getenv("PATH");
    std::optional<std::filesystem::path> const exeDir =
        findFbcExecutableDir(env ? env : "");
    if (!exeDir) {
      return std::filesystem::path{};
    }
    std::filesystem::path const dir = systemIncludeDirForExecutable(*exeDir);
    std::error_code ec;
    return std::filesystem::is_directory(dir, ec) ? dir
                                                  : std::filesystem::path{};
  }();
  return cached;
}

// True when `normalizedPath` is `normalizedRoot` itself or lies under it. A
// mirror of WorkspaceIndex::isInsideRoot for the free include-search helpers.
bool pathAtOrUnder(std::string const &normalizedPath,
                   std::string const &normalizedRoot) {
  if (normalizedPath.size() < normalizedRoot.size()) {
    return false;
  }
  if (normalizedPath.compare(0, normalizedRoot.size(), normalizedRoot) != 0) {
    return false;
  }
  if (normalizedPath.size() == normalizedRoot.size()) {
    return true;
  }
  char const next = normalizedPath[normalizedRoot.size()];
  return next == '/' || next == '\\';
}

// The FreeBASIC project directory of an including file: the nearest ancestor
// directory (the file excluded) that has an `inc` or `include` subdirectory —
// fbc's canonical `-i inc` layout, where shared headers live in a sibling of
// the source tree. Used as an additional include-search anchor for documents
// opened from a sibling project outside the workspace root. Returns an empty
// path when no ancestor matches.
std::filesystem::path projectIncludeDirOf(std::filesystem::path const &file) {
  std::filesystem::path dir = file.parent_path();
  for (;;) {
    if (dir.empty()) {
      break;
    }
    for (char const *sub : {"inc", "include"}) {
      std::error_code ec;
      if (std::filesystem::is_directory(dir / sub, ec)) {
        return dir;
      }
    }
    std::filesystem::path const parent = dir.parent_path();
    if (parent == dir) {
      break;
    }
    dir = parent;
  }
  return {};
}

std::optional<std::string>
resolveIncludeTarget(std::string_view literal,
                     std::filesystem::path const &includingFile,
                     std::filesystem::path const &workspaceRoot,
                     std::filesystem::path const &systemIncludeDir) {
  std::string s(literal);
  std::replace(s.begin(), s.end(), '\\', '/');
  std::filesystem::path const lit(s);

  // 1. The including file's own directory (mirrors fbc: source-relative
  //    headers win).
  std::error_code ec;
  std::filesystem::path candidate = includingFile.parent_path() / lit;
  if (std::filesystem::is_regular_file(candidate, ec)) {
    return normalizePath(candidate);
  }

  // 2. Relative to the workspace root.
  candidate = workspaceRoot / lit;
  if (std::filesystem::is_regular_file(candidate, ec)) {
    return normalizePath(candidate);
  }

  // 3. The including file's own FreeBASIC project directory (a directory
  //    with an `inc`/`include` child of the source tree - fbc's `-i inc`
  //    layout) and its immediate subdirectories. Only consulted when the
  //    document lies outside the workspace root: an editor may open a file
  //    from a sibling project whose headers the workspace-root search can
  //    never reach (e.g. the project compiles from its own folder, not the
  //    client's). In-root documents keep the workspace-root precedence
  //    below unchanged.
  if (!pathAtOrUnder(normalizePath(includingFile),
                     normalizePath(workspaceRoot))) {
    std::filesystem::path const projDir = projectIncludeDirOf(includingFile);
    if (!projDir.empty()) {
      candidate = projDir / lit;
      if (std::filesystem::is_regular_file(candidate, ec)) {
        return normalizePath(candidate);
      }
      std::error_code rerr;
      std::filesystem::directory_iterator const end;
      for (std::filesystem::directory_iterator it(projDir, rerr); it != end;
           it.increment(rerr)) {
        if (rerr) {
          break;
        }
        std::error_code sterr;
        std::filesystem::file_status const st = it->status(sterr);
        if (sterr || !std::filesystem::is_directory(st)) {
          continue;
        }
        candidate = it->path() / lit;
        if (std::filesystem::is_regular_file(candidate, ec)) {
          return normalizePath(candidate);
        }
      }
    }
  }

  // 4. Every immediate subdirectory of the workspace root. FreeBASIC
  //    projects keep shared headers in an `inc` / `include` / `src` (etc.)
  //    child of the root, so `#include "folder/file.bi"` matches under such
  //    a child without knowing which one holds the headers.
  {
    std::error_code rerr;
    std::filesystem::directory_iterator const end;
    for (std::filesystem::directory_iterator it(workspaceRoot, rerr); it != end;
         it.increment(rerr)) {
      if (rerr) {
        break;
      }
      std::error_code sterr;
      std::filesystem::file_status const st = it->status(sterr);
      if (sterr || !std::filesystem::is_directory(st)) {
        continue;
      }
      candidate = it->path() / lit;
      if (std::filesystem::is_regular_file(candidate, ec)) {
        return normalizePath(candidate);
      }
    }
  }

  // 5. The FreeBASIC installation's own header folder (resolved from `fbc`
  //    on PATH). Additional dirs (fbc `-i`) join via an M11 settings option.
  if (!systemIncludeDir.empty()) {
    candidate = systemIncludeDir / lit;
    if (std::filesystem::is_regular_file(candidate, ec)) {
      return normalizePath(candidate);
    }
  }
  return std::nullopt;
}

IndexedFile indexedFileFromAnalysis(std::string const &normalizedPath,
                                    std::uint64_t mtime, std::uint64_t size,
                                    AnalyzedDoc const &doc,
                                    std::filesystem::path const &workspaceRoot,
                                    bool fromDisk) {
  IndexedFile f;
  f.path = normalizedPath;
  f.mtime = mtime;
  f.size = size;
  f.lang = doc.parse.lang;
  f.roots = doc.parse.roots;
  f.pragmaOnce = doc.pragmaOnce;
  f.fromDisk = fromDisk;
  for (IncludeDirective const &inc : doc.includes) {
    IncludeEdge e;
    e.literal = inc.literal;
    e.targetRange = inc.target;
    e.once = inc.once;
    if (std::optional<std::string> t =
            resolveIncludeTarget(inc.literal, normalizedPath, workspaceRoot)) {
      e.target = std::move(*t);
    }
    f.includes.push_back(std::move(e));
  }
  return f;
}

// ---------------------------------------------------------------------------
// Projection helpers (called under mu_ by upsert / remove / scan)
// ---------------------------------------------------------------------------

void WorkspaceIndex::addToProjections(
    std::shared_ptr<IndexedFile const> const &f) {
  outInc_[f->path] = f->includes;
  for (Symbol const &root : f->roots) {
    if (root.key.empty()) {
      continue;
    }
    byKey_[root.key].push_back(KeyedDecl{f, &root});
  }
}

void WorkspaceIndex::subtractFromProjections(
    std::shared_ptr<IndexedFile const> const &f) {
  outInc_.erase(f->path);
  for (Symbol const &root : f->roots) {
    if (root.key.empty()) {
      continue;
    }
    auto it = byKey_.find(root.key);
    if (it == byKey_.end()) {
      continue;
    }
    auto &vec = it->second;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                             [p = f.get()](KeyedDecl const &kd) {
                               return kd.file.get() == p;
                             }),
              vec.end());
    if (vec.empty()) {
      byKey_.erase(it);
    }
  }
}

// ---------------------------------------------------------------------------
// WorkspaceIndex lifecycle
// ---------------------------------------------------------------------------

WorkspaceIndex::WorkspaceIndex(std::filesystem::path const &root)
    : root_(std::filesystem::absolute(root).lexically_normal()),
      rootNorm_(normalizePath(root_)) {}

bool WorkspaceIndex::isInsideRoot(std::string const &normalizedPath) const {
  if (normalizedPath.size() < rootNorm_.size()) {
    return false;
  }
  if (normalizedPath.compare(0, rootNorm_.size(), rootNorm_) != 0) {
    return false;
  }
  if (normalizedPath.size() == rootNorm_.size()) {
    return true; // the root itself (a directory, never a source file)
  }
  char const next = normalizedPath[rootNorm_.size()];
  return next == '/' || next == '\\';
}

WorkspaceIndex::~WorkspaceIndex() { close(); }

void WorkspaceIndex::open() {
  if (running_.exchange(true)) {
    return;
  }
  rescan_ = std::thread([this] { rescanLoop(); });
}

void WorkspaceIndex::close() {
  running_.store(false);
  // Wake the rescan loop's parked waits; a scan it already started finishes
  // before the join below (rescan_ is joined before scanner_ so the two can
  // never join the same scanner_ concurrently).
  rescanCv_.notify_all();
  if (rescan_.joinable()) {
    rescan_.join();
  }
  if (scanner_.joinable()) {
    scanner_.join();
  }
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

void WorkspaceIndex::scan(bool async) {
  if (async) {
    if (scanner_.joinable()) {
      scanner_.join();
    }
    scanner_ = std::thread([this] { scan(false); });
    return;
  }

  std::error_code ec;
  std::set<std::string> seen;
  std::filesystem::recursive_directory_iterator it(
      root_, std::filesystem::directory_options::skip_permission_denied, ec);
  std::filesystem::recursive_directory_iterator const end;
  if (ec) {
    return;
  }

  for (; it != end; it.increment(ec)) {
    if (ec) {
      break;
    }
    std::filesystem::directory_entry const entry = *it;
    std::filesystem::file_status const st = entry.status(ec);
    if (ec) {
      continue;
    }
    if (std::filesystem::is_directory(st) &&
        isSkippedDir(entry.path().filename())) {
      it.disable_recursion_pending();
      continue;
    }
    if (std::filesystem::is_directory(st)) {
      continue;
    }
    if (!hasSourceSuffix(entry.path())) {
      continue;
    }
    std::string const norm = normalizePath(entry.path());
    // Defensive: a junction/symlink inside the root could resolve to a
    // tree outside it. The index is strictly workspace-scoped.
    if (!isInsideRoot(norm)) {
      it.disable_recursion_pending();
      continue;
    }
    seen.insert(norm);

    std::uint64_t mtime = 0;
    std::uint64_t size = 0;
    if (!statFile(entry.path(), &mtime, &size)) {
      continue;
    }
    {
      std::lock_guard<std::mutex> const lk(mu_);
      auto found = files_.find(norm);
      // A fromDisk=false open-buffer entry must never satisfy scan's
      // cache-hit: scan is disk truth, buffers are live truth.
      if (found != files_.end() && found->second->fromDisk &&
          found->second->mtime == mtime && found->second->size == size) {
        continue;
      }
    }

    std::string content;
    if (!readTextFile(entry, &content)) {
      continue;
    }
    AnalyzedDoc const doc = analyze(content);
    upsert(indexedFileFromAnalysis(norm, mtime, size, doc, root_, true));
  }

  {
    std::lock_guard<std::mutex> const lk(mu_);
    for (auto itm = files_.begin(); itm != files_.end();) {
      if (!std::filesystem::exists(itm->second->path, ec) ||
          seen.count(itm->second->path) == 0) {
        subtractFromProjections(itm->second);
        itm = files_.erase(itm);
      } else {
        ++itm;
      }
    }
  }
}

void WorkspaceIndex::watchedFilesChanged() {
  {
    std::lock_guard<std::mutex> const lk(rescanMu_);
    rescanQueued_ = true;
  }
  rescanCv_.notify_all();
}

void WorkspaceIndex::rescanLoop() {
  std::unique_lock<std::mutex> lk(rescanMu_);
  while (running_.load()) {
    // Park until an event arrives (or shutdown).
    rescanCv_.wait(lk, [this] { return rescanQueued_ || !running_.load(); });
    if (!running_.load()) {
      break;
    }
    // Wait out an idle window so a burst of events coalesces into one
    // scan; nothing but shutdown aborts this wait early, so every event
    // in the window is absorbed by the single scan that follows.
    auto const idle = std::chrono::steady_clock::now() + kRescanDebounce;
    if (rescanCv_.wait_until(lk, idle, [this] { return !running_.load(); })) {
      break;
    }
    rescanQueued_ = false;
    lk.unlock();
    scan(true);
    lk.lock();
  }
}

// ---------------------------------------------------------------------------
// Insert / remove
// ---------------------------------------------------------------------------

void WorkspaceIndex::upsert(IndexedFile entry) {
  entry.path = normalizePath(entry.path);
  // The index is strictly workspace-scoped: never index files outside the
  // root, e.g. an open-buffer edit to a system header or a file in a
  // sibling directory. Such an edit still invalidates any on-demand closure
  // cache for the path, so the next request re-reads the live buffer (see
  // ensureClosure).
  if (!isInsideRoot(entry.path)) {
    std::lock_guard<std::mutex> const lk(mu_);
    closureFiles_.erase(entry.path);
    return;
  }
  // Capture the key before the move: C++17 sequences the right operand of
  // `operator=` first, so moving `entry` into the shared_ptr must not race
  // the subscript's key evaluation (which would leave an empty key).
  std::string const key = entry.path;
  std::shared_ptr<IndexedFile const> ptr;
  {
    std::lock_guard<std::mutex> const lk(mu_);
    auto existing = files_.find(key);
    if (existing != files_.end()) {
      subtractFromProjections(existing->second);
    }
    ptr = std::make_shared<IndexedFile const>(std::move(entry));
    files_[key] = ptr;
    addToProjections(ptr);
  }
}

void WorkspaceIndex::remove(std::string const &path) {
  std::string const norm = normalizePath(path);
  {
    std::lock_guard<std::mutex> const lk(mu_);
    auto it = files_.find(norm);
    if (it != files_.end()) {
      subtractFromProjections(it->second);
      files_.erase(it);
    }
    closureFiles_.erase(norm);
  }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::vector<std::shared_ptr<IndexedFile const>>
WorkspaceIndex::snapshot() const {
  std::lock_guard<std::mutex> const lk(mu_);
  std::vector<std::shared_ptr<IndexedFile const>> out;
  out.reserve(files_.size());
  for (auto const &kv : files_) {
    out.push_back(kv.second);
  }
  return out;
}

std::size_t WorkspaceIndex::size() const {
  std::lock_guard<std::mutex> const lk(mu_);
  return files_.size();
}

std::vector<KeyedDecl> WorkspaceIndex::byKey(std::string const &key) const {
  std::lock_guard<std::mutex> const lk(mu_);
  auto it = byKey_.find(key);
  if (it == byKey_.end()) {
    return {};
  }
  return it->second;
}

std::shared_ptr<IndexedFile const>
WorkspaceIndex::fileAt(std::string const &normalizedPath) const {
  std::lock_guard<std::mutex> const lk(mu_);
  std::string const norm = normalizePath(normalizedPath);
  auto it = files_.find(norm);
  if (it != files_.end()) {
    return it->second;
  }
  auto cit = closureFiles_.find(norm);
  if (cit != closureFiles_.end()) {
    return cit->second.entry;
  }
  return nullptr;
}

std::vector<std::string>
WorkspaceIndex::transitiveIncludes(std::string const &normalizedPath) const {
  std::lock_guard<std::mutex> const lk(mu_);
  std::vector<std::string> out;
  std::set<std::string> visited;
  visited.insert(normalizedPath);
  std::function<void(std::string const &)> visit =
      [&](std::string const &node) {
        std::vector<IncludeEdge> const *edges = nullptr;
        auto it = outInc_.find(node);
        if (it != outInc_.end()) {
          edges = &it->second;
        } else {
          auto cit = closureFiles_.find(node);
          if (cit != closureFiles_.end()) {
            edges = &cit->second.entry->includes;
          }
        }
        if (edges == nullptr) {
          return;
        }
        for (IncludeEdge const &e : *edges) {
          if (e.target.empty()) {
            continue;
          }
          if (visited.insert(e.target).second) {
            out.push_back(e.target);
            visit(e.target);
          }
        }
      };
  visit(normalizedPath);
  return out;
}

// ---------------------------------------------------------------------------
// On-demand closure (resolution-only, for documents outside the workspace
// root - see ensureClosure)
// ---------------------------------------------------------------------------

void WorkspaceIndex::ensureClosure(std::string const &normalizedPath,
                                   FileResolver const &resolver) {
  std::string const start = normalizePath(normalizedPath);
  // Serialize the whole walk: the resolver may stat/read/parse (it never
  // calls back into the index), so it must not run under mu_, and concurrent
  // handlers for the same document share a single expansion.
  std::lock_guard<std::mutex> const walkLock(closureMu_);
  std::set<std::string> expanded;
  std::vector<std::string> front{start};
  while (!front.empty()) {
    std::vector<std::string> next;
    for (std::string const &p : front) {
      if (!expanded.insert(p).second) {
        continue;
      }
      std::shared_ptr<IndexedFile const> entry;
      {
        std::lock_guard<std::mutex> const lk(mu_);
        auto it = files_.find(p);
        if (it != files_.end()) {
          // The workspace scan or open-buffer upsert already owns this file;
          // its include edges stay current through the normal update path.
          entry = it->second;
        }
      }
      if (!entry) {
        bool useCached = false;
        {
          std::lock_guard<std::mutex> const lk(mu_);
          auto it = closureFiles_.find(p);
          if (it != closureFiles_.end()) {
            // The requesting file is a live buffer: re-resolve it every time
            // so unsaved include edits are honored. A cached open-buffer
            // entry (fromDisk=false) is likewise always re-resolved; a
            // closed file is reused while its disk state is unchanged.
            if (p != start && it->second.entry->fromDisk) {
              std::uint64_t m = 0;
              std::uint64_t s = 0;
              statFile(p, &m, &s);
              useCached = it->second.mtime == m && it->second.size == s;
            }
            if (useCached) {
              entry = it->second.entry;
            }
          }
        }
        if (!entry) {
          std::shared_ptr<IndexedFile const> const built = resolver(p);
          std::lock_guard<std::mutex> const lk(mu_);
          if (built != nullptr) {
            closureFiles_[p] = ClosureEntry{built, built->mtime, built->size};
            entry = built;
          } else {
            // The path cannot be served (deleted / unreadable): remember it
            // with no includes so it is never re-probed this session.
            fblang::IndexedFile plain;
            plain.path = p;
            plain.fromDisk = true;
            auto const empty =
                std::make_shared<fblang::IndexedFile const>(std::move(plain));
            closureFiles_[p] = ClosureEntry{empty, 0, 0};
            entry = empty;
          }
        }
      }
      for (IncludeEdge const &e : entry->includes) {
        if (!e.target.empty()) {
          next.push_back(e.target);
        }
      }
    }
    front.swap(next);
  }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

std::filesystem::path WorkspaceIndex::root() const { return root_; }

} // namespace fblang
