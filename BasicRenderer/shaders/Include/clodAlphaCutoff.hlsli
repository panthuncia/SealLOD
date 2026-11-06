#ifndef CLOD_ALPHA_CUTOFF_HLSLI
#define CLOD_ALPHA_CUTOFF_HLSLI

// The alpha cutoff test any-hit and the opacity micromap bakes all use, so they can't drift apart.
bool CLodRtAlphaIsOpaque(float alpha, float cutoff)
{
    return alpha >= cutoff;
}

#endif // CLOD_ALPHA_CUTOFF_HLSLI
