//<script>
// UFI-TOOLS community store entry: U60 Pro 三页屏幕 UI, v1.0.0.
// Based on 33333s/u60pro-devui and scoltzero/u60pro-devui-remix.
// Source and license: https://github.com/huangtengsz-ui/u60pro-screen-ui
(async () => {
  'use strict';
  const NAME = 'U60 Pro 三页屏幕 UI';
  const BUTTON_TEXT = '三页屏幕 UI';
  const RELEASE_URL = "https://github.com/huangtengsz-ui/u60pro-screen-ui/releases/download/v1.0.0";
  const INSTALL_SCRIPT = "#!/bin/sh\n# Bundled into the UFI-TOOLS store plugin. Variables are filled by build_store_plugin.py.\nset -eu\n\nBASE_URL='https://github.com/huangtengsz-ui/u60pro-screen-ui/releases/download/v1.0.0'\nBIN_FALLBACK_URL='https://raw.githubusercontent.com/huangtengsz-ui/u60pro-screen-ui/v1.0.0-static/candidate/runtime-bin/u60pro-devui'\nUI_FALLBACK_URL='https://raw.githubusercontent.com/huangtengsz-ui/u60pro-screen-ui/v1.0.0-static/release-assets/v1.0.0/ui.tar.gz'\nBIN_SHA='dd2d66b98b7a9130e8eb28303775833e4c790e974d5264f4941605a597ad3d6f'\nUI_SHA='5568deea7aba7118d621cb8ea031f66ff6562554ce8cfa916654bfaa577c60d2'\nDEVUI_DIR=${DEVUI_DIR:-/data/plugins/u60pro-devui}\nDATAD_BIN=${DATAD_BIN:-/data/plugins/zwrt-datad/zwrt-datad}\nBIN=\"$DEVUI_DIR/u60pro-devui\"\nUI=\"$DEVUI_DIR/ui\"\nSTART_SH=\"$DEVUI_DIR/start-devui.sh\"\n\nfail() { echo \"ERROR: $*\" >&2; exit 1; }\nhash_file() {\n    if command -v sha256sum >/dev/null 2>&1; then sha256sum \"$1\" | cut -d ' ' -f 1\n    else shasum -a 256 \"$1\" | cut -d ' ' -f 1; fi\n}\ndownload_asset() {\n    if curl -fLsS --retry 1 --connect-timeout 8 --max-time 150 \"$1\" -o \"$3\"; then return 0; fi\n    echo '主下载地址不可用，改用 GitHub 源码镜像' >&2\n    curl -fLsS --retry 2 --connect-timeout 10 --max-time 150 \"$2\" -o \"$3\"\n}\nrestart_ui() {\n    [ \"${SKIP_RESTART:-0}\" = 1 ] && return 0\n    killall u60pro-devui 2>/dev/null || true\n    sh \"$START_SH\" legacy || return 1\n    sleep 2\n    pidof u60pro-devui >/dev/null 2>&1\n}\n\n[ \"${SKIP_RESTART:-0}\" = 1 ] || [ \"$(uname -m)\" = aarch64 ] || fail '仅支持 AArch64 U60 Pro'\n[ \"${SKIP_RESTART:-0}\" = 1 ] || grep -q \"sdx75/generic\" /etc/openwrt_release 2>/dev/null || fail '当前固件平台未验证'\n[ -x \"$DATAD_BIN\" ] || fail '请先用「屏幕管理插件」安装 datad 后端'\n[ -x \"$START_SH\" ] || fail '请先用「屏幕管理插件」安装并启动屏幕服务'\n[ -x \"$BIN\" ] || fail '未找到屏幕渲染程序'\n[ -d \"$UI\" ] || fail '未找到屏幕页面目录'\n[ ! -e \"$DEVUI_DIR/ui.screen-new\" ] && [ ! -e \"$DEVUI_DIR/ui.screen-old\" ] || fail '发现上次未完成的 UI 暂存目录，请先检查'\ncommand -v curl >/dev/null 2>&1 || fail '设备缺少 curl'\ncommand -v tar >/dev/null 2>&1 || fail '设备缺少 tar'\n\nSTAGE=$(mktemp -d \"$DEVUI_DIR/.screen-ui-stage.XXXXXX\") || fail '无法创建暂存目录'\ntrap 'rm -rf \"$STAGE\"' EXIT HUP INT TERM\ndownload_asset \"$BASE_URL/u60pro-devui-aarch64\" \"$BIN_FALLBACK_URL\" \"$STAGE/u60pro-devui\" || fail '程序下载失败'\n[ \"$(hash_file \"$STAGE/u60pro-devui\")\" = \"$BIN_SHA\" ] || fail '程序 SHA-256 校验失败'\ndownload_asset \"$BASE_URL/ui.tar.gz\" \"$UI_FALLBACK_URL\" \"$STAGE/ui.tar.gz\" || fail '页面下载失败'\n[ \"$(hash_file \"$STAGE/ui.tar.gz\")\" = \"$UI_SHA\" ] || fail '页面 SHA-256 校验失败'\n\nmkdir \"$STAGE/new-ui\" \"$STAGE/merged\" || fail '无法创建页面暂存目录'\ntar -xzf \"$STAGE/ui.tar.gz\" -C \"$STAGE/new-ui\" || fail '页面解包失败'\nMANIFEST=\"$STAGE/new-ui/.devui-managed-files\"\n[ -s \"$MANIFEST\" ] || fail '页面包缺少管理清单'\nwhile IFS= read -r rel || [ -n \"$rel\" ]; do\n    case \"$rel\" in ''|/*|*'..'*|*'\\\\'*) fail \"非法页面路径: $rel\" ;; esac\n    [ -f \"$STAGE/new-ui/$rel\" ] || fail \"页面包缺少: $rel\"\ndone < \"$MANIFEST\"\n\ncp -a \"$UI/.\" \"$STAGE/merged/\" || fail '无法保留已有设置和自定义页面'\nif [ -f \"$UI/.devui-managed-files\" ]; then\n    while IFS= read -r rel || [ -n \"$rel\" ]; do\n        case \"$rel\" in ''|/*|*'..'*|*'\\\\'*) continue ;; esac\n        # The old upstream manifest marks this custom control as managed;\n        # keep it because this release does not replace that device script.\n        [ \"$rel\" = 'functions/cpuctl.sh' ] && continue\n        rm -f \"$STAGE/merged/$rel\"\n    done < \"$UI/.devui-managed-files\"\nfi\nrm -f \"$STAGE/merged/05-charts.html\" \"$STAGE/merged/06-system.html\" \\\n    \"$STAGE/merged/functions/fmswitch.html\" \"$STAGE/merged/functions/fmsimpin.sh\"\ncp -a \"$STAGE/new-ui/.\" \"$STAGE/merged/\" || fail '页面暂存失败'\nchmod 755 \"$STAGE/u60pro-devui\"\n\nSTAMP=$(date +%Y%m%d-%H%M%S)\nBACKUP=\"$DEVUI_DIR/.screen-ui-backup-$STAMP-$$\"\nmkdir \"$BACKUP\" || fail '无法创建回退备份'\ncp -a \"$BIN\" \"$BACKUP/u60pro-devui\" || fail '程序备份失败'\ncp -a \"$UI\" \"$BACKUP/ui\" || fail '页面备份失败'\nprintf '%s\\n' \"$BACKUP\" > \"$DEVUI_DIR/.screen-ui-last-backup\"\n\nmv \"$STAGE/merged\" \"$DEVUI_DIR/ui.screen-new\" || fail '页面暂存移动失败'\nmv \"$UI\" \"$DEVUI_DIR/ui.screen-old\" || fail '现有页面暂存失败'\nif ! mv \"$DEVUI_DIR/ui.screen-new\" \"$UI\"; then\n    mv \"$DEVUI_DIR/ui.screen-old\" \"$UI\"\n    fail '页面切换失败，已恢复旧页面'\nfi\nif ! mv -f \"$STAGE/u60pro-devui\" \"$BIN\"; then\n    rm -rf \"$UI\"\n    mv \"$DEVUI_DIR/ui.screen-old\" \"$UI\"\n    fail '程序切换失败，已恢复旧页面'\nfi\n\nif ! restart_ui; then\n    cp -a \"$BACKUP/u60pro-devui\" \"$BIN.restore-new\"\n    mv -f \"$BIN.restore-new\" \"$BIN\"\n    rm -rf \"$UI\"\n    mv \"$DEVUI_DIR/ui.screen-old\" \"$UI\"\n    restart_ui || true\n    fail '新版启动失败，已恢复旧版'\nfi\nif [ \"$(hash_file \"$BIN\")\" != \"$BIN_SHA\" ]; then\n    cp -a \"$BACKUP/u60pro-devui\" \"$BIN.restore-new\"\n    mv -f \"$BIN.restore-new\" \"$BIN\"\n    rm -rf \"$UI\"\n    mv \"$DEVUI_DIR/ui.screen-old\" \"$UI\"\n    restart_ui || true\n    fail '安装后程序校验失败，已恢复旧版'\nfi\nrm -rf \"$DEVUI_DIR/ui.screen-old\"\necho \"OK: 屏幕 UI 已安装；回退备份 $BACKUP\"\n";
  const RESTORE_SCRIPT = "#!/bin/sh\nset -eu\nDEVUI_DIR=${DEVUI_DIR:-/data/plugins/u60pro-devui}\nBIN=\"$DEVUI_DIR/u60pro-devui\"\nUI=\"$DEVUI_DIR/ui\"\nSTART_SH=\"$DEVUI_DIR/start-devui.sh\"\n[ -f \"$DEVUI_DIR/.screen-ui-last-backup\" ] || { echo 'ERROR: 没有可回退的备份'; exit 1; }\nBACKUP=$(cat \"$DEVUI_DIR/.screen-ui-last-backup\")\ncase \"$BACKUP\" in \"$DEVUI_DIR\"/.screen-ui-backup-*) ;; *) echo 'ERROR: 备份路径异常'; exit 1 ;; esac\n[ -f \"$BACKUP/u60pro-devui\" ] && [ -d \"$BACKUP/ui\" ] || { echo 'ERROR: 备份不完整'; exit 1; }\ncp -a \"$BACKUP/u60pro-devui\" \"$BIN.restore-new\"\ncp -a \"$BACKUP/ui\" \"$UI.restore-new\"\nmv -f \"$BIN.restore-new\" \"$BIN\"\nmv \"$UI\" \"$UI.restore-current\"\nif ! mv \"$UI.restore-new\" \"$UI\"; then\n    mv \"$UI.restore-current\" \"$UI\"\n    echo 'ERROR: 页面回退失败'; exit 1\nfi\nif [ \"${SKIP_RESTART:-0}\" != 1 ]; then\n    killall u60pro-devui 2>/dev/null || true\n    sh \"$START_SH\" legacy\n    sleep 2\n    pidof u60pro-devui >/dev/null 2>&1 || { echo 'ERROR: 旧版启动失败'; exit 1; }\nfi\nrm -rf \"$UI.restore-current\"\necho \"OK: 已恢复 $BACKUP\"\n";
  const buttonId = 'u60-screen-ui-open';
  const modalId = 'u60-screen-ui-modal';

  async function root(command, timeout = 30000) {
    const base = typeof KANO_baseURL !== 'undefined' && KANO_baseURL ? KANO_baseURL : '/api';
    const headers = {'Content-Type': 'application/json'};
    if (typeof common_headers !== 'undefined' && common_headers) Object.assign(headers, common_headers);
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeout + 5000);
    try {
      const response = await fetch(`${base}/run_shell`, {
        method: 'POST', headers, signal: controller.signal,
        body: JSON.stringify({cmd: command, timeout})
      });
      if (!response.ok) return {success: false, content: response.status === 401 ? 'HTTP 401：请先登录 UFI-TOOLS 身份口令' : `HTTP ${response.status}`};
      const data = await response.json();
      return {success: !!data.success, content: data.content || ''};
    } finally {
      clearTimeout(timer);
    }
  }

  function shellCommand(script, action) {
    const marker = 'U60_SCREEN_UI_SCRIPT_END';
    return `f=/tmp/u60-screen-ui-${action}-$$.sh\ncat > "$f" <<'${marker}'\n${script}\n${marker}\nsh "$f"\nrc=$?\nrm -f "$f"\nexit "$rc"`;
  }

  function node(tag, label, styles) {
    const element = document.createElement(tag);
    if (label) element.textContent = label;
    if (styles) element.style.cssText = styles;
    return element;
  }

  function openModal() {
    if (document.getElementById(modalId)) return;
    const overlay = node('div', '', 'position:fixed;inset:0;z-index:100000;background:rgba(0,0,0,.64);display:flex;align-items:center;justify-content:center;padding:16px;box-sizing:border-box');
    overlay.id = modalId;
    const card = node('div', '', 'box-sizing:border-box;width:min(440px,100%);max-height:90vh;overflow:auto;background:#17232c;color:#f3f8fa;border:1px solid #487284;border-radius:16px;padding:20px;font:14px/1.55 system-ui,sans-serif;box-shadow:0 16px 44px #0007');
    const title = node('h2', NAME, 'margin:0 0 10px;font-size:19px');
    const info = node('p', '适用于已安装「屏幕管理插件」及 datad 的 U60 Pro B31。安装会更新屏幕程序和页面，保留原有配置与自定义文件。之后如从原屏幕管理插件的旧更新源更新，可能覆盖此版本。', 'margin:0 0 10px;color:#c8d6dc');
    const credits = node('p', '基于 33333s 原版与 scoltzero Remix 改版；双卡页面致谢 Aawuxing。原项目许可与完整来源见 GitHub NOTICE.md。', 'margin:0 0 12px;color:#9ec8d8;font-size:12px');
    const source = node('p', `资源：${RELEASE_URL}`, 'margin:0 0 12px;color:#9ec8d8;overflow-wrap:anywhere;font-size:12px');
    const status = node('pre', '尚未检查设备', 'white-space:pre-wrap;word-break:break-word;background:#0b151a;border:1px solid #365261;border-radius:9px;padding:10px;min-height:48px;max-height:180px;overflow:auto');
    const row = node('div', '', 'display:flex;flex-wrap:wrap;gap:8px');
    const check = node('button', '检查设备');
    const install = node('button', '安装此版本');
    const restore = node('button', '恢复上个版本');
    const close = node('button', '关闭');
    for (const b of [check, install, restore, close]) b.style.cssText = 'padding:9px 12px;border:1px solid #59899a;border-radius:9px;background:#254456;color:#fff;cursor:pointer;font:inherit';
    install.style.background = '#166b82';
    restore.style.background = '#4a5369';
    row.append(check, install, restore, close);
    card.append(title, info, credits, source, status, row);
    overlay.append(card);
    document.body.append(overlay);
    overlay.addEventListener('click', e => { if (e.target === overlay) overlay.remove(); });
    close.onclick = () => overlay.remove();

    async function busy(label, action) {
      for (const b of [check, install, restore, close]) b.disabled = true;
      status.textContent = `${label}中，请勿关闭页面…`;
      try {
        const result = await action();
        status.textContent = `${result.success ? '完成' : '失败'}：\n${result.content || '(无返回内容)'}`;
      } catch (error) {
        status.textContent = `失败：${error?.message || error}`;
      } finally {
        for (const b of [check, install, restore, close]) b.disabled = false;
      }
    }
    check.onclick = () => busy('检查', () => root('uname -m; test -x /data/plugins/zwrt-datad/zwrt-datad && echo datad=ready; test -x /data/plugins/u60pro-devui/start-devui.sh && echo manager=ready; pidof u60pro-devui >/dev/null && echo screen=running; test -f /data/plugins/u60pro-devui/.screen-ui-last-backup && echo backup=available; echo OK', 10000));
    install.onclick = () => {
      if (!window.confirm('将安装此屏幕版本并短暂重启屏幕程序。继续？')) return;
      busy('安装', () => root(shellCommand(INSTALL_SCRIPT, 'install'), 360000));
    };
    restore.onclick = () => {
      if (!window.confirm('将恢复安装前的屏幕程序和页面。继续？')) return;
      busy('恢复', () => root(shellCommand(RESTORE_SCRIPT, 'restore'), 30000));
    };
  }

  for (let attempt = 0; attempt < 100; attempt++) {
    if (document.getElementById(buttonId)) return;
    const slot = document.querySelector('.functions-container .actions-buttons') || document.querySelector('.collapse_box');
    if (slot) {
      const button = node('button', BUTTON_TEXT);
      button.id = buttonId;
      button.onclick = openModal;
      slot.append(button);
      return;
    }
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  console.error(`${NAME}: UFI-TOOLS 插件按钮容器未找到`);
})();
//</script >
