Texture2D<float4> InputTexture : register(t0); // Input BGRA32 texture
RWTexture2D<float> YPlane : register(u0);      // Output Y plane (luminance)
RWTexture2D<float2> UVPlane : register(u1);    // Output UV plane (chroma, interleaved)

[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 coord = DTid.xy;
    
    // Get texture dimensions
    uint width, height;
    InputTexture.GetDimensions(width, height);
    
    if (coord.x >= width || coord.y >= height) return;

    // Load the input texture directly (no sampling needed in compute shader)
    float4 bgra = InputTexture.Load(int3(coord, 0));

    // Convert BGRA to YUV (note: bgra.rgb is actually BGR, so we need to adjust)
    float r = bgra.z; // Red is in the Z component for BGRA
    float g = bgra.y; // Green is in the Y component
    float b = bgra.x; // Blue is in the X component
    
    float y = 0.299 * r + 0.587 * g + 0.114 * b;
    float u = -0.168736 * r - 0.331264 * g + 0.5 * b + 0.5; // Range [0,1] for UNORM
    float v = 0.5 * r - 0.418688 * g - 0.081312 * b + 0.5;  // Range [0,1] for UNORM

    // Write Y plane
    YPlane[coord] = y;

    // Write UV plane (4:4:0 subsampling: only write UV for even rows)
    // Use bitwise AND instead of modulo for SM 5.0 compatibility
    if ((coord.y & 1) == 0) {
        // Map to half-height UV texture coordinates
        UVPlane[uint2(coord.x, coord.y / 2)] = float2(u, v);
    }
}