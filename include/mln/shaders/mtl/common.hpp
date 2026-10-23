#pragma once

namespace mln {
namespace shaders {

constexpr auto prelude = R"(
#include <metal_stdlib>
using namespace metal;

// The maximum allowed miter limit is 2.0 at the moment. the extrude normal is stored
// in a byte (-128..127). We scale regular normals up to length 63, but there are also
// "special" normals that have a bigger length (of up to 126 in this case).
#define LINE_NORMAL_SCALE (1.0 / (127 / 2))

// The attribute conveying progress along a line is scaled to [0, 2^15).
#define MAX_LINE_DISTANCE 32767.0

#define SDF_PX 8.0

// OpenGL `mod` is `x-y*floor(x/y)` where `floor` rounds down.
// Metal `fmod` is `x-y*trunc(x/y)` where `trunc` rounds toward zero.
// This function provides GL-compatible modulus for porting GLSL shaders.
template <typename T1, typename T2>
inline auto glMod(T1 x, T2 y) { return x - y * metal::floor(x/y); }

inline float radians(float degrees) {
    return M_PI_F * degrees / 180.0;
}

// Unpack a pair of values that have been packed into a single float.
// The packed values are assumed to be 8-bit unsigned integers, and are
// packed like so: packedValue = floor(input[0]) * 256 + input[1],
float2 unpack_float(const float packedValue) {
    const int packedIntValue = int(packedValue);
    const int v0 = packedIntValue / 256;
    return float2(v0, packedIntValue - v0 * 256);
}
float2 unpack_opacity(const float packedOpacity) {
    return float2(float(int(packedOpacity) / 2) / 127.0, glMod(packedOpacity, 2.0));
}
// To minimize the number of attributes needed, we encode a 4-component
// color into a pair of floats (i.e. a vec2) as follows:
// [ floor(color.r * 255) * 256 + color.g * 255, floor(color.b * 255) * 256 + color.a * 255 ]
float4 decode_color(const float2 encoded) {
    return float4(unpack_float(encoded[0]) / 255, unpack_float(encoded[1]) / 255);
}
// Unpack a pair of paint values and interpolate between them.
float unpack_mix_float(device const float packedValue[2], const float t) {
    return mix(packedValue[0], packedValue[1], t);
}
float unpack_mix_float(const float2 packedValue, const float t) {
    return mix(packedValue[0], packedValue[1], t);
}
// Unpack a pair of paint values and interpolate between them.
float4 unpack_mix_color(device const float packedColors[4], const float t) {
    return mix(decode_color(float2(packedColors[0], packedColors[1])),
               decode_color(float2(packedColors[2], packedColors[3])), t);
}
float4 unpack_mix_color(const float4 packedColors, const float t) {
    return mix(decode_color(float2(packedColors[0], packedColors[1])),
               decode_color(float2(packedColors[2], packedColors[3])), t);
}

// Unpack pattern position
inline float2 get_pattern_pos(const float2 pixel_coord_upper, const float2 pixel_coord_lower,
                     const float2 pattern_size, const float tile_units_to_pixels, const float2 pos) {
    const float2 offset = glMod(glMod(glMod(pixel_coord_upper, pattern_size) * 256.0, pattern_size) * 256.0 + pixel_coord_lower, pattern_size);
    return (tile_units_to_pixels * pos + offset) / pattern_size;
}

// Sample the terrain elevation in meters at a tile-local coordinate, with manual
// bilinear interpolation on DEM pixel centers (the DEM has a 2px backfilled border),
// matching the maplibre-gl-js get_elevation() prelude function. Unlike gl-js (global
// terrain uniforms), MapLibre Native carries the DEM data per-drawable, so the DEM
// texture and dem_* values are passed in as arguments rather than read from globals.
// Decode one DEM texel to meters. The clamp at the call sites matters: read() out of range
// is undefined where the sampler's clamp-to-edge used to cover it, and a tile with no DEM of
// its own is bound the 1x1 placeholder, whose only valid texel is (0, 0).
inline float dem_texel(texture2d<float, access::sample> dem, int2 texel, float4 dem_unpack) {
    float4 data = dem.read(uint2(texel)) * 255.0;
    data.a = -1.0;
    return dot(data, dem_unpack);
}

inline float get_elevation(float2 pos,
                           texture2d<float, access::sample> dem,
                           sampler dem_sampler,
                           float4 dem_coords,
                           float4 dem_unpack,
                           float dem_dim,
                           float dem_exaggeration,
                           float dem_enabled) {
    if (dem_enabled == 0.0) {
        return 0.0;
    }
    const float2 coord = (pos * dem_coords.x + dem_coords.yz) * dem_dim + 2.0;
    const float2 f = fract(coord);
    const int2 c = int2(floor(coord));
    const int2 hi = int2(dem.get_width(), dem.get_height()) - 1;
    const float tl = dem_texel(dem, clamp(c, int2(0), hi), dem_unpack);
    const float tr = dem_texel(dem, clamp(c + int2(1, 0), int2(0), hi), dem_unpack);
    const float bl = dem_texel(dem, clamp(c + int2(0, 1), int2(0), hi), dem_unpack);
    const float br = dem_texel(dem, clamp(c + int2(1, 1), int2(0), hi), dem_unpack);
    const float elevation = mix(mix(tl, tr, f.x), mix(bl, br, f.x), f.y);
    return elevation * dem_exaggeration;
}

// Unpack a depth value packed by the terrain depth pass (mtl/terrain_depth.hpp),
// converted to NDC z, as in the maplibre-gl-js prelude
inline float unpack_depth(float4 rgba_depth) {
    const float4 bit_shift = float4(1.0 / (256.0 * 256.0 * 256.0), 1.0 / (256.0 * 256.0), 1.0 / 256.0, 1.0);
    return dot(rgba_depth, bit_shift) * 2.0 - 1.0;
}

// Opacity of a fragment behind the terrain, in [0, 1]: 1 fully visible, 0 fully
// hidden, with a soft ramp over ~0.002 NDC depth and a small bias so geometry
// sitting exactly on the terrain surface (e.g. a label anchored to it) does not
// occlude itself. Matches the maplibre-gl-js depthOpacity() prelude function.
inline float depth_opacity(float3 frag,
                           texture2d<float, access::sample> depth_texture,
                           sampler depth_sampler) {
    const float2 uv = frag.xy * 0.5 + 0.5;
    const float d = unpack_depth(depth_texture.sample(depth_sampler, float2(uv.x, 1.0 - uv.y))) + 0.0001 -
                    frag.z;
    return 1.0 - max(0.0, min(1.0, -d * 500.0));
}

// Whether a clip-space position is visible in front of the terrain, from the
// packed terrain depth texture, matching maplibre-gl-js calculate_visibility().
// Unlike gl-js (global terrain uniforms), the depth texture and enable flag are
// passed as arguments.
inline float calculate_visibility(float4 pos,
                                  texture2d<float, access::sample> depth_texture,
                                  sampler depth_sampler,
                                  float depth_enabled) {
    if (depth_enabled == 0.0) {
        return 1.0;
    }
    const float3 frag = pos.xyz / pos.w;
    // check if coordinate is fully visible
    const float d = depth_opacity(frag, depth_texture, depth_sampler);
    if (d > 0.95) {
        return 1.0;
    }
    // if not, sample some pixels above: a label whose anchor is just behind a
    // ridge still shows if its glyphs poke above it (maplibre-gl-js behaviour)
    return (d + depth_opacity(frag + float3(0.0, 0.01, 0.0), depth_texture, depth_sampler)) / 2.0;
}

template<class ForwardIt, class T>
ForwardIt upper_bound(ForwardIt first, ForwardIt last, thread const T& value)
{
    size_t count = last - first;
    while (count > 0)
    {
        ForwardIt it = first;
        const size_t step = count / 2;
        it += step;
        if (!(value < *it))
        {
            first = ++it;
            count -= step + 1;
        }
        else {
            count = step;
        }
    }
    return first;
}

float interpolationFactor(float base, float rangeMin, float rangeMax, float z) {
    const float zoomDiff = rangeMax - rangeMin;
    const float zoomProgress = z - rangeMin;
    if (zoomDiff == 0) {
        return 0;
    } else if (base == 1.0f) {
        return zoomProgress / zoomDiff;
    } else {
        return (pow(base, zoomProgress) - 1) / (pow(base, zoomDiff) - 1);
    }
}


enum class GPUInterpType : uint16_t {
    Step,
    Linear,
    Exponential,
    Bezier
};
enum class GPUOutputType : uint16_t {
    Float,
    Color,
};
enum class GPUOptions : uint16_t {
    None = 0,
    IntegerZoom = 1 << 0,
    Transitioning = 1 << 1,
};
bool operator&(GPUOptions a, GPUOptions b) { return (uint16_t)a & (uint16_t)b; }

constant const int maxExprStops = 16;
struct alignas(16) GPUExpression {
    GPUOutputType outputType;
    uint16_t stopCount;
    GPUOptions options;
    GPUInterpType interpolation;

