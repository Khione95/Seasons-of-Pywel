#include "archive.h"
#include "climate.h"
#include "gpu.h"
#include "log.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"

const char* const SEASON_NAMES[SEASON_COUNT] = { "spring", "summer", "autumn", "winter" };

// ---------------------------------------------------------------------------
// The files
//
// <group>/0.pamt: u32, u32 number of .paz files, u32, 12 bytes per .paz; then
// the folder name block, the file name block (u32 size + nodes: u32 parent,
// u8 length, characters - a name is its chain of nodes), the folders (u32
// count; hash, name, first file, file count) and the files (u32 count; name,
// offset, packed size, size, u16 .paz, u16 flags). The game keeps the file as
// it reads it and looks files up in it.
//
// meta/0.pathc: u32 x2, u32 header size, u32 header count, u32 file count,
// u32 x2; the DDS headers; the files' keys (hashlittle("/<path>", 0xC5EDE),
// sorted) and one 20-byte record per key (header number, the stored sizes of
// the first four mips) - the game needs the record that goes with the data.
//
// At the start the plugin reads these files itself and notes, for every file
// a season has a copy of, where its entries are (file and offset) and what they
// hold for each season. Whatever way the game then reads them, the bytes of
// those entries are set to the season's before the game sees them, and where
// they land in memory is kept, so a season can be changed later.
// ---------------------------------------------------------------------------

struct Record
{
    uint64_t offset;            // in the file
    int size;
    bool climate;               // the climate texture's: follows For()
    uint8_t value[SEASON_COUNT][20];     // what the entry holds in each season (summer: the original)
    uint8_t* live[4];           // where the game has it (it may read a file more than once)
    int lives;
};

static Season For(const Record& r, Season s)
{
    if (r.climate)
        return SUMMER;      // the game's own file (its packed copy is read wrongly by the texture loader)

    return s;
}

