// Clears what the fullscreen pass covers: transparent, or only the stencil buffer when colors aren't written.
float4 main() : SV_Target0 {
    return float4(0.0, 0.0, 0.0, 0.0);
}
