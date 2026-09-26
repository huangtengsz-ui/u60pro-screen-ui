# U60 Pro 三页屏幕 UI · 1.0.0

这个目录包含 UFI-TOOLS 插件商店的单文件插件和屏幕资源。商店只收一个 `.js` 或 `.txt` 文件，因此投稿选择 `U60Pro三页屏幕UI.js`；屏幕程序和页面优先从 [GitHub Releases](https://github.com/huangtengsz-ui/u60pro-screen-ui/releases) 下载，设备连不上 `github.com` 时改用同仓库的 `raw.githubusercontent.com` 固定标签地址。`v1.0.0` Release 包含 `u60pro-devui-aarch64`、`ui.tar.gz`、`source.tar.gz`、`version.json`、`SHA256SUMS`、插件的 ASCII 文件名副本和许可文件包 `licenses.tar.gz`。

## 适用范围

- 已在 U60 Pro B31、OpenWrt 23.05.4、AArch64、320×480 屏幕上验证当前程序和页面。其他固件与硬件版本仍需测试。
- 需先安装现有「u60屏幕管理插件」及 `zwrt-datad`。此包只替换屏幕渲染程序和页面文件，不安装后端，不更改开机脚本、系统时间、网络 ADB 或用户配置。
- 当前设计为三页主界面，设置集中在二级页；下拉面板用于快捷操作。飞猫专属 UI 文件不在此包内。
- 从原「屏幕管理插件」的旧更新源更新 devui 或 UI 会覆盖此版本；需要时可通过本插件重新安装。原管理插件中的旧兼容性提示可能仍基于其历史版页面检查。
- 本版基于 [33333s 原项目](https://github.com/33333s/u60pro-devui)和 [scoltzero Remix](https://github.com/scoltzero/u60pro-devui-remix) 改版；双卡与分卡流量页面的初始实现致谢 [Aawuxing](https://github.com/Aawuxing)。许可与第三方清单见 `NOTICE.md`。

## 文件与版本

- 商店文件：`U60Pro三页屏幕UI.js`，插件版本 `1.0.0`。
- 渲染程序：`u60pro-devui-aarch64`，资源版本 `1.3.0-huang.1`。
- 页面：`ui.tar.gz`，资源版本 `0.5.0-huang.1`。
- 设备安装前先校验程序和页面 SHA-256；原程序和页面在设备内保留为 `.screen-ui-backup-*`，可通过插件“恢复上个版本”回退。

## 发布流程

1. 在公开仓库 `huangtengsz-ui/u60pro-screen-ui` 保留源代码、此说明和版权文件。
2. 在 `v1.0.0` Release 挂载附件，核对公开下载 URL 与脚本中的 `RELEASE_URL`，并验证 SHA-256。
3. 在 [UFI-TOOLS 社区投稿页](https://ufitools.ikuns.top/submit)上传**单个** `.js` 文件，填写版本 `1.0.0` 和不超过 120 字的说明，等待审核。
4. 审核通过后，用测试设备从商店安装并验证；后续更新沿用同一文件名和更高版本号。

建议投稿说明：`基于 33333s 原版与 scoltzero Remix 改版，致谢 Aawuxing 双卡页面。U60 Pro B31 三页 UI，突出网速、信号和流量；需先安装屏幕管理插件及 datad，支持备份回退。`

## 投稿表单填写

2026-09-26 核对社区投稿页与站点配置：当前只接收 `.js`、`.txt`，描述上限为 120 字；页面显示文件、版本号、插件描述三个必填项，提交后在「我的投稿」查看审核结果。商店列表将文件名去掉扩展名作为插件名称展示，作者由投稿账号显示，没有独立的作者输入框。更新时沿用同一文件名并提高版本号。

| 字段 | 本版填写值 |
| --- | --- |
| 插件文件 / 展示名称 | `U60Pro三页屏幕UI.js` / `U60Pro三页屏幕UI` |
| 版本号 | `1.0.0` |
| 插件描述 | `基于 33333s 原版与 scoltzero Remix 改版，致谢 Aawuxing 双卡页面。U60 Pro B31 三页 UI，突出网速、信号和流量；需先安装屏幕管理插件及 datad，支持备份回退。`（104 字） |
| 作者 | 由登录的投稿账号显示；不填写原作者为投稿人。原作者在描述、插件安装弹窗和 `NOTICE.md` 致谢。首次投稿的长文件名因「名字不是简介」未通过审核，因此展示名改为简短功能名称。 |

当前页面没有要求单独上传图标、截图或分类。正式投稿前，须先发布 GitHub Release 附件，使脚本中的公开下载地址可用。

## 本地复核

运行 `python3 release/build_release.py` 与 `python3 release/build_store_plugin.py` 重新打包。前者从已验证的 `candidate/` 复制资源，并在归档内重建正确的 `.devui-managed-files`；不会修改候选版或设备。然后用 `shasum -a 256 -c SHA256SUMS`、`tar -tzf ui.tar.gz` 和 `node --check U60Pro三页屏幕UI.js` 检查输出。
