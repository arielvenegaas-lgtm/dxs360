/* blit_vs.hlsl: fullscreen-quad passthrough vertex shader.
 * 360-legal semantics only (POSITION + TEXCOORD). Compile on the PC:
 *   fxc /nologo /T vs_3_0 /E main /Fh blit_vs.h /Vn g_blit_vs blit_vs.hlsl
 */
struct VS_IN  { float4 Pos : POSITION; float2 Uv : TEXCOORD0; };
struct VS_OUT { float4 Pos : POSITION; float2 Uv : TEXCOORD0; };

VS_OUT main( VS_IN In ) {
    VS_OUT Out;
    Out.Pos = In.Pos;
    Out.Uv  = In.Uv;
    return Out;
}
