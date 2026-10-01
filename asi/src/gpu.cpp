#include "gpu.h"
#include "log.h"

#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"

// ---------------------------------------------------------------------------
// Season textures on the graphics card
//
// The game streams most textures in and out, but some stay loaded for long
// (the trees' and grass' distant images, the far terrain) or for good (the
// climate texture: winter's snow). A season change must reach those too.
//
// Every season file's mips are known by their first 64 bytes (a signature).
// The game's copies to the graphics card (CopyTextureRegion from an upload
// buffer) are compared with them: a match tells which texture, season and mip
// that copy is. On a season change the season's version of every such
// texture, each of its loaded mips, is uploaded through the game's queue.
// When the game frees a texture, a small object attached to it tells.
// ---------------------------------------------------------------------------

static const size_t SIG = 64;

struct FileInfo
{
    SeasonFileRef ref;
    uint32_t width, height, mips;
    uint32_t blockBytes, blockSize;     // bytes of a block, pixels a block spans (1 for plain pixels)
    uint32_t head;                      // header bytes
    int texture;                        // index in g_textures
};

struct Texture
{
    std::string key;                    // group + path
    int file[SEASON_COUNT];             // its version in each season (-1: none)
};

struct SigEntry
{
    uint8_t bytes[SIG];
    int file;
    uint32_t mip;
};

static std::vector<FileInfo> g_files;
static std::vector<Texture> g_textures;
static std::unordered_multimap<uint64_t, SigEntry> g_sigs;
static volatile LONG g_sigsReady = 0;

