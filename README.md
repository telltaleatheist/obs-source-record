# Source Record — fork with synchronized starts and a recording panel

This is a fork of [exeldro/obs-source-record](https://github.com/exeldro/obs-source-record). It adds:

- **Synchronized start.** Upstream starts each source recording one after another as the Record
  button is pressed, each first initializing its own encoder, so the files begin up to a second or
  more apart. Here every recording set to follow the main recording ("Recording" mode) is started
  as one batch: all encoders are initialized in parallel, then all outputs start together, so the
  files begin on the same frame.
- **A sync report.** When recording stops, `<main recording name>.sync.json` is written next to the
  source recordings with each file's exact start offset from the main recording (from the render
  timestamp of each file's first frame), in nanoseconds and frames.
- **A Recording panel** (Docks → Recording), laid out like the Multiple output dock: Start all /
  Stop all, the main recording, and one row per source recording with a tick (record with Start
  all), its own Start/Stop, and live status (time, size, dropped frames). Stopping asks first.
- A build using the current obs-plugintemplate layout (`cmake --preset windows-x64`, OBS 32.2).

Measured on OBS 32.2 (Windows, NVENC): source recordings started together are frame-exact against
each other; the main recording's picture runs one frame ahead of its own timestamp relative to them,
which the report notes. Everything below is the upstream README.

---

# Source Record filter for OBS Studio

Plugin for OBS Studio to make sources available to record via a filter

# Download

https://obsproject.com/forum/resources/source-record.1285/

# Build
1. In-tree build
    - Build OBS Studio: https://obsproject.com/wiki/Install-Instructions
    - Check out this repository to plugins/source-record
    - Add `add_subdirectory(source-record)` to plugins/CMakeLists.txt
    - Rebuild OBS Studio

1. Stand-alone build (Linux only)
    - Verify that you have package with development files for OBS
    - Check out this repository and run `cmake -S . -B build -DBUILD_OUT_OF_TREE=On && cmake --build build`

# Donations
https://www.paypal.me/exeldro
