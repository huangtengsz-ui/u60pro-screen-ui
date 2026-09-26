#!/bin/sh
set -eu
DEVUI_DIR=${DEVUI_DIR:-/data/plugins/u60pro-devui}
BIN="$DEVUI_DIR/u60pro-devui"
UI="$DEVUI_DIR/ui"
START_SH="$DEVUI_DIR/start-devui.sh"
[ -f "$DEVUI_DIR/.screen-ui-last-backup" ] || { echo 'ERROR: 没有可回退的备份'; exit 1; }
BACKUP=$(cat "$DEVUI_DIR/.screen-ui-last-backup")
case "$BACKUP" in "$DEVUI_DIR"/.screen-ui-backup-*) ;; *) echo 'ERROR: 备份路径异常'; exit 1 ;; esac
[ -f "$BACKUP/u60pro-devui" ] && [ -d "$BACKUP/ui" ] || { echo 'ERROR: 备份不完整'; exit 1; }
cp -a "$BACKUP/u60pro-devui" "$BIN.restore-new"
cp -a "$BACKUP/ui" "$UI.restore-new"
mv -f "$BIN.restore-new" "$BIN"
mv "$UI" "$UI.restore-current"
if ! mv "$UI.restore-new" "$UI"; then
    mv "$UI.restore-current" "$UI"
    echo 'ERROR: 页面回退失败'; exit 1
fi
if [ "${SKIP_RESTART:-0}" != 1 ]; then
    killall u60pro-devui 2>/dev/null || true
    sh "$START_SH" legacy
    sleep 2
    pidof u60pro-devui >/dev/null 2>&1 || { echo 'ERROR: 旧版启动失败'; exit 1; }
fi
rm -rf "$UI.restore-current"
echo "OK: 已恢复 $BACKUP"
