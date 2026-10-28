#include "pch.h"
#include <mutex>
#include "../../OptiScaler/dlssnr/DlssNr_StreamlinePicture.h"
static void Expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static bool running = false, registered = false;
static unsigned sets = 0, runs = 0, skips = 0, flowResets = 0, virtualPauses = 0;
struct Setting { bool value_or_default() { return true; } };
struct Config
{
    Setting DlssNrFinishedPicture, DlssNrEnabled;
    static Config* Instance() { static Config value; return &value; }
};
namespace DlssNr
{
void NativeInputSkipped() { ++skips; }
void FinishedPictureUnavailable(const char*) {}
namespace StreamlinePicture
{
bool Owns(IDXGISwapChain*) { return registered; }
bool NativeFgRunning() { return running; }
}
}
struct Producer { void Reset() { ++flowResets; } } producer;
struct Driver
{
    enum class Status { Off, PausedForNativeFg };
    Status _status = Status::Off;
    bool _nativeRan = true;
    Producer* _producer = &producer;
    void PauseVirtualUpscaler() { ++virtualPauses; }
    void PauseForNativeFg();
    bool Enabled() { return true; }
    bool Failed() { return false; }
    template<class S> void RunFrame(S&, int, int) { ++runs; }
} g_driver;
struct Source { void SetPresent(IDXGISwapChain*, ID3D12CommandQueue*, ID3D12Device*) { ++sets; } } g_source;
struct Ui { int TakeRequest() { return 0; } int View() { return 0; } } g_ui;
#include "FinishedNativePause.inl"
#include "FinishedNativeDriver.inl"
std::recursive_mutex g_nrMutex;
namespace DlssNr
{
namespace Late { void Say(const char*) {} }
#include "FinishedNativeEntry.inl"
}
int main() try
{
    // These pointers are identity-only: guarded paths must return before any COM call. The
    // ordinary path ends at source/producer seams; it does not run optical flow or inference.
    auto* chain = reinterpret_cast<IDXGISwapChain*>(uintptr_t(1));
    auto* queue = reinterpret_cast<ID3D12CommandQueue*>(uintptr_t(2));
    auto* device = reinterpret_cast<ID3D12Device*>(uintptr_t(3));
    for (bool owns : {false, true})
    {
        running = true; registered = owns;
        RunFrame(chain, queue, device);
        Expect(runs == 0 && sets == 0, "Native driver fed generated/lower presents into optical flow/NR");
    }
    Expect(flowResets == 1 && virtualPauses == 1 && !g_driver._nativeRan,
           "Native FG pause kept stale flow history or reset it on every generated frame");
    running = false; registered = true;
    RunFrame(chain, queue, device);
    Expect(runs == 1 && sets == 1 && skips == 2, "FG-off registered chain did not recover ordinary native input");
    registered = false;
    RunFrame(chain, queue, device);
    Expect(runs == 2 && sets == 2, "Ordinary FG-off native input did not recover");
    auto* cmd = reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t(4));
    auto* picture = reinterpret_cast<ID3D12Resource*>(uintptr_t(5));
    for (bool virtualInput : {false, true})
    {
        running = true;
        Expect(!DlssNr::ApplyFinishedPictureInput(queue, cmd, picture, nullptr, picture, nullptr, false, false,
                   DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, D3D12_RESOURCE_STATE_PRESENT, 100, virtualInput),
               "Native/virtual entry bypassed native-FG guard");
        running = false;
        Expect(DlssNr::ApplyFinishedPictureInput(queue, cmd, picture, nullptr, picture, nullptr, false, false,
                   DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, D3D12_RESOURCE_STATE_PRESENT, 101, virtualInput),
               "FG-off native/virtual entry did not pass its early guards");
    }
    std::cout << "PASS: production native driver and native/virtual input guards; no generated-frame optical flow/NR\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
