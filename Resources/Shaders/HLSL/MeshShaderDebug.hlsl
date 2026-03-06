
struct DebugMeshVertex
{
    float4 svPosition    : SV_POSITION;

    nointerpolation uint meshletIndex : MESHLET_INDEX;
};


struct Camera
{
    float4x4 view;
    float4x4 invViewProj;
};
cbuffer CameraBuffer : register(b0) { Camera camera; };

struct Object
{
    float4x4 transform;
};
cbuffer ObjectBuffer : register(b1) { Object object; };

struct MeshletData
{
    uint vertexOffset;
    uint vertexCount;
    uint primOffset;
    uint primCount;
};

StructuredBuffer<MeshletData> meshlets     : register(t5);
StructuredBuffer<uint>        meshletVerts : register(t6);
StructuredBuffer<uint3>       meshletPrims : register(t7);
StructuredBuffer<float3>      positions    : register(t8);


#define MAX_VERTS 128u
#define MAX_PRIMS 128u

[outputtopology("triangle")]
[numthreads(MAX_VERTS, 1, 1)]
void mainMS_Debug(
    in  uint3              groupId  : SV_GroupID,
    in  uint               threadId : SV_GroupThreadID,
    out vertices DebugMeshVertex verts[MAX_VERTS],
    out indices  uint3           prims[MAX_PRIMS])
{
    const MeshletData m = meshlets[groupId.x];

    SetMeshOutputCounts(m.vertexCount, m.primCount);

    // One thread -> one vertex
    if (threadId < m.vertexCount)
    {
        const uint vi = meshletVerts[m.vertexOffset + threadId];

        const float4 worldPos4 = mul(object.transform, float4(positions[vi], 1.0f));
        verts[threadId].svPosition   = mul(camera.invViewProj, worldPos4);
        verts[threadId].meshletIndex = groupId.x;   // broadcast index to all verts
    }

    // One thread -> one primitive
    if (threadId < m.primCount)
    {
        prims[threadId] = meshletPrims[m.primOffset + threadId];
    }
}

struct DebugPixelInput : DebugMeshVertex { };

struct DebugPixelOutput
{
    float4 color : SV_TARGET;
};


float3 MeshletIndexToColor(uint index)
{
    uint h = index * 2654435761u;   // Knuth multiplicative hash
    float hue = float(h & 0xFFFFu) / 65536.0f;

    // Fixed saturation and value so all colors are vivid.
    const float S = 0.85f;
    const float V = 0.95f;

    // HSV to RGB
    float H6   = hue * 6.0f;
    float frac = H6 - floor(H6);
    uint  sect = uint(H6) % 6u;

    float p = V * (1.0f - S);
    float q = V * (1.0f - S * frac);
    float t = V * (1.0f - S * (1.0f - frac));

    float3 rgb;
    switch (sect)
    {
        case 0:  rgb = float3(V, t, p); break;
        case 1:  rgb = float3(q, V, p); break;
        case 2:  rgb = float3(p, V, t); break;
        case 3:  rgb = float3(p, q, V); break;
        case 4:  rgb = float3(t, p, V); break;
        default: rgb = float3(V, p, q); break;
    }
    return rgb;
}

DebugPixelOutput mainPS_Debug(DebugPixelInput _input)
{
    DebugPixelOutput output;
    output.color = float4(MeshletIndexToColor(_input.meshletIndex), 1.0f);
    return output;
}
