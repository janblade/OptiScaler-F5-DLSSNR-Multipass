#include "pch.h"
// Execute the production Finished Picture evaluate branch and Acquire early guards. Only
// the expensive model/copy operations are observers; a paused path must not reach any of them.
static bool paused = false;
static unsigned captures = 0, preSrModels = 0, privateDlss = 0, cancels = 0;
static void Expect(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
enum class SwapchainInteropApi { None, Other };
struct State
{
    ::SwapchainInteropApi swapchainInteropApi = ::SwapchainInteropApi::None;
    uint64_t frameCount = 100;
    static State& Instance() { static State value; return value; }
};
struct Config
{
    struct Setting { bool value_or_default() { return true; } } DlssNrEnabled;
} cfg;
namespace StreamlinePicture { bool PauseEvaluation() { return paused; } }
namespace Late
{
struct Slot {} testSlot;
void Cancel() { ++cancels; }
void Say(const char*) {}
void Capture(ID3D12GraphicsCommandList*, void*, bool) { ++captures; }
#include "FinishedAcquire.inl"
}
namespace DeferredSr
{
void Cancel() { ++cancels; }
void Before(ID3D12GraphicsCommandList*, void*, uint64_t, uint64_t, void*) { ++preSrModels; ++privateDlss; }
void After(ID3D12GraphicsCommandList*, void*, uint64_t) { ++privateDlss; }
}
struct Clock { uint64_t AtSeam(bool, bool, uint64_t submitted) { return submitted; } } g_nrSeamClock;
static unsigned finishedMode = 1;
static ID3D12CommandQueue* timingQueue = nullptr;
static bool rayReconstruction = false, beforeUpscale = true;
static auto* cmdList = reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t(1));
static void* params = reinterpret_cast<void*>(uintptr_t(2));
#include "FinishedEvaluate.inl"
int main() try
{
    for (unsigned mode : {1u, 2u})
        for (bool before : {false, true})
        {
            finishedMode = mode; beforeUpscale = before; paused = true;
            const auto cancelBefore = cancels;
            Evaluate();
            Expect(!captures && !preSrModels && !privateDlss && cancels == cancelBefore + 2,
                   "Unregistered native FG executed capture/NR/private DLSS before Present discarded it");
        }
    Expect(!Late::Acquire(cmdList), "Capture Acquire bypassed unregistered native-FG pause");
    paused = false;
    Expect(Late::Acquire(cmdList) == &Late::testSlot, "Registered/FG-off capture remained paused");
    beforeUpscale = true; finishedMode = 1; Evaluate();
    Expect(captures == 1, "Registered/FG-off Finished Picture stopped capturing");
    finishedMode = 2; Evaluate();
    Expect(preSrModels == 1 && privateDlss == 1, "Registered/FG-off deferred model path stopped running");
    beforeUpscale = false; Evaluate();
    Expect(privateDlss == 2, "Registered/FG-off deferred after seam stopped running");
    std::cout << "PASS: production evaluate and capture gate; unregistered FG has zero NR/private-DLSS/copy calls\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
