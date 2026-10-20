#pragma once

#include <xell.h>

#include <low_latency/low_latency_tech/low_latency_tech.h>
#include <Config.h>
#include <nvapi/NvApiTypes.h>

#include "OutputRule.h"

enum class InputMarkerMode
{
    NoMarkers,
    SimStartOnly,            // AntiLag 2, its sleep marks the simulation start
    SimStartAndPresentStart, // AntiLag 2 with frame generation
    SimOnly,                 // Start and End
    FullMarkers,             // TODO: add something about xell's partial async markers
};

struct InputContext
{
    LowLatencyInput caller;
    bool localContext; // input created by Opti
    bool noFrameId;
    InputMarkerMode markerMode;
    API api; // pDevice's API, NotSelected for a D3D11 or D3D12 device, told apart by the device
};

enum class InputResult : uint32_t
{
    Ok,
    UsingDifferentInput,
    InputNotSupported,
    InvalidParameter,
    LowLatencyUpdateFail,
    NotEnoughReports,
    NoReadyOutput,
    GenericError,
};

class ReflexVk;
class ReflexNvApiVk;

// Completes the type in every file that includes this one: one that only declares the members below would give them
// a weaker alignment than input_common.cpp (LTCG warning C4744), and the linker may keep that one
static_assert(sizeof(std::atomic<std::shared_ptr<LowLatencyTech>>) > 0);

struct TimingData
{
    std::optional<TimingEntry> timeRange; // in ns, value stored in length
    std::optional<TimingEntry> simulation;
    std::optional<TimingEntry> renderSubmit;
    std::optional<TimingEntry> present;
    std::optional<TimingEntry> driver;
    std::optional<TimingEntry> osRenderQueue;
    std::optional<TimingEntry> gpuRender;
};

class InputCommon
{
    inline static std::atomic<std::shared_ptr<LowLatencyTech>> currently_active_tech;
    inline static std::mutex create_tech_mutex {};

    // OptiScaler's XeFG paces with an XeLL context of ours (acquire_xefg_xell). While XeLL lowers latency it is the
    // output itself; next to a Reflex or AntiLag 2 output it stays XeFG's with low latency off (forced mode: no sleeps,
    // no markers) while the output does the sleeping.
    inline static std::atomic<std::shared_ptr<LowLatencyTech>> xefg_xell;

    // The frame reports of the input's markers: the game's threads write them, the overlay and GetLatency read them
    inline static FrameReport frame_reports[FRAME_REPORTS_BUFFER_SIZE] {};
    inline static std::mutex report_mutex {};
    inline static std::array<std::atomic_uint64_t, 6> last_marker_frame_ids {}; // Simulation start to present end
    inline static std::atomic<LowLatencyMode> failed_output = LowLatencyMode::None;
    inline static std::atomic<std::chrono::steady_clock::rep> failed_time = 0; // steady_clock ticks
    // The device it failed on: on Vulkan what the device has doesn't change, it isn't tried again there
    inline static std::atomic<IUnknown*> failed_device = nullptr;
    inline static std::atomic_uint64_t last_present_start_frame_id = 0;
    inline static std::atomic_uint32_t delay_deinit = 0;
    inline static std::array<SleepMode, static_cast<size_t>(LowLatencyInput::_)> sleep_mode_copies {};
    inline static std::mutex sleep_copy_mutex {};
    inline static std::atomic_uint32_t fps_limit_interval_us = 0; // OptiScaler's FPS limit for the output, 0 = none
    // The present each input last reported the game's own frame generation on (async markers), 0 = never
    inline static std::atomic_uint64_t present_count = 0;
    inline static std::array<std::atomic_uint64_t, static_cast<size_t>(LowLatencyInput::_)> last_fg_present {};
    // OptiScaler's XeFG's present queue for its out of band markers (referenced), and the game frame id of the batch
    // it last marked
    inline static std::atomic<ID3D12CommandQueue*> xefg_present_queue = nullptr;
    inline static std::atomic_uint64_t xefg_frame_id = 0;
    // Time the low latency output slept since XeFG's Dynamic MFG last asked (the frame time the game doesn't need)
    inline static std::atomic<double> slept_ms = 0.0;

