// =====================================================================================
//  smav2 ddraw.dll - Direct3D 11 presenter shaders
// =====================================================================================
// One full-screen triangle (VS_Fullscreen, no vertex buffer) covering the viewport, which the
// presenter sets to the picture rectangle (letterbox bars are the cleared black around it).
// Pixel shaders, one per filter=:
//   PS_Nearest   point sampling: every game pixel is a solid block (perfect at 2x, 3x)
//   PS_Linear    bilinear: smooth/soft at any size
//   PS_Sharp     exact "sharp bilinear": blocks with only a 1-screen-pixel wide blend at their
//                edges, so non-whole factors (1.28x, 1.44x) stay crisp but even
//   PS_Scale2x   Scale2x/EPX edge smoothing (a classic pixel-art upscaler): each game pixel is
//                split in 2x2 and corners take a neighbour's colour where an edge runs diagonally
// Compiled at build time with fxc (build.bat) into build\shader_*.h, embedded in the DLL.
// =====================================================================================

Texture2D    Frame  : register(t0);   // the game's 800x600 picture (BGRA)
SamplerState Point  : register(s0);
SamplerState Bilin  : register(s1);

cbuffer Params : register(b0)
{
    float2 TexSize;   // 800, 600
    float2 OutSize;   // picture size on screen, e.g. 1600, 1200
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

// Full-screen triangle from the vertex index: (0,0) (2,0) (0,2) in uv.
VSOut VS_Fullscreen(uint id : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PS_Nearest(VSOut i) : SV_Target { return Frame.Sample(Point, i.uv); }

float4 PS_Linear(VSOut i) : SV_Target { return Frame.Sample(Bilin, i.uv); }

// Sharp bilinear: inside each source texel, keep the texel's own colour except within half an
// output pixel of its border, where it blends linearly with the neighbour.
float4 PS_Sharp(VSOut i) : SV_Target
{
    float2 scale  = OutSize / TexSize;              // >= 1
    float2 texel  = i.uv * TexSize;
    float2 base   = floor(texel);
    float2 f      = texel - base;                   // position inside the texel, 0..1
    float2 region = 0.5 - 0.5 / scale;              // half-width of the solid area
    float2 d      = f - 0.5;
    float2 g      = (d - clamp(d, -region, region)) * scale + 0.5;
    return Frame.Sample(Bilin, (base + g) / TexSize);
}

// Scale2x / EPX. E = this texel, A/B/C/D = up/right/left/down neighbours.
float4 PS_Scale2x(VSOut i) : SV_Target
{
    float2 texel = i.uv * TexSize;
    float2 c     = floor(texel) + 0.5;
    float2 q     = frac(texel);                     // which quarter of the texel we are in
    float2 px    = 1.0 / TexSize;
    float3 E = Frame.Sample(Point, c * px).rgb;
    float3 A = Frame.Sample(Point, (c + float2( 0, -1)) * px).rgb;
    float3 B = Frame.Sample(Point, (c + float2( 1,  0)) * px).rgb;
    float3 C = Frame.Sample(Point, (c + float2(-1,  0)) * px).rgb;
    float3 D = Frame.Sample(Point, (c + float2( 0,  1)) * px).rgb;
    const float eps = 0.004;
    #define EQ(x, y) (all(abs((x) - (y)) < eps))
    float3 r = E;
    if (q.x < 0.5 && q.y < 0.5)       { if (EQ(C, A) && !EQ(C, D) && !EQ(A, B)) r = A; }   // top-left
    else if (q.x >= 0.5 && q.y < 0.5) { if (EQ(A, B) && !EQ(A, C) && !EQ(B, D)) r = B; }   // top-right
    else if (q.x < 0.5)               { if (EQ(D, C) && !EQ(D, B) && !EQ(C, A)) r = C; }   // bottom-left
    else                              { if (EQ(B, D) && !EQ(B, A) && !EQ(D, C)) r = D; }   // bottom-right
    return float4(r, 1);
}
