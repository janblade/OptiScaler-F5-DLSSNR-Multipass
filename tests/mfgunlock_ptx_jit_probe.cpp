// Host-side check for the RTX 20/30 kernel retarget. For every CUDA fatbin container in a provider file it takes the
// PTX the runtime would hand to the driver (the Blackwell PTX where the container has one, else the Ada PTX),
// decompresses it, rewrites ".target" to the requested architecture and asks this PC's CUDA driver to JIT it. PTX is
// forward compatible, so a driver on a newer GPU accepts PTX written for sm_86 or sm_75 and rejects it only when it
// uses an instruction that architecture does not have, which is exactly the question for RTX 30 / RTX 20.
// usage: mfgunlock_ptx_jit_probe <file> <arch> [-v] [-dump <dir>] [-rewrite75]   (arch = 75 | 86 | 89 | 120)
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using CUresult = int;
using CUdevice = int;
using CUcontext = void*;
using CUmodule = void*;
constexpr int CU_JIT_ERROR_LOG_BUFFER = 5;
constexpr int CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES = 6;

bool Lz4Decompress(const unsigned char* src, size_t srcSize, std::vector<unsigned char>& out, size_t outSize)
{
    out.assign(outSize, 0);
    size_t i = 0, o = 0;
    while (i < srcSize)
    {
        const unsigned token = src[i++];
        size_t literals = token >> 4;
        if (literals == 15)
        {
            unsigned char b;
            do
            {
                if (i >= srcSize)
                    return false;
                b = src[i++];
                literals += b;
            } while (b == 255);
        }
        if (i + literals > srcSize || o + literals > outSize)
            return false;
        std::memcpy(&out[o], src + i, literals);
        i += literals;
        o += literals;
        if (i >= srcSize)
            break;
        if (i + 2 > srcSize)
            return false;
        const size_t offset = src[i] | (src[i + 1] << 8);
        i += 2;
        size_t match = token & 15;
        if (match == 15)
        {
            unsigned char b;
            do
            {
                if (i >= srcSize)
                    return false;
                b = src[i++];
                match += b;
            } while (b == 255);
        }
        match += 4;
        if (offset == 0 || offset > o || o + match > outSize)
            return false;
        for (size_t k = 0; k < match; ++k, ++o)
            out[o] = out[o - offset];
    }
    return o == outSize;
}

