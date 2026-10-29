Texture2D<float4> Texture : register(t0, space2);
SamplerState Sampler : register(s0, space2);

cbuffer UniformBlockBlur : register(b0, space3) {
    float4 Weights : packoffset(c0);        // x: the center's weight, yzw: one, two and three texels away
    float4 TexCoordLimits : packoffset(c1); // xy: min, zw: max
};

struct Input {
    float2 TexCoord0 : TEXCOORD0;
    float2 TexCoord1 : TEXCOORD1;
    float2 TexCoord2 : TEXCOORD2;
    float2 TexCoord3 : TEXCOORD3;
    float2 TexCoord4 : TEXCOORD4;
    float2 TexCoord5 : TEXCOORD5;
    float2 TexCoord6 : TEXCOORD6;
};

float4 Tap(float2 tex_coord, float weight) {
    float2 in_region = step(TexCoordLimits.xy, tex_coord) * step(tex_coord, TexCoordLimits.zw);
    return Texture.Sample(Sampler, tex_coord) * in_region.x * in_region.y * weight;
}

float4 main(Input input) : SV_Target0 {
    return Tap(input.TexCoord0, Weights.w) + Tap(input.TexCoord1, Weights.z) + Tap(input.TexCoord2, Weights.y) +
        Tap(input.TexCoord3, Weights.x) + Tap(input.TexCoord4, Weights.y) + Tap(input.TexCoord5, Weights.z) +
        Tap(input.TexCoord6, Weights.w);
}
