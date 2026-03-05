#include <array>
#include <unordered_map>   // for CPU meshlet vertex remapping

/**
* Sapphire Suite Debugger / Maths / Transform
*/
#include <SA/Collections/Debug>
#include <SA/Collections/Maths>
#include <SA/Collections/Transform>


// ========== Windowing ==========

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

GLFWwindow* window = nullptr;
constexpr SA::Vec2ui windowSize = { 1200, 900 };

void GLFWErrorCallback(int32_t error, const char* description)
{
    SA_LOG((L"GLFW Error [%1]: %2", error, description), Error, GLFW.API);
}


// ========== Renderer ==========

#include <wrl.h>
template <typename T>
using MComPtr = Microsoft::WRL::ComPtr<T>;

/**
* Force Agility SDK 615 – required for Mesh Shader support.
*/
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 615; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

#include <d3d12.h>
#include <DXGIDebug.h>


// === Validation Layers ===

#if SA_DEBUG
DWORD VLayerCallbackCookie = 0;

void ValidationLayersDebugCallback(D3D12_MESSAGE_CATEGORY _category,
    D3D12_MESSAGE_SEVERITY _severity,
    D3D12_MESSAGE_ID _ID,
    LPCSTR _description,
    void* _context)
{
    (void)_context;

    std::wstring categoryStr;
    switch (_category)
    {
    case D3D12_MESSAGE_CATEGORY_APPLICATION_DEFINED:  categoryStr = L"Application Defined"; break;
    case D3D12_MESSAGE_CATEGORY_MISCELLANEOUS:        categoryStr = L"Miscellaneous";        break;
    case D3D12_MESSAGE_CATEGORY_INITIALIZATION:       categoryStr = L"Initialization";       break;
    case D3D12_MESSAGE_CATEGORY_CLEANUP:              categoryStr = L"Cleanup";              break;
    case D3D12_MESSAGE_CATEGORY_COMPILATION:          categoryStr = L"Compilation";          break;
    case D3D12_MESSAGE_CATEGORY_STATE_CREATION:       categoryStr = L"State Creation";       break;
    case D3D12_MESSAGE_CATEGORY_STATE_SETTING:        categoryStr = L"State Setting";        break;
    case D3D12_MESSAGE_CATEGORY_STATE_GETTING:        categoryStr = L"State Getting";        break;
    case D3D12_MESSAGE_CATEGORY_RESOURCE_MANIPULATION:categoryStr = L"Resource Manipulation";break;
    case D3D12_MESSAGE_CATEGORY_EXECUTION:            categoryStr = L"Execution";            break;
    case D3D12_MESSAGE_CATEGORY_SHADER:               categoryStr = L"Shader";               break;
    default:                                          categoryStr = L"Unknown";              break;
    }

    std::wstring dets = SA::StringFormat(L"ID [%1]\tCategory [%2]", static_cast<int>(_ID), categoryStr);

    switch (_severity)
    {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION: SA_LOG(_description, AssertFailure, DX12.ValidationLayers, std::move(dets)); break;
    case D3D12_MESSAGE_SEVERITY_ERROR:      SA_LOG(_description, Error,          DX12.ValidationLayers, std::move(dets)); break;
    case D3D12_MESSAGE_SEVERITY_WARNING:    SA_LOG(_description, Warning,        DX12.ValidationLayers, std::move(dets)); break;
    case D3D12_MESSAGE_SEVERITY_INFO:       return; // filtered – too noisy
    default:                                SA_LOG(_description, Normal,         DX12.ValidationLayers, std::move(dets)); break;
    }
}
#endif


// === Factory === /* 0001 */

#include <dxgi1_6.h>
MComPtr<IDXGIFactory6> factory;


// === Device === /* 0002 */

MComPtr<ID3D12Device> device;
MComPtr<ID3D12CommandQueue> graphicsQueue;

HANDLE deviceFenceEvent;
MComPtr<ID3D12Fence> deviceFence;
uint32_t deviceFenceValue = 1u;

void WaitDeviceIdle()
{
    graphicsQueue->Signal(deviceFence.Get(), deviceFenceValue);
    deviceFence->SetEventOnCompletion(deviceFenceValue, deviceFenceEvent);
    WaitForSingleObjectEx(deviceFenceEvent, INFINITE, false);
    ++deviceFenceValue;
}


// === Swapchain === /* 0003 */

constexpr uint32_t bufferingCount = 3;

MComPtr<IDXGISwapChain3> swapchain;
std::array<MComPtr<ID3D12Resource>, bufferingCount> swapchainImages{ nullptr };
uint32_t swapchainFrameIndex = 0u;

HANDLE swapchainFenceEvent = nullptr;
MComPtr<ID3D12Fence> swapchainFence;
std::array<uint32_t, bufferingCount> swapchainFenceValues{ 0u };


// === Commands === /* 0004 */

std::array<MComPtr<ID3D12CommandAllocator>, bufferingCount> cmdAllocs;

/**
* CHANGED from ID3D12GraphicsCommandList1 to ID3D12GraphicsCommandList6.
* ID3D12GraphicsCommandList6 exposes DispatchMesh() which is required
* to launch a mesh-shader pipeline.
*/
MComPtr<ID3D12GraphicsCommandList6> cmdList;


// === Scene Textures === /* 0005 */

constexpr DXGI_FORMAT sceneColorFormat    = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr float       sceneClearColor[]   = { 0.0f, 0.1f, 0.2f, 1.0f };
MComPtr<ID3D12DescriptorHeap> sceneRTViewHeap;          /* 0006-1 */

constexpr DXGI_FORMAT        sceneDepthFormat     = DXGI_FORMAT_D16_UNORM;
constexpr D3D12_CLEAR_VALUE  sceneDepthClearValue { .Format = sceneDepthFormat, .DepthStencil = { 1.0f, 0 } };
MComPtr<ID3D12Resource>      sceneDepthTexture;
MComPtr<ID3D12DescriptorHeap>sceneDepthRTViewHeap;      /* 0006-2 */


// === Pipeline === /* 0008 */

D3D12_VIEWPORT viewport{};
D3D12_RECT     scissorRect{};

#include <d3dcompiler.h>    // native compiler (unused at runtime; kept for reference)

#include <dxc/dxcapi.h>
MComPtr<IDxcUtils>     shaderCompilerUtils;
MComPtr<IDxcCompiler3> shaderCompiler;

MComPtr<ID3DBlob> CompileShader(std::wstring _path, std::wstring _entry, std::wstring _target, std::vector<std::wstring> _defines = {})
{
    MComPtr<IDxcBlobEncoding> blob;
    DxcBuffer dx;

    const HRESULT hrLoad = shaderCompilerUtils->LoadFile(_path.c_str(), nullptr, &blob);
    if (FAILED(hrLoad))
    {
        SA_LOG((L"Load Shader {%1} failed!", _path), Error, DXC, (L"Error Code: %1", hrLoad));
        return nullptr;
    }

    dx.Ptr      = blob->GetBufferPointer();
    dx.Size     = blob->GetBufferSize();
    dx.Encoding = 0;

    std::vector<LPCWSTR> cArgs
    {
        L"-E", _entry.c_str(),
        L"-T", _target.c_str(),
        L"-HV", L"2021",
        DXC_ARG_WARNINGS_ARE_ERRORS,
        DXC_ARG_PACK_MATRIX_ROW_MAJOR,
        DXC_ARG_ALL_RESOURCES_BOUND,
    };

#if SA_DEBUG
    cArgs.push_back(DXC_ARG_DEBUG);
    cArgs.push_back(DXC_ARG_SKIP_OPTIMIZATIONS);
#else
    cArgs.push_back(DXC_ARG_OPTIMIZATION_LEVEL3);
#endif

    for (auto& define : _defines)
    {
        cArgs.push_back(L"-D");
        cArgs.push_back(define.c_str());
    }

    MComPtr<IDxcResult> result;
    shaderCompiler->Compile(&dx, cArgs.data(), static_cast<uint32_t>(cArgs.size()), nullptr, IID_PPV_ARGS(&result));

    HRESULT hr;
    result->GetStatus(&hr);
    if (FAILED(hr))
    {
        MComPtr<IDxcBlobUtf8> errors;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (errors && errors->GetStringLength() > 0)
            SA_LOG((L"Shader {%1:%2} Compilation failed!", _path, _entry), Error, DXC, errors->GetStringPointer());
        return nullptr;
    }

    MComPtr<ID3DBlob> code;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr);
    return code;
}


// = Lit =
/**
* CHANGED: litVertexShader -> litMeshShader.
* No VS or input layout; geometry is fed entirely through SRVs.
*/
MComPtr<ID3DBlob> litMeshShader;
MComPtr<ID3DBlob> litPixelShader;

MComPtr<ID3D12RootSignature> litRootSign;
MComPtr<ID3D12PipelineState> litPipelineState;

