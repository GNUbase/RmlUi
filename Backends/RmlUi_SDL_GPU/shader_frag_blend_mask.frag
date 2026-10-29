Texture2D<float4> Texture : register(t0, space2);
Texture2D<float4> TextureMask : register(t1, space2);
SamplerState Sampler : register(s0, space2);
SamplerState SamplerMask : register(s1, space2);

float4 main(float4 Color : TEXCOORD0, float2 TexCoord : TEXCOORD1) : SV_Target0 {
    float4 texColor = Texture.Sample(Sampler, TexCoord);
    float maskAlpha = TextureMask.Sample(SamplerMask, TexCoord).a;
    return texColor * maskAlpha;
}
