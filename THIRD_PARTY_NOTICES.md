# Third-Party Notices

DirBridge uses third-party libraries and assets. This file summarizes the dependencies used by the current Windows build and points to the licenses that must be kept with source and binary distributions.

This notice is informational and does not replace the license text of each dependency.

## Project License

DirBridge source code is licensed under the Apache License 2.0. See `LICENSE`.

## Runtime and Build Dependencies

| Component | Version / Source | Use | License notes |
| --- | --- | --- | --- |
| Qt Widgets / Qt Svg | Qt 6.8.0 in the current Windows build environment | GUI framework and SVG rendering | Qt is available under LGPL/GPL/commercial terms depending on the chosen distribution. The Windows package dynamically deploys Qt runtime files. Keep Qt license files with release assets when preparing a public binary release. |
| libcurl | `8.20.0_5` Windows MinGW package from curl.se | FTP/SFTP protocol access | curl license. The Windows package may include additional bundled libraries such as LibreSSL, libssh2, nghttp2/nghttp3/ngtcp2, Brotli, zlib-ng, zstd and public suffix/certificate data. Keep the copied license files under `licenses/curl/` in release packages. |
| nlohmann/json | `3.12.0` | JSON configuration parsing and serialization | MIT License. |
| spdlog | `1.17.0` | Logging | MIT License. |
| Fluent UI System Icons | Repository asset copy under `resources/icons/fluent/` | Toolbar and UI icons | MIT License. See `resources/licenses/FluentUI-System-Icons-LICENSE.txt`. |
| MinGW runtime libraries | From the configured MinGW compiler runtime directory | C/C++ runtime DLLs for Windows package | GCC runtime libraries are distributed with their own license terms and runtime exceptions. Keep the relevant runtime notices when preparing a public binary release. |

## Release Package Rule

Windows release packages should include:

- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `CHANGELOG.md`
- `README.md`
- `resources/licenses/FluentUI-System-Icons-LICENSE.txt`
- copied license files for bundled libcurl package dependencies when available under `third_party/_source`

If dependency versions change, update this file and `deps.lock.json` in the same change.
