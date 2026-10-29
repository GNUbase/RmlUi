// Fullscreen passes: the quad's positions are already in clip space. The texture coordinates are scaled and offset, to
// read a part of the source texture or to shift it. The texture coordinates are the only output, so that the fragment
// shaders' inputs start at the same register: Direct3D 12 links the stages by register, and compilers drop unused
// inputs.
cbuffer UniformBlockTexCoordTransform : register(b0, space1) {
    float4 TexCoordTransform : packoffset(c0); // xy: scale, zw: offset
};

struct Input {
    float2 Position : TEXCOORD0;
    float4 InColor : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

struct Output {
    float2 TexCoord : TEXCOORD0;
    float4 Position : SV_Position;
};

Output main(Input input) {
    Output output;
    output.TexCoord = input.TexCoord * TexCoordTransform.xy + TexCoordTransform.zw;
    output.Position = float4(input.Position, 0, 1);
    return output;
}
