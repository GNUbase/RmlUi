Texture2D<float4> Texture : register(t0, space2);
SamplerState Sampler : register(s0, space2);

cbuffer UniformBlockDropShadow : register(b0, space3) {
    float4 ShadowColor : packoffset(c0);
    float4 TexCoordLimits : packoffset(c1); // xy: min, zw: max
};

float4 main(float4 Color : TEXCOORD0, float2 TexCoord : TEXCOORD1) : SV_Target0 {
    float2 in_region = step(TexCoordLimits.xy, TexCoord) * step(TexCoord, TexCoordLimits.zw);
    return Texture.Sample(Sampler, TexCoord).a * in_region.x * in_region.y * ShadowColor;
}
