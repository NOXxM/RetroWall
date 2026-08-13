// ---------------------------------------------------------------------------
// NV12ToRGB.hlsl  (reference copy; the same source is embedded in
// VideoRenderer.cpp and compiled at runtime with D3DCompile)
//
// Samples an NV12 surface that lives in VRAM and converts YUV -> RGB entirely on
// the GPU. The luma (Y) plane is exposed as an R8_UNORM SRV; the chroma (CbCr)
// plane as an R8G8_UNORM SRV of the same texture — no CPU readback ever occurs.
// Conversion assumes BT.709 limited ("studio") range, the common case for HD
// H.264/HEVC. See the note in the .cpp about reading the real range/matrix from
// the media type if you need to support full-range or BT.601 content.
// ---------------------------------------------------------------------------

Texture2D<float>  LumaPlane   : register(t0);  // R8   -> Y
Texture2D<float2> ChromaPlane : register(t1);  // R8G8 -> Cb,Cr
SamplerState      LinearClamp : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Fullscreen triangle from the vertex id — no vertex/index buffers bound.
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    float  y  = LumaPlane.Sample(LinearClamp, i.uv).r;
    float2 cbcr = ChromaPlane.Sample(LinearClamp, i.uv).rg;

    // Limited-range normalization: Y in [16,235], CbCr in [16,240] (of 255).
    y = (y - 16.0 / 255.0) * (255.0 / 219.0);
    float cb = (cbcr.x - 128.0 / 255.0) * (255.0 / 224.0);
    float cr = (cbcr.y - 128.0 / 255.0) * (255.0 / 224.0);

    // BT.709 YCbCr -> RGB.
    float3 rgb;
    rgb.r = y + 1.5748 * cr;
    rgb.g = y - 0.1873 * cb - 0.4681 * cr;
    rgb.b = y + 1.8556 * cb;

    return float4(saturate(rgb), 1.0);
}
