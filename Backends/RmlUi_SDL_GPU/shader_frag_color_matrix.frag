Texture2D<float4> Texture : register(t0, space2);
SamplerState Sampler : register(s0, space2);

cbuffer UniformBlockColorMatrix : register(b0, space3) {
    float4x4 ColorMatrix : packoffset(c0);
};

float4 main(float2 TexCoord : TEXCOORD0) : SV_Target0 {
    // The general case uses a 4x5 color matrix for full rgba transformation, plus a constant term with the last column.
    // However, we only consider the case of rgb transformations. Thus, we could in principle use a 3x4 matrix, but we
    // keep the alpha row for simplicity.
    // In the general case we should do the matrix transformation in non-premultiplied space. However, without alpha
    // transformations, we can do it directly in premultiplied space to avoid the extra division and multiplication
    // steps. In this space, the constant term needs to be multiplied by the alpha value, instead of unity.
    float4 texColor = Texture.Sample(Sampler, TexCoord);
    float3 transformedColor = mul(ColorMatrix, texColor).rgb;
    return float4(transformedColor, texColor.a);
}
