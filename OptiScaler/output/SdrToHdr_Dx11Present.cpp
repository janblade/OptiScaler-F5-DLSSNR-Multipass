#include "pch.h"
#include "SdrToHdr_Dx11Present.h"

#include <Logger.h>

namespace SdrToHdr
{
namespace
{
struct Pending
{
    ProxyChainBase* proxy = nullptr;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> real;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    PassConstants constants;
};

thread_local Pending t_pending;
} // namespace

void ArmDx11Pass(ProxyChainBase* proxy, IDXGISwapChain3* real, ID3D11DeviceContext* context, const PassConstants& constants)
{
    t_pending.proxy = proxy;
    t_pending.real = real;
    t_pending.context = context;
    t_pending.constants = constants;
}

void RunPendingDx11Pass()
{
    if (t_pending.proxy == nullptr)
        return;

    // Disarmed first: the run happens once, whatever it does
    Pending run;
    run.proxy = t_pending.proxy;
    run.real = std::move(t_pending.real);
    run.context = std::move(t_pending.context);
    run.constants = t_pending.constants;
    t_pending.proxy = nullptr;

    const char* why = run.proxy->Present(run.context.Get(), run.real.Get(), run.constants);

    if (why != nullptr && run.proxy->NoteFailureLogged())
        LOG_ERROR("HDR output: {}; the real back buffer keeps the last picture", why);
}
} // namespace SdrToHdr
