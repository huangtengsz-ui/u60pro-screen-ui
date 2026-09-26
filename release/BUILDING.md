# Building the screen renderer

`runtime-src/` is the renderer source used for this release. `ui/` contains
the page templates and generated manifest. The archive includes `stb_image.h`.

The renderer was cross-compiled for static AArch64 musl using a Bootlin
toolchain and the `runtime-src/scripts/build.sh` recipe. That recipe expects
FreeType and litehtml v0.10 static libraries in the paths described by the
script. Their source and licenses are available from the projects named in
`NOTICE.md`; the libraries are not bundled in this source archive. This means
the archive is sufficient to inspect our changes but still needs those build
dependencies to reproduce the binary.

The prebuilt executable was tested on the user's U60 Pro B31 with the existing
`zwrt-datad` screen backend. Use the SHA-256 values in `SHA256SUMS` to identify
the exact release assets.