    union InterpOptions {
        struct Exponential {
            float base;
        } exponential;

        struct Bezier {
            float x1;
            float y1;
            float x2;
            float y2;
        } bezier;
    } interpOptions;

    float inputs[maxExprStops];

    union Stops {
        float floats[maxExprStops];
        float2 colors[maxExprStops];
    } stops;

    float eval(float zoom) device const {
        const auto effectiveZoom = (options & GPUOptions::IntegerZoom) ? floor(zoom) : zoom;
        const auto index = find(effectiveZoom);
        if (index == 0) {
            return stops.floats[0];
        } else if (index == stopCount) {
            return stops.floats[stopCount - 1];
        }
        switch (interpolation) {
            case GPUInterpType::Step: return stops.floats[index - 1];
            default: assert(false);
                [[fallthrough]];
            case GPUInterpType::Linear: assert(interpOptions.exponential.base == 1.0f);
                [[fallthrough]];
            case GPUInterpType::Exponential: {
                const float rangeBeg = inputs[index - 1];
                const float rangeEnd = inputs[index];
                const auto t = interpolationFactor(interpOptions.exponential.base, rangeBeg, rangeEnd, effectiveZoom);
                return mix(stops.floats[index - 1], stops.floats[index], t);
            }
            case GPUInterpType::Bezier:
                assert(false);
                return stops.floats[0];
        }
    }