struct Tracked
{
    int texture;
    uint32_t mips;                      // bit per mip the game copied into
    uint32_t resourceMips;
    uint32_t mipOffset;                 // the file's mip at the resource's mip 0 (the game may leave out the biggest)
    Season season;                      // the version it holds
    volatile LONG state;                // D3D12_RESOURCE_STATES the game left it in
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::unordered_map<ID3D12Resource*, Tracked> g_tracked;
static ID3D12CommandQueue* g_queue = NULL;
static ID3D12Device* g_device = NULL;
static __declspec(thread) int t_ours = 0;
static volatile LONG g_matches = 0, g_freed = 0;
static volatile LONG g_target = SUMMER;     // the season the textures should hold
static HANDLE g_wake = NULL;
static DWORD WINAPI Keeper(LPVOID);

static uint64_t Hash(const uint8_t* p)
{
    uint64_t h = 1469598103934665603ull;

    for (size_t i = 0; i < SIG; ++i)
        h = (h ^ p[i]) * 1099511628211ull;

    return h;
}

// ---------------------------------------------------------------------------
// The season data's files, as the game packs them
// ---------------------------------------------------------------------------

static bool Lz4Decode(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize)
{
    size_t i = 0, o = 0;

    while (i < srcSize)
    {
        uint8_t token = src[i++];
        size_t lit = token >> 4;

        if (lit == 15)
        {
            uint8_t b;
            do { if (i >= srcSize) return false; b = src[i++]; lit += b; } while (b == 255);
        }

        if (i + lit > srcSize || o + lit > dstSize)
            return false;

        memcpy(dst + o, src + i, lit);
        i += lit;
        o += lit;

        if (i >= srcSize)
            break;

        if (i + 2 > srcSize)
            return false;

        size_t offset = src[i] | (src[i + 1] << 8);
        i += 2;
        size_t ml = token & 15;

        if (ml == 15)
        {
            uint8_t b;
            do { if (i >= srcSize) return false; b = src[i++]; ml += b; } while (b == 255);
        }

        ml += 4;

        if (offset == 0 || offset > o || o + ml > dstSize)
            return false;

        for (size_t k = 0; k < ml; ++k, ++o)
            dst[o] = dst[o - offset];
    }

    return o == dstSize;
}

static bool Describe(const uint8_t* d, size_t n, FileInfo* f)
{
    if (n < 128 || memcmp(d, "DDS ", 4) != 0)
        return false;

    memcpy(&f->height, d + 12, 4);
    memcpy(&f->width, d + 16, 4);
    memcpy(&f->mips, d + 28, 4);
    f->mips = f->mips ? f->mips : 1;
    f->head = 128;
    uint32_t fourcc;
    memcpy(&fourcc, d + 84, 4);
    f->blockBytes = 0;
    f->blockSize = 4;

    if (fourcc == '1TXD')
        f->blockBytes = 8;
    else if (fourcc == '3TXD' || fourcc == '5TXD')
        f->blockBytes = 16;
    else if (fourcc == '01XD')
    {
        if (n < 148)
            return false;

        f->head = 148;
        uint32_t dxgi;
        memcpy(&dxgi, d + 128, 4);

        if ((dxgi >= 70 && dxgi <= 72) || (dxgi >= 79 && dxgi <= 81))
            f->blockBytes = 8;
        else if ((dxgi >= 73 && dxgi <= 78) || (dxgi >= 82 && dxgi <= 84) || (dxgi >= 94 && dxgi <= 99))
            f->blockBytes = 16;
        else if (dxgi == 28 || dxgi == 29 || dxgi == 87 || dxgi == 88 || dxgi == 91 || dxgi == 27)
        {
            f->blockBytes = 4;
            f->blockSize = 1;
        }
    }
    else if (fourcc == 0)
    {
        uint32_t bits;
        memcpy(&bits, d + 88, 4);
        f->blockBytes = bits / 8;
        f->blockSize = 1;
    }

    return f->blockBytes != 0 && f->mips <= 16;
}

static void MipShape(const FileInfo& f, uint32_t m, uint32_t* rowBytes, uint32_t* rows)
{
    uint32_t w = f.width >> m ? f.width >> m : 1, h = f.height >> m ? f.height >> m : 1;
    uint32_t bx = (w + f.blockSize - 1) / f.blockSize, by = (h + f.blockSize - 1) / f.blockSize;
    *rowBytes = (bx ? bx : 1) * f.blockBytes;
    *rows = by ? by : 1;
}

// A file's mips, unpacked.
static bool Unpack(const FileInfo& f, const std::vector<uint8_t>& file, std::vector<std::vector<uint8_t>>* mips)
{
    uint32_t stored[4];
    memcpy(stored, file.data() + 0x20, 16);
    size_t pos = f.head;
    mips->clear();

    for (uint32_t m = 0; m < f.mips; ++m)
    {
        uint32_t rowBytes, rows;
        MipShape(f, m, &rowBytes, &rows);
        size_t raw = (size_t)rowBytes * rows;
        size_t st = m < 4 && stored[m] ? stored[m] : raw;
        std::vector<uint8_t> px(raw);

        if (pos + st > file.size())
            return false;

        if (st < raw)
        {
            if (!Lz4Decode(file.data() + pos, st, px.data(), raw))
                return false;
        }
        else
            memcpy(px.data(), file.data() + pos, raw);

        pos += st;
        mips->push_back(std::move(px));
    }

    return true;
}

static bool ReadStored(const SeasonFileRef& r, HANDLE* cache, std::wstring* cachePath, std::vector<uint8_t>* out)
{
    if (*cachePath != r.dat)
    {
        if (*cache != INVALID_HANDLE_VALUE)
            CloseHandle(*cache);

        *cache = CreateFileW(r.dat.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        *cachePath = r.dat;
    }

    if (*cache == INVALID_HANDLE_VALUE)
        return false;

    out->resize(r.stored);
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)r.offset;
    DWORD got = 0;
    return SetFilePointerEx(*cache, pos, NULL, FILE_BEGIN) && ReadFile(*cache, out->data(), r.stored, &got, NULL) && got == r.stored;
}

// The signatures of every mip whose first row is 64 bytes or more (and not
// all one byte: those say nothing).
static void Learn(bool climateOnly)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    std::wstring path;
    std::vector<uint8_t> file;
    std::vector<std::vector<uint8_t>> mips;
    std::vector<std::pair<uint64_t, SigEntry>> sigs;
    int learned = 0;

    for (size_t i = 0; i < g_files.size(); ++i)
    {
        const FileInfo& f = g_files[i];
        bool climate = f.ref.group == "0002";

        if (climate != climateOnly || !ReadStored(f.ref, &h, &path, &file) || !Unpack(f, file, &mips))
            continue;

        for (uint32_t m = 0; m < f.mips; ++m)
        {
            uint32_t rowBytes, rows;
            MipShape(f, m, &rowBytes, &rows);

            if (rowBytes < SIG)
                continue;

            // Bytes that repeat (empty or one-colour blocks) are shared by many
            // textures: they say nothing.
            const uint8_t* p = mips[m].data();
            bool periodic = false;

            for (size_t period : { 1, 2, 4, 8, 16, 32 })
            {
                bool same = true;

                for (size_t k = period; k < SIG && same; ++k)
                    same = p[k] == p[k - period];

                periodic = periodic || same;
            }

            if (periodic)
                continue;

            SigEntry e;
            memcpy(e.bytes, p, SIG);
            e.file = (int)i;
            e.mip = m;
            sigs.push_back({ Hash(p), e });
        }

        ++learned;
    }

    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);

    AcquireSRWLockExclusive(&g_lock);

    for (auto& s : sigs)
        g_sigs.insert(s);

    ReleaseSRWLockExclusive(&g_lock);
    Log("gpu: %d %s files known by their mips (%zu signatures)", learned, climateOnly ? "climate" : "season", sigs.size());
}

