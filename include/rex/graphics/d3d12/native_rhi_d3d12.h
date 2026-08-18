#pragma once

// D3D12 implementation of the native-render RHI (rex/graphics/native_rhi.h).
// Constructed and owned by the D3D12 command processor; a thin passthrough
// onto the deferred command list, the CP barrier queue and the CP submission
// counters, reproducing exactly the D3D12 usage the scene renderer was
// originally written against.

#include <cstddef>
#include <cstdint>

#include <rex/graphics/native_rhi.h>
#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;

using NativeRhiD3D12ExtensionCallback = void (*)(
    ID3D12GraphicsCommandList* command_list,
    ID3D12GraphicsCommandList1* command_list_1,
    const void* payload, size_t payload_size);

// Creates the device wrapper (call once; destroy with
// DestroyNativeRhiDevice after all GPU work completed).
nrhi::Device* CreateNativeRhiDevice(D3D12CommandProcessor* command_processor);
void DestroyNativeRhiDevice(nrhi::Device* device);

// Per-frame, inside the guest-output refresher: wraps the presenter's guest
// output resource (cached by identity), drains completed retirement, and
// returns the frame Cmd. guest_output_out receives the wrapped texture.
nrhi::Cmd* NativeRhiBeginFrame(nrhi::Device* device,
                               ID3D12Resource* guest_output_resource,
                               DXGI_FORMAT guest_output_format,
                               D3D12_RESOURCE_STATES guest_output_internal_state,
                               uint32_t width, uint32_t height,
                               nrhi::Texture** guest_output_out);

ID3D12Device* NativeRhiD3D12GetDevice(nrhi::Device* device);
ID3D12Resource* NativeRhiD3D12GetTextureResource(
    nrhi::Texture* texture);
// Copies the exact native SRV backing an RHI texture view into a caller-owned
// CBV/SRV/UAV descriptor. This preserves format and component swizzle for
// extension passes that use their own root signature and descriptor heap.
bool NativeRhiD3D12CopyTextureViewDescriptor(
    nrhi::Device* device, nrhi::TextureView* view,
    D3D12_CPU_DESCRIPTOR_HANDLE destination);
void NativeRhiD3D12RecordExtension(
    nrhi::Cmd* cmd, NativeRhiD3D12ExtensionCallback callback,
    const void* payload, size_t payload_size);

}  // namespace rex::graphics::d3d12