struct IndexFile
{
    std::wstring path;          // lower case
    std::vector<Record> records;    // by offset
    // The game keeps its own copy of the file list (20 bytes per file): found
    // by bytes of it no season changes, its entries are changed too.
    uint64_t listAt = 0, listEnd = 0;   // the file list in the file
    uint64_t signatureAt = 0;           // the signature's place in the file
    uint8_t signature[48] = {};
    bool located = false;               // the game's copy was looked for
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::vector<IndexFile> g_files;
static volatile LONG g_season = SUMMER;

// The climate texture: the game always gets its own; the seasons change it on
// the graphics card (gpu.cpp) and in the game's climate maps (climate.cpp).
struct Record;
static Season For(const Record& r, Season s);
static int g_count[SEASON_COUNT];
static std::vector<std::pair<uint32_t, uint32_t>> g_hashes;     // index file hashes: as on disk, as read

static uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static uint32_t HashLittle(const char* data, size_t n, uint32_t init)
{
    uint32_t a, b, c;
    a = b = c = 0xDEADBEEF + (uint32_t)n + init;
    const uint8_t* k = (const uint8_t*)data;

    while (n > 12)
    {
        uint32_t w[3];
        memcpy(w, k, 12);
        a += w[0]; b += w[1]; c += w[2];
        a -= c; a ^= Rol(c, 4); c += b;
        b -= a; b ^= Rol(a, 6); a += c;
        c -= b; c ^= Rol(b, 8); b += a;
        a -= c; a ^= Rol(c, 16); c += b;
        b -= a; b ^= Rol(a, 19); a += c;
        c -= b; c ^= Rol(b, 4); b += a;
        k += 12; n -= 12;
    }

    if (n == 0)
        return c;

    uint8_t tail[12] = { 0 };
    memcpy(tail, k, n);
    uint32_t w[3];
    memcpy(w, tail, 12);
    c += w[2]; b += w[1]; a += w[0];
    c ^= b; c -= Rol(b, 14);
    a ^= c; a -= Rol(c, 11);
    b ^= a; b -= Rol(a, 25);
    c ^= b; c -= Rol(b, 16);
    a ^= c; a -= Rol(c, 4);
    b ^= a; b -= Rol(a, 14);
    c ^= b; c -= Rol(b, 24);
    return c;
}

static bool SafeCopy(void* to, const void* from, size_t n)
{
    __try
    {
        memcpy(to, from, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static uint32_t U32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

static std::wstring Lower(std::wstring s)
{
    for (wchar_t& c : s)
        c = towlower(c == '/' ? '\\' : c);
    return s;
}

static bool ReadWhole(const std::wstring& path, std::vector<uint8_t>* out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    out->resize((size_t)size.QuadPart);
    DWORD got = 0;
    bool ok = ReadFile(h, out->data(), (DWORD)out->size(), &got, NULL) && got == out->size();
    CloseHandle(h);
    return ok;
}

// A name out of a name block: its chain of nodes, root first.
static std::string NodePath(const uint8_t* block, size_t size, uint32_t off)
{
    std::vector<std::pair<const char*, uint8_t>> parts;

    while (off != 0xFFFFFFFF && off + 5 <= size && parts.size() < 256)
    {
        uint8_t n = block[off + 4];

        if (off + 5 + n > size)
            break;

        parts.emplace_back((const char*)block + off + 5, n);
        off = U32(block + off);
    }

    std::string s;

    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
        s.append(it->first, it->second);

    return s;
}

// The last node of a name (the end of it), cheap to look at.
static bool LeafMentionsSeason(const uint8_t* block, size_t size, uint32_t off)
{
    if (off + 5 > size)
        return false;

    uint8_t n = block[off + 4];

    if (off + 5 + n > size)
        return false;

    std::string leaf((const char*)block + off + 5, n);

    for (int s = 0; s < SEASON_COUNT; ++s)
        if (s != SUMMER && leaf.find(SEASON_NAMES[s]) != std::string::npos)
            return true;

    return false;
}

// ---------------------------------------------------------------------------
// The season data (Seasons\data): <group>.dat holds every season's files of
// that archive group, packed as the game packs its own; seasons.idx lists
// them (season|group|path|offset|stored size|size|flags|4 stored mip sizes).
//
// The game reads an archive (.paz) only up to 2 GB, and a file's index entry
// can only point into its own group's archives. So each group gets a region
// past the end of one of its archives, still below 2 GB: the season's entries
// point there, and reads there are served from <group>.dat.
// ---------------------------------------------------------------------------

struct SeasonFile
{
    Season which;
    std::string path;           // in the group, "tree/texture/x.dds"
    uint64_t offset;            // in <group>.dat
    uint32_t stored, size, flags;
    uint32_t sizes[4];
};

struct Region
{
    std::string group;          // "0001"
    std::wstring dat;           // its data file
    uint64_t datSize;
    uint32_t paz;               // the archive the region is past the end of
    uint64_t pazEnd, base;      // where that archive ends, where the region starts
    uint64_t datStart, datEnd;  // the part of the data file it holds
    uint64_t fakeSize;          // the archive's size as the game is told it: up to the region's end
    HANDLE handle;              // the data file, opened when first read
    SRWLOCK lock;
};

static std::unordered_map<std::string, std::vector<SeasonFile>> g_data;   // by group
static std::wstring g_dataFolder;

static std::vector<Region*> g_regions;

struct Pair
{
    std::string path;           // "/folder/name" as the size table keys it
    Season which;
    uint32_t sizes[4];
};

// The record at offset; a new one holds the original in every season.
static Record* RecordAt(IndexFile& f, uint64_t offset, int size, const uint8_t* original)
{
    for (Record& r : f.records)
        if (r.offset == offset)
            return &r;

    Record r = {};
    r.offset = offset;
    r.size = size;

    for (int s = 0; s < SEASON_COUNT; ++s)
        memcpy(r.value[s], original, size);

    f.records.push_back(r);
    return &f.records.back();
}

static void LoadSeasonData(const std::wstring& folder)
{
    FILE* f = NULL;

    if (_wfopen_s(&f, (folder + L"\\seasons.idx").c_str(), L"r") != 0 || !f)
    {
        Log("season data: %S\\seasons.idx not found", folder.c_str());
        return;
    }

    char line[1024];
    int n = 0;

    while (fgets(line, sizeof(line), f))
    {
        char season[16], group[16], path[512];
        unsigned long long offset;
        unsigned stored, size, flags, s0, s1, s2, s3;

        if (sscanf_s(line, "%15[^|]|%15[^|]|%511[^|]|%llu|%u|%u|%u|%u|%u|%u|%u", season, (unsigned)sizeof(season),
                     group, (unsigned)sizeof(group), path, (unsigned)sizeof(path), &offset, &stored, &size, &flags,
                     &s0, &s1, &s2, &s3) != 11)
            continue;

        SeasonFile sf = {};
        sf.which = SEASON_COUNT;

        for (int i = 0; i < SEASON_COUNT; ++i)
            if (_stricmp(season, SEASON_NAMES[i]) == 0)
                sf.which = (Season)i;

        // Summer lines too: the game's own textures, packed with the same mip
        // sizes as the other seasons (a season change must not change them).
        if (sf.which == SEASON_COUNT)
            continue;

        sf.path = path;
        sf.offset = offset;
        sf.stored = stored;
        sf.size = size;
        sf.flags = flags;
        sf.sizes[0] = s0; sf.sizes[1] = s1; sf.sizes[2] = s2; sf.sizes[3] = s3;
        g_data[group].push_back(sf);
        ++g_count[sf.which];
        ++n;
    }

    fclose(f);
    Log("season data: %d files", n);
}

// One group's index: the entries of the files the seasons change.
static void ScanPamt(const std::wstring& path, const std::string& group, const std::wstring& dataFolder,
                     std::vector<Pair>* pairs)
{
    auto data = g_data.find(group);

    if (data == g_data.end())
        return;

    std::vector<uint8_t> file;

    if (!ReadWhole(path, &file) || file.size() < 16)
        return;

    const uint8_t* d = file.data();
    size_t size = file.size();
    uint32_t pazCount = U32(d + 4);
    size_t p = 12 + 12 * (size_t)pazCount;
    const uint8_t *dirs = NULL, *names = NULL;
    size_t dirsSize = 0, namesSize = 0;

    auto block = [&](const uint8_t** at, size_t* n) -> bool
    {
        if (p + 4 > size) return false;
        *n = U32(d + p);
        if (p + 4 + *n > size) return false;
        *at = d + p + 4;
        p += 4 + *n;
        return true;
    };

    if (!block(&dirs, &dirsSize) || !block(&names, &namesSize) || p + 4 > size)
        return;

    uint32_t folderCount = U32(d + p);
    const uint8_t* folders = d + p + 4;
    p += 4 + 16 * (size_t)folderCount;

    if (p + 4 > size)
        return;

    uint32_t fileCount = U32(d + p);
    size_t filesAt = p + 4;

    if (filesAt + 20 * (size_t)fileCount > size)
        return;

    // The regions: past the end of each of the group's archives (the archive
    // table: index, checksum, size), aligned, up to 2 GB; the data file is
    // spread over them in order.
    std::wstring dat = dataFolder + L"\\";

    for (char c : group)
        dat += (wchar_t)c;

    dat += L".dat";
    WIN32_FILE_ATTRIBUTE_DATA fa;

    if (!GetFileAttributesExW(dat.c_str(), GetFileExInfoStandard, &fa))
    {
        Log("season data: %S missing", dat.c_str());
        return;
    }

    uint64_t datSize = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    std::vector<Region*> regions;

    for (uint32_t i = 0; i < pazCount; ++i)
    {
        Region* r = new Region();
        r->group = group;
        r->dat = dat;
        r->datSize = datSize;
        r->lock = SRWLOCK_INIT;
        r->paz = U32(d + 12 + 12 * i);
        r->pazEnd = U32(d + 12 + 12 * i + 8);
        r->base = (r->pazEnd + (64ull << 20) + (16ull << 20) - 1) & ~((16ull << 20) - 1);

        if (r->base < 0x7FFF0000ull)
            regions.push_back(r);
        else
            delete r;
    }

    // Each texture's versions (back to back in the data file) into one region,
    // the first with room for all of them: a season change then changes only
    // an entry's offset - one 4-byte value, never the archive as well (the
    // game reads entries while they change).
    std::vector<const SeasonFile*> byOffset;

    for (const SeasonFile& sf : data->second)
        byOffset.push_back(&sf);

    std::sort(byOffset.begin(), byOffset.end(), [](const SeasonFile* a, const SeasonFile* b) { return a->offset < b->offset; });
    std::unordered_map<const SeasonFile*, Region*> regionOf;
    size_t k = 0;

    if (!regions.empty())
        regions[0]->datStart = regions[0]->datEnd = byOffset.empty() ? 0 : byOffset[0]->offset;

    for (size_t i = 0; i < byOffset.size();)
    {
        // The versions of this texture: the run of files with its path.
        size_t j = i + 1;

        while (j < byOffset.size() && byOffset[j]->path == byOffset[i]->path)
            ++j;

        uint64_t from = byOffset[i]->offset, to = byOffset[j - 1]->offset + byOffset[j - 1]->stored;

        while (k < regions.size() && regions[k]->base + (to - regions[k]->datStart) >= 0x7FFF0000ull)
        {
            if (++k < regions.size())
                regions[k]->datStart = regions[k]->datEnd = from;
        }

        if (k >= regions.size())
            break;

        regions[k]->datEnd = to;

        for (size_t v = i; v < j; ++v)
            regionOf[byOffset[v]] = regions[k];

        i = j;
    }

    if (regionOf.size() < byOffset.size())
        Log("season data %s: %zu of %zu files have no room below 2 GB in the group's archives", group.c_str(),
            byOffset.size() - regionOf.size(), byOffset.size());

    // "folder/name" -> its season files, and the folders they are in.
    std::unordered_map<std::string, std::vector<const SeasonFile*>> wanted;

    for (const SeasonFile& sf : data->second)
        wanted[sf.path].push_back(&sf);

    std::unordered_map<std::string, bool> wantedFolders;

    for (auto& w : wanted)
        wantedFolders[w.first.substr(0, w.first.rfind('/'))] = true;

    IndexFile out;
    out.path = Lower(path);
    out.records.reserve(wanted.size() + 2);
    int found = 0;

    for (uint32_t fo = 0; fo < folderCount; ++fo)
    {
        uint32_t first = U32(folders + 16 * fo + 8), count = U32(folders + 16 * fo + 12);

        if ((size_t)first + count > fileCount)
            continue;

        std::string folder = NodePath(dirs, dirsSize, U32(folders + 16 * fo + 4));

        for (char& c : folder)
            if (c == '\\')
                c = '/';

        while (!folder.empty() && folder.front() == '/') folder.erase(0, 1);
        while (!folder.empty() && folder.back() == '/') folder.pop_back();

        if (!wantedFolders.count(folder))
            continue;

        for (uint32_t i = first; i < first + count; ++i)
        {
            std::string full = folder + "/" + NodePath(names, namesSize, U32(d + filesAt + 20 * i));
            auto it = wanted.find(full);

            if (it == wanted.end())
                continue;

            // The entry, less its name: offset, stored size, size, archive, flags.
            uint64_t at = filesAt + 20 * (uint64_t)i + 4;
            Record* r = RecordAt(out, at, 16, d + at);
            r->climate = full == "texture/climate_texture_1.dds";

            // Each season's version; a season without one (winter for the
            // plants) takes the summer copy: never the game's own file, whose
            // mip sizes differ from the ones the game keeps for the texture.
            const SeasonFile* bySeason[SEASON_COUNT] = {};

            for (const SeasonFile* sf : it->second)
                if (regionOf.count(sf))
                    bySeason[sf->which] = sf;

            for (int sn = 0; sn < SEASON_COUNT; ++sn)
            {
                const SeasonFile* sf = bySeason[sn] ? bySeason[sn] : bySeason[SUMMER];

                if (!sf)
                    continue;

                // The climate texture in summer: the game's own file, read as
                // the game reads it (its versions need no common sizes). The
                // game's texture loader decodes the packed copy wrongly from
                // about 100 KB on: snow in summer, in patches.
                if (r->climate && sn == SUMMER)
                    continue;

                Region* region = regionOf[sf];
                uint32_t v[4] = { (uint32_t)(region->base + (sf->offset - region->datStart)), sf->stored, sf->size,
                                  (region->paz & 0xFFFF) | (sf->flags << 16) };

                memcpy(r->value[sn], v, 16);
                Pair pr = { "/" + full, (Season)sn, { sf->sizes[0], sf->sizes[1], sf->sizes[2], sf->sizes[3] } };
                pairs->push_back(pr);
            }

            ++found;
        }
    }

    for (Region* r : regions)
        if (r->datEnd > r->datStart)
        {
            // The archive table gives each archive's size: the game is told
            // the archive reaches to the end of the region (in every season).
            r->fakeSize = (r->base + (r->datEnd - r->datStart) + 0xFFFF) & ~0xFFFFull;

            for (uint32_t i = 0; i < pazCount; ++i)
                if (U32(d + 12 + 12 * i) == r->paz)
                {
                    uint64_t at = 12 + 12 * (uint64_t)i + 8;
                    Record* rec = RecordAt(out, at, 4, d + at);
                    uint32_t size = (uint32_t)r->fakeSize;

                    for (int k = 0; k < SEASON_COUNT; ++k)
                        memcpy(rec->value[k], &size, 4);
                }

            g_regions.push_back(r);
            Log("season data %s: %llu MB past archive %u (ends %llu, region at %llu)", group.c_str(),
                (r->datEnd - r->datStart) >> 20, r->paz, r->pazEnd, r->base);
        }

    if (out.records.empty())
        return;

    // The file starts with hashlittle(file from byte 12, 0xC5EDE), which the
    // game checks (and meta/0.papgt keeps a copy of): the hash of the file as
    // the game will read it, with the start season's entries.
    std::vector<uint8_t> patched = file;

    for (const Record& r : out.records)
        memcpy(patched.data() + r.offset, r.value[For(r, (Season)g_season)], r.size);

    uint32_t before = U32(d), after = HashLittle((const char*)patched.data() + 12, patched.size() - 12, 0xC5EDE);
    uint8_t v[4];
    memcpy(v, &after, 4);
    RecordAt(out, 0, 4, v);
    g_hashes.push_back({ before, after });
    Log("index %S: %d of %zu season paths found; hash %08X -> %08X", path.c_str(), found, wanted.size(), before, after);
    out.listAt = filesAt;
    out.listEnd = filesAt + 20 * (uint64_t)fileCount;

    // The first 48 bytes of the list (at an entry) no season record lies in.
    for (uint64_t w = out.listAt; w + sizeof(out.signature) <= out.listEnd; w += 20)
    {
        bool clear = true;

        for (const Record& r : out.records)
            clear = clear && (r.offset + r.size <= w || r.offset >= w + sizeof(out.signature));

        if (clear)
        {
            out.signatureAt = w;
            memcpy(out.signature, d + w, sizeof(out.signature));
            break;
        }
    }

    g_files.push_back(std::move(out));
}

// meta/0.papgt: u32, u32 hashlittle(file from byte 12, 0xC5EDE), u32, then the
// groups (flags, name, the hash of their 0.pamt). The groups whose index the
// season changes get their new hash, and the file its own.
static void ScanPapgt(const std::wstring& path)
{
    std::vector<uint8_t> file;

    if (g_hashes.empty() || !ReadWhole(path, &file) || file.size() < 16)
        return;

    IndexFile out;
    out.path = Lower(path);
    std::vector<uint8_t> patched = file;
    int found = 0;

    for (const auto& [before, after] : g_hashes)
        for (size_t at = 12; at + 4 <= file.size(); at += 4)
            if (U32(file.data() + at) == before)
            {
                uint8_t v[4];
                memcpy(v, &after, 4);
                RecordAt(out, at, 4, v);
                memcpy(patched.data() + at, v, 4);
                ++found;
                break;
            }

    uint32_t self = HashLittle((const char*)patched.data() + 12, patched.size() - 12, 0xC5EDE);
    uint8_t v[4];
    memcpy(v, &self, 4);
    RecordAt(out, 4, 4, v);
    std::sort(out.records.begin(), out.records.end(), [](const Record& a, const Record& b) { return a.offset < b.offset; });
    Log("group list: %d of %zu index hashes changed (own hash %08X -> %08X)", found, g_hashes.size(), U32(file.data() + 4), self);
    g_files.push_back(std::move(out));
}

static void ScanPathc(const std::wstring& path, const std::vector<Pair>& pairs)
{
    std::vector<uint8_t> file;

    if (!ReadWhole(path, &file) || file.size() < 28)
    {
        Log("size table %S: not read", path.c_str());
        return;
    }

    const uint8_t* d = file.data();
    uint32_t headerSize = U32(d + 8), headers = U32(d + 12), count = U32(d + 16);
    size_t keys = 28 + (size_t)headers * headerSize;
    size_t records = keys + (size_t)count * 4;

    if (records + (size_t)count * 20 > file.size())
    {
        Log("size table: unexpected layout");
        return;
    }

    auto find = [&](const std::string& p) -> int64_t
    {
        uint32_t key = HashLittle(p.c_str(), p.size(), 0xC5EDE);
        const uint32_t* k = (const uint32_t*)(d + keys);
        const uint32_t* e = std::lower_bound(k, k + count, key);
        return (e != k + count && *e == key) ? (e - k) : -1;
    };

    IndexFile out;
    out.path = Lower(path);
    out.records.reserve(pairs.size());
    int missing = 0;

    for (const Pair& p : pairs)
    {
        int64_t o = find(p.path);

        if (o < 0)
        {
            if (missing++ < 5)
                Log("size table: no record for %s", p.path.c_str());
            continue;
        }

        uint64_t at = records + 20 * (uint64_t)o;
        Record* r = RecordAt(out, at, 20, d + at);
        r->climate = p.path == "/texture/climate_texture_1.dds";
        // Only the stored mip sizes: the original's DDS header (and with it
        // the texture's class, which DMM keeps for the originals but not for
        // new files) stays - the copies have the same size and format.
        memcpy(r->value[p.which] + 4, p.sizes, 16);
    }

    Log("size table: %zu records (%d missing)", out.records.size(), missing);
    g_files.push_back(std::move(out));
}

static void Scan()
{
    // The game folder: the one above bin64.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring root = exe;
    root = root.substr(0, root.rfind('\\'));
    root = root.substr(0, root.rfind('\\'));

    // The season data: bin64\Seasons\data
    std::wstring dataFolder = std::wstring(exe).substr(0, std::wstring(exe).rfind('\\')) + L"\\Seasons\\data";
    g_dataFolder = dataFolder;
    LoadSeasonData(dataFolder);

    std::vector<Pair> pairs;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((root + L"\\*").c_str(), &fd);

    if (f != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
            {
                std::wstring pamt = root + L"\\" + fd.cFileName + L"\\0.pamt";

                if (GetFileAttributesW(pamt.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    std::wstring w = fd.cFileName;
                    std::string group;

                    for (wchar_t c : w)
                        group += (char)c;

                    ScanPamt(pamt, group, dataFolder, &pairs);
                }
            }
        } while (FindNextFileW(f, &fd));

        FindClose(f);
    }

    for (IndexFile& i : g_files)
        std::sort(i.records.begin(), i.records.end(), [](const Record& a, const Record& b) { return a.offset < b.offset; });

    size_t before = g_files.size();
    ScanPathc(root + L"\\meta\\0.pathc", pairs);

    if (g_files.size() > before)
        std::sort(g_files.back().records.begin(), g_files.back().records.end(),
                  [](const Record& a, const Record& b) { return a.offset < b.offset; });

    ScanPapgt(root + L"\\meta\\0.papgt");
    Log("season copies: spring %d, autumn %d, winter %d", g_count[SPRING], g_count[AUTUMN], g_count[WINTER]);
}

// ---------------------------------------------------------------------------
// Reads: the entries in what the game just read get the season's bytes.
// ---------------------------------------------------------------------------

struct Open
{
    HANDLE handle;
    int file;           // in g_files
};

static SRWLOCK g_openLock = SRWLOCK_INIT;
static std::vector<Open> g_open;

static int FileOf(HANDLE h)
{
    int f = -1;
    AcquireSRWLockShared(&g_openLock);

    for (const Open& o : g_open)
        if (o.handle == h)
        {
            f = o.file;
            break;
        }

    ReleaseSRWLockShared(&g_openLock);
    return f;
}

static void Forget(HANDLE h)
{
    AcquireSRWLockExclusive(&g_openLock);
    g_open.erase(std::remove_if(g_open.begin(), g_open.end(), [h](const Open& o) { return o.handle == h; }), g_open.end());
    ReleaseSRWLockExclusive(&g_openLock);
}

struct PazOpen
{
    HANDLE handle;
    Region* region;
};

static std::vector<PazOpen> g_pazOpen;      // under g_openLock

static Region* RegionOf(HANDLE h)
{
    Region* r = NULL;
    AcquireSRWLockShared(&g_openLock);

    for (const PazOpen& o : g_pazOpen)
        if (o.handle == h)
        {
            r = o.region;
            break;
        }

    ReleaseSRWLockShared(&g_openLock);
    return r;
}

// "...\0001\3.paz": the archive a region is past the end of.
static void OpenedPaz(HANDLE h, const std::wstring& n)
{
    size_t slash = n.rfind('\\');

    if (slash == std::wstring::npos || slash == 0)
        return;

    size_t before = n.rfind('\\', slash - 1);
    std::wstring group = n.substr(before == std::wstring::npos ? 0 : before + 1, slash - (before == std::wstring::npos ? 0 : before + 1));
    uint32_t paz = (uint32_t)_wtoi(n.c_str() + slash + 1);

    for (Region* r : g_regions)
    {
        std::wstring g;

        for (char c : r->group)
            g += (wchar_t)c;

        if (g == group && r->paz == paz)
        {
            AcquireSRWLockExclusive(&g_openLock);
            g_pazOpen.erase(std::remove_if(g_pazOpen.begin(), g_pazOpen.end(), [h](const PazOpen& o) { return o.handle == h; }),
                            g_pazOpen.end());
            g_pazOpen.push_back({ h, r });
            ReleaseSRWLockExclusive(&g_openLock);
            Log("archive %S opened: season data served past %llu", n.c_str(), r->base);
        }
    }
}

static void Opened(HANDLE h, const wchar_t* name, const char* how)
{
    if (h == INVALID_HANDLE_VALUE || !name)
        return;

    std::wstring n = Lower(name);
    size_t len = n.size();

    if (len > 4 && n.compare(len - 4, 4, L".paz") == 0)
    {
        OpenedPaz(h, n);
        return;
    }
    bool index = (len > 7 && n.compare(len - 7, 7, L"\\0.pamt") == 0) || (len > 8 && n.compare(len - 8, 8, L"\\0.pathc") == 0) ||
                 (len > 8 && n.compare(len - 8, 8, L"\\0.papgt") == 0);

    if (!index)
        return;

    int file = -1;

    for (size_t i = 0; i < g_files.size(); ++i)
        if (n.size() >= g_files[i].path.size() && n.compare(n.size() - g_files[i].path.size(), g_files[i].path.size(), g_files[i].path) == 0)
            file = (int)i;

    // Relative paths and the like: match the group folder and file name.
    if (file < 0)
    {
        size_t last = n.rfind('\\');
        size_t cut = last != std::wstring::npos && last > 0 ? n.rfind('\\', last - 1) : std::wstring::npos;
        std::wstring tail = cut != std::wstring::npos ? n.substr(cut) : n;

        for (size_t i = 0; i < g_files.size(); ++i)
        {
            const std::wstring& p = g_files[i].path;

            if (p.size() >= tail.size() && p.compare(p.size() - tail.size(), tail.size(), tail) == 0)
                file = (int)i;
        }
    }

    if (file < 0)
        return;

    Log("open (%s) %S", how, name);

    Forget(h);
    AcquireSRWLockExclusive(&g_openLock);
    g_open.push_back({ h, file });
    ReleaseSRWLockExclusive(&g_openLock);
}

// The game read [at, at + n) of file f into buffer.
static void Patch(int f, uint64_t at, uint8_t* buffer, size_t n)
{
    int patched = 0;
    AcquireSRWLockExclusive(&g_lock);
    Season season = (Season)g_season;

    for (Record& r : g_files[f].records)
    {
        if (r.offset < at || r.offset + r.size > at + n)
            continue;

        uint8_t* p = buffer + (r.offset - at);

        if (SafeCopy(p, r.value[For(r, season)], r.size))
        {
            bool seen = false;

            for (int i = 0; i < r.lives; ++i)
                seen = seen || r.live[i] == p;

            if (!seen)
            {
                if (r.lives < 4)
                    r.live[r.lives++] = p;
                else
                    r.live[3] = p;
            }
            ++patched;
        }
    }

    ReleaseSRWLockExclusive(&g_lock);

    if (patched)
        Log("read %zu bytes at %llu of %S: %d entries set to %s", n, at, g_files[f].path.c_str(), patched, SEASON_NAMES[season]);
}

typedef HANDLE(WINAPI* CreateFileWFn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFileAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFile2Fn)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
typedef BOOL(WINAPI* ReadFileFn)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI* GetOverlappedResultFn)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
typedef BOOL(WINAPI* CloseHandleFn)(HANDLE);
typedef HANDLE(WINAPI* CreateFileMappingWFn)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);

static CreateFileWFn g_createFileW = NULL;
static CreateFileAFn g_createFileA = NULL;
static CreateFile2Fn g_createFile2 = NULL;
static ReadFileFn g_readFile = NULL;
static GetOverlappedResultFn g_getOverlappedResult = NULL;
static CloseHandleFn g_closeHandle = NULL;
static CreateFileMappingWFn g_createFileMappingW = NULL;

// Overlapped reads still running: patched when they complete.
struct Pending
{
    HANDLE handle;
    LPOVERLAPPED ov;
    uint8_t* buffer;
    uint64_t at;
    int file;
};

static SRWLOCK g_pendingLock = SRWLOCK_INIT;
static std::vector<Pending> g_pending;

static HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE t)
{
    HANDLE h = g_createFileW(name, access, share, sa, disp, flags, t);
    Opened(h, name, "W");
    return h;
}

static HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE t)
{
    HANDLE h = g_createFileA(name, access, share, sa, disp, flags, t);

    if (h != INVALID_HANDLE_VALUE && name)
    {
        wchar_t w[MAX_PATH * 2];
        MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH * 2);
        Opened(h, w, "A");
    }

    return h;
}

