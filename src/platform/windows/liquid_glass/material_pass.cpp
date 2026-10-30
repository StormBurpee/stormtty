#include "material_pass.hpp"
#include "stormtty/glass_material.hpp"
#include "stormtty_glass_shader.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <winrt/base.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace stormtty::windows::liquid_glass {
namespace {
using winrt::check_hresult;
void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
winrt::com_ptr<ID3DBlob> compile_shader(const char* entry, const char* profile) {
    winrt::com_ptr<ID3DBlob> code, diagnostics;
    const auto result = D3DCompile(shader_source, sizeof(shader_source)-1, "StormTTY.LiquidGlass",
        nullptr, nullptr, entry, profile,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, code.put(), diagnostics.put());
    if (FAILED(result)) {
        std::string message = std::string("Liquid Glass shader compilation failed: ") + entry;
        if (diagnostics && diagnostics->GetBufferSize()) {
            // Compiler diagnostics contain only our embedded shader, never desktop pixels.
            const auto bytes = std::min<std::size_t>(diagnostics->GetBufferSize(), 4096);
            message += "\n";
            message.append(static_cast<const char*>(diagnostics->GetBufferPointer()), bytes);
        }
        throw std::runtime_error(message);
    }
    return code;
}
// D2D shares the immediate context. Swap a dedicated context-state object rather
// than assuming a ClearState followed by BeginDraw reconstructs D2D's cached state.
class ContextScope final {
public:
    ContextScope(ID3D11DeviceContext1* context, ID3DDeviceContextState* state) : context_(context) {
        context_->SwapDeviceContextState(state, previous_.put());
    }
    ~ContextScope() { context_->SwapDeviceContextState(previous_.get(), nullptr); }
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
private:
    ID3D11DeviceContext1* context_;
    winrt::com_ptr<ID3DDeviceContextState> previous_;
};
}
struct MaterialPass::Impl {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext1> context;
    winrt::com_ptr<ID3DDeviceContextState> state;
    winrt::com_ptr<ID3D11VertexShader> vertex;
    winrt::com_ptr<ID3D11PixelShader> pixel;
    winrt::com_ptr<ID3D11Buffer> parameters;
    winrt::com_ptr<ID3D11SamplerState> sampler;
    winrt::com_ptr<ID3D11BlendState> blend;
    winrt::com_ptr<ID3D11DepthStencilState> depth;
    winrt::com_ptr<ID3D11RasterizerState> raster;
    winrt::com_ptr<ID3D11Texture2D> source_copy, output;
    winrt::com_ptr<ID3D11ShaderResourceView> source_view;
    winrt::com_ptr<ID3D11RenderTargetView> output_view;
    UINT width{}, height{};
    Impl(ID3D11Device* input_device, ID3D11DeviceContext* input_context) {
        require(input_device && input_context, "Liquid Glass needs a native D3D11 device/context.");
        require(input_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE,
                "Liquid Glass requires an immediate device context.");
        device.copy_from(input_device);
        check_hresult(input_context->QueryInterface(__uuidof(ID3D11DeviceContext1), context.put_void()));
        winrt::com_ptr<ID3D11Device> context_device;
        input_context->GetDevice(context_device.put());
        require(context_device.get() == input_device, "Liquid Glass context/device mismatch.");
        auto device1 = device.as<ID3D11Device1>();
        require(device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0, "Liquid Glass requires D3D feature level 11.0.");
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        const UINT state_flags=(device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ?
            D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        D3D_FEATURE_LEVEL chosen{};
        check_hresult(device1->CreateDeviceContextState(state_flags, &level, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device), &chosen, state.put()));
        const auto vs = compile_shader("fullscreen_vs", "vs_5_0");
        const auto ps = compile_shader("glass_ps", "ps_5_0");
        check_hresult(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vertex.put()));
        check_hresult(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, pixel.put()));
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = static_cast<UINT>(sizeof(glass_optics::ShaderParameters));
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check_hresult(device->CreateBuffer(&buffer, nullptr, parameters.put()));
        D3D11_SAMPLER_DESC sampling{};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxAnisotropy = 1;
        sampling.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        check_hresult(device->CreateSamplerState(&sampling, sampler.put()));
        D3D11_BLEND_DESC blending{};
        blending.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blending.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        blending.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blending.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blending.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blending.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blending.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check_hresult(device->CreateBlendState(&blending, blend.put()));
        D3D11_DEPTH_STENCIL_DESC depth_desc{};
        depth_desc.DepthEnable = FALSE;
        depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        depth_desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
        depth_desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
        depth_desc.FrontFace = {D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
        depth_desc.BackFace = depth_desc.FrontFace;
        check_hresult(device->CreateDepthStencilState(&depth_desc, depth.put()));
        D3D11_RASTERIZER_DESC raster_desc{};
        raster_desc.FillMode = D3D11_FILL_SOLID;
        raster_desc.CullMode = D3D11_CULL_NONE;
        raster_desc.DepthClipEnable = TRUE;
        check_hresult(device->CreateRasterizerState(&raster_desc, raster.put()));
    }
    void resize(UINT w, UINT h) {
        if (source_copy && w == width && h == height) return;
        // Construct a complete replacement before changing the currently valid pair.
        winrt::com_ptr<ID3D11Texture2D> new_source, new_output;
        winrt::com_ptr<ID3D11ShaderResourceView> new_source_view;
        winrt::com_ptr<ID3D11RenderTargetView> new_output_view;
        D3D11_TEXTURE2D_DESC description{};
        description.Width=w; description.Height=h; description.MipLevels=1; description.ArraySize=1;
        description.Format=DXGI_FORMAT_B8G8R8A8_TYPELESS; description.SampleDesc.Count=1;
        description.Usage=D3D11_USAGE_DEFAULT; description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        check_hresult(device->CreateTexture2D(&description,nullptr,new_source.put()));
        description.BindFlags=D3D11_BIND_RENDER_TARGET;
        check_hresult(device->CreateTexture2D(&description,nullptr,new_output.put()));
        // WGC display input is assumed opaque. For fractional-alpha encoded
        // premultiplied input, direct sRGB decode would be incorrect: that needs
        // unpremultiply/decode/repremultiply and is outside this source contract.
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels=1;
        check_hresult(device->CreateShaderResourceView(new_source.get(),&srv,new_source_view.put()));
        D3D11_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; rtv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        check_hresult(device->CreateRenderTargetView(new_output.get(),&rtv,new_output_view.put()));
        source_copy=std::move(new_source); output=std::move(new_output);
        source_view=std::move(new_source_view); output_view=std::move(new_output_view);
        width=w; height=h;
    }
};
MaterialPass::MaterialPass(ID3D11Device* device, ID3D11DeviceContext* context)
    : impl_(std::make_unique<Impl>(device,context)) {}
