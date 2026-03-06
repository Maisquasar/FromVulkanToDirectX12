struct MeshVertex
{
    // Vertex world-space position.
    float3 worldPosition : POSITION;
    // Clip-space position
    float4 svPosition    : SV_POSITION;
    float3 viewPosition  : VIEW_POSITION;
    float3x3 TBN         : TBN;
    float2 uv            : TEXCOORD;
};


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

// CPU-generated meshlet descriptor (one per thread group).
struct MeshletData
{
    uint vertexOffset;
    uint vertexCount; 
    uint primOffset;  
    uint primCount;   
};

StructuredBuffer<MeshletData> meshlets     : register(t5);
StructuredBuffer<uint> meshletVerts : register(t6); // global vertex index per local slot
StructuredBuffer<uint3> meshletPrims : register(t7); // local-index triangle per meshlet prim

StructuredBuffer<float3> positions : register(t8);
StructuredBuffer<float3> normals : register(t9);
StructuredBuffer<float3> tangents : register(t10);
StructuredBuffer<float2> uvs : register(t11);

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

    SetMeshOutputCounts(m.vertexCount, m.primCount);

    // One thread = one vertex (threads beyond vertexCount are idle).
    if (threadId < m.vertexCount)
    {
        const uint vi = meshletVerts[m.vertexOffset + threadId];

        // World transform
        const float4 worldPos4 = mul(object.transform, float4(positions[vi], 1.0f));
        verts[threadId].worldPosition = worldPos4.xyz / worldPos4.w;
        verts[threadId].svPosition    = mul(camera.invViewProj, worldPos4);
        verts[threadId].viewPosition  = float3(camera.view._14, camera.view._24, camera.view._34);

        // TBN matrix
        const float3 n = normalize(mul((float3x3)object.transform, normals[vi]));
        const float3 t = normalize(mul((float3x3)object.transform, tangents[vi]));
        const float3 b = cross(n, t);
        verts[threadId].TBN = transpose(float3x3(t, b, n));

        verts[threadId].uv = uvs[vi];
    }

    // One thread = one primitive (local indices into this meshlet).
    if (threadId < m.primCount)
    {
        prims[threadId] = meshletPrims[m.primOffset + threadId];
    }
}

//-------------------- Pixel Shader --------------------

struct PixelInput : MeshVertex
{
};

struct PixelOutput
{
	float4 color  : SV_TARGET;
};

// Constants.
static const float PI = 3.14159265359;


//---------- Bindings ----------
struct PointLight
{
	float3 position;

	float intensity;

	float3 color;

	float radius;
};

StructuredBuffer<PointLight> pointLights : register(t0);


Texture2D<float4> albedo : register(t1);
Texture2D<float3> normalMap : register(t2);
Texture2D<float> metallicMap : register(t3);
Texture2D<float> roughnessMap : register(t4);

SamplerState pbrSampler : register(s0); // Use same sampler for all textures.


//---------- Helper Functions ----------
float ComputeAttenuation(float3 _vLight, float _lightRange)
{
	const float distance = length(_vLight);

	return max(1 - (distance / _lightRange), 0.0);
}

float3 FresnelSchlick(float3 _f0, float _cosTheta)
{
	return _f0 + (1.0 - _f0) * pow(1.0 - _cosTheta, 5.0);
}

float DistributionGGX(float _cosAlpha, float _roughness)
{
	// Normal distribution function: GGX model.
	const float roughSqr = _roughness * _roughness;

	const float denom = _cosAlpha * _cosAlpha * (roughSqr - 1.0) + 1.0;

	return roughSqr / (PI * denom * denom);
}

float GeometrySchlickGGX(float _cosRho, float _roughness)
{
	// Geometry distribution function: GGX model.

	const float r = _roughness + 1.0;

	const float k = (r * r) / 8.0;

	return _cosRho / (_cosRho * (1.0 - k) + k);
}
  
float GeometrySmith(float _cosTheta, float _cosRho, float _roughness)
{
	float ggx1 = GeometrySchlickGGX(_cosRho, _roughness);
	float ggx2 = GeometrySchlickGGX(_cosTheta, _roughness);
	
	return ggx1 * ggx2;
}


//---------- Main ----------
PixelOutput mainPS(PixelInput _input)
{
	PixelOutput output;


	//---------- Base Color ----------
	const float4 baseColor = albedo.Sample(pbrSampler, _input.uv);

	if (baseColor.a < 0.001)
		discard;


	//---------- Normal ----------
	const float3 vnNormal = normalize(mul(_input.TBN, normalMap.Sample(pbrSampler, _input.uv) * 2.0f - 1.0f));

	//---------- Lighting ----------
	const float metallic = metallicMap.Sample(pbrSampler, _input.uv);
	const float roughness = roughnessMap.Sample(pbrSampler, _input.uv);
	const float3 vnCamera = normalize(_input.viewPosition - _input.worldPosition);
	const float3 f0 = lerp(float3(0.04, 0.04, 0.04), baseColor.xyz, metallic);

	float3 finalColor = float3(0.0f, 0.0f, 0.0f);


	//----- Point Lights -----
	float3 sum = float3(0.0f, 0.0f, 0.0f);

	uint num;
	uint stride;
	pointLights.GetDimensions(num, stride);

	for(uint i = 0; i < num; ++i)
	{
		PointLight pLight = pointLights[i];

		const float3 vLight = pLight.position - _input.worldPosition;
		const float3 vnLight = normalize(vLight);

		//----- BRDF -----
		const float cosTheta = dot(vnNormal, vnLight);

		const float attenuation = ComputeAttenuation(vLight, pLight.radius);

		if (cosTheta > 0.0 && attenuation > 0.0)
		{
		//{ Specular Component

			// Halfway vector.
			const float3 vnHalf = normalize(vnLight + vnCamera);

			// Blinn-Phong variant. Phong formula is: dot(vnNormal, vnCamera)
			const float cosAlpha = dot(vnNormal, vnHalf);
			const float cosRho = dot(vnNormal, vnCamera);

			const float3 F = FresnelSchlick(f0, cosTheta);

			float3 specularBRDF = float3(0.0f, 0.0f, 0.0f);

			if(cosAlpha > 0.0 && cosRho > 0.0)
			{
				const float NDF = DistributionGGX(cosAlpha, roughness);
				const float G = GeometrySmith(cosTheta, cosRho, roughness);

				// Cook-Torrance specular BRDF.
				specularBRDF = (NDF * G * F) / (4.0 * cosTheta * cosRho);
			}

		//}


		//{ Diffuse Component

			const float3 kD = (float3(1.0f, 1.0f, 1.0f) - F) * (1.0 - metallic);

			// Lambert Diffuse.
			const float3 diffuseBRDF = kD * baseColor.xyz / PI;

		//}

			finalColor += (diffuseBRDF + specularBRDF) * cosTheta * attenuation * pLight.color * pLight.intensity;
		}
	}

	output.color = float4(finalColor, 1.0f);

	return output;
}