static HANDLE WINAPI HookCreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disp, LPCREATEFILE2_EXTENDED_PARAMETERS ex)
{
    HANDLE h = g_createFile2(name, access, share, disp, ex);
    Opened(h, name, "2");
    return h;
}

// Where the reads that bring the game its completion notices go: shared (what
// lands there is never looked at), grown when a read is bigger. Old buffers are
// kept: a read still in flight may write into one.
static uint8_t* Scratch(DWORD size)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static uint8_t* buffer = NULL;
    static DWORD capacity = 0;
    AcquireSRWLockExclusive(&lock);

    if (size > capacity)
    {
        capacity = size < (16u << 20) ? (16u << 20) : ((size + 0xFFFFF) & ~0xFFFFFu);
        buffer = (uint8_t*)VirtualAlloc(NULL, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }

    uint8_t* b = buffer;
    ReleaseSRWLockExclusive(&lock);
    return b;
}

// Overlapped reads served from the season data: filled again when the game
// is told they are done (what Windows writes meanwhile must not stay).
struct Served
{
    LPOVERLAPPED ov;
    Region* region;
    uint8_t* buffer;
    DWORD want;
    uint64_t at;
    uint32_t check;     // the first bytes as filled
};

static SRWLOCK g_servedLock = SRWLOCK_INIT;
static std::vector<Served> g_servedList;
static volatile LONG g_overwritten = 0, g_refilled = 0;

