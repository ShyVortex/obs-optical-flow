**obs-optical-flow** is a video filter for [OBS Studio](https://obsproject.com) that does real-time **optical-flow frame interpolation** (motion smoothing, like the "MEMC" feature of modern TVs). Put a 24 or 30 fps source (a video, a camera, a console through a capture card) into a 60 fps canvas and the filter replaces OBS' duplicated frames with motion-compensated in-between frames, computed entirely on the GPU.

## Version {{VERSION}} ({{DATE}})

{{CHANGELOG}}

{{COMPARE}}

## Downloads

| Platform | File | Graphics |
|---|---|---|
| Windows 10/11 (x64) | `obs-optical-flow-{{VERSION}}-windows-x64.zip` | Direct3D 11, OpenGL |
| Linux (x86_64) | `obs-optical-flow-{{VERSION}}-linux-x86_64.tar.gz` | OpenGL |
| macOS 13+ (Apple Silicon) | `obs-optical-flow-{{VERSION}}-macos-arm64.zip` | OpenGL |

Built against OBS Studio {{OBS_VERSION}}; requires OBS {{OBS_VERSION}} or newer. `SHA256SUMS.txt` lists the checksum of each file.

## Installation

Close OBS first, then:

- **Windows:** extract the zip into `C:\ProgramData\obs-studio\plugins\`, so you get `C:\ProgramData\obs-studio\plugins\obs-optical-flow\bin\64bit\obs-optical-flow.dll`.
- **Linux:** `mkdir -p ~/.config/obs-studio/plugins && tar -xzf obs-optical-flow-{{VERSION}}-linux-x86_64.tar.gz -C ~/.config/obs-studio/plugins`. For OBS installed from a distro package; Flatpak OBS is not supported. On hybrid-GPU laptops start OBS on the discrete GPU (e.g. `prime-run obs`).
- **macOS:** extract the zip and move `obs-optical-flow.plugin` into `~/Library/Application Support/obs-studio/plugins/`. The plugin is ad-hoc signed, not notarized: if macOS blocks it, run `xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-optical-flow.plugin`.

## Usage

1. Set the OBS canvas frame rate higher than your source: **Settings → Video → Common FPS Values → 60** (or a custom integer FPS such as 120).
2. Right-click the source → **Filters** → **+** → **Optical Flow Frame Interpolation**.
3. Set **Debug view** to **Split screen** to compare: the left half shows the original, the right half the interpolated result.

The filter adds one source-frame interval plus one canvas tick of delay (about 50 ms for 30 → 60 fps, 33 ms in low-latency mode), so you may want to add a matching audio sync offset to the source.

Full documentation: https://github.com/{{REPO}}#readme
