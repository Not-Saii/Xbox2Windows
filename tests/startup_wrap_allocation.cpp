// #2653: when the D3D12 stand-in cannot allocate an object wrapper, the slot that creates the object returns
// E_OUTOFMEMORY with a null output instead of writing through a null pointer. The fixture-only export
// x2w_d3d12_fail_wraps makes the next wrapper allocations fail; each slot is then called once armed and once
// normally.
//
//   startup_wrap_allocation [control]   (X2W_D3D12_PERMISSIVE=1 X2W_D3D12_DEBUG=0)
//     "control" never arms the failure, so every armed expectation must fail and the fixture must print FAIL.
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*, REFIID, void**);
extern "C" __declspec(dllimport) void x2w_d3d12_fail_wraps(LONG count);

namespace {

bool control;
int failures;

template<class... Args>
HRESULT invoke(IUnknown* object, unsigned slot, Args... args)
{
    using Call = HRESULT (WINAPI*)(IUnknown*, Args...);
    return reinterpret_cast<Call>((*reinterpret_cast<void***>(object))[slot])(object, args...);
}

void arm()
{
    if (!control) x2w_d3d12_fail_wraps(1);
}

// One creation: armed it must return E_OUTOFMEMORY and leave the output null; then it must succeed.
template<class Create>
void check(const char* name, Create create)
{
    ComPtr<IUnknown> object;
    arm();
    HRESULT hr = create(object.ReleaseAndGetAddressOf());
    x2w_d3d12_fail_wraps(0);
    const bool refused = hr == E_OUTOFMEMORY && !object;
    if (!refused) {
        std::printf("FAIL: %s with the wrapper allocation failing returned 0x%08lx and %s output\n", name,
                    static_cast<unsigned long>(hr), object ? "a non-null" : "a null");
        ++failures;
    }
    hr = create(object.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !object) {
        std::printf("FAIL: %s afterwards returned 0x%08lx\n", name, static_cast<unsigned long>(hr));
        ++failures;
        return;
    }
    if (refused) std::printf("%s: E_OUTOFMEMORY with a null output, then S_OK\n", name);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "control"))) return 2;
    control = argc == 2;

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))))
        return 77;
    std::array<unsigned char, 64> parameters{};

    // Device creation itself: the desktop device reference is released and the output stays null.
    ComPtr<IUnknown> device;
    check("D3D12XboxCreateDevice", [&](IUnknown** out) {
        return D3D12XboxCreateDevice(adapter.Get(), parameters.data(), __uuidof(ID3D12Device), reinterpret_cast<void**>(out));
    });
    if (FAILED(D3D12XboxCreateDevice(adapter.Get(), parameters.data(), __uuidof(ID3D12Device),
                                     reinterpret_cast<void**>(device.GetAddressOf()))) || !device) {
        std::puts("FAIL: no device to continue with");
        return 3;
    }

    // Slot 55, derived pipeline: the placeholder path of the reported stop.
    check("Device::slot55 derived pipeline", [&](IUnknown** out) {
        return invoke(device.Get(), 55, static_cast<void*>(nullptr), 0u, static_cast<const void*>(nullptr),
                      &__uuidof(ID3D12PipelineState), reinterpret_cast<void**>(out));
    });

    // Slot 8, command queue: a desktop queue is created before the wrapper.
    const D3D12_COMMAND_QUEUE_DESC queue{D3D12_COMMAND_LIST_TYPE_DIRECT};
    check("Device::CreateCommandQueue", [&](IUnknown** out) {
        return invoke(device.Get(), 8, &queue, &__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(out));
    });

    // Slot 16, root signature: a desktop root signature is created before the wrapper.
    ComPtr<ID3DBlob> blob;
    const D3D12_ROOT_SIGNATURE_DESC empty{};
    if (FAILED(D3D12SerializeRootSignature(&empty, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr))) return 3;
    check("Device::CreateRootSignature", [&](IUnknown** out) {
        return invoke(device.Get(), 16, 0u, blob->GetBufferPointer(), blob->GetBufferSize(), &__uuidof(ID3D12RootSignature),
                      reinterpret_cast<void**>(out));
    });

    // Slot 14, descriptor heap: memory of the stand-in's own may back the heap before the wrapper.
    const struct {
        D3D12_DESCRIPTOR_HEAP_DESC desc;
        UINT64 backing, bytes;
    } heap{{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4}, 0, 0};
    check("Device::CreateDescriptorHeap", [&](IUnknown** out) {
        return invoke(device.Get(), 14, &heap, &__uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(out));
    });

    if (failures) {
        std::printf("FAIL: %d check(s)\n", failures);
        return 1;
    }
    std::puts("PASS");
    return 0;
}
