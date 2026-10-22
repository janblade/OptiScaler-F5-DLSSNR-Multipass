// Per-pass GPU benchmark; no preview/NR/UI, identical motion/depth/settings per build.
// Compile against upstream or modified motion implementation to compare algorithms.
#define main UnusedOpticalFlowRegressionMain
#include "nr_optical_flow_gpu.cpp"
#undef main
#include <map>

int main(int argc, char** argv)
{
    kWidth = argc > 1 ? uint32_t(std::atoi(argv[1])) : 2560;
    kHeight = argc > 2 ? uint32_t(std::atoi(argv[2])) : 1440;
    if (!kWidth || !kHeight) return 2;
    Gpu gpu;
    if (!gpu.Init()) return 2;
    OpticalFlowDx12 flow;
    if (!flow.Init(gpu.device.Get())) return 3;
    std::vector<ComPtr<ID3D12Resource>> pictures;
    for (int n = 0; n < 4; ++n) pictures.push_back(gpu.Picture(n*7.f, n*-3.f));
    auto depth = gpu.DepthMap(kWidth*.3125f, kHeight*.2778f, kWidth*.1953f);
    D3D12_QUERY_HEAP_DESC queryDesc { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 64, 0 };
    ComPtr<ID3D12QueryHeap> queries;
    if (FAILED(gpu.device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queries)))) return 4;
    auto readback = gpu.Buffer(64*8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 frequency = 0;
    if (FAILED(gpu.queue->GetTimestampFrequency(&frequency))) return 5;
    flow.SetTimestampHeap(queries.Get(), 64);
    std::map<std::string, double> sums;
    std::vector<double> timings;
    constexpr int warmup = 24, measured = 240;
    for (int n = 0; n < warmup + measured; ++n)
    {
        flow.ClearTimestamps();
        if (!flow.Dispatch(gpu.list.Get(), pictures[n%4].Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                           depth.Get(), DXGI_FORMAT_R32_FLOAT, true)) return 6;
        const auto count = flow.TimestampCount();
        gpu.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, count, readback.Get(), 0);
        gpu.Submit();
        if (n < warmup) continue;
        UINT64* ticks = nullptr;
        if (FAILED(readback->Map(0, nullptr, (void**)&ticks))) return 7;
        double total = 0;
        for (uint32_t i = 1; i < count; ++i)
        {
            const double ms = (ticks[i]-ticks[i-1])*1000.0/frequency;
            sums[flow.TimestampName(i)] += ms;
            total += ms;
        }
        readback->Unmap(0, nullptr);
        timings.push_back(total);
    }
    std::sort(timings.begin(), timings.end());
    double sum = 0;
    for (const auto sample : timings) sum += sample;
    printf("{\"width\":%u,\"height\":%u,\"frames\":%d,\"gpu_mean_ms\":%.6f,\"gpu_median_ms\":%.6f,\"gpu_p95_ms\":%.6f,\"passes_ms\":{",
           kWidth,kHeight,measured,sum/measured,timings[measured/2],timings[measured*95/100]);
    bool first = true;
    for (const auto& pass : sums)
    {
        printf("%s\"%s\":%.6f",first?"":",",pass.first.c_str(),pass.second/measured);
        first = false;
    }
    printf("}}\n");
    return FAILED(gpu.device->GetDeviceRemovedReason()) ? 8 : 0;
}
