#!/bin/bash
# Release SatSquatch's MapLibre Native build: platform/satsquatch-release.sh VERSION
# (e.g. 2026.10.19-globe-terrain-ss.8, a new one each time: SwiftPM caches a version's contents).
# Builds iOS and Android (every ABI) from this checkout's committed state and publishes them:
# - iOS: the XCFramework as the release asset of github.com/WxByte/maplibre-native-dist, whose
#   Package.swift points at it by URL and checksum;
# - Android: the AAR into github.com/WxByte/maplibre-native-maven, a Maven repository served from
#   raw.githubusercontent.com.
# Clones of both live in $RELEASE_DIR (default ~/maplibre-native-release). Needs gh signed in with
# push access to WxByte. See SATSQUATCH.md.
set -eo pipefail
V=$1
[[ $V =~ ^[0-9]{4}\.[0-9]+\.[0-9]+-.+ ]] || { echo "usage: $0 VERSION"; exit 2; }
M=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
R=${RELEASE_DIR:-$HOME/maplibre-native-release}
[ -z "$(git -C "$M" status --porcelain --untracked-files=no)" ] || { echo "commit your changes first"; exit 1; }
mkdir -p "$R"
for repo in dist maven; do
  [ -d "$R/$repo" ] || gh repo clone "WxByte/maplibre-native-$repo" "$R/$repo"
  git -C "$R/$repo" pull -q --ff-only
done
git -C "$R/dist" ls-remote --exit-code --tags origin "$V" >/dev/null && { echo "$V is already released"; exit 1; }

echo "$V" > "$M/platform/ios/VERSION"
echo "$V" > "$M/platform/android/VERSION"
git -C "$M" diff --quiet || git -C "$M" commit -qm "SatSquatch version $V" platform/ios/VERSION platform/android/VERSION
REV=$(git -C "$M" rev-parse --short=10 HEAD)

(cd "$M/platform/ios" && bazel build --compilation_mode=opt --//:renderer=metal --//:plugins=false \
  "--features=dead_strip,thin_lto" --objc_enable_binary_stripping --apple_generate_dsym \
  //platform/ios:MapLibre.dynamic --embed_label="maplibre_ios_$V")
(cd "$M/platform/android" && ./gradlew :MapLibreAndroid:publishOpenglReleasePublications \
  -PpublicationRepositoryUrl="file://$R/maven")

# The asset is named for the URL in Package.swift; gh publishes the release once it's uploaded.
ZIP=$(mktemp -d)/MapLibre.xcframework.zip
cp "$M/bazel-bin/platform/ios/MapLibre.dynamic.xcframework.zip" "$ZIP"
SUM=$(shasum -a 256 "$ZIP" | cut -d' ' -f1)
cd "$R/dist"
sed -i '' -E "s|releases/download/[^/]+/|releases/download/$V/|; s|checksum: \"[0-9a-f]+\"|checksum: \"$SUM\"|" Package.swift
git commit -qam "MapLibre $V: WxByte/maplibre-native $REV"
git push -q origin HEAD
gh release create "$V" "$ZIP" --target "$(git rev-parse HEAD)" --title "$V" \
  --notes "MapLibre Native for iOS built from WxByte/maplibre-native $REV (branch satsquatch/globe-terrain)."
git fetch -q --tags

cd "$R/maven"
git add -A
git commit -qm "MapLibre $V: WxByte/maplibre-native $REV"
git push -q origin HEAD

git -C "$M" push -q
echo "Released $V. In the app: the version in app.json, ios/Podfile and android/gradle.properties, then pod install."
