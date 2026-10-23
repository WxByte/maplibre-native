#pragma once

#include <mln/shaders/shader_source.hpp>
#include <mln/shaders/webgpu/shader_program.hpp>

namespace mln {
namespace shaders {

// The enum values are already defined in layer_ubo.hpp
// WebGPU uses the same indices as Metal

template <>
struct ShaderSource<BuiltIn::Prelude, gfx::Backend::Type::WebGPU> {
    static constexpr const char* name = "Prelude";

    // Common WGSL code that can be included in other shaders
    static constexpr auto prelude = R"(
// Constants
const PI: f32 = 3.14159265358979323846;
const LINE_NORMAL_SCALE: f32 = 1.0 / 63.0;  // 1.0 / (127 / 2)
const MAX_LINE_DISTANCE: f32 = 32767.0;
const SDF_PX: f32 = 8.0;

// WebGPU equivalent of GLSL mod function
fn glMod(x: f32, y: f32) -> f32 {
    return x - y * floor(x / y);
}

fn glMod2(x: vec2<f32>, y: f32) -> vec2<f32> {
    return x - y * floor(x / y);
}

fn glMod2v(x: vec2<f32>, y: vec2<f32>) -> vec2<f32> {
    return x - y * floor(x / y);
}

// GLSL compatibility helpers
fn gl_mod(x: vec2<f32>, y: vec2<f32>) -> vec2<f32> {
    return glMod2v(x, y);
}

// Sample the terrain elevation in meters at a tile-local coordinate, with manual
// bilinear interpolation on DEM pixel centers (the DEM has a 2px backfilled border),
// matching the maplibre-gl-js get_elevation() prelude function. Unlike gl-js (global
// terrain uniforms), MapLibre Native carries the DEM data per-drawable, so the DEM
// texture and dem_* values are passed in as arguments rather than read from globals.
// Decode one DEM texel to meters. The clamp at the call sites matters: textureLoad out of
// range is undefined where the sampler's clamp-to-edge used to cover it, and a tile with
// no DEM of its own is bound the 1x1 placeholder, whose only valid texel is (0, 0).
fn dem_texel(dem: texture_2d<f32>, texel: vec2<i32>, dem_unpack: vec4<f32>) -> f32 {
    var data = textureLoad(dem, texel, 0) * 255.0;
    data.a = -1.0;
    return dot(data, dem_unpack);
}

fn get_elevation(pos: vec2<f32>,
                 dem: texture_2d<f32>,
                 dem_sampler: sampler,
                 dem_coords: vec4<f32>,
                 dem_unpack: vec4<f32>,
                 dem_dim: f32,
                 dem_exaggeration: f32,
                 dem_enabled: f32) -> f32 {
    if (dem_enabled == 0.0) {
        return 0.0;
    }
    let coord = (pos * dem_coords.x + dem_coords.yz) * dem_dim + 2.0;
    let f = fract(coord);
    let c = vec2<i32>(floor(coord));
    let hi = vec2<i32>(textureDimensions(dem, 0)) - vec2<i32>(1);
    let tl = dem_texel(dem, clamp(c, vec2<i32>(0), hi), dem_unpack);
    let tr = dem_texel(dem, clamp(c + vec2<i32>(1, 0), vec2<i32>(0), hi), dem_unpack);
    let bl = dem_texel(dem, clamp(c + vec2<i32>(0, 1), vec2<i32>(0), hi), dem_unpack);
    let br = dem_texel(dem, clamp(c + vec2<i32>(1, 1), vec2<i32>(0), hi), dem_unpack);
    let elevation = mix(mix(tl, tr, f.x), mix(bl, br, f.x), f.y);
    return elevation * dem_exaggeration;
}

// Radians conversion
// Place a clip-space position computed with a tile-local drape matrix into the
// current terrain drape render target (see the GL prelude for the derivation).
// `matrix` carries the drawable's tile (z, x, y) in its unused third column and
// target_tile is GlobalPaintParamsUBO drape_tile (w != 0 while drawing into a
// drape target).
fn apply_drape_transform(clip: vec4<f32>, matrix: mat4x4<f32>, target_tile: vec4<f32>) -> vec4<f32> {
    if (target_tile.w == 0.0) {
        return clip;
    }
    let tile = matrix[2].xyz;
    let k = target_tile.x - tile.x; // target zoom - drawable zoom
    let scale = exp2(k);
    var offset: vec2<f32>;
    if (k >= 0.0) {
        offset = tile.yz * scale - target_tile.yz;
    } else {
        offset = (tile.yz - target_tile.yz * exp2(-k)) * scale;
    }
    var result = clip;
    result.x = clip.x * scale + (scale - 1.0 + 2.0 * offset.x);
    result.y = clip.y * scale + (1.0 - scale - 2.0 * offset.y);
    return result;
}

// Unpack a depth value packed by the terrain depth pass (webgpu/terrain_depth.hpp),
// converted to NDC z, as in the maplibre-gl-js prelude
fn unpack_depth(rgba_depth: vec4<f32>) -> f32 {
    let bit_shift = vec4<f32>(1.0 / (256.0 * 256.0 * 256.0), 1.0 / (256.0 * 256.0), 1.0 / 256.0, 1.0);
    return dot(rgba_depth, bit_shift) * 2.0 - 1.0;
}

// Opacity of a fragment behind the terrain, in [0, 1]: 1 fully visible, 0 fully
// hidden, with a soft ramp over ~0.002 NDC depth and a small bias so geometry
// sitting exactly on the terrain surface (e.g. a label anchored to it) does not
// occlude itself. Matches the maplibre-gl-js depthOpacity() prelude function.
// WebGPU has y-up NDC but a top-left texture origin (like Metal, unlike GL), so
// the V coordinate is flipped here where the GL version does not flip.
fn depth_opacity(frag: vec3<f32>,
                 depth_texture: texture_2d<f32>,
                 depth_sampler: sampler) -> f32 {
    let uv = frag.xy * 0.5 + 0.5;
    let d = unpack_depth(textureSampleLevel(depth_texture, depth_sampler, vec2<f32>(uv.x, 1.0 - uv.y), 0.0)) +
            0.0001 - frag.z;
    return 1.0 - max(0.0, min(1.0, -d * 500.0));
}

// Whether a clip-space position is visible in front of the terrain, from the
// packed terrain depth texture, matching maplibre-gl-js calculate_visibility().
fn calculate_visibility(pos: vec4<f32>,
                        depth_texture: texture_2d<f32>,
                        depth_sampler: sampler,
                        depth_enabled: f32) -> f32 {
    if (depth_enabled == 0.0) {
        return 1.0;
    }
    let frag = pos.xyz / pos.w;
    // check if coordinate is fully visible
    let d = depth_opacity(frag, depth_texture, depth_sampler);
    if (d > 0.95) {
        return 1.0;
    }
    // if not, sample some pixels above: a label whose anchor is just behind a
    // ridge still shows if its glyphs poke above it (maplibre-gl-js behaviour)
    return (d + depth_opacity(frag + vec3<f32>(0.0, 0.01, 0.0), depth_texture, depth_sampler)) / 2.0;
}

fn radians(degrees: f32) -> f32 {
    return PI * degrees / 180.0;
}

// Helper functions for unpacking data
fn unpack_float(packedValue: f32) -> vec2<f32> {
    let packedIntValue = i32(packedValue);
    let v0 = packedIntValue / 256;
    return vec2<f32>(f32(v0), f32(packedIntValue - v0 * 256));
}

fn unpack_opacity(packedOpacity: f32) -> vec2<f32> {
    let intOpacity = i32(packedOpacity) / 2;
    return vec2<f32>(f32(intOpacity) / 127.0, glMod(packedOpacity, 2.0));
}

// Decode a color that has been packed into two floats
fn decode_color(encoded: vec2<f32>) -> vec4<f32> {
    let e0 = unpack_float(encoded[0]) / 255.0;
    let e1 = unpack_float(encoded[1]) / 255.0;
    return vec4<f32>(e0.x, e0.y, e1.x, e1.y);
}

// Unpack and interpolate between two float values
fn unpack_mix_float(packedValue: vec2<f32>, t: f32) -> f32 {
    return mix(packedValue[0], packedValue[1], t);
}

// Unpack and interpolate between two colors
fn unpack_mix_color(packedColors: vec4<f32>, t: f32) -> vec4<f32> {
    let minColor = decode_color(vec2<f32>(packedColors[0], packedColors[1]));
    let maxColor = decode_color(vec2<f32>(packedColors[2], packedColors[3]));
    return mix(minColor, maxColor, t);
}

// Get pattern position for texture atlas lookups
fn get_pattern_pos(pixel_coord_upper: vec2<f32>, pixel_coord_lower: vec2<f32>,
                   pattern_size: vec2<f32>, tile_units_to_pixels: f32, pos: vec2<f32>) -> vec2<f32> {
    let offset_a = gl_mod(pixel_coord_upper, pattern_size) * 256.0;
    let offset_b = gl_mod(offset_a, pattern_size) * 256.0 + pixel_coord_lower;
    let offset = gl_mod(offset_b, pattern_size);
    return (tile_units_to_pixels * pos + offset) / pattern_size;
}

struct ProjectionUBO {
    matrix: mat4x4<f32>,
    fallback_matrix: mat4x4<f32>,
    tile_mercator_coords: vec4<f32>,
    clipping_plane: vec4<f32>,
    projection_transition: f32,
    depth_offset: f32,
    translate: vec2<f32>,
};

// Pole vertices carry these sentinel Y values in their raw position.
const GLOBE_POLE_NORTH_Y: f32 = -32767.5;
const GLOBE_POLE_SOUTH_Y: f32 = 32766.5;

#ifdef PROJECTION_GLOBE

const GLOBE_RADIUS: f32 = 6371008.8;

// Tile position (0..EXTENT) to a point on the unit sphere; the pole sentinels in rawPos map to the poles.
fn projectToSphere(translatedPos: vec2<f32>, rawPos: vec2<f32>, projection: ProjectionUBO) -> vec3<f32> {
    let mercator_pos = projection.tile_mercator_coords.xy + projection.tile_mercator_coords.zw * translatedPos;
    let spherical_x = mercator_pos.x * PI * 2.0 + PI;
    // sin/cos of the latitude from the Mercator Y via the tangent half-angle identities: no atan, and float32 precision survives near the equator.
    let t = exp(PI - (mercator_pos.y * PI * 2.0));
    let t2 = t * t;
    let denom = t2 + 1.0;
    let sin_sy = (t2 - 1.0) / denom;
    let cos_sy = (2.0 * t) / denom;
    var pos = vec3<f32>(sin(spherical_x) * cos_sy, sin_sy, cos(spherical_x) * cos_sy);
    if (rawPos.y < GLOBE_POLE_NORTH_Y) {
        pos = vec3<f32>(0.0, 1.0, 0.0);
    }
    if (rawPos.y > GLOBE_POLE_SOUTH_Y) {
        pos = vec3<f32>(0.0, -1.0, 0.0);
    }
    return pos;
}

fn globeRotateVector(vec: vec3<f32>, angles: vec2<f32>) -> vec3<f32> {
    var axisRight = vec3<f32>(vec.z, 0.0, -vec.x);
    var axisUp = cross(axisRight, vec);
    axisRight = normalize(axisRight);
    axisUp = normalize(axisUp);
    let t = tan(angles);
    return normalize(vec + axisRight * t.x + axisUp * t.y);
}

// cos(latitude) at a tile Y, from the same exp() form as projectToSphere.
fn circumferenceRatioAtTileY(tileY: f32, projection: ProjectionUBO) -> f32 {
    let mercator_pos_y = projection.tile_mercator_coords.y + projection.tile_mercator_coords.w * tileY;
    let t = exp(PI - (mercator_pos_y * PI * 2.0));
    return (2.0 * t) / (t * t + 1.0);
}

fn projectLineThickness(tileY: f32, projection: ProjectionUBO) -> f32 {
    let thickness = 1.0 / circumferenceRatioAtTileY(tileY, projection);
    if (projection.projection_transition < 0.999) {
        return mix(1.0, thickness, projection.projection_transition);
    }
    return thickness;
}

fn globeComputeClippingZ(spherePos: vec3<f32>, projection: ProjectionUBO) -> f32 {
    return 1.0 - (dot(spherePos, projection.clipping_plane.xyz) + projection.clipping_plane.w);
}

fn interpolateProjection(posInTile: vec2<f32>, spherePos: vec3<f32>, elevation: f32, projection: ProjectionUBO) -> vec4<f32> {
    let elevatedPos = spherePos * (1.0 + elevation / GLOBE_RADIUS);
    var globePosition = projection.matrix * vec4<f32>(elevatedPos, 1.0);
    // Clip the far side of the globe through Z; the layer's depth shift keeps layer order.
    globePosition.z = globeComputeClippingZ(elevatedPos, projection) * globePosition.w - projection.depth_offset;

    if (projection.projection_transition > 0.999) {
        return globePosition;
    }

    let flatPosition = projection.fallback_matrix * vec4<f32>(posInTile, elevation, 1.0);
    let z_globeness_threshold = 0.2;
    var result = globePosition;
    result.z = mix(0.0, globePosition.z, clamp((projection.projection_transition - z_globeness_threshold) / (1.0 - z_globeness_threshold), 0.0, 1.0));
    let xyw = mix(flatPosition.xyw, globePosition.xyw, projection.projection_transition);
    result.x = xyw.x;
    result.y = xyw.y;
    result.w = xyw.z;
    if ((posInTile.y < GLOBE_POLE_NORTH_Y) || (posInTile.y > GLOBE_POLE_SOUTH_Y)) {
        result = globePosition;
        let poles_hidden_anim_percentage = 0.02;
        result.z = mix(globePosition.z, 100.0, pow(max((1.0 - projection.projection_transition) / poles_hidden_anim_percentage, 0.0), 8.0));
    }
    return result;
}

// Keeps the matrix Z, for geometry that needs the depth buffer.
fn interpolateProjectionFor3D(posInTile: vec2<f32>, spherePos: vec3<f32>, elevation: f32, projection: ProjectionUBO) -> vec4<f32> {
    let elevatedPos = spherePos * (1.0 + elevation / GLOBE_RADIUS);
    let globePosition = projection.matrix * vec4<f32>(elevatedPos, 1.0);
    if (projection.projection_transition > 0.999) {
        return globePosition;
    }
    let fallbackPosition = projection.fallback_matrix * vec4<f32>(posInTile, elevation, 1.0);
    return mix(fallbackPosition, globePosition, projection.projection_transition);
}

fn projectTile(pos: vec2<f32>, projection: ProjectionUBO) -> vec4<f32> {
    return interpolateProjection(pos, projectToSphere(pos + projection.translate, vec2<f32>(0.0, 0.0), projection), 0.0, projection);
}

// The zoom 0 tile's buffer wraps around the planet onto the tile itself, so the line shaders discard the fragments
// beyond its X extent; every other tile (a mercator width below the whole world's) hands on a value inside it.
fn antimeridianClipX(posInTile: vec2<f32>, projection: ProjectionUBO) -> f32 {
    return select(0.0, posInTile.x, projection.tile_mercator_coords.z * 8192.0 >= 1.0);
}

fn clippedAtAntimeridian(tileX: f32) -> bool {
    return tileX < 0.0 || tileX >= 8192.0;
}

// The variant for geometry that can carry pole vertices; rawPos is the untranslated position.
fn projectTileWithPoles(pos: vec2<f32>, rawPos: vec2<f32>, projection: ProjectionUBO) -> vec4<f32> {
    return interpolateProjection(pos, projectToSphere(pos + projection.translate, rawPos, projection), 0.0, projection);
}

fn projectTileWithElevation(pos: vec2<f32>, elevation: f32, projection: ProjectionUBO) -> vec4<f32> {
    return interpolateProjection(pos, projectToSphere(pos + projection.translate, vec2<f32>(0.0, 0.0), projection), elevation, projection);
}

fn projectTileFor3D(pos: vec2<f32>, elevation: f32, projection: ProjectionUBO) -> vec4<f32> {
    return interpolateProjectionFor3D(pos, projectToSphere(pos + projection.translate, pos, projection), elevation, projection);
}

#else

fn projectTile(pos: vec2<f32>, projection: ProjectionUBO) -> vec4<f32> {
    return projection.matrix * vec4<f32>(pos, 0.0, 1.0);
}

// Pole vertices only exist on the globe; put them behind the near plane so their triangles are clipped.
fn projectTileWithPoles(pos: vec2<f32>, rawPos: vec2<f32>, projection: ProjectionUBO) -> vec4<f32> {
    var result = projection.matrix * vec4<f32>(pos, 0.0, 1.0);
    if (rawPos.y < GLOBE_POLE_NORTH_Y || rawPos.y > GLOBE_POLE_SOUTH_Y) {
        result.z = -10000000.0;
    }
    return result;
}

fn projectTileWithElevation(pos: vec2<f32>, elevation: f32, projection: ProjectionUBO) -> vec4<f32> {
    return projection.matrix * vec4<f32>(pos, elevation, 1.0);
}

fn projectTileFor3D(pos: vec2<f32>, elevation: f32, projection: ProjectionUBO) -> vec4<f32> {
    return projection.matrix * vec4<f32>(pos, elevation, 1.0);
}

fn projectLineThickness(tileY: f32, projection: ProjectionUBO) -> f32 {
    return 1.0;
}

#endif
)";
};

} // namespace shaders
} // namespace mln