static void Fill(Region* r, uint8_t* buffer, DWORD want, uint64_t at);

static void Completed(LPOVERLAPPED ov)
{
    Served sv = {};
    bool found = false;
    AcquireSRWLockExclusive(&g_servedLock);

    for (size_t i = 0; i < g_servedList.size(); ++i)
        if (g_servedList[i].ov == ov)
        {
            sv = g_servedList[i];
            g_servedList.erase(g_servedList.begin() + i);
            found = true;
            break;
        }

    ReleaseSRWLockExclusive(&g_servedLock);

    if (!found)
        return;

    uint32_t now = 0;
    SafeCopy(&now, sv.buffer, sv.want < 4 ? sv.want : 4);

    if (now != sv.check && InterlockedIncrement(&g_overwritten) <= 10)
        Log("served read at +%llu of %s/%u was overwritten before the game looked (%08X, filled %08X)",
            sv.at - sv.region->base, sv.region->group.c_str(), sv.region->paz, now, sv.check);

    Fill(sv.region, sv.buffer, sv.want, sv.at);
    InterlockedIncrement(&g_refilled);
}

static void Fill(Region* r, uint8_t* buffer, DWORD want, uint64_t at)
{
    memset(buffer, 0, want);

    if (at + want > r->base)
    {
        uint64_t from = r->datStart + (at > r->base ? at - r->base : 0);
        size_t skip = at > r->base ? 0 : (size_t)(r->base - at);
        uint64_t n = want - skip;

        if (from < r->datEnd)
        {
            if (from + n > r->datEnd)
                n = r->datEnd - from;

            AcquireSRWLockExclusive(&r->lock);

            if (!r->handle)
                r->handle = g_createFileW(r->dat.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

            DWORD read = 0;
            LARGE_INTEGER pos;
            pos.QuadPart = (LONGLONG)from;

            if (r->handle == INVALID_HANDLE_VALUE || !SetFilePointerEx(r->handle, pos, NULL, FILE_BEGIN) ||
                !g_readFile(r->handle, buffer + skip, (DWORD)n, &read, NULL))
                Log("season data %s: read failed at %llu", r->group.c_str(), from);

            ReleaseSRWLockExclusive(&r->lock);
        }
    }

}

static BOOL Serve(Region* r, HANDLE h, uint8_t* buffer, DWORD want, LPDWORD got, LPOVERLAPPED ov, uint64_t at)
{
    Fill(r, buffer, want, at);

    if (!ov)
    {
        if (got)
            *got = want;

        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)(at + want);
        SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }

    // Overlapped: the game waits for Windows to tell it the read is done (an
    // event or a completion port). A real read of the same length, from the
    // start of the archive into a scratch buffer, brings that notice; the
    // game's buffer already holds the season data.
    Served sv = { ov, r, buffer, want, at, 0 };
    SafeCopy(&sv.check, buffer, want < 4 ? want : 4);
    AcquireSRWLockExclusive(&g_servedLock);
    g_servedList.push_back(sv);
    ReleaseSRWLockExclusive(&g_servedLock);

    uint8_t* scratch = Scratch(want);
    DWORD offset = ov->Offset, offsetHigh = ov->OffsetHigh;
    ov->Offset = 0;
    ov->OffsetHigh = 0;
    BOOL ok = g_readFile(h, scratch, want, got, ov);
    DWORD error = GetLastError();
    ov->Offset = offset;
    ov->OffsetHigh = offsetHigh;

    if (ok)
        Completed(ov);
    SetLastError(error);
    return ok;
}