// = Meshlet Debug =
/**
* Separate pipeline that colors every meshlet with a unique hash-derived
* color. Reuses litRootSign (identical binding layout) so no rebinding
* is needed at draw time — only SetPipelineState changes.
*
* Toggle at runtime by pressing F1.
*/
MComPtr<ID3DBlob>            debugMeshShader;
MComPtr<ID3DBlob>            debugPixelShader;
MComPtr<ID3D12PipelineState> debugPipelineState;
bool                         debugMeshletsMode = false;


// ===================================================================
// === Pipeline State Stream helpers (no d3dx12.h dependency)     ===
// ===================================================================
//
// To create a mesh-shader pipeline, CreateGraphicsPipelineState() cannot
// be used (it has no MS/AS slot). We must use ID3D12Device2::CreatePipelineState()
// with a D3D12_PIPELINE_STATE_STREAM_DESC made up of typed sub-objects.
//
// Each sub-object is: { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE, <payload> }
// aligned to pointer size.

template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE SubType, typename T>
struct alignas(void*) PSSub
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ SubType };
    T data{};
};

using PSS_RootSignature = PSSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,        ID3D12RootSignature*>;
using PSS_MS            = PSSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS,                    D3D12_SHADER_BYTECODE>;
using PSS_PS            = PSSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,                    D3D12_SHADER_BYTECODE>;
using PSS_Blend         = PSSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,                 D3D12_BLEND_DESC>;
using PSS_Rasterizer    = PSSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,            D3D12_RASTERIZER_DESC>;
using PSS_DepthStencil  = PSSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,         D3D12_DEPTH_STENCIL_DESC>;
using PSS_RTFormats     = PSSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>;
using PSS_DSVFormat     = PSSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,  DXGI_FORMAT>;
using PSS_SampleDesc    = PSSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,           DXGI_SAMPLE_DESC>;

/**
* Packed stream consumed by CreatePipelineState().
* Members MUST remain in declaration order – the API reads them sequentially.
*/
struct MeshShaderPipelineStateStream
{
    PSS_RootSignature rootSignature;
    PSS_MS            ms;
    PSS_PS            ps;
    PSS_Blend         blend;
    PSS_Rasterizer    rasterizer;
    PSS_DepthStencil  depthStencil;
    PSS_RTFormats     rtFormats;
    PSS_DSVFormat     dsvFormat;
    PSS_SampleDesc    sampleDesc;
};


// === Scene Objects === /* 0009 */

MComPtr<ID3D12DescriptorHeap> pbrSphereSRVHeap;

// Camera
struct CameraUBO { SA::Mat4f view; SA::Mat4f invViewProj; };
SA::TransformPRf cameraTr;
constexpr float cameraMoveSpeed = 4.0f;
constexpr float cameraRotSpeed  = 16.0f;
constexpr float cameraNear      = 0.1f;
constexpr float cameraFar       = 1000.0f;
constexpr float cameraFOV       = 90.0f;
std::array<MComPtr<ID3D12Resource>, bufferingCount> cameraBuffers;

// Object
struct ObjectUBO { SA::Mat4f transform; };
constexpr SA::Vec3f spherePosition(0.5f, 0.0f, 2.0f);
MComPtr<ID3D12Resource> sphereObjectBuffer;

// PointLights
struct PointLightUBO
{
    SA::Vec3f position;
    float     intensity = 0.0f;
    SA::Vec3f color;
    float     radius    = 0.0f;
};
constexpr uint32_t      pointLightNum = 2;
MComPtr<ID3D12Resource> pointLightBuffer;


// ===================================================================
// === NEW: Meshlet CPU types                                      ===
// ===================================================================

/**
* Matches the HLSL MeshletData struct in MeshShader.hlsl.
*/
struct MeshletDesc
{
    uint32_t vertexOffset; ///< Start of unique-vertex list in meshletVertsBuf.
    uint32_t vertexCount;  ///< Number of unique vertices (<= MAX_VERTS = 128).
    uint32_t primOffset;   ///< Start of triangle list in meshletPrimsBuf.
    uint32_t primCount;    ///< Number of triangles    (<= MAX_PRIMS = 128).
};

/**
* One triangle: three local vertex indices within the parent meshlet.
* Matches StructuredBuffer<uint3> in the mesh shader.
*/
struct MeshletPrimitive { uint32_t x, y, z; };


// === Resources === /* 0010 */

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#pragma warning(disable : 4505)
#include <stb_image_resize2.h>
#pragma warning(default : 4505)


bool SubmitBufferToGPU(MComPtr<ID3D12Resource> _gpuBuffer, uint64_t _size, const void* _data, D3D12_RESOURCE_STATES _stateAfter)
{
    MComPtr<ID3D12Resource> stagingBuffer;

    const D3D12_HEAP_PROPERTIES heap{ .Type = D3D12_HEAP_TYPE_UPLOAD };
    const D3D12_RESOURCE_DESC desc{
        .Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width            = _size,
        .Height           = 1, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format           = DXGI_FORMAT_UNKNOWN,
        .SampleDesc       = { .Count = 1, .Quality = 0 },
        .Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };

    // Buffers must be created in COMMON. UPLOAD-heap buffers are implicitly
    // usable as a copy source without any explicit ResourceBarrier.
    const HRESULT hrStag = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&stagingBuffer));
    if (FAILED(hrStag))
    {
        SA_LOG(L"Create Staging Buffer failed!", Error, DX12, (L"Error code: %1", hrStag));
        return false;
    }

    const D3D12_RANGE range{ .Begin = 0, .End = 0 };
    void* data = nullptr;
    stagingBuffer->Map(0, &range, reinterpret_cast<void**>(&data));
    std::memcpy(data, _data, _size);
    stagingBuffer->Unmap(0, nullptr);

    cmdList->CopyBufferRegion(_gpuBuffer.Get(), 0, stagingBuffer.Get(), 0, _size);

    const D3D12_RESOURCE_BARRIER barrier{
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {
            .pResource   = _gpuBuffer.Get(),
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
            .StateAfter  = _stateAfter,
        },
    };
    cmdList->ResourceBarrier(1, &barrier);
    cmdList->Close();

    ID3D12CommandList* cmdListsArr[] = { cmdList.Get() };
    graphicsQueue->ExecuteCommandLists(1, cmdListsArr);
    WaitDeviceIdle();

    cmdAllocs[0]->Reset();
    cmdList->Reset(cmdAllocs[0].Get(), nullptr);
    return true;
}

bool SubmitTextureToGPU(MComPtr<ID3D12Resource> _gpuTexture, const std::vector<SA::Vec2ui>& _extents, uint64_t _totalSize, uint32_t _channelNum, const void* _data)
{
    MComPtr<ID3D12Resource> stagingBuffer;

    const D3D12_HEAP_PROPERTIES heap{ .Type = D3D12_HEAP_TYPE_UPLOAD };
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = _totalSize, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };

    // Same rule: staging buffer (which is a Buffer dimension) must start in COMMON.
    const HRESULT hrStag = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&stagingBuffer));
    if (FAILED(hrStag)) { SA_LOG(L"Create Staging Buffer failed!", Error, DX12, (L"Error code: %1", hrStag)); return false; }

    const D3D12_RANGE range{ .Begin = 0, .End = 0 };
    void* data = nullptr;
    stagingBuffer->Map(0, &range, reinterpret_cast<void**>(&data));
    std::memcpy(data, _data, _totalSize);
    stagingBuffer->Unmap(0, nullptr);

    const D3D12_RESOURCE_DESC resDesc = _gpuTexture->GetDesc();
    UINT64 offset = 0u;
    for (UINT16 i = 0; i < resDesc.MipLevels; ++i)
    {
        const D3D12_TEXTURE_COPY_LOCATION src{
            .pResource = stagingBuffer.Get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            .PlacedFootprint = {
                .Offset    = offset,
                .Footprint = { resDesc.Format, _extents[i].x, _extents[i].y, 1, _extents[i].x * _channelNum }
            }
        };
        const D3D12_TEXTURE_COPY_LOCATION dst{ .pResource = _gpuTexture.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = i };
        cmdList->CopyTextureRegion(&dst, 0u, 0u, 0u, &src, nullptr);
        offset += _extents[i].x * _extents[i].y * _channelNum;
    }

    const D3D12_RESOURCE_BARRIER barrier{
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = { .pResource = _gpuTexture.Get(), .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST, .StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
    };
    cmdList->ResourceBarrier(1, &barrier);
    cmdList->Close();

    ID3D12CommandList* cmdListsArr[] = { cmdList.Get() };
    graphicsQueue->ExecuteCommandLists(1, cmdListsArr);
    WaitDeviceIdle();
    cmdAllocs[0]->Reset();
    cmdList->Reset(cmdAllocs[0].Get(), nullptr);
    return true;
}