MaterialPass::~MaterialPass() = default;
void MaterialPass::draw(ID3D11Texture2D* captured_crop, ID3D11Texture2D* destination, float scale) {
    auto& p=*impl_;
    require(captured_crop && destination && captured_crop != destination,
            "Liquid Glass source and destination must be distinct textures.");
    require(std::isfinite(scale) && scale>=.5f && scale<=4.f,
            "Experimental Liquid Glass supports 50–400 percent display scale.");
    D3D11_TEXTURE2D_DESC source_desc{}, target_desc{};
    captured_crop->GetDesc(&source_desc); destination->GetDesc(&target_desc);
    require(source_desc.Width>=2 && source_desc.Height>=2 && source_desc.Width<=4096 && source_desc.Height<=4096 &&
            std::uint64_t(source_desc.Width)*source_desc.Height<=4194304,
            "Liquid Glass client surface exceeds its bounded four-megapixel budget.");
    require(source_desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM && target_desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM &&
            source_desc.Width==target_desc.Width && source_desc.Height==target_desc.Height &&
            source_desc.MipLevels==1 && target_desc.MipLevels==1 &&
            source_desc.ArraySize==1 && target_desc.ArraySize==1 &&
            source_desc.SampleDesc.Count==1 && target_desc.SampleDesc.Count==1 &&
            source_desc.SampleDesc.Quality==0 && target_desc.SampleDesc.Quality==0 &&
            source_desc.Usage==D3D11_USAGE_DEFAULT && target_desc.Usage==D3D11_USAGE_DEFAULT &&
            source_desc.CPUAccessFlags==0 && target_desc.CPUAccessFlags==0,
            "Liquid Glass requires matching single-sampled BGRA8 source/destination textures.");
    winrt::com_ptr<ID3D11Device> source_device, target_device;
    captured_crop->GetDevice(source_device.put()); destination->GetDevice(target_device.put());
    require(source_device.get()==p.device.get() && target_device.get()==p.device.get(),
            "Liquid Glass cannot copy across D3D devices.");
    p.resize(source_desc.Width,source_desc.Height);
    glass_optics::MaterialConfig config;
    config.center={float(p.width)*.5f,float(p.height)*.5f};
    config.size={float(p.width),float(p.height)};
    config.corner_radius=std::min(24.f*scale,float(std::min(p.width,p.height))*.5f);
    config.rim_width=std::min(12.f*scale,config.corner_radius);
    config.thickness=8.f*scale;
    config.maximum_displacement=10.f*scale;
    config.blur_radius=1.25f*scale;
    config.highlight_strength=.12f;
    const auto constants=glass_optics::GlassMaterial(config).shader_parameters(p.width,p.height);
    // Swap before copies as well: the destination might be retained as a D2D target
    // in the caller's pipeline. The saved state is restored only after our copy.
    ContextScope scope(p.context.get(),p.state.get());
    p.context->CopyResource(p.source_copy.get(),captured_crop);
    p.context->UpdateSubresource(p.parameters.get(),0,nullptr,&constants,0,0);
    p.context->IASetInputLayout(nullptr);
    p.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p.context->VSSetShader(p.vertex.get(),nullptr,0);
    p.context->HSSetShader(nullptr,nullptr,0); p.context->DSSetShader(nullptr,nullptr,0); p.context->GSSetShader(nullptr,nullptr,0);
    p.context->PSSetShader(p.pixel.get(),nullptr,0);
    p.context->SetPredication(nullptr,FALSE);
    auto* sampler=p.sampler.get(); auto* parameters=p.parameters.get(); auto* source_view=p.source_view.get();
    p.context->PSSetSamplers(0,1,&sampler); p.context->PSSetConstantBuffers(0,1,&parameters);
    p.context->PSSetShaderResources(0,1,&source_view);
    auto* target=p.output_view.get();
    p.context->OMSetRenderTargets(1,&target,nullptr);
    p.context->OMSetBlendState(p.blend.get(),nullptr,0xffffffffu);
    p.context->OMSetDepthStencilState(p.depth.get(),0);
    p.context->RSSetState(p.raster.get());
    const D3D11_VIEWPORT viewport{0,0,float(p.width),float(p.height),0,1};
    p.context->RSSetViewports(1,&viewport);
    p.context->Draw(3,0);
    // No views of the working textures remain bound while copying or across frames.
    ID3D11ShaderResourceView* empty=nullptr;
    p.context->PSSetShaderResources(0,1,&empty);
    p.context->OMSetRenderTargets(0,nullptr,nullptr);
    p.context->CopyResource(destination,p.output.get());
    check_hresult(p.device->GetDeviceRemovedReason());
}
}
