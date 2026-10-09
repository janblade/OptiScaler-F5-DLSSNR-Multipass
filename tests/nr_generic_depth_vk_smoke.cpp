// Host check of resource_tracking/GenericDepth_VkEvents.h, the Vulkan depth observer's bookkeeping: recorded Vulkan
// event sequences (render passes, dynamic rendering, secondary command buffers, an imageless framebuffer) go through it
// into native::DepthFinderCore, no GPU and no game needed. The pick must be the scene's depth each time, and the copies
// must be asked for where a copy can be recorded (outside a pass), in the layout the image is in there.
//
//   cl /std:c++20 /EHsc /W4 /Iexternal\vulkan\include tests\nr_generic_depth_vk_smoke.cpp OptiScaler\native\DepthFinderCore.cpp
#include "../OptiScaler/resource_tracking/GenericDepth_VkEvents.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace GenericDepthVkEvents;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

static constexpr uint32_t W = 1920, H = 1080;
static constexpr uint32_t kWarmup = 2;

template <typename T> static T Handle(uint64_t value) { return (T) (uintptr_t) value; }

// The game's objects: a scene depth (D32, reversed-Z), a shadow map, a colour target for the HUD.
static const VkImage kSceneImage = Handle<VkImage>(0x100);
static const VkImage kShadowImage = Handle<VkImage>(0x200);
static const VkImage kColorImage = Handle<VkImage>(0x300);
static const VkImageView kSceneView = Handle<VkImageView>(0x101);
static const VkImageView kShadowView = Handle<VkImageView>(0x201);
static const VkImageView kColorView = Handle<VkImageView>(0x301);
static const VkFramebuffer kSceneFb = Handle<VkFramebuffer>(0x110);
static const VkFramebuffer kShadowFb = Handle<VkFramebuffer>(0x210);
static const VkFramebuffer kHudFb = Handle<VkFramebuffer>(0x310);
static const VkFramebuffer kImagelessFb = Handle<VkFramebuffer>(0x410);
static const VkRenderPass kScenePass = Handle<VkRenderPass>(0x120);  // colour 0, depth 1 (CLEAR, UNDEFINED -> ATTACHMENT)
static const VkRenderPass kShadowPass = Handle<VkRenderPass>(0x220); // depth 0 (CLEAR, -> SHADER_READ_ONLY)
static const VkRenderPass kHudPass = Handle<VkRenderPass>(0x320);    // colour only
static const VkCommandBuffer kCmd = Handle<VkCommandBuffer>(0x1000);
static const VkCommandBuffer kSecondaryA = Handle<VkCommandBuffer>(0x2000);
static const VkCommandBuffer kSecondaryB = Handle<VkCommandBuffer>(0x3000);

