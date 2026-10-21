#pragma once

// Kernel-variant census and per-group GPU timing of NVIDIA's DLSS-NR model, taken from the NvAPI launches.
//
// The DLL launches every kernel of one evaluation through NvAPI_D3D12_LaunchCuKernelChain on the game's command list, and the wrapper
// in DlssNrNative.cpp sees each call. A timestamp is recorded before the first launch of an evaluation and after every launch call,
// the pairs are resolved into a readback buffer, and the deltas are attributed to the kernel group of the call that ended them.
// Read back at least 90 evaluations later, when the command list has long since retired, so no fence is needed.
//
// What it answers, on any GPU: which kernel set runs (fp8-named or the plain fp16 ones, and how many of each), and where the GPU time
// of one evaluation goes. Approximate on purpose: kernels of a chain wait on counters and can overlap, so a timestamp between two calls
// marks when the earlier work reached it, not an isolated kernel time. Read the percentages as a ranking, not as exact milliseconds.
//
// Off unless asked for (ini [DlssNr] KernelProfile). It samples 3 consecutive evaluations out of every 240, a few dozen timestamps
// per launch and one small readback each, and changes nothing that is launched.

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace DlssNrKernelProfile
{
constexpr const char* kGroups[] = { "pre_block", "post_block", "swin_1h_32", "swin_2h_64", "swin_4h_128", "swin_8h_256",
                                    "split_swin_16h", "vit_1d", "cg2r_post_process", "cg2r_copy", "dec_input_upsample",
                                    "cb_clear", "vit_copy", "other" };
constexpr unsigned kGroupCount = (unsigned) (sizeof(kGroups) / sizeof(kGroups[0]));

struct Info
{
    unsigned char group = kGroupCount - 1;
    bool fp8 = false; // the name carries the _fp8 suffix (the RTX 50 fp8 kernels); the plain ones are the fp16 kernels
    std::string name;
};

inline Info Classify(const char* name)
{
    Info info;
    info.name = name != nullptr ? name : "";
    info.fp8 = strstr(info.name.c_str(), "_fp8") != nullptr;

    struct Rule { const char* part; unsigned char group; };
    // pre/post block names also contain swin_1h_32, and split_swin names contain swin, so the order matters
    static const Rule rules[] = { { "nr_vit_copy", 12 }, { "pre_block", 0 },    { "post_block", 1 },        { "split_swin", 6 },
                                  { "cc_vit_1d", 7 },   { "cg2r_post_process", 8 }, { "cg2r_copy", 9 },
                                  { "dec_input_upsample", 10 }, { "cc_cb_clear", 11 }, { "swin_1h_32", 2 },
                                  { "swin_2h_64", 3 },  { "swin_4h_128", 4 },      { "swin_8h_256", 5 } };

    for (const Rule& r : rules)
    {
        if (strstr(info.name.c_str(), r.part) != nullptr)
        {
            info.group = r.group;
            break;
        }
    }

    return info;
}

class Profiler
{
  public:
    bool Recording(const ID3D12GraphicsCommandList* cmd) const { return recording_ >= 0 && slots_[recording_].cmd == cmd; }

    // Start of one model evaluation on `cmd`. Also collects any finished samples.
    void Begin(ID3D12GraphicsCommandList* cmd, bool want)
    {
        ++evaluation_;
        Collect();
        recording_ = -1;

        if (!want || failed_ || cmd == nullptr || (evaluation_ % 240) >= 3)
            return;

        if (!Init(cmd))
            return;

        for (unsigned i = 0; i < kSlots; ++i)
        {
            Slot& s = slots_[i];

            if (s.pending)
                continue;

            s = Slot { s.heap, s.readback };
            s.cmd = cmd;
            s.evaluation = evaluation_;
            cmd->EndQuery(s.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s.queries++);
            recording_ = (int) i;
            return;
        }
    }

    // A launch call was recorded on `cmd`: `infos` are its kernels.
    void Launched(ID3D12GraphicsCommandList* cmd, const std::vector<Info>& infos)
    {
        if (!Recording(cmd) || infos.empty())
            return;

        Slot& s = slots_[recording_];

        if (s.queries >= kCap)
        {
            s.truncated = true;
            return;
        }

        Rec rec;
        rec.timeGroup = infos.front().group;

        for (const Info& info : infos)
        {
            ++rec.kernels[info.group];
            s.fp8Kernels += info.fp8;
            s.plainKernels += !info.fp8;

            if (info.fp8 && s.fp8Example.empty())
                s.fp8Example = info.name;
            else if (!info.fp8 && s.plainExample.empty())
                s.plainExample = info.name;
        }

        s.recs.push_back(std::move(rec));
        cmd->EndQuery(s.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s.queries++);
    }

    // End of the evaluation on `cmd`: resolve what was recorded.
    void End(ID3D12GraphicsCommandList* cmd)
    {
        if (!Recording(cmd))
            return;

        Slot& s = slots_[recording_];
        cmd->ResolveQueryData(s.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, s.queries, s.readback.Get(), 0);
        s.pending = true;
        s.queuedAt = evaluation_;
        recording_ = -1;
    }

    std::vector<std::string> TakeReports()
    {
        std::vector<std::string> out;
        out.swap(reports_);
        return out;
    }

  private:
    static constexpr unsigned kSlots = 6;
    static constexpr unsigned kCap = 1024;

    struct Rec
    {
        unsigned char timeGroup = 0;
        unsigned kernels[kGroupCount] = {};
    };

    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        ID3D12GraphicsCommandList* cmd = nullptr; // identity only
        std::vector<Rec> recs;
        unsigned queries = 0;
        unsigned fp8Kernels = 0;
        unsigned plainKernels = 0;
        bool pending = false;
        bool truncated = false;
        unsigned long long queuedAt = 0;
        unsigned long long evaluation = 0;
        std::string fp8Example;
        std::string plainExample;
    };

    bool Init(ID3D12GraphicsCommandList* cmd)
    {
        if (ready_)
            return true;

        Microsoft::WRL::ComPtr<ID3D12Device> device;

        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))))
        {
            failed_ = true;
            return false;
        }

        // The timestamp clock is the same for every direct queue of a device, so a throwaway one is enough to read it.
        D3D12_COMMAND_QUEUE_DESC desc {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;

        if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) || FAILED(queue->GetTimestampFrequency(&frequency_)) ||
            frequency_ == 0)
        {
            failed_ = true;
            return false;
        }

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = (UINT64) kCap * sizeof(UINT64);
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_QUERY_HEAP_DESC queryDesc { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kCap, 0 };

        for (Slot& s : slots_)
        {
            if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&s.heap))) ||
                FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr, IID_PPV_ARGS(&s.readback))))
            {
                failed_ = true;
                return false;
            }
        }

        ready_ = true;
        return true;
    }

    void Collect()
    {
        for (Slot& s : slots_)
        {
            if (!s.pending || evaluation_ < s.queuedAt + 90)
                continue;

            s.pending = false;
            UINT64* data = nullptr;
            D3D12_RANGE range { 0, (SIZE_T) s.queries * sizeof(UINT64) };

            if (FAILED(s.readback->Map(0, &range, (void**) &data)) || data == nullptr)
                continue;

            std::vector<UINT64> ticks(data, data + s.queries);
            D3D12_RANGE none { 0, 0 };
            s.readback->Unmap(0, &none);
            Report(s, ticks);
        }
    }

    void Report(const Slot& s, const std::vector<UINT64>& ticks)
    {
        if (ticks.size() < 2 || ticks.front() == 0 || ticks.back() < ticks.front() || s.recs.size() + 1 != ticks.size())
            return; // the command list never ran, or the readback is not ours

        double groupMs[kGroupCount] = {};
        unsigned groupKernels[kGroupCount] = {};

        for (size_t i = 0; i < s.recs.size(); ++i)
        {
            const double ms = ticks[i + 1] >= ticks[i] ? (double) (ticks[i + 1] - ticks[i]) * 1000.0 / (double) frequency_ : 0.0;
            groupMs[s.recs[i].timeGroup] += ms;

            for (unsigned g = 0; g < kGroupCount; ++g)
                groupKernels[g] += s.recs[i].kernels[g];
        }

        const double totalMs = (double) (ticks.back() - ticks.front()) * 1000.0 / (double) frequency_;
        std::vector<unsigned> order(kGroupCount);

        for (unsigned g = 0; g < kGroupCount; ++g)
            order[g] = g;

        std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) { return groupMs[a] > groupMs[b]; });

        char head[512];
        snprintf(head, sizeof head,
                 "DLSS-NR kernel profile #%llu (evaluation %llu): %.2f ms GPU over %zu launch calls, %u kernels: %u fp8-named, "
                 "%u plain (fp16)%s. e.g. fp8: %s | plain: %s | ",
                 (unsigned long long) ++reportId_, s.evaluation, totalMs, s.recs.size(), s.fp8Kernels + s.plainKernels, s.fp8Kernels,
                 s.plainKernels, s.truncated ? ", TRUNCATED" : "", s.fp8Example.empty() ? "none" : s.fp8Example.c_str(),
                 s.plainExample.empty() ? "none" : s.plainExample.c_str());

        std::string line = head;

        for (unsigned g : order)
        {
            if (groupKernels[g] == 0)
                continue;

            char part[96];
            snprintf(part, sizeof part, "%s %.2f ms (%.0f%%, x%u)  ", kGroups[g], groupMs[g],
                     totalMs > 0.0 ? 100.0 * groupMs[g] / totalMs : 0.0, groupKernels[g]);
            line += part;
        }

        line += "(approximate: chained kernels overlap)";
        reports_.push_back(std::move(line));
    }

    Slot slots_[kSlots];
    int recording_ = -1;
    unsigned long long evaluation_ = 0;
    unsigned long long reportId_ = 0;
    UINT64 frequency_ = 0;
    bool ready_ = false;
    bool failed_ = false;
    std::vector<std::string> reports_;
};
} // namespace DlssNrKernelProfile
