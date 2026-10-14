/* blit_ps.hlsl: sample the software framebuffer texture.
 * No discard (360 fxc rejects it). Compile on the PC:
 *   fxc /nologo /T ps_3_0 /E main /Fh blit_ps.h /Vn g_blit_ps blit_ps.hlsl
 */
sampler2D fb : register(s0);

float4 main( float2 Uv : TEXCOORD0 ) : COLOR {
    return tex2D( fb, Uv );
}