static DWORD WINAPI LearnThread(LPVOID)
{
    Learn(false);
    InterlockedExchange(&g_sigsReady, 2);
    return 0;
}

static void Prepare()
{
    std::vector<SeasonFileRef> refs = ArchiveListSeasonFiles();
    std::unordered_map<std::string, int> byKey;
    HANDLE h = INVALID_HANDLE_VALUE;
    std::wstring path;
    std::vector<uint8_t> head;

    for (const SeasonFileRef& r : refs)
    {
        FileInfo f = {};
        f.ref = r;
        SeasonFileRef first = r;
        first.stored = r.stored < 256 ? r.stored : 256;

        if (!ReadStored(first, &h, &path, &head) || !Describe(head.data(), head.size(), &f))
            continue;

        std::string key = r.group + "/" + r.path;
        auto it = byKey.find(key);

        if (it == byKey.end())
        {
            Texture t;
            t.key = key;

            for (int s = 0; s < SEASON_COUNT; ++s)
                t.file[s] = -1;

            it = byKey.emplace(key, (int)g_textures.size()).first;
            g_textures.push_back(t);
        }

        f.texture = it->second;
        g_textures[f.texture].file[r.season] = (int)g_files.size();
        g_files.push_back(f);
    }

    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);

    Log("gpu: %zu season files of %zu textures", g_files.size(), g_textures.size());
}

// ---------------------------------------------------------------------------
// Knowing when the game frees a texture: an object of ours in its private
// data is released with it.
// ---------------------------------------------------------------------------

static const GUID SEASONS_WATCH = { 0x5ea50e5a, 0x1c2b, 0x4f7e, { 0x9a, 0x51, 0x3c, 0x2e, 0x7d, 0x90, 0x11, 0x08 } };

class Watcher : public IUnknown
{
public:
    explicit Watcher(ID3D12Resource* r) : m_refs(1), m_resource(r) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (iid == __uuidof(IUnknown))
        {
            *out = this;
            AddRef();
            return S_OK;
        }

        *out = NULL;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_refs); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG n = InterlockedDecrement(&m_refs);

        if (n == 0)
        {
            // The texture is gone: forget it (its address may be used again).
            AcquireSRWLockExclusive(&g_lock);
            g_tracked.erase(m_resource);
            ReleaseSRWLockExclusive(&g_lock);
            InterlockedIncrement(&g_freed);
            delete this;
        }

        return (ULONG)n;
    }

private:
    volatile LONG m_refs;
    ID3D12Resource* m_resource;
};

// ---------------------------------------------------------------------------
// The game's copies and barriers
// ---------------------------------------------------------------------------

typedef void(STDMETHODCALLTYPE* CopyTextureRegionFn)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                                      const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
typedef void(STDMETHODCALLTYPE* ResourceBarrierFn)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);

static CopyTextureRegionFn g_copyTextureRegion = NULL;
static ResourceBarrierFn g_resourceBarrier = NULL;

static bool ReadSignature(ID3D12Resource* src, UINT64 offset, uint8_t* out)
{
    void* p = NULL;
    D3D12_RANGE none = { 0, 0 };

    if (FAILED(src->Map(0, &none, &p)) || !p)
        return false;

    bool ok = true;

    __try
    {
        memcpy(out, (const uint8_t*)p + offset, SIG);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }

    src->Unmap(0, &none);
    return ok;
}

