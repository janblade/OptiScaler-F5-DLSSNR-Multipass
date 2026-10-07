#include "pch.h"

#include "NativeDriver.h"
#include "VirtualUpscalerDriver.h"

#include <Config.h>
#include <dlssnr/DlssNrFeature_Dx12.h>

#include <imgui/imgui.h>

namespace native
{

namespace
{
constexpr float kPreviewMaxSpeed = 24.0f; // pixels per frame that show as full brightness
} // namespace

void NativeDriver::ReleaseVirtualUpscaler()
{
    if (_virtualUpscaler != nullptr)
        _virtualUpscaler->Release();
}

void NativeDriver::PauseVirtualUpscaler()
{
    if (_virtualUpscaler != nullptr)
        _virtualUpscaler->Pause();
}

bool NativeDriver::Enabled()
{
    if (!Config::Instance()->DlssNrNativeMotion.value_or_default())
    {
        ReleaseVirtualUpscaler();
        _status = Status::Off;
        return false;
    }

    return true;
}

void NativeDriver::LogDiagnostics()
{
    if (_diagPresents % 300 != 0)
        return;

    LOG_INFO("{} frames: of {} presents, picture ready {}, waiting for the game's upscaler {}, "
             "submitted {}, flow valid {} (matched with depth {}), trust mask ran {}, native input applied {}, scene "
             "cuts {}, virtual upscaler {}",
             _tag, _diagPresents, _diagReady, _diagWaiting, _diagSubmitted, _diagFlowValid, _diagDepth, _diagTrust,
             _diagNative, _cuts, _virtualUpscaler != nullptr && _virtualUpscaler->Active() ? "active" : "not active");
}

NativeDriver::RunResult NativeDriver::RunFrame(IFrameSource& source, bool flowPreview)
{
    RunResult out;

    FrameInput input;
    const auto acquired = source.Acquire(input);

    ++_diagPresents;
    _diagReady += acquired == AcquireStatus::Ready ? 1 : 0;
    _diagWaiting += acquired == AcquireStatus::WaitingForUpscaler ? 1 : 0;

    const bool gameTookOver = _takeover.Update(acquired == AcquireStatus::WaitingForUpscaler, Util::MillisecondsNow());

    if (acquired == AcquireStatus::WaitingForUpscaler)
    {
        // The game's own upscaler call: ours pauses, and goes (with frame generation's context, as a game's feature
        // would) only once the game's calls have run for a while. A menu or cutscene that calls it for a moment would
        // otherwise cost a rebuild and a gap in frame generation each time (VirtualUpscalerHold.h).
        if (gameTookOver)
            ReleaseVirtualUpscaler();
        else
            PauseVirtualUpscaler();

        _status = Status::Waiting;

        if (_producer)
            _producer->Reset();

        LogDiagnostics();
        return out;
    }

    if (acquired != AcquireStatus::Ready)
    {
        LogDiagnostics();
        return out;
    }

    ID3D12Device* device = source.Device();

    if (_producer == nullptr || _producer->Device() != device)
    {
        auto fresh = std::make_unique<NativeProducer>();

        const double initStart = Util::MillisecondsNow();

        if (!fresh->Init(device))
        {
            _failure = fresh->Error();
            _status = Status::Failed;
            LOG_ERROR("{}: {}", _tag, _failure);
            source.Return(input, FrameOutput {});
            return out;
        }

        // The upscaler backend belongs to the old device too.
        if (_virtualUpscaler != nullptr)
        {
            delete _virtualUpscaler;
            _virtualUpscaler = nullptr;
        }

        _producer = std::move(fresh);
        LOG_INFO("{}: optical flow and trust mask ready{} (in {:.1f} ms, on the present thread)", _tag, _readyNote,
                 Util::MillisecondsNow() - initStart);
    }

    // Presents the picture to an upscaler backend as a synthetic call instead of running DLSS-NR on it. Takes priority
    // when both are on. Under a D3D11 game's frame generation swap chain (Dx11wDx12SC) it runs from OnFGPresent instead
    // of the menu's present.
    const bool useVirtualUpscaler = Config::Instance()->DlssNrNativeUpscaler.value_or_default();

    if (useVirtualUpscaler && _virtualUpscaler == nullptr)
        _virtualUpscaler = new VirtualUpscalerDriver();
    else if (!useVirtualUpscaler)
        ReleaseVirtualUpscaler();

    NativeProducer::Options options;
    options.apply = useVirtualUpscaler || Config::Instance()->DlssNrNativeInput.value_or_default();
    options.flowPreview = flowPreview;
    options.previewMaxSpeed = kPreviewMaxSpeed;

    const auto apply = [this, &source, useVirtualUpscaler](ID3D12GraphicsCommandList* cmd, const NativeFrame& frame)
    {
        if (useVirtualUpscaler)
            return _virtualUpscaler->Run(cmd, frame);

        const DXGI_COLOR_SPACE_TYPE type = frame.space == ColorSpace::ScRgb ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                           : frame.space == ColorSpace::Pq  ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                                                            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        return DlssNr::ApplyNativeInput(source.Queue(), cmd, frame.color, frame.depth, frame.motion,
                                        frame.depthReversed, frame.reset, type, frame.pictureState);
    };

    FrameOutput output;
    const auto result = _producer->Run(source.Queue(), input, options, apply, output);
    source.Return(input, output);

    if (result.stoppedAt != nullptr)
    {
        ++_stopped;

        if (_stopped == 1 || _stopped == 100 || _stopped % 1000 == 0)
            LOG_WARN("{}: the producer stopped at \"{}\" (HRESULT 0x{:X}, device removed reason 0x{:X}), "
                     "{} frames so far",
                     _tag, result.stoppedAt, (unsigned) result.stoppedHr, (unsigned) result.deviceRemoved, _stopped);
    }

    _diagFlowValid += result.flowValid ? 1 : 0;
    _diagDepth += result.flowValid && _producer->Flow() != nullptr && _producer->Flow()->UsedDepth() ? 1 : 0;
    _diagTrust += result.trustRan ? 1 : 0;
    _diagNative += result.nativeRan ? 1 : 0;
    _diagSubmitted += result.submitted ? 1 : 0;

    LogDiagnostics();

    _nativeRan = result.nativeRan;

    if (result.trustRan)
        _trustFrame = _frame;

    if (result.flowValid)
    {
        // How often the depth finder has a copy for the mask: the log shows it every 600 frames.
        ++_seen;
        _ran += result.trustRan ? 1 : 0;

        if (_seen == 600)
        {
            LOG_INFO("{}: the trust mask ran in {} of {} frames", _tag, _ran, _seen);
            _seen = _ran = 0;
        }
    }

    if (result.sceneCut)
    {
        ++_cuts;
        LOG_INFO("{}: scene cut seen ({:.0f}% of the picture distrusted), histories reset", _tag,
                 result.distrustedShare * 100.0f);
    }

    if (result.submitted)
    {
        ++_frame;
        _status = Status::Running;
    }

    out.ran = true;
    out.submitted = result.submitted;
    return out;
}

bool NativeDriver::NrOnlyRunning() const
{
    return _status == Status::Running && _nativeRan && !Config::Instance()->DlssNrNativeUpscaler.value_or_default();
}

void NativeDriver::DrawStatus()
{
    switch (_status)
    {
    case Status::Off:
        ImGui::TextDisabled("Waiting for the first frame.");
        return;
    case Status::Waiting:
        ImGui::TextDisabled("Standing aside: the game is calling an upscaler.");
        return;
    case Status::Failed:
        ImGui::TextDisabled("Could not start: %s", _failure.c_str());
        return;
    case Status::Running:
        break;
    }

    if (Config::Instance()->DlssNrNativeUpscaler.value_or_default())
    {
        if (_nativeRan && _virtualUpscaler != nullptr && _virtualUpscaler->Active())
            ImGui::TextDisabled("%s is running as a stabiliser on this picture.",
                                _virtualUpscaler->BackendName().c_str());
        else if (_virtualUpscaler != nullptr && !_virtualUpscaler->Error().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Stabiliser: %s", _virtualUpscaler->Error().c_str());
        else
            ImGui::TextDisabled("Stabiliser: waiting for the motion estimate.");
    }
    else if (_nativeRan)
    {
        ImGui::TextDisabled("NR running on this picture.");
    }
    else
    {
        const std::string reason = DlssNr::FinishedPictureStatus();
        ImGui::TextDisabled("%s", reason.empty() ? "NR: waiting for the motion estimate." : reason.c_str());
    }
}

void NativeDriver::DrawFlowTuning()
{
    // Always shown and open on Optical F5Low's page; the settings live in the running motion estimate, so there is
    // nothing to tune until a mode runs.
    if (!ImGui::TreeNodeEx("Flow tuning (to compare, applies at once)##flowtuning", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    if (!_producer)
    {
        ImGui::TextDisabled("Shown once a mode is running.");
        ImGui::TreePop();
        return;
    }

    auto& tune = _producer->Flow()->Tuning();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Smoothing radius (0 = off)##flowsmooth", &tune.smoothRadius, 0, 4);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Search radius##flowsearch", &tune.radius, 1, 3);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Coarse cells as candidates##flowcells", &tune.coarseCells, 1, 9);
    ImGui::Checkbox("Last frame's flow as a candidate##flowhistory", &tune.useHistory);
    ImGui::Checkbox("Match within a surface (uses depth)##flowdepth", &tune.depthMatching);
    ImGui::Checkbox("Match within what looks alike##flowlook", &tune.lookWeights);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "%s", "The match counts the pixels around each one by how close their brightness is to its own,\n"
                  "so a moving thing's edge does not drag its motion onto what lies beside it. Without depth it\n"
                  "does what depth would; with depth it keeps a misaligned depth from doing harm.");
    ImGui::Checkbox("Camera motion where the picture is flat##flowglobal", &tune.globalCandidate);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          "Where nothing in the picture says how it moved (a plain wall, sky), use what the whole\n"
                          "picture did last frame. It also moves the flat inside of a still HUD panel while the\n"
                          "camera turns; switch it off to compare.");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Frames unchanged before it counts as still (0 = off)##flowstill", &tune.stillFrames, 0, 60);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          "Where the picture has not changed at all for this many frames (a HUD panel), the camera\n"
                          "motion above no longer wins a tie with no motion, so the HUD stays still while the\n"
                          "picture pans behind it. Near a line or an edge a panning wall changes, so it keeps moving\n"
                          "with the camera; a plain patch far from any is held still too. 0 switches the rule off.");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Still: largest change allowed##flowstilleps", &tune.stillEpsilon, 0.0005f, 0.05f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Still: what the camera must win by##flowstillmargin", &tune.stillMargin, 0.0f, 0.2f, "%.3f");
    ImGui::Checkbox("Cheaper sub-pixel refinement##flowinverse", &tune.inverseRefinement);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          "Finds the picture's gradients once from the current frame and stops early. Faster, but\n"
                          "less exact on thin lines and grain; switch it on and off to compare.");
    ImGui::Checkbox("Perceptual luma (HDR and SDR)##flowluma", &tune.perceptualLuma);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          "Matches on a lightness that follows how the eye sees it: an SDR picture as it is, an HDR\n"
                          "one (scRGB, PQ) after dividing by a white of 203 nits and a lightness curve, so dark\n"
                          "detail counts like bright. Off: the older tone-mapped luma.");
    bool preferStill = tune.zeroMargin > 0.0f;

    if (ImGui::Checkbox("Prefer no motion in flat ground (experimental)##flowzero", &preferStill))
        tune.zeroMargin = preferStill ? 0.001f : 0.0f;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          "Where no motion matches almost as well as the best offset, keep the flow at zero. Holds a\n"
                          "still HUD panel still while the picture pans behind it, but also stops a plain wall\n"
                          "from moving with the pan; off by default.");
    ImGui::Checkbox("Find a hard cut on its own frame##flowscene", &tune.sceneCutDetector);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          "Compares the picture's brightness histograms with the last frame's on the GPU. On a cut\n"
                          "the flow is zero and every pixel is distrusted on that very frame, instead of a few\n"
                          "frames later. A fade or an exposure change is not a cut.");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Confidence knee##flowknee", &tune.confidenceKnee, 0.0005f, 0.05f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);

    if (ImGui::Button("Close to the earlier build##flowold"))
    {
        tune.smoothRadius = 0;
        tune.radius = 2;
        tune.coarseCells = 1;
        tune.useHistory = false;
    }

    ImGui::SameLine();

    if (ImGui::Button("Defaults##flowdefaults"))
        tune = OpticalFlowDx12::Settings {};

    ImGui::TreePop();
}

void NativeDriver::DrawTrustViewCombo()
{
    if (!_producer)
        return;

    static const char* kViews[] = {
        "Final mask", "Depth check",        "Revealed-surface check", "Flow consistency check",
        "Luma check", "Outside the picture"
    };
    int view = _producer->Trust()->Tuning().debugView;

    ImGui::SetNextItemWidth(220.0f);

    if (ImGui::Combo("Show##trustview", &view, kViews, IM_ARRAYSIZE(kViews)))
        _producer->Trust()->Tuning().debugView = view;
}

} // namespace native
