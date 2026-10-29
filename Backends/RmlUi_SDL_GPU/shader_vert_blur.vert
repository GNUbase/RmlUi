// One pass of a separable gaussian blur: seven texture coordinates around the fragment's, along the blur direction.
cbuffer UniformBlockBlur : register(b0, space1) {
    float4 TexelOffset : packoffset(c0); // xy: one texel along the blur direction
};

struct Input {
    float2 Position : TEXCOORD0;
    float4 InColor : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

struct Output {
    float2 TexCoord0 : TEXCOORD0;
    float2 TexCoord1 : TEXCOORD1;
    float2 TexCoord2 : TEXCOORD2;
    float2 TexCoord3 : TEXCOORD3;
    float2 TexCoord4 : TEXCOORD4;
    float2 TexCoord5 : TEXCOORD5;
    float2 TexCoord6 : TEXCOORD6;
    float4 Position : SV_Position;
};

Output main(Input input) {
    Output output;
    output.TexCoord0 = input.TexCoord + 3.0 * TexelOffset.xy;
    output.TexCoord1 = input.TexCoord + 2.0 * TexelOffset.xy;
    output.TexCoord2 = input.TexCoord + 1.0 * TexelOffset.xy;
    output.TexCoord3 = input.TexCoord;
    output.TexCoord4 = input.TexCoord - 1.0 * TexelOffset.xy;
    output.TexCoord5 = input.TexCoord - 2.0 * TexelOffset.xy;
    output.TexCoord6 = input.TexCoord - 3.0 * TexelOffset.xy;
    output.Position = float4(input.Position, 0, 1);
    return output;
}