static void Check(const D3D12_TEXTURE_COPY_LOCATION* dst, const D3D12_TEXTURE_COPY_LOCATION* src, UINT x, UINT y)
{
    if (x || y || src->PlacedFootprint.Footprint.RowPitch < SIG)
        return;

    uint8_t sig[SIG];

    if (!ReadSignature(src->pResource, src->PlacedFootprint.Offset, sig))
        return;

    uint64_t h = Hash(sig);
    int file = -1;
    uint32_t mip = 0;
    D3D12_RESOURCE_DESC desc = dst->pResource->GetDesc();
    UINT resourceMip = desc.MipLevels ? dst->SubresourceIndex % desc.MipLevels : 0;

    AcquireSRWLockShared(&g_lock);
    auto range = g_sigs.equal_range(h);

    for (auto it = range.first; it != range.second; ++it)
    {
        const FileInfo& f = g_files[it->second.file];

        // The same bytes, and a mip of the same size as the one copied into.
        uint64_t w = desc.Width >> resourceMip ? desc.Width >> resourceMip : 1;
        uint32_t fw = f.width >> it->second.mip ? f.width >> it->second.mip : 1;

        if (fw == w && memcmp(it->second.bytes, sig, SIG) == 0)
        {
            if (file >= 0 && g_files[file].texture != f.texture)
            {
                file = -2;      // more than one texture starts this way: not used
                break;
            }

            file = it->second.file;
            mip = it->second.mip;
        }
    }

    ReleaseSRWLockShared(&g_lock);

    if (file < 0)
        return;

    const FileInfo& f = g_files[file];
    bool first = false;
    AcquireSRWLockExclusive(&g_lock);
    auto t = g_tracked.find(dst->pResource);

    if (t == g_tracked.end())
    {
        Tracked tr = {};
        tr.texture = f.texture;
        tr.resourceMips = desc.MipLevels;
        tr.mipOffset = mip >= resourceMip ? mip - resourceMip : 0;
        tr.season = f.ref.season;
        tr.state = D3D12_RESOURCE_STATE_COMMON;
        t = g_tracked.emplace(dst->pResource, tr).first;
        first = true;
    }

    if (t->second.texture == f.texture && mip == resourceMip + t->second.mipOffset)
    {
        t->second.mips |= 1u << resourceMip;
        t->second.season = f.ref.season;
    }

    ReleaseSRWLockExclusive(&g_lock);

    if (first)
    {
        Watcher* w = new Watcher(dst->pResource);
        dst->pResource->SetPrivateDataInterface(SEASONS_WATCH, w);
        w->Release();       // the texture holds it now
    }

    if (InterlockedIncrement(&g_matches) <= 12)
        Log("gpu: %s (%s, mip %u) copied into %p (mip %u of %u)", g_textures[f.texture].key.c_str(), SEASON_NAMES[f.ref.season],
            mip, dst->pResource, resourceMip, desc.MipLevels);
}

static void STDMETHODCALLTYPE HookCopyTextureRegion(ID3D12GraphicsCommandList* list, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x,
                                                    UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box)
{
    if (!t_ours && g_sigsReady && dst && src && dst->pResource && src->pResource &&
        dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
        Check(dst, src, x, y);

    g_copyTextureRegion(list, dst, x, y, z, src, box);
}

// The state the game leaves each tracked texture in, for our copies.
static void STDMETHODCALLTYPE HookResourceBarrier(ID3D12GraphicsCommandList* list, UINT n, const D3D12_RESOURCE_BARRIER* barriers)
{
    if (!t_ours && !g_tracked.empty())
    {
        AcquireSRWLockShared(&g_lock);

        for (UINT i = 0; i < n; ++i)
            if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
            {
                auto t = g_tracked.find(barriers[i].Transition.pResource);

                if (t != g_tracked.end())
                    InterlockedExchange(&t->second.state, (LONG)barriers[i].Transition.StateAfter);
            }

        ReleaseSRWLockShared(&g_lock);
    }

    g_resourceBarrier(list, n, barriers);
}

// ReShade (in the game folder) wraps the game's objects: the queue below them.
static ID3D12CommandQueue* Unwrapped(ID3D12CommandQueue* queue)
{
    static const GUID IID_ReShadeUnwrapped = { 0x7f2c9a11, 0x3b4e, 0x4d6a, { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 } };
    IUnknown* current = queue;
    current->AddRef();

    for (int depth = 0; depth < 4; ++depth)
    {
        IUnknown* inner = NULL;

        if (FAILED(current->QueryInterface(IID_ReShadeUnwrapped, (void**)&inner)) || !inner || inner == current)
        {
            if (inner)
                inner->Release();
            break;
        }

        current->Release();
        current = inner;
    }

    ID3D12CommandQueue* q = NULL;
    current->QueryInterface(IID_PPV_ARGS(&q));
    current->Release();
    return q;
}

void GpuInit(ID3D12CommandQueue* queue)
{
    if (g_queue || !queue)
        return;

    g_queue = Unwrapped(queue);

    if (!g_queue || FAILED(g_queue->GetDevice(IID_PPV_ARGS(&g_device))))
    {
        Log("gpu: no device");
        return;
    }

    g_target = ArchiveSeason();
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, Keeper, NULL, 0, NULL));

    // The climate texture is uploaded soon after: its signatures first.
    Prepare();
    Learn(true);
    InterlockedExchange(&g_sigsReady, 1);
    CloseHandle(CreateThread(NULL, 0, LearnThread, NULL, 0, NULL));

    // The command list functions every list shares, from a list of our own.
    ID3D12CommandAllocator* allocator = NULL;
    ID3D12GraphicsCommandList* list = NULL;

    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, IID_PPV_ARGS(&list))))
    {
        Log("gpu: no command list");
        if (allocator) allocator->Release();
        return;
    }

    void** vtable = *(void***)list;
    bool ok = MH_CreateHook(vtable[16], (void*)HookCopyTextureRegion, (void**)&g_copyTextureRegion) == MH_OK &&
              MH_CreateHook(vtable[26], (void*)HookResourceBarrier, (void**)&g_resourceBarrier) == MH_OK &&
              MH_EnableHook(vtable[16]) == MH_OK && MH_EnableHook(vtable[26]) == MH_OK;
    list->Close();
    list->Release();
    allocator->Release();
    Log("gpu: %s", ok ? "watching the game's texture uploads" : "could not hook the copy commands");
}

