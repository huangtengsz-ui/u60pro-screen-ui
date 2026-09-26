//<script>
// UFI-TOOLS community store entry: U60 Pro 三页屏幕 UI, v1.0.0.
// Based on 33333s/u60pro-devui and scoltzero/u60pro-devui-remix.
// Source and license: https://github.com/huangtengsz-ui/u60pro-screen-ui
(async () => {
  'use strict';
  const NAME = 'U60 Pro 三页屏幕 UI｜致谢 33333s、scoltzero';
  const BUTTON_TEXT = '三页屏幕 UI';
  const RELEASE_URL = @@RELEASE_URL_JSON@@;
  const INSTALL_SCRIPT = @@INSTALL_SCRIPT_JSON@@;
  const RESTORE_SCRIPT = @@RESTORE_SCRIPT_JSON@@;
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
    card.append(title, info, source, status, row);
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
