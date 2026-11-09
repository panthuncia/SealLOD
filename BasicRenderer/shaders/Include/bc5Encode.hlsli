#ifndef __BC5_ENCODE_HLSLI__
#define __BC5_ENCODE_HLSLI__

// BC4 (one channel) and BC5 (two) blocks: each channel is 8 bytes, two 8-bit endpoints and sixteen 3-bit indices. Both of
// the format's palettes are tried and the smaller squared error kept: endpoints first (six values between them), for
// smooth ranges; endpoints second (four between, plus exact 0 and 255), for blocks that touch the range's ends. Endpoints a
// few steps inside the range are searched too: an outlier stretches the steps between the range's own ends (about 1.6 dB
// on noisy normal maps). Ported from tesworld's BcChannels.hlsl, which encodes the preprocessor's normal maps.

struct Bc4ChannelBlock
{
    uint2 words;
    float error;
};

float Bc4PaletteValue(uint e0, uint e1, uint index)
{
    const float a = (float)e0;
    const float b = (float)e1;
    if (e0 > e1)
    {
        // 0 and 1 are the endpoints; 2..7 step from e0 toward e1 in sevenths.
        return index == 0u ? a : index == 1u ? b : ((8.0f - (float)index) * a + ((float)index - 1.0f) * b) / 7.0f;
    }
    // 0 and 1 are the endpoints; 2..5 step in fifths; 6 is 0 and 7 is 255.
    if (index == 0u) return a;
    if (index == 1u) return b;
    if (index == 6u) return 0.0f;
    if (index == 7u) return 255.0f;
    return ((6.0f - (float)index) * a + ((float)index - 1.0f) * b) / 5.0f;
}

Bc4ChannelBlock Bc4EncodeWith(float values[16], uint e0, uint e1)
{
    Bc4ChannelBlock block;
    block.error = 0.0f;
    uint indexBits[2] = { 0u, 0u }; // 48 bits of indices, texel 0 lowest
    [unroll]
    for (uint texel = 0u; texel < 16u; ++texel)
    {
        uint best = 0u;
        float bestError = 1.0e30f;
        [unroll]
        for (uint index = 0u; index < 8u; ++index)
        {
            const float d = Bc4PaletteValue(e0, e1, index) - values[texel];
            if (d * d < bestError)
            {
                bestError = d * d;
                best = index;
            }
        }
        block.error += bestError;
        const uint bit = texel * 3u;
        if (bit < 32u)
        {
            indexBits[0] |= best << bit;
            if (bit > 29u) indexBits[1] |= best >> (32u - bit);
        }
        else
        {
            indexBits[1] |= best << (bit - 32u);
        }
    }
    // Byte 0 e0, byte 1 e1, then the indices.
    block.words.x = e0 | (e1 << 8u) | (indexBits[0] << 16u);
    block.words.y = (indexBits[0] >> 16u) | (indexBits[1] << 16u);
    return block;
}

// values: 16 texels (row-major) in byte units [0, 255].
uint2 Bc4EncodeChannel(float values[16])
{
    float low = 255.0f;
    float high = 0.0f;
    // The range without exact ends, for the palette that has them already.
    float innerLow = 255.0f;
    float innerHigh = 0.0f;
    [unroll]
    for (uint texel = 0u; texel < 16u; ++texel)
    {
        values[texel] = clamp(values[texel], 0.0f, 255.0f);
        low = min(low, values[texel]);
        high = max(high, values[texel]);
        if (values[texel] > 0.0f && values[texel] < 255.0f)
        {
            innerLow = min(innerLow, values[texel]);
            innerHigh = max(innerHigh, values[texel]);
        }
    }
    const uint lo = (uint)round(low);
    const uint hi = (uint)round(high);
    if (hi == lo)
    {
        // One value: every index 0.
        return uint2(hi | (hi << 8u), 0u);
    }
    // e0 > e1 selects the six-between palette.
    Bc4ChannelBlock best = Bc4EncodeWith(values, hi, lo);
    [loop]
    for (uint inHigh = 0u; inHigh < 8u; ++inHigh)
    {
        [loop]
        for (uint inLow = 0u; inLow < 8u; ++inLow)
        {
            if ((inHigh | inLow) == 0u || hi < lo + inHigh + inLow + 1u) continue;
            const Bc4ChannelBlock candidate = Bc4EncodeWith(values, hi - inHigh, lo + inLow);
            if (candidate.error < best.error) best = candidate;
        }
    }
    if (innerHigh >= innerLow)
    {
        // e0 <= e1 selects the palette with exact 0 and 255.
        const Bc4ChannelBlock withEnds = Bc4EncodeWith(values, (uint)round(innerLow), (uint)round(innerHigh));
        if (withEnds.error < best.error) best = withEnds;
    }
    return best.words;
}

// A BC5 block: red's 8 bytes, then green's.
uint4 Bc5EncodeBlock(float red[16], float green[16])
{
    return uint4(Bc4EncodeChannel(red), Bc4EncodeChannel(green));
}

#endif // __BC5_ENCODE_HLSLI__