// ---------------------------------------------------------------------------
// A season change: every tracked texture gets the season's version of the
// mips it has loaded (a season without its own version takes summer's).
// ---------------------------------------------------------------------------

struct Job
{
    ID3D12Resource* resource;
    int file;
    uint32_t mips;
    UINT resourceMips;
    uint32_t mipOffset;
    D3D12_RESOURCE_STATES state;
};

// One batch: the jobs' mips through one upload buffer and command list.
static void Upload(std::vector<Job>& jobs, std::vector<std::vector<std::vector<uint8_t>>>& data)
{
    UINT64 total = 0;
    std::vector<std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>> feet(jobs.size());
    std::vector<std::vector<UINT>> rowsOf(jobs.size());

    for (size_t j = 0; j < jobs.size(); ++j)
    {
        D3D12_RESOURCE_DESC desc = jobs[j].resource->GetDesc();
        UINT n = jobs[j].resourceMips;
        feet[j].resize(n);
        rowsOf[j].resize(n);
        std::vector<UINT64> rowBytes(n);
        UINT64 size = 0;
        g_device->GetCopyableFootprints(&desc, 0, n, 0, feet[j].data(), rowsOf[j].data(), rowBytes.data(), &size);

        for (UINT m = 0; m < n; ++m)
            feet[j][m].Offset += total;

        total += (size + 511) & ~511ull;
    }

    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upload = NULL;

    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                 IID_PPV_ARGS(&upload))))
    {
        Log("gpu: no upload buffer (%llu bytes)", total);
        return;
    }

    uint8_t* p = NULL;
    D3D12_RANGE none = { 0, 0 };
    upload->Map(0, &none, (void**)&p);

    for (size_t j = 0; j < jobs.size(); ++j)
    {
        const FileInfo& f = g_files[jobs[j].file];

        for (UINT m = 0; m < jobs[j].resourceMips && m + jobs[j].mipOffset < data[j].size(); ++m)
        {
            if (!(jobs[j].mips & (1u << m)))
                continue;

            uint32_t fm = m + jobs[j].mipOffset, rowBytes, rows;
            MipShape(f, fm, &rowBytes, &rows);

            for (UINT r = 0; r < rows && r < rowsOf[j][m]; ++r)
                memcpy(p + feet[j][m].Offset + (size_t)r * feet[j][m].Footprint.RowPitch, data[j][fm].data() + (size_t)r * rowBytes,
                       rowBytes);
        }
    }

    upload->Unmap(0, NULL);

    ID3D12CommandAllocator* allocator = NULL;
    ID3D12GraphicsCommandList* list = NULL;
    ID3D12Fence* fence = NULL;
    HANDLE done = CreateEventW(NULL, FALSE, FALSE, NULL);
    bool finished = false;
    ++t_ours;

    if (SUCCEEDED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
        SUCCEEDED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, IID_PPV_ARGS(&list))) &&
        SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        std::vector<D3D12_RESOURCE_BARRIER> to, back;

        for (Job& j : jobs)
            if (j.state != D3D12_RESOURCE_STATE_COMMON && j.state != D3D12_RESOURCE_STATE_COPY_DEST)
            {
                D3D12_RESOURCE_BARRIER b = {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = j.resource;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = j.state;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                to.push_back(b);
                std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                back.push_back(b);
            }

        if (!to.empty())
            list->ResourceBarrier((UINT)to.size(), to.data());

        for (size_t j = 0; j < jobs.size(); ++j)
            for (UINT m = 0; m < jobs[j].resourceMips && m + jobs[j].mipOffset < data[j].size(); ++m)
            {
                if (!(jobs[j].mips & (1u << m)))
                    continue;

                D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
                dst.pResource = jobs[j].resource;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = m;
                src.pResource = upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = feet[j][m];
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
            }

        if (!back.empty())
            list->ResourceBarrier((UINT)back.size(), back.data());

        list->Close();
        ID3D12CommandList* lists[] = { list };
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(fence, 1);
        fence->SetEventOnCompletion(1, done);
        finished = WaitForSingleObject(done, 10000) == WAIT_OBJECT_0;
    }

    --t_ours;

    if (fence) fence->Release();
    if (list) list->Release();
    if (allocator) allocator->Release();
    CloseHandle(done);

    if (finished)
        upload->Release();
    else
        Log("gpu: an upload did not finish in 10 s (its buffer is kept)");
}

