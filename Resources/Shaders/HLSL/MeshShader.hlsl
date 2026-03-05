//-------------------- Mesh Shader --------------------
//
// Replaces the traditional vertex shader + input assembler stage.
// Vertex data (positions, normals, tangents, UVs) is now passed as
// StructuredBuffers. CPU-generated Meshlets drive thread-group dispatch.
//
// Register layout (space0):
//   b0 = Camera CBV          (mesh shader)
//   b1 = Object CBV           (mesh shader)
//   t0 = PointLights SRV      (pixel shader)
//   t1 = Albedo texture       (pixel shader)
//   t2 = Normal map           (pixel shader)
//   t3 = Metallic map         (pixel shader)
//   t4 = Roughness map        (pixel shader)
//   t5 = Meshlet descriptors  (mesh shader)
//   t6 = Meshlet vertex index (mesh shader)
//   t7 = Meshlet primitives   (mesh shader)
//   t8 = Vertex positions     (mesh shader)
//   t9 = Vertex normals       (mesh shader)
//   t10= Vertex tangents      (mesh shader)
//   t11= Vertex UVs           (mesh shader)
//   s0 = PBR anisotropic sampler (pixel shader)


// ===================================================================
// === Shared Vertex Output (interpolated to pixel shader)         ===
// ===================================================================

struct MeshVertex
{
    /// Vertex world-space position.
    float3 worldPosition : POSITION;

    /// Clip-space position consumed by the rasterizer.
    float4 svPosition    : SV_POSITION;

    /// Camera world-space position (replicated per vertex for lighting).
    float3 viewPosition  : VIEW_POSITION;

    /// Tangent-Bitangent-Normal matrix for normal mapping.
    float3x3 TBN         : TBN;

    /// Texture coordinates.
    float2 uv            : TEXCOORD;
};


// ===================================================================
// === Mesh Shader Bindings                                        ===
// ===================================================================

struct Camera
{
    float4x4 view;
    float4x4 invViewProj; // projection * inverse(view)
};
cbuffer CameraBuffer : register(b0)
{
    Camera camera;
};

struct Object
{
    float4x4 transform;
};
cbuffer ObjectBuffer : register(b1)
{
    Object object;
};

/// CPU-generated meshlet descriptor (one per thread group).
struct MeshletData
{
    uint vertexOffset; ///< First entry in meshletVerts[] for this meshlet.
    uint vertexCount;  ///< Number of unique vertices in this meshlet (<= MAX_VERTS).
    uint primOffset;   ///< First entry in meshletPrims[] for this meshlet.
    uint primCount;    ///< Number of triangles in this meshlet (<= MAX_PRIMS).
};

StructuredBuffer<MeshletData> meshlets     : register(t5);
StructuredBuffer<uint>        meshletVerts : register(t6); // global vertex index per local slot
StructuredBuffer<uint3>       meshletPrims : register(t7); // local-index triangle per meshlet prim

StructuredBuffer<float3>      positions    : register(t8);
StructuredBuffer<float3>      normals      : register(t9);
StructuredBuffer<float3>      tangents     : register(t10);
StructuredBuffer<float2>      uvs          : register(t11);


// ===================================================================
// === Mesh Shader                                                 ===
// ===================================================================

#define MAX_VERTS 128u
#define MAX_PRIMS 128u

[outputtopology("triangle")]
[numthreads(MAX_VERTS, 1, 1)]
void mainMS(
    in  uint3       groupId  : SV_GroupID,
    in  uint        threadId : SV_GroupThreadID,
    out vertices MeshVertex  verts[MAX_VERTS],
    out indices  uint3       prims[MAX_PRIMS])
{
    const MeshletData m = meshlets[groupId.x];

    // Tell the rasterizer how many outputs this meshlet actually produces.
    SetMeshOutputCounts(m.vertexCount, m.primCount);

    // ------------------------------------------------------------------
    // One thread -> one vertex  (threads beyond vertexCount are idle).
    // ------------------------------------------------------------------
    if (threadId < m.vertexCount)
    {
        const uint vi = meshletVerts[m.vertexOffset + threadId];

        // World transform
        const float4 worldPos4 = mul(object.transform, float4(positions[vi], 1.0f));
        verts[threadId].worldPosition = worldPos4.xyz / worldPos4.w;
        verts[threadId].svPosition    = mul(camera.invViewProj, worldPos4);
        verts[threadId].viewPosition  = float3(camera.view._14, camera.view._24, camera.view._34);

        // TBN matrix (row-major constructor => transpose to get column-major TBN)
        const float3 n = normalize(mul((float3x3)object.transform, normals[vi]));
        const float3 t = normalize(mul((float3x3)object.transform, tangents[vi]));
        const float3 b = cross(n, t);
        verts[threadId].TBN = transpose(float3x3(t, b, n));

        verts[threadId].uv = uvs[vi];
    }

    // ------------------------------------------------------------------
    // One thread -> one primitive  (local indices into this meshlet).
    // ------------------------------------------------------------------
    if (threadId < m.primCount)
    {
        prims[threadId] = meshletPrims[m.primOffset + threadId];
    }
}


