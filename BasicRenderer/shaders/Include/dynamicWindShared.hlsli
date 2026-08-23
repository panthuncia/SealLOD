#ifndef DYNAMIC_WIND_SHARED_HLSLI
#define DYNAMIC_WIND_SHARED_HLSLI

struct DynamicWindFrameGPU
{
	uint fieldSlice0;
	uint fieldSlice1;
	uint fieldDimensions;
	uint fieldValid;
	float fieldCellSize;
	float fieldOriginX;
	float fieldOriginY;
	float fieldInterpolation;
	float currentTime;
	float previousTime;
	float deltaTime;
	float strength;
	float windX;
	float windY;
	float gustStrength;
	float treeDisplacementScale;
};

float DynamicWindPhase(uint seed)
{
	seed ^= seed >> 16u;
	seed *= 0x7feb352du;
	seed ^= seed >> 15u;
	seed *= 0x846ca68bu;
	seed ^= seed >> 16u;
	return (seed & 0x00FFFFFFu) * (6.28318530718f / 16777216.0f);
}

float4 DynamicWindLoadCell(StructuredBuffer<uint> field, uint cellIndex)
{
	const uint xy = field[cellIndex * 2u];
	const uint za = field[cellIndex * 2u + 1u];
	return float4(
		f16tof32(xy & 0xFFFFu), f16tof32(xy >> 16u),
		f16tof32(za & 0xFFFFu), f16tof32(za >> 16u));
}

float4 DynamicWindSampleSlice(
	StructuredBuffer<uint> field,
	float2 position,
	uint width,
	uint height,
	DynamicWindFrameGPU frame)
{
	const float2 grid =
		(position - float2(frame.fieldOriginX, frame.fieldOriginY)) /
		frame.fieldCellSize - 0.5f;
	if (any(grid < 0.0f) || grid.x >= width - 1u || grid.y >= height - 1u)
		return float4(0.0f, 0.0f, 0.0f, -1.0f);
	const uint2 base = (uint2)floor(grid);
	const float2 fraction = frac(grid);
	const float4 a = lerp(
		DynamicWindLoadCell(field, base.y * width + base.x),
		DynamicWindLoadCell(field, base.y * width + base.x + 1u), fraction.x);
	const float4 b = lerp(
		DynamicWindLoadCell(field, (base.y + 1u) * width + base.x),
		DynamicWindLoadCell(field, (base.y + 1u) * width + base.x + 1u), fraction.x);
	return lerp(a, b, fraction.y);
}

// positionSkyrimXY is in the field's native Skyrim horizontal coordinate
// system.  The returned vector is an exposure/direction vector; consumer
// response code applies frame.strength and its own compliance scale.
float3 DynamicWindSampleLevel0(float2 positionSkyrimXY, DynamicWindFrameGPU frame)
{
	const float3 fallback = float3(frame.windX, frame.windY, 0.0f);
	if (frame.fieldValid == 0u || frame.fieldCellSize <= 0.0f)
		return fallback;
	const uint width = frame.fieldDimensions & 0xFFFFu;
	const uint height = frame.fieldDimensions >> 16u;
	if (width < 2u || height < 2u)
		return fallback;
	StructuredBuffer<uint> field0 = ResourceDescriptorHeap[frame.fieldSlice0];
	StructuredBuffer<uint> field1 = ResourceDescriptorHeap[frame.fieldSlice1];
	const float4 a = DynamicWindSampleSlice(field0, positionSkyrimXY, width, height, frame);
	const float4 b = DynamicWindSampleSlice(field1, positionSkyrimXY, width, height, frame);
	if (min(a.w, b.w) < 0.5f)
		return fallback;
	return lerp(a.xyz, b.xyz, frame.fieldInterpolation);
}

#endif