static void MakeResources(Tracker& t)
{
    t.OnImage(kSceneImage, { VK_FORMAT_D32_SFLOAT, W, H, VK_SAMPLE_COUNT_1_BIT, 0 });
    t.OnImage(kShadowImage, { VK_FORMAT_D16_UNORM, 2048, 2048, VK_SAMPLE_COUNT_1_BIT, 0 });
    t.OnImage(kColorImage, { VK_FORMAT_B8G8R8A8_UNORM, W, H, VK_SAMPLE_COUNT_1_BIT, 0 });
    t.OnView(kSceneView, kSceneImage);
    t.OnView(kShadowView, kShadowImage);
    t.OnView(kColorView, kColorImage);
    t.OnFramebuffer(kSceneFb, { kColorView, kSceneView });
    t.OnFramebuffer(kShadowFb, { kShadowView });
    t.OnFramebuffer(kHudFb, { kColorView });
    t.OnFramebuffer(kImagelessFb, {});

    RenderPassInfo scene;
    scene.attachments = {
        { VK_FORMAT_B8G8R8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        { VK_FORMAT_D32_SFLOAT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
    };
    scene.subpasses = { { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
    t.OnRenderPass(kScenePass, scene);

    RenderPassInfo shadow;
    shadow.attachments = { { VK_FORMAT_D16_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
    shadow.subpasses = { { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
    t.OnRenderPass(kShadowPass, shadow);

    RenderPassInfo hud;
    hud.attachments = { { VK_FORMAT_B8G8R8A8_UNORM, VK_ATTACHMENT_LOAD_OP_LOAD,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR } };
    hud.subpasses = { { -1, VK_IMAGE_LAYOUT_UNDEFINED } };
    t.OnRenderPass(kHudPass, hud);
}

static void Present(native::DepthFinderCore& core)
{
    core.BeginPresent(W, H);
    core.EndPresent(W, H, kWarmup);
}

static PassBegin Pass(VkRenderPass pass, VkFramebuffer fb, uint32_t width, const VkClearValue* clears, uint32_t count)
{
    PassBegin begin;
    begin.renderPass = pass;
    begin.framebuffer = fb;
    begin.clears = clears;
    begin.clearCount = count;
    begin.areaWidth = width;
    return begin;
}

struct Copies
{
    std::vector<Copy> list;
    void Add(const Copy& c)
    {
        if (c.take)
            list.push_back(c);
    }
};

// A frame of render passes: shadow map, scene (cleared to 0.0: reversed-Z), HUD.
static void RenderPassFrame(Tracker& t, native::DepthFinderCore& core, Copies& copies)
{
    t.Begin(kCmd, false);

    VkClearValue shadowClear[1] {};
    shadowClear[0].depthStencil.depth = 1.0f;
    copies.Add(t.BeginRenderPass(kCmd, Pass(kShadowPass, kShadowFb, 2048, shadowClear, 1)));
    for (int i = 0; i < 30; ++i)
        t.Draw(kCmd, 3000, 1);
    copies.Add(t.EndRenderPass(kCmd));

    VkClearValue sceneClear[2] {};
    sceneClear[1].depthStencil.depth = 0.0f;
    copies.Add(t.BeginRenderPass(kCmd, Pass(kScenePass, kSceneFb, W, sceneClear, 2)));
    t.Viewport(kCmd, (float) W);
    for (int i = 0; i < 200; ++i)
        t.Draw(kCmd, 6000, 1);
    t.Indirect(kCmd, 50);
    copies.Add(t.EndRenderPass(kCmd));

    copies.Add(t.BeginRenderPass(kCmd, Pass(kHudPass, kHudFb, W, nullptr, 0)));
    for (int i = 0; i < 20; ++i)
        t.Draw(kCmd, 6, 1);
    copies.Add(t.EndRenderPass(kCmd));

    t.End(kCmd);
    Present(core);
}

// The same frame with dynamic rendering (the scene in DEPTH_ATTACHMENT_OPTIMAL), the scene split over a suspended and a
// resumed pass.
static void DynamicFrame(Tracker& t, native::DepthFinderCore& core, Copies& copies)
{
    t.Begin(kCmd, false);

    RenderingBegin shadow;
    shadow.depthView = kShadowView;
    shadow.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    shadow.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    shadow.areaWidth = 2048;
    copies.Add(t.BeginRendering(kCmd, shadow));
    for (int i = 0; i < 30; ++i)
        t.Draw(kCmd, 3000, 1);
    copies.Add(t.EndRendering(kCmd));

    RenderingBegin scene;
    scene.depthView = kSceneView;
    scene.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    scene.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    scene.clearDepth = 0.0f;
    scene.areaWidth = W;
    scene.suspending = true;
    copies.Add(t.BeginRendering(kCmd, scene));
    for (int i = 0; i < 100; ++i)
        t.Draw(kCmd, 6000, 1);
    copies.Add(t.EndRendering(kCmd));

    scene.suspending = false;
    scene.resuming = true;
    scene.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    copies.Add(t.BeginRendering(kCmd, scene));
    for (int i = 0; i < 100; ++i)
        t.Draw(kCmd, 6000, 1);
    copies.Add(t.EndRendering(kCmd));

    t.End(kCmd);
    Present(core);
}

// The scene's draws recorded into two secondary command buffers, executed inside the primary's pass.
static void SecondaryFrame(Tracker& t, native::DepthFinderCore& core, Copies& copies)
{
    t.Begin(kSecondaryA, true);
    for (int i = 0; i < 100; ++i)
        t.Draw(kSecondaryA, 6000, 1);
    t.End(kSecondaryA);

    t.Begin(kSecondaryB, true);
    for (int i = 0; i < 100; ++i)
        t.Draw(kSecondaryB, 6000, 1);
    t.End(kSecondaryB);

    t.Begin(kCmd, false);

    VkClearValue shadowClear[1] {};
    shadowClear[0].depthStencil.depth = 1.0f;
    copies.Add(t.BeginRenderPass(kCmd, Pass(kShadowPass, kShadowFb, 2048, shadowClear, 1)));
    for (int i = 0; i < 30; ++i)
        t.Draw(kCmd, 3000, 1);
    copies.Add(t.EndRenderPass(kCmd));

    VkClearValue sceneClear[2] {};
    sceneClear[1].depthStencil.depth = 0.0f;
    copies.Add(t.BeginRenderPass(kCmd, Pass(kScenePass, kSceneFb, W, sceneClear, 2)));
    const VkCommandBuffer secondaries[] = { kSecondaryA, kSecondaryB };
    t.Execute(kCmd, secondaries, 2);
    copies.Add(t.EndRenderPass(kCmd));

    t.End(kCmd);
    Present(core);
}

int main()
{
    // Render passes: the scene is picked, reversed-Z, and copied after its pass ends, in its final layout. The shadow map
    // is never copied.
    {
        native::DepthFinderCore core;
        core.Start({});
        core.SetSnapshotsWanted(true);
        Tracker t(core);
        MakeResources(t);
        Copies copies;

        for (int frame = 0; frame < 12; ++frame)
            RenderPassFrame(t, core, copies);

        const auto pick = core.CurrentPick();
        CHECK(pick.valid && pick.id == (uint64_t) kSceneImage);
        CHECK(pick.width == W && pick.height == H);
        CHECK(pick.reversed);
        // Once picked (after the warm-up), one copy every frame.
        CHECK(copies.list.size() >= 6);

        for (const auto& c : copies.list)
        {
            CHECK(c.image == kSceneImage);
            CHECK(c.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            CHECK(std::string(c.where) == "unbind");
        }

        CHECK(t.DroppedRequests() == 0);
        printf("render passes: pick %llx, %zu copies\n", (unsigned long long) pick.id, copies.list.size());
    }

    // Dynamic rendering, the scene split over a suspended and a resumed pass: copied after the resumed pass ends, never
    // between the two.
    {
        native::DepthFinderCore core;
        core.Start({});
        core.SetSnapshotsWanted(true);
        Tracker t(core);
        MakeResources(t);
        Copies copies;

        for (int frame = 0; frame < 12; ++frame)
            DynamicFrame(t, core, copies);

        const auto pick = core.CurrentPick();
        CHECK(pick.valid && pick.id == (uint64_t) kSceneImage);
        CHECK(pick.reversed);
        CHECK(!copies.list.empty());

        for (const auto& c : copies.list)
        {
            CHECK(c.image == kSceneImage);
            CHECK(c.layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        }

        printf("dynamic rendering: pick %llx, %zu copies, %llu dropped (at the suspend)\n",
               (unsigned long long) pick.id, copies.list.size(), (unsigned long long) t.DroppedRequests());
    }

    // Secondary command buffers: their draws count for the primary's pass, so the scene is still the pick.
    {
        native::DepthFinderCore core;
        core.Start({});
        core.SetSnapshotsWanted(true);
        Tracker t(core);
        MakeResources(t);
        Copies copies;

        for (int frame = 0; frame < 12; ++frame)
            SecondaryFrame(t, core, copies);

        const auto pick = core.CurrentPick();
        CHECK(pick.valid && pick.id == (uint64_t) kSceneImage);
        CHECK(!copies.list.empty());

        for (const auto& c : copies.list)
            CHECK(c.image == kSceneImage);

        // Freed secondaries are forgotten: the primary's draws go straight to the core again.
        t.Forget(kSecondaryA);
        t.Forget(kSecondaryB);
        printf("secondary command buffers: pick %llx, %zu copies\n", (unsigned long long) pick.id, copies.list.size());
    }

    // An imageless framebuffer: the depth view comes with the begin.
    {
        native::DepthFinderCore core;
        core.Start({});
        core.SetSnapshotsWanted(true);
        Tracker t(core);
        MakeResources(t);
        Copies copies;

        for (int frame = 0; frame < 12; ++frame)
        {
            t.Begin(kCmd, false);
            const VkImageView views[] = { kColorView, kSceneView };
            VkClearValue sceneClear[2] {};
            sceneClear[1].depthStencil.depth = 0.0f;
            PassBegin begin = Pass(kScenePass, kImagelessFb, W, sceneClear, 2);
            begin.views = views;
            begin.viewCount = 2;
            copies.Add(t.BeginRenderPass(kCmd, begin));
            for (int i = 0; i < 200; ++i)
                t.Draw(kCmd, 6000, 1);
            copies.Add(t.EndRenderPass(kCmd));
            t.End(kCmd);
            Present(core);
        }

        const auto pick = core.CurrentPick();
        CHECK(pick.valid && pick.id == (uint64_t) kSceneImage);
        CHECK(!copies.list.empty());
        printf("imageless framebuffer: pick %llx, %zu copies\n", (unsigned long long) pick.id, copies.list.size());
    }

    // vkCmdClearDepthStencilImage outside a pass counts as a clear (here the reversed-Z one); a pass that loads (LOAD)
    // then draws the scene. The copy at the pass's end is in its final layout.
    {
        native::DepthFinderCore core;
        core.Start({});
        core.SetSnapshotsWanted(true);
        Tracker t(core);
        MakeResources(t);

        RenderPassInfo loadPass;
        loadPass.attachments = { { VK_FORMAT_D32_SFLOAT, VK_ATTACHMENT_LOAD_OP_LOAD,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL } };
        loadPass.subpasses = { { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
        const VkRenderPass kLoadPass = Handle<VkRenderPass>(0x520);
        const VkFramebuffer kDepthOnlyFb = Handle<VkFramebuffer>(0x510);
        t.OnRenderPass(kLoadPass, loadPass);
        t.OnFramebuffer(kDepthOnlyFb, { kSceneView });
        Copies copies;

        for (int frame = 0; frame < 12; ++frame)
        {
            t.Begin(kCmd, false);
            copies.Add(t.ClearImage(kCmd, kSceneImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0.0f));
            copies.Add(t.BeginRenderPass(kCmd, Pass(kLoadPass, kDepthOnlyFb, W, nullptr, 0)));
            for (int i = 0; i < 200; ++i)
                t.Draw(kCmd, 6000, 1);
            copies.Add(t.EndRenderPass(kCmd));
            t.End(kCmd);
            Present(core);
        }

        const auto pick = core.CurrentPick();
        CHECK(pick.valid && pick.id == (uint64_t) kSceneImage && pick.reversed);
        CHECK(!copies.list.empty());

        for (const auto& c : copies.list)
            CHECK(c.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

        printf("clear image + load pass: pick %llx, %zu copies\n", (unsigned long long) pick.id, copies.list.size());
    }

    // A destroyed image is forgotten: a pass on a view of it binds nothing.
    {
        native::DepthFinderCore core;
        core.Start({});
        Tracker t(core);
        MakeResources(t);
        t.OnImageDestroyed(kSceneImage);
        ImageInfo info;
        CHECK(!t.FindImage(kSceneImage, &info));
        CHECK(t.FindImage(kShadowImage, &info) && info.format == VK_FORMAT_D16_UNORM);
    }

    printf(fails == 0 ? "PASS\n" : "FAIL (%d)\n", fails);
    return fails == 0 ? 0 : 1;
}