    float4 evalColor(float zoom) device const {
        const auto effectiveZoom = (options & GPUOptions::IntegerZoom) ? floor(zoom) : zoom;
        const auto index = find(effectiveZoom);
        if (index == 0) {
            return getColor(0);
        } else if (index == stopCount) {
            return getColor(stopCount - 1);
        }
        switch (interpolation) {
            case GPUInterpType::Step:
                return getColor(index - 1);
            default:
                assert(false);
                [[fallthrough]];
            case GPUInterpType::Linear:
                assert(interpOptions.exponential.base == 1.0f);
                [[fallthrough]];
            case GPUInterpType::Exponential: {
                const float rangeBeg = inputs[index - 1];
                const float rangeEnd = inputs[index];
                const auto t = interpolationFactor(interpOptions.exponential.base, rangeBeg, rangeEnd, effectiveZoom);
                return mix(getColor(index - 1), getColor(index), clamp(t, 0.0, 1.0));
            }
            case GPUInterpType::Bezier:
                assert(false);
                return getColor(0);
        }
    }

    /// Get the index of the entry to use from the zoom level
    size_t find(float zoom) device const {
        return upper_bound(&inputs[0], &inputs[stopCount], zoom) - &inputs[0];
    }

    float4 getColor(size_t index) device const { return decode_color(stops.colors[index]); }
};
static_assert(sizeof(GPUExpression) == 32 + (4 + 8) * maxExprStops, "wrong size");
static_assert(sizeof(GPUExpression) % 16 == 0, "wrong alignment");

struct alignas(16) GlobalPaintParamsUBO {
    /*  0 */ float2 pattern_atlas_texsize;
    /*  8 */ float2 units_to_pixels;
    /* 16 */ float2 world_size;
    /* 24 */ float camera_to_center_distance;
    /* 28 */ float symbol_fade_change;
    /* 32 */ float aspect_ratio;
    /* 36 */ float pixel_ratio;
    /* 40 */ float map_zoom;
    /* 44 */ float pad1;
    /* 48 */ float4 drape_tile;
    /* 64 */
};
static_assert(sizeof(GlobalPaintParamsUBO) == 4 * 16, "wrong size");

// Place a clip-space position computed with a tile-local drape matrix into the
// current terrain drape render target (see the GL prelude for the derivation).
// `matrix` carries the drawable's tile (z, x, y) in its unused third column and
// target_tile is GlobalPaintParamsUBO::drape_tile (w != 0 while drawing into a
// drape target).
inline float4 apply_drape_transform(float4 clip, float4x4 matrix, float4 target_tile) {
    if (target_tile.w == 0.0) {
        return clip;
    }
    const float3 tile = float3(matrix[2][0], matrix[2][1], matrix[2][2]);
    const float k = target_tile.x - tile.x; // target zoom - drawable zoom
    const float scale = exp2(k);
    float2 offset;
    if (k >= 0.0) {
        offset = tile.yz * scale - target_tile.yz;
    } else {
        offset = (tile.yz - target_tile.yz * exp2(-k)) * scale;
    }
    clip.x = clip.x * scale + (scale - 1.0 + 2.0 * offset.x);
    clip.y = clip.y * scale + (1.0 - scale - 2.0 * offset.y);
    return clip;
}

struct alignas(16) ProjectionUBO {
    /*   0 */ float4x4 matrix;
    /*  64 */ float4x4 fallback_matrix;
    /* 128 */ float4 tile_mercator_coords;
    /* 144 */ float4 clipping_plane;
    /* 160 */ float projection_transition;
    /* 164 */ float depth_offset;
    /* 168 */ float2 translate;
    /* 176 */
};
static_assert(sizeof(ProjectionUBO) == 11 * 16, "wrong size");

// Pole vertices carry these sentinel Y values in their raw position.
#define GLOBE_POLE_NORTH_Y -32767.5
#define GLOBE_POLE_SOUTH_Y 32766.5

#if defined(PROJECTION_GLOBE)

#define GLOBE_RADIUS 6371008.8

// Tile position (0..EXTENT) to a point on the unit sphere; the pole sentinels in rawPos map to the poles.
inline float3 projectToSphere(float2 translatedPos, float2 rawPos, device const ProjectionUBO& projection) {
    const float2 mercator_pos = projection.tile_mercator_coords.xy + projection.tile_mercator_coords.zw * translatedPos;
    const float spherical_x = mercator_pos.x * M_PI_F * 2.0 + M_PI_F;
    // sin/cos of the latitude from the Mercator Y via the tangent half-angle identities: no atan, and float32 precision survives near the equator.
    const float t = exp(M_PI_F - (mercator_pos.y * M_PI_F * 2.0));
    const float t2 = t * t;
    const float denom = t2 + 1.0;
    const float sin_sy = (t2 - 1.0) / denom;
    const float cos_sy = (2.0 * t) / denom;
    float3 pos = float3(sin(spherical_x) * cos_sy, sin_sy, cos(spherical_x) * cos_sy);
    if (rawPos.y < GLOBE_POLE_NORTH_Y) {
        pos = float3(0.0, 1.0, 0.0);
    }
    if (rawPos.y > GLOBE_POLE_SOUTH_Y) {
        pos = float3(0.0, -1.0, 0.0);
    }
    return pos;
}

inline float3 globeRotateVector(float3 vec, float2 angles) {
    float3 axisRight = float3(vec.z, 0.0, -vec.x);
    float3 axisUp = cross(axisRight, vec);
    axisRight = normalize(axisRight);
    axisUp = normalize(axisUp);
    const float2 t = tan(angles);
    return normalize(vec + axisRight * t.x + axisUp * t.y);
}

// cos(latitude) at a tile Y, from the same exp() form as projectToSphere.
inline float circumferenceRatioAtTileY(float tileY, device const ProjectionUBO& projection) {
    const float mercator_pos_y = projection.tile_mercator_coords.y + projection.tile_mercator_coords.w * tileY;
    const float t = exp(M_PI_F - (mercator_pos_y * M_PI_F * 2.0));
    return (2.0 * t) / (t * t + 1.0);
}

inline float projectLineThickness(float tileY, device const ProjectionUBO& projection) {
    const float thickness = 1.0 / circumferenceRatioAtTileY(tileY, projection);
    if (projection.projection_transition < 0.999) {
        return mix(1.0, thickness, projection.projection_transition);
    }
    return thickness;
}

inline float globeComputeClippingZ(float3 spherePos, device const ProjectionUBO& projection) {
    return 1.0 - (dot(spherePos, projection.clipping_plane.xyz) + projection.clipping_plane.w);
}

inline float4 interpolateProjection(float2 posInTile, float3 spherePos, float elevation, device const ProjectionUBO& projection) {
    const float3 elevatedPos = spherePos * (1.0 + elevation / GLOBE_RADIUS);
    float4 globePosition = projection.matrix * float4(elevatedPos, 1.0);
    // Clip the far side of the globe through Z; the layer's depth shift keeps layer order.
    // The layer offset is subtracted in clip space on purpose: the separation it buys grows as the camera nears the
    // sphere, where the hemisphere's Z gradient is steepest; a constant NDC offset z-fights fill against outline.
    globePosition.z = globeComputeClippingZ(elevatedPos, projection) * globePosition.w - projection.depth_offset;

    if (projection.projection_transition > 0.999) {
        return globePosition;
    }

    const float4 flatPosition = projection.fallback_matrix * float4(posInTile, elevation, 1.0);
    const float z_globeness_threshold = 0.2;
    float4 result = globePosition;
    result.z = mix(0.0, globePosition.z, clamp((projection.projection_transition - z_globeness_threshold) / (1.0 - z_globeness_threshold), 0.0, 1.0));
    result.xyw = mix(flatPosition.xyw, globePosition.xyw, projection.projection_transition);
    if ((posInTile.y < GLOBE_POLE_NORTH_Y) || (posInTile.y > GLOBE_POLE_SOUTH_Y)) {
        result = globePosition;
        const float poles_hidden_anim_percentage = 0.02;
        result.z = mix(globePosition.z, 100.0, pow(max((1.0 - projection.projection_transition) / poles_hidden_anim_percentage, 0.0), 8.0));
    }
    return result;
}

// Keeps the matrix Z, for geometry that needs the depth buffer.
inline float4 interpolateProjectionFor3D(float2 posInTile, float3 spherePos, float elevation, device const ProjectionUBO& projection) {
    const float3 elevatedPos = spherePos * (1.0 + elevation / GLOBE_RADIUS);
    const float4 globePosition = projection.matrix * float4(elevatedPos, 1.0);
    if (projection.projection_transition > 0.999) {
        return globePosition;
    }
    const float4 fallbackPosition = projection.fallback_matrix * float4(posInTile, elevation, 1.0);
    return mix(fallbackPosition, globePosition, projection.projection_transition);
}

inline float4 projectTile(float2 pos, device const ProjectionUBO& projection) {
    return interpolateProjection(pos, projectToSphere(pos + projection.translate, float2(0.0, 0.0), projection), 0.0, projection);
}

// The zoom 0 tile's buffer wraps around the planet onto the tile itself, so the line shaders discard the fragments
// beyond its X extent; every other tile (a mercator width below the whole world's) hands on a value inside it.
inline float antimeridianClipX(float2 posInTile, device const ProjectionUBO& projection) {
    return projection.tile_mercator_coords.z * 8192.0 >= 1.0 ? posInTile.x : 0.0;
}

inline bool clippedAtAntimeridian(float tileX) {
    return tileX < 0.0 || tileX >= 8192.0;
}

// The variant for geometry that can carry pole vertices; rawPos is the untranslated position.
inline float4 projectTile(float2 pos, float2 rawPos, device const ProjectionUBO& projection) {
    return interpolateProjection(pos, projectToSphere(pos + projection.translate, rawPos, projection), 0.0, projection);
}

inline float4 projectTileWithElevation(float2 pos, float elevation, device const ProjectionUBO& projection) {
    return interpolateProjection(pos, projectToSphere(pos + projection.translate, float2(0.0, 0.0), projection), elevation, projection);
}

inline float4 projectTileFor3D(float2 pos, float elevation, device const ProjectionUBO& projection) {
    return interpolateProjectionFor3D(pos, projectToSphere(pos + projection.translate, pos, projection), elevation, projection);
}

#else

inline float4 projectTile(float2 pos, device const ProjectionUBO& projection) {
    return projection.matrix * float4(pos, 0.0, 1.0);
}

// Pole vertices only exist on the globe; put them behind the near plane so their triangles are clipped.
inline float4 projectTile(float2 pos, float2 rawPos, device const ProjectionUBO& projection) {
    float4 result = projection.matrix * float4(pos, 0.0, 1.0);
    if (rawPos.y < GLOBE_POLE_NORTH_Y || rawPos.y > GLOBE_POLE_SOUTH_Y) {
        result.z = -10000000.0;
    }
    return result;
}

inline float4 projectTileWithElevation(float2 pos, float elevation, device const ProjectionUBO& projection) {
    return projection.matrix * float4(pos, elevation, 1.0);
}

inline float4 projectTileFor3D(float2 pos, float elevation, device const ProjectionUBO& projection) {
    return projection.matrix * float4(pos, elevation, 1.0);
}

inline float projectLineThickness(float, device const ProjectionUBO&) {
    return 1.0;
}

#endif

enum {
    idGlobalPaintParamsUBO,
    idGlobalUBOIndex,
    globalUBOCount,
};

enum {
    idDrawableReservedVertexOnlyUBO = globalUBOCount,
    idDrawableReservedFragmentOnlyUBO,
    idProjectionUBO,
    drawableReservedUBOCount
};

)";

}
} // namespace mln
