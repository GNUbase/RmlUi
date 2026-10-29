// Fullscreen passes: the quad's positions are already in clip space. The texture coordinates are scaled and offset, to
// read a part of the source texture or to shift it.
cbuffer UniformBlockTexCoordTransform : register(b0, space1) {
    float4 TexCoordTransform : packoffset(c0); // xy: scale, zw: offset
};

struct Input {
    float2 Position : TEXCOORD0;
    float4 InColor : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

struct Output {
    float4 Color : TEXCOORD0;
    float2 TexCoord : TEXCOORD1;
    float4 Position : SV_Position;
};

Output main(Input input) {
    Output output;
    output.Color = input.InColor;
    output.TexCoord = input.TexCoord * TexCoordTransform.xy + TexCoordTransform.zw;
    output.Position = float4(input.Position, 0, 1);
    return output;
}
