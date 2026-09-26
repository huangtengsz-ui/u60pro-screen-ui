# Copyright and third-party notices

This screen UI is a derivative of [33333s/u60pro-devui](https://github.com/33333s/u60pro-devui)
and [scoltzero/u60pro-devui-remix](https://github.com/scoltzero/u60pro-devui-remix).
The Remix project credits [Aawuxing](https://github.com/Aawuxing) for the
initial dual-SIM and per-SIM traffic pages also adapted here. The upstream
projects use the MIT license. Their original copyright notice and full MIT
text are retained in `LICENSE`.

The statically linked renderer uses these third-party components. Their full
license texts are included in `licenses/`:

| Component | License file | Source |
| --- | --- | --- |
| litehtml | `litehtml-LICENSE` | https://github.com/litehtml/litehtml |
| litehtml's Gumbo parser | `gumbo-LICENSE` | https://github.com/litehtml/litehtml/tree/master/src/gumbo |
| FreeType | `FreeType-FTL.TXT` | https://freetype.org/license.html |
| stb_image | `stb-LICENSE` | https://github.com/nothings/stb |

This package contains no device configuration, network credentials, SIM data,
identity tokens, or device backups. The backend `zwrt-datad` and existing
`屏幕管理插件` are prerequisites and are not redistributed here.