    static void mark_frame_generation(LowLatencyInput input)
    {
        last_fg_present[static_cast<size_t>(input)] = present_count + 1;
    }

    static bool reports_frame_generation(LowLatencyInput input)
    {
        auto last = last_fg_present[static_cast<size_t>(input)].load();
        return last != 0 && present_count + 1 - last < 20;
    }

    static bool xefg_paced();

    // Each input's calls come from the game's threads, the UE mod's tick and OptiScaler's present: one bit per input
    inline static std::atomic<uint32_t> available_inputs = 0;
    inline static std::atomic<LowLatencyInput> activeInput = LowLatencyInput::None;
    inline static std::atomic<LowLatencyMode> activeOutput = LowLatencyMode::None;

    static bool available(LowLatencyInput input)
    {
        return (available_inputs.load(std::memory_order_relaxed) & (1u << static_cast<uint32_t>(input))) != 0;
    }
    inline static bool enabled = false;

    // The API of each input's device, the output runs on the active input's
    inline static std::array<std::atomic<API>, static_cast<size_t>(LowLatencyInput::_)> input_api {};
    inline static std::atomic<API> output_api = API::NotSelected;

    // OptiScaler's own low latency (native/NativeLowLatency.cpp) runs: its calls are the OptiScaler input while it
    // does. Not a lasting availability like the game's: it stops and starts.
    inline static std::atomic_bool optiscaler_running = false;
    // The inputs that have set a sleep mode, one bit each (a game may only mark frames)
    inline static std::atomic<uint32_t> sleep_modes_set = 0;
    // XeFG's XeLL contexts made so far (a new one gets its sleep mode again)
    inline static std::atomic<uint32_t> xefg_generation = 0;
    // The device the output was started on (null: XeFG's XeLL, or none)
    inline static std::atomic<IUnknown*> output_device = nullptr;
    // Anti-Lag 2 or Reflex could not start next to XeFG's XeLL: XeLL keeps lowering latency
    // (native::lowlatency::XefgSide)
    inline static std::atomic_bool antilag2_failed = false;
    inline static std::atomic_bool reflex_failed = false;
    // The XeFG side OptiScaler's own sleep mode was planned for (its cap counts the side's frames)
    inline static std::atomic<lloutput::XefgLatency> optiscaler_mode_side = lloutput::XefgLatency::XeLL;
    // The VK_NV_low_latency2 output can't go on (OutputRule.h ChooseReflexVk): made again on the next call. Once per
    // frame (update), cleared when the output stops.
    inline static std::atomic_bool reflex_vk_stale = false;

    // An Anti-Lag 2 output FSR FG may still use: fakenvapi::reportFGPresent hands its context to FSR through the swap
    // chain on every present, so it is deinited a few presents after it stopped (retire, update). create_tech_mutex.
    inline static std::shared_ptr<LowLatencyTech> retired_antilag2 {};
    inline static std::atomic_uint32_t retired_presents = 0;
    static void retire(std::shared_ptr<LowLatencyTech> tech);

