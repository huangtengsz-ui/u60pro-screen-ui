#!/bin/sh
# Bundled into the UFI-TOOLS store plugin. Variables are filled by build_store_plugin.py.
set -eu

BASE_URL='@@BASE_URL@@'
BIN_SHA='@@BIN_SHA@@'
UI_SHA='@@UI_SHA@@'
DEVUI_DIR=${DEVUI_DIR:-/data/plugins/u60pro-devui}
DATAD_BIN=${DATAD_BIN:-/data/plugins/zwrt-datad/zwrt-datad}
BIN="$DEVUI_DIR/u60pro-devui"
UI="$DEVUI_DIR/ui"
START_SH="$DEVUI_DIR/start-devui.sh"

fail() { echo "ERROR: $*" >&2; exit 1; }
hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d ' ' -f 1
    else shasum -a 256 "$1" | cut -d ' ' -f 1; fi
}
restart_ui() {
    [ "${SKIP_RESTART:-0}" = 1 ] && return 0
    killall u60pro-devui 2>/dev/null || true
    sh "$START_SH" legacy || return 1
    sleep 2
    pidof u60pro-devui >/dev/null 2>&1
}

[ "${SKIP_RESTART:-0}" = 1 ] || [ "$(uname -m)" = aarch64 ] || fail '仅支持 AArch64 U60 Pro'
[ "${SKIP_RESTART:-0}" = 1 ] || grep -q "sdx75/generic" /etc/openwrt_release 2>/dev/null || fail '当前固件平台未验证'
[ -x "$DATAD_BIN" ] || fail '请先用「屏幕管理插件」安装 datad 后端'
[ -x "$START_SH" ] || fail '请先用「屏幕管理插件」安装并启动屏幕服务'
[ -x "$BIN" ] || fail '未找到屏幕渲染程序'
[ -d "$UI" ] || fail '未找到屏幕页面目录'
[ ! -e "$DEVUI_DIR/ui.screen-new" ] && [ ! -e "$DEVUI_DIR/ui.screen-old" ] || fail '发现上次未完成的 UI 暂存目录，请先检查'
command -v curl >/dev/null 2>&1 || fail '设备缺少 curl'
command -v tar >/dev/null 2>&1 || fail '设备缺少 tar'

STAGE=$(mktemp -d "$DEVUI_DIR/.screen-ui-stage.XXXXXX") || fail '无法创建暂存目录'
trap 'rm -rf "$STAGE"' EXIT HUP INT TERM
curl -fLsS --retry 2 --connect-timeout 10 --max-time 150 "$BASE_URL/u60pro-devui-aarch64" -o "$STAGE/u60pro-devui" || fail '程序下载失败'
[ "$(hash_file "$STAGE/u60pro-devui")" = "$BIN_SHA" ] || fail '程序 SHA-256 校验失败'
curl -fLsS --retry 2 --connect-timeout 10 --max-time 150 "$BASE_URL/ui.tar.gz" -o "$STAGE/ui.tar.gz" || fail '页面下载失败'
[ "$(hash_file "$STAGE/ui.tar.gz")" = "$UI_SHA" ] || fail '页面 SHA-256 校验失败'

mkdir "$STAGE/new-ui" "$STAGE/merged" || fail '无法创建页面暂存目录'
tar -xzf "$STAGE/ui.tar.gz" -C "$STAGE/new-ui" || fail '页面解包失败'
MANIFEST="$STAGE/new-ui/.devui-managed-files"
[ -s "$MANIFEST" ] || fail '页面包缺少管理清单'
while IFS= read -r rel || [ -n "$rel" ]; do
    case "$rel" in ''|/*|*'..'*|*'\\'*) fail "非法页面路径: $rel" ;; esac
    [ -f "$STAGE/new-ui/$rel" ] || fail "页面包缺少: $rel"
done < "$MANIFEST"

cp -a "$UI/." "$STAGE/merged/" || fail '无法保留已有设置和自定义页面'
if [ -f "$UI/.devui-managed-files" ]; then
    while IFS= read -r rel || [ -n "$rel" ]; do
        case "$rel" in ''|/*|*'..'*|*'\\'*) continue ;; esac
        # The old upstream manifest marks this custom control as managed;
        # keep it because this release does not replace that device script.
        [ "$rel" = 'functions/cpuctl.sh' ] && continue
        rm -f "$STAGE/merged/$rel"
    done < "$UI/.devui-managed-files"
fi
rm -f "$STAGE/merged/05-charts.html" "$STAGE/merged/06-system.html" \
    "$STAGE/merged/functions/fmswitch.html" "$STAGE/merged/functions/fmsimpin.sh"
cp -a "$STAGE/new-ui/." "$STAGE/merged/" || fail '页面暂存失败'
chmod 755 "$STAGE/u60pro-devui"

STAMP=$(date +%Y%m%d-%H%M%S)
BACKUP="$DEVUI_DIR/.screen-ui-backup-$STAMP-$$"
mkdir "$BACKUP" || fail '无法创建回退备份'
cp -a "$BIN" "$BACKUP/u60pro-devui" || fail '程序备份失败'
cp -a "$UI" "$BACKUP/ui" || fail '页面备份失败'
printf '%s\n' "$BACKUP" > "$DEVUI_DIR/.screen-ui-last-backup"

mv "$STAGE/merged" "$DEVUI_DIR/ui.screen-new" || fail '页面暂存移动失败'
mv "$UI" "$DEVUI_DIR/ui.screen-old" || fail '现有页面暂存失败'
if ! mv "$DEVUI_DIR/ui.screen-new" "$UI"; then
    mv "$DEVUI_DIR/ui.screen-old" "$UI"
    fail '页面切换失败，已恢复旧页面'
fi
if ! mv -f "$STAGE/u60pro-devui" "$BIN"; then
    rm -rf "$UI"
    mv "$DEVUI_DIR/ui.screen-old" "$UI"
    fail '程序切换失败，已恢复旧页面'
fi

if ! restart_ui; then
    cp -a "$BACKUP/u60pro-devui" "$BIN.restore-new"
    mv -f "$BIN.restore-new" "$BIN"
    rm -rf "$UI"
    mv "$DEVUI_DIR/ui.screen-old" "$UI"
    restart_ui || true
    fail '新版启动失败，已恢复旧版'
fi
if [ "$(hash_file "$BIN")" != "$BIN_SHA" ]; then
    cp -a "$BACKUP/u60pro-devui" "$BIN.restore-new"
    mv -f "$BIN.restore-new" "$BIN"
    rm -rf "$UI"
    mv "$DEVUI_DIR/ui.screen-old" "$UI"
    restart_ui || true
    fail '安装后程序校验失败，已恢复旧版'
fi
rm -rf "$DEVUI_DIR/ui.screen-old"
echo "OK: 屏幕 UI 已安装；回退备份 $BACKUP"
