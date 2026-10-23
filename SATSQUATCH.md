# SatSquatch's MapLibre Native build

Branch `satsquatch/globe-terrain`: the globe (PR #4533, `refs/pull/4533/head`) with 3D terrain
(`feature/terrain-3d`, draft PR #4190) merged in, for the SatSquatch app (iOS and Android through
MapLibre React Native). Neither upstream release has both.

## How the two fit together

- Terrain is for the flat map only. A style that can show the globe at some zoom keeps it off
  (`map_impl.cpp`, `Projection::Impl::canShowGlobe`): the drape targets and the elevated layers'
  elevation have no globe variant.
- Shaders keep the globe's projection (`projectTile`, `ProjectionUBO`) and add the terrain's:
  - draped layers (background, fill*, line*, raster, hillshade, color-relief) apply
    `apply_drape_transform` after `projectTile`;
  - `LayerTweaker::getProjectionData` takes the drape flag (off by default; those layers' tweakers ask
    for it), so while draping the ProjectionUBO and the drawable's `matrix` are the drape target's;
  - circles and symbols take the terrain's elevation where the globe had 0 (0 on the globe).
- `SymbolDrawableUBO`: the globe's fields, then the terrain's DEM fields from offset 288 (21 × 16), in
  C++, GLSL and Metal alike.
- GL shader headers (`include/mln/shaders/gl`) are generated: `node shaders/generate_shader_code.mjs`.
- Vulkan and WebGPU shaders are the globe side only: SatSquatch builds OpenGL (Android) and Metal (iOS).

## Building

Submodules: `git submodule update --init --recursive --depth 1`, without vcpkg, Vulkan, glslang and
WebGPU, which neither build uses.

Android (NDK 28.2, CMake ≥ 3.24, JDK 21), into a Maven repository:

    cd platform/android
    ./gradlew -Pmaplibre.abis=arm64-v8a :MapLibreAndroid:publishOpenglReleasePublications \
        -PpublicationRepositoryUrl=file://$HOME/maplibre-native-maven

(`-Pmaplibre.abis` left out builds every ABI, for store builds.) The version is `platform/android/VERSION`.

iOS (Bazel through bazelisk), an XCFramework:

    cd platform/ios
    bazel build --compilation_mode=opt --//:renderer=metal --//:plugins=false \
        "--features=dead_strip,thin_lto" --objc_enable_binary_stripping --apple_generate_dsym \
        //platform/ios:MapLibre.dynamic --embed_label=maplibre_ios_$(cat VERSION)

It's unzipped into the Swift package at `~/maplibre-native-dist` (a binary target by path), which the
app's `spmSpec` points at. The simulator slice is arm64 only (`platform/ios/BUILD.bazel`).
