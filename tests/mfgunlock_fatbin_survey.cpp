// Read-only survey of the CUDA fatbin containers in a provider file: per container, every image's kind, architecture,
// size and whether it is stored compressed, plus the PTX ".target" / ".version" and any text of interest.
// usage: mfgunlock_fatbin_survey <file> [-v]
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const bool verbose = argc > 2 && std::strcmp(argv[2], "-v") == 0;

    const unsigned char magic[] = { 0x50, 0xED, 0x55, 0xBA };
    std::map<std::string, unsigned> shapes;
    unsigned containers = 0, plainPtx = 0, packedPtx = 0, plain120 = 0, ptxOnly89 = 0;
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
        std::string shape;
        bool has120 = false, has89ptx = false, has89cubin = false;
        while (q + 0x40 <= end)
        {
            uint16_t kind;
            uint32_t ehs, csz, arch;
            uint64_t psz, dsz;
            std::memcpy(&kind, &d[q], 2);
            std::memcpy(&ehs, &d[q + 4], 4);
            std::memcpy(&psz, &d[q + 8], 8);
            std::memcpy(&csz, &d[q + 16], 4);
            std::memcpy(&arch, &d[q + 0x1c], 4);
            std::memcpy(&dsz, &d[q + 0x38], 8);
            if (ehs < 0x20 || q + ehs + psz > end)
                break;
            // A payload is plain when it starts with PTX/ELF text, not with a zstd or lz4 stream.
            const unsigned char* body = &d[q + ehs];
            const bool plainText = psz >= 8 && (std::memcmp(body, "//\n// Gen", 9) == 0 || body[0] == '/' ||
                                                body[0] == '.' || std::memcmp(body, "\x7f" "ELF", 4) == 0);
            const bool packed = !plainText;
            if (verbose)
                std::printf("  c%u kind=%u arch=%u csz=%u dsz=%llu psz=%llu first=%02x %02x %02x %02x\n", containers,
                            kind, arch, csz, static_cast<unsigned long long>(dsz),
                            static_cast<unsigned long long>(psz), body[0], body[1], body[2], body[3]);
            const char* tag = kind == 1 ? "ptx" : kind == 2 ? "elf" : "k?";
            shape += std::string(tag) + std::to_string(arch) + (packed ? "z " : " ");
            if (kind == 1)
            {
                (packed ? packedPtx : plainPtx)++;
                if (arch == 120)
                    has120 = true;
                if (arch == 89)
                    has89ptx = true;
                if (verbose && !packed)
                {
                    const std::string text(reinterpret_cast<const char*>(&d[q + ehs]), static_cast<size_t>(psz));
                    const auto t = text.find(".target");
                    const auto v = text.find(".version");
                    std::printf("  c%u ptx%u .version=%s .target=%s size=%llu\n", containers, arch,
                                v == std::string::npos ? "?" : text.substr(v + 9, 4).c_str(),
                                t == std::string::npos ? "?" : text.substr(t + 8, 7).c_str(),
                                static_cast<unsigned long long>(psz));
                }
            }
            if (kind == 2 && arch == 89)
                has89cubin = true;
            q += ehs + psz;
        }
        if (has120)
            ++plain120;
        if (has89ptx && !has120)
            ++ptxOnly89;
        (void) has89cubin;
        ++shapes[shape];
        ++containers;
        i = end;
    }
    std::printf("containers=%u ptxPlain=%u ptxPacked=%u containersWith120Ptx=%u containersOnly89Ptx=%u\n", containers,
                plainPtx, packedPtx, plain120, ptxOnly89);
    for (const auto& [s, n] : shapes)
        std::printf("  %3u x %s\n", n, s.c_str());
    return 0;
}