void GenerateMipMapsCPU(SA::Vec2ui _extent, std::vector<char>& _data, uint32_t& _outMipLevels, uint32_t& _outTotalSize, std::vector<SA::Vec2ui>& _outExtents, uint32_t _channelNum, uint32_t _layerNum = 1u)
{
    _outMipLevels = static_cast<uint32_t>(std::floor(std::log2(max(_extent.x, _extent.y)))) + 1;
    _outExtents.resize(_outMipLevels);

    for (uint32_t i = 0u; i < _outMipLevels; ++i)
    {
        _outExtents[i]  = _extent;
        _outTotalSize  += _extent.x * _extent.y * _channelNum * _layerNum * sizeof(stbi_uc);
        if (_extent.x > 1) _extent.x >>= 1;
        if (_extent.y > 1) _extent.y >>= 1;
    }

    _data.resize(_outTotalSize);

    unsigned char* src = reinterpret_cast<unsigned char*>(_data.data());
    for (uint32_t i = 1u; i < _outMipLevels; ++i)
    {
        uint64_t srcLayerOff  = _outExtents[i-1].x * _outExtents[i-1].y * _channelNum * sizeof(stbi_uc);
        uint64_t currLayerOff = _outExtents[i  ].x * _outExtents[i  ].y * _channelNum * sizeof(stbi_uc);
        unsigned char* dst    = src + srcLayerOff * _layerNum;

        for (uint32_t j = 0; j < _layerNum; ++j)
        {
            bool res = stbir_resize_uint8_linear(src, (int)_outExtents[i-1].x, (int)_outExtents[i-1].y, 0,
                                                 dst, (int)_outExtents[i].x,   (int)_outExtents[i].y,   0,
                                                 static_cast<stbir_pixel_layout>(_channelNum));
            if (!res) { SA_LOG(L"Mip map creation failed!", Error, STB); return; }
            dst += currLayerOff;
            src += srcLayerOff;
        }
    }
}


// ===================================================================
// === NEW: CPU Meshlet Builder                                    ===
// ===================================================================
//
// Walks the triangle list and packs triangles into meshlets.
// Each meshlet has at most MAX_VERTS unique vertices and MAX_PRIMS triangles,
// mirroring the shader-side constants MAX_VERTS=128 and MAX_PRIMS=128.
//
// Output:
//   meshlets      – one MeshletDesc per group dispatch
//   meshletVerts  – global vertex indices (per-meshlet concatenated)
//   meshletPrims  – local triangle indices (per-meshlet concatenated)

void BuildMeshlets(
    const uint16_t*               indices,
    uint32_t                      triCount,
    std::vector<MeshletDesc>&     meshlets,
    std::vector<uint32_t>&        meshletVerts,
    std::vector<MeshletPrimitive>&meshletPrims)
{
    constexpr uint32_t MAX_VERTS = 128u;
    constexpr uint32_t MAX_PRIMS = 128u;

    MeshletDesc cur{};
    std::unordered_map<uint32_t, uint32_t> remap; // global vertex idx -> local meshlet idx

    for (uint32_t t = 0; t < triCount; ++t)
    {
        uint32_t i0 = indices[t * 3 + 0];
        uint32_t i1 = indices[t * 3 + 1];
        uint32_t i2 = indices[t * 3 + 2];

        uint32_t newVerts = (remap.count(i0) ? 0u : 1u)
                          + (remap.count(i1) ? 0u : 1u)
                          + (remap.count(i2) ? 0u : 1u);

        // Flush current meshlet if it would overflow.
        if (cur.vertexCount + newVerts > MAX_VERTS || cur.primCount >= MAX_PRIMS)
        {
            meshlets.push_back(cur);
            remap.clear();
            cur.vertexOffset = static_cast<uint32_t>(meshletVerts.size());
            cur.primOffset   = static_cast<uint32_t>(meshletPrims.size());
            cur.vertexCount  = 0u;
            cur.primCount    = 0u;
        }

        auto addVert = [&](uint32_t gi) -> uint32_t {
            auto it = remap.find(gi);
            if (it != remap.end()) return it->second;
            uint32_t li   = cur.vertexCount++;
            remap[gi]     = li;
            meshletVerts.push_back(gi);
            return li;
        };

        uint32_t l0 = addVert(i0), l1 = addVert(i1), l2 = addVert(i2);
        meshletPrims.push_back({ l0, l1, l2 });
        cur.primCount++;
    }

    if (cur.primCount > 0)
        meshlets.push_back(cur);
}


// ===================================================================
// === NEW: Sphere mesh buffers – now SRVs, not vertex buffers    ===
// ===================================================================
//
// In the vertex-shader path the sphere used VertexBufferViews and an
// IndexBufferView bound via IASetVertexBuffers / IASetIndexBuffer.
//
// In the mesh-shader path the same data lives in DEFAULT-heap buffers
// and is read by the mesh shader as StructuredBuffers (SRVs).
// The index buffer is consumed during CPU meshlet generation and is
// never uploaded to the GPU as an index buffer.

MComPtr<ID3D12Resource> spherePosBuffer;     // StructuredBuffer<float3>  t8
MComPtr<ID3D12Resource> sphereNormBuffer;    // StructuredBuffer<float3>  t9
MComPtr<ID3D12Resource> sphereTangBuffer;    // StructuredBuffer<float3>  t10
MComPtr<ID3D12Resource> sphereUVBuffer;      // StructuredBuffer<float2>  t11
MComPtr<ID3D12Resource> sphereMeshletBuf;    // StructuredBuffer<MeshletData>       t5
MComPtr<ID3D12Resource> sphereMeshletVertBuf;// StructuredBuffer<uint>              t6
MComPtr<ID3D12Resource> sphereMeshletPrimBuf;// StructuredBuffer<uint3>             t7

uint32_t sphereMeshletCount = 0u;

// PBR textures
MComPtr<ID3D12Resource> rustedIron2AlbedoTexture;
MComPtr<ID3D12Resource> rustedIron2NormalTexture;
MComPtr<ID3D12Resource> rustedIron2MetallicTexture;
MComPtr<ID3D12Resource> rustedIron2RoughnessTexture;


// ===================================================================
// === Helper: create a GPU-only DEFAULT buffer                   ===
// ===================================================================

MComPtr<ID3D12Resource> CreateDefaultBuffer(uint64_t size, const wchar_t* name)
{
    MComPtr<ID3D12Resource> buf;
    const D3D12_HEAP_PROPERTIES heap{ .Type = D3D12_HEAP_TYPE_DEFAULT };
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = size, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };
    /**
    * Buffers are ALWAYS created in D3D12_RESOURCE_STATE_COMMON regardless of the
    * value passed here. Passing anything else triggers validation warning #1328.
    * The transition to COPY_DEST is handled implicitly by the DX12 runtime for
    * DEFAULT-heap buffers when CopyBufferRegion is called (buffer promotion).
    */
    const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buf));
    if (FAILED(hr)) { SA_LOG((L"CreateDefaultBuffer {%1} failed!", name), Error, DX12, (L"Error code: %1", hr)); return nullptr; }
    buf->SetName(name);
    SA_LOG((L"CreateDefaultBuffer {%1} success.", name), Info, DX12, (L"\"%1\" [%2]", name, buf.Get()));
    return buf;
}


int main()
{
    // ================================================================
    // Initialization
    // ================================================================
    if (true)
    {
        SA::Debug::InitDefaultLogger();

        // --- GLFW ---
        {
            glfwSetErrorCallback(GLFWErrorCallback);
            glfwInit();
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            window = glfwCreateWindow(windowSize.x, windowSize.y, "FVTDX12_MeshShader-Window", nullptr, nullptr);
            if (!window) { SA_LOG(L"GLFW create window failed!", Error, GLFW); return EXIT_FAILURE; }
            SA_LOG("GLFW create window success.", Info, GLFW, window);
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }

        // ============================================================
        // Renderer
        // ============================================================
        {
            // Factory /* 0001-I */
            if (true)
            {
                UINT dxgiFactoryFlags = 0;

#if SA_DEBUG
                {
                    MComPtr<ID3D12Debug1> debugController;
                    const HRESULT hrDbg = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
                    if (SUCCEEDED(hrDbg)) { debugController->EnableDebugLayer(); debugController->SetEnableGPUBasedValidation(true); }
                    else SA_LOG(L"DebugController init failed.", Error, DX12, (L"Error Code: %1", hrDbg));

                    MComPtr<IDXGIInfoQueue> dxgiInfoQueue;
                    const HRESULT hrIQ = DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiInfoQueue));
                    if (SUCCEEDED(hrIQ))
                    {
                        dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, true);
                        dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR,      true);
                        dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING,    true);
                    }
                }
                dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

                const HRESULT hrFactory = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory));
                if (FAILED(hrFactory)) { SA_LOG(L"Create Factory failed!", Error, DX12, (L"Error Code: %1", hrFactory)); return EXIT_FAILURE; }
                SA_LOG(L"Create Factory success.", Info, DX12, factory.Get());
            }


            // Device /* 0002-I */
            if (true)
            {
                MComPtr<IDXGIAdapter3> adapter;
                const HRESULT hrGPU = factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
                if (FAILED(hrGPU)) { SA_LOG(L"Adapter not found!", Error, DX12, (L"Error Code: %1", hrGPU)); return EXIT_FAILURE; }

                const HRESULT hrDev = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
                if (FAILED(hrDev)) { SA_LOG(L"Create Device failed!", Error, DX12, (L"Error Code: %1", hrDev)); return EXIT_FAILURE; }
                device->SetName(L"Main Device");
                SA_LOG(L"Create Device success.", Info, DX12, device.Get());

                /**
                * Verify mesh shader support.
                * Mesh shaders require D3D_FEATURE_LEVEL_12_0 + Shader Model 6.5 +
                * either the Agility SDK 615 or Windows 11+.
                */
                {
                    D3D12_FEATURE_DATA_SHADER_MODEL smQuery{ .HighestShaderModel = D3D_SHADER_MODEL_6_5 };
                    const HRESULT hrSM = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &smQuery, sizeof(smQuery));
                    if (FAILED(hrSM) || smQuery.HighestShaderModel < D3D_SHADER_MODEL_6_5)
                    {
                        SA_LOG(L"Shader Model 6.5 not supported! Mesh shaders unavailable.", Error, DX12);
                        return EXIT_FAILURE;
                    }

                    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
                    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opts7, sizeof(opts7));
                    if (opts7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
                    {
                        SA_LOG(L"Mesh Shader Tier NOT supported on this GPU.", Error, DX12);
                        return EXIT_FAILURE;
                    }
                    SA_LOG(L"Mesh Shader support confirmed.", Info, DX12);
                }

