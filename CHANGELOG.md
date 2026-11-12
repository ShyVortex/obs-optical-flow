# Changelog

All notable changes to obs-optical-flow are documented here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the
project uses [Semantic Versioning](https://semver.org/).

The release workflow publishes the section whose heading matches the
version in `CMakeLists.txt` as the release changelog, so add a section
before releasing.

## [1.1.0] - Unreleased

### Added
- **Linux and Windows support.** The plugin now builds and runs on Linux
  (OpenGL) and Windows (Direct3D 11 and OpenGL) in addition to macOS.
- Cross-platform CMake build that links the libobs of an installed OBS
  Studio; no OBS source build needed. `cmake --install` targets the
  per-user OBS plugin folder on each OS.
- Test harnesses for every platform and graphics backend
  (`--backend opengl|d3d11`, `--adapter`), registered with CTest, plus
  bit-exact output fingerprints in `flow-test`.
- GitHub Actions workflows that build all three platforms and publish
  releases on demand.

### Changed
- All shader texture reads use explicit-LOD sampling (`SampleLevel`) so the
  effect compiles with the Direct3D 11 HLSL compiler. Output is unchanged.

### Fixed
- `flow-test` and `perf-test` now set the `flow_quant` shader parameter the
  same way the filter does.

## [1.0.0] - 2026-07-04

### Added
- Initial release for macOS (Apple Silicon): real-time optical-flow frame
  interpolation filter for OBS Studio with bidirectional block-matching
  flow, occlusion-aware blending, scene-cut detection, automatic bypass,
  HDR-aware output, debug views and a low-latency mode.
