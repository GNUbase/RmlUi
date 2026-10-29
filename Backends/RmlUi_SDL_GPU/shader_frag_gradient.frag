#define LINEAR 0
#define RADIAL 1
#define CONIC 2
#define REPEATING_LINEAR 3
#define REPEATING_RADIAL 4
#define REPEATING_CONIC 5
#define PI 3.14159265
#define MAX_NUM_STOPS 16

cbuffer UniformBlockGradient : register(b0, space3) {
    int4 FuncNumStops : packoffset(c0);   // x: one of the functions above, y: number of stops
    float4 PV : packoffset(c1);           // xy: linear: starting point, radial: center, conic: center
                                          // zw: linear: vector to ending point, radial: 2d curvature (inverse radius),
                                          //     conic: angled unit vector
    float4 StopPositions[MAX_NUM_STOPS / 4] : packoffset(c2); // normalized, 0 -> starting point, 1 -> ending point
    float4 StopColors[MAX_NUM_STOPS] : packoffset(c6);
};

float StopPosition(int i) {
    return StopPositions[i / 4][i % 4];
}

float4 MixStopColors(float t) {
    float4 color = StopColors[0];
    for (int i = 1; i < FuncNumStops.y; i++)
        color = lerp(color, StopColors[i], smoothstep(StopPosition(i - 1), StopPosition(i), t));
    return color;
}

// GLSL's mod: the result has the sign of y.
float Modulo(float x, float y) {
    return x - y * floor(x / y);
}

float4 main(float4 Color : TEXCOORD0, float2 TexCoord : TEXCOORD1) : SV_Target0 {
    int func = FuncNumStops.x;
    float2 p = PV.xy;
    float2 v = PV.zw;
    float t = 0.0;

    if (func == LINEAR || func == REPEATING_LINEAR)
    {
        float dist_square = dot(v, v);
        float2 V = TexCoord - p;
        t = dot(v, V) / dist_square;
    }
    else if (func == RADIAL || func == REPEATING_RADIAL)
    {
        float2 V = TexCoord - p;
        t = length(v * V);
    }
    else if (func == CONIC || func == REPEATING_CONIC)
    {
        // GLSL's mat2(v.x, -v.y, v.y, v.x) is column-major: its rows are (v.x, v.y) and (-v.y, v.x).
        float2 W = TexCoord - p;
        float2 V = float2(v.x * W.x + v.y * W.y, -v.y * W.x + v.x * W.y);
        t = 0.5 + atan2(-V.x, V.y) / (2.0 * PI);
    }

    if (func == REPEATING_LINEAR || func == REPEATING_RADIAL || func == REPEATING_CONIC)
    {
        float t0 = StopPosition(0);
        float t1 = StopPosition(FuncNumStops.y - 1);
        t = t0 + Modulo(t - t0, t1 - t0);
    }

    return Color * MixStopColors(t);
}
