Texture2D<float> YPlane : register(t0);       // Input Y plane (luminance)
Texture2D<float2> UVPlane : register(t1);     // Input UV plane (chroma, interleaved)

RWTexture2D<float4> OutputTexture : register(u0); // Output BGRA32 texture

[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 coord = DTid.xy;
    
    // Get texture dimensions from the Y plane
    uint width, height;
    YPlane.GetDimensions(width, height);
    
    if (coord.x >= width || coord.y >= height) return;

    // Read Y plane directly
    float y = YPlane.Load(int3(coord, 0));

    // Read UV plane (4:4:0 subsampling: map Y coordinate to UV coordinate)
    // UV plane is half-height, so map coord.y to UV texture space
    uint uvY = coord.y / 2;  // Map to UV plane coordinates
    float2 uv_01 = UVPlane.Load(int3(coord.x, uvY, 0)); // Hardware converts R8G8_UNORM to [0,1] float2
    float2 uv = uv_01 - 0.5;

    // Convert YUV to RGB
    float r = y + 1.402 * uv.y;
    float g = y - 0.344136 * uv.x - 0.714136 * uv.y;
    float b = y + 1.772 * uv.x;

    r = saturate(r);
    g = saturate(g);
    b = saturate(b);

    // Write output texture (BGRA format, alpha = 1.0)
    OutputTexture[coord] = float4(b, g, r, 1.0);
}