#include "pch.h"
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Spatial.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"

void require(bool value)
{
    if (!value)
        throw std::runtime_error("Spatial metadata/gain policy failed");
}
int main()
{
    try
    {
        namespace Sp = DlssNr::Spatial;
        namespace Ss = DlssNr::SuperiorScaling;
        Sp::Settings settings;
        settings.enabled = true;
        unsigned checks = 0;
        for (const auto size : { DlssNr::GuideExtent { 3840, 2160 }, { 1919, 1079 } })
            for (float scale : { .5f, .59f, .65f, .75f, 1.0f, 1.5f })
            {
                const auto layout = Sp::Build(settings, size.width, size.height, scale, false);
                require(layout.active);
                require(layout.ordinaryW == Ss::WorkingExtent(size.width, scale));
                require(layout.ordinaryH == Ss::WorkingExtent(size.height, scale));
                const auto gainScale = Ss::ResolutionScale(size.width, size.height, layout.modelW, layout.modelH);
                const double expectedPixels =
                    double(layout.modelW) * layout.modelH / (double(size.width) * size.height);
                require(std::abs(double(gainScale) * gainScale - expectedPixels) < 1e-6);
                const auto guides = DlssNr::ResolveGuideRegions({ 2048, 1200 }, { 4096, 2304 }, { 1920, 1080 },
                                                                { 3840, 2160 }, true, 16, 8, 64, 32);
                for (auto reference : { DlssNr::GuideExtent { 1920, 1080 }, { 3840, 2160 }, { 0, 0 } })
                {
                    const auto constants = Sp::MakeGuideConstants(layout, guides, reference, 1920, -1080);
                    const auto rw = reference.width ? reference.width : guides.motion.width;
                    const auto rh = reference.height ? reference.height : guides.motion.height;
                    require(std::abs(constants.motionScale[0] - 1920.f * size.width / rw) < .001f);
                    require(std::abs(constants.motionScale[1] + 1080.f * size.height / rh) < .001f);
                    require(constants.depthRect[0] == 16 && constants.depthRect[1] == 8);
                    require(constants.motionRect[0] == 64 && constants.motionRect[1] == 32);
                    ++checks;
                }
                ++checks;
            }
        // Declaration controls vector units even when the available subrect is clipped.
        const auto native = Sp::Build(settings, 3840, 2160, 1.f, false);
        require(Ss::NeedsTemporal(3840, 2160, 3840, 2160, true, native.modelW, native.modelH));
        require(!Ss::NeedsTemporal(3840, 2160, 3840, 2160, false, native.modelW, native.modelH));
        require(!Ss::NeedsTemporal(3840, 2160, 3840, 2160, true, 3840, 2160));
        require(Ss::NeedsTemporal(3840, 2160, 3840, 2160, true, 3840, 1936)); // one compressed axis
        require(Ss::NeedsTemporal(3840, 2160, 2496, 1404, false, 2496, 1404));
        require(Ss::NeedsTemporal(3840, 2160, 2496, 1404, true, 2240, 1264));
        require(!Ss::NeedsTemporal(3840, 2160, 5760, 3240, true, 5184, 2928)); // supersampling is unchanged
        require(!Ss::NeedsTemporal(0, 2160, 3840, 2160, true, 3456, 1952));
        const DlssNr::GuideRegions clipped { { 20, 7, 1234, 700 }, { 3, 5, 1888, 1060 } };
        const auto c = Sp::MakeGuideConstants(native, clipped, { 1920, 1080 }, 1920, -1080);
        require(c.motionScale[0] == 3840 && c.motionScale[1] == -2160);
        const auto pre = Sp::Build(settings, 1920, 1080, 1.f, false);
        const auto preC = Sp::MakeGuideConstants(pre, clipped, { 3840, 2160 }, 3840, -2160);
        require(preC.motionScale[0] == 1920 && preC.motionScale[1] == -1080);
        const auto p65 = Sp::Build(settings, 3840, 2160, .65f, false);
        require(p65.ordinaryW == 2496 && p65.ordinaryH == 1404);
        require(Ss::ResolutionScale(3840, 2160, p65.modelW, p65.modelH) < .65f);
        require(Ss::ResolutionScale(3840, 2160, native.modelW, native.modelH) < 1.f);
        Sp::Settings asymmetric = settings;
        asymmetric.workX = 85;
        asymmetric.centerX = 70;
        asymmetric.workY = 95;
        const auto both = Sp::Build(asymmetric, 3840, 2160, .65f, false);
        require(both.active);
        const auto equivalent = Ss::ResolutionScale(3840, 2160, both.modelW, both.modelH);
        require(std::abs(equivalent - std::sqrt((both.modelW / 3840.f) * (both.modelH / 2160.f))) < 1e-6f);
        require(Ss::EffectiveBand(.8f, equivalent, 1, 1) < Ss::EffectiveBand(.8f, .65f, 1, 1));
        std::cout << "Spatial metadata and final-resolution gain policy PASS: " << checks
                  << " cases, native/pre/post-SR, clipped/offset guides, exact Superior grid and both packed axes\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
