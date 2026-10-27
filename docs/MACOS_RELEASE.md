# Building a signed development release

The `dev` app is built from a clean, committed revision of this fork. It includes the integrated
client features described in the README; server binaries and game files are not bundled.
Development releases are published as GitHub prereleases, with a source tag and SHA-256 checksum.

## Optimized build

After bootstrapping both dependency architectures as described in the README:

```sh
BB_BUILD_TYPE=Release BB_LTO=ON bash build_macos.sh --test
```

This builds the C/C++ runtime and native hosts with `-O3` and LTO, both renderer architectures
with CMake Release optimization and ThinLTO, and the Swift launcher with `-O`. Timing code
retains `-ffp-contract=off -fno-fast-math`; test assertions remain enabled. CMake caches created
with Makefiles can be reused by setting `CMAKE_GENERATOR='Unix Makefiles'`. Set `BB_BUILD_JOBS`
to limit parallel compilation (default 6). PGO is not supported by the current macOS build.

Resource-binding caching, pipeline/shader caches, fullscreen load avoidance and lossless texture
compression retain their enabled defaults. Fresh launcher configurations use MetalFX Temporal
Quality, 1080p output, balanced shader compilation and a 60 FPS cap. Existing saved settings
and explicit environment overrides are preserved. Seamless co-op retains its fixed 60 FPS
timing. Frame generation, HDR, added lighting and native velocity remain optional experiments;
these are not required compiler or renderer optimizations.

## Signing and notarization

Install a Developer ID Application certificate with its private key in the login keychain.
Store the matching Apple notarization credentials with `xcrun notarytool store-credentials`.
Do not put private keys, Apple passwords or tokens in this repository.

```sh
BB_SIGN_IDENTITY='Developer ID Application: Your Name (TEAMID)' \
BB_NOTARY_PROFILE='your-keychain-profile' \
bash scripts/macos/package_app.sh 0.5.10-dev.20261027.1
```

Packaging signs each nested Mach-O file before signing the app. Developer ID signatures use
the hardened runtime and secure timestamps. Only `bb-probe`, which maps the user's x86-64 game
and generates compatibility trampolines, receives the unsigned executable memory entitlement.
Library validation remains enabled; no debugger or get-task-allow entitlement is added.

Packaging submits the ZIP to Apple, requires an Accepted result, staples and validates the
ticket, checks the app with Gatekeeper, then creates the final ZIP and its `.sha256` file.
The notarization response is retained locally under `dist/notarization-<version>.json`.
Leaving both signing variables unset produces an ad-hoc local package without notarization.

Test the actual signed package in-game before publishing, using isolated copies of saves and
hashes of original game files. Verify the unpacked final ZIP again with `codesign --verify
--deep --strict`, `xcrun stapler validate` and `spctl --assess --type execute`. Record the build
flags, source SHA, server and limits of gameplay coverage in release notes.

Fork release tags do not run the upstream ad-hoc upload job: this prevents CI from replacing
a signed release asset. Manual workflow runs still produce build artifacts. Publish the final
ZIP and checksum on this fork; automatic launcher checks use `evill33t/bloodborne_mac_port`.
Development prereleases appear when the player enables **Include test builds**.

## Sources and authors

- **Mark Hall / Wooloomooloo2 and contributors**: the original build, self-contained packaging,
  launcher and direct Metal runtime in
  [bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port).
- **Ronny Stiftel / evill33t**: this fork's release flags, Developer ID packaging, signing helper
  and fork update routing.
- **Apple**: [notarization requirements](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution),
  [hardened runtime](https://developer.apple.com/documentation/security/hardened-runtime) and
  [unsigned executable memory entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.allow-unsigned-executable-memory).
- The package retains all bundled third-party license and provenance notices under
  `Contents/Resources/licenses`. See the README's credits, `docs/ONLINE_PROVENANCE.md`,
  `gpu/bbseamless/PROVENANCE.md` and `assets/branding/README.md` for original client and artwork
  authors; the signing workflow does not change their ownership or license terms.