    static bool deinit_current_tech();
    static bool init_tech(IUnknown* pDevice, API api, LowLatencyMode desiredMode, const lloutput::OutputPlan& plan);
    static LowLatencyMode to_mode(lloutput::Tech tech);
    // `api`: the call's device's (XeLL is D3D12's)
    static lloutput::OutputPlan plan_output(API api, IUnknown* pDevice);
    static void use_output(std::shared_ptr<LowLatencyTech> tech);
    static SleepMode shaped_sleep_mode(lloutput::Role role);
    static void apply_xefg_xell_mode(const LowLatencyTech* output);
    static bool update_low_latency_tech(IUnknown* pDevice, API api, std::optional<LowLatencyMode> mode = std::nullopt);
    // The same for an input's call, which also marks the input as available unless it only asks (`query`: GetLatency,
    // GetSleepStatus). A game that only reads latency runs no low latency of its own: it must not push OptiScaler's
    // calls aside (default_input), and nobody would sleep (NBA 2K27 reads it for seconds before its first marker).
    static bool update_for_call(const InputContext& inputContext, IUnknown* pDevice, bool query = false);
    static void add_marker_to_report(const MarkerParams& marker_params);
    static bool copy_frame_reports(FrameReport* reports); // NVAPI_BUFFER_SIZE reports, oldest first
    static void clear_reports();

    // The API of the call's device
    static API device_api(const InputContext& inputContext, IUnknown* pDevice);

    // Marks the game's input as available (not for a `query`), returns the call's API
    static API register_call(const InputContext& inputContext, IUnknown* pDevice, bool query = false);

    // The output that does the same on the call's API: AntiLag Vk on Vulkan, AntiLag 2 on D3D, XeLL is D3D12 only
    static LowLatencyMode for_api(LowLatencyMode mode, API api);
    static SleepMode get_sleep_copy(LowLatencyInput input)
    {
        std::scoped_lock lock(sleep_copy_mutex);
        return sleep_mode_copies[static_cast<size_t>(input)];
    }

    static void set_sleep_copy(LowLatencyInput input, const SleepMode& mode)
    {
        std::scoped_lock lock(sleep_copy_mutex);
        sleep_mode_copies[static_cast<size_t>(input)] = mode;
    }
    static void apply_sleep_mode(LowLatencyTech* tech);

  public:
    static InputResult set_low_latency_tech(IUnknown* pDevice, LowLatencyMode mode);

    static InputResult sleep(const InputContext& inputContext, IUnknown* pDevice,
                             std::optional<uint32_t> frame_id = std::nullopt);
    // The time the output slept since the last call, 0 if it didn't (XeFG's Dynamic MFG)
    static double take_sleep_ms() { return slept_ms.exchange(0.0); }
    // toOutput false only tracks the marker, the output gets it another way
    static InputResult set_marker(const InputContext& inputContext, IUnknown* pDevice,
                                  const MarkerParams& marker_params, bool toOutput = true);
    static InputResult set_async_marker(const InputContext& inputContext, ID3D12CommandQueue* pCommandQueue,
                                        const MarkerParams& marker_params);
    static InputResult set_sleep_mode(const InputContext& inputContext, IUnknown* pDevice, SleepMode* sleep_mode);
    static InputResult get_sleep_status(const InputContext& inputContext, IUnknown* pDevice, SleepParams* sleep_params);
    static InputResult
    get_latency(const InputContext& inputContext, IUnknown* pDev,
                void* latency_params); // NV_LATENCY_RESULT_PARAMS* for reflex, xell_frame_report_t* for xell,
    // The reports of the markers OptiScaler tracked, NVAPI_BUFFER_SIZE of them, oldest first
    static InputResult get_frame_reports(const InputContext& inputContext, IUnknown* pDev, FrameReport* reports);
    static bool get_timing_data(TimingData& timingDataOut);
    static uint64_t get_last_present_start_frame_id() { return last_present_start_frame_id; };
    static LowLatencyMode active_output() { return activeOutput; }

    // The active input's API, the swapchain's without one
    static API uses_api();
    static InputResult mark_present_start(IUnknown* pDevice);

    // Stops the output; the next call of the active input starts it again. `release` false lets go of it without a call
    // to its device, for a device that is gone.
    static void stop_output(bool release = true);

    // Once per frame of every game (ReflexHooks::update): Force State, and who keeps OptiScaler's frame cap
    // (OutputRule.h CapForGame) with the rate it goes in at.
    static void update();

