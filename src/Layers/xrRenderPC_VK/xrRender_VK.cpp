// Step 2 of docs/VK_MODULE_PLAN.md: the Vulkan renderer becomes a module the
// engine can select, and nothing more.
//
// Every interface method is a generated no-op from vk_stubs_generated.h. The
// point is not to draw - it is to make `renderer_vk` reach SetupEnv and then
// report, in call order, exactly what the engine asks a renderer to do. That
// list is the specification for step 3, and it is far more reliable read off a
// running engine than guessed from the headers.

#include "stdafx.h"

#include "Include/xrRender/xrRender.h"
#include "vk_stubs_generated.h"

namespace xray::render::render_vk
{
constexpr pcstr RENDERER_VK_MODE = "renderer_vk"; // id 7, continuing the r2..rgl sequence
constexpr int RENDERER_VK_INDEX = 7;

static vk::VKRenderStub RImplementation;
static vk::VKRenderFactoryStub RenderFactoryImpl;
static vk::VKCDUInterfaceStub DUImpl;
static vk::VKUIRenderStub UIRenderImpl;
#ifdef DEBUG
static vk::VKDebugRenderStub DebugRenderImpl;
#endif

class VKRendererModule final : public RendererModule
{
    xr_vector<std::pair<pcstr, int>> modes;

public:
    const xr_vector<std::pair<pcstr, int>>& ObtainSupportedModes() override
    {
        ZoneScoped;

        // No hardware test yet. The probe in vk_probe.cpp establishes that this
        // machine can present through Vulkan; wiring that check in belongs with
        // the device creation of step 3, not here, because a check that passes
        // would still be followed by 168 no-ops.
        if (modes.empty())
            modes.emplace_back(RENDERER_VK_MODE, RENDERER_VK_INDEX);

        return modes;
    }

    bool CheckGameRequirements() override
    {
        Log("~ [vk] renderer_vk is a stub: it installs no-op interfaces and draws nothing.");
        Log("~ [vk] Selecting it is only useful for reading which methods the engine calls.");
        return true;
    }

    void SetupEnv(pcstr mode) override
    {
        ZoneScoped;

        Msg("~ [vk] SetupEnv(%s) - installing stub interfaces", mode);

        GEnv.Render = &RImplementation;
        GEnv.RenderFactory = &RenderFactoryImpl;
        GEnv.DU = &DUImpl;
        GEnv.UIRender = &UIRenderImpl;
#ifdef DEBUG
        GEnv.DRender = &DebugRenderImpl;
#endif
    }

    void ClearEnv() override
    {
        modes.clear();

        if (GEnv.Render == &RImplementation)
        {
            GEnv.Render = nullptr;
            GEnv.RenderFactory = nullptr;
            GEnv.DU = nullptr;
            GEnv.UIRender = nullptr;
            GEnv.DRender = nullptr;
        }
    }
} static s_vk_module;

RendererModule* GetRendererModule()
{
    return &s_vk_module;
}
} // namespace xray::render::render_vk