// Mirrors the min/max/cvt/mma rewrites sm_75 needs, so the probe can show what a rewrite would have to cover.
std::string ReplaceAll(std::string text, const std::string& from, const std::string& to)
{
    for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size())
        text.replace(at, from.size(), to);
    return text;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
        return 2;
    const int arch = std::atoi(argv[2]);
    bool verbose = false;
    bool inplace = false;
    const char* dumpDir = nullptr;
    for (int a = 3; a < argc; ++a)
    {
        if (!std::strcmp(argv[a], "-v"))
            verbose = true;
        else if (!std::strcmp(argv[a], "-inplace"))
            inplace = true;
        else if (!std::strcmp(argv[a], "-dump") && a + 1 < argc)
            dumpDir = argv[++a];
    }

    std::ifstream in(argv[1], std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    HMODULE cuda = LoadLibraryW(L"nvcuda.dll");
    if (!cuda)
    {
        std::printf("no nvcuda.dll\n");
        return 3;
    }
    auto cuInit = reinterpret_cast<CUresult(WINAPI*)(unsigned)>(GetProcAddress(cuda, "cuInit"));
    auto cuDeviceGet = reinterpret_cast<CUresult(WINAPI*)(CUdevice*, int)>(GetProcAddress(cuda, "cuDeviceGet"));
    auto cuCtxCreate =
        reinterpret_cast<CUresult(WINAPI*)(CUcontext*, unsigned, CUdevice)>(GetProcAddress(cuda, "cuCtxCreate_v2"));
    auto cuModuleLoadDataEx = reinterpret_cast<CUresult(WINAPI*)(CUmodule*, const void*, unsigned, int*, void**)>(
        GetProcAddress(cuda, "cuModuleLoadDataEx"));
    auto cuModuleUnload = reinterpret_cast<CUresult(WINAPI*)(CUmodule)>(GetProcAddress(cuda, "cuModuleUnload"));
    CUdevice device = 0;
    CUcontext context = nullptr;
    if (cuInit(0) || cuDeviceGet(&device, 0) || cuCtxCreate(&context, 0, device))
    {
        std::printf("CUDA init failed\n");
        return 3;
    }

    const unsigned char magic[] = { 0x50, 0xED, 0x55, 0xBA };
    unsigned containers = 0, ok = 0, failed = 0, skipped = 0;
    for (size_t i = 0; i + 16 <= d.size();)
    {
        if (std::memcmp(&d[i], magic, 4) != 0)
        {
            ++i;
            continue;
        }
        uint16_t hs;
        uint64_t fs;
        std::memcpy(&hs, &d[i + 6], 2);
        std::memcpy(&fs, &d[i + 8], 8);
        if (hs != 0x10 || fs == 0 || fs > d.size() - i - 16)
        {
            i += 4;
            continue;
        }
        size_t q = i + 16;
        const size_t end = q + fs;
        size_t srcOff = 0, srcHdr = 0, srcPsz = 0, srcCsz = 0, srcDsz = 0;
        int srcArch = 0;
        while (q + 0x40 <= end)
        {
            uint16_t kind;
            uint32_t ehs, csz, imgArch;
            uint64_t psz, dsz;
            std::memcpy(&kind, &d[q], 2);
            std::memcpy(&ehs, &d[q + 4], 4);
            std::memcpy(&psz, &d[q + 8], 8);
            std::memcpy(&csz, &d[q + 16], 4);
            std::memcpy(&imgArch, &d[q + 0x1c], 4);
            std::memcpy(&dsz, &d[q + 0x38], 8);
            if (ehs < 0x20 || q + ehs + psz > end)
                break;
            if (kind == 1 && (imgArch == 120 || (imgArch == 89 && srcArch != 120)))
            {
                srcOff = q;
                srcHdr = ehs;
                srcPsz = static_cast<size_t>(psz);
                srcCsz = csz;
                srcDsz = static_cast<size_t>(dsz);
                srcArch = static_cast<int>(imgArch);
            }
            q += ehs + psz;
        }
        const unsigned index = containers++;
        i = end;
        if (srcArch == 0)
        {
            ++skipped;
            continue;
        }

        std::vector<unsigned char> text;
        std::vector<unsigned char> raw(d.begin() + static_cast<ptrdiff_t>(srcOff + srcHdr),
                                       d.begin() + static_cast<ptrdiff_t>(srcOff + srcHdr + srcPsz));
        if (inplace)
        {
            // The edit the runtime would make on the compressed bytes: same length, nothing else touched.
            const std::string needle = ".target sm_";
            size_t hits = 0, hit = 0;
            for (size_t k = 0; k + needle.size() < raw.size(); ++k)
                if (std::memcmp(&raw[k], needle.c_str(), needle.size()) == 0)
                    ++hits, hit = k;
            const std::string from = srcArch == 120 ? "120" : "89";
            const std::string to = srcArch == 120 ? std::to_string(arch) + " " : std::to_string(arch);
            if (hits != 1 || to.size() != from.size() ||
                std::memcmp(&raw[hit + needle.size()], from.c_str(), from.size()) != 0)
            {
                std::printf("c%u: in-place edit not possible (raw hits=%zu)\n", index, hits);
                ++failed;
                continue;
            }
            std::memcpy(&raw[hit + needle.size()], to.c_str(), to.size());
        }
        const unsigned char* payload = raw.data();
        if (srcCsz != 0 && srcDsz != 0)
        {
            if (payload[0] == 0x28 && payload[1] == 0xB5)
            {
                std::printf("c%u: zstd payload, not handled by this probe\n", index);
                ++skipped;
                continue;
            }
            if (!Lz4Decompress(payload, srcCsz, text, srcDsz))
            {
                std::printf("c%u: lz4 decode failed\n", index);
                ++failed;
                continue;
            }
        }
        else
        {
            text.assign(payload, payload + srcPsz);
        }

        std::string ptx(reinterpret_cast<const char*>(text.data()), text.size());
        const auto nul = ptx.find('\0');
        if (nul != std::string::npos)
            ptx.resize(nul);
        const auto at = ptx.find(".target sm_");
        if (at == std::string::npos)
        {
            std::printf("c%u: no .target\n", index);
            ++failed;
            continue;
        }
        if (!inplace)
        {
            size_t until = at + 11;
            while (until < ptx.size() && ptx[until] >= '0' && ptx[until] <= '9')
                ++until;
            ptx.replace(at + 11, until - (at + 11), std::to_string(arch));
        }

        if (dumpDir)
        {
            char name[MAX_PATH];
            std::snprintf(name, sizeof(name), "%s\\c%02u_sm%d.ptx", dumpDir, index, arch);
            std::ofstream(name, std::ios::binary) << ptx;
        }

        char log[4096] = {};
        int options[] = { CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES };
        void* values[] = { log, reinterpret_cast<void*>(static_cast<uintptr_t>(sizeof(log))) };
        CUmodule module = nullptr;
        const CUresult result = cuModuleLoadDataEx(&module, ptx.c_str(), 2, options, values);
        if (result == 0)
        {
            ++ok;
            if (verbose)
                std::printf("c%u: ok (src sm_%d, %zu bytes)\n", index, srcArch, ptx.size());
            cuModuleUnload(module);
        }
        else
        {
            ++failed;
            std::printf("c%u: JIT failed %d (src sm_%d): %.300s\n", index, result, srcArch, log);
        }
    }
    std::printf("arch=%d containers=%u jitOk=%u jitFailed=%u skipped=%u\n", arch, containers, ok, failed, skipped);
    return failed == 0 ? 0 : 1;
}