    // Around each of OptiScaler's XeFG's presents (wrapped swap chain): next to a Reflex or AntiLag 2 output, out of
    // band present markers with the frame id of the batch XeFG marked, as DLSSG sends them; AntiLag 2 takes the frame
    // type from them
    static void fg_output_present(ID3D12CommandQueue* presentQueue, bool presented);

    // XeFG's own XeLL markers (from the XeLL hook): 80860000 and 80860001 around the interpolation work it submits in
    // its proxy swapchain's Present, with the game's last present start frame id. DLSSG's out of band render submit
    // markers for a Reflex or AntiLag 2 output next to XeFG.
    static void xefg_marker(void* xellContext, uint32_t frame_id, uint32_t marker);

    // Around the game's present on XeFG's proxy swapchain: the batch XeFG marked during it (a new frame id) and how
    // many frames XeFG sends to its presenter for it (xefgSwapChainGetLastPresentStatus)
    static uint64_t xefg_marked_frame_id() { return xefg_frame_id; }
    static void xefg_present_status(uint64_t frameIdBefore, uint32_t framesPresented);

    // The input whose calls get through (OutputRule.h ChooseInput)
    static LowLatencyInput default_input();

    // OptiScaler's own low latency starts or stops being an input (native::lowlatency)
    static void set_optiscaler_running(bool running);

    // The game's device is being released: an output on it stops, without a call to it unless it is still `alive` (a
    // VkDevice before vkDestroyDevice: the output's own objects on it go first)
    static void device_released(IUnknown* pDevice, bool alive = false);

    // The output's own context (an Anti-Lag 2 context for FSR FG, the XeLL one), null without one
    static void* output_context();
    // The output lowers latency now (Force State included)
    static bool output_enabled();
    // What lowers latency for the menu and the logs: InputCommon's output (None without one)
    static LowLatencyMode current_mode();
    static bool low_latency_active();
    // The output's latency report (the driver's for Reflex, `estimated` false; the inputs' markers for the others)
    static NvAPI_Status output_latency(NV_LATENCY_RESULT_PARAMS* params, bool* estimated);
    // OptiScaler's FSR FG presents a frame (fakenvapi::reportFGPresent): the output knows frame generation runs and,
    // for Anti-Lag 2 without FSR's own context, whether this one is generated
    static void report_fg_present(bool fgState, bool interpolated);
    static bool antilag2_could_not_start() { return antilag2_failed.load(); }
    static bool reflex_could_not_start() { return reflex_failed.load(); }

    // XeFG's XeLL contexts made so far: a new one needs the sleep mode again
    static uint32_t xefg_xell_generation() { return xefg_generation.load(); }

    // The GPU vendor's own low latency output
    static LowLatencyMode default_output(API api);

    // Why the GPU can't run the output, nullptr when it can: Reflex needs an Nvidia GPU (and its nvapi on D3D),
    // AntiLag 2 an AMD GPU
    static const char* output_unavailable(LowLatencyMode output, API api);

    // Why the input can't drive the output, nullptr when it can
    static const char* incompatibility(LowLatencyInput input, LowLatencyMode output);

    // Set when the frame generation in use decides the low latency output or input: the game's XeFG keeps its own
    // XeLL
    static std::optional<LowLatencyMode> forced_output();
    static std::optional<LowLatencyInput> forced_input();

    // The XeLL output's context, for OptiScaler's XeFG
    static xell_context_handle_t xell_output_context();

    // The Vulkan Reflex output on VK_NV_low_latency2, for passing the game's own calls of it through
    static std::shared_ptr<ReflexVk> reflex_vk_output();
    // The Vulkan Reflex output on NVIDIA's NvAPI Vulkan Reflex (the game's out of band queue)
    static std::shared_ptr<ReflexNvApiVk> reflex_nvapi_vk_output();

    // The XeLL context OptiScaler's XeFG paces with (see xefg_xell), released after XeFG's context is destroyed
    static xell_context_handle_t acquire_xefg_xell(IUnknown* pDevice);
    static void release_xefg_xell();
};
