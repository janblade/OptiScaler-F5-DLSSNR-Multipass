#pragma once
#include "interpass_timing_fixture.h"
#include <functional>

// Resident synthetic stages only; excludes NR/DLSS inference and CPU setup.
inline void measure(LowFixture& f, std::ofstream& out, const char* stage, int scale,
                    const std::function<void()>& baseline, const std::function<void()>& candidate)
{
    auto& r = f.r;
    for (unsigned block = 0; block < 8; ++block)
        for (unsigned quartet = 0; quartet < 48; ++quartet)
        {
            r.begin();
            r.list->SetComputeRootSignature(r.root.Get());
            ID3D12DescriptorHeap* heaps[] = { r.heap.Get() };
            r.list->SetDescriptorHeaps(1, heaps);
            for (unsigned position = 0; position < 4; ++position)
            {
                const bool changed = (position == 1 || position == 2) != (block % 2 != 0);
                r.list->EndQuery(f.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, position * 2);
                for (unsigned repeat = 0; repeat < 4; ++repeat)
                    (changed ? candidate : baseline)();
                r.list->EndQuery(f.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, position * 2 + 1);
            }
            r.list->ResolveQueryData(f.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 8, f.times.Get(), 0);
            r.finish();
            UINT64* stamps;
            check(f.times->Map(0, nullptr, reinterpret_cast<void**>(&stamps)));
            if (quartet >= 8)
                for (unsigned position = 0; position < 4; ++position)
                {
                    const bool changed = (position == 1 || position == 2) != (block % 2 != 0);
                    const double ms =
                        double(stamps[position * 2 + 1] - stamps[position * 2]) * 1000.0 / f.frequency / 4;
                    out << stage << ',' << scale << ',' << (changed ? "candidate" : "base") << ',' << block << ','
                        << quartet - 8 << ',' << position << ',' << ms << '\n';
                }
            f.times->Unmap(0, nullptr);
        }
    out.flush();
    std::cout << stage << " P" << scale << " complete\n" << std::flush;
}
