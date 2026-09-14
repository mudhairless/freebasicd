#include "index.h"

#include <rapidjson/document.h>
#include <rapidjson/rapidjson.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "parser.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>

namespace fblang {

namespace {

constexpr int kIndexVersion = 1;
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
    auto getStr = [&](char const* key) -> std::string const {
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

std::string serializeFiles(std::string const& workspace, std::vector<IndexedFile> const& files)
{
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("version");
    w.Uint(kIndexVersion);
    w.Key("workspace");
    w.String(workspace.c_str(), static_cast<rapidjson::SizeType>(workspace.size()));
    w.Key("files");
    w.StartArray();
    for (IndexedFile const& f : files)
    {
        w.StartObject();
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
    }
    w.EndArray();
    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize());
}

bool deserializeFiles(std::string_view json, std::string const& expectedWorkspace,
                      std::vector<IndexedFile>* out)
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
    if (!doc.HasMember("workspace") || !doc["workspace"].IsString() ||
        doc["workspace"].GetString() != expectedWorkspace)
    {
        return false;
    }
    if (!doc.HasMember("files") || !doc["files"].IsArray())
    {
        return false;
    }
    for (auto const& fv : doc["files"].GetArray())
    {
        if (!fv.IsObject() || !fv.HasMember("path") || !fv["path"].IsString() ||
            !fv.HasMember("mtime") || !fv["mtime"].IsUint64() || !fv.HasMember("size") ||
            !fv["size"].IsUint64() || !fv.HasMember("lang") || !fv["lang"].IsString() ||
            !fv.HasMember("symbols") || !fv["symbols"].IsArray())
        {
            return false;
        }
        IndexedFile f;
        f.path = fv["path"].GetString();
        f.mtime = fv["mtime"].GetUint64();
        f.size = fv["size"].GetUint64();
        f.lang = fv["lang"].GetString();
        for (auto const& sv : fv["symbols"].GetArray())
        {
            Symbol root;
            if (!readSymbol(sv, &root))
            {
                return false;
            }
            f.roots.push_back(std::move(root));
        }
        out->push_back(std::move(f));
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

std::string normalizePath(std::filesystem::path path)
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

std::string workspaceKey(std::string const& normalizedRoot)
{
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : normalizedRoot)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::filesystem::path defaultCacheDir()
{
#if defined(_WIN32)
    if (char const* d = std::getenv("LOCALAPPDATA"))
    {
        return std::filesystem::path(d) / "fb-lsp";
    }
#elif defined(__APPLE__)
    if (char const* h = std::getenv("HOME"))
    {
        return std::filesystem::path(h) / "Library" / "Application Support" / "fb-lsp";
    }
#else
    if (char const* xdg = std::getenv("XDG_DATA_HOME"))
    {
        if (*xdg)
        {
            return std::filesystem::path(xdg) / "fb-lsp";
        }
    }
    if (char const* h = std::getenv("HOME"))
    {
        return std::filesystem::path(h) / ".local" / "share" / "fb-lsp";
    }
#endif
    std::error_code ec;
    std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
    if (!ec)
    {
        return tmp / "fb-lsp";
    }
    return std::filesystem::path(".");
}

bool statFile(std::filesystem::path const& path, std::uint64_t* mtime, std::uint64_t* size)
{
    std::error_code ec;
    std::filesystem::file_status st = std::filesystem::status(path, ec);
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

WorkspaceIndex::WorkspaceIndex(std::filesystem::path root, std::filesystem::path cacheDir)
    : root_(std::filesystem::absolute(root).lexically_normal())
{
    if (cacheDir.empty())
    {
        cacheDir_ = defaultCacheDir() / workspaceKey(normalizePath(root_));
    }
    else
    {
        cacheDir_ = cacheDir / workspaceKey(normalizePath(root_));
    }
    indexFile_ = cacheDir_ / "index.json";
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
        std::lock_guard<std::mutex> lk(cvMu_);
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
    std::filesystem::recursive_directory_iterator end;
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
        std::filesystem::file_status st = entry.status(ec);
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

        std::uint64_t mtime = 0, size = 0;
        if (!statFile(entry.path(), &mtime, &size))
        {
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
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
        std::lock_guard<std::mutex> lk(mu_);
        for (auto itm = files_.begin(); itm != files_.end();)
        {
            std::filesystem::path p(itm->second->path);
            if (!std::filesystem::exists(p, ec) || seen.count(itm->second->path) == 0)
            {
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
    std::lock_guard<std::mutex> lk(mu_);
    files_[key] = std::make_shared<IndexedFile const>(std::move(entry));
}

void WorkspaceIndex::remove(std::string const& path)
{
    std::string const norm = normalizePath(path);
    std::lock_guard<std::mutex> lk(mu_);
    files_.erase(norm);
}

std::vector<std::shared_ptr<IndexedFile const>> WorkspaceIndex::snapshot() const
{
    std::lock_guard<std::mutex> lk(mu_);
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
    std::lock_guard<std::mutex> lk(mu_);
    return files_.size();
}

std::filesystem::path WorkspaceIndex::root() const
{
    return root_;
}

std::filesystem::path WorkspaceIndex::indexFile() const
{
    return indexFile_;
}

bool WorkspaceIndex::loadFromDisk()
{
    std::string json;
    if (!readTextFile(indexFile_, &json))
    {
        return false;
    }
    std::vector<IndexedFile> files;
    if (!deserializeFiles(json, normalizePath(root_), &files))
    {
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    for (IndexedFile& f : files)
    {
        f.path = normalizePath(f.path);
        std::string const key = f.path;  // capture before the move (see upsert)
        files_[key] = std::make_shared<IndexedFile const>(std::move(f));
    }
    return true;
}

void WorkspaceIndex::flushSoon()
{
    {
        std::lock_guard<std::mutex> lk(cvMu_);
        dirty_ = true;
    }
    cv_.notify_all();
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
        std::lock_guard<std::mutex> lk(mu_);
        files.reserve(files_.size());
        for (auto const& kv : files_)
        {
            files.push_back(*kv.second);
        }
    }
    if (files.empty())
    {
        return;
    }
    // Persist only entries that still match their last parsed disk state; a
    // live buffer that diverged from disk must never be written as if it were
    // the on-disk file, or the next boot would treat buffer symbols as disk
    // truth after a matching mtime/size check.
    std::vector<IndexedFile> valid;
    valid.reserve(files.size());
    for (IndexedFile const& f : files)
    {
        std::uint64_t mtime = 0, size = 0;
        if (statFile(f.path, &mtime, &size) && mtime == f.mtime && size == f.size)
        {
            valid.push_back(f);
        }
    }
    if (valid.empty())
    {
        return;
    }
    writeAtomic(indexFile_, serializeFiles(normalizePath(root_), valid));
}

}  // namespace fblang