static BOOL WINAPI HookReadFile(HANDLE h, LPVOID buffer, DWORD want, LPDWORD got, LPOVERLAPPED ov)
{
    if (Region* r = RegionOf(h))
    {
        uint64_t at = 0;

        if (ov)
            at = ((uint64_t)ov->OffsetHigh << 32) | ov->Offset;
        else
        {
            LARGE_INTEGER zero = {}, pos = {};
            SetFilePointerEx(h, zero, &pos, FILE_CURRENT);
            at = (uint64_t)pos.QuadPart;
        }

        if (at >= r->pazEnd)
            return Serve(r, h, (uint8_t*)buffer, want, got, ov, at);

        return g_readFile(h, buffer, want, got, ov);
    }

    int f = FileOf(h);

    if (f < 0)
        return g_readFile(h, buffer, want, got, ov);

    uint64_t at = 0;

    if (ov)
        at = ((uint64_t)ov->OffsetHigh << 32) | ov->Offset;
    else
    {
        LARGE_INTEGER zero = {}, pos = {};
        SetFilePointerEx(h, zero, &pos, FILE_CURRENT);
        at = (uint64_t)pos.QuadPart;
    }

    DWORD n = 0;
    BOOL ok = g_readFile(h, buffer, want, &n, ov);
    DWORD error = GetLastError();

    if (got)
        *got = n;

    if (ok)
        Patch(f, at, (uint8_t*)buffer, n);
    else if (error == ERROR_IO_PENDING && ov)
    {
        AcquireSRWLockExclusive(&g_pendingLock);
        g_pending.push_back({ h, ov, (uint8_t*)buffer, at, f });
        ReleaseSRWLockExclusive(&g_pendingLock);
    }

    SetLastError(error);
    return ok;
}

