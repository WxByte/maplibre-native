# SatSquatch's MapLibre Native build

Branch `satsquatch/globe-terrain`: the globe (PR #4533, `refs/pull/4533/head`) with 3D terrain
(`feature/terrain-3d`, draft PR #4190) merged in, for the SatSquatch app (iOS and Android through
MapLibre React Native). Neither upstream release has both.

## How the two fit together

- Terrain shows on the flat map and on the globe at every zoom. On the globe the tiles of the top and bottom
  rows also cover the polar caps past them, as the plain globe's do (`RenderTerrain::createDrawableForTile`:
  their edge row joined to the shaders' pole sentinel, at sea level). The background drapes into every terrain
  tile (`RenderBackgroundLayer`, as GL JS draws it into each render-to-texture tile), and on the globe draping
  matches and places tiles without their wrap (`RenderTarget::drapeTile`: covers can give a tile and its
  parent different wraps on the far side). On the globe the mesh's shader is its globe variant
  (`projectTileFor3D` through each tile's `ProjectionUBO`), the draped layers render into the terrain's tiles
  through the drape matrix alone (`LayerTweaker::drapeProjectionData`), the mesh writes the ground's depth
  instead of the sea-level sphere, and the camera's centre anchors on the terrain on the flat map only.
  Under terrain the globe's tile covers take in the heights of the terrain loaded in view
  (`util::TileElevationProvider::getViewElevationRange`, as GL JS takes the terrain's), so relief rising towards the
  camera keeps its tiles at the screen's near edge; the view's range rather than each tile's, which flipped the
  cover every frame. The mesh's fallback matrix, which the globe blends to between zoom 11 and 12, is the flat
  map's tile matrix (`PaintParameters::projectionDataForTile(...).fallbackMatrix`; `matrixForTile` is the
  sphere's on the globe).
- With terrain, covers vary their zoom with the distance from the camera (`TileLodMode::Adaptive`, as GL JS
  does), and the mesh's tile cap keeps the tiles nearest the camera. On the flat map a tile's culling box rises by
  its terrain's height in the frustum's own units (`numTiles / worldSize`: the frustum's z, the inverse
  projection's, is in meters).
- For the app's custom layers: the terrain's heights (`CustomLayerRenderParameters::terrainElevation`, iOS
  `MLNMapView terrainElevationsAtCoordinates:`), drawn from the mesh's own DEM cache; with terrain they draw
  in the 3D depth range; and they can drape onto the terrain (`CustomLayerHost::drapeRevision` and
  `renderToTile`, iOS `MLNCustomStyleLayer drapeRevision:` and `drawTileWithZoom:`): a tile layer group with a
  drawable per drape target, its revision folded into the drape's content signatures; only the target's own
  drawable draws it (the flat map's dilated cover overlaps, and each drawable would draw the whole target
  again).
- Shaders keep the globe's projection (`projectTile`, `ProjectionUBO`) and add the terrain's:
  - draped layers (background, fill*, line*, raster, hillshade, color-relief) apply
    `apply_drape_transform` after `projectTile` (Metal's line shaders in their globe branch too);
  - in a drape target the tile clipping masks are placed as the drape places the tiles
    (`PaintParameters::globeClipMasks`: the globe's own masks are the screen's), in a stencil the pass
    clears (Metal's render pass honours `clearStencil`; the simulator's combined depth/stencil texture is the
    drape target's stencil too, as a device's separate one is);
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

(`-Pmaplibre.abis` left out builds every ABI, as releases do.) The version is `platform/android/VERSION`.

iOS (Bazel through bazelisk), an XCFramework (`bazel-bin/platform/ios/MapLibre.dynamic.xcframework.zip`):

    cd platform/ios
    bazel build --compilation_mode=opt --//:renderer=metal --//:plugins=false \
        "--features=dead_strip,thin_lto" --objc_enable_binary_stripping --apple_generate_dsym \
        //platform/ios:MapLibre.dynamic --embed_label=maplibre_ios_$(cat VERSION)

The simulator slice is arm64 only (`platform/ios/BUILD.bazel`).

## Releases

`platform/satsquatch-release.sh VERSION` builds both from the committed branch and publishes them, public on
GitHub so any machine (EAS too) builds the app without this checkout:

- iOS: [WxByte/maplibre-native-dist](https://github.com/WxByte/maplibre-native-dist), a Swift package whose
  binary target is the version's release asset (`MapLibre.xcframework.zip`) by URL and checksum. The app's
  `spmSpec` (app.json, ios/Podfile) asks for its exact version.
- Android: [WxByte/maplibre-native-maven](https://github.com/WxByte/maplibre-native-maven), a Maven
  repository (`org.maplibre.gl:android-sdk-opengl`) the app's Gradle reads from
  `https://raw.githubusercontent.com/WxByte/maplibre-native-maven/main`.

Versions are `2026.10.19-globe-terrain-ss.N` (upstream's base, then ours), a new one per release: SwiftPM
caches a version's contents.
