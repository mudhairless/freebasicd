#include "index.h"

#include "hash_sha256/hash_sha256.h"

#include <rapidjson/document.h>
#include <rapidjson/rapidjson.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "parser.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <vector>

namespace fblang {

namespace {

constexpr int kIndexVersion = 2;
constexpr auto kFlushDebounce = std::chrono::milliseconds(1500);

// Skip directories that never hold authored sources.
bool isSkippedDir(std::filesystem::path const& name)
{
    std::string const n = toLowerChars(name.string());
    if (!n.empty() && n[0] == '.')
    {
        return true;
    }
    return n == "build" || n == "bin" || n == "obj";
}

bool hasSourceSuffix(std::filesystem::path const& p)
{
    std::string const ext = toLowerChars(p.extension().string());
    return ext == ".bas" || ext == ".bi";
}

bool readTextFile(std::filesystem::path const& p, std::string* out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
    {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

void writeSymbol(rapidjson::Writer<rapidjson::StringBuffer>& w, Symbol const& s)
{
    w.StartObject();
    w.Key("name");
    w.String(s.name.c_str(), static_cast<rapidjson::SizeType>(s.name.size()));
    w.Key("key");
    w.String(s.key.c_str(), static_cast<rapidjson::SizeType>(s.key.size()));
    w.Key("kind");
    w.Uint(static_cast<unsigned>(s.kind));
    w.Key("range");
    w.StartArray();
    w.Uint(s.range.beg);
    w.Uint(s.range.end);
    w.EndArray();
    w.Key("selection");
    w.StartArray();
    w.Uint(s.selection.beg);
    w.Uint(s.selection.end);
    w.EndArray();
    w.Key("signature");
    w.String(s.signature.c_str(), static_cast<rapidjson::SizeType>(s.signature.size()));
    w.Key("doc");
    w.String(s.doc.c_str(), static_cast<rapidjson::SizeType>(s.doc.size()));
    w.Key("children");
    w.StartArray();
    for (Symbol const& c : s.children)
    {
        writeSymbol(w, c);
    }
    w.EndArray();
    w.EndObject();
}

bool readSegment(rapidjson::Value const& v, char const* key, SourceRange* out)
{
    if (!v.HasMember(key) || !v[key].IsArray())
    {
        return false;
    }
    auto const& arr = v[key];
    if (arr.Size() != 2 || !arr[0].IsUint() || !arr[1].IsUint())
    {
        return false;
    }
    out->beg = arr[0].GetUint();
    out->end = arr[1].GetUint();
    return true;
}

bool readSymbol(rapidjson::Value const& v, Symbol* out)
{
    if (!v.IsObject())
    {
        return false;
    }
    if (!v.HasMember("name") || !v["name"].IsString() || !v.HasMember("key") ||
        !v["key"].IsString() || !v.HasMember("kind") || !v["kind"].IsUint())
    {
        return false;
    }
    out->name = v["name"].GetString();
    out->key = v["key"].GetString();
    out->kind = static_cast<SymbolKind>(v["kind"].GetUint());
    if (!readSegment(v, "range", &out->range) || !readSegment(v, "selection", &out->selection))
    {
        return false;
    }
    auto getStr = [&](char const* key) -> std::string {
        if (v.HasMember(key) && v[key].IsString())
        {
            return std::string(v[key].GetString(), v[key].GetStringLength());
        }
        return {};
    };
    out->signature = getStr("signature");
    out->doc = getStr("doc");
    if (v.HasMember("children") && v["children"].IsArray())
    {
        for (auto const& c : v["children"].GetArray())
        {
            Symbol child;
            if (!readSymbol(c, &child))
            {
                return false;
            }
            out->children.push_back(std::move(child));
        }
    }
    return true;
}

// One cache file holds exactly one indexed source document. Its filename is
// the SHA-256 of the normalized source path (see cacheFileFor).
std::string serializeFile(IndexedFile const& f)
{
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("version");
    w.Uint(kIndexVersion);
    w.Key("path");
    w.String(f.path.c_str(), static_cast<rapidjson::SizeType>(f.path.size()));
    w.Key("mtime");
    w.Uint64(f.mtime);
    w.Key("size");
    w.Uint64(f.size);
    w.Key("lang");
    w.String(f.lang.c_str(), static_cast<rapidjson::SizeType>(f.lang.size()));
    w.Key("symbols");
    w.StartArray();
    for (Symbol const& s : f.roots)
    {
        writeSymbol(w, s);
    }
    w.EndArray();
    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize());
}

bool deserializeFile(std::string_view json, IndexedFile* out)
{
    rapidjson::Document doc;
    doc.Parse(json.data(), json.size());
    if (doc.HasParseError() || !doc.IsObject())
    {
        return false;
    }
    if (!doc.HasMember("version") || !doc["version"].IsUint() ||
        doc["version"].GetUint() != kIndexVersion)
    {
        return false;
    }
    if (!doc.HasMember("path") || !doc["path"].IsString() || !doc.HasMember("mtime") ||
        !doc["mtime"].IsUint64() || !doc.HasMember("size") || !doc["size"].IsUint64() ||
        !doc.HasMember("lang") || !doc["lang"].IsString() || !doc.HasMember("symbols") ||
        !doc["symbols"].IsArray())
    {
        return false;
    }
    out->path = doc["path"].GetString();
    out->mtime = doc["mtime"].GetUint64();
    out->size = doc["size"].GetUint64();
    out->lang = doc["lang"].GetString();
    for (auto const& sv : doc["symbols"].GetArray())
    {
        Symbol root;
        if (!readSymbol(sv, &root))
        {
            return false;
        }
        out->roots.push_back(std::move(root));
    }
    return true;
}

// Write `json` to `path` atomically: temp file in the same directory, then
// rename over the target. A crash leaves the old file intact.
bool writeAtomic(std::filesystem::path const& path, std::string const& json)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        return false;
    }
    std::filesystem::path const tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return false;
        }
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        out.flush();
        if (!out)
        {
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace

std::string sha256Hex(std::string_view data)
{
    hash_sha256 hasher;
    hasher.sha256_init();
    std::vector<std::uint8_t> bytes(data.begin(), data.end());
    hasher.sha256_update(bytes.data(), bytes.size());
    sha256_type const digest = hasher.sha256_final();
    static constexpr char const kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (std::uint8_t const byte : digest)
    {
        out.push_back(kHex[(byte >> 4U) & 0x0FU]);
        out.push_back(kHex[byte & 0x0FU]);
    }
    return out;
}

std::string normalizePath(std::filesystem::path const& path)
{
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    if (ec)
    {
        abs = path;
    }
    std::string s = abs.lexically_normal().string();
#ifdef _WIN32
    for (char& c : s)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
#endif
    return s;
}

// Cache subdirectory and cache filename both key off SHA-256: the workspace
// root chooses the subdirectory (workspace isolation), each source path its
// own cache file (direct lookup, no scan to find a document's entry).
std::string workspaceKey(std::string const& normalizedRoot)
{
    return sha256Hex(normalizedRoot);
}

std::filesystem::path defaultCacheDir()
{
#ifdef _WIN32
    if (char const* d = std::getenv("LOCALAPPDATA"))
    {
        return std::filesystem::path(d) / "freebasiclsp" / "index";
    }
#elif defined(__APPLE__)
    if (char const* h = std::getenv("HOME"))
    {
        return std::filesystem::path(h) / "Library" / "Application Support" / "freebasiclsp" / "index";
    }
#else
    if (char const* xdg = std::getenv("XDG_STATE_HOME"))
    {
        if (*xdg)
        {
            return std::filesystem::path(xdg) / "freebasiclsp" / "index";
        }
    }
    if (char const* h = std::getenv("HOME"))
    {
        return std::filesystem::path(h) / ".local" / "state" / "freebasiclsp" / "index";
    }
#endif
    std::error_code ec;
    std::filesystem::path const tmp = std::filesystem::temp_directory_path(ec);
    if (!ec)
    {
        return tmp / "freebasiclsp" / "index";
    }
    return std::filesystem::path(".");
}

std::filesystem::path cacheFileFor(std::filesystem::path const& dir, std::string const& normalizedPath)
{
    return dir / (sha256Hex(normalizedPath) + ".json");
}

bool statFile(std::filesystem::path const& path, std::uint64_t* mtime, std::uint64_t* size)
{
    std::error_code ec;
    std::filesystem::file_status const st = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::is_regular_file(st))
    {
        return false;
    }
    auto lm = std::filesystem::last_write_time(path, ec);
    if (ec)
    {
        return false;
    }
    if (mtime)
    {
        *mtime = static_cast<std::uint64_t>(lm.time_since_epoch().count());
    }
    if (size)
    {
        *size = std::filesystem::file_size(path, ec);
        if (ec)
        {
            return false;
        }
    }
    return true;
}

WorkspaceIndex::WorkspaceIndex(std::filesystem::path const& root, std::filesystem::path const& cacheDir)
    : root_(std::filesystem::absolute(root).lexically_normal())
{
    cacheDir_ = cacheDir.empty() ? defaultCacheDir() : cacheDir;
    indexDir_ = cacheDir_ / workspaceKey(normalizePath(root_));
}

WorkspaceIndex::~WorkspaceIndex()
{
    close();
}

void WorkspaceIndex::open()
{
    if (running_.exchange(true))
    {
        return;
    }
    loadFromDisk();
    flusher_ = std::thread([this] { flusherLoop(); });
}

void WorkspaceIndex::close()
{
    running_.store(false);
    {
        std::lock_guard<std::mutex> const lk(cvMu_);
        dirty_ = true;
    }
    cv_.notify_all();
    if (flusher_.joinable())
    {
        flusher_.join();
    }
    if (scanner_.joinable())
    {
        scanner_.join();
    }
    flushNow();
}

void WorkspaceIndex::scan(bool async)
{
    if (async)
    {
        if (scanner_.joinable())
        {
            scanner_.join();
        }
        scanner_ = std::thread([this] {
            scan(false);
            flushSoon();
        });
        return;
    }

    std::error_code ec;
    std::set<std::string> seen;
    std::filesystem::recursive_directory_iterator it(root_, std::filesystem::directory_options::skip_permission_denied,
                                                    ec);
    std::filesystem::recursive_directory_iterator const end;
    if (ec)
    {
        return;
    }

    bool changed = false;
    for (; it != end; it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        std::filesystem::directory_entry const entry = *it;
        std::filesystem::file_status const st = entry.status(ec);
        if (ec)
        {
            continue;
        }
        if (std::filesystem::is_directory(st) && isSkippedDir(entry.path().filename()))
        {
            it.disable_recursion_pending();
            continue;
        }
        if (std::filesystem::is_directory(st))
        {
            continue;
        }
        if (!hasSourceSuffix(entry.path()))
        {
            continue;
        }
        std::string const norm = normalizePath(entry.path());
        seen.insert(norm);

        std::uint64_t mtime = 0;
        std::uint64_t size = 0;
        if (!statFile(entry.path(), &mtime, &size))
        {
            continue;
        }
        {
            std::lock_guard<std::mutex> const lk(mu_);
            auto found = files_.find(norm);
            if (found != files_.end() && found->second->mtime == mtime &&
                found->second->size == size)
            {
                continue;
            }
        }

        std::string content;
        if (!readTextFile(entry, &content))
        {
            continue;
        }
        ParseResult parse = parseDocument(content);
        IndexedFile f;
        f.path = norm;
        f.mtime = mtime;
        f.size = size;
        f.lang = parse.lang;
        f.roots = std::move(parse.roots);
        upsert(std::move(f));
        changed = true;
    }

    {
        std::lock_guard<std::mutex> const lk(mu_);
        for (auto itm = files_.begin(); itm != files_.end();)
        {
            std::filesystem::path const p(itm->second->path);
            if (!std::filesystem::exists(p, ec) || seen.count(itm->second->path) == 0)
            {
                removeCacheFile(itm->second->path);
                itm = files_.erase(itm);
            }
            else
            {
                ++itm;
            }
        }
    }
    if (changed)
    {
        flushSoon();
    }
}

void WorkspaceIndex::upsert(IndexedFile entry)
{
    entry.path = normalizePath(entry.path);
    // Capture the key before the move: C++17 sequences the right operand of
    // `operator=` first, so moving `entry` into the shared_ptr must not race
    // the subscript's key evaluation (which would leave an empty key).
    std::string const key = entry.path;
    std::lock_guard<std::mutex> const lk(mu_);
    files_[key] = std::make_shared<IndexedFile const>(std::move(entry));
}

void WorkspaceIndex::remove(std::string const& path)
{
    std::string const norm = normalizePath(path);
    bool erased = false;
    {
        std::lock_guard<std::mutex> const lk(mu_);
        erased = files_.erase(norm) > 0;
    }
    if (erased)
    {
        removeCacheFile(norm);
    }
}

std::vector<std::shared_ptr<IndexedFile const>> WorkspaceIndex::snapshot() const
{
    std::lock_guard<std::mutex> const lk(mu_);
    std::vector<std::shared_ptr<IndexedFile const>> out;
    out.reserve(files_.size());
    for (auto const& kv : files_)
    {
        out.push_back(kv.second);
    }
    return out;
}

std::size_t WorkspaceIndex::size() const
{
    std::lock_guard<std::mutex> const lk(mu_);
    return files_.size();
}

std::filesystem::path WorkspaceIndex::root() const
{
    return root_;
}

std::filesystem::path WorkspaceIndex::cacheDir() const
{
    return cacheDir_;
}

std::filesystem::path WorkspaceIndex::indexDir() const
{
    return indexDir_;
}

std::filesystem::path WorkspaceIndex::cachePathFor(std::string const& normalizedPath) const
{
    return cacheFileFor(indexDir_, normalizedPath);
}

bool WorkspaceIndex::loadFromDisk()
{
    std::error_code ec;
    std::vector<IndexedFile> files;
    std::filesystem::directory_iterator const end;
    for (std::filesystem::directory_iterator it(indexDir_, ec); it != end; it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        std::filesystem::path const p = it->path();
        std::error_code sec;
        std::filesystem::file_status const s = it->status(sec);
        if (sec || !std::filesystem::is_regular_file(s) || p.extension().string() != ".json")
        {
            continue;
        }
        std::string json;
        if (!readTextFile(p, &json))
        {
            continue;
        }
        IndexedFile f;
        if (!deserializeFile(json, &f))
        {
            continue;
        }
        std::string const norm = normalizePath(f.path);
        // The cache filename must be the SHA-256 of the stored path; anything
        // else is a misplaced or corrupt entry and is not trusted.
        if (p.stem().string() != sha256Hex(norm))
        {
            continue;
        }
        f.path = norm;
        files.push_back(std::move(f));
    }
    {
        std::lock_guard<std::mutex> const lk(mu_);
        for (IndexedFile& f : files)
        {
            std::string const key = f.path;  // capture before the move (see upsert)
            files_[key] = std::make_shared<IndexedFile const>(std::move(f));
        }
    }
    return !files.empty();
}

void WorkspaceIndex::flushSoon()
{
    {
        std::lock_guard<std::mutex> const lk(cvMu_);
        dirty_ = true;
    }
    cv_.notify_all();
}

void WorkspaceIndex::removeCacheFile(std::string const& normalizedPath)
{
    std::error_code ec;
    std::filesystem::remove(cacheFileFor(indexDir_, normalizedPath), ec);
}

void WorkspaceIndex::flusherLoop()
{
    std::unique_lock<std::mutex> lk(cvMu_);
    while (running_.load())
    {
        cv_.wait_for(lk, kFlushDebounce, [this] { return dirty_ || !running_.load(); });
        if (dirty_)
        {
            dirty_ = false;
            lk.unlock();
            flushNow();
            lk.lock();
        }
    }
}

void WorkspaceIndex::flushNow()
{
    std::vector<IndexedFile> files;
    {
        std::lock_guard<std::mutex> const lk(mu_);
        files.reserve(files_.size());
        for (auto const& kv : files_)
        {
            files.push_back(*kv.second);
        }
    }
    // Persist only entries that still match their last parsed disk state; a
    // live buffer that diverged from disk must never be written as if it were
    // the on-disk file, or the next boot would treat buffer symbols as disk
    // truth after a matching mtime/size check.
    for (IndexedFile const& f : files)
    {
        std::uint64_t mtime = 0;
        std::uint64_t size = 0;
        if (statFile(f.path, &mtime, &size) && mtime == f.mtime && size == f.size)
        {
            writeAtomic(cacheFileFor(indexDir_, f.path), serializeFile(f));
        }
    }
}

}  // namespace fblang