typedef BOOL(WINAPI* GetOverlappedResultExFn)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL);
static GetOverlappedResultExFn g_getOverlappedResultEx = NULL;

static BOOL WINAPI HookGetOverlappedResultEx(HANDLE h, LPOVERLAPPED ov, LPDWORD got, DWORD ms, BOOL alertable)
{
    BOOL ok = g_getOverlappedResultEx(h, ov, got, ms, alertable);

    if (ok)
        Completed(ov);

    return ok;
}

static BOOL WINAPI HookGetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD got, BOOL wait)
{
    BOOL ok = g_getOverlappedResult(h, ov, got, wait);

    if (ok)
        Completed(ov);

    if (ok)
    {
        Pending p = {};
        bool found = false;
        AcquireSRWLockExclusive(&g_pendingLock);

        for (size_t i = 0; i < g_pending.size(); ++i)
            if (g_pending[i].handle == h && g_pending[i].ov == ov)
            {
                p = g_pending[i];
                g_pending.erase(g_pending.begin() + i);
                found = true;
                break;
            }

        ReleaseSRWLockExclusive(&g_pendingLock);

        if (found && got)
            Patch(p.file, p.at, p.buffer, *got);
    }

    return ok;
}

static BOOL WINAPI HookCloseHandle(HANDLE h)
{
    if (FileOf(h) >= 0)
        Forget(h);

    if (RegionOf(h))
    {
        AcquireSRWLockExclusive(&g_openLock);
        g_pazOpen.erase(std::remove_if(g_pazOpen.begin(), g_pazOpen.end(), [h](const PazOpen& o) { return o.handle == h; }),
                        g_pazOpen.end());
        ReleaseSRWLockExclusive(&g_openLock);
    }

    return g_closeHandle(h);
}