#if SA_DEBUG
                {
                    MComPtr<ID3D12InfoQueue1> infoQueue;
                    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
                    {
                        infoQueue->RegisterMessageCallback(ValidationLayersDebugCallback, D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS, nullptr, &VLayerCallbackCookie);
                        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
                        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,      true);
                        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING,    true);
                    }
                }
#endif

                // Queue
                {
                    const D3D12_COMMAND_QUEUE_DESC desc{ .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
                    const HRESULT hrQ = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&graphicsQueue));
                    if (FAILED(hrQ)) { SA_LOG(L"Create Graphics Queue failed!", Error, DX12, (L"Error Code: %1", hrQ)); return EXIT_FAILURE; }
                    graphicsQueue->SetName(L"GraphicsQueue");
                    SA_LOG(L"Create Graphics Queue success.", Info, DX12, graphicsQueue.Get());
                }

                // Device fence
                {
                    deviceFenceEvent = CreateEvent(nullptr, false, false, nullptr);
                    if (!deviceFenceEvent) { SA_LOG(L"Create Device Fence Event failed!", Error, DX12); return EXIT_FAILURE; }
                    const HRESULT hrFence = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&deviceFence));
                    if (FAILED(hrFence)) { SA_LOG(L"Create Device Fence failed!", Error, DX12, (L"Error Code: %1", hrFence)); return EXIT_FAILURE; }
                    deviceFence->SetName(L"DeviceFence");
                    SA_LOG(L"Create Device Fence success.", Info, DX12, deviceFence.Get());
                }
            }


            // Swapchain /* 0003-I */
            if (true)
            {
                const DXGI_SWAP_CHAIN_DESC1 desc{
                    .Width = windowSize.x, .Height = windowSize.y,
                    .Format = sceneColorFormat, .Stereo = false,
                    .SampleDesc = { .Count = 1, .Quality = 0 },
                    .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
                    .BufferCount = bufferingCount,
                    .Scaling = DXGI_SCALING_STRETCH,
                    .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
                };

                MComPtr<IDXGISwapChain1> sc1;
                const HRESULT hrSC = factory->CreateSwapChainForHwnd(graphicsQueue.Get(), glfwGetWin32Window(window), &desc, nullptr, nullptr, &sc1);
                if (FAILED(hrSC)) { SA_LOG(L"Create Swapchain failed!", Error, DX12, (L"Error Code: %1", hrSC)); return EXIT_FAILURE; }
                sc1.As(&swapchain);

                for (uint32_t i = 0; i < bufferingCount; ++i)
                {
                    swapchain->GetBuffer(i, IID_PPV_ARGS(&swapchainImages[i]));
                    swapchainImages[i]->SetName((L"SwapchainBackBuffer [" + std::to_wstring(i) + L"]").c_str());
                }

                swapchainFenceEvent = CreateEvent(nullptr, false, false, nullptr);
                device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&swapchainFence));
                swapchainFence->SetName(L"SwapchainFence");
            }


            // Commands /* 0004-I */
            if (true)
            {
                for (uint32_t i = 0; i < bufferingCount; ++i)
                {
                    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAllocs[i]));
                    cmdAllocs[i]->SetName((L"CommandAlloc [" + std::to_wstring(i) + L"]").c_str());
                }

                /**
                * CHANGED: Request ID3D12GraphicsCommandList6 directly.
                * This interface adds DispatchMesh() which launches mesh-shader workloads.
                */
                const HRESULT hrCL = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAllocs[0].Get(), nullptr, IID_PPV_ARGS(&cmdList));
                if (FAILED(hrCL)) { SA_LOG(L"Create Command List failed!", Error, DX12, (L"Error Code: %1", hrCL)); return EXIT_FAILURE; }
                cmdList->SetName(L"CommandList");
                SA_LOG(L"Create Command List success.", Info, DX12, cmdList.Get());
                cmdList->Close();
            }


            // Scene Textures /* 0005-I */
            if (true)
            {
                // Color RT View Heap /* 0006-I1 */
                {
                    const D3D12_DESCRIPTOR_HEAP_DESC desc{ .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = bufferingCount };
                    device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&sceneRTViewHeap));
                    sceneRTViewHeap->SetName(L"SceneRTViewHeap");

                    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = sceneRTViewHeap->GetCPUDescriptorHandleForHeapStart();
                    const UINT rtvOff = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                    for (uint32_t i = 0; i < bufferingCount; ++i)
                    {
                        device->CreateRenderTargetView(swapchainImages[i].Get(), nullptr, rtvHandle);
                        rtvHandle.ptr += rtvOff;
                    }
                }

                // Depth texture /* 0005-I1 */
                {
                    const D3D12_RESOURCE_DESC desc{
                        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                        .Width = windowSize.x, .Height = windowSize.y,
                        .DepthOrArraySize = 1, .MipLevels = 1,
                        .Format = sceneDepthFormat,
                        .SampleDesc = { .Count = 1 },
                        .Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                    };
                    const D3D12_HEAP_PROPERTIES heap{ .Type = D3D12_HEAP_TYPE_DEFAULT };
                    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &sceneDepthClearValue, IID_PPV_ARGS(&sceneDepthTexture));
                    sceneDepthTexture->SetName(L"SceneDepthTexture");
                }

                // Depth DSV Heap /* 0006-I2 */
                {
                    const D3D12_DESCRIPTOR_HEAP_DESC desc{ .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV, .NumDescriptors = 1 };
                    device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&sceneDepthRTViewHeap));
                    sceneDepthRTViewHeap->SetName(L"SceneDepthViewHeap");
                    device->CreateDepthStencilView(sceneDepthTexture.Get(), nullptr, sceneDepthRTViewHeap->GetCPUDescriptorHandleForHeapStart());
                }
            }


            // Pipeline /* 0008-I */
            if (true)
            {
                // Viewport & Scissor
                viewport    = { .TopLeftX = 0, .TopLeftY = 0, .Width = float(windowSize.x), .Height = float(windowSize.y), .MinDepth = 0.0f, .MaxDepth = 1.0f };
                scissorRect = { .left = 0, .top = 0, .right = LONG(windowSize.x), .bottom = LONG(windowSize.y) };

                // Shader compiler
                {
                    DxcCreateInstance(CLSID_DxcUtils,     IID_PPV_ARGS(&shaderCompilerUtils));
                    DxcCreateInstance(CLSID_DxcCompiler,  IID_PPV_ARGS(&shaderCompiler));
                }

                // Lit pipeline (Mesh Shader version)
                {
                    // Root Signature /* 0008-1-I */
                    {
                        /**
                        * CHANGED root signature structure:
                        *   - Removed D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                        *     (mesh shaders bypass the input assembler entirely).
                        *   - Camera and Object CBVs are now SHADER_VISIBILITY_MESH.
                        *   - New descriptor table for mesh data SRVs (t5-t11), MESH visibility.
                        *
                        * Heap layout (pbrSphereSRVHeap, 12 descriptors):
                        *   slot 0  -> t0  PointLights        (pixel)
                        *   slot 1  -> t1  Albedo             (pixel)
                        *   slot 2  -> t2  Normal map         (pixel)
                        *   slot 3  -> t3  Metallic map       (pixel)
                        *   slot 4  -> t4  Roughness map      (pixel)
                        *   slot 5  -> t5  Meshlet descs      (mesh)
                        *   slot 6  -> t6  Meshlet vert idxs  (mesh)
                        *   slot 7  -> t7  Meshlet primitives (mesh)
                        *   slot 8  -> t8  Vertex positions   (mesh)
                        *   slot 9  -> t9  Vertex normals     (mesh)
                        *   slot 10 -> t10 Vertex tangents    (mesh)
                        *   slot 11 -> t11 Vertex UVs         (mesh)
                        */

                        const D3D12_DESCRIPTOR_RANGE1 pointLightRange{
                            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, .NumDescriptors = 1,
                            .BaseShaderRegister = 0, .RegisterSpace = 0,
                            .Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC,
                            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                        };

                        const D3D12_DESCRIPTOR_RANGE1 pbrTextureRange[]{
                            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND }, // Albedo
                            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND }, // Normal
                            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND }, // Metallic
                            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND }, // Roughness
                        };

                        /**
                        * NEW: Mesh data SRVs (t5-t11) — consumed by the mesh shader.
                        * All 7 slots are contiguous in the heap so one range covers them.
                        */
                        const D3D12_DESCRIPTOR_RANGE1 meshDataRange{
                            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, .NumDescriptors = 7,
                            .BaseShaderRegister = 5, .RegisterSpace = 0,
                            .Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC,
                            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                        };

                        const D3D12_ROOT_PARAMETER1 params[]{
                            // [0] Camera CBV b0 — mesh shader
                            {
                                .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
                                .Descriptor = { .ShaderRegister = 0, .RegisterSpace = 0, .Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE },
                                .ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH,
                            },
                            // [1] Object CBV b1 — mesh shader
                            {
                                .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
                                .Descriptor = { .ShaderRegister = 1, .RegisterSpace = 0, .Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE },
                                .ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH,
                            },
                            // [2] Mesh data SRVs (t5-t11) — mesh shader
                            {
                                .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                                .DescriptorTable = { .NumDescriptorRanges = 1, .pDescriptorRanges = &meshDataRange },
                                .ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH,
                            },
                            // [3] PointLights SRV (t0) — pixel shader
                            {
                                .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                                .DescriptorTable = { .NumDescriptorRanges = 1, .pDescriptorRanges = &pointLightRange },
                                .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
                            },
                            // [4] PBR textures (t1-t4) — pixel shader
                            {
                                .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                                .DescriptorTable = { .NumDescriptorRanges = _countof(pbrTextureRange), .pDescriptorRanges = pbrTextureRange },
                                .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
                            },
                        };

                        const D3D12_STATIC_SAMPLER_DESC sampler{
                            .Filter = D3D12_FILTER_ANISOTROPIC,
                            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP, .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP, .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                            .MaxAnisotropy = 16, .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS,
                            .MinLOD = 0.0f, .MaxLOD = D3D12_FLOAT32_MAX,
                            .ShaderRegister = 0, .RegisterSpace = 0, .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL
                        };

                        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc{
                            .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
                            .Desc_1_1{
                                .NumParameters    = _countof(params),
                                .pParameters      = params,
                                .NumStaticSamplers = 1,
                                .pStaticSamplers  = &sampler,
                                /**
                                * CHANGED: removed D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT.
                                * Mesh shaders do not use the Input Assembler stage at all.
                                * Omitting this flag is a minor optimization (driver can skip IA setup).
                                */
                                .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
                            }
                        };

                        MComPtr<ID3DBlob> signature, error;
                        const HRESULT hrSer = D3D12SerializeVersionedRootSignature(&rsDesc, &signature, &error);
                        if (FAILED(hrSer))
                        {
                            std::string e(static_cast<char*>(error->GetBufferPointer()), error->GetBufferSize());
                            SA_LOG(L"Serialize Lit RootSignature failed!", Error, DX12, e);
                            return EXIT_FAILURE;
                        }
                        const HRESULT hrRS = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&litRootSign));
                        if (FAILED(hrRS)) { SA_LOG(L"Create Lit RootSignature failed!", Error, DX12, (L"Error Code: %1", hrRS)); return EXIT_FAILURE; }
                        SA_LOG(L"Create Lit RootSignature success.", Info, DX12, litRootSign.Get());
                    }


                    // Mesh Shader (replaces Vertex Shader)
                    {
                        /**
                        * CHANGED: compile mainMS with target ms_6_5.
                        * ms_6_5 requires SM 6.5 and the Agility SDK (see FORCE_AGILITY_SDK_615).
                        */
                        litMeshShader = CompileShader(L"Resources/Shaders/HLSL/MeshShader.hlsl", L"mainMS", L"ms_6_5");
                        if (!litMeshShader) return EXIT_FAILURE;
                    }

                    // Pixel Shader
                    {
                        litPixelShader = CompileShader(L"Resources/Shaders/HLSL/MeshShader.hlsl", L"mainPS", L"ps_6_5");
                        if (!litPixelShader) return EXIT_FAILURE;
                    }


                    // Helper lambda: build a MeshShaderPipelineStateStream and call CreatePipelineState.
                    // Used for both the lit and debug pipelines to avoid duplicating the boilerplate.
                    auto CreateMeshPipeline = [&](ID3DBlob* ms, ID3DBlob* ps, MComPtr<ID3D12PipelineState>& outPSO, const wchar_t* name) -> bool
                    {
                        const D3D12_RENDER_TARGET_BLEND_DESC rtBlend{
                            .BlendEnable = false, .LogicOpEnable = false,
                            .SrcBlend = D3D12_BLEND_ONE,   .DestBlend = D3D12_BLEND_ZERO,  .BlendOp = D3D12_BLEND_OP_ADD,
                            .SrcBlendAlpha = D3D12_BLEND_ONE, .DestBlendAlpha = D3D12_BLEND_ZERO, .BlendOpAlpha = D3D12_BLEND_OP_ADD,
                            .LogicOp = D3D12_LOGIC_OP_NOOP, .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
                        };
                        const D3D12_BLEND_DESC blendDesc{
                            .RenderTarget = { rtBlend, rtBlend, rtBlend, rtBlend, rtBlend, rtBlend, rtBlend, rtBlend }
                        };
                        const D3D12_RASTERIZER_DESC rasterDesc{
                            .FillMode = D3D12_FILL_MODE_SOLID, .CullMode = D3D12_CULL_MODE_BACK,
                            .FrontCounterClockwise = FALSE,
                            .DepthBias = D3D12_DEFAULT_DEPTH_BIAS, .DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
                            .SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
                            .DepthClipEnable = TRUE,
                        };
                        const D3D12_DEPTH_STENCIL_DESC dsDesc{
                            .DepthEnable = TRUE, .DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL,
                            .DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL, .StencilEnable = FALSE,
                        };

                        MeshShaderPipelineStateStream pss;
                        pss.rootSignature.data = litRootSign.Get();
                        pss.ms.data            = { ms->GetBufferPointer(), ms->GetBufferSize() };
                        pss.ps.data            = { ps->GetBufferPointer(), ps->GetBufferSize() };
                        pss.blend.data         = blendDesc;
                        pss.rasterizer.data    = rasterDesc;
                        pss.depthStencil.data  = dsDesc;
                        pss.rtFormats.data     = { { sceneColorFormat, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
                                                     DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN }, 1u };
                        pss.dsvFormat.data     = sceneDepthFormat;
                        pss.sampleDesc.data    = { .Count = 1, .Quality = 0 };

                        const D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{
                            .SizeInBytes                   = sizeof(MeshShaderPipelineStateStream),
                            .pPipelineStateSubobjectStream = &pss,
                        };

                        MComPtr<ID3D12Device2> device2;
                        device->QueryInterface(IID_PPV_ARGS(&device2));
                        const HRESULT hrPSO = device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&outPSO));
                        if (FAILED(hrPSO)) { SA_LOG((L"Create PipelineState {%1} failed!", name), Error, DX12, (L"Error Code: %1", hrPSO)); return false; }
                        outPSO->SetName(name);
                        SA_LOG((L"Create PipelineState {%1} success.", name), Info, DX12, outPSO.Get());
                        return true;
                    };


                    // Pipeline State — Lit
                    {
                        /**
                        * CHANGED: mesh-shader pipelines cannot be created with
                        * CreateGraphicsPipelineState() (that function has no AS/MS slots).
                        * We must use CreatePipelineState() with a D3D12_PIPELINE_STATE_STREAM_DESC.
                        */
                        if (!CreateMeshPipeline(litMeshShader.Get(), litPixelShader.Get(), litPipelineState, L"LitMeshPipeline"))
                            return EXIT_FAILURE;
                    }


                    // Pipeline State — Meshlet Debug
                    {
                        /**
                        * Debug shaders live in MeshShaderDebug.hlsl.
                        * mainMS_Debug: identical mesh shader but also outputs meshletIndex.
                        * mainPS_Debug: ignores all PBR bindings; outputs MeshletIndexToColor().
                        *
                        * Uses the same root signature as the lit pipeline so no descriptor
                        * rebinding is needed when toggling modes at runtime.
                        */
                        debugMeshShader = CompileShader(L"Resources/Shaders/HLSL/MeshShaderDebug.hlsl", L"mainMS_Debug", L"ms_6_5");
                        if (!debugMeshShader) return EXIT_FAILURE;

                        debugPixelShader = CompileShader(L"Resources/Shaders/HLSL/MeshShaderDebug.hlsl", L"mainPS_Debug", L"ps_6_5");
                        if (!debugPixelShader) return EXIT_FAILURE;

                        if (!CreateMeshPipeline(debugMeshShader.Get(), debugPixelShader.Get(), debugPipelineState, L"DebugMeshletPipeline"))
                            return EXIT_FAILURE;
                    }
                }
            }


            cmdList->Reset(cmdAllocs[0].Get(), nullptr);


            // Scene Objects /* 0009-I */
            if (true)
            {
                // SRV Heap
                // CHANGED: 5 slots -> 12 slots (added 7 for mesh data).
                {
                    const D3D12_DESCRIPTOR_HEAP_DESC desc{
                        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                        .NumDescriptors = 12,   // 0:pointLights 1-4:PBR 5-11:mesh data
                        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                    };
                    device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&pbrSphereSRVHeap));
                    pbrSphereSRVHeap->SetName(L"PBR+Mesh SRV Heap");
                    SA_LOG(L"Create PBR+Mesh SRV Heap success.", Info, DX12, pbrSphereSRVHeap.Get());
                }

                // Camera Buffers (upload heap – updated every frame)
                {
                    const D3D12_HEAP_PROPERTIES heap{ .Type = D3D12_HEAP_TYPE_UPLOAD };
                    const D3D12_RESOURCE_DESC desc{
                        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                        .Width = sizeof(CameraUBO), .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
                        .Format = DXGI_FORMAT_UNKNOWN, .SampleDesc = { .Count = 1 }, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                    };
                    for (uint32_t i = 0; i < bufferingCount; ++i)
                    {
                        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&cameraBuffers[i]));
                        cameraBuffers[i]->SetName((L"CameraBuffer [" + std::to_wstring(i) + L"]").c_str());
                    }
                }

                // Sphere Object Buffer
                {
                    sphereObjectBuffer = CreateDefaultBuffer(sizeof(ObjectUBO), L"SphereObjectBuffer");
                    if (!sphereObjectBuffer) return EXIT_FAILURE;
                    const SA::Mat4f tr = SA::Mat4f::MakeTranslation(spherePosition);
                    if (!SubmitBufferToGPU(sphereObjectBuffer, sizeof(ObjectUBO), &tr, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER))
                        return EXIT_FAILURE;
                }

                // PointLights Buffer
                {
                    const uint64_t size = pointLightNum * sizeof(PointLightUBO);
                    pointLightBuffer = CreateDefaultBuffer(size, L"PointLightsBuffer");
                    if (!pointLightBuffer) return EXIT_FAILURE;

                    std::array<PointLightUBO, pointLightNum> lights{
                        PointLightUBO{ .position = { -0.25f, -1.0f, 0.0f }, .intensity = 4.0f, .color = { 1.0f, 1.0f, 0.0f }, .radius = 3.0f },
                        PointLightUBO{ .position = {  1.75f,  2.0f, 1.0f }, .intensity = 7.0f, .color = { 0.0f, 1.0f, 1.0f }, .radius = 4.0f },
                    };

                    if (!SubmitBufferToGPU(pointLightBuffer, size, lights.data(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
                        return EXIT_FAILURE;

                    // SRV -> slot 0
                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{
                        .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                        .Buffer = { .NumElements = pointLightNum, .StructureByteStride = sizeof(PointLightUBO) }
                    };
                    device->CreateShaderResourceView(pointLightBuffer.Get(), &srvDesc, pbrSphereSRVHeap->GetCPUDescriptorHandleForHeapStart());
                }
            }


            // Resources /* 0010-I */
            if (true)
            {
                const UINT srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = pbrSphereSRVHeap->GetCPUDescriptorHandleForHeapStart();

                Assimp::Importer importer;

                // --------------------------------------------------------
                // Sphere mesh  (CHANGED: load into SRV buffers, build meshlets)
                // --------------------------------------------------------
                {
                    const char* path = "Resources/Models/Shapes/sphere.obj";
                    const aiScene* scene = importer.ReadFile(path, aiProcess_CalcTangentSpace | aiProcess_ConvertToLeftHanded);
                    if (!scene) { SA_LOG(L"Assimp loading failed!", Error, Assimp, path); return EXIT_FAILURE; }

                    const aiMesh* mesh = scene->mMeshes[0];

                    // ---- Vertex position buffer -> t8 ----
                    {
                        const uint64_t sz = sizeof(SA::Vec3f) * mesh->mNumVertices;
                        spherePosBuffer = CreateDefaultBuffer(sz, L"SpherePositionBuffer");
                        if (!spherePosBuffer) return EXIT_FAILURE;
                        if (!SubmitBufferToGPU(spherePosBuffer, sz, mesh->mVertices, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                            return EXIT_FAILURE;
                    }

                    // ---- Vertex normal buffer -> t9 ----
                    {
                        const uint64_t sz = sizeof(SA::Vec3f) * mesh->mNumVertices;
                        sphereNormBuffer = CreateDefaultBuffer(sz, L"SphereNormalBuffer");
                        if (!sphereNormBuffer) return EXIT_FAILURE;
                        if (!SubmitBufferToGPU(sphereNormBuffer, sz, mesh->mNormals, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                            return EXIT_FAILURE;
                    }

                    // ---- Vertex tangent buffer -> t10 ----
                    {
                        const uint64_t sz = sizeof(SA::Vec3f) * mesh->mNumVertices;
                        sphereTangBuffer = CreateDefaultBuffer(sz, L"SphereTangentBuffer");
                        if (!sphereTangBuffer) return EXIT_FAILURE;
                        if (!SubmitBufferToGPU(sphereTangBuffer, sz, mesh->mTangents, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                            return EXIT_FAILURE;
                    }

                    // ---- Vertex UV buffer -> t11 ----
                    {
                        std::vector<SA::Vec2f> uvs;
                        uvs.reserve(mesh->mNumVertices);
                        for (uint32_t i = 0; i < mesh->mNumVertices; ++i)
                            uvs.push_back({ mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y });

                        const uint64_t sz = sizeof(SA::Vec2f) * mesh->mNumVertices;
                        sphereUVBuffer = CreateDefaultBuffer(sz, L"SphereUVBuffer");
                        if (!sphereUVBuffer) return EXIT_FAILURE;
                        if (!SubmitBufferToGPU(sphereUVBuffer, sz, uvs.data(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                            return EXIT_FAILURE;
                    }

                    // ---- Build meshlets from the index buffer (CPU-side only) ----
                    {
                        // Pack Assimp 32-bit faces into uint16_t indices.
                        const uint32_t triCount = mesh->mNumFaces;
                        std::vector<uint16_t> indices(triCount * 3);
                        for (uint32_t i = 0; i < triCount; ++i)
                        {
                            indices[i*3+0] = uint16_t(mesh->mFaces[i].mIndices[0]);
                            indices[i*3+1] = uint16_t(mesh->mFaces[i].mIndices[1]);
                            indices[i*3+2] = uint16_t(mesh->mFaces[i].mIndices[2]);
                        }

                        std::vector<MeshletDesc>     meshlets;
                        std::vector<uint32_t>        meshletVerts;
                        std::vector<MeshletPrimitive> meshletPrims;
                        BuildMeshlets(indices.data(), triCount, meshlets, meshletVerts, meshletPrims);
                        sphereMeshletCount = static_cast<uint32_t>(meshlets.size());

                        SA_LOG((L"Sphere meshlet count: %1", sphereMeshletCount), Info, DX12);

                        // Upload meshlet descriptor buffer -> t5
                        {
                            const uint64_t sz = sizeof(MeshletDesc) * meshlets.size();
                            sphereMeshletBuf = CreateDefaultBuffer(sz, L"SphereMeshletDescBuf");
                            if (!sphereMeshletBuf) return EXIT_FAILURE;
                            if (!SubmitBufferToGPU(sphereMeshletBuf, sz, meshlets.data(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                                return EXIT_FAILURE;
                        }

                        // Upload meshlet vertex index buffer -> t6
                        {
                            const uint64_t sz = sizeof(uint32_t) * meshletVerts.size();
                            sphereMeshletVertBuf = CreateDefaultBuffer(sz, L"SphereMeshletVertBuf");
                            if (!sphereMeshletVertBuf) return EXIT_FAILURE;
                            if (!SubmitBufferToGPU(sphereMeshletVertBuf, sz, meshletVerts.data(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                                return EXIT_FAILURE;
                        }

                        // Upload meshlet primitive buffer -> t7
                        {
                            const uint64_t sz = sizeof(MeshletPrimitive) * meshletPrims.size();
                            sphereMeshletPrimBuf = CreateDefaultBuffer(sz, L"SphereMeshletPrimBuf");
                            if (!sphereMeshletPrimBuf) return EXIT_FAILURE;
                            if (!SubmitBufferToGPU(sphereMeshletPrimBuf, sz, meshletPrims.data(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                                return EXIT_FAILURE;
                        }
                    }

                    // ---- Create SRVs for mesh data (heap slots 5-11) ----
                    {
                        // Advance past PointLights (slot 0) and PBR textures (slots 1-4).
                        D3D12_CPU_DESCRIPTOR_HANDLE h = pbrSphereSRVHeap->GetCPUDescriptorHandleForHeapStart();
                        h.ptr += srvStride * 5; // start at slot 5

                        auto makeSRV = [&](ID3D12Resource* res, UINT numElems, UINT stride)
                        {
                            D3D12_SHADER_RESOURCE_VIEW_DESC d{
                                .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                                .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                .Buffer = { .NumElements = numElems, .StructureByteStride = stride }
                            };
                            device->CreateShaderResourceView(res, &d, h);
                            h.ptr += srvStride;
                        };

                        makeSRV(sphereMeshletBuf.Get(),     sphereMeshletCount,                       sizeof(MeshletDesc));
                        // meshletVerts count = sum of all vertexCounts — derive from buffer size.
                        makeSRV(sphereMeshletVertBuf.Get(), UINT(sphereMeshletVertBuf->GetDesc().Width / sizeof(uint32_t)),      sizeof(uint32_t));
                        makeSRV(sphereMeshletPrimBuf.Get(), UINT(sphereMeshletPrimBuf->GetDesc().Width / sizeof(MeshletPrimitive)), sizeof(MeshletPrimitive));
                        makeSRV(spherePosBuffer.Get(),      mesh->mNumVertices, sizeof(SA::Vec3f));
                        makeSRV(sphereNormBuffer.Get(),     mesh->mNumVertices, sizeof(SA::Vec3f));
                        makeSRV(sphereTangBuffer.Get(),     mesh->mNumVertices, sizeof(SA::Vec3f));
                        makeSRV(sphereUVBuffer.Get(),       mesh->mNumVertices, sizeof(SA::Vec2f));
                    }
                }

                // --------------------------------------------------------
                // PBR Textures  (unchanged, slots 1-4)
                // --------------------------------------------------------
                {
                    stbi_set_flip_vertically_on_load(true);

                    // Advance CPU handle to slot 1 for PBR textures.
                    cpuHandle.ptr += srvStride; // skip slot 0 (pointLights)

                    auto loadAndUploadTexture = [&](const char* path, int desiredChannels, DXGI_FORMAT fmt,
                                                    MComPtr<ID3D12Resource>& outTex, const wchar_t* name) -> bool
                    {
                        int w, h, ch;
                        char* raw = reinterpret_cast<char*>(stbi_load(path, &w, &h, &ch, desiredChannels));
                        if (!raw) { SA_LOG((L"STBI load {%1} failed", path), Error, STB, stbi_failure_reason()); return false; }
                        ch = desiredChannels;

                        std::vector<char> data(raw, raw + w * h * ch);
                        uint32_t mipLevels = 0, totalSize = 0;
                        std::vector<SA::Vec2ui> extents;
                        GenerateMipMapsCPU({ uint32_t(w), uint32_t(h) }, data, mipLevels, totalSize, extents, ch);

                        const D3D12_HEAP_PROPERTIES heap{ .Type = D3D12_HEAP_TYPE_DEFAULT };
                        const D3D12_RESOURCE_DESC desc{
                            .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                            .Width = uint32_t(w), .Height = uint32_t(h),
                            .DepthOrArraySize = 1, .MipLevels = UINT16(mipLevels),
                            .Format = fmt, .SampleDesc = { .Count = 1 },
                        };
                        const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTex));
                        if (FAILED(hr)) { SA_LOG((L"Create Texture {%1} failed", name), Error, DX12, (L"Error code: %1", hr)); stbi_image_free(raw); return false; }
                        outTex->SetName(name);
                        SA_LOG((L"Create Texture {%1} success.", name), Info, DX12, outTex.Get());

                        if (!SubmitTextureToGPU(outTex, extents, totalSize, ch, data.data())) { stbi_image_free(raw); return false; }
                        stbi_image_free(raw);

                        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{ .Format = fmt, .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
                            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                            .Texture2D = { .MipLevels = mipLevels } };
                        device->CreateShaderResourceView(outTex.Get(), &srvDesc, cpuHandle);
                        cpuHandle.ptr += srvStride;
                        return true;
                    };

                    if (!loadAndUploadTexture("Resources/Textures/RustedIron2/rustediron2_basecolor.png", 4, DXGI_FORMAT_R8G8B8A8_UNORM, rustedIron2AlbedoTexture,    L"RustedIron2 Albedo"))   return EXIT_FAILURE;
                    if (!loadAndUploadTexture("Resources/Textures/RustedIron2/rustediron2_normal.png",    4, DXGI_FORMAT_R8G8B8A8_UNORM, rustedIron2NormalTexture,    L"RustedIron2 Normal"))   return EXIT_FAILURE;
                    if (!loadAndUploadTexture("Resources/Textures/RustedIron2/rustediron2_metallic.png",  1, DXGI_FORMAT_R8_UNORM,       rustedIron2MetallicTexture,  L"RustedIron2 Metallic")) return EXIT_FAILURE;
                    if (!loadAndUploadTexture("Resources/Textures/RustedIron2/rustediron2_roughness.png", 1, DXGI_FORMAT_R8_UNORM,       rustedIron2RoughnessTexture, L"RustedIron2 Roughness"))return EXIT_FAILURE;
                }
            }

            cmdList->Close();
        }
    }


    // ================================================================
    // Loop
    // ================================================================
    if (true)
    {
        double oldMouseX = 0.0, oldMouseY = 0.0;
        float dx = 0.0f, dy = 0.0f;
        glfwGetCursorPos(window, &oldMouseX, &oldMouseY);

        const float fixedTime = 0.0025f;
        float accumulateTime = 0.0f;
        auto start = std::chrono::steady_clock::now();

        while (!glfwWindowShouldClose(window))
        {
            auto end = std::chrono::steady_clock::now();
            float deltaTime = std::chrono::duration_cast<std::chrono::duration<float, std::milli>>(end - start).count();
            accumulateTime += deltaTime;
            start = end;

            if (accumulateTime >= fixedTime)
            {
                accumulateTime -= fixedTime;
                glfwPollEvents();

                // Input
                if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, true);
                if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) cameraTr.position += fixedTime * cameraMoveSpeed * cameraTr.Right();
                if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) cameraTr.position -= fixedTime * cameraMoveSpeed * cameraTr.Right();
                if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) cameraTr.position += fixedTime * cameraMoveSpeed * cameraTr.Up();
                if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) cameraTr.position -= fixedTime * cameraMoveSpeed * cameraTr.Up();
                if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) cameraTr.position += fixedTime * cameraMoveSpeed * cameraTr.Forward();
                if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) cameraTr.position -= fixedTime * cameraMoveSpeed * cameraTr.Forward();

                // F1: toggle meshlet debug visualisation
                {
                    static bool f1WasDown = false;
                    const bool  f1IsDown  = glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS;
                    if (f1IsDown && !f1WasDown)
                    {
                        debugMeshletsMode = !debugMeshletsMode;
                        SA_LOG((L"Meshlet debug mode: %1", debugMeshletsMode ? L"ON" : L"OFF"), Info, DX12);
                    }
                    f1WasDown = f1IsDown;
                }

                double mouseX = 0.0, mouseY = 0.0;
                glfwGetCursorPos(window, &mouseX, &mouseY);
                if (mouseX != oldMouseX || mouseY != oldMouseY)
                {
                    dx += float(mouseX - oldMouseX) * fixedTime * cameraRotSpeed * SA::Maths::DegToRad<float>;
                    dy += float(mouseY - oldMouseY) * fixedTime * cameraRotSpeed * SA::Maths::DegToRad<float>;
                    oldMouseX = mouseX; oldMouseY = mouseY;
                    dx = dx >  SA::Maths::Pi<float> ? dx - SA::Maths::Pi<float> : dx < -SA::Maths::Pi<float> ? dx + SA::Maths::Pi<float> : dx;
                    dy = dy >  SA::Maths::Pi<float> ? dy - SA::Maths::Pi<float> : dy < -SA::Maths::Pi<float> ? dy + SA::Maths::Pi<float> : dy;
                    cameraTr.rotation = SA::Quatf(cos(dx), 0, sin(dx), 0) * SA::Quatf(cos(dy), sin(dy), 0, 0);
                }
            }


            // ---- Render ----
            {
                // Swapchain Begin
                {
                    const UINT32 prevVal = swapchainFenceValues[swapchainFrameIndex];
                    swapchainFrameIndex  = swapchain->GetCurrentBackBufferIndex();
                    const UINT32 currVal = swapchainFenceValues[swapchainFrameIndex];
                    if (swapchainFence->GetCompletedValue() < currVal)
                    {
                        swapchainFence->SetEventOnCompletion(currVal, swapchainFenceEvent);
                        WaitForSingleObjectEx(swapchainFenceEvent, INFINITE, FALSE);
                    }
                    swapchainFenceValues[swapchainFrameIndex] = prevVal + 1;
                }

                // Update Camera
                auto cameraBuffer = cameraBuffers[swapchainFrameIndex];
                {
                    CameraUBO ubo;
                    ubo.view        = cameraTr.Matrix();
                    const SA::Mat4f proj = SA::Mat4f::MakePerspective(cameraFOV, float(windowSize.x) / float(windowSize.y), cameraNear, cameraFar);
                    ubo.invViewProj = proj * ubo.view.GetInversed();

                    const D3D12_RANGE r{ .Begin = 0, .End = 0 };
                    void* data = nullptr;
                    cameraBuffer->Map(0, &r, reinterpret_cast<void**>(&data));
                    std::memcpy(data, &ubo, sizeof(CameraUBO));
                    cameraBuffer->Unmap(0, nullptr);
                }

                // Record Commands
                {
                    auto& cmdAlloc = cmdAllocs[swapchainFrameIndex];
                    cmdAlloc->Reset();
                    cmdList->Reset(cmdAlloc.Get(), nullptr);

                    auto sceneColorRT = swapchainImages[swapchainFrameIndex];

                    // Transition color -> RenderTarget
                    {
                        const D3D12_RESOURCE_BARRIER barrier{
                            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                            .Transition = { .pResource = sceneColorRT.Get(), .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                            .StateBefore = D3D12_RESOURCE_STATE_COMMON, .StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET }
                        };
                        cmdList->ResourceBarrier(1, &barrier);
                    }

                    // Bind & Clear RTs
                    {
                        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = sceneRTViewHeap->GetCPUDescriptorHandleForHeapStart();
                        rtvHandle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) * swapchainFrameIndex;
                        const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = sceneDepthRTViewHeap->GetCPUDescriptorHandleForHeapStart();

                        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
                        cmdList->ClearRenderTargetView(rtvHandle, sceneClearColor, 0, nullptr);
                        cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, sceneDepthClearValue.DepthStencil.Depth, 0, 0, nullptr);
                    }

                    cmdList->RSSetViewports(1, &viewport);
                    cmdList->RSSetScissorRects(1, &scissorRect);

                    // Mesh Shader Draw
                    {
                        // Bind the single CBV_SRV_UAV heap.
                        ID3D12DescriptorHeap* heaps[] = { pbrSphereSRVHeap.Get() };
                        cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

                        cmdList->SetGraphicsRootSignature(litRootSign.Get());

                        // [0] Camera CBV (mesh shader)
                        cmdList->SetGraphicsRootConstantBufferView(0, cameraBuffer->GetGPUVirtualAddress());

                        // [1] Object CBV (mesh shader)
                        cmdList->SetGraphicsRootConstantBufferView(1, sphereObjectBuffer->GetGPUVirtualAddress());

                        const UINT srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                        const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = pbrSphereSRVHeap->GetGPUDescriptorHandleForHeapStart();

                        // [2] Mesh data SRVs  (t5-t11, heap slots 5-11)
                        cmdList->SetGraphicsRootDescriptorTable(2, { gpuBase.ptr + srvStride * 5 });

                        // [3] PointLights SRV (t0, heap slot 0)
                        cmdList->SetGraphicsRootDescriptorTable(3, { gpuBase.ptr + srvStride * 0 });

                        // [4] PBR textures    (t1-t4, heap slots 1-4)
                        cmdList->SetGraphicsRootDescriptorTable(4, { gpuBase.ptr + srvStride * 1 });

                        /**
                        * Switch between the lit PBR pipeline and the meshlet debug pipeline.
                        * Both pipelines share litRootSign, so all descriptor bindings above
                        * remain valid regardless of which PSO is active.
                        *
                        * debugPipelineState uses mainMS_Debug + mainPS_Debug:
                        *   - mainMS_Debug forwards SV_GroupID.x as a nointerpolation meshletIndex.
                        *   - mainPS_Debug converts that index to a vivid hashed HSV color.
                        *     No PBR textures or lights are sampled in debug mode.
                        */
                        cmdList->SetPipelineState(
                            debugMeshletsMode ? debugPipelineState.Get()
                                             : litPipelineState.Get());

                        /**
                        * DispatchMesh(X, Y, Z) launches X*Y*Z mesh-shader thread groups.
                        * Each group (== one meshlet) reads its descriptor from meshlets[SV_GroupID.x].
                        * No IA state needs to be set – the IA stage is completely bypassed.
                        */
                        cmdList->DispatchMesh(sphereMeshletCount, 1, 1);
                    }

                    // Transition color -> Present
                    {
                        const D3D12_RESOURCE_BARRIER barrier{
                            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                            .Transition = { .pResource = sceneColorRT.Get(), .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                            .StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET, .StateAfter = D3D12_RESOURCE_STATE_PRESENT }
                        };
                        cmdList->ResourceBarrier(1, &barrier);
                    }

                    cmdList->Close();
                    ID3D12CommandList* lists[] = { cmdList.Get() };
                    graphicsQueue->ExecuteCommandLists(_countof(lists), lists);
                }

                // Swapchain End
                {
                    const HRESULT hrPresent = swapchain->Present(1, 0);
                    if (FAILED(hrPresent)) { SA_LOG(L"Swapchain Present failed", Error, DX12, (L"Error code: %1", hrPresent)); return EXIT_FAILURE; }

                    graphicsQueue->Signal(swapchainFence.Get(), swapchainFenceValues[swapchainFrameIndex]);
                }
            }

            SA_LOG_END_OF_FRAME();
        }
    }


    // ================================================================
    // Uninitialization
    // ================================================================
    if (true)
    {
        WaitDeviceIdle();

        // Resources
        rustedIron2RoughnessTexture = nullptr;
        rustedIron2MetallicTexture  = nullptr;
        rustedIron2NormalTexture    = nullptr;
        rustedIron2AlbedoTexture    = nullptr;

        // Sphere mesh buffers (SRVs)
        sphereUVBuffer          = nullptr;
        sphereTangBuffer        = nullptr;
        sphereNormBuffer        = nullptr;
        spherePosBuffer         = nullptr;
        sphereMeshletPrimBuf    = nullptr;
        sphereMeshletVertBuf    = nullptr;
        sphereMeshletBuf        = nullptr;

        // Scene Objects
        for (uint32_t i = 0; i < bufferingCount; ++i) cameraBuffers[i] = nullptr;
        sphereObjectBuffer = nullptr;
        pointLightBuffer   = nullptr;
        pbrSphereSRVHeap   = nullptr;

        // Pipeline
        // Debug pipeline
        debugPipelineState = nullptr;
        debugPixelShader   = nullptr;
        debugMeshShader    = nullptr;
        // litRootSign is shared — destroy after both pipelines are gone
        litPipelineState   = nullptr;
        litPixelShader     = nullptr;
        litMeshShader      = nullptr;
        litRootSign        = nullptr;
        shaderCompiler.Reset();
        shaderCompilerUtils.Reset();

        // Scene Textures
        sceneRTViewHeap      = nullptr;
        sceneDepthRTViewHeap = nullptr;
        sceneDepthTexture    = nullptr;

        // Commands
        cmdList = nullptr;
        for (uint32_t i = 0; i < bufferingCount; ++i) cmdAllocs[i] = nullptr;

        // Swapchain
        CloseHandle(swapchainFenceEvent); swapchainFenceEvent = nullptr;
        swapchainFence = nullptr;
        for (uint32_t i = 0; i < bufferingCount; ++i) swapchainImages[i] = nullptr;
        swapchain = nullptr;

        // Device
        CloseHandle(deviceFenceEvent); deviceFenceEvent = nullptr;
        deviceFence    = nullptr;
        graphicsQueue  = nullptr;

#if SA_DEBUG
        if (VLayerCallbackCookie)
        {
            MComPtr<ID3D12InfoQueue1> iq;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&iq))))
            { iq->UnregisterMessageCallback(VLayerCallbackCookie); VLayerCallbackCookie = 0; }
        }
#endif
        device = nullptr;

        // Factory
        factory = nullptr;

#if SA_DEBUG
        MComPtr<IDXGIDebug1> dxgiDbg;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDbg))))
            dxgiDbg->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
#endif

        // GLFW
        glfwDestroyWindow(window); window = nullptr;
        glfwTerminate();
    }

    return 0;
}