// ===================================================================
// === Pixel Shader  (unchanged from LitShader.hlsl)              ===
// ===================================================================

struct PixelInput : MeshVertex { };

struct PixelOutput
{
    float4 color : SV_TARGET;
};

static const float PI = 3.14159265359f;

// ---------- Pixel-shader bindings ----------

struct PointLight
{
    float3 position;
    float  intensity;
    float3 color;
    float  radius;
};

StructuredBuffer<PointLight> pointLights : register(t0);

Texture2D<float4> albedo      : register(t1);
Texture2D<float3> normalMap   : register(t2);
Texture2D<float>  metallicMap : register(t3);
Texture2D<float>  roughnessMap: register(t4);

SamplerState pbrSampler : register(s0);

// ---------- BRDF helpers ----------

float ComputeAttenuation(float3 vLight, float lightRange)
{
    return max(1.0f - length(vLight) / lightRange, 0.0f);
}

float3 FresnelSchlick(float3 f0, float cosTheta)
{
    return f0 + (1.0f - f0) * pow(1.0f - cosTheta, 5.0f);
}

float DistributionGGX(float cosAlpha, float roughness)
{
    const float r2    = roughness * roughness;
    const float denom = cosAlpha * cosAlpha * (r2 - 1.0f) + 1.0f;
    return r2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float cosRho, float roughness)
{
    const float r = roughness + 1.0f;
    const float k = (r * r) / 8.0f;
    return cosRho / (cosRho * (1.0f - k) + k);
}

float GeometrySmith(float cosTheta, float cosRho, float roughness)
{
    return GeometrySchlickGGX(cosRho, roughness) * GeometrySchlickGGX(cosTheta, roughness);
}

// ---------- Main ----------

PixelOutput mainPS(PixelInput _input)
{
    PixelOutput output;

    const float4 baseColor = albedo.Sample(pbrSampler, _input.uv);
    if (baseColor.a < 0.001f)
        discard;

    const float3 vnNormal  = normalize(mul(_input.TBN, normalMap.Sample(pbrSampler, _input.uv) * 2.0f - 1.0f));
    const float  metallic  = metallicMap.Sample(pbrSampler, _input.uv);
    const float  roughness = roughnessMap.Sample(pbrSampler, _input.uv);
    const float3 vnCamera  = normalize(_input.viewPosition - _input.worldPosition);
    const float3 f0        = lerp(float3(0.04f, 0.04f, 0.04f), baseColor.xyz, metallic);

    float3 finalColor = float3(0.0f, 0.0f, 0.0f);

    uint numLights, stride;
    pointLights.GetDimensions(numLights, stride);

    for (uint i = 0; i < numLights; ++i)
    {
        const PointLight pl = pointLights[i];

        const float3 vLight  = pl.position - _input.worldPosition;
        const float3 vnLight = normalize(vLight);

        const float cosTheta   = dot(vnNormal, vnLight);
        const float attenuation = ComputeAttenuation(vLight, pl.radius);

        if (cosTheta > 0.0f && attenuation > 0.0f)
        {
            const float3 vnHalf  = normalize(vnLight + vnCamera);
            const float  cosAlpha = dot(vnNormal, vnHalf);
            const float  cosRho   = dot(vnNormal, vnCamera);

            const float3 F = FresnelSchlick(f0, cosTheta);

            float3 specularBRDF = float3(0.0f, 0.0f, 0.0f);
            if (cosAlpha > 0.0f && cosRho > 0.0f)
            {
                const float NDF = DistributionGGX(cosAlpha, roughness);
                const float G   = GeometrySmith(cosTheta, cosRho, roughness);
                specularBRDF    = (NDF * G * F) / (4.0f * cosTheta * cosRho);
            }

            const float3 kD          = (1.0f - F) * (1.0f - metallic);
            const float3 diffuseBRDF = kD * baseColor.xyz / PI;

            finalColor += (diffuseBRDF + specularBRDF) * cosTheta * attenuation * pl.color * pl.intensity;
        }
    }

    output.color = float4(finalColor, 1.0f);
    return output;
}