typedef BOOL(WINAPI* GetFileSizeExFn)(HANDLE, PLARGE_INTEGER);
typedef DWORD(WINAPI* GetFileSizeFn)(HANDLE, LPDWORD);
typedef BOOL(WINAPI* GetFileInformationByHandleFn)(HANDLE, LPBY_HANDLE_FILE_INFORMATION);
typedef BOOL(WINAPI* GetFileInformationByHandleExFn)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
static GetFileSizeExFn g_getFileSizeEx = NULL;
static GetFileSizeFn g_getFileSize = NULL;
static GetFileInformationByHandleFn g_getFileInformationByHandle = NULL;
static GetFileInformationByHandleExFn g_getFileInformationByHandleEx = NULL;

static BOOL WINAPI HookGetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    BOOL ok = g_getFileSizeEx(h, size);
    Region* r = ok && size ? RegionOf(h) : NULL;

    if (r && r->fakeSize > (uint64_t)size->QuadPart)
    {
        size->QuadPart = (LONGLONG)r->fakeSize;
    }

    return ok;
}

static DWORD WINAPI HookGetFileSize(HANDLE h, LPDWORD high)
{
    DWORD low = g_getFileSize(h, high);
    Region* r = low != INVALID_FILE_SIZE ? RegionOf(h) : NULL;

    if (r)
    {
        if (high)
            *high = (DWORD)(r->fakeSize >> 32);

        return (DWORD)r->fakeSize;
    }

    return low;
}

static BOOL WINAPI HookGetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info)
{
    BOOL ok = g_getFileInformationByHandle(h, info);
    Region* r = ok && info ? RegionOf(h) : NULL;

    if (r)
    {
        info->nFileSizeHigh = (DWORD)(r->fakeSize >> 32);
        info->nFileSizeLow = (DWORD)r->fakeSize;
    }

    return ok;
}

static BOOL WINAPI HookGetFileInformationByHandleEx(HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID info, DWORD size)
{
    BOOL ok = g_getFileInformationByHandleEx(h, cls, info, size);
    Region* r = ok && info && cls == FileStandardInfo ? RegionOf(h) : NULL;

    if (r)
    {
        FILE_STANDARD_INFO* st = (FILE_STANDARD_INFO*)info;
        st->EndOfFile.QuadPart = (LONGLONG)r->fakeSize;
        st->AllocationSize.QuadPart = (LONGLONG)r->fakeSize;
    }

    return ok;
}

static HANDLE WINAPI HookCreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name)
{
    int f = FileOf(file);

    if (f >= 0)
        Log("mapping %S - not changed (read another way)", g_files[f].path.c_str());

    return g_createFileMappingW(file, sa, protect, hi, lo, name);
}

bool ArchiveInit(Season start)
{
    g_season = start;
    Scan();

    if (MH_Initialize() != MH_OK)
    {
        Log("hooks: MinHook failed");
        return false;
    }

    bool ok =
        MH_CreateHookApi(L"kernelbase", "CreateFileW", (LPVOID)HookCreateFileW, (LPVOID*)&g_createFileW) == MH_OK &&
        MH_CreateHookApi(L"kernelbase", "CreateFileA", (LPVOID)HookCreateFileA, (LPVOID*)&g_createFileA) == MH_OK &&
        MH_CreateHookApi(L"kernelbase", "CreateFile2", (LPVOID)HookCreateFile2, (LPVOID*)&g_createFile2) == MH_OK &&
        MH_CreateHookApi(L"kernelbase", "ReadFile", (LPVOID)HookReadFile, (LPVOID*)&g_readFile) == MH_OK &&
        MH_CreateHookApi(L"kernelbase", "GetOverlappedResult", (LPVOID)HookGetOverlappedResult, (LPVOID*)&g_getOverlappedResult) == MH_OK &&
        MH_CreateHookApi(L"kernelbase", "GetOverlappedResultEx", (LPVOID)HookGetOverlappedResultEx, (LPVOID*)&g_getOverlappedResultEx) == MH_OK &&
        MH_CreateHookApi(L"kernelbase", "CloseHandle", (LPVOID)HookCloseHandle, (LPVOID*)&g_closeHandle) == MH_OK &&
        MH_CreateHookApi(L"kernelbase", "CreateFileMappingW", (LPVOID)HookCreateFileMappingW, (LPVOID*)&g_createFileMappingW) == MH_OK &&
        MH_CreateHookApi(L"kernelbase", "GetFileSizeEx", (LPVOID)HookGetFileSizeEx, (LPVOID*)&g_getFileSizeEx) == MH_OK &&
        MH_CreateHookApi(L"kernelbase", "GetFileSize", (LPVOID)HookGetFileSize, (LPVOID*)&g_getFileSize) == MH_OK &&
        MH_CreateHookApi(L"kernelbase", "GetFileInformationByHandle", (LPVOID)HookGetFileInformationByHandle,
                         (LPVOID*)&g_getFileInformationByHandle) == MH_OK &&
        MH_CreateHookApi(L"kernelbase", "GetFileInformationByHandleEx", (LPVOID)HookGetFileInformationByHandleEx,
                         (LPVOID*)&g_getFileInformationByHandleEx) == MH_OK &&
        MH_EnableHook(MH_ALL_HOOKS) == MH_OK;

    Log("hooks: %s", ok ? "on" : "FAILED");
    return ok;
}