// Brings every tracked texture holding another season to `season`.
static void Bring(Season season)
{

    // What to change, with a reference each so nothing goes away meanwhile.
    std::vector<Job> all;
    AcquireSRWLockExclusive(&g_lock);

    for (auto& [res, t] : g_tracked)
    {
        const Texture& tex = g_textures[t.texture];
        int file = tex.file[season] >= 0 ? tex.file[season] : tex.file[SUMMER];

        if (file < 0 || g_files[file].ref.season == t.season || !t.mips)
            continue;

        res->AddRef();
        all.push_back({ res, file, t.mips, t.resourceMips, t.mipOffset, (D3D12_RESOURCE_STATES)t.state });
        t.season = g_files[file].ref.season;
    }

    ReleaseSRWLockExclusive(&g_lock);

    DWORD started = GetTickCount();
    HANDLE h = INVALID_HANDLE_VALUE;
    std::wstring path;
    std::vector<uint8_t> file;
    std::vector<Job> batch;
    std::vector<std::vector<std::vector<uint8_t>>> data;
    size_t bytes = 0, done = 0;

    for (size_t i = 0; i <= all.size(); ++i)
    {
        if (i < all.size())
        {
            std::vector<std::vector<uint8_t>> mips;

            if (ReadStored(g_files[all[i].file].ref, &h, &path, &file) && Unpack(g_files[all[i].file], file, &mips))
            {
                for (auto& m : mips)
                    bytes += m.size();

                batch.push_back(all[i]);
                data.push_back(std::move(mips));
            }
        }

        // Batches of about 64 MB.
        if (!batch.empty() && (bytes > (64u << 20) || i == all.size()))
        {
            Upload(batch, data);
            done += batch.size();
            batch.clear();
            data.clear();
            bytes = 0;
        }
    }

    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);

    for (Job& j : all)
        j.resource->Release();

    if (done)
        Log("gpu: %s - %zu textures on the graphics card changed in %lu ms (%zu known, %ld freed so far)", SEASON_NAMES[season], done,
            GetTickCount() - started, g_tracked.size(), g_freed);
}

// The keeper: every second (or at once after a season change), textures
// holding another season than the current one get its version - after a
// change, and those the game loads at an awkward moment.
static DWORD WINAPI Keeper(LPVOID)
{
    for (;;)
    {
        WaitForSingleObject(g_wake, 1000);

        if (g_device && g_sigsReady)
            Bring((Season)g_target);
    }
}

void GpuSeasonChanged(Season season)
{
    InterlockedExchange(&g_target, season);

    if (g_wake)
        SetEvent(g_wake);
}

bool SeasonTextureMip(const char* group, const char* path, Season season, uint32_t mip, std::vector<uint8_t>* out, uint32_t* width, uint32_t* height)
{
    std::vector<uint8_t> file;
    std::vector<std::vector<uint8_t>> mips;
    FileInfo f;

    if (!ArchiveSeasonFileBytes(group, path, season, &file) || !Describe(file.data(), file.size(), &f) || mip >= f.mips || !Unpack(f, file, &mips))
        return false;

    *width = f.width >> mip ? f.width >> mip : 1;
    *height = f.height >> mip ? f.height >> mip : 1;
    *out = std::move(mips[mip]);
    return true;
}