// Copies of index file f in [base, base + size): their entries join the ones changed.
static int FindCopyIn(uint8_t* base, size_t size, IndexFile* f)
{
    int found = 0;

    __try
    {
        uint32_t first;
        memcpy(&first, f->signature, 4);

        for (size_t i = 0; i + sizeof(f->signature) <= size; i += 4)
        {
            if (*(uint32_t*)(base + i) != first || memcmp(base + i, f->signature, sizeof(f->signature)) != 0)
                continue;

            // copy + file offset = the entry's place in the game's list.
            uint8_t* copy = base + i - f->signatureAt;

            if (base + i - (f->signatureAt - f->listAt) < base)
                continue;

            // A copy of the file: its entries hold one of our values.
            int known = 0, checked = 0;
            size_t step = f->records.size() / 8 + 1;

            for (size_t k = 0; k < f->records.size() && checked < 8; k += step)
            {
                Record& r = f->records[k];
                uint8_t* at = copy + r.offset;

                if (r.offset < f->listAt || r.offset >= f->listEnd || at < base || at + r.size > base + size)
                    continue;

                ++checked;

                for (int sn = 0; sn < SEASON_COUNT; ++sn)
                    if (memcmp(at, r.value[sn], r.size) == 0)
                    {
                        ++known;
                        break;
                    }
            }

            if (!checked || known < checked)
                continue;

            bool already = false;

            for (size_t k = 0; k < f->records.size() && !already; ++k)
                for (int l = 0; l < f->records[k].lives && !already; ++l)
                    already = f->records[k].live[l] == copy + f->records[k].offset;

            if (already)
                continue;

            for (size_t k = 0; k < f->records.size(); ++k)
            {
                Record& r = f->records[k];

                if (r.offset < f->listAt || r.offset >= f->listEnd)
                    continue;       // only the file list is kept (the header is checked when read)

                if (r.lives < 4)
                    r.live[r.lives++] = copy + r.offset;
                else
                    r.live[3] = copy + r.offset;
            }

            ++found;
            Log("the game's copy of %S found at %p", f->path.c_str(), copy);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return found;
}

// The game reads an index into a buffer, checks it and keeps a copy of its
// own: found once by its signature (and checked against known entries), its
// entries are changed along with the others.
static void LocateCopies()
{
    std::vector<IndexFile*> files;

    for (IndexFile& f : g_files)
        if (!f.located && f.signatureAt)
            files.push_back(&f);

    if (files.empty())
        return;

    DWORD started = GetTickCount();
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t* addr = NULL;
    int found = 0;

    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        uint8_t* base = (uint8_t*)mbi.BaseAddress;
        size_t size = mbi.RegionSize;
        addr = base + size;

        // Plain read-write memory only: write-combined memory (the graphics
        // card's upload buffers) is very slow to read, gigabytes of it froze the game.
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE)
            continue;

        for (IndexFile* f : files)
            found += FindCopyIn(base, size, f);
    }

    for (IndexFile* f : files)
        f->located = true;

    Log("looked for the game's index copies: %d found in %lu ms", found, GetTickCount() - started);
}

static void ApplySeason(Season season);

static DWORD WINAPI SwitchThread(LPVOID p)
{
    ApplySeason((Season)(INT_PTR)p);
    GpuSeasonChanged((Season)(INT_PTR)p);
    ClimateSeasonChanged((Season)(INT_PTR)p);
    return 0;
}

std::vector<SeasonFileRef> ArchiveListSeasonFiles()
{
    std::vector<SeasonFileRef> out;

    for (auto* set : { &g_data })
        for (auto& [group, files] : *set)
        {
            std::wstring dat = g_dataFolder + L"\\";

            for (char c : group)
                dat += (wchar_t)c;

            dat += L".dat";

            for (const SeasonFile& sf : files)
                out.push_back({ group, sf.path, sf.which, dat, sf.offset, sf.stored, sf.size });
        }

    return out;
}

bool ArchiveSeasonFileBytes(const char* group, const char* path, Season season, std::vector<uint8_t>* out)
{
    auto data = g_data.find(group);

    if (data == g_data.end())
        return false;

    for (const SeasonFile& sf : data->second)
    {
        if (sf.which != season || sf.path != path)
            continue;

        std::wstring dat = g_dataFolder + L"\\";

        for (const char* c = group; *c; ++c)
            dat += (wchar_t)*c;

        dat += L".dat";
        HANDLE f = CreateFileW(dat.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

        if (f == INVALID_HANDLE_VALUE)
            return false;

        out->resize(sf.stored);
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)sf.offset;
        DWORD got = 0;
        bool ok = SetFilePointerEx(f, pos, NULL, FILE_BEGIN) && ReadFile(f, out->data(), sf.stored, &got, NULL) && got == sf.stored;
        CloseHandle(f);
        return ok;
    }

    return false;
}

static volatile LONG g_switching = 0;

// From the menu (the game's window thread): done on a thread of its own, so
// looking for the game's copies never holds the game up.
void ArchiveSetSeason(Season season)
{
    InterlockedExchange(&g_season, season);
    InterlockedIncrement(&g_switching);
    HANDLE t = CreateThread(NULL, 0, SwitchThread, (LPVOID)(INT_PTR)season, 0, NULL);

    if (t)
        CloseHandle(t);
    else
        InterlockedDecrement(&g_switching);
}

bool ArchiveSwitching()
{
    return g_switching != 0;
}

static void ApplySeason(Season season)
{
    int done = 0, gone = 0, unread = 0;
    AcquireSRWLockExclusive(&g_lock);
    LocateCopies();

    for (IndexFile& f : g_files)
        for (Record& r : f.records)
        {
            if (!r.lives)
            {
                ++unread;
                continue;
            }

            // Wherever the game still has one of our values (it may have
            // freed or reused a buffer it read into).
            bool any = false;

            for (int i = 0; i < r.lives; ++i)
            {
                uint8_t now[20];
                bool ours = SafeCopy(now, r.live[i], r.size);
                bool known = false;

                for (int s = 0; ours && s < SEASON_COUNT && !known; ++s)
                    known = memcmp(now, r.value[s], r.size) == 0;

                if (known && SafeCopy(r.live[i], r.value[For(r, season)], r.size))
                    any = true;
            }

            if (any)
                ++done;
            else
                ++gone;
        }

    ReleaseSRWLockExclusive(&g_lock);
    InterlockedDecrement(&g_switching);
    Log("season %s: %d entries changed, %d no longer there, %d never read", SEASON_NAMES[season], done, gone, unread);
}

Season ArchiveSeason() { return (Season)g_season; }

int ArchiveSeasonFiles(Season season) { return g_count[season]; }
