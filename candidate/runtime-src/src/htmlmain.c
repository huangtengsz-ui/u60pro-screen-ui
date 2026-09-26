/*
 * htmlmain.c - HTML UI shell. The program is fixed; the UI lives in
 * /data/plugins/u60pro-devui/ui
 * as plain HTML/CSS. Each *.html (except menu.html) is a swipeable page;
 * {{TOKENS}} are filled from the zwrt-datad snapshot. Touch swipes pages and
 * taps fire anchor actions (href="act:..."). Power key: short = backlight,
 * long = the menu.html overlay.
 *
 * SPDX-License-Identifier: MIT
 */
#include "drm_disp.h"
#include "data.h"
#include "json.h"
#include "touch_input.h"
#include "key_input.h"
#include "backlight.h"
#include "devui_ext.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

extern void        html_view_init(uint16_t *fb, int w, int h, int pitch_px, int rotate, const char *font_path);
extern void        html_view_set_uidir(const char *dir);
extern int         html_view_render_html(const char *html);
extern int         html_view_render_to(uint16_t *buf, const char *html);
extern int         html_view_render_to_scroll(uint16_t *buf, const char *html, int scroll);
extern void        html_view_target_begin(uint16_t *buf, int scroll);
extern void        html_view_target_begin_size(uint16_t *buf, int h, int scroll);
extern void        html_view_target_end(void);
extern const char *html_view_click(float x, float y);
extern int         html_view_rect(const char *sel, int *x, int *y, int *w, int *h);
extern void        html_view_polyline(int x, int y, int w, int h, const int *vals, int n,
                                      int vmin, int vmax, int r, int g, int b, int thick, int fill_a);
extern void        html_view_timed_polyline(int x, int y, int w, int h,
                                            const uint32_t *times, const int *vals, int n,
                                            uint32_t now_sec, int window_sec,
                                            int vmin, int vmax, int r, int g, int b,
                                            int thick, int fill_a);
extern void        html_view_set_scroll(int y);
extern void        html_view_set_clip_top(int y);
extern void        html_view_fill_rect(int x, int y, int w, int h, int r, int g, int b, int a);
extern void        html_view_fill_poly(const int *xs, const int *ys, int n, int r, int g, int b, int a);
extern void        html_view_fill_round_rect(int x, int y, int w, int h, int rad, int r, int g, int b, int a);
extern void        html_view_fill_bevel_circle(int cx, int cy, int rad, int r, int g, int b,
                                               int light_theme, int pressed);
extern void        html_view_draw_power_glyph(int cx, int cy, int rad, int kind,
                                              int r, int g, int b, int a);
extern int         html_view_text_width_px(const char *text, int size);
extern void        html_view_text_bounds_px(const char *text, int size, int *x0, int *y0, int *x1, int *y1);
extern void        html_view_draw_text_px(int x, int y, const char *text, int size, int bold,
                                          int r, int g, int b, int a);
extern void        html_view_draw_text_contrast_px(int x, int y, const char *text, int size, int bold,
                                                   int dr, int dg, int db, int lr, int lg, int lb, int a);
extern int         html_view_render_tall(uint16_t *buf, const char *html, int bufh);
extern int         html_view_draw_current_tall(uint16_t *buf, int bufh);
extern int         html_view_render_overlay(const char *html);
extern void        html_view_suspend(void);
static void        maybe_dump_fb(drm_disp_t *d);

#ifndef UI_DIR
#define UI_DIR "/data/plugins/u60pro-devui/ui"
#endif
#define FUNCTIONS_DIR UI_DIR "/functions"
#define UI_FONT "/usr/ui/fonts/ZTEZhengYuan.ttf"
#define DATAD_HTTP_ADDR "127.0.0.1"
#define DATAD_HTTP_PORT 9460
#define DATAD_HTTP_TIMEOUT_MS 300

static volatile sig_atomic_t g_run = 1;
static volatile sig_atomic_t g_plugin_diag_requested;
static volatile int g_ui_awake = 1;
static void on_sig(int s) { (void)s; g_run = 0; }
static void on_plugin_diag_sig(int s) { (void)s; g_plugin_diag_requested = 1; }

static uint32_t millis(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint32_t monotonic_seconds(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec;
}

static int g_display_offset_minutes = INT_MIN;
static int g_system_clock_config_read;
static int g_system_clock_offset_minutes = INT_MIN;

static int parse_system_clock_offset(const char *value, int *out)
{
    char *end;
    double hours;
    int minutes;

    if (!value || !*value || !out) return 0;
    hours = strtod(value, &end);
    while (end && *end == ' ') end++;
    if (end == value || (end && *end) || hours < -12.0 || hours > 14.0) return 0;
    minutes = (int)(hours * 60.0 + (hours < 0 ? -0.5 : 0.5));
    *out = minutes;
    return 1;
}

/* The vendor SNTP service can store local wall time in the Unix clock.  The
 * configured time_from_utc is the clock's basis, not the UI's selected zone. */
static int configured_system_clock_offset_minutes(void)
{
    FILE *fp;
    char line[256], value[64];
    int fallback = INT_MIN;

    if (g_system_clock_config_read) return g_system_clock_offset_minutes;
    g_system_clock_config_read = 1;
    fp = fopen("/etc/config/zwrt_zte_sntp", "r");
    if (!fp) return INT_MIN;
    while (fgets(line, sizeof line, fp)) {
        if (sscanf(line, " option time_from_utc '%63[^']'", value) == 1 &&
            parse_system_clock_offset(value, &g_system_clock_offset_minutes))
            break;
        if (fallback == INT_MIN &&
            sscanf(line, " option timezone '%63[^']'", value) == 1)
            (void)parse_system_clock_offset(value, &fallback);
    }
    fclose(fp);
    if (g_system_clock_offset_minutes == INT_MIN)
        g_system_clock_offset_minutes = fallback;
    return g_system_clock_offset_minutes;
}

static int clock_basis_minutes(const devui_data_t *data)
{
    /* The UFI-TOOLS time repair sets the Unix clock to real UTC and installs
     * the China timezone.  The old vendor time_from_utc option can remain 8,
     * so a live +0800 system timezone takes precedence over that old value. */
    time_t now = time(NULL);
    struct tm local;
    char zone[8] = {0};
    int configured = configured_system_clock_offset_minutes();
    if (localtime_r(&now, &local) &&
        strftime(zone, sizeof zone, "%z", &local) &&
        strcmp(zone, "+0800") == 0)
        return 0;
    if (configured != INT_MIN) return configured;
    if (data && data->timezone_clock_basis_available)
        return data->timezone_system_offset_minutes;
    return 0;
}

static int display_clock_adjust_minutes(void)
{
    if (g_display_offset_minutes == INT_MIN)
        g_display_offset_minutes = 480 - clock_basis_minutes(NULL);
    return g_display_offset_minutes;
}

static void devui_localtime(time_t when, struct tm *out)
{
    time_t adjusted = when + (time_t)display_clock_adjust_minutes() * 60;
    gmtime_r(&adjusted, out);
}

static void devui_time_stamp(char *out, size_t outlen, time_t when)
{
    struct tm tmv;
    devui_localtime(when, &tmv);
    strftime(out, outlen, "%F %T", &tmv);
}

static void devui_posix_tz(char *out, size_t outlen)
{
    int posix_minutes = -display_clock_adjust_minutes();
    char sign = posix_minutes < 0 ? '-' : '+';
    int abs_minutes = posix_minutes < 0 ? -posix_minutes : posix_minutes;
    snprintf(out, outlen, "UTC%c%d:%02d", sign, abs_minutes / 60, abs_minutes % 60);
}

/* Optional service pages under ui/functions/.  These fixed paths intentionally
 * avoid a generic "run shell from HTML" action: custom pages remain data-only,
 * while known local services get a small, auditable control surface. */
#define TAILSCALE_ACTION_LOG "/tmp/devui-tailscale-action.log"
#define MIHOMO_ACTION_LOG "/tmp/devui-mihomo-action.log"
#define WIREGUARD_ACTION_LOG "/tmp/devui-wireguard-action.log"
#define OPERATOR_ACTION_LOG "/tmp/devui-operator-action.log"
#define FM_ACTION_LOG "/tmp/devui-fmswitch-action.log"
#define FM_CTL_BUNDLED UI_DIR "/functions/fmsimpin.sh"
#define FM_SWITCH_RC "/tmp/devui-fmswitch.rc"
#define FM_DISPLAY_FILE "/data/plugins/u60pro-devui/flymodem-display.conf"
#define FM_DISPLAY_TMP  "/data/plugins/u60pro-devui/flymodem-display.conf.tmp"
#define CPU_CTL_BUNDLED UI_DIR "/functions/cpuctl.sh"
#define CPU_CTL_LEGACY  UI_DIR "/../cpuctl.sh"
#define CPU_CTL_OLD     "/data/ufi-tools/u60pro-devui/cpuctl.sh"
#define CPU_ACTION_LOG "/tmp/devui-cpu-action.log"
#define SIM_ACTION_LOG "/tmp/devui-sim-action.log"
#define PLUGIN_DETECT_LOG "/data/plugins/u60pro-devui/plugin-detect.log"

struct plugin_candidate {
    const char *dir;
    const char *ctl;
    const char *bin;
};

static const struct plugin_candidate g_ts_candidates[] = {
    { "/data/plugins/tailscale", "/data/plugins/tailscale/tsctl.sh", "/data/plugins/tailscale/bin/tailscale" },
    { "/data/ufi-tools/tailscale", "/data/ufi-tools/tailscale/tsctl.sh", "/data/ufi-tools/tailscale/bin/tailscale" },
    { "/data/kano_plugins/tailscale", "/data/kano_plugins/tailscale/tsctl.sh", "/data/kano_plugins/tailscale/bin/tailscale" },
};
static const struct plugin_candidate g_mh_candidates[] = {
    { "/data/plugins/mihomo", "/data/plugins/mihomo/mm.sh", "/data/plugins/mihomo/mihomo" },
    { "/data/ufi-tools/mihomo", "/data/ufi-tools/mihomo/mm.sh", "/data/ufi-tools/mihomo/mihomo" },
    { "/data/kano_plugins/mihomo", "/data/kano_plugins/mihomo/mm.sh", "/data/kano_plugins/mihomo/mihomo" },
};
static const struct plugin_candidate g_wg_candidates[] = {
    { "/data/plugins/wireguard", "/data/plugins/wireguard/wgctl.sh", "/data/plugins/wireguard/bin/wg" },
    { "/data/ufi-tools/wireguard", "/data/ufi-tools/wireguard/wgctl.sh", "/data/ufi-tools/wireguard/bin/wg" },
    { "/data/kano_plugins/wireguard", "/data/kano_plugins/wireguard/wgctl.sh", "/data/kano_plugins/wireguard/bin/wg" },
};
static const struct plugin_candidate g_operator_candidates[] = {
    { "/data/plugins/operator-lock", "/data/plugins/operator-lock/operatorctl.sh", NULL },
    { "/data/ufi-tools/operator-lock", "/data/ufi-tools/operator-lock/operatorctl.sh", NULL },
    { "/data/kano_plugins/operator-lock", "/data/kano_plugins/operator-lock/operatorctl.sh", NULL },
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Shell adapters are valid APIs even when an importer preserved them as 0644;
 * these two plugins are invoked through `sh`, so readability is sufficient. */
static const struct plugin_candidate *plugin_script_select(
    const struct plugin_candidate *items, size_t count, int require_bin)
{
    for (size_t i = 0; i < count; i++) {
        if (access(items[i].ctl, R_OK) != 0) continue;
        if (require_bin && items[i].bin && access(items[i].bin, X_OK) != 0) continue;
        return &items[i];
    }
    return NULL;
}

static const struct plugin_candidate *plugin_complete_select(
    const struct plugin_candidate *items, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (access(items[i].ctl, R_OK) == 0 && items[i].bin &&
            access(items[i].bin, X_OK) == 0)
            return &items[i];
    return NULL;
}

static const struct plugin_candidate *operator_complete_select(void)
{
    char config[320];
    for (size_t i = 0; i < ARRAY_LEN(g_operator_candidates); i++) {
        snprintf(config, sizeof config, "%s/config.json", g_operator_candidates[i].dir);
        if (access(g_operator_candidates[i].ctl, R_OK) == 0 &&
            access(config, R_OK) == 0)
            return &g_operator_candidates[i];
    }
    return NULL;
}

#define WG_MAX_PEERS 16
#define OP_MAX_CANDIDATES 24

struct wg_peer_state {
    char public_key[48];
    char endpoint[96];
    char allowed_ips[192];
    long long latest_handshake;
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
};

struct operator_candidate_state {
    int status;
    char plmn[8];
    char name[80];
};

static uint32_t g_plugin_status_at;
static int g_ts_installed, g_ts_running, g_ts_connected, g_ts_boot;
static int g_mh_installed, g_mh_running, g_mh_tun, g_mh_rules;
static int g_cpu_installed;
static int g_wg_installed, g_wg_running, g_wg_deps, g_wg_boot;
static int g_wg_peer_n, g_wg_peer_total, g_wg_peer_active;
static int g_op_installed, g_op_registered, g_op_job_running, g_op_at_busy;
static int g_op_candidate_n, g_op_candidate_total;
static int g_fm_available, g_fm_switching, g_fm_enabled;
static char g_ts_pid[16] = "-", g_ts_ip[48] = "-", g_ts_version[32] = "-";
static char g_ts_host[64] = "-", g_ts_routes[160] = "-";
static char g_mh_pid[16] = "-", g_mh_version[64] = "-", g_mh_mode[24] = "-";
static char g_mh_port[16] = "-", g_mh_ipset[24] = "-";
static char g_cpu_mode[24] = "unknown", g_cpu_gov[24] = "-";
static char g_cpu_cur[24] = "-", g_cpu_min[24] = "-", g_cpu_max[24] = "-";
static char g_wg_iface[32] = "wg-ufi0", g_wg_address[96] = "-", g_wg_port[16] = "-";
static char g_wg_mode[24] = "stopped", g_wg_uptime[32] = "-";
static struct wg_peer_state g_wg_peers[WG_MAX_PEERS];
static char g_op_sim[32] = "-", g_op_operator[32] = "-", g_op_rat[32] = "-";
static char g_op_mode[24] = "-", g_op_job_status[24] = "idle", g_op_job_message[160] = "-";
static char g_op_rat_pref[16] = "auto", g_op_failure_policy[24] = "stay_offline";
static char g_fm_carrier[8] = "-", g_fm_provider[32] = "-", g_fm_nettype[24] = "-";
static char g_fm_band[32] = "-", g_fm_signal[16] = "-", g_fm_plmn[16] = "-";
static char g_fm_detect_source[32] = "none", g_fm_detect_reason[128] = "not checked";
static char g_fm_confirm_pin[8], g_fm_switch_pin[8];
static uint32_t g_fm_refresh_at, g_fm_confirm_until, g_fm_switch_until;
static char g_toast[48];
static uint32_t g_toast_until;
static char g_op_selected[8];
static uint32_t g_op_confirm_until;
static struct operator_candidate_state g_op_scan[OP_MAX_CANDIDATES];

static const char *cpu_ctl_path(void)
{
    if (access(CPU_CTL_BUNDLED, R_OK) == 0) return CPU_CTL_BUNDLED;
    if (access(CPU_CTL_LEGACY, R_OK) == 0) return CPU_CTL_LEGACY;
    return CPU_CTL_OLD;
}

static int cpu_control_available(void)
{
    return access(cpu_ctl_path(), R_OK) == 0 &&
           access("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", R_OK | W_OK) == 0;
}

#define TEMPLATE_CACHE_CAP 8
#define PAGE_HTML_CACHE_CAP 1048576

struct template_cache_entry {
    char path[320];
    long long mtime_ns;
    char *content;
    int used;
};

static struct template_cache_entry g_tmpl_cache[TEMPLATE_CACHE_CAP];
static int g_tmpl_next;

/* ---- pages / theme ---- */
static char g_pages[24][288];
static int  g_npages, g_cur;
static char g_subpage[64];       /* non-empty = a second-level page under ui/subpages */
static char g_subpage_path[300];
#define PINNED_HEADER_TOP 26
#define PINNED_HEADER_H 52
struct nav_entry {
    char name[64], path[300];
    int scroll, header_valid;
    uint16_t header[320 * PINNED_HEADER_H];
};
#define NAV_DEPTH 6
static struct nav_entry g_nav_stack[NAV_DEPTH];
static int g_nav_depth;
static int  g_quick_open;
static int  g_quick_previous_scroll;
static char g_quick_previous_subpage[64];
static char g_quick_previous_path[300];
static uint16_t g_pinned_header[320 * PINNED_HEADER_H];
static uint16_t g_quick_previous_header[320 * PINNED_HEADER_H];
static int g_pinned_header_valid;
static int g_quick_previous_header_valid;
static int  g_theme;      /* 0 = dark, 1 = light */
static int  g_show_key;   /* reveal WiFi password (default hidden) */
static int  g_show_cellid; /* reveal NR Cell ID (default hidden) */
static int  g_show_imei;   /* reveal IMEI (default hidden) */
static int  g_speed_bits = 1; /* 1 = Mbps (bit rate), 0 = MB/s (byte rate) */
static int  g_show_batpct = 1; /* show percent text inside status battery */
static int  g_charging;   /* set from snapshot; drives charge animation cadence */
static int  g_stat_bat, g_stat_sig, g_stat_lowbat;
static char g_stat_time[8], g_stat_speed[40], g_stat_gen[8];
static unsigned g_phase;  /* animation tick (battery charge sweep) */
static int  g_charge_boot; /* power-off charging boot: full-screen charging UI */
static int  g_scroll;     /* current page vertical scroll offset */
static int  g_page_h;     /* last rendered page content height */
static int  g_autooff_ms; /* auto screen-off timeout, 0 = never */
static int  g_refresh_ms = 1000; /* state refresh interval, 0 = paused */
static int  g_chart_cpu_sec = 48;
static int  g_chart_mem_sec = 48;
static int  g_chart_net_sec = 48;
static int  g_chart_batt_sec = 48;
static int  g_tz_draft_hour = 8;
static int  g_tz_draft_minute;
static int  g_tz_draft_dst;
static int  g_tz_draft_dirty;
static int  g_plan_edit_slot = 1;
static int  g_plan_draft_enabled;
static unsigned long long g_plan_draft_gib;
static int  g_plan_draft_reset_day = 1;
static int  g_plan_draft_dirty;
static char g_plan_draft_sim_id[24];
static int  g_sig_read;   /* ML1/raw signaling read switch */
static int  g_sig_parse;  /* decoded LTE/NR signaling parse switch + page visibility */
static int  g_neighbor_open; /* expand neighbor-cell list on the first signal page */
static int  g_saved_bright = -1; /* persisted backlight level, -1 = none yet */
enum { ST_STATE_MISSING = 0, ST_STATE_IDLE, ST_STATE_RUNNING, ST_STATE_DONE, ST_STATE_FAIL };
#define SPEEDTEST_BIN "/data/plugins/better-speedtest/better-speedtest"
#define SPEEDTEST_LOG "/tmp/better-speedtest.log"
#define SPEEDTEST_LOOP_FLAG "/tmp/better-speedtest.loop"
#define SPEEDTEST_LOOP_PID "/tmp/better-speedtest.loop.pid"
static char g_st_src[16] = "auto";
static char g_st_dir[8] = "both";
static int  g_st_dur = 15;
static int  g_st_state = ST_STATE_MISSING;
static int  g_st_installed = -1;
static int  g_st_have_result;
static uint32_t g_st_poll_at;
static char g_st_phase[24], g_st_msg[160], g_st_err[160];
static char g_st_node[96], g_st_source[32], g_st_carrier[32], g_st_city[32], g_st_ulmsg[96];
static double g_st_cur_mbps, g_st_cur_peak;
static double g_st_dl_avg, g_st_dl_peak, g_st_ul_avg, g_st_ul_peak, g_st_ping, g_st_jitter;
#define ST_HIST 48
static int g_st_dl_hist[ST_HIST], g_st_ul_hist[ST_HIST], g_st_dl_n, g_st_ul_n;
static int g_last_clock_min = -1;       /* HH:MM changes only once per minute */
static char *g_render_html_cache;
static size_t g_render_html_cache_cap;
static size_t g_render_html_cache_len;
static char g_render_html_cache_path[320];
static int g_render_html_cache_scroll;
static int g_render_html_cache_modal;
static long g_render_html_cache_sms_open;
static int g_render_html_cache_lock;
static long long g_render_html_cache_css_mtime = -1;
static long long g_pages_dir_mtime = -1;
static uint32_t g_pages_scan_at;
static int normalize_refresh_ms(int ms);
static int normalize_chart_sec(int sec);

static void invalidate_render_html_cache(void);
static int sim_slot_refresh(uint32_t now, int force);
static int dual_sim_management_status(void);
static int sim_traffic_page_status(void);
static int fm_card_available(void);

/* ---- page-2 aux state, cached (-1 = unknown). Like the reference plugin,
 * bands are controlled purely with `ifconfig wlanN up/down` and read back from
 * operstate (wlan0=main 2.4G, wlan2=main 5G); uci/`wifi reload`/`zwrt_wlan
 * reload` are NOT used (they don't work / wedge the radios). DevUI is the sole
 * owner of WiFi PSM: the selected target is persisted under the plugin data
 * directory and one hotplug script reads it on every ifup. The DHCP pool is
 * computed live from uci. Toggles flip optimistically; a throttle reconciles. */
#define WIFI_PSM_STATE_FILE "/data/plugins/u60pro-devui/wifi-power-save.conf"
#define WIFI_PSM_STATE_TMP  "/data/plugins/u60pro-devui/wifi-power-save.conf.tmp"
#define WIFI_PSM_HOTPLUG    "/etc/hotplug.d/iface/99-devui-wifi-powersave"
#define WIFI_PSM_LEGACY     "/etc/hotplug.d/iface/99-disable-powersave"
#define WIFI_PSM_PLUGIN     "/etc/hotplug.d/iface/psm"
#define WIFI_PSM_UFI_BOOT   "/data/ufi-tools/sdcard/ufi_tools_boot.sh"
static int  g_w24 = -1, g_w5 = -1, g_wpsm = -1;
static int  g_wpsm_target = -1;
static int  g_dps = -1;   /* power direct-supply mode (zwrt_bsp.charger), -1 unknown */
static int  g_adb_pending = -1; /* optimistic ADB state while USB re-enumerates */
static int  g_usb_net = -1;     /* Type-C network sharing composition active */
static int  g_typec_source = -1; /* 1 = reverse charge peer, 0 = charge U60 */
static int  g_typec_attached = -1;
static int  g_usb_net_pending = -1;
static int  g_typec_pending = -1;
static uint32_t g_usb_net_until, g_typec_until;
static char g_dhcp_pool[48];
/* currently WiFi-associated client MACs (lowercased, comma-joined). DHCP leases
 * linger after a device leaves, so the device list filters against this (live
 * station dump) to match the vendor's "only connected" behavior. */
static char g_assoc_macs[640];
static uint32_t g_wifi_aux_at;
static uint32_t g_wifi_psm_repair_at;

/* Is this MAC currently associated to a radio? (case-insensitive substring) */
static int mac_assoc(const char *mac)
{
    if (!g_assoc_macs[0] || !mac || !mac[0]) return 1;   /* unknown -> don't hide */
    char lc[24]; int j = 0;
    for (const char *p = mac; *p && j < 23; p++) {
        char c = *p; if (c >= 'A' && c <= 'Z') c += 32; lc[j++] = c;
    }
    lc[j] = 0;
    return strstr(g_assoc_macs, lc) != NULL;
}

static int read_long_path(const char *path, long *out)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    long v;
    int ok = fscanf(fp, "%ld", &v) == 1;
    fclose(fp);
    if (!ok) return 0;
    *out = v;
    return 1;
}

static int read_line_path(const char *path, char *out, size_t cap)
{
    FILE *fp = fopen(path, "r");
    size_t n;
    if (!fp || cap == 0) { if (fp) fclose(fp); return 0; }
    if (!fgets(out, (int)cap, fp)) { fclose(fp); return 0; }
    fclose(fp);
    n = strcspn(out, "\r\n");
    out[n] = 0;
    return 1;
}

static int boot_is_nonsilent(void)
{
    char cmdline[2048];
    FILE *fp = fopen("/proc/cmdline", "r");
    if (!fp) return 0;
    size_t n = fread(cmdline, 1, sizeof cmdline - 1, fp);
    fclose(fp);
    cmdline[n] = 0;
    return strstr(cmdline, "silent_boot.mode=nonsilent") != NULL;
}

static int read_mode_main_state(char *out, size_t cap)
{
    FILE *fp = fopen("/etc/config/zwrt_zte_mc_tmp", "r");
    char line[256];
    if (!fp || cap == 0) { if (fp) fclose(fp); return 0; }
    while (fgets(line, sizeof line, fp)) {
        if (!strstr(line, "option mode_main_state")) continue;
        char *q = strchr(line, '\'');
        if (!q) continue;
        char *e = strchr(q + 1, '\'');
        if (!e) continue;
        size_t n = (size_t)(e - (q + 1));
        if (n >= cap) n = cap - 1;
        memcpy(out, q + 1, n);
        out[n] = 0;
        fclose(fp);
        return 1;
    }
    fclose(fp);
    return 0;
}

static int boot_is_charge_mode(void)
{
    char mode[64];
    if (read_mode_main_state(mode, sizeof mode)) {
        if (!strncmp(mode, "mode_power_off_", 15)) return 1;
        if (!strcmp(mode, "mode_power_on") || !strcmp(mode, "mode_power_on_charger")) return 0;
    }
    return !boot_is_nonsilent();
}

static int boot_has_external_power(void)
{
    long v;
    char st[32];
    if (read_long_path("/sys/class/power_supply/usb/online", &v) && v != 0) return 1;
    if (read_long_path("/sys/class/power_supply/charger_zte/present_mbb", &v) && v != 0) return 1;
    if (read_long_path("/sys/class/power_supply/type-c_zte/present_mbb", &v) && v != 0) return 1;
    if (read_line_path("/sys/class/power_supply/battery/status", st, sizeof st)) {
        if (!strcmp(st, "Charging") || !strcmp(st, "Full")) return 1;
    }
    if (read_long_path("/sys/class/power_supply/charger_zte/status_mbb", &v) && v == 1) return 1;
    if (read_long_path("/sys/class/power_supply/battery_zte/status_mbb", &v) && v == 1) return 1;
    if (read_long_path("/sys/class/power_supply/statistics_zte/batt_status", &v) && v == 1) return 1;
    return 0;
}

/* ---- screen lock (4-digit PIN) ---- */
static char g_pin[8];        /* stored PIN ("" = lock disabled) */
static int  g_lock_state;    /* 0=normal, 1=locked preview, 2=setup pad, 3=unlock pad */
static char g_pin_entry[8];  /* digits typed so far */
static int  g_lock_err;      /* 1 = show the unlock-pad error message */

static void scan_pages(void)
{
    g_npages = 0;
    DIR *dp = opendir(UI_DIR);
    if (!dp) return;
    char names[24][64]; int n = 0;
    struct dirent *de;
    while ((de = readdir(dp)) && n < 24) {
        size_t l = strlen(de->d_name);
        if (de->d_name[0] != '.' &&
            l > 5 && strcmp(de->d_name + l - 5, ".html") == 0 &&
            strcmp(de->d_name, "menu.html") != 0 &&
            strcmp(de->d_name, "lockscreen.html") != 0) {
            if (!g_sig_parse && strcmp(de->d_name, "01a-cell.html") == 0)
                continue;
            strncpy(names[n], de->d_name, 63); names[n][63] = 0; n++;
        }
    }
    closedir(dp);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[64]; strcpy(t, names[i]); strcpy(names[i], names[j]); strcpy(names[j], t);
            }
    for (int i = 0; i < n; i++)
        snprintf(g_pages[i], sizeof g_pages[i], "%s/%s", UI_DIR, names[i]);
    g_npages = n;
}

static void rescan_pages_keep_current(void)
{
    char cur_name[64];

    cur_name[0] = 0;
    if (g_npages > 0 && g_cur >= 0 && g_cur < g_npages) {
        const char *base = strrchr(g_pages[g_cur], '/');
        snprintf(cur_name, sizeof cur_name, "%s", base ? base + 1 : g_pages[g_cur]);
    }

    scan_pages();
    if (g_npages <= 0) {
        g_cur = 0;
        return;
    }
    if (cur_name[0]) {
        for (int i = 0; i < g_npages; i++) {
            const char *base = strrchr(g_pages[i], '/');
            const char *name = base ? base + 1 : g_pages[i];
            if (!strcmp(name, cur_name)) {
                g_cur = i;
                return;
            }
        }
    }
    if (g_cur >= g_npages) g_cur = g_npages - 1;
    if (g_cur < 0) g_cur = 0;
}

/* ---- value formatting ---- */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void fmt_bytes(char *o, size_t n, long b) {
    if (b >= 1024L * 1024 * 1024) snprintf(o, n, "%.2f GB", b / 1073741824.0);
    else                          snprintf(o, n, "%.1f MB", b / 1048576.0);
}
static void fmt_bytes_u64(char *o, size_t n, unsigned long long b) {
    if (b >= 1024ULL * 1024 * 1024) snprintf(o, n, "%.2f GB", b / 1073741824.0);
    else if (b >= 1024ULL * 1024)   snprintf(o, n, "%.1f MB", b / 1048576.0);
    else if (b >= 1024ULL)          snprintf(o, n, "%.1f KB", b / 1024.0);
    else                            snprintf(o, n, "%llu B", b);
}
static void fmt_uptime(char *o, size_t n, long s) {
    long d = s / 86400; s %= 86400;
    snprintf(o, n, "%ldd %02ld:%02ld:%02ld", d, s / 3600, (s / 60) % 60, s % 60);
}
static void fmt_uptime_s(char *o, size_t n, long s) {   /* compact, for the grid */
    long d = s / 86400, hh = (s % 86400) / 3600, mm = (s % 3600) / 60;
    if (d > 0)       snprintf(o, n, "%ldd %ldh", d, hh);
    else if (hh > 0) snprintf(o, n, "%ldh %ldm", hh, mm);
    else             snprintf(o, n, "%ldm", mm);
}
/* compact speed for the status bar: 3.0M / 80K / 224B */
static void fmt_speed_c(char *o, size_t n, long bps) {
    if (bps >= 1024 * 1024) snprintf(o, n, "%.1fM", bps / 1048576.0);
    else if (bps >= 1024)   snprintf(o, n, "%.0fK", bps / 1024.0);
    else                    snprintf(o, n, "%ldB", bps);
}
static void fmt_one(char *o, size_t n, double v) {
    if (v >= 10) snprintf(o, n, "%.0f", v);
    else         snprintf(o, n, "%.1f", v);
}
/* Single speed value with unit, honoring the bit/byte-rate setting (matches the
 * status-bar units): bits=1 -> Mbps/Kbps/bps, bits=0 -> MB/s/KB/s/B/s. */
static void fmt_speed_u(char *o, size_t n, long bps, int bits)
{
    double v = bps * (bits ? 8.0 : 1.0);
    const char *unit; double div;
    if (bits) { if (v >= 1e6) { unit = "Mbps"; div = 1e6; } else if (v >= 1e3) { unit = "Kbps"; div = 1e3; } else { unit = "bps"; div = 1; } }
    else      { if (v >= 1e6) { unit = "MB/s"; div = 1e6; } else if (v >= 1e3) { unit = "KB/s"; div = 1e3; } else { unit = "B/s"; div = 1; } }
    double val = v / div;
    if (val >= 10) snprintf(o, n, "%.0f %s", val, unit);
    else           snprintf(o, n, "%.1f %s", val, unit);
}

/* Status-bar speed pair: "up/down <unit>", shared unit picked from the
 * larger of the two. bits=1 -> Mbps/Kbps/bps; bits=0 -> MB/s/KB/s/B/s. */
static void fmt_speed_pair(char *buf, size_t cap, long up, long down, int bits) {
    double mul = bits ? 8.0 : 1.0;
    double u = up * mul, d = down * mul, mx = u > d ? u : d;
    const char *unit; double div;
    if (bits) { if (mx >= 1e6) { unit = "Mbps"; div = 1e6; } else if (mx >= 1e3) { unit = "Kbps"; div = 1e3; } else { unit = "bps"; div = 1; } }
    else      { if (mx >= 1e6) { unit = "MB/s"; div = 1e6; } else if (mx >= 1e3) { unit = "KB/s"; div = 1e3; } else { unit = "B/s"; div = 1; } }
    char us[12], ds[12];
    fmt_one(us, sizeof us, u / div); fmt_one(ds, sizeof ds, d / div);
    snprintf(buf, cap, "\xe2\x86\x91%s \xe2\x86\x93%s %s", us, ds, unit);
}

/* ---- {{key}} template substitution ---- */
struct kv { const char *k; const char *v; };
#define KV_MAX 320
static void html_esc(char *dst, size_t cap, const char *src);
static int signal_value_present(const char *s);

static char *apply_template(const char *tmpl, struct kv *t, int n)
{
    static char out[1048576];
    size_t o = 0;
    for (const char *p = tmpl; *p && o < sizeof(out) - 1; ) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (end) {
                int kl = (int)(end - (p + 2));
                const char *v = "";
                for (int i = 0; i < n; i++)
                    if ((int)strlen(t[i].k) == kl && strncmp(t[i].k, p + 2, kl) == 0) { v = t[i].v; break; }
                size_t vl = strlen(v);
                if (o + vl < sizeof(out) - 1) { memcpy(out + o, v, vl); o += vl; }
                else {
                    size_t room = sizeof(out) - 1 - o;
                    if (room) memcpy(out + o, v, room);
                    o = sizeof(out) - 1;
                    out[o] = 0;
                    break;
                }
                p = end + 2;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    return out;
}

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(fp); return NULL; }
    for (;;) {
        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char *nbuf = realloc(buf, ncap);
            if (!nbuf) { free(buf); fclose(fp); return NULL; }
            buf = nbuf; cap = ncap;
        }
        size_t r = fread(buf + n, 1, cap - n - 1, fp);
        n += r;
        if (r == 0) break;
    }
    if (ferror(fp)) { free(buf); fclose(fp); return NULL; }
    buf[n] = 0; fclose(fp);
    return buf;
}

static long long file_mtime_ns(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long long)st.st_mtime * 1000000000LL + (long long)st.st_mtim.tv_nsec;
}

static long long ui_scan_mtime_ns(void)
{
    long long a = file_mtime_ns(UI_DIR);
    long long b = file_mtime_ns(FUNCTIONS_DIR);
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    return a ^ (b << 1);
}

static int rescan_pages_if_changed(void)
{
    long long mtime = ui_scan_mtime_ns();
    if (mtime < 0 || mtime == g_pages_dir_mtime) return 0;
    g_pages_dir_mtime = mtime;
    rescan_pages_keep_current();
    invalidate_render_html_cache();
    return 1;
}

static const char *read_template_cached(const char *path)
{
    long long mtime = file_mtime_ns(path);
    if (mtime < 0) return NULL;

    for (int i = 0; i < TEMPLATE_CACHE_CAP; i++) {
        struct template_cache_entry *e = &g_tmpl_cache[i];
        if (!e->used || !e->path[0]) continue;
        if (strcmp(e->path, path) != 0) continue;

        if (e->mtime_ns == mtime && e->content) return e->content;

        char *next = read_file(path);
        if (!next) return e->content;   /* keep previous cached copy if read fails */
        free(e->content);
        e->content = next;
        e->mtime_ns = mtime;
        return e->content;
    }

    char *tmpl = read_file(path);
    if (!tmpl) return NULL;

    for (int i = 0; i < TEMPLATE_CACHE_CAP; i++) {
        if (!g_tmpl_cache[i].used) {
            snprintf(g_tmpl_cache[i].path, sizeof(g_tmpl_cache[i].path), "%s", path);
            g_tmpl_cache[i].mtime_ns = mtime;
            g_tmpl_cache[i].content = tmpl;
            g_tmpl_cache[i].used = 1;
            return g_tmpl_cache[i].content;
        }
    }

    struct template_cache_entry *e = &g_tmpl_cache[g_tmpl_next];
    free(e->content);
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->mtime_ns = mtime;
    e->content = tmpl;
    e->used = 1;
    g_tmpl_next = (g_tmpl_next + 1) % TEMPLATE_CACHE_CAP;
    return e->content;
}

static int usb_pid_is(const char *needle)
{
    int ok = 0;
    char *pid = read_file("/sys/kernel/config/usb_gadget/g1/idProduct");
    if (pid) {
        ok = strstr(pid, needle) != NULL;
        free(pid);
    }
    return ok;
}

static int usb_net_has_carrier(void)
{
    int ok = 0;
    char *c = read_file("/sys/class/net/rndis0/carrier");
    if (c) { if (c[0] == '1') ok = 1; free(c); }
    c = read_file("/sys/class/net/ecm0/carrier");
    if (c) { if (c[0] == '1') ok = 1; free(c); }
    return ok;
}

static void usb_net_watchdog_stop(void)
{
    system("pid=$(cat /tmp/u60-usbnet-watchdog.pid 2>/dev/null); "
           "[ -n \"$pid\" ] && kill \"$pid\" 2>/dev/null; "
           "rm -f /tmp/u60-usbnet-watchdog.pid /tmp/u60-usbnet-enabled");
}

static void usb_net_apply_source_async(void)
{
    system("(touch /tmp/u60-typec-source /tmp/u60-usbnet-switching; "
           "date '+%F %T usbnet apply source' >>/tmp/usb.log 2>/dev/null; "
           "ubus call zwrt_bsp.powerbank set '{\"state\":0}'; "
           "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}'; "
           "sleep 1; echo peripheral > /sys/bus/platform/devices/a600000.ssusb/mode 2>/dev/null; "
           "/sbin/usb_composition 90B1 n n y n; "
           "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do "
           "att=$(ubus call zwrt_bsp.typec list 2>/dev/null | grep cc_attch_state | grep -o '[0-9]' | head -1); "
           "pid2=$(cat /sys/kernel/config/usb_gadget/g1/idProduct 2>/dev/null); "
           "e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null); "
           "if [ \"$att\" = \"1\" ] && { [ \"$pid2\" = \"0x90b1\" ] || [ \"$pid2\" = \"0x90B1\" ]; } && [ \"$e\" = \"1\" ]; then break; fi; "
           "sleep 1; done; "
           "if [ \"$att\" = \"1\" ] && { [ \"$pid2\" = \"0x90b1\" ] || [ \"$pid2\" = \"0x90B1\" ]; } && [ \"$e\" = \"1\" ]; then "
           "ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"source\"}'; "
           "sleep 1; ubus call zwrt_bsp.powerbank set '{\"state\":1}'; "
           "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\"}'; "
           "sleep 1; /sbin/usb_composition 90B1 n n y n; "
           "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do "
           "r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null); "
           "e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null); "
           "if [ \"$r\" = \"1\" ] || [ \"$e\" = \"1\" ]; then break; fi; "
           "sleep 1; done; "
           "else "
           "date '+%F %T usbnet source no ecm carrier, try rndis' >>/tmp/usb.log 2>/dev/null; "
           "rm -f /tmp/u60-typec-source; "
           "/sbin/usb_composition 9057 n n y n; "
           "sleep 10; "
           "r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null); "
           "e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null); "
           "if [ \"$r\" != \"1\" ] && [ \"$e\" != \"1\" ]; then "
           "date '+%F %T usbnet rndis no carrier, fallback 90B1' >>/tmp/usb.log 2>/dev/null; "
           "/sbin/usb_composition 90B1 n n y n; fi; "
           "fi; "
           "rm -f /tmp/u60-usbnet-switching) >/tmp/u60-usbnet-switch.log 2>&1 &");
}

static void usb_net_apply_sink_async(void)
{
    system("(touch /tmp/u60-usbnet-switching; rm -f /tmp/u60-typec-source; "
           "date '+%F %T usbnet apply sink' >>/tmp/usb.log 2>/dev/null; "
           "ubus call zwrt_bsp.powerbank set '{\"state\":0}'; "
           "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}'; "
           "sleep 1; echo peripheral > /sys/bus/platform/devices/a600000.ssusb/mode 2>/dev/null; "
           "/sbin/usb_composition 9057 n n y n; "
           "sleep 10; "
           "r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null); "
           "e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null); "
           "if [ \"$r\" != \"1\" ] && [ \"$e\" != \"1\" ]; then "
           "date '+%F %T usbnet fallback 90B1' >>/tmp/usb.log 2>/dev/null; "
           "/sbin/usb_composition 90B1 n n y n; fi; "
           "rm -f /tmp/u60-usbnet-switching) >/tmp/u60-usbnet-switch.log 2>&1 &");
}

static void usb_power_only_apply_async(int source)
{
    if (source) {
        system("(touch /tmp/u60-typec-source /tmp/u60-usbnet-switching; "
               "date '+%F %T usbpower apply source' >>/tmp/usb.log 2>/dev/null; "
               "ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"source\"}'; "
               "sleep 1; ubus call zwrt_bsp.powerbank set '{\"state\":1}'; "
               "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\"}'; "
               "sleep 1; SER=$(cat /sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber 2>/dev/null); "
               "sh /sbin/usb/compositions/usb_switch 0x19d2 0x1225 mass_storage \"$SER\"; "
               "rm -f /tmp/u60-usbnet-switching) >/tmp/u60-usbnet-switch.log 2>&1 &");
    } else {
        system("(touch /tmp/u60-usbnet-switching; rm -f /tmp/u60-typec-source; "
               "date '+%F %T usbpower apply sink' >>/tmp/usb.log 2>/dev/null; "
               "ubus call zwrt_bsp.powerbank set '{\"state\":0}'; "
               "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}'; "
               "sleep 1; SER=$(cat /sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber 2>/dev/null); "
               "sh /sbin/usb/compositions/usb_switch 0x19d2 0x1225 mass_storage \"$SER\"; "
               "rm -f /tmp/u60-usbnet-switching) >/tmp/u60-usbnet-switch.log 2>&1 &");
    }
}

static void usb_net_watchdog_start(void)
{
    usb_net_watchdog_stop();
    system("cat >/tmp/u60-usbnet-watchdog.sh <<'EOF'\n"
           "#!/bin/sh\n"
           "touch /tmp/u60-usbnet-enabled\n"
           "echo $$ >/tmp/u60-usbnet-watchdog.pid\n"
           "while [ -f /tmp/u60-usbnet-enabled ]; do\n"
           "  if [ -f /tmp/u60-usbnet-switching ]; then sleep 2; continue; fi\n"
           "  pid=$(cat /sys/kernel/config/usb_gadget/g1/idProduct 2>/dev/null)\n"
           "  tc=$(ubus call zwrt_bsp.typec list 2>/dev/null)\n"
           "  att=$(echo \"$tc\" | grep cc_attch_state | grep -o '[0-9]' | head -1)\n"
           "  pr=$(echo \"$tc\" | grep power_role | grep -o 'source\\|sink' | head -1)\n"
           "  r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null)\n"
           "  e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "  case \"$pid\" in 0x9057|0x90b1|0x90B1) netpid=1;; *) netpid=0;; esac\n"
            "  wantsrc=0; [ -f /tmp/u60-typec-source ] && wantsrc=1\n"
            "  rolebad=0\n"
            "  if [ \"$wantsrc\" = 1 ] && [ \"$pr\" != source ]; then rolebad=1; fi\n"
            "  if [ \"$wantsrc\" != 1 ] && [ \"$pr\" != sink ]; then rolebad=1; fi\n"
            "  if [ \"$att\" = 1 ] && { [ \"$rolebad\" = 1 ] || [ \"$netpid\" != 1 ] || { [ \"$r\" != 1 ] && [ \"$e\" != 1 ]; }; }; then\n"
            "    date '+%F %T usbnet watchdog rearm' >>/tmp/usb.log 2>/dev/null\n"
            "    touch /tmp/u60-usbnet-switching\n"
            "    if [ \"$wantsrc\" = 1 ]; then\n"
            "      ubus call zwrt_bsp.powerbank set '{\"state\":0}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      echo peripheral > /sys/bus/platform/devices/a600000.ssusb/mode 2>/dev/null\n"
            "      /sbin/usb_composition 90B1 n n y n >>/tmp/usb.log 2>&1\n"
            "      for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do\n"
            "        att=$(ubus call zwrt_bsp.typec list 2>/dev/null | grep cc_attch_state | grep -o '[0-9]' | head -1)\n"
            "        pid2=$(cat /sys/kernel/config/usb_gadget/g1/idProduct 2>/dev/null)\n"
            "        e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "        if [ \"$att\" = 1 ] && { [ \"$pid2\" = 0x90b1 ] || [ \"$pid2\" = 0x90B1 ]; } && [ \"$e\" = 1 ]; then break; fi\n"
            "        sleep 1\n"
            "      done\n"
            "      if [ \"$att\" = 1 ] && { [ \"$pid2\" = 0x90b1 ] || [ \"$pid2\" = 0x90B1 ]; } && [ \"$e\" = 1 ]; then\n"
            "      ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"source\"}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      ubus call zwrt_bsp.powerbank set '{\"state\":1}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\"}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      /sbin/usb_composition 90B1 n n y n >>/tmp/usb.log 2>&1\n"
            "      for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do\n"
            "        r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null)\n"
            "        e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "        if [ \"$r\" = 1 ] || [ \"$e\" = 1 ]; then break; fi\n"
            "        sleep 1\n"
            "      done\n"
            "      else\n"
            "        date '+%F %T usbnet watchdog source no ecm carrier, try rndis' >>/tmp/usb.log 2>/dev/null\n"
            "        rm -f /tmp/u60-typec-source\n"
            "        /sbin/usb_composition 9057 n n y n >>/tmp/usb.log 2>&1\n"
            "        sleep 10\n"
            "        r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null)\n"
            "        e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "        if [ \"$r\" != 1 ] && [ \"$e\" != 1 ]; then\n"
            "          date '+%F %T usbnet watchdog rndis no carrier, fallback 90B1' >>/tmp/usb.log 2>/dev/null\n"
            "          /sbin/usb_composition 90B1 n n y n >>/tmp/usb.log 2>&1\n"
            "        fi\n"
            "      fi\n"
            "    else\n"
            "      ubus call zwrt_bsp.powerbank set '{\"state\":0}' >/dev/null 2>&1\n"
            "      ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      echo peripheral > /sys/bus/platform/devices/a600000.ssusb/mode 2>/dev/null\n"
            "      /sbin/usb_composition 9057 n n y n >>/tmp/usb.log 2>&1\n"
            "      sleep 10\n"
            "      r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null)\n"
            "      e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "      if [ \"$r\" != 1 ] && [ \"$e\" != 1 ]; then\n"
            "        date '+%F %T usbnet watchdog fallback 90B1' >>/tmp/usb.log 2>/dev/null\n"
            "        /sbin/usb_composition 90B1 n n y n >>/tmp/usb.log 2>&1\n"
            "      fi\n"
            "    fi\n"
            "    rm -f /tmp/u60-usbnet-switching\n"
            "  fi\n"
            "  sleep 12\n"
            "done\n"
           "EOF\n"
           "chmod +x /tmp/u60-usbnet-watchdog.sh\n"
           "nohup /tmp/u60-usbnet-watchdog.sh >/tmp/u60-usbnet-watchdog.log 2>&1 &");
}

/* Kiwi rejects `power_save on` on the 2.4G AP (wlan0) with EINVAL, while the
 * 5G AP supports it. Treat the 5G radio(s) as the authoritative PSM state;
 * otherwise the unsupported wlan0 would make the switch immediately flip off. */
static int wifi_psm_live_state(void)
{
    FILE *fp = popen(
        "for w in wlan2 wlan3; do "
        "[ -d /sys/class/net/$w ] || continue; "
        "iw dev $w get power_save 2>/dev/null | awk '/Power save:/{print $3; exit}'; "
        "done", "r");
    int seen = 0, on = 0, off = 0;
    char line[32];
    if (!fp) return -1;
    while (fgets(line, sizeof line, fp)) {
        if (!strncmp(line, "on", 2)) { on = 1; seen = 1; }
        else if (!strncmp(line, "off", 3)) { off = 1; seen = 1; }
    }
    pclose(fp);
    if (!seen) return -1;
    if (on && off) return -2;
    return on ? 1 : 0;
}

static int wifi_psm_read_target(void)
{
    FILE *fp = fopen(WIFI_PSM_STATE_FILE, "r");
    char value[16];
    if (!fp) return -1;
    value[0] = 0;
    if (!fgets(value, sizeof value, fp)) value[0] = 0;
    fclose(fp);
    if (!strncmp(value, "on", 2)) return 1;
    if (!strncmp(value, "off", 3)) return 0;
    return -1;
}

static int wifi_psm_write_target(int on)
{
    FILE *fp;
    int ok;
    mkdir("/data/plugins/u60pro-devui", 0755);
    fp = fopen(WIFI_PSM_STATE_TMP, "w");
    if (!fp) return -1;
    ok = fprintf(fp, "%s\n", on ? "on" : "off") >= 0;
    if (fclose(fp) != 0) ok = 0;
    if (!ok) {
        unlink(WIFI_PSM_STATE_TMP);
        return -1;
    }
    if (rename(WIFI_PSM_STATE_TMP, WIFI_PSM_STATE_FILE) != 0) {
        unlink(WIFI_PSM_STATE_TMP);
        return -1;
    }
    return 0;
}

static int wifi_psm_install_hotplug(void)
{
    const char *tmp = "/etc/hotplug.d/iface/99-devui-wifi-powersave.tmp";
    FILE *fp;
    mkdir("/etc/hotplug.d/iface", 0755);
    fp = fopen(tmp, "w");
    if (!fp) return -1;
    fputs("#!/bin/sh\n"
          "[ \"$ACTION\" = ifup ] || [ \"$ACTION\" = ifupdate ] || exit 0\n"
          "mode=$(cat " WIFI_PSM_STATE_FILE " 2>/dev/null)\n"
          "case \"$mode\" in on|off) ;; *) exit 0 ;; esac\n"
          "(\n"
          "  i=0\n"
          "  while [ $i -lt 8 ]; do\n"
          "    applied=0\n"
          "    for w in wlan0 wlan1 wlan2 wlan3; do\n"
          "      [ -d /sys/class/net/$w ] || continue\n"
          "      iw dev $w set power_save $mode 2>/dev/null && applied=1\n"
          "    done\n"
          "    [ $applied -eq 1 ] && exit 0\n"
          "    i=$((i+1))\n"
          "    sleep 1\n"
          "  done\n"
          ") >/dev/null 2>&1 &\n", fp);
    if (fclose(fp) != 0) { unlink(tmp); return -1; }
    if (chmod(tmp, 0755) != 0 || rename(tmp, WIFI_PSM_HOTPLUG) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void wifi_psm_cleanup_legacy(void)
{
    unlink(WIFI_PSM_PLUGIN);
    unlink(WIFI_PSM_LEGACY);
    if (access(WIFI_PSM_UFI_BOOT, F_OK) == 0)
        (void)system("sed -i '/psm_boot/d' " WIFI_PSM_UFI_BOOT " >/dev/null 2>&1");
}

static int wifi_psm_apply_now(int on)
{
    const char *mode = on ? "on" : "off";
    char cmd[256];
    int live;
    snprintf(cmd, sizeof cmd,
             "for w in wlan0 wlan1 wlan2 wlan3; do "
             "[ -d /sys/class/net/$w ] || continue; "
             "iw dev $w set power_save %s 2>/dev/null; done", mode);
    (void)system(cmd);
    usleep(300000);
    live = wifi_psm_live_state();
    if (live >= 0 && live != on) {
        (void)system(cmd);
        usleep(300000);
        live = wifi_psm_live_state();
    }
    return live;
}

static int wifi_psm_set_target(int on)
{
    wifi_psm_cleanup_legacy();
    if (wifi_psm_write_target(on) != 0 || wifi_psm_install_hotplug() != 0)
        return -1;
    g_wpsm_target = on;
    g_wpsm = wifi_psm_apply_now(on);
    if (g_wpsm == -1) g_wpsm = on;
    return g_wpsm == on ? 0 : -1;
}

static void wifi_psm_prepare(void)
{
    int target = wifi_psm_read_target();
    int live = wifi_psm_live_state();

    /* The first upgraded boot preserves the currently effective mode. */
    if (target < 0) target = live == 1 ? 1 : 0;
    wifi_psm_cleanup_legacy();
    if (wifi_psm_write_target(target) == 0 && wifi_psm_install_hotplug() == 0) {
        g_wpsm_target = target;
        g_wpsm = wifi_psm_apply_now(target);
        if (g_wpsm == -1) g_wpsm = target;
    }
}

/* Read page-2 aux state into the cache: band on/off from netdev operstate (the
 * live truth on this firmware), PSM from `iw power_save`, and the DHCP pool
 * range computed live from uci (lan ip + start/limit offsets). One shell
 * round-trip, called on a throttle. */
static void wifi_aux_refresh(void)
{
    FILE *fp = popen(
        "echo W0=$(cat /sys/class/net/wlan0/operstate 2>/dev/null);"
        "echo W2=$(cat /sys/class/net/wlan2/operstate 2>/dev/null);"
        "ip=$(uci -q get network.lan.ipaddr); st=$(uci -q get dhcp.lan.start); lim=$(uci -q get dhcp.lan.limit);"
        "if [ -n \"$ip\" ] && [ -n \"$st\" ]; then pre=${ip%.*}; end=$((st+lim-1)); [ $end -gt 254 ] && end=254;"
        "echo \"POOL=$pre.$st - $pre.$end\"; fi;"
        /* "connected now" = WiFi-associated stations plus ARP-complete entries
         * (ARP covers LAN/USB clients too; leases alone linger 24h). */
        "echo MACS=$({ for w in wlan0 wlan1 wlan2 wlan3; do iw dev $w station dump 2>/dev/null | awk '/Station/{print $2}'; done;"
        " awk 'NR>1 && $3!=\"0x0\"{print $4}' /proc/net/arp 2>/dev/null; } | tr 'A-Z\\n' 'a-z,');"
        "echo DPS=$(ubus call zwrt_bsp.charger list 2>/dev/null | grep direct_power_supply_mode | grep -o 'enable\\|disable');"
        "pid=$(cat /sys/kernel/config/usb_gadget/g1/idProduct 2>/dev/null);"
        "echo USBNET=$([ \"$pid\" = 0x9057 ] || [ \"$pid\" = 0x90b1 ] || [ \"$pid\" = 0x90B1 ] && echo 1 || echo 0);"
        "tc=$(ubus call zwrt_bsp.typec list 2>/dev/null);"
        "echo TYPECSRC=$(echo \"$tc\" | grep power_role | grep -o 'source\\|sink' | head -1);"
        "echo TYPECATTACH=$(echo \"$tc\" | grep cc_attch_state | grep -o '[0-9]' | head -1)", "r");
    if (!fp) return;
    char line[768];
    while (fgets(line, sizeof line, fp)) {
        if      (!strncmp(line, "W0=", 3))   g_w24 = strstr(line, "=up") != NULL;
        else if (!strncmp(line, "W2=", 3))   g_w5  = strstr(line, "=up") != NULL;
        else if (!strncmp(line, "POOL=", 5)) {
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            snprintf(g_dhcp_pool, sizeof g_dhcp_pool, "%s", line + 5);
        }
        else if (!strncmp(line, "MACS=", 5)) {
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            snprintf(g_assoc_macs, sizeof g_assoc_macs, "%s", line + 5);
        }
        else if (!strncmp(line, "DPS=", 4)) {
            if (strstr(line, "disable")) g_dps = 0;
            else if (strstr(line, "enable")) g_dps = 1;
        }
        else if (!strncmp(line, "USBNET=", 7)) {
            g_usb_net = atoi(line + 7) ? 1 : 0;
        }
        else if (!strncmp(line, "TYPECSRC=", 9)) {
            if (strstr(line, "source")) g_typec_source = 1;
            else if (strstr(line, "sink")) g_typec_source = 0;
        }
        else if (!strncmp(line, "TYPECATTACH=", 12)) {
            g_typec_attached = atoi(line + 12) ? 1 : 0;
        }
    }
    pclose(fp);

    {
        int live = wifi_psm_live_state();
        uint32_t now = millis();
        g_wpsm = live == -1 ? g_wpsm_target : live;
        if (g_wpsm_target >= 0 && live != -1 && live != g_wpsm_target &&
            (!g_wifi_psm_repair_at || now - g_wifi_psm_repair_at >= 10000)) {
            g_wifi_psm_repair_at = now;
            wifi_psm_cleanup_legacy();
            (void)wifi_psm_install_hotplug();
            g_wpsm = wifi_psm_apply_now(g_wpsm_target);
        }
    }
}

static void line_value(char *dst, size_t cap, const char *line, size_t prefix_len)
{
    const char *src = line + prefix_len;
    size_t len = strcspn(src, "\r\n");
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
    if (!dst[0]) snprintf(dst, cap, "-");
}

static void plugin_action_log_html(char *dst, size_t cap, const char *path)
{
    FILE *fp;
    char line[320], last[3][320], esc[640];
    int seen = 0, o = 0;

    dst[0] = 0;
    fp = fopen(path, "r");
    if (fp) {
        while (fgets(line, sizeof line, fp)) {
            size_t len = strcspn(line, "\r\n");
            line[len] = 0;
            if (!line[0]) continue;
            snprintf(last[seen % 3], sizeof last[0], "%s", line);
            seen++;
        }
        fclose(fp);
    }
    if (!seen) {
        snprintf(dst, cap, "<div class='svc-log-empty'>暂无操作记录</div>");
        return;
    }
    int count = seen < 3 ? seen : 3;
    int start = seen < 3 ? 0 : seen % 3;
    for (int i = 0; i < count && o < (int)cap - 1; i++) {
        html_esc(esc, sizeof esc, last[(start + i) % 3]);
        o += snprintf(dst + o, cap - (size_t)o,
                      "<div class='svc-log-line'>%s</div>", esc);
    }
}

static void plugin_action_note(const char *path, const char *text)
{
    FILE *fp = fopen(path, "a");
    char stamp[32];
    if (!fp) return;
    devui_time_stamp(stamp, sizeof stamp, time(NULL));
    fprintf(fp, "[%s] %s\n", stamp, text);
    fclose(fp);
}

static void plugin_action_submit(const char *log_path, const char *runner,
                                 const char *ctl, const char *verb, const char *label)
{
    char cmd[1536];
    char tz[32];
    devui_posix_tz(tz, sizeof tz);
    snprintf(cmd, sizeof cmd,
             "nohup sh -c 'export TZ=%s; log=\"%s\"; tmp=\"${log}.out.$$\"; "
             "printf \"[%%s] 开始执行：%s\\n\" \"$(date \"+%%F %%T\")\" >>\"$log\"; "
             "%s%s %s >\"$tmp\" 2>&1; rc=$?; "
             "while IFS= read -r line || [ -n \"$line\" ]; do "
             "printf \"[%%s] %%s\\n\" \"$(date \"+%%F %%T\")\" \"$line\"; done <\"$tmp\" >>\"$log\"; "
             "rm -f \"$tmp\"; printf \"[%%s] 执行完成：%s，退出码 %%s\\n\" "
             "\"$(date \"+%%F %%T\")\" \"$rc\" >>\"$log\"; "
             "tail -n 30 \"$log\" >\"$log.trim\" && mv \"$log.trim\" \"$log\"; exit \"$rc\"' "
             ">/dev/null 2>&1 &",
             tz, log_path, label, runner, ctl, verb, label);
    system(cmd);
}

static int plugin_status_page(const char *path)
{
    return path && (strstr(path, "/functions/tailscale.html") ||
                    strstr(path, "/functions/clash.html") ||
                    strstr(path, "/functions/mihomo.html") ||
                    strstr(path, "/functions/cpu-performance.html") ||
                    strstr(path, "/functions/wireguard.html") ||
                    strstr(path, "/functions/operator-lock.html") ||
                    strstr(path, "/functions/fmswitch.html") ||
                    strstr(path, "/functions/sim-switch.html") ||
                    strstr(path, "/functions/sim-traffic.html") ||
                    strstr(path, "/functions/timezone.html"));
}

static int plugin_page_named(const char *path, const char *name)
{
    char needle[96];
    snprintf(needle, sizeof needle, "/functions/%s", name);
    return path && strstr(path, needle) != NULL;
}

static char *trim_text(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = 0;
    return s;
}

static void duration_short(char *dst, size_t cap, long long sec)
{
    if (sec <= 0) snprintf(dst, cap, "-");
    else if (sec >= 3600) snprintf(dst, cap, "%lldh %lldm", sec / 3600, (sec % 3600) / 60);
    else if (sec >= 60) snprintf(dst, cap, "%lldm %llds", sec / 60, sec % 60);
    else snprintf(dst, cap, "%llds", sec);
}

static const char *fm_carrier_name(const char *carrier)
{
    if (!strcmp(carrier, "YD")) return "中国移动";
    if (!strcmp(carrier, "DX")) return "中国电信";
    if (!strcmp(carrier, "LT")) return "中国联通";
    return "未知";
}

static const char *fm_pin_carrier(const char *pin)
{
    if (!strcmp(pin, "0200")) return "YD";
    if (!strcmp(pin, "0300")) return "DX";
    if (!strcmp(pin, "0100")) return "LT";
    return NULL;
}

static int fm_network_registered(void)
{
    return !strcmp(g_fm_nettype, "SA") || !strcmp(g_fm_nettype, "NSA") ||
           !strcmp(g_fm_nettype, "LTE") || !strcmp(g_fm_nettype, "ENDC");
}

static int fm_display_read(int *configured)
{
    FILE *fp;
    char line[16];
    long value;
    char *end = NULL;

    if (configured) *configured = 0;
    fp = fopen(FM_DISPLAY_FILE, "r");
    if (!fp) return 0;
    if (!fgets(line, sizeof line, fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    value = strtol(line, &end, 10);
    if (end == line || (value != 0 && value != 1)) return 0;
    if (configured) *configured = 1;
    return value == 1;
}

static int fm_display_write(int enabled)
{
    FILE *fp;
    int write_ok;

    mkdir("/data/plugins/u60pro-devui", 0755);
    fp = fopen(FM_DISPLAY_TMP, "w");
    if (!fp) return -1;
    write_ok = fprintf(fp, "%d\n", enabled ? 1 : 0) >= 0;
    if (fclose(fp) != 0) write_ok = 0;
    if (!write_ok) {
        unlink(FM_DISPLAY_TMP);
        return -1;
    }
    (void)chmod(FM_DISPLAY_TMP, 0644);
    if (rename(FM_DISPLAY_TMP, FM_DISPLAY_FILE) != 0) {
        unlink(FM_DISPLAY_TMP);
        return -1;
    }
    g_fm_enabled = enabled ? 1 : 0;
    return 0;
}

static int fm_switch_rc_read(int *rc)
{
    char line[24], *end = NULL;
    long value;
    if (!read_line_path(FM_SWITCH_RC, line, sizeof line)) return 0;
    value = strtol(line, &end, 10);
    if (end == line) return 0;
    *rc = (int)value;
    return 1;
}

static void fm_switch_finish(uint32_t now, int ok, const char *reason)
{
    g_fm_switching = 0;
    g_fm_switch_pin[0] = 0;
    g_fm_switch_until = 0;
    g_fm_confirm_pin[0] = 0;
    g_fm_confirm_until = 0;
    unlink(FM_SWITCH_RC);
    snprintf(g_toast, sizeof g_toast, "%s", ok ? "飞猫三网切换成功" : reason ? reason : "飞猫三网切换失败");
    g_toast_until = now + 2200;
}

static int refresh_fm_status(uint32_t now, int force)
{
    uint32_t interval = g_fm_switching ? 1000U : 5000U;
    FILE *fp;
    char cmd[512], line[192];
    char old_source[sizeof g_fm_detect_source], old_reason[sizeof g_fm_detect_reason];
    int old_available = g_fm_available, old_switching = g_fm_switching;
    int rc;

    if (!force && g_fm_refresh_at && now - g_fm_refresh_at < interval) return 0;
    g_fm_refresh_at = now;
    snprintf(old_source, sizeof old_source, "%s", g_fm_detect_source);
    snprintf(old_reason, sizeof old_reason, "%s", g_fm_detect_reason);
    g_fm_available = 0;
    snprintf(g_fm_detect_source, sizeof g_fm_detect_source, "none");
    snprintf(g_fm_detect_reason, sizeof g_fm_detect_reason, "controller returned no match");
    snprintf(g_fm_carrier, sizeof g_fm_carrier, "-");
    snprintf(g_fm_provider, sizeof g_fm_provider, "-");
    snprintf(g_fm_nettype, sizeof g_fm_nettype, "-");
    snprintf(g_fm_band, sizeof g_fm_band, "-");
    snprintf(g_fm_signal, sizeof g_fm_signal, "-");
    snprintf(g_fm_plmn, sizeof g_fm_plmn, "-");

    if (access(FM_CTL_BUNDLED, R_OK) == 0) {
        snprintf(cmd, sizeof cmd, "sh '%s' status", FM_CTL_BUNDLED);
        fp = popen(cmd, "r");
        if (fp) {
            while (fgets(line, sizeof line, fp)) {
                if (!strncmp(line, "FM_AVAILABLE=", 13))
                    g_fm_available = line[13] == '1';
                else if (!strncmp(line, "FM_DETECT_SOURCE=", 17))
                    line_value(g_fm_detect_source, sizeof g_fm_detect_source, line, 17);
                else if (!strncmp(line, "FM_DETECT_REASON=", 17))
                    line_value(g_fm_detect_reason, sizeof g_fm_detect_reason, line, 17);
                else if (!strncmp(line, "FM_CARRIER=", 11))
                    line_value(g_fm_carrier, sizeof g_fm_carrier, line, 11);
                else if (!strncmp(line, "FM_PROVIDER=", 12))
                    line_value(g_fm_provider, sizeof g_fm_provider, line, 12);
                else if (!strncmp(line, "FM_NETTYPE=", 11))
                    line_value(g_fm_nettype, sizeof g_fm_nettype, line, 11);
                else if (!strncmp(line, "FM_BAND=", 8))
                    line_value(g_fm_band, sizeof g_fm_band, line, 8);
                else if (!strncmp(line, "FM_SIGNAL=", 10))
                    line_value(g_fm_signal, sizeof g_fm_signal, line, 10);
                else if (!strncmp(line, "FM_PLMN=", 8))
                    line_value(g_fm_plmn, sizeof g_fm_plmn, line, 8);
            }
            pclose(fp);
        }
    }

    if (g_fm_switching) {
        if (fm_switch_rc_read(&rc)) {
            const char *target = fm_pin_carrier(g_fm_switch_pin);
            if (rc == 0 && target && g_fm_available &&
                !strcmp(g_fm_carrier, target) && fm_network_registered())
                fm_switch_finish(now, 1, NULL);
            else
                fm_switch_finish(now, 0, rc == 0 ? "目标运营商状态未确认" : "飞猫三网切换失败");
        } else if ((int32_t)(now - g_fm_switch_until) >= 0) {
            fm_switch_finish(now, 0, "飞猫三网切换超时");
        }
    }
    return old_available != g_fm_available || old_switching != g_fm_switching ||
           strcmp(old_source, g_fm_detect_source) || strcmp(old_reason, g_fm_detect_reason);
}

static int fm_card_available(void)
{
    if (access(FM_CTL_BUNDLED, R_OK) != 0) return 0;
    (void)refresh_fm_status(millis(), 0);
    return g_fm_available;
}

static void fm_display_load(void)
{
    /* This device does not use a Flycat SIM. Keep the optional controller
     * inactive without creating a Flycat-only configuration file. */
    g_fm_enabled = 0;
}

static int fm_switch_start(const char *pin, uint32_t now)
{
    char cmd[768];
    unlink(FM_SWITCH_RC);
    if (snprintf(cmd, sizeof cmd,
                 "(sh '%s' switch '%s' >/dev/null 2>&1; echo $? >'%s') &",
                 FM_CTL_BUNDLED, pin, FM_SWITCH_RC) >= (int)sizeof cmd)
        return -1;
    if (system(cmd) != 0) return -1;
    snprintf(g_fm_switch_pin, sizeof g_fm_switch_pin, "%s", pin);
    g_fm_switching = 1;
    g_fm_switch_until = now + 100000U;
    g_fm_confirm_pin[0] = 0;
    g_fm_confirm_until = 0;
    g_fm_refresh_at = 0;
    g_plugin_status_at = 0;
    return 0;
}

static void fm_switch_action(const char *pin, uint32_t now)
{
    const char *target = fm_pin_carrier(pin);
    (void)refresh_fm_status(now, 1);
    if (!target) {
        snprintf(g_toast, sizeof g_toast, "飞猫切换目标无效");
    } else if (!g_fm_available) {
        snprintf(g_toast, sizeof g_toast, "未检测到飞猫分身卡");
    } else if (g_fm_switching) {
        snprintf(g_toast, sizeof g_toast, "飞猫三网正在切换");
    } else if (!strcmp(g_fm_carrier, target)) {
        g_fm_confirm_pin[0] = 0;
        g_fm_confirm_until = 0;
        snprintf(g_toast, sizeof g_toast, "当前正在使用%s", fm_carrier_name(target));
    } else if (!strcmp(g_fm_confirm_pin, pin) &&
               (int32_t)(g_fm_confirm_until - now) > 0) {
        if (fm_switch_start(pin, now) == 0)
            snprintf(g_toast, sizeof g_toast, "正在切换到%s", fm_carrier_name(target));
        else
            snprintf(g_toast, sizeof g_toast, "无法启动飞猫三网切换");
    } else {
        snprintf(g_fm_confirm_pin, sizeof g_fm_confirm_pin, "%s", pin);
        g_fm_confirm_until = now + 4000U;
        snprintf(g_toast, sizeof g_toast, "请再次点击确认%s", fm_carrier_name(target));
    }
    g_toast_until = now + 1800;
}

static const char *fm_button_class(const char *pin, uint32_t now)
{
    const char *carrier = fm_pin_carrier(pin);
    if (!carrier || !g_fm_available) return "disabled";
    if (g_fm_switching)
        return !strcmp(g_fm_switch_pin, pin) ? "sim-busy" : "disabled";
    if (!strcmp(g_fm_confirm_pin, pin) && (int32_t)(g_fm_confirm_until - now) > 0)
        return "sim-armed";
    return !strcmp(g_fm_carrier, carrier) ? "seg-on" : "";
}

static const char *fm_button_label(const char *pin, uint32_t now)
{
    const char *carrier = fm_pin_carrier(pin);
    if (g_fm_switching && !strcmp(g_fm_switch_pin, pin)) return "切换中";
    if (!strcmp(g_fm_confirm_pin, pin) && (int32_t)(g_fm_confirm_until - now) > 0)
        return "再次确认";
    return carrier ? fm_carrier_name(carrier) : "未知";
}

static void refresh_tailscale_status(void)
{
    const struct plugin_candidate *p = plugin_complete_select(g_ts_candidates, ARRAY_LEN(g_ts_candidates));
    FILE *fp;
    char line[512], cmd[2048];

    g_ts_installed = g_ts_running = g_ts_connected = g_ts_boot = 0;
    snprintf(g_ts_pid, sizeof g_ts_pid, "-");
    snprintf(g_ts_ip, sizeof g_ts_ip, "-");
    snprintf(g_ts_version, sizeof g_ts_version, "-");
    snprintf(g_ts_host, sizeof g_ts_host, "-");
    snprintf(g_ts_routes, sizeof g_ts_routes, "-");
    if (!p) return;
    g_ts_installed = 1;
    snprintf(cmd, sizeof cmd,
             "tpid=$(pidof tailscaled 2>/dev/null | awk '{print $1}'); echo TS_PID=${tpid:--};"
             "tip=$(ip -4 addr show dev tailscale0 2>/dev/null | awk '/inet /{sub(/\\/.*/,\"\",$2);print $2;exit}'); echo TS_IP=${tip:--};"
             "echo TS_VER=$(sed -n '1{s/[[:space:]].*//;p;q}' '%s/version.txt' 2>/dev/null);"
             "echo TS_HOST=$(sed -n 's/.*\"hostname\":\"\\([^\"]*\\)\".*/\\1/p' '%s/config.json' 2>/dev/null);"
             "echo TS_ROUTES=$(sed -n 's/.*\"advertise_routes\":\"\\([^\"]*\\)\".*/\\1/p' '%s/config.json' 2>/dev/null);"
             "grep -q '\"auto_start\":true' '%s/config.json' 2>/dev/null && echo TS_BOOT=1 || echo TS_BOOT=0;",
             p->dir, p->dir, p->dir, p->dir);
    fp = popen(cmd, "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        if      (!strncmp(line, "TS_PID=", 7))    line_value(g_ts_pid, sizeof g_ts_pid, line, 7);
        else if (!strncmp(line, "TS_IP=", 6))     line_value(g_ts_ip, sizeof g_ts_ip, line, 6);
        else if (!strncmp(line, "TS_VER=", 7))    line_value(g_ts_version, sizeof g_ts_version, line, 7);
        else if (!strncmp(line, "TS_HOST=", 8))   line_value(g_ts_host, sizeof g_ts_host, line, 8);
        else if (!strncmp(line, "TS_ROUTES=", 10)) line_value(g_ts_routes, sizeof g_ts_routes, line, 10);
        else if (!strncmp(line, "TS_BOOT=", 8))   g_ts_boot = atoi(line + 8);
    }
    pclose(fp);
    g_ts_running = strcmp(g_ts_pid, "-") != 0;
    g_ts_connected = g_ts_running && strcmp(g_ts_ip, "-") != 0;
}

static void refresh_mihomo_status(void)
{
    const struct plugin_candidate *p = plugin_complete_select(g_mh_candidates, ARRAY_LEN(g_mh_candidates));
    FILE *fp;
    char line[512], cmd[2048];

    g_mh_installed = g_mh_running = g_mh_tun = g_mh_rules = 0;
    snprintf(g_mh_pid, sizeof g_mh_pid, "-");
    snprintf(g_mh_version, sizeof g_mh_version, "-");
    snprintf(g_mh_mode, sizeof g_mh_mode, "-");
    snprintf(g_mh_port, sizeof g_mh_port, "-");
    snprintf(g_mh_ipset, sizeof g_mh_ipset, "-");
    if (!p) return;
    g_mh_installed = 1;
    snprintf(cmd, sizeof cmd,
             "mpid=$(pidof mihomo 2>/dev/null | awk '{print $1}'); echo MH_PID=${mpid:--};"
             "[ -d /sys/class/net/utun ] && echo MH_TUN=1 || echo MH_TUN=0;"
             "ip rule show 2>/dev/null | grep -q 'lookup 2022' && echo MH_RULES=1 || echo MH_RULES=0;"
             "echo MH_VER=$('%s' -v 2>/dev/null | awk 'NR==1{print $3;exit}');"
             "echo MH_MODE=$(sed -n 's/^mode:[[:space:]]*//p' '%s/config.yaml' 2>/dev/null | head -1);"
             "echo MH_PORT=$(sed -n 's/^mixed-port:[[:space:]]*//p' '%s/config.yaml' 2>/dev/null | head -1);"
             "echo MH_IPSET=$(ipset list chnroute 2>/dev/null | awk -F': ' '/Number of entries/{print $2;exit}');",
             p->bin, p->dir, p->dir);
    fp = popen(cmd, "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        if      (!strncmp(line, "MH_PID=", 7))    line_value(g_mh_pid, sizeof g_mh_pid, line, 7);
        else if (!strncmp(line, "MH_TUN=", 7))    g_mh_tun = atoi(line + 7);
        else if (!strncmp(line, "MH_RULES=", 9))  g_mh_rules = atoi(line + 9);
        else if (!strncmp(line, "MH_VER=", 7))    line_value(g_mh_version, sizeof g_mh_version, line, 7);
        else if (!strncmp(line, "MH_MODE=", 8))   line_value(g_mh_mode, sizeof g_mh_mode, line, 8);
        else if (!strncmp(line, "MH_PORT=", 8))   line_value(g_mh_port, sizeof g_mh_port, line, 8);
        else if (!strncmp(line, "MH_IPSET=", 9))  line_value(g_mh_ipset, sizeof g_mh_ipset, line, 9);
    }
    pclose(fp);
    g_mh_running = strcmp(g_mh_pid, "-") != 0;
}

static void refresh_cpu_status(void)
{
    const char *ctl = cpu_ctl_path();
    FILE *fp;
    char line[256], cmd[512];

    g_cpu_installed = 0;
    snprintf(g_cpu_mode, sizeof g_cpu_mode, "unknown");
    snprintf(g_cpu_gov, sizeof g_cpu_gov, "-");
    snprintf(g_cpu_cur, sizeof g_cpu_cur, "-");
    snprintf(g_cpu_min, sizeof g_cpu_min, "-");
    snprintf(g_cpu_max, sizeof g_cpu_max, "-");
    if (!cpu_control_available()) return;
    snprintf(cmd, sizeof cmd, "sh '%s' status 2>/dev/null", ctl);
    fp = popen(cmd, "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        if      (!strncmp(line, "CPU_INST=", 9)) g_cpu_installed = atoi(line + 9);
        else if (!strncmp(line, "CPU_MODE=", 9)) line_value(g_cpu_mode, sizeof g_cpu_mode, line, 9);
        else if (!strncmp(line, "CPU_GOV=", 8))  line_value(g_cpu_gov, sizeof g_cpu_gov, line, 8);
        else if (!strncmp(line, "CPU_CUR=", 8))  line_value(g_cpu_cur, sizeof g_cpu_cur, line, 8);
        else if (!strncmp(line, "CPU_MIN=", 8))  line_value(g_cpu_min, sizeof g_cpu_min, line, 8);
        else if (!strncmp(line, "CPU_MAX=", 8))  line_value(g_cpu_max, sizeof g_cpu_max, line, 8);
    }
    pclose(fp);
}

static void wg_peer_add(const char *pub, const char *endpoint, const char *allowed,
                        long long handshake, unsigned long long rx, unsigned long long tx)
{
    int idx = g_wg_peer_total++;
    if (handshake > 0 && time(NULL) - handshake <= 180) g_wg_peer_active++;
    if (idx >= WG_MAX_PEERS) return;
    struct wg_peer_state *peer = &g_wg_peers[g_wg_peer_n++];
    snprintf(peer->public_key, sizeof peer->public_key, "%s", pub && *pub ? pub : "-");
    snprintf(peer->endpoint, sizeof peer->endpoint, "%s", endpoint && *endpoint ? endpoint : "-");
    snprintf(peer->allowed_ips, sizeof peer->allowed_ips, "%s", allowed && *allowed ? allowed : "-");
    peer->latest_handshake = handshake;
    peer->rx_bytes = rx;
    peer->tx_bytes = tx;
}

static void wg_load_config_peers(const char *dir)
{
    char path[320], line[512], pub[96] = "", endpoint[128] = "", allowed[256] = "";
    int in_peer = 0;
    FILE *fp;
    snprintf(path, sizeof path, "%s/wg-ufi0.conf", dir);
    fp = fopen(path, "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        char *s = trim_text(line), *eq;
        if (*s == '[') {
            if (in_peer && pub[0]) wg_peer_add(pub, endpoint, allowed, 0, 0, 0);
            in_peer = !strcasecmp(s, "[Peer]");
            pub[0] = endpoint[0] = allowed[0] = 0;
            continue;
        }
        if (!in_peer || !(eq = strchr(s, '='))) continue;
        *eq++ = 0;
        char *key = trim_text(s), *value = trim_text(eq);
        if (!strcasecmp(key, "PublicKey")) snprintf(pub, sizeof pub, "%s", value);
        else if (!strcasecmp(key, "Endpoint")) snprintf(endpoint, sizeof endpoint, "%s", value);
        else if (!strcasecmp(key, "AllowedIPs")) snprintf(allowed, sizeof allowed, "%s", value);
    }
    if (in_peer && pub[0]) wg_peer_add(pub, endpoint, allowed, 0, 0, 0);
    fclose(fp);
}

static void wg_load_running_peers(const struct plugin_candidate *p)
{
    FILE *fp;
    char cmd[512], line[1024];
    int first = 1;
    snprintf(cmd, sizeof cmd, "'%s' show '%s' dump 2>/dev/null", p->bin, g_wg_iface);
    fp = popen(cmd, "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        char *save = NULL, *cols[8] = {0};
        int n = 0;
        if (first) { first = 0; continue; }
        for (char *v = strtok_r(line, "\t\r\n", &save); v && n < 8; v = strtok_r(NULL, "\t\r\n", &save)) cols[n++] = v;
        if (n < 7) continue;
        wg_peer_add(cols[0], cols[2], cols[3], atoll(cols[4]), strtoull(cols[5], NULL, 10), strtoull(cols[6], NULL, 10));
    }
    pclose(fp);
}

static void refresh_wireguard_status(void)
{
    const struct plugin_candidate *p = plugin_complete_select(g_wg_candidates, ARRAY_LEN(g_wg_candidates));
    FILE *fp;
    char line[512], cmd[512];
    int have_wg = 0, have_kmod = 0;

    g_wg_installed = g_wg_running = g_wg_deps = g_wg_boot = 0;
    g_wg_peer_n = g_wg_peer_total = g_wg_peer_active = 0;
    memset(g_wg_peers, 0, sizeof g_wg_peers);
    snprintf(g_wg_iface, sizeof g_wg_iface, "wg-ufi0");
    snprintf(g_wg_address, sizeof g_wg_address, "-");
    snprintf(g_wg_port, sizeof g_wg_port, "-");
    snprintf(g_wg_mode, sizeof g_wg_mode, "stopped");
    snprintf(g_wg_uptime, sizeof g_wg_uptime, "-");
    if (!p) return;
    g_wg_installed = 1;
    snprintf(cmd, sizeof cmd, "sh '%s' status 2>/dev/null", p->ctl);
    fp = popen(cmd, "r");
    if (fp) {
        while (fgets(line, sizeof line, fp)) {
            if      (!strncmp(line, "wg=", 3))           have_wg = !strncmp(line + 3, "true", 4);
            else if (!strncmp(line, "kmod=", 5))         have_kmod = !strncmp(line + 5, "true", 4);
            else if (!strncmp(line, "running=", 8))      g_wg_running = !strncmp(line + 8, "true", 4);
            else if (!strncmp(line, "interface=", 10))   line_value(g_wg_iface, sizeof g_wg_iface, line, 10);
            else if (!strncmp(line, "listen_port=", 12)) line_value(g_wg_port, sizeof g_wg_port, line, 12);
            else if (!strncmp(line, "addresses=", 10))   line_value(g_wg_address, sizeof g_wg_address, line, 10);
            else if (!strncmp(line, "start_mode=", 11))  line_value(g_wg_mode, sizeof g_wg_mode, line, 11);
            else if (!strncmp(line, "uptime=", 7))       duration_short(g_wg_uptime, sizeof g_wg_uptime, atoll(line + 7));
            else if (!strncmp(line, "boot_enabled=", 13)) g_wg_boot = !strncmp(line + 13, "true", 4);
        }
        pclose(fp);
    }
    g_wg_deps = have_wg && have_kmod;
    if (g_wg_running && p->bin && access(p->bin, X_OK) == 0) wg_load_running_peers(p);
    else wg_load_config_peers(p->dir);
}

static void operator_env_load(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[256];
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        if      (!strncmp(line, "sim_state=", 10)) line_value(g_op_sim, sizeof g_op_sim, line, 10);
        else if (!strncmp(line, "operator=", 9))   line_value(g_op_operator, sizeof g_op_operator, line, 9);
        else if (!strncmp(line, "rat=", 4))        line_value(g_op_rat, sizeof g_op_rat, line, 4);
        else if (!strncmp(line, "cops_mode=", 10)) line_value(g_op_mode, sizeof g_op_mode, line, 10);
        else if (!strncmp(line, "registered=", 11)) g_op_registered = !strncmp(line + 11, "true", 4);
    }
    fclose(fp);
}

static int operator_quoted_field(const char **cursor, char *dst, size_t cap)
{
    const char *a = strchr(*cursor, '\"'), *b;
    size_t len;
    if (!a || !(b = strchr(a + 1, '\"'))) return 0;
    len = (size_t)(b - a - 1);
    if (len >= cap) len = cap - 1;
    memcpy(dst, a + 1, len);
    dst[len] = 0;
    *cursor = b + 1;
    return 1;
}

static void operator_candidate_add(int status, const char *plmn, const char *name)
{
    for (int i = 0; i < g_op_candidate_n; i++) {
        if (strcmp(g_op_scan[i].plmn, plmn)) continue;
        if (status == 2 || (status == 1 && g_op_scan[i].status != 2)) g_op_scan[i].status = status;
        if (!g_op_scan[i].name[0] && name && *name) snprintf(g_op_scan[i].name, sizeof g_op_scan[i].name, "%s", name);
        return;
    }
    g_op_candidate_total++;
    if (g_op_candidate_n >= OP_MAX_CANDIDATES) return;
    struct operator_candidate_state *item = &g_op_scan[g_op_candidate_n++];
    item->status = status;
    snprintf(item->plmn, sizeof item->plmn, "%s", plmn);
    snprintf(item->name, sizeof item->name, "%s", name && *name ? name : plmn);
}

static void operator_scan_load(const char *path)
{
    char *raw = read_file(path);
    const char *p;
    if (!raw) return;
    p = raw;
    while ((p = strchr(p, '('))) {
        char *end = NULL, long_name[80] = "", short_name[80] = "", plmn[8] = "";
        long status = strtol(p + 1, &end, 10);
        const char *q = end;
        if (end == p + 1 || status < 0 || status > 3 ||
            !operator_quoted_field(&q, long_name, sizeof long_name) ||
            !operator_quoted_field(&q, short_name, sizeof short_name) ||
            !operator_quoted_field(&q, plmn, sizeof plmn)) {
            p++;
            continue;
        }
        if ((strlen(plmn) == 5 || strlen(plmn) == 6) && strspn(plmn, "0123456789") == strlen(plmn))
            operator_candidate_add((int)status, plmn, long_name[0] ? long_name : short_name);
        p = q;
    }
    free(raw);
}

static int operator_rat_valid(const char *value)
{
    return value && (!strcmp(value, "auto") || !strcmp(value, "wcdma") ||
                     !strcmp(value, "lte") || !strcmp(value, "nr_sa"));
}

static int operator_policy_valid(const char *value)
{
    return value && (!strcmp(value, "stay_offline") || !strcmp(value, "restore_auto"));
}

static void refresh_operator_status(void)
{
    const struct plugin_candidate *p = operator_complete_select();
    char path[320], tmp[192];
    char *json;

    g_op_installed = g_op_registered = g_op_job_running = g_op_at_busy = 0;
    g_op_candidate_n = g_op_candidate_total = 0;
    memset(g_op_scan, 0, sizeof g_op_scan);
    snprintf(g_op_sim, sizeof g_op_sim, "-");
    snprintf(g_op_operator, sizeof g_op_operator, "-");
    snprintf(g_op_rat, sizeof g_op_rat, "-");
    snprintf(g_op_mode, sizeof g_op_mode, "-");
    snprintf(g_op_job_status, sizeof g_op_job_status, "idle");
    snprintf(g_op_job_message, sizeof g_op_job_message, "-");
    snprintf(g_op_rat_pref, sizeof g_op_rat_pref, "auto");
    snprintf(g_op_failure_policy, sizeof g_op_failure_policy, "stay_offline");
    if (!p) return;
    g_op_installed = 1;
    snprintf(path, sizeof path, "%s/status.env", p->dir);
    operator_env_load(path);
    snprintf(path, sizeof path, "%s/config.json", p->dir);
    json = read_file(path);
    if (json) {
        if (json_get(json, "rat", tmp, sizeof tmp) && operator_rat_valid(tmp))
            snprintf(g_op_rat_pref, sizeof g_op_rat_pref, "%s", tmp);
        if (json_get(json, "failure_policy", tmp, sizeof tmp) && operator_policy_valid(tmp))
            snprintf(g_op_failure_policy, sizeof g_op_failure_policy, "%s", tmp);
        if (!g_op_selected[0] && json_get(json, "target_plmn", tmp, sizeof tmp) && (strlen(tmp) == 5 || strlen(tmp) == 6))
            snprintf(g_op_selected, sizeof g_op_selected, "%s", tmp);
        free(json);
    }
    snprintf(path, sizeof path, "%s/job.json", p->dir);
    json = read_file(path);
    if (json) {
        if (json_get(json, "status", tmp, sizeof tmp) && tmp[0]) snprintf(g_op_job_status, sizeof g_op_job_status, "%s", tmp);
        if (json_get(json, "message", tmp, sizeof tmp) && tmp[0]) snprintf(g_op_job_message, sizeof g_op_job_message, "%s", tmp);
        g_op_job_running = !strcmp(g_op_job_status, "queued") || !strcmp(g_op_job_status, "running");
        free(json);
    }
    g_op_at_busy = g_op_job_running;
    snprintf(path, sizeof path, "%s/scan.raw", p->dir);
    operator_scan_load(path);
}

static void plugin_status_refresh(const char *path, int force)
{
    uint32_t now = millis();
    uint32_t interval = plugin_page_named(path, "operator-lock.html") && !g_op_job_running ? 10000 : 2000;

    if (!plugin_status_page(path)) return;
    if (!force && g_plugin_status_at && now - g_plugin_status_at < interval) return;
    g_plugin_status_at = now;
    if (plugin_page_named(path, "tailscale.html")) refresh_tailscale_status();
    else if (plugin_page_named(path, "clash.html") || plugin_page_named(path, "mihomo.html")) refresh_mihomo_status();
    else if (plugin_page_named(path, "cpu-performance.html")) refresh_cpu_status();
    else if (plugin_page_named(path, "wireguard.html")) refresh_wireguard_status();
    else if (plugin_page_named(path, "operator-lock.html")) refresh_operator_status();
    else if (plugin_page_named(path, "fmswitch.html"))
        (void)refresh_fm_status(now, force);
    else if (plugin_page_named(path, "sim-switch.html") ||
             plugin_page_named(path, "sim-traffic.html"))
        (void)sim_slot_refresh(now, force);
}

/* ---- screen lock (PIN) persistence. The PIN lives in a dotfile under the UI
 * dir (which always exists) so it survives reboots and isn't clobbered by
 * pushing the .html pages. lock_enabled() == "a PIN is set". ---- */
#define LOCK_PIN_FILE UI_DIR "/.lockpin"
static int lock_enabled(void) { return g_pin[0] != 0; }
static void load_pin(void)
{
    g_pin[0] = 0;
    FILE *fp = fopen(LOCK_PIN_FILE, "r");
    if (!fp) return;
    char b[16]; int o = 0;
    if (fgets(b, sizeof b, fp))
        for (const char *p = b; *p && o < 4; p++)
            if (*p >= '0' && *p <= '9') g_pin[o++] = *p;
    g_pin[o] = 0;
    if (o != 4) g_pin[0] = 0;   /* ignore anything malformed */
    fclose(fp);
    g_refresh_ms = normalize_refresh_ms(g_refresh_ms);
}
static void save_pin(const char *pin)
{
    snprintf(g_pin, sizeof g_pin, "%s", pin);
    FILE *fp = fopen(LOCK_PIN_FILE, "w");
    if (fp) { fprintf(fp, "%s\n", pin); fclose(fp); }
}
static void clear_pin(void) { g_pin[0] = 0; remove(LOCK_PIN_FILE); }
/* Enter the lock pad. setup=1 -> set a new PIN; setup=0 -> unlock prompt. */
static void enter_lock(int setup)
{
    g_lock_state = setup ? 2 : 1;
    g_pin_entry[0] = 0; g_lock_err = 0; g_scroll = 0;
}

/* ---- persisted UI settings (theme / speed unit / auto-off / refresh interval).
 * Stored next to the binary in /data/plugins/u60pro-devui so they survive reinstalling the
 * binary and re-pushing UI files (the PIN persists separately in .lockpin). */
#define CONF_FILE "/data/plugins/u60pro-devui/devui.conf"
static void load_conf(void)
{
    FILE *fp = fopen(CONF_FILE, "r");
    if (!fp) return;
    char line[64], sval[16]; int v;
    while (fgets(line, sizeof line, fp)) {
        if      (sscanf(line, "theme=%d", &v) == 1)      g_theme = !!v;
        else if (sscanf(line, "speed_bits=%d", &v) == 1) g_speed_bits = !!v;
        else if (sscanf(line, "show_batpct=%d", &v) == 1) g_show_batpct = !!v;
        else if (sscanf(line, "autooff=%d", &v) == 1)    g_autooff_ms = v;
        else if (sscanf(line, "refresh_ms=%d", &v) == 1) g_refresh_ms = normalize_refresh_ms(v);
        else if (sscanf(line, "chart_cpu_sec=%d", &v) == 1) g_chart_cpu_sec = normalize_chart_sec(v);
        else if (sscanf(line, "chart_mem_sec=%d", &v) == 1) g_chart_mem_sec = normalize_chart_sec(v);
        else if (sscanf(line, "chart_net_sec=%d", &v) == 1) g_chart_net_sec = normalize_chart_sec(v);
        else if (sscanf(line, "chart_batt_sec=%d", &v) == 1) g_chart_batt_sec = normalize_chart_sec(v);
        else if (sscanf(line, "sig_read=%d", &v) == 1)   g_sig_read = !!v;
        else if (sscanf(line, "sig_parse=%d", &v) == 1)  g_sig_parse = !!v;
        else if (sscanf(line, "bright=%d", &v) == 1)     g_saved_bright = v;
        else if (sscanf(line, "st_src=%15s", sval) == 1) snprintf(g_st_src, sizeof g_st_src, "%s", sval);
        else if (sscanf(line, "st_dir=%15s", sval) == 1) snprintf(g_st_dir, sizeof g_st_dir, "%s", sval);
        else if (sscanf(line, "st_dur=%d", &v) == 1)     g_st_dur = v;
    }
    fclose(fp);
}
static void save_conf(void)
{
    FILE *fp = fopen(CONF_FILE, "w");
    if (!fp) return;
    fprintf(fp,
            "theme=%d\nspeed_bits=%d\nshow_batpct=%d\nautooff=%d\nrefresh_ms=%d\n"
            "chart_cpu_sec=%d\nchart_mem_sec=%d\nchart_net_sec=%d\nchart_batt_sec=%d\n"
            "sig_read=%d\nsig_parse=%d\nbright=%d\nst_src=%s\nst_dir=%s\nst_dur=%d\n",
            g_theme, g_speed_bits, g_show_batpct, g_autooff_ms, g_refresh_ms,
            g_chart_cpu_sec, g_chart_mem_sec, g_chart_net_sec, g_chart_batt_sec,
            g_sig_read, g_sig_parse, backlight_get(), g_st_src, g_st_dir, g_st_dur);
    fclose(fp);
}

static const char *chart_intervals_html(void)
{
    static char buf[4096];
    static const int secs[] = { 30, 48, 60, 120, 300 };
    static const char *labels[] = { "30s", "48s", "1min", "2min", "5min" };
    static const struct { const char *id, *label; int *value; } rows[] = {
        { "cpu", "CPU", &g_chart_cpu_sec },
        { "mem", "内存", &g_chart_mem_sec },
        { "net", "网速", &g_chart_net_sec },
        { "batt", "电池", &g_chart_batt_sec }
    };
    int o = 0;

    o += snprintf(buf + o, sizeof buf - (size_t)o,
                  "<div class='card chart-settings'><div class='ctitle'>显示区间</div>");
    for (size_t row = 0; row < sizeof rows / sizeof rows[0]; row++) {
        o += snprintf(buf + o, sizeof buf - (size_t)o,
                      "<div class='chart-range-row'><span class='chart-range-label'>%s</span>"
                      "<span class='chart-range-seg'>", rows[row].label);
        for (size_t k = 0; k < sizeof secs / sizeof secs[0]; k++)
            o += snprintf(buf + o, sizeof buf - (size_t)o,
                          "<a href='act:chartsec:%s:%d' class='chart-range-cell%s'>%s</a>",
                          rows[row].id, secs[k], *rows[row].value == secs[k] ? " chart-range-on" : "", labels[k]);
        o += snprintf(buf + o, sizeof buf - (size_t)o, "</span></div>");
    }
    snprintf(buf + o, sizeof buf - (size_t)o, "</div>");
    return buf;
}

static const char *speedtest_norm_src(const char *src)
{
    if (!strcmp(src, "cnspeed")) return "cnspeed";
    if (!strcmp(src, "ookla")) return "ookla";
    if (!strcmp(src, "cdn")) return "cdn";
    return "auto";
}

static const char *speedtest_norm_dir(const char *dir)
{
    if (!strcmp(dir, "dl")) return "dl";
    if (!strcmp(dir, "ul")) return "ul";
    return "both";
}

static int speedtest_norm_dur(int sec)
{
    if (sec == 0) return 0;   /* 0 = loop until stopped by the user */
    if (sec == 10 || sec == 15 || sec == 20) return sec;
    return 15;
}

static int speedtest_loop_mode(void)
{
    return g_st_dur == 0;
}

static void speedtest_apply_saved_prefs(void)
{
    snprintf(g_st_src, sizeof g_st_src, "%s", speedtest_norm_src(g_st_src));
    snprintf(g_st_dir, sizeof g_st_dir, "%s", speedtest_norm_dir(g_st_dir));
    g_st_dur = speedtest_norm_dur(g_st_dur);
}

static int speedtest_binary_ready(void)
{
    return access(SPEEDTEST_BIN, X_OK) == 0;
}

static int speedtest_running(void)
{
    return system("pidof better-speedtest >/dev/null 2>&1") == 0;
}

static int speedtest_loop_running(void)
{
    long pid = 0;
    if (access(SPEEDTEST_LOOP_FLAG, F_OK) != 0) return 0;
    if (read_long_path(SPEEDTEST_LOOP_PID, &pid) && pid > 0 && kill((pid_t)pid, 0) == 0)
        return 1;
    return speedtest_running();
}

static void speedtest_clear_data(void)
{
    g_st_have_result = 0;
    g_st_phase[0] = 0;
    g_st_msg[0] = 0;
    g_st_err[0] = 0;
    g_st_node[0] = 0;
    g_st_source[0] = 0;
    g_st_carrier[0] = 0;
    g_st_city[0] = 0;
    g_st_ulmsg[0] = 0;
    g_st_cur_mbps = g_st_cur_peak = 0;
    g_st_dl_avg = g_st_dl_peak = 0;
    g_st_ul_avg = g_st_ul_peak = 0;
    g_st_ping = g_st_jitter = 0;
}

static void speedtest_hist_clear(void)
{
    g_st_dl_n = 0;
    g_st_ul_n = 0;
}

static void speedtest_hist_push(int *hist, int *n, double mbps)
{
    int v;
    if (mbps < 0.0) mbps = 0.0;
    v = (int)(mbps * 10.0 + 0.5);
    if (*n < ST_HIST) {
        hist[(*n)++] = v;
    } else {
        memmove(hist, hist + 1, (ST_HIST - 1) * sizeof hist[0]);
        hist[ST_HIST - 1] = v;
    }
}

static int speedtest_json_str(const char *line, const char *key, char *out, size_t cap)
{
    char needle[48];
    const char *p;
    size_t o = 0;
    snprintf(needle, sizeof needle, "\"%s\":", key);
    p = strstr(line, needle);
    if (!p || cap == 0) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = 0;
    return 1;
}

static int speedtest_json_num(const char *line, const char *key, double *out)
{
    char needle[48];
    char *end = NULL;
    const char *p;
    snprintf(needle, sizeof needle, "\"%s\":", key);
    p = strstr(line, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    *out = strtod(p, &end);
    return end && end != p;
}

static const char *speedtest_phase_label(const char *phase)
{
    if (!strcmp(phase, "download")) return "下行测速";
    if (!strcmp(phase, "upload")) return "上行测速";
    if (!strcmp(phase, "start")) return "准备测速";
    if (!strcmp(phase, "geo")) return "定位中";
    if (!strcmp(phase, "node")) return "选择节点";
    if (!strcmp(phase, "probe")) return "探测中";
    if (!strcmp(phase, "result")) return "测速完成";
    if (!strcmp(phase, "error")) return "测速失败";
    return "测速中";
}

static void speedtest_parse_line(const char *line)
{
    char phase[24];
    if (!line || !line[0] || !strcmp(line, "DONE")) return;
    if (!speedtest_json_str(line, "phase", phase, sizeof phase)) return;
    snprintf(g_st_phase, sizeof g_st_phase, "%s", phase);
    if (!strcmp(phase, "download") || !strcmp(phase, "upload")) {
        (void)speedtest_json_num(line, "mbps", &g_st_cur_mbps);
        (void)speedtest_json_num(line, "peak", &g_st_cur_peak);
        if (!strcmp(phase, "download"))
            speedtest_hist_push(g_st_dl_hist, &g_st_dl_n, g_st_cur_mbps);
        else
            speedtest_hist_push(g_st_ul_hist, &g_st_ul_n, g_st_cur_mbps);
        g_st_state = ST_STATE_RUNNING;
        return;
    }
    if (!strcmp(phase, "result")) {
        g_st_have_result = 1;
        (void)speedtest_json_str(line, "node", g_st_node, sizeof g_st_node);
        (void)speedtest_json_str(line, "source", g_st_source, sizeof g_st_source);
        (void)speedtest_json_str(line, "carrier", g_st_carrier, sizeof g_st_carrier);
        (void)speedtest_json_str(line, "city", g_st_city, sizeof g_st_city);
        (void)speedtest_json_str(line, "ul_msg", g_st_ulmsg, sizeof g_st_ulmsg);
        (void)speedtest_json_num(line, "dl_avg", &g_st_dl_avg);
        (void)speedtest_json_num(line, "dl_peak", &g_st_dl_peak);
        (void)speedtest_json_num(line, "ul_avg", &g_st_ul_avg);
        (void)speedtest_json_num(line, "ul_peak", &g_st_ul_peak);
        (void)speedtest_json_num(line, "ping", &g_st_ping);
        (void)speedtest_json_num(line, "jitter", &g_st_jitter);
        g_st_state = ST_STATE_DONE;
        return;
    }
    if (speedtest_json_str(line, "msg", g_st_msg, sizeof g_st_msg)) {
        if (!strcmp(phase, "error")) {
            snprintf(g_st_err, sizeof g_st_err, "Test failed");
            g_st_state = ST_STATE_FAIL;
        } else {
            g_st_state = ST_STATE_RUNNING;
        }
    }
}

static int speedtest_poll(uint32_t now_ms)
{
    int prev_state = g_st_state, prev_installed = g_st_installed, prev_have_result = g_st_have_result;
    double prev_cur = g_st_cur_mbps, prev_peak = g_st_cur_peak, prev_dl = g_st_dl_avg, prev_ul = g_st_ul_avg;
    double prev_ping = g_st_ping, prev_jitter = g_st_jitter;
    char prev_phase[24], prev_msg[160], prev_err[160], prev_node[96], prev_source[32], prev_ulmsg[96];
    snprintf(prev_phase, sizeof prev_phase, "%s", g_st_phase);
    snprintf(prev_msg, sizeof prev_msg, "%s", g_st_msg);
    snprintf(prev_err, sizeof prev_err, "%s", g_st_err);
    snprintf(prev_node, sizeof prev_node, "%s", g_st_node);
    snprintf(prev_source, sizeof prev_source, "%s", g_st_source);
    snprintf(prev_ulmsg, sizeof prev_ulmsg, "%s", g_st_ulmsg);

    g_st_installed = speedtest_binary_ready();
    if (g_st_installed < 0) g_st_installed = 0;
    {
        int running = speedtest_running();
        int loop_running = speedtest_loop_running();
        char *log = read_file(SPEEDTEST_LOG);
        if (loop_running) running = 1;
        if (log && log[0]) {
            char *save = NULL;
            speedtest_clear_data();
            speedtest_hist_clear();
            for (char *line = strtok_r(log, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
                speedtest_parse_line(line);
            if (running) {
                if (!g_st_have_result && g_st_state != ST_STATE_FAIL)
                    g_st_state = ST_STATE_RUNNING;
                if (loop_running && g_st_have_result)
                    g_st_state = ST_STATE_RUNNING;
            } else if (g_st_have_result) {
                g_st_state = ST_STATE_DONE;
            } else if (g_st_err[0] || g_st_phase[0] || g_st_msg[0]) {
                if (!g_st_err[0]) snprintf(g_st_err, sizeof g_st_err, "测速结束但没有结果");
                g_st_state = ST_STATE_FAIL;
            } else {
                g_st_state = g_st_installed ? ST_STATE_IDLE : ST_STATE_MISSING;
            }
        } else if (!running) {
            if (!g_st_installed) g_st_state = ST_STATE_MISSING;
            else if (g_st_state == ST_STATE_RUNNING) g_st_state = ST_STATE_IDLE;
            else if (g_st_state == ST_STATE_MISSING) g_st_state = ST_STATE_IDLE;
        } else {
            g_st_state = ST_STATE_RUNNING;
            if (!g_st_msg[0] && !g_st_phase[0]) snprintf(g_st_msg, sizeof g_st_msg, "测速中");
        }
        free(log);
    }
    g_st_poll_at = now_ms;
    return prev_state != g_st_state || prev_installed != g_st_installed || prev_have_result != g_st_have_result ||
           strcmp(prev_phase, g_st_phase) || strcmp(prev_msg, g_st_msg) || strcmp(prev_err, g_st_err) ||
           strcmp(prev_node, g_st_node) || strcmp(prev_source, g_st_source) || strcmp(prev_ulmsg, g_st_ulmsg) ||
           fabs(prev_cur - g_st_cur_mbps) > 0.04 || fabs(prev_peak - g_st_cur_peak) > 0.04 ||
           fabs(prev_dl - g_st_dl_avg) > 0.04 || fabs(prev_ul - g_st_ul_avg) > 0.04 ||
           fabs(prev_ping - g_st_ping) > 0.04 || fabs(prev_jitter - g_st_jitter) > 0.04;
}

static void speedtest_start(void)
{
    char cmd[768], extra[32] = "";
    speedtest_apply_saved_prefs();
    g_st_installed = speedtest_binary_ready();
    if (!g_st_installed) {
        speedtest_clear_data();
        speedtest_hist_clear();
        snprintf(g_st_err, sizeof g_st_err, "未安装测速器");
        g_st_state = ST_STATE_MISSING;
        return;
    }
    if (!strcmp(g_st_dir, "dl")) snprintf(extra, sizeof extra, " --no-upload");
    else if (!strcmp(g_st_dir, "ul")) snprintf(extra, sizeof extra, " --no-download");
    speedtest_clear_data();
    speedtest_hist_clear();
    snprintf(g_st_msg, sizeof g_st_msg, "正在启动");
    g_st_state = ST_STATE_RUNNING;
    if (speedtest_loop_mode()) {
        snprintf(cmd, sizeof cmd,
                 "rm -f " SPEEDTEST_LOG " " SPEEDTEST_LOOP_PID "; : > " SPEEDTEST_LOG "; touch " SPEEDTEST_LOOP_FLAG "; "
                 "(while [ -f " SPEEDTEST_LOOP_FLAG " ]; do "
                 ": > " SPEEDTEST_LOG "; "
                 "echo '{\"phase\":\"start\",\"msg\":\"循环测速中，手动停止前不会自动结束\"}' >> " SPEEDTEST_LOG "; "
                 SPEEDTEST_BIN " test --json --src %s --dur 15%s >> " SPEEDTEST_LOG " 2>&1; "
                 "sleep 1; "
                 "done) & echo $! > " SPEEDTEST_LOOP_PID,
                 g_st_src, extra);
    } else {
        unlink(SPEEDTEST_LOOP_FLAG);
        unlink(SPEEDTEST_LOOP_PID);
        snprintf(cmd, sizeof cmd,
                 "rm -f " SPEEDTEST_LOG "; : > " SPEEDTEST_LOG "; " SPEEDTEST_BIN " test --json --src %s --dur %d%s > " SPEEDTEST_LOG " 2>&1 &",
                 g_st_src, g_st_dur, extra);
    }
    if (system(cmd) != 0) {
        speedtest_hist_clear();
        snprintf(g_st_err, sizeof g_st_err, "启动失败");
        g_st_state = ST_STATE_FAIL;
    }
}

static void speedtest_stop(void)
{
    unlink(SPEEDTEST_LOOP_FLAG);
    system("if [ -s " SPEEDTEST_LOOP_PID " ]; then kill -9 $(cat " SPEEDTEST_LOOP_PID ") 2>/dev/null || true; fi; "
           "for p in $(pidof better-speedtest 2>/dev/null); do kill -9 $p; done");
    unlink(SPEEDTEST_LOOP_PID);
    unlink(SPEEDTEST_LOG);
    speedtest_clear_data();
    speedtest_hist_clear();
    g_st_installed = speedtest_binary_ready();
    g_st_state = g_st_installed ? ST_STATE_IDLE : ST_STATE_MISSING;
    snprintf(g_st_msg, sizeof g_st_msg, "已停止");
}

static int path_is_speedtest(const char *path)
{
    const char *base = strrchr(path, '/');
    if (!base) base = path; else base++;
    return !strcmp(base, "07-speedtest.html") || !strcmp(base, "speedtest.html") ||
           strstr(base, "-speedtest.html") != NULL;
}

static int path_is_signal_detail(const char *path)
{
    const char *base = strrchr(path, '/');
    if (!base) base = path; else base++;
    return !strcmp(base, "01a-cell.html") || strstr(base, "cell.html") != NULL;
}

static int path_is_signal_home(const char *path)
{
    const char *base = strrchr(path, '/');
    if (!base) base = path; else base++;
    return !strcmp(base, "01-signal.html");
}

static int path_is_signal_page(const char *path)
{
    return path_is_signal_home(path) || path_is_signal_detail(path);
}

static int path_is_lock_page(const char *path)
{
    const char *base;
    if (!path) return 0;
    base = strrchr(path, '/');
    if (!base) base = path; else base++;
    return !strcmp(base, "lock.html");
}

static int signal_live_enabled(void)
{
    /* Raw reads may stay on for datad, but UI decoded cards obey parse only. */
    return g_sig_parse;
}

static int speedtest_page_index(void)
{
    for (int i = 0; i < g_npages; i++)
        if (path_is_speedtest(g_pages[i])) return i;
    return -1;
}

static int subpage_name_ok(const char *name)
{
    size_t l;
    if (!name || !*name) return 0;
    if (name[0] == '.') return 0;
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return 0;
    if (strpbrk(name, "\"'<>&")) return 0;
    l = strlen(name);
    return l > 5 && strcmp(name + l - 5, ".html") == 0;
}

struct function_detection {
    int visible;
    char reason[48];
    char detail[320];
};

struct plugin_detect_cache {
    const char *name;
    int initialized;
    int visible;
    char reason[48];
    char detail[320];
};

static struct plugin_detect_cache g_plugin_detect_cache[] = {
    { "tailscale.html", 0, 0, "", "" },
    { "clash.html", 0, 0, "", "" },
    { "cpu-performance.html", 0, 0, "", "" },
    { "wireguard.html", 0, 0, "", "" },
    { "operator-lock.html", 0, 0, "", "" },
    { "sim-switch.html", 0, 0, "", "" },
    { "sim-traffic.html", 0, 0, "", "" },
    { "fmswitch.html", 0, 0, "", "" },
    { "timezone.html", 0, 0, "", "" },
};

static void function_detection_set(struct function_detection *out, int visible,
                                   const char *reason, const char *detail)
{
    if (!out) return;
    out->visible = visible;
    snprintf(out->reason, sizeof out->reason, "%s", reason ? reason : "unknown");
    snprintf(out->detail, sizeof out->detail, "%s", detail ? detail : "-");
}

static void plugin_candidate_detection(const struct plugin_candidate *items, size_t count,
                                       struct function_detection *out)
{
    char first_reason[48] = "";
    char first_detail[320] = "";
    int saw_dir = 0;

    for (size_t i = 0; i < count; i++) {
        if (access(items[i].dir, F_OK) != 0) continue;
        saw_dir = 1;
        if (access(items[i].ctl, F_OK) != 0) {
            if (!first_reason[0]) {
                snprintf(first_reason, sizeof first_reason, "controller_missing");
                snprintf(first_detail, sizeof first_detail, "%s", items[i].ctl);
            }
            continue;
        }
        if (access(items[i].ctl, R_OK) != 0) {
            if (!first_reason[0]) {
                snprintf(first_reason, sizeof first_reason, "controller_not_readable");
                snprintf(first_detail, sizeof first_detail, "%s", items[i].ctl);
            }
            continue;
        }
        if (!items[i].bin || access(items[i].bin, F_OK) != 0) {
            if (!first_reason[0]) {
                snprintf(first_reason, sizeof first_reason, "core_missing");
                snprintf(first_detail, sizeof first_detail, "%s", items[i].bin ? items[i].bin : items[i].dir);
            }
            continue;
        }
        if (access(items[i].bin, X_OK) != 0) {
            if (!first_reason[0]) {
                snprintf(first_reason, sizeof first_reason, "core_not_executable");
                snprintf(first_detail, sizeof first_detail, "%s", items[i].bin);
            }
            continue;
        }
        function_detection_set(out, 1, "available", items[i].dir);
        return;
    }
    if (!saw_dir)
        function_detection_set(out, 0, "plugin_dir_missing", count ? items[0].dir : "-");
    else
        function_detection_set(out, 0, first_reason[0] ? first_reason : "plugin_incomplete",
                               first_detail[0] ? first_detail : "-");
}

static void operator_candidate_detection(struct function_detection *out)
{
    char first_reason[48] = "";
    char first_detail[320] = "";
    int saw_dir = 0;

    for (size_t i = 0; i < ARRAY_LEN(g_operator_candidates); i++) {
        char config[320];
        const struct plugin_candidate *item = &g_operator_candidates[i];
        if (access(item->dir, F_OK) != 0) continue;
        saw_dir = 1;
        if (access(item->ctl, F_OK) != 0) {
            if (!first_reason[0]) {
                snprintf(first_reason, sizeof first_reason, "controller_missing");
                snprintf(first_detail, sizeof first_detail, "%s", item->ctl);
            }
            continue;
        }
        if (access(item->ctl, R_OK) != 0) {
            if (!first_reason[0]) {
                snprintf(first_reason, sizeof first_reason, "controller_not_readable");
                snprintf(first_detail, sizeof first_detail, "%s", item->ctl);
            }
            continue;
        }
        snprintf(config, sizeof config, "%s/config.json", item->dir);
        if (access(config, F_OK) != 0) {
            if (!first_reason[0]) {
                snprintf(first_reason, sizeof first_reason, "config_missing");
                snprintf(first_detail, sizeof first_detail, "%s", config);
            }
            continue;
        }
        if (access(config, R_OK) != 0) {
            if (!first_reason[0]) {
                snprintf(first_reason, sizeof first_reason, "config_not_readable");
                snprintf(first_detail, sizeof first_detail, "%s", config);
            }
            continue;
        }
        function_detection_set(out, 1, "available", item->dir);
        return;
    }
    if (!saw_dir)
        function_detection_set(out, 0, "plugin_dir_missing", g_operator_candidates[0].dir);
    else
        function_detection_set(out, 0, first_reason[0] ? first_reason : "plugin_incomplete",
                               first_detail[0] ? first_detail : "-");
}

static void plugin_detect_rotate_if_needed(void)
{
    FILE *fp = fopen(PLUGIN_DETECT_LOG, "r");
    char line[512];
    int lines = 0;
    if (!fp) return;
    while (lines < 200 && fgets(line, sizeof line, fp)) lines++;
    fclose(fp);
    if (lines < 200) return;
    unlink(PLUGIN_DETECT_LOG ".old");
    (void)rename(PLUGIN_DETECT_LOG, PLUGIN_DETECT_LOG ".old");
}

static void plugin_detect_record(const char *name, const struct function_detection *result,
                                 int force)
{
    struct plugin_detect_cache *cache = NULL;
    FILE *fp;
    char stamp[32];

    for (size_t i = 0; i < ARRAY_LEN(g_plugin_detect_cache); i++)
        if (!strcmp(name, g_plugin_detect_cache[i].name)) {
            cache = &g_plugin_detect_cache[i];
            break;
        }
    if (!cache || !result) return;
    if (!force && cache->initialized && cache->visible == result->visible &&
        !strcmp(cache->reason, result->reason) && !strcmp(cache->detail, result->detail))
        return;

    mkdir("/data/plugins/u60pro-devui", 0755);
    plugin_detect_rotate_if_needed();
    fp = fopen(PLUGIN_DETECT_LOG, "a");
    if (fp) {
        devui_time_stamp(stamp, sizeof stamp, time(NULL));
        fprintf(fp, "[%s] page=%s visible=%d reason=%s detail=%s\n",
                stamp, name, result->visible, result->reason, result->detail);
        fclose(fp);
    }
    cache->initialized = 1;
    cache->visible = result->visible;
    snprintf(cache->reason, sizeof cache->reason, "%s", result->reason);
    snprintf(cache->detail, sizeof cache->detail, "%s", result->detail);
}

static void function_detection_eval(const char *name, struct function_detection *out, int force_log)
{
    char page[320];
    char detail[320];
    const char *ctl;
    int status;

    function_detection_set(out, 0, "invalid_name", "-");
    if (!name || !subpage_name_ok(name)) return;
    snprintf(page, sizeof page, "%s/%s", FUNCTIONS_DIR, name);
    if (access(page, F_OK) != 0) {
        function_detection_set(out, 0, "page_missing", page);
        plugin_detect_record(name, out, force_log);
        return;
    }
    if (access(page, R_OK) != 0) {
        function_detection_set(out, 0, "page_not_readable", page);
        plugin_detect_record(name, out, force_log);
        return;
    }

    if (!strcmp(name, "tailscale.html"))
        plugin_candidate_detection(g_ts_candidates, ARRAY_LEN(g_ts_candidates), out);
    else if (!strcmp(name, "clash.html") || !strcmp(name, "mihomo.html"))
        plugin_candidate_detection(g_mh_candidates, ARRAY_LEN(g_mh_candidates), out);
    else if (!strcmp(name, "wireguard.html"))
        plugin_candidate_detection(g_wg_candidates, ARRAY_LEN(g_wg_candidates), out);
    else if (!strcmp(name, "operator-lock.html"))
        operator_candidate_detection(out);
    else if (!strcmp(name, "cpu-performance.html")) {
        ctl = cpu_ctl_path();
        if (access(ctl, F_OK) != 0)
            function_detection_set(out, 0, "controller_missing", ctl);
        else if (access(ctl, R_OK) != 0)
            function_detection_set(out, 0, "controller_not_readable", ctl);
        else if (access("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", F_OK) != 0)
            function_detection_set(out, 0, "capability_missing", "cpufreq policy0 governor");
        else if (access("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", R_OK | W_OK) != 0)
            function_detection_set(out, 0, "capability_not_writable", "cpufreq policy0 governor");
        else
            function_detection_set(out, 1, "available", ctl);
    } else if (!strcmp(name, "sim-switch.html")) {
        status = dual_sim_management_status();
        if (status < 0) function_detection_set(out, 0, "device_query_failed", "support_dual_sim unknown");
        else if (!status) function_detection_set(out, 0, "device_unsupported", "support_dual_sim=0");
        else function_detection_set(out, 1, "available", "support_dual_sim=1");
    } else if (!strcmp(name, "sim-traffic.html")) {
        status = sim_traffic_page_status();
        if (status < 0) function_detection_set(out, 0, "datad_unavailable", "GET /state failed");
        else if (!status) function_detection_set(out, 0, "capability_missing", "sim_traffic_available=0");
        else function_detection_set(out, 1, "available", "sim_traffic_available=1");
    } else if (!strcmp(name, "fmswitch.html")) {
        if (!g_fm_enabled)
            function_detection_set(out, 0, "user_disabled", "advanced setting off");
        else if (access(FM_CTL_BUNDLED, F_OK) != 0)
            function_detection_set(out, 0, "controller_missing", FM_CTL_BUNDLED);
        else if (access(FM_CTL_BUNDLED, R_OK) != 0)
            function_detection_set(out, 0, "controller_not_readable", FM_CTL_BUNDLED);
        else {
            int available = fm_card_available();
            snprintf(detail, sizeof detail, "display=enabled; source=%s; %s",
                     g_fm_detect_source, g_fm_detect_reason);
            function_detection_set(out, 1, available ? "available" : "manual_enabled", detail);
        }
    } else {
        function_detection_set(out, 1, "available", !strcmp(name, "timezone.html") ? "built-in" : "custom page");
    }
    plugin_detect_record(name, out, force_log);
}

static void plugin_detect_log_all(int force)
{
    struct function_detection result;
    for (size_t i = 0; i < ARRAY_LEN(g_plugin_detect_cache); i++)
        function_detection_eval(g_plugin_detect_cache[i].name, &result, force);
}

/* Known service pages are useful only when their auditable control adapter is
 * installed. Keep strict checks, but record the exact hidden reason. */
static int function_control_api_available(const char *name)
{
    struct function_detection result;
    if (!name) return 0;
    function_detection_eval(name, &result, 0);
    return result.visible;
}

static int subpage_open(const char *name)
{
    char path[300];
    if (!subpage_name_ok(name)) return 0;
    snprintf(path, sizeof path, "%s/subpages/%s", UI_DIR, name);
    if (access(path, R_OK) != 0) return 0;
    if (g_subpage[0] && !g_quick_open) {
        if (g_nav_depth >= NAV_DEPTH) return 0;
        struct nav_entry *prev = &g_nav_stack[g_nav_depth++];
        snprintf(prev->name, sizeof prev->name, "%s", g_subpage);
        snprintf(prev->path, sizeof prev->path, "%s", g_subpage_path);
        prev->scroll = g_scroll;
        prev->header_valid = g_pinned_header_valid;
        if (prev->header_valid)
            memcpy(prev->header, g_pinned_header, sizeof prev->header);
    }
    snprintf(g_subpage, sizeof g_subpage, "%s", name);
    snprintf(g_subpage_path, sizeof g_subpage_path, "%s", path);
    g_scroll = 0;
    g_pinned_header_valid = 0;
    return 1;
}

static int quick_panel_open(void)
{
    if (g_quick_open) return 0;
    snprintf(g_quick_previous_subpage, sizeof g_quick_previous_subpage, "%s", g_subpage);
    snprintf(g_quick_previous_path, sizeof g_quick_previous_path, "%s", g_subpage_path);
    g_quick_previous_scroll = g_scroll;
    g_quick_previous_header_valid = g_pinned_header_valid;
    if (g_pinned_header_valid)
        memcpy(g_quick_previous_header, g_pinned_header, sizeof g_pinned_header);
    char quick_path[300];
    snprintf(quick_path, sizeof quick_path, "%s/subpages/quick.html", UI_DIR);
    if (access(quick_path, R_OK) != 0) return 0;
    snprintf(g_subpage, sizeof g_subpage, "quick.html");
    snprintf(g_subpage_path, sizeof g_subpage_path, "%s", quick_path);
    g_scroll = 0;
    g_pinned_header_valid = 0;
    g_quick_open = 1;
    return 1;
}

static int function_page_open(const char *name)
{
    char path[300];
    if (!subpage_name_ok(name)) return 0;
    if (!function_control_api_available(name)) return 0;
    snprintf(path, sizeof path, "%s/%s", FUNCTIONS_DIR, name);
    if (access(path, R_OK) != 0) return 0;
    if (g_subpage[0] && !g_quick_open) {
        if (g_nav_depth >= NAV_DEPTH) return 0;
        struct nav_entry *prev = &g_nav_stack[g_nav_depth++];
        snprintf(prev->name, sizeof prev->name, "%s", g_subpage);
        snprintf(prev->path, sizeof prev->path, "%s", g_subpage_path);
        prev->scroll = g_scroll;
        prev->header_valid = g_pinned_header_valid;
        if (prev->header_valid)
            memcpy(prev->header, g_pinned_header, sizeof prev->header);
    }
    snprintf(g_subpage, sizeof g_subpage, "func:%s", name);
    snprintf(g_subpage_path, sizeof g_subpage_path, "%s", path);
    g_scroll = 0;
    g_pinned_header_valid = 0;
    return 1;
}

static void subpage_close(void)
{
    if (g_quick_open) {
        snprintf(g_subpage, sizeof g_subpage, "%s", g_quick_previous_subpage);
        snprintf(g_subpage_path, sizeof g_subpage_path, "%s", g_quick_previous_path);
        g_scroll = g_quick_previous_scroll;
        g_pinned_header_valid = g_quick_previous_header_valid;
        if (g_pinned_header_valid)
            memcpy(g_pinned_header, g_quick_previous_header, sizeof g_pinned_header);
        g_quick_open = 0;
        return;
    }
    if (g_nav_depth > 0) {
        struct nav_entry *prev = &g_nav_stack[--g_nav_depth];
        snprintf(g_subpage, sizeof g_subpage, "%s", prev->name);
        snprintf(g_subpage_path, sizeof g_subpage_path, "%s", prev->path);
        g_scroll = prev->scroll;
        g_pinned_header_valid = prev->header_valid;
        if (g_pinned_header_valid)
            memcpy(g_pinned_header, prev->header, sizeof g_pinned_header);
        return;
    }
    g_subpage[0] = 0;
    g_subpage_path[0] = 0;
    g_scroll = 0;
    g_pinned_header_valid = 0;
    g_fm_confirm_pin[0] = 0;
    g_fm_confirm_until = 0;
}

static void subpage_close_all(void)
{
    g_nav_depth = 0;
    subpage_close();
}

static const char *active_page_path(void)
{
    if (g_subpage[0]) return g_subpage_path;
    if (g_npages > 0 && g_cur >= 0 && g_cur < g_npages) return g_pages[g_cur];
    return "";
}

static int path_is_function_page(const char *path)
{
    size_t l = strlen(FUNCTIONS_DIR);
    return path && !strncmp(path, FUNCTIONS_DIR, l) && path[l] == '/';
}

static void function_label_from_name(char *dst, size_t cap, const char *name)
{
    size_t l;
    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!name) return;
    l = strlen(name);
    if (l > 5 && !strcmp(name + l - 5, ".html")) l -= 5;
    if (l >= cap) l = cap - 1;
    memcpy(dst, name, l);
    dst[l] = 0;
    for (size_t i = 0; dst[i]; i++)
        if (dst[i] == '_' || dst[i] == '-') dst[i] = ' ';
}

static int function_title_from_html(const char *html, char *dst, size_t cap)
{
    const char *p, *gt, *end;
    size_t l;
    if (!html || !dst || cap == 0) return 0;
    p = strcasestr(html, "<title");
    if (!p) return 0;
    gt = strchr(p, '>');
    if (!gt) return 0;
    gt++;
    end = strcasestr(gt, "</title>");
    if (!end || end <= gt) return 0;
    while (gt < end && (*gt == ' ' || *gt == '\t' || *gt == '\r' || *gt == '\n')) gt++;
    while (end > gt && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    l = (size_t)(end - gt);
    if (l >= cap) l = cap - 1;
    memcpy(dst, gt, l);
    dst[l] = 0;
    return dst[0] != 0;
}

static void function_title_for_file(char *dst, size_t cap, const char *path, const char *name)
{
    char *html = read_file(path);
    if (html) {
        if (function_title_from_html(html, dst, cap)) {
            free(html);
            return;
        }
        free(html);
    }
    function_label_from_name(dst, cap, name);
}

static int g_st_home_open = 0;

static const char *speedtest_home_button_html(void)
{
    if (g_lock_state || !speedtest_binary_ready()) return "";
    return g_st_home_open ?
           "<a href='act:sttoggle' class='speed-home on'>收起测速</a>" :
           "<a href='act:sttoggle' class='speed-home'>网络测速</a>";
}

static const char *speedtest_function_tile_html(void)
{
    if (!speedtest_binary_ready()) return "";
    return "<a href='act:sub:speedtest.html' class='func-tile ft-speed has-icon'>"
           "<span class='ui-ico ico-speed'><i></i><em></em><b></b></span>"
           "<span class='func-name'>网络测速</span>"
           "<span class='func-desc'>打开完整测速仪表盘</span>"
           "</a>";
}

static const char *function_tile_desc(const char *name)
{
    if (!strcmp(name, "tailscale.html")) return "组网状态与子网路由";
    if (!strcmp(name, "clash.html") || !strcmp(name, "mihomo.html"))
        return "代理状态与服务控制";
    if (!strcmp(name, "cpu-performance.html")) return "频率策略与温控状态";
    if (!strcmp(name, "wireguard.html")) return "隧道状态与 Peer";
    if (!strcmp(name, "operator-lock.html")) return "扫描并锁定运营商";
    if (!strcmp(name, "fmswitch.html")) return "飞猫分身卡三网切换";
    if (!strcmp(name, "sim-switch.html")) return "驻网模式与数据卡";
    if (!strcmp(name, "sim-traffic.html")) return "按卡流量与套餐";
    if (!strcmp(name, "timezone.html")) return "显示时区与周期";
    return "自定义工具";
}

static int append_function_tile(char *buf, size_t cap, int o, const char *name)
{
    char path[300], raw[96], title[160], href[160];
    const char *icon = "advanced";
    const char *wide = !strcmp(name, "cpu-performance.html") ? " func-tile-wide" : "";
    if (!name || o < 0 || (size_t)o >= cap) return o;
    if (!subpage_name_ok(name) || !function_control_api_available(name)) return o;
    snprintf(path, sizeof path, "%s/%s", FUNCTIONS_DIR, name);
    if (access(path, R_OK) != 0) return o;
    function_title_for_file(raw, sizeof raw, path, name);
    html_esc(title, sizeof title, raw);
    html_esc(href, sizeof href, name);
    if (!strcmp(name, "timezone.html")) icon = "clock";
    else if (!strcmp(name, "cpu-performance.html")) icon = "chip";
    else if (!strcmp(name, "operator-lock.html")) icon = "signal";
    else if (!strcmp(name, "sim-switch.html")) icon = "sim";
    return o + snprintf(buf + o, cap - (size_t)o,
                        "<a href=\"act:func:%s\" class=\"func-tile func-custom has-icon%s\">"
                        "<span class=\"ui-ico ico-%s\"><i></i><em></em><b></b></span>"
                        "<span class=\"func-name\">%s</span>"
                        "<span class=\"func-desc\">%s</span>"
                        "</a>",
                        href, wide, icon, title, function_tile_desc(name));
}

static const char *system_function_tiles_html(void)
{
    static char buf[2048];
    int o = 0;
    buf[0] = 0;
    o = append_function_tile(buf, sizeof buf, o, "sim-switch.html");
    o = append_function_tile(buf, sizeof buf, o, "sim-traffic.html");
    return buf;
}

static const char *timezone_function_tile_html(void)
{
    static char buf[1024];
    buf[0] = 0;
    (void)append_function_tile(buf, sizeof buf, 0, "timezone.html");
    return buf;
}

static const char *custom_function_tiles_html(void)
{
    static char buf[8192];
    char names[32][64];
    int n = 0;
    DIR *dp;
    struct dirent *de;
    int o = 0;

    buf[0] = 0;
    mkdir(FUNCTIONS_DIR, 0755);
    dp = opendir(FUNCTIONS_DIR);
    if (!dp) return buf;
    while ((de = readdir(dp)) && n < 32) {
        if (de->d_name[0] == '.') continue;
        if (!subpage_name_ok(de->d_name)) continue;
        if (!strcmp(de->d_name, "sim-switch.html") ||
            !strcmp(de->d_name, "sim-traffic.html") ||
            !strcmp(de->d_name, "fmswitch.html") ||
            !strcmp(de->d_name, "timezone.html")) continue;
        if (!function_control_api_available(de->d_name)) continue;
        snprintf(names[n], sizeof names[n], "%s", de->d_name);
        n++;
    }
    closedir(dp);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[64]; strcpy(t, names[i]); strcpy(names[i], names[j]); strcpy(names[j], t);
    }

    for (int i = 0; i < n; i++) {
        o = append_function_tile(buf, sizeof buf, o, names[i]);
        if (o >= (int)sizeof(buf) - 256) break;
    }
    return buf;
}

static void bytes_short(char *dst, size_t cap, unsigned long long n)
{
    if (n >= 1024ULL * 1024ULL * 1024ULL) snprintf(dst, cap, "%.1f GB", (double)n / (1024.0 * 1024.0 * 1024.0));
    else if (n >= 1024ULL * 1024ULL) snprintf(dst, cap, "%.1f MB", (double)n / (1024.0 * 1024.0));
    else if (n >= 1024ULL) snprintf(dst, cap, "%.1f KB", (double)n / 1024.0);
    else snprintf(dst, cap, "%llu B", n);
}

static const char *wireguard_peer_html(void)
{
    static char buf[14000];
    int o = 0;
    time_t now = time(NULL);
    buf[0] = 0;
    if (!g_wg_peer_n) {
        snprintf(buf, sizeof buf, "<div class='compact-empty'>暂无 Peer 配置</div>");
        return buf;
    }
    for (int i = 0; i < g_wg_peer_n && o < (int)sizeof(buf) - 600; i++) {
        struct wg_peer_state *p = &g_wg_peers[i];
        char key[24], endpoint[240], allowed[420], rx[32], tx[32], age[48];
        const char *state = "未连接", *cls = "muted";
        snprintf(key, sizeof key, "%.12s...", p->public_key);
        html_esc(endpoint, sizeof endpoint, p->endpoint);
        html_esc(allowed, sizeof allowed, p->allowed_ips);
        bytes_short(rx, sizeof rx, p->rx_bytes);
        bytes_short(tx, sizeof tx, p->tx_bytes);
        if (p->latest_handshake > 0) {
            long long sec = (long long)now - p->latest_handshake;
            if (sec < 0) sec = 0;
            duration_short(age, sizeof age, sec);
            if (sec <= 180) { state = "活跃"; cls = "ok"; }
            else { state = "过期"; cls = "warn"; }
        } else snprintf(age, sizeof age, "从未握手");
        o += snprintf(buf + o, sizeof buf - (size_t)o,
                      "<div class='peer-row'>"
                      "<div class='peer-head'><span class='peer-key'>%s</span><span class='peer-state %s'>%s</span></div>"
                      "<div class='peer-detail'>%s · %s</div>"
                      "<div class='peer-meta'>%s · RX %s · TX %s</div>"
                      "</div>", key, cls, state, endpoint, allowed, age, rx, tx);
    }
    if (g_wg_peer_total > g_wg_peer_n)
        snprintf(buf + o, sizeof buf - (size_t)o,
                 "<div class='compact-more'>另有 %d 个 Peer，请在网页端查看</div>",
                 g_wg_peer_total - g_wg_peer_n);
    return buf;
}

static const char *operator_list_html(void)
{
    static char buf[15000];
    int o = 0;
    if (!g_op_candidate_n) {
        snprintf(buf, sizeof buf, "<div class='compact-empty'>暂无扫描结果</div>");
        return buf;
    }
    if (!g_op_selected[0]) {
        for (int i = 0; i < g_op_candidate_n; i++)
            if (g_op_scan[i].status == 2) {
                snprintf(g_op_selected, sizeof g_op_selected, "%s", g_op_scan[i].plmn);
                break;
            }
    }
    for (int i = 0; i < g_op_candidate_n && o < (int)sizeof(buf) - 500; i++) {
        struct operator_candidate_state *item = &g_op_scan[i];
        char name[200];
        const char *state = item->status == 2 ? "当前" : item->status == 1 ? "可用" : item->status == 3 ? "禁止" : "未知";
        const char *state_cls = item->status == 2 ? "ok" : item->status == 1 ? "ready" : item->status == 3 ? "bad" : "muted";
        int selected = !strcmp(g_op_selected, item->plmn);
        html_esc(name, sizeof name, item->name);
        if (item->status == 3) {
            o += snprintf(buf + o, sizeof buf - (size_t)o,
                          "<div class='operator-row disabled%s'><span class='operator-main'>%s<small>%s</small></span><span class='operator-state %s'>%s</span></div>",
                          selected ? " selected" : "", name, item->plmn, state_cls, state);
        } else {
            o += snprintf(buf + o, sizeof buf - (size_t)o,
                          "<a href='act:opselect:%s' class='operator-row%s'><span class='operator-main'>%s<small>%s</small></span><span class='operator-state %s'>%s</span></a>",
                          item->plmn, selected ? " selected" : "", name, item->plmn, state_cls, state);
        }
    }
    if (g_op_candidate_total > g_op_candidate_n)
        snprintf(buf + o, sizeof buf - (size_t)o,
                 "<div class='compact-more'>另有 %d 项，请在网页端查看</div>",
                 g_op_candidate_total - g_op_candidate_n);
    return buf;
}

static const char *speedtest_src_html(void)
{
    static const struct { const char *v; const char *lab; } opts[4] = {
        { "auto", "自动" }, { "cnspeed", "全球网测" }, { "ookla", "Ookla" }, { "cdn", "CDN" } };
    static char buf[512];
    int o = snprintf(buf, sizeof buf, "<div class='stseg'>");
    for (int i = 0; i < 4; i++)
        o += snprintf(buf + o, sizeof buf - o, "<a href='act:stsrc:%s' class='stsegc%s'>%s</a>",
                      opts[i].v, !strcmp(g_st_src, opts[i].v) ? " on" : "", opts[i].lab);
    snprintf(buf + o, sizeof buf - o, "</div>");
    return buf;
}

static const char *speedtest_dir_html(void)
{
    static const struct { const char *v; const char *lab; } opts[3] = {
        { "both", "上下行" }, { "dl", "下行" }, { "ul", "上行" } };
    static char buf[384];
    int o = snprintf(buf, sizeof buf, "<div class='stseg'>");
    for (int i = 0; i < 3; i++)
        o += snprintf(buf + o, sizeof buf - o, "<a href='act:stdir:%s' class='stsegc%s'>%s</a>",
                      opts[i].v, !strcmp(g_st_dir, opts[i].v) ? " on" : "", opts[i].lab);
    snprintf(buf + o, sizeof buf - o, "</div>");
    return buf;
}

static const char *speedtest_dur_html(void)
{
    static const struct { int sec; const char *lab; } opts[4] = {
        { 10, "10s" }, { 15, "15s" }, { 20, "20s" }, { 0, "循环" } };
    static char buf[420];
    int o = snprintf(buf, sizeof buf, "<div class='stseg'>");
    for (int i = 0; i < 4; i++)
        o += snprintf(buf + o, sizeof buf - o, "<a href='act:stdur:%d' class='stsegc%s'>%s</a>",
                      opts[i].sec,
                      g_st_dur == opts[i].sec ? (opts[i].sec == 0 ? " loop on" : " on") : (opts[i].sec == 0 ? " loop" : ""),
                      opts[i].lab);
    snprintf(buf + o, sizeof buf - o, "</div>");
    return buf;
}

static const char *speedtest_loop_hint_html(void)
{
    if (!speedtest_loop_mode()) return "";
    return "<div class='stloop-warn'>循环测速不会自动停止，注意流量消耗。</div>";
}

static const char *speedtest_install_html(void)
{
    static char buf[256], esc[160];
    if (g_st_installed > 0) return "";
    html_esc(esc, sizeof esc, SPEEDTEST_BIN);
    snprintf(buf, sizeof buf, "<div class='sthint bad'>未安装：<span class='mono'>%s</span></div>", esc);
    return buf;
}

static const char *speedtest_action_html(void)
{
    if (g_st_installed <= 0) return "<span class='stbtn dis'>安装后可用</span>";
    if (g_st_state == ST_STATE_RUNNING) return "<a href='act:ststop' class='stbtn stop'>停止测速</a>";
    if (speedtest_loop_mode()) return "<a href='act:ststart' class='stbtn loop'>开始循环测速</a>";
    return "<a href='act:ststart' class='stbtn'>开始测速</a>";
}

static int speedtest_pct(double v, double scale)
{
    int pct;
    if (scale < 1.0) scale = 1.0;
    if (v < 0.0) v = 0.0;
    pct = (int)(v * 100.0 / scale + 0.5);
    if (v > 0.0 && pct < 4) pct = 4;
    if (pct > 100) pct = 100;
    return pct;
}

static void speedtest_chart_row(char *buf, size_t cap, int *o,
                                const char *lab, double val, double scale,
                                const char *unit, const char *cls)
{
    int pct = speedtest_pct(val, scale);
    if (*o >= (int)cap) return;
    *o += snprintf(buf + *o, cap - (size_t)*o,
                   "<div class='stbar-row'><span class='stbar-lab'>%s</span>"
                   "<span class='stbar-box'><span class='stbar-fill %s' style='width:%d%%'></span></span>"
                   "<span class='stbar-val'>%.1f%s</span></div>",
                   lab, cls ? cls : "", pct, val, unit);
}

static const char *speedtest_gauge_html(void)
{
    static char buf[640];
    double cur = 0.0, peak = 0.0, scale = 100.0;
    const char *label = "待机";
    char sub[128];
    int pct;
    if (g_st_state == ST_STATE_RUNNING) {
        cur = g_st_cur_mbps;
        peak = g_st_cur_peak;
        label = (!strcmp(g_st_phase, "upload")) ? "当前上行" :
                (!strcmp(g_st_phase, "download")) ? "当前下行" : "测速中";
        snprintf(sub, sizeof sub, "%s / 峰值 %.1f Mbps", speedtest_phase_label(g_st_phase), peak);
    } else if (g_st_have_result) {
        cur = g_st_dl_avg >= g_st_ul_avg ? g_st_dl_avg : g_st_ul_avg;
        peak = g_st_dl_peak >= g_st_ul_peak ? g_st_dl_peak : g_st_ul_peak;
        label = "本次速度";
        snprintf(sub, sizeof sub, "下行 %.1f / 上行 %.1f Mbps", g_st_dl_avg, g_st_ul_avg);
    } else if (g_st_state == ST_STATE_MISSING) {
        label = "未安装";
        snprintf(sub, sizeof sub, "安装测速器后可用");
    } else if (g_st_state == ST_STATE_FAIL) {
        label = "失败";
        snprintf(sub, sizeof sub, "%s", g_st_err[0] ? g_st_err : "测速失败");
    } else {
        label = "准备就绪";
        snprintf(sub, sizeof sub, "点击开始测速");
    }
    if (peak > scale) scale = peak;
    if (cur > scale) scale = cur;
    pct = speedtest_pct(cur, scale);
    snprintf(buf, sizeof buf,
             "<div class='st-card st-gauge'>"
             "<div class='st-gauge-title'>网速仪表盘</div>"
             "<div class='st-gauge-face'><span class='st-gauge-num'>%.1f</span><span class='st-gauge-unit'>Mbps</span></div>"
             "<div class='st-scale'><span class='st-fill' style='width:%d%%'></span></div>"
             "<div class='st-gauge-sub'>%s · %s</div>"
             "</div>",
             cur, pct, label, sub);
    return buf;
}

static const char *speedtest_chart_html(void)
{
    static char buf[1200];
    int o = 0;
    double speed_scale = 100.0;
    double max_speed = g_st_dl_peak;
    if (g_st_ul_peak > max_speed) max_speed = g_st_ul_peak;
    if (g_st_cur_peak > max_speed) max_speed = g_st_cur_peak;
    if (max_speed > speed_scale) speed_scale = max_speed;
    o += snprintf(buf + o, sizeof buf - (size_t)o, "<div class='st-card st-chart'><div class='st-chart-title'>测速图表</div>");
    if (g_st_have_result) {
        speedtest_chart_row(buf, sizeof buf, &o, "下行", g_st_dl_avg, speed_scale, "M", "");
        speedtest_chart_row(buf, sizeof buf, &o, "峰值", g_st_dl_peak, speed_scale, "M", "");
        speedtest_chart_row(buf, sizeof buf, &o, "上行", g_st_ul_avg, speed_scale, "M", "up");
        speedtest_chart_row(buf, sizeof buf, &o, "峰值", g_st_ul_peak, speed_scale, "M", "up");
        speedtest_chart_row(buf, sizeof buf, &o, "延迟", g_st_ping, 100.0, "ms", "ping");
    } else if (g_st_state == ST_STATE_RUNNING) {
        speedtest_chart_row(buf, sizeof buf, &o, "当前", g_st_cur_mbps, speed_scale, "M", !strcmp(g_st_phase, "upload") ? "up" : "");
        speedtest_chart_row(buf, sizeof buf, &o, "峰值", g_st_cur_peak, speed_scale, "M", !strcmp(g_st_phase, "upload") ? "up" : "");
    } else {
        speedtest_chart_row(buf, sizeof buf, &o, "下行", 0.0, speed_scale, "M", "");
        speedtest_chart_row(buf, sizeof buf, &o, "上行", 0.0, speed_scale, "M", "up");
        speedtest_chart_row(buf, sizeof buf, &o, "延迟", 0.0, 100.0, "ms", "ping");
    }
    snprintf(buf + o, sizeof buf - (size_t)o, "</div>");
    return buf;
}

static double speedtest_display_mbps(void)
{
    if (g_st_state == ST_STATE_RUNNING) return g_st_cur_mbps;
    if (g_st_have_result) return g_st_dl_avg >= g_st_ul_avg ? g_st_dl_avg : g_st_ul_avg;
    return 0.0;
}

static double speedtest_display_peak_mbps(void)
{
    double peak = g_st_cur_peak;
    if (g_st_dl_peak > peak) peak = g_st_dl_peak;
    if (g_st_ul_peak > peak) peak = g_st_ul_peak;
    if (peak < speedtest_display_mbps()) peak = speedtest_display_mbps();
    if (peak < 100.0) peak = 100.0;
    return peak;
}

static const char *speedtest_dial_html(void)
{
    static char buf[720], sub[160], esc[220];
    double cur = speedtest_display_mbps();
    if (g_st_state == ST_STATE_RUNNING) {
        snprintf(sub, sizeof sub, "%s / 峰值 %.1f Mbps", speedtest_phase_label(g_st_phase), g_st_cur_peak);
    } else if (g_st_have_result) {
        snprintf(sub, sizeof sub, "下行 %.1f / 上行 %.1f Mbps", g_st_dl_avg, g_st_ul_avg);
    } else if (g_st_state == ST_STATE_MISSING) {
        snprintf(sub, sizeof sub, "安装测速器后可用");
    } else if (g_st_state == ST_STATE_FAIL) {
        snprintf(sub, sizeof sub, "%s", g_st_err[0] ? g_st_err : "测速失败");
    } else if (speedtest_loop_mode()) {
        snprintf(sub, sizeof sub, "循环模式 / 手动停止");
    } else {
        snprintf(sub, sizeof sub, "准备就绪 / %d秒", g_st_dur);
    }
    html_esc(esc, sizeof esc, sub);
    snprintf(buf, sizeof buf,
             "<div class='st-card st-gauge'>"
             "<div id='st-gauge-dial' class='st-gauge-dial' style='width:184px;height:184px;margin:0 auto'></div>"
             "<div class='st-gauge-sub'>%.1f Mbps · %s</div>"
             "</div>",
             cur, esc);
    return buf;
}

static const char *speedtest_lines_html(void)
{
    static char buf[720];
    double dl = g_st_have_result ? g_st_dl_avg : (g_st_dl_n > 0 ? g_st_dl_hist[g_st_dl_n - 1] / 10.0 : 0.0);
    double ul = g_st_have_result ? g_st_ul_avg : (g_st_ul_n > 0 ? g_st_ul_hist[g_st_ul_n - 1] / 10.0 : 0.0);
    snprintf(buf, sizeof buf,
             "<div class='st-card st-lines'>"
             "<div class='st-line-head'>下行折线<b>%.1f Mbps</b></div>"
             "<div id='st-chart-dl' class='st-line-chart'></div>"
             "<div class='st-line-head'>上行折线<b>%.1f Mbps</b></div>"
             "<div id='st-chart-ul' class='st-line-chart'></div>"
             "</div>",
             dl, ul);
    return buf;
}

static const char *speedtest_home_inline_html(void)
{
    static char buf[8192];
    if (g_lock_state || !g_st_home_open || !speedtest_binary_ready()) return "";
    snprintf(buf, sizeof buf,
             "<div class='st-home-panel'>"
             "%s"
             "%s"
             "<div class='st-card st-options'>"
             "<div class='st-caption'>测速源</div>%s"
             "<div class='st-caption'>方向</div>%s"
             "<div class='st-caption'>时长</div>%s"
             "%s"
             "</div>"
             "<div class='st-actionbar'>%s</div>"
             "</div>",
             speedtest_dial_html(),
             speedtest_lines_html(),
             speedtest_src_html(),
             speedtest_dir_html(),
             speedtest_dur_html(),
             speedtest_loop_hint_html(),
             speedtest_action_html());
    return buf;
}
static const char *speedtest_status_html(void)
{
    static char buf[320], raw[180], esc[256];
    const char *cls = "idle";
    if (g_st_state == ST_STATE_RUNNING) {
        cls = "run";
        if (speedtest_loop_mode() && g_st_have_result)
            snprintf(raw, sizeof raw, "循环测速中 / 手动停止");
        else if (!strcmp(g_st_phase, "download") || !strcmp(g_st_phase, "upload"))
            snprintf(raw, sizeof raw, "%s %.1f Mbps / 峰值 %.1f",
                     speedtest_phase_label(g_st_phase), g_st_cur_mbps, g_st_cur_peak);
        else if (speedtest_loop_mode())
            snprintf(raw, sizeof raw, "循环测速中 / 手动停止");
        else if (g_st_phase[0]) snprintf(raw, sizeof raw, "%s", speedtest_phase_label(g_st_phase));
        else if (g_st_msg[0]) snprintf(raw, sizeof raw, "测速中");
        else snprintf(raw, sizeof raw, "测速中");
    } else if (g_st_state == ST_STATE_DONE) {
        cls = "done";
        snprintf(raw, sizeof raw, "完成 / 下行 %.1f Mbps", g_st_dl_avg);
    } else if (g_st_state == ST_STATE_FAIL) {
        cls = "fail";
        snprintf(raw, sizeof raw, "%s", g_st_err[0] ? g_st_err : "测速失败");
    } else if (g_st_state == ST_STATE_MISSING) {
        cls = "fail";
        snprintf(raw, sizeof raw, "未安装测速器");
    } else {
        snprintf(raw, sizeof raw, "准备就绪 / %d秒", g_st_dur);
    }
    html_esc(esc, sizeof esc, raw);
    snprintf(buf, sizeof buf, "<div class='ststat %s'>%s</div>", cls, esc);
    return buf;
}

static const char *speedtest_result_html(void)
{
    static char buf[1400], node[192], source[96], loc[128], ul[192], err[192];
    char loc_raw[96];
    if (g_st_have_result) {
        html_esc(node, sizeof node, g_st_node[0] ? g_st_node : "-");
        html_esc(source, sizeof source, g_st_source[0] ? g_st_source : "-");
        if (g_st_carrier[0] || g_st_city[0])
            snprintf(loc_raw, sizeof loc_raw, "%s%s%s", g_st_carrier, (g_st_carrier[0] && g_st_city[0]) ? " / " : "", g_st_city);
        else
            snprintf(loc_raw, sizeof loc_raw, "-");
        html_esc(loc, sizeof loc, loc_raw);
        snprintf(ul, sizeof ul, "%.1f / %.1f Mbps", g_st_ul_avg, g_st_ul_peak);
        snprintf(buf, sizeof buf,
                 "<div class='stres'>"
                 "<div class='strow'><span>节点</span><b>%s</b></div>"
                 "<div class='strow'><span>来源</span><b>%s</b></div>"
                 "<div class='strow'><span>位置</span><b>%s</b></div>"
                 "<div class='strow'><span>下行</span><b>%.1f / %.1f Mbps</b></div>"
                 "<div class='strow'><span>上行</span><b>%s</b></div>"
                 "<div class='strow'><span>延迟</span><b>%.1f ms / 抖动 %.1f ms</b></div>"
                 "</div>",
                 node, source, loc, g_st_dl_avg, g_st_dl_peak, ul, g_st_ping, g_st_jitter);
        return buf;
    }
    if (g_st_state == ST_STATE_FAIL) {
        html_esc(err, sizeof err, g_st_err[0] ? g_st_err : "测速失败");
        snprintf(buf, sizeof buf, "<div class='stempty bad'>%s</div>", err);
        return buf;
    }
    if (g_st_state == ST_STATE_RUNNING && speedtest_loop_mode()) return "<div class='stempty'>循环测速中，手动停止前不会自动结束。</div>";
    if (g_st_state == ST_STATE_RUNNING) return "<div class='stempty'>测速进行中，结果稍后显示。</div>";
    if (g_st_state == ST_STATE_MISSING) return "<div class='stempty'>安装测速器后显示结果。</div>";
    return "<div class='stempty'>暂无测速结果。</div>";
}

static const char *speedtest_log_card_html(void)
{
    static char buf[2200];
    snprintf(buf, sizeof buf,
             "<div class='st-card st-log'>"
             "<div class='st-caption'>测速日志</div>"
             "%s%s"
             "</div>",
             speedtest_status_html(),
             speedtest_result_html());
    return buf;
}

static int hsr_freq_is_whitelist(long arfcn)
{
    static const long hsr_arfcn[] = {
        507150, 527070, 531390, 153370,
        505230, 627744, 634464, 423630
    };

    for (size_t i = 0; i < sizeof(hsr_arfcn) / sizeof(hsr_arfcn[0]); i++)
        if (arfcn == hsr_arfcn[i])
            return 1;
    return 0;
}

static int hsr_freq_hint(int mcc, const char *arfcn)
{
    char *end = NULL;
    long v;

    if (mcc != 460 || !arfcn || !arfcn[0] || !strcmp(arfcn, "-"))
        return 0;
    v = strtol(arfcn, &end, 10);
    if (end == arfcn || v <= 0)
        return 0;
    return hsr_freq_is_whitelist(v);
}

#define SIGNAL_MAX_CARRIERS 16
#define SIGNAL_MAX_NEIGHBORS 16

struct signal_carrier_metric {
    int valid;
    int used;
    char name[32];
    char type[16];
    char pci[16];
    char arfcn[32];
    char rb[16];
    char grants[16];
    char dl_rb[16];
    char dl_grants[16];
    char ul_rb[16];
    char ul_grants[16];
    char mcs[16];
    char modulation[32];
    char mimo[48];
    char layers[16];
    char bler[16];
    char ssb[16];
    char serving_ssb[16];
    char ml1_rsrp[16];
    char ml1_rsrq[16];
    char ml1_sinr[16];
};

struct signal_neighbor_metric {
    int valid;
    char pci[16];
    char band[24];
    char arfcn[32];
    char rsrp[16];
    char rsrq[16];
};

static void signal_carrier_metric_reset(struct signal_carrier_metric *c)
{
    if (!c) return;
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "-");
    snprintf(c->type, sizeof c->type, "-");
    snprintf(c->pci, sizeof c->pci, "-");
    snprintf(c->arfcn, sizeof c->arfcn, "-");
    snprintf(c->rb, sizeof c->rb, "-");
    snprintf(c->grants, sizeof c->grants, "-");
    snprintf(c->dl_rb, sizeof c->dl_rb, "-");
    snprintf(c->dl_grants, sizeof c->dl_grants, "-");
    snprintf(c->ul_rb, sizeof c->ul_rb, "-");
    snprintf(c->ul_grants, sizeof c->ul_grants, "-");
    snprintf(c->mcs, sizeof c->mcs, "-");
    snprintf(c->modulation, sizeof c->modulation, "-");
    snprintf(c->mimo, sizeof c->mimo, "-");
    snprintf(c->layers, sizeof c->layers, "-");
    snprintf(c->bler, sizeof c->bler, "-");
    snprintf(c->ssb, sizeof c->ssb, "-");
    snprintf(c->serving_ssb, sizeof c->serving_ssb, "-");
    snprintf(c->ml1_rsrp, sizeof c->ml1_rsrp, "-");
    snprintf(c->ml1_rsrq, sizeof c->ml1_rsrq, "-");
    snprintf(c->ml1_sinr, sizeof c->ml1_sinr, "-");
}

static void signal_neighbor_metric_reset(struct signal_neighbor_metric *n)
{
    if (!n) return;
    memset(n, 0, sizeof *n);
    snprintf(n->pci, sizeof n->pci, "-");
    snprintf(n->band, sizeof n->band, "-");
    snprintf(n->arfcn, sizeof n->arfcn, "-");
    snprintf(n->rsrp, sizeof n->rsrp, "-");
    snprintf(n->rsrq, sizeof n->rsrq, "-");
}

static const char *signal_carrier_val(const char *v)
{
    return signal_value_present(v) ? v : "-";
}

static void carrier_metric_html(char *dst, size_t cap,
                                const struct signal_carrier_metric *m,
                                int is_nr)
{
    char a[64], b[64], c[64], d[64], e[64], f[64];
    size_t off;

    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!m || !m->valid) return;

    if (is_nr) {
        const char *ssb = signal_value_present(m->serving_ssb) ? m->serving_ssb : m->ssb;
        html_esc(a, sizeof a, signal_carrier_val(m->mimo));
        html_esc(b, sizeof b, signal_carrier_val(m->layers));
        html_esc(c, sizeof c, signal_carrier_val(m->dl_grants));
        html_esc(d, sizeof d, signal_carrier_val(m->ul_grants));
        html_esc(e, sizeof e, signal_carrier_val(m->dl_rb));
        html_esc(f, sizeof f, signal_carrier_val(m->ul_rb));
        snprintf(dst, cap,
                 "<div class='cx'><span>MIMO %s</span><span>L %s</span>"
                 "<span>G %s/%s</span><span>RB %s/%s</span>",
                 a, b, c, d, e, f);
        html_esc(a, sizeof a, signal_carrier_val(m->bler));
        html_esc(b, sizeof b, signal_carrier_val(ssb));
        snprintf(dst + strlen(dst), cap - strlen(dst),
                 "<span>BLER %s</span><span>SSB %s</span></div>", a, b);
    } else {
        const char *mod = signal_value_present(m->modulation) ? m->modulation : m->mcs;
        html_esc(a, sizeof a, signal_carrier_val(mod));
        html_esc(b, sizeof b, signal_carrier_val(m->layers));
        html_esc(c, sizeof c, signal_carrier_val(m->grants));
        html_esc(d, sizeof d, signal_carrier_val(m->rb));
        html_esc(e, sizeof e, signal_carrier_val(m->bler));
        snprintf(dst, cap,
                 "<div class='cx'><span>MCS %s</span><span>L %s</span>"
                 "<span>G %s</span><span>RB %s</span><span>BLER %s</span></div>",
                 a, b, c, d, e);
    }
    if (signal_value_present(m->ml1_rsrp) ||
        signal_value_present(m->ml1_rsrq) ||
        signal_value_present(m->ml1_sinr)) {
        html_esc(a, sizeof a, signal_carrier_val(m->ml1_rsrp));
        html_esc(b, sizeof b, signal_carrier_val(m->ml1_rsrq));
        html_esc(c, sizeof c, signal_carrier_val(m->ml1_sinr));
        off = strlen(dst);
        snprintf(dst + off, cap - off,
                 "<div class='cx sigsrc'><span>ML1 RSRP %s</span><span>RSRQ %s</span>",
                 a, b);
        if (signal_value_present(m->ml1_sinr)) {
            off = strlen(dst);
            snprintf(dst + off, cap - off, "<span>SINR %s</span>", c);
        }
        off = strlen(dst);
        snprintf(dst + off, cap - off, "</div>");
    }
}

/* Append one carrier card (band/bw + PCI, then RSRP/SINR colored by quality).
 * A carrier reporting the floor sentinel (RSRP <= -140) is "configured but not
 * active"; its values are grayed out and tagged inactive. */
static int car_row(char *buf, int o, int cap, const char *band, const char *bw,
                    const char *arfcn, const char *pci, const char *rsrp, const char *sinr,
                    int hsr_hint, const struct signal_carrier_metric *metric)
{
    const char *rsrp_show = signal_value_present(rsrp) ? rsrp : "-";
    const char *sinr_show = signal_value_present(sinr) ? sinr : "-";
    int has_rsrp;
    int has_sinr;
    double rp;
    double sn;
    int inactive;
    const char *rq;
    const char *sq;
    const char *tag;
    const char *hsr = hsr_hint ? "<span class='chsr'>高铁专网</span>" : "";
    int is_nr_metric = band && band[0] == 'n';
    const char *al = is_nr_metric ? "ARFCN" : "EARFCN";
    char bw_text[20];
    char extra[768];

    has_rsrp = signal_value_present(rsrp_show);
    has_sinr = signal_value_present(sinr_show);
    rp = has_rsrp ? atof(rsrp_show) : -150.0;
    sn = has_sinr ? atof(sinr_show) : -99.0;
    inactive = has_rsrp && rp <= -140.0;
    rq = !has_rsrp || inactive ? "q-off" : rp >= -85 ? "q-good" : rp >= -105 ? "q-mid" : "q-bad";
    sq = !has_sinr || inactive ? "q-off" : sn >= 13  ? "q-good" : sn >= 0    ? "q-mid" : "q-bad";
    tag = inactive ? "<span class='coff'>未激活</span>" : "";
    carrier_metric_html(extra, sizeof extra, metric, is_nr_metric);
    if (signal_value_present(bw))
        snprintf(bw_text, sizeof bw_text, "%sM", bw);
    else
        snprintf(bw_text, sizeof bw_text, "-");
    return o + snprintf(buf + o, cap - o,
        "<div class='ccd%s%s'><span class='cb'>%s</span><span class='cbw'> %s</span>%s%s"
        "<span class='cinfo'><span class='carfcn'>%s %s</span><span class='cpci'>PCI %s</span></span>"
        "<div class='cm'><span class='ml'>RSRP</span><span class='%s'>%s</span>"
        "<span class='ml ml2'>SINR</span><span class='%s'>%s</span></div>%s</div>",
        inactive ? " off" : "", hsr_hint ? " hsrhint" : "",
        band, bw_text, tag, hsr,
        al, (arfcn && arfcn[0]) ? arfcn : "-", (pci && pci[0]) ? pci : "-",
        rq, rsrp_show, sq, sinr_show,
        extra);
}

static int carrier_is_inactive(const char *rsrp)
{
    return atof((rsrp && rsrp[0]) ? rsrp : "") <= -140.0;
}

/* Split a CA group "f0,f1,..." into fields[] (returns count). */
static int ca_split(char *g, char **f, int maxf)
{
    int n = 0; char *save;
    for (char *tk = strtok_r(g, ",", &save); tk && n < maxf; tk = strtok_r(NULL, ",", &save)) f[n++] = tk;
    return n;
}

static int ca_groups(char *s, char **g, int maxg)
{
    int n = 0; char *save;
    for (char *tk = strtok_r(s, ";", &save); tk && n < maxg; tk = strtok_r(NULL, ";", &save)) g[n++] = tk;
    return n;
}

/* LTE SCC signal is sometimes exposed separately from legacy 5-field lteca.
 * We accept both full 11-field groups and shorter signal-only groups by
 * locating the first "RSRP-like" value (typically <= -40 dBm), then taking
 * SINR from two fields later when present. */
static int ca_sig_pick(char *g, const char **rsrp, const char **sinr)
{
    char *f[16];
    int nf = ca_split(g, f, 16), rp = -1;
    *rsrp = *sinr = NULL;
    for (int i = 0; i < nf; i++) {
        if (atof(f[i]) <= -40.0) { rp = i; break; }
    }
    if (rp < 0) return 0;
    *rsrp = f[rp];
    if (rp + 2 < nf)      *sinr = f[rp + 2];
    else if (rp + 1 < nf) *sinr = f[rp + 1];
    return 1;
}

static int signal_level(int rsrp)
{
    int lvl;
    if      (rsrp == 0)    lvl = 0;   /* unknown / no signal */
    else if (rsrp >= -80)  lvl = 5;
    else if (rsrp >= -90)  lvl = 4;
    else if (rsrp >= -100) lvl = 3;
    else if (rsrp >= -110) lvl = 2;
    else                   lvl = 1;
    return lvl;
}

/* ---- rolling chart history, independent from page rendering ---- */
#define CHART_HIST 301
struct chart_sample {
    uint32_t monotonic_sec;
    int32_t cpu_usage;
    int32_t cpu_temp;
    int32_t mem_used_pct;
    int32_t rx_speed;
    int32_t tx_speed;
    int32_t battery_temp;
    int32_t battery_power_mw;
};
static struct chart_sample g_chart_hist[CHART_HIST];
static int g_chart_head, g_chart_count;
static uint32_t g_chart_last_attempt_sec;

/* charge (charger input) or discharge (battery) power, in milliwatts. */
/* Battery power in milliwatts (use battery_voltage * |battery_current|). */
static int power_mw(const devui_data_t *d)
{
    double w = (d->bat_uv / 1e6) * (labs(d->bat_ua) / 1e6);
    return (int)(w * 1000);
}

static int32_t chart_i32(long v)
{
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return (int32_t)v;
}

static void chart_hist_push(uint32_t sec, const devui_data_t *d)
{
    int i;
    if (!d || d->cpu_usage < 0 || d->mem_used_pct < 0 ||
        d->rx_speed < 0 || d->tx_speed < 0)
        return;
    if (g_chart_count < CHART_HIST) {
        i = (g_chart_head + g_chart_count) % CHART_HIST;
        g_chart_count++;
    } else {
        i = g_chart_head;
        g_chart_head = (g_chart_head + 1) % CHART_HIST;
    }
    g_chart_hist[i].monotonic_sec = sec;
    g_chart_hist[i].cpu_usage = chart_i32(d->cpu_usage);
    g_chart_hist[i].cpu_temp = chart_i32(d->cpu_temp);
    g_chart_hist[i].mem_used_pct = chart_i32(d->mem_used_pct);
    g_chart_hist[i].rx_speed = chart_i32(d->rx_speed);
    g_chart_hist[i].tx_speed = chart_i32(d->tx_speed);
    g_chart_hist[i].battery_temp = chart_i32(d->bat_temp);
    g_chart_hist[i].battery_power_mw = chart_i32(power_mw(d));
}

static void chart_sample_tick(int awake)
{
    uint32_t sec = monotonic_seconds();
    devui_data_t d;
    if (sec == g_chart_last_attempt_sec) return;
    g_chart_last_attempt_sec = sec;
    if ((awake ? data_refresh_live(&d) : data_chart_metrics(&d)))
        chart_hist_push(sec, &d);
}

enum chart_field {
    CHART_CPU, CHART_CPU_TEMP, CHART_MEM, CHART_RX, CHART_TX,
    CHART_BATT_TEMP, CHART_BATT_POWER
};

static int chart_history_values(enum chart_field field, uint32_t *times, int *vals,
                                int window_sec, uint32_t now_sec)
{
    int n = 0;
    uint32_t left = now_sec > (uint32_t)window_sec ? now_sec - (uint32_t)window_sec : 0;
    for (int k = 0; k < g_chart_count; k++) {
        const struct chart_sample *s = &g_chart_hist[(g_chart_head + k) % CHART_HIST];
        int32_t v;
        if (s->monotonic_sec < left || s->monotonic_sec > now_sec) continue;
        switch (field) {
        case CHART_CPU:        v = s->cpu_usage; break;
        case CHART_CPU_TEMP:   v = s->cpu_temp; break;
        case CHART_MEM:        v = s->mem_used_pct; break;
        case CHART_RX:         v = s->rx_speed; break;
        case CHART_TX:         v = s->tx_speed; break;
        case CHART_BATT_TEMP:  v = s->battery_temp; break;
        default:               v = s->battery_power_mw; break;
        }
        times[n] = s->monotonic_sec;
        vals[n] = (int)v;
        n++;
    }
    return n;
}

/* Draw the chart placeholders (#chart-cpu/#chart-mem/#chart-net) natively as
 * Bresenham polylines over the history. Called after the page is rendered; a
 * no-op on pages without those elements. */
static void draw_charts(void)
{
    static uint32_t times[CHART_HIST];
    static int a[CHART_HIST], bvals[CHART_HIST];
    uint32_t now_sec = monotonic_seconds();
    int n, nb;
    int x, y, w, h;
    if (html_view_rect("#chart-cpu", &x, &y, &w, &h)) {
        n = chart_history_values(CHART_CPU, times, a, g_chart_cpu_sec, now_sec);
        html_view_timed_polyline(x, y, w, h, times, a, n, now_sec, g_chart_cpu_sec,
                                 0, 100, 0x4f, 0x8f, 0xe8, 2, 26);
        n = chart_history_values(CHART_CPU_TEMP, times, a, g_chart_cpu_sec, now_sec);
        html_view_timed_polyline(x, y, w, h, times, a, n, now_sec, g_chart_cpu_sec,
                                 20, 70, 0xd5, 0xa6, 0x3d, 2, 0);
    }
    if (html_view_rect("#chart-mem", &x, &y, &w, &h)) {
        n = chart_history_values(CHART_MEM, times, a, g_chart_mem_sec, now_sec);
        html_view_timed_polyline(x, y, w, h, times, a, n, now_sec, g_chart_mem_sec,
                                 0, 100, 0x55, 0xbc, 0x7b, 2, 34);
    }
    if (html_view_rect("#chart-net", &x, &y, &w, &h)) {
        int mx = 1;
        n = chart_history_values(CHART_RX, times, a, g_chart_net_sec, now_sec);
        nb = chart_history_values(CHART_TX, times, bvals, g_chart_net_sec, now_sec);
        for (int i = 0; i < n; i++) if (a[i] > mx) mx = a[i];
        for (int i = 0; i < nb; i++) if (bvals[i] > mx) mx = bvals[i];
        html_view_timed_polyline(x, y, w, h, times, a, n, now_sec, g_chart_net_sec,
                                 0, mx, 0x4f, 0x8f, 0xe8, 2, 22);
        html_view_timed_polyline(x, y, w, h, times, bvals, nb, now_sec, g_chart_net_sec,
                                 0, mx, 0xd5, 0xa6, 0x3d, 2, 0);
    }
    if (html_view_rect("#chart-batt", &x, &y, &w, &h)) {
        int mx = 1;
        n = chart_history_values(CHART_BATT_POWER, times, a, g_chart_batt_sec, now_sec);
        nb = chart_history_values(CHART_BATT_TEMP, times, bvals, g_chart_batt_sec, now_sec);
        for (int i = 0; i < n; i++) if (a[i] > mx) mx = a[i];
        for (int i = 0; i < n; i++) a[i] = (int)((int64_t)a[i] * 100 / mx);
        for (int i = 0; i < nb; i++) bvals[i] = (bvals[i] - 20) * 2;
        html_view_timed_polyline(x, y, w, h, times, a, n, now_sec, g_chart_batt_sec,
                                 0, 100, 0x4f, 0x8f, 0xe8, 2, 22);
        html_view_timed_polyline(x, y, w, h, times, bvals, nb, now_sec, g_chart_batt_sec,
                                 0, 100, 0xd5, 0xa6, 0x3d, 2, 0);
    }
}

/* Padlock glyph centered in the lower screen, shown on the locked preview
 * (state 1). Drawn natively so it does not depend on font glyph coverage. */
static void draw_lock_icon(void)
{
    const int cx = 160, cy = 406;          /* icon center, lower-middle */
    int dark = !g_theme;
    int lv = dark ? 0xff : 0x00;           /* lock color: white(dark) / black(light) */
    /* shackle: arch ring (semicircle + short legs); legs end inside the body */
    {
        int xs[40], ys[40], n = 0;
        const int ro = 7, ri = 5, arc_cy = cy - 2, leg = cy + 3;
        xs[n] = cx - ro; ys[n] = leg; n++;
        for (int aa = 180; aa >= 0; aa -= 30) { double r = aa * 3.14159265 / 180; xs[n] = cx + (int)(ro * cos(r)); ys[n] = arc_cy - (int)(ro * sin(r)); n++; }
        xs[n] = cx + ro; ys[n] = leg; n++;
        xs[n] = cx + ri; ys[n] = leg; n++;
        for (int aa = 0; aa <= 180; aa += 30) { double r = aa * 3.14159265 / 180; xs[n] = cx + (int)(ri * cos(r)); ys[n] = arc_cy - (int)(ri * sin(r)); n++; }
        xs[n] = cx - ri; ys[n] = leg; n++;
        html_view_fill_poly(xs, ys, n, lv, lv, lv, 255);
    }
    /* body + keyhole (keyhole in page bg tone to look punched through) */
    html_view_fill_round_rect(cx - 10, cy + 1, 20, 15, 4, lv, lv, lv, 255);
    html_view_fill_round_rect(cx - 2, cy + 6, 4, 4, 2,
                              dark ? 0x0d : 0xee, dark ? 0x12 : 0xf3, dark ? 0x19 : 0xf8, 255);
}

/* SMS detail dialog + status-bar envelope state. */
static long g_sms_open = -1;   /* opened SMS message id for detail dialog (-1 = none) */
static int  g_sms_unread_now;  /* unread SMS present -> draw the status-bar envelope */
static char g_sms_num[40], g_sms_date[16], g_sms_text[DEVUI_SMS_TEXT_MAX];   /* opened message */
static int  g_sms_scroll, g_sms_scroll_max;
static int  g_sms_view_x, g_sms_view_y, g_sms_view_w, g_sms_view_h;

/* Discard all latched input when the dialog closes so an old release cannot
 * be replayed against the message card exposed underneath. */
static void sms_close(touch_input_t *touch, int *need_render)
{
    g_sms_open = -1;
    g_sms_scroll = g_sms_scroll_max = 0;
    touch_input_clear_taps(touch);
    *need_render = 1;
}

/* Draw a small envelope in the fixed status bar, just right of the clock, when
 * there are unread messages. Native shape; the font has no envelope glyph. */
static void draw_sms_icon(void)
{
    if (!g_sms_unread_now) return;
    const int ew = 16, eh = 11;
    int ex = 8 + html_view_text_width_px(g_stat_time, 16) + 7;
    int ey = (26 - eh) / 2;
    /* body: blue rounded rectangle */
    html_view_fill_round_rect(ex, ey, ew, eh, 2, 0x4f, 0x8f, 0xe8, 255);
    /* flap: white inverted-V from the two top corners down to center */
    int xs[3] = { ex + 1, ex + ew - 1, ex + ew / 2 };
    int ys[3] = { ey + 1, ey + 1,      ey + eh / 2 + 1 };
    html_view_fill_poly(xs, ys, 3, 0xff, 0xff, 0xff, 255);
}

/* ---- band lock (éé¢): comma-list band sets ---- */
static void draw_native_statusbar(void)
{
    const int dark = !g_theme;
    const int bg_r = dark ? 0x11 : 0xdc, bg_g = dark ? 0x1b : 0xe7, bg_b = dark ? 0x27 : 0xf2;
    const int fg_r = dark ? 0xee : 0x17, fg_g = dark ? 0xf4 : 0x22, fg_b = dark ? 0xfb : 0x32;
    const int dim_r = dark ? 0x6f : 0x7a, dim_g = dark ? 0x80 : 0x89, dim_b = dark ? 0x94 : 0x99;

    html_view_fill_rect(0, 0, 320, 26, bg_r, bg_g, bg_b, 255);
    html_view_draw_text_px(8, 5, g_stat_time, 15, 0, fg_r, fg_g, fg_b, 255);

    const int bat_x = 279, bat_y = 5, bat_w = 35, bat_h = 16;
    const int tip_x = bat_x + bat_w, tip_y = bat_y + 5;
    const int sig_y = 3, sig_h = 18;
    const int sig_left = bat_x - 49;
    static const int bh[5] = { 7, 7, 7, 7, 7 };
    static const int bx[5] = { 18, 23, 28, 33, 38 };
    static const int bw[5] = { 4, 4, 4, 4, 4 };
    const int bar_left = sig_left + bx[0];
    const int bar_right = sig_left + bx[4] + bw[4];
    const int group_gap = bat_x - bar_right;
    int sw = html_view_text_width_px(g_stat_speed, 12);
    int sx = bar_left - group_gap - sw; if (sx < 84) sx = 84;
    html_view_draw_text_px(sx, 6, g_stat_speed, 12, 0, fg_r, fg_g, fg_b, 255);

    const int base = sig_y + sig_h + 1;
    for (int i = 0; i < 5; i++) {
        int on = i < g_stat_sig;
        int r = on ? fg_r : dim_r, g = on ? fg_g : dim_g, b = on ? fg_b : dim_b;
        int x = sig_left + bx[i];
        html_view_fill_round_rect(x, base - bh[i], bw[i], bh[i], 2, r, g, b, 255);
    }
    int gen_size = 11, gw = html_view_text_width_px(g_stat_gen, gen_size);
    if (gw > 24) { gen_size = 10; gw = html_view_text_width_px(g_stat_gen, gen_size); }
    if (gw > 24) { gen_size = 9; }
    gw = html_view_text_width_px(g_stat_gen, gen_size);
    int gx = bar_left + ((bar_right - bar_left) - gw) / 2;
    html_view_draw_text_px(gx, sig_y, g_stat_gen, gen_size, 0, fg_r, fg_g, fg_b, 255);

    html_view_fill_round_rect(bat_x, bat_y, bat_w, bat_h, 4, fg_r, fg_g, fg_b, 255);
    if (dark)
        html_view_fill_round_rect(bat_x + 1, bat_y + 1, bat_w - 2, bat_h - 2, 3, bg_r, bg_g, bg_b, 255);
    else
        html_view_fill_round_rect(bat_x + 1, bat_y + 1, bat_w - 2, bat_h - 2, 3, 0xcb, 0xd7, 0xe4, 255);
    html_view_fill_round_rect(tip_x, tip_y, 2, 7, 1, fg_r, fg_g, fg_b, 255);

    int bat_pct = clampi(g_stat_bat, 0, 100);
    int draw_charging = g_charging;
    int fr = fg_r, fgc = fg_g, fb = fg_b;
    if (draw_charging) { fr = 0x55; fgc = 0xbc; fb = 0x7b; }
    else if (g_stat_lowbat) { fr = 0xd4; fgc = 0x5c; fb = 0x55; }
    int fill_w = (bat_w - 4) * bat_pct / 100;
    if (fill_w > 0)
        html_view_fill_round_rect(bat_x + 2, bat_y + 2, fill_w, bat_h - 4, 2, fr, fgc, fb, 255);

    if (g_show_batpct) {
        char bp[8]; snprintf(bp, sizeof bp, "%d", bat_pct);
        int x0, y0, x1, y1;
        int pct_size = 12;
        html_view_text_bounds_px(bp, pct_size, &x0, &y0, &x1, &y1);
        if (x1 - x0 > bat_w - 4) {
            pct_size = 11;
            html_view_text_bounds_px(bp, pct_size, &x0, &y0, &x1, &y1);
        }
        int tx = bat_x + (bat_w - (x1 - x0)) / 2 - x0;
        int ty = bat_y + (bat_h - (y1 - y0)) / 2 - y0 + 1;
        html_view_draw_text_contrast_px(tx, ty, bp, pct_size, 0,
                                        0x12, 0x25, 0x18,
                                        0xf2, 0xf5, 0xf7, 255);
    }
}

static void draw_center_text_px(int cx, int y, const char *text, int size, int bold,
                                int r, int g, int b, int a)
{
    int x0, y0, x1, y1;
    html_view_text_bounds_px(text, size, &x0, &y0, &x1, &y1);
    html_view_draw_text_px(cx - (x1 - x0) / 2 - x0, y - y0, text, size, bold, r, g, b, a);
}

struct charge_boot_state {
    int pct;
    int charger;
    int charging;
    int full;
    long chg_uv;
    long chg_ua;
};

static void charge_boot_refresh(struct charge_boot_state *s)
{
    static devui_data_t d;
    char st[32];
    long v;
    memset(s, 0, sizeof *s);
    s->pct = -1;

    if (data_refresh(&d)) {
        s->pct = d.bat_percent;
        s->charger = d.charger_connect ? 1 : 0;
        s->charging = (d.charger_connect || d.charging) ? 1 : 0;
        s->chg_uv = d.chg_uv;
        s->chg_ua = d.chg_ua;
    }
    if (s->pct < 0 && read_long_path("/sys/class/power_supply/battery/capacity", &v))
        s->pct = (int)v;
    if (read_line_path("/sys/class/power_supply/battery/status", st, sizeof st)) {
        if (!strcmp(st, "Full")) s->full = 1;
        if (!strcmp(st, "Charging")) s->charging = 1;
    }
    if (!s->charger && read_long_path("/sys/class/power_supply/usb/online", &v))
        s->charger = v != 0;
    if (!s->charger && read_long_path("/sys/class/power_supply/usb/present", &v))
        s->charger = v != 0;
    if (!s->charger && (s->charging || s->full)) s->charger = 1;
    if (s->pct < 0) s->pct = 0;
    if (s->pct > 100) s->pct = 100;
}

static void render_charge_boot(drm_disp_t *disp)
{
    struct charge_boot_state s;
    const int W = disp->width, H = disp->height;
    const int bx = 54, by = 130, bw = 208, bh = 104;
    const int ix = bx + 7, iy = by + 7, iw = bw - 14, ih = bh - 14;
    const int tipw = 10, tiph = 32;
    int fill_w, pulse, fr, fg, fb;
    char pct[8], line1[32], line2[64];

    charge_boot_refresh(&s);
    g_phase++;

    html_view_fill_rect(0, 0, W, H, 0x09, 0x0d, 0x14, 255);
    html_view_fill_round_rect(-28, 36, 154, 154, 72, 0x18, 0x24, 0x38, 88);
    html_view_fill_round_rect(184, 282, 156, 156, 78, 0x0f, 0x2a, 0x22, 72);
    draw_center_text_px(W / 2, 50, "关机充电", 20, 0, 0xd9, 0xdf, 0xea, 255);

    html_view_fill_round_rect(bx, by, bw, bh, 26, 0xd9, 0xdf, 0xea, 255);
    html_view_fill_round_rect(bx + 3, by + 3, bw - 6, bh - 6, 23, 0x0f, 0x14, 0x1d, 255);
    html_view_fill_round_rect(bx + bw, by + (bh - tiph) / 2, tipw, tiph, 4, 0xd9, 0xdf, 0xea, 255);

    pulse = (int)(g_phase % 24);
    if (pulse > 12) pulse = 24 - pulse;
    fr = 0x5e; fg = 0xc8; fb = 0x5e;
    if (s.full) { fr = 0x58; fg = 0xb7; fb = 0xff; }
    else if (s.pct <= 20 && !s.charger) { fr = 0xe8; fg = 0x53; fb = 0x3a; }
    if (s.charger && !s.full) {
        fr = clampi(fr + pulse * 3, 0, 255);
        fg = clampi(fg + pulse * 2, 0, 255);
        fb = clampi(fb + pulse * 3 / 2, 0, 255);
    }
    fill_w = iw * s.pct / 100;
    if (fill_w <= 0 && s.charger) fill_w = 8;
    if (fill_w > 0) {
        html_view_fill_round_rect(ix, iy, fill_w, ih, 18, fr, fg, fb, 255);
        if (s.charger && !s.full && fill_w > 20) {
            int sheen = ix + ((int)(g_phase * 11) % (fill_w + 28)) - 28;
            if (sheen < ix) sheen = ix;
            if (sheen < ix + fill_w)
                html_view_fill_rect(sheen, iy + 6, 18, ih - 12, 0xff, 0xff, 0xff, 42);
        }
    }

    if (s.charger) {
        int cx = W / 2, cy = by + bh / 2;
        int xs[7] = { cx - 12, cx + 2, cx - 3, cx + 11, cx - 8, cx - 2, cx - 11 };
        int ys[7] = { cy - 30, cy - 30, cy - 6, cy - 6, cy + 30, cy + 5, cy + 5 };
        int shx[7], shy[7];
        int br = s.full ? 0xf5 : 0xff;
        int bg = s.full ? 0xfb : 0xf4;
        int bb = s.full ? 0xff : 0xa7;
        for (int i = 0; i < 7; i++) { shx[i] = xs[i] + 1; shy[i] = ys[i] + 2; }
        html_view_fill_poly(shx, shy, 7, 0x05, 0x08, 0x0c, 120);
        html_view_fill_poly(xs, ys, 7, br, bg, bb, 245);
    }

    snprintf(pct, sizeof pct, "%d%%", s.pct);
    draw_center_text_px(W / 2, 274, pct, 46, 1, 0xf5, 0xf7, 0xfa, 255);

    if (s.full) snprintf(line1, sizeof line1, "已充满");
    else if (s.charging) snprintf(line1, sizeof line1, "充电中");
    else if (s.charger) snprintf(line1, sizeof line1, "已接入电源");
    else snprintf(line1, sizeof line1, "等待充电");
    draw_center_text_px(W / 2, 330, line1, 20, 0, 0x9f, 0xd6, 0xb5, 255);

    if (s.chg_uv > 0 && s.chg_ua > 0)
        snprintf(line2, sizeof line2, "输入 %.1fV · %ldmA", s.chg_uv / 1000000.0, s.chg_ua / 1000);
    else
        snprintf(line2, sizeof line2, "按电源键可熄屏/亮屏");
    draw_center_text_px(W / 2, 364, line2, 14, 0, 0x8e, 0x98, 0xa6, 255);

    drm_disp_dirty(disp, 0, 0, W - 1, H - 1);
    maybe_dump_fb(disp);
}

static int  g_modal;           /* 0 none, 1 SA, 2 NSA, 3 LTE, 4 signal settings */
static int  g_pwr_confirm;     /* power menu: 0 none, 1 poweroff armed, 2 reboot armed */
static uint32_t g_pwr_until;   /* millis the armed state auto-resets at */
static char g_net_pending[16]; /* optimistic net mode until net_select catches up */
static char g_uni_sa[256], g_uni_nsa[256], g_uni_lte[256];   /* available bands (max seen) */
static char g_sel_sa[256], g_sel_nsa[256], g_sel_lte[256];   /* selected (to lock) */

/* ---- dual-SIM slot switch (U60 Pro dual-SIM models only) ----
 * Slot 1 is the removable/self-inserted SIM; slot 2 is the built-in SIM. The
 * stock modem API may leave the inactive slot's SIM fields empty, so presence
 * is determined only by support_dual_sim and never by sim2_states. */
#define SIM_SWITCH_RC  "/tmp/u60-devui-sim-switch.rc"
#define SIM_SWITCH_LOG "/tmp/u60-devui-sim-switch.log"
static int g_sim_dual = -1;          /* -1 unknown, 0 single-SIM, 1 dual-SIM */
static int g_sim_slot = -1;          /* 1 removable, 2 built-in */
static int g_sim_confirm_slot;
static int g_sim_switch_target;
static int g_sim_switching;
static uint32_t g_sim_refresh_at;
static uint32_t g_sim_confirm_until;
static uint32_t g_sim_switch_until;
enum sim_operation_kind {
    SIM_OPERATION_NONE = 0,
    SIM_OPERATION_SLOT,
    SIM_OPERATION_SINGLE,
    SIM_OPERATION_DUAL,
    SIM_OPERATION_AUTO
};
static enum sim_operation_kind g_sim_operation;
static int g_sim_operation_value;
static int g_sim_p1 = -1, g_sim_p2 = -1, g_sim_auto = -1;
static char g_sim_state1[32] = "-", g_sim_state2[32] = "-";
static char g_sim_operator1[48] = "-", g_sim_operator2[48] = "-";
static int g_sim_mode_confirm;
static uint32_t g_sim_mode_confirm_until;

static const char *sim_slot_label(int slot)
{
    return slot == 1 ? "自插卡" : slot == 2 ? "内置卡" : "未知卡槽";
}

static int sim_switch_rc_read(int *rc)
{
    char line[24];
    char *end = NULL;
    long value;
    if (!read_line_path(SIM_SWITCH_RC, line, sizeof line)) return 0;
    value = strtol(line, &end, 10);
    if (end == line) return 0;
    *rc = (int)value;
    return 1;
}

static void sim_switch_finish(uint32_t now, int ok, const char *reason)
{
    int target = g_sim_switch_target;
    enum sim_operation_kind operation = g_sim_operation;
    char note[96];
    g_sim_switching = 0;
    g_sim_switch_target = 0;
    g_sim_operation = SIM_OPERATION_NONE;
    g_sim_operation_value = 0;
    g_sim_switch_until = 0;
    unlink(SIM_SWITCH_RC);
    if (ok) {
        if (operation == SIM_OPERATION_SLOT)
            snprintf(g_toast, sizeof g_toast, "已切换到%s", sim_slot_label(target));
        else if (operation == SIM_OPERATION_SINGLE)
            snprintf(g_toast, sizeof g_toast, "已切换为单卡模式");
        else if (operation == SIM_OPERATION_DUAL)
            snprintf(g_toast, sizeof g_toast, "已切换为双卡双待");
        else if (operation == SIM_OPERATION_AUTO)
            snprintf(g_toast, sizeof g_toast, g_sim_auto == 1 ? "智能切换已开启" : "智能切换已关闭");
        else
            snprintf(g_toast, sizeof g_toast, "双卡操作已完成");
        snprintf(note, sizeof note, "操作成功：%s", g_toast);
    } else {
        snprintf(g_toast, sizeof g_toast, "%s", reason ? reason : "SIM 卡切换失败");
        snprintf(note, sizeof note, "操作失败：%s", g_toast);
    }
    plugin_action_note(SIM_ACTION_LOG, note);
    g_toast_until = now + 2200;
}

/* Returns 1 when visible SIM state changed. A failed read preserves the last
 * known dual-SIM capability and slot so a transient modem restart does not
 * make the controls disappear or falsely highlight the target slot. */
static int sim_slot_refresh(uint32_t now, int force)
{
    uint32_t interval = g_sim_switching ? 1000U : 5000U;
    FILE *fp;
    char line[128];
    char state1[32] = "", state2[32] = "", operator1[48] = "", operator2[48] = "";
    int dual = -1, slot = -1, p1 = -1, p2 = -1, auto_switch = -1;
    int old_dual = g_sim_dual, old_slot = g_sim_slot;
    int old_switching = g_sim_switching;
    int old_p1 = g_sim_p1, old_p2 = g_sim_p2, old_auto = g_sim_auto;
    int rc;

    if (!force && g_sim_refresh_at && now - g_sim_refresh_at < interval) return 0;
    g_sim_refresh_at = now;
    fp = popen(
        "j=$(ubus -t 3 call zwrt_zte_mdm.api get_sim_info_before '{}' 2>/dev/null); "
        "printf 'DUAL=%s\\nSLOT=%s\\nP1=%s\\nP2=%s\\nSTATE1=%s\\nSTATE2=%s\\nOP1=%s\\nOP2=%s\\nAUTO=%s\\n' "
        "\"$(printf '%s' \"$j\" | jsonfilter -e '@.support_dual_sim' 2>/dev/null)\" "
        "\"$(printf '%s' \"$j\" | jsonfilter -e '@.current_sim_slot' 2>/dev/null)\" "
        "\"$(printf '%s' \"$j\" | jsonfilter -e '@.sim1_provision_state' 2>/dev/null)\" "
        "\"$(printf '%s' \"$j\" | jsonfilter -e '@.sim2_provision_state' 2>/dev/null)\" "
        "\"$(printf '%s' \"$j\" | jsonfilter -e '@.sim_states' 2>/dev/null)\" "
        "\"$(printf '%s' \"$j\" | jsonfilter -e '@.sim2_states' 2>/dev/null)\" "
        "\"$(printf '%s' \"$j\" | jsonfilter -e '@.Operator' 2>/dev/null)\" "
        "\"$(printf '%s' \"$j\" | jsonfilter -e '@.Operator2' 2>/dev/null)\" "
        "\"$(uci -q get zwrt_zte_mdm.sim_info.st_auto_switch_card_flag 2>/dev/null)\"",
        "r");
    if (fp) {
        while (fgets(line, sizeof line, fp)) {
            if (!strncmp(line, "DUAL=", 5) && (line[5] == '0' || line[5] == '1')) dual = line[5] - '0';
            else if (!strncmp(line, "SLOT=", 5) && (line[5] == '1' || line[5] == '2')) slot = line[5] - '0';
            else if (!strncmp(line, "P1=", 3) && (line[3] == '0' || line[3] == '1')) p1 = line[3] - '0';
            else if (!strncmp(line, "P2=", 3) && (line[3] == '0' || line[3] == '1')) p2 = line[3] - '0';
            else if (!strncmp(line, "STATE1=", 7)) line_value(state1, sizeof state1, line, 7);
            else if (!strncmp(line, "STATE2=", 7)) line_value(state2, sizeof state2, line, 7);
            else if (!strncmp(line, "OP1=", 4)) line_value(operator1, sizeof operator1, line, 4);
            else if (!strncmp(line, "OP2=", 4)) line_value(operator2, sizeof operator2, line, 4);
            else if (!strncmp(line, "AUTO=", 5) && (line[5] == '0' || line[5] == '1')) auto_switch = line[5] - '0';
        }
        pclose(fp);
    }
    if (dual >= 0) g_sim_dual = dual;
    if (slot >= 0) g_sim_slot = slot;
    if (p1 >= 0) g_sim_p1 = p1;
    if (p2 >= 0) g_sim_p2 = p2;
    if (auto_switch >= 0) g_sim_auto = auto_switch;
    if (state1[0] && strcmp(state1, "-")) snprintf(g_sim_state1, sizeof g_sim_state1, "%s", state1);
    if (state2[0] && strcmp(state2, "-")) snprintf(g_sim_state2, sizeof g_sim_state2, "%s", state2);
    if (operator1[0] && strcmp(operator1, "-")) snprintf(g_sim_operator1, sizeof g_sim_operator1, "%s", operator1);
    if (operator2[0] && strcmp(operator2, "-")) snprintf(g_sim_operator2, sizeof g_sim_operator2, "%s", operator2);

    if (g_sim_switching) {
        int complete = 0;
        if (g_sim_operation == SIM_OPERATION_SLOT)
            complete = g_sim_slot == g_sim_switch_target;
        else if (g_sim_operation == SIM_OPERATION_SINGLE)
            complete = (g_sim_p1 + g_sim_p2) == 1 &&
                       ((g_sim_slot == 1 && g_sim_p1 == 1) || (g_sim_slot == 2 && g_sim_p2 == 1));
        else if (g_sim_operation == SIM_OPERATION_DUAL)
            complete = g_sim_p1 == 1 && g_sim_p2 == 1;
        else if (g_sim_operation == SIM_OPERATION_AUTO)
            complete = g_sim_auto == g_sim_operation_value;
        if (complete) {
            sim_switch_finish(now, 1, NULL);
        } else if (sim_switch_rc_read(&rc) && rc != 0) {
            sim_switch_finish(now, 0, "SIM 卡切换命令失败");
        } else if ((int32_t)(now - g_sim_switch_until) >= 0) {
            sim_switch_finish(now, 0, "SIM 卡切换超时");
        }
    }
    return old_dual != g_sim_dual || old_slot != g_sim_slot || old_switching != g_sim_switching ||
           old_p1 != g_sim_p1 || old_p2 != g_sim_p2 || old_auto != g_sim_auto;
}

static int sim_operation_start(enum sim_operation_kind operation, int value, uint32_t now)
{
    char cmd[1024], action[768];
    int current = g_sim_slot == 2 ? 2 : 1;
    int other = current == 1 ? 2 : 1;
    const char *label;

    action[0] = 0;
    if (operation == SIM_OPERATION_SLOT && (value == 1 || value == 2)) {
        snprintf(action, sizeof action,
                 "ubus -t 20 call zwrt_zte_mdm.api zwrt_zte_mdm_activate_sim "
                 "'{\"sim_card_id\":%d}'", value);
        label = value == 1 ? "切换到自插卡" : "切换到内置卡";
    } else if (operation == SIM_OPERATION_SINGLE) {
        snprintf(action, sizeof action,
                 "ubus -t 20 call zwrt_zte_mdm.api zwrt_mdm_change_provision_session "
                 "'{\"active_slot\":%d,\"active_flag\":1}' && "
                 "ubus -t 20 call zwrt_zte_mdm.api zwrt_mdm_change_provision_session "
                 "'{\"active_slot\":%d,\"active_flag\":0}'",
                 current, other);
        label = "切换为单卡模式";
    } else if (operation == SIM_OPERATION_DUAL) {
        snprintf(action, sizeof action,
                 "ubus -t 20 call zwrt_zte_mdm.api zwrt_mdm_change_provision_session "
                 "'{\"active_slot\":1,\"active_flag\":1}' && "
                 "ubus -t 20 call zwrt_zte_mdm.api zwrt_mdm_change_provision_session "
                 "'{\"active_slot\":2,\"active_flag\":1}'");
        label = "切换为双卡双待";
    } else if (operation == SIM_OPERATION_AUTO && (value == 0 || value == 1)) {
        snprintf(action, sizeof action,
                 "ubus -t 15 call zwrt_zte_mdm.api st_set_auto_switch_slot "
                 "'{\"auto_switch_slot_flag\":%d}'", value);
        label = value ? "开启智能切换" : "关闭智能切换";
    } else {
        return -1;
    }
    unlink(SIM_SWITCH_RC);
    unlink(SIM_SWITCH_LOG);
    if (snprintf(cmd, sizeof cmd,
                 "(%s >" SIM_SWITCH_LOG " 2>&1; echo $? >" SIM_SWITCH_RC ") &",
                 action) >= (int)sizeof cmd)
        return -1;
    if (system(cmd) != 0) return -1;
    g_sim_switching = 1;
    g_sim_operation = operation;
    g_sim_operation_value = value;
    g_sim_switch_target = operation == SIM_OPERATION_SLOT ? value : 0;
    g_sim_switch_until = now + 60000U;
    g_sim_confirm_slot = 0;
    g_sim_confirm_until = 0;
    g_sim_mode_confirm = 0;
    g_sim_mode_confirm_until = 0;
    g_sim_refresh_at = 0;
    snprintf(g_toast, sizeof g_toast, "%s中", label);
    plugin_action_note(SIM_ACTION_LOG, label);
    g_toast_until = now + 2200;
    return 0;
}

static int sim_switch_start(int slot, uint32_t now)
{
    return sim_operation_start(SIM_OPERATION_SLOT, slot, now);
}

static void sim_slot_action(int slot, uint32_t now)
{
    (void)sim_slot_refresh(now, 1);
    if (g_sim_dual != 1) {
        snprintf(g_toast, sizeof g_toast, "当前设备不支持双卡切换");
        g_toast_until = now + 1800;
        return;
    }
    if (g_sim_switching) {
        snprintf(g_toast, sizeof g_toast, "SIM 卡正在切换中");
        g_toast_until = now + 1800;
        return;
    }
    if (slot == g_sim_slot) {
        g_sim_confirm_slot = 0;
        g_sim_confirm_until = 0;
        snprintf(g_toast, sizeof g_toast, "当前正在使用%s", sim_slot_label(slot));
        g_toast_until = now + 1800;
        return;
    }
    if (g_sim_confirm_slot == slot && (int32_t)(g_sim_confirm_until - now) > 0) {
        if (sim_switch_start(slot, now) != 0) {
            snprintf(g_toast, sizeof g_toast, "无法启动 SIM 卡切换");
            g_toast_until = now + 1800;
        }
        return;
    }
    g_sim_confirm_slot = slot;
    g_sim_mode_confirm = 0;
    g_sim_confirm_until = now + 4000U;
    snprintf(g_toast, sizeof g_toast, "请再次点击%s确认", sim_slot_label(slot));
    g_toast_until = now + 1800;
}

static void sim_mode_action(int dual_mode, uint32_t now)
{
    int target = dual_mode ? 2 : 1;
    (void)sim_slot_refresh(now, 1);
    if (g_sim_dual != 1) {
        snprintf(g_toast, sizeof g_toast, "当前设备不支持双卡管理");
    } else if (g_sim_switching) {
        snprintf(g_toast, sizeof g_toast, "已有双卡操作正在执行");
    } else if ((!dual_mode && (g_sim_p1 + g_sim_p2) == 1) ||
               (dual_mode && g_sim_p1 == 1 && g_sim_p2 == 1)) {
        snprintf(g_toast, sizeof g_toast, dual_mode ? "当前已是双卡双待" : "当前已是单卡模式");
        g_sim_mode_confirm = 0;
    } else if (g_sim_mode_confirm == target && (int32_t)(g_sim_mode_confirm_until - now) > 0) {
        if (sim_operation_start(dual_mode ? SIM_OPERATION_DUAL : SIM_OPERATION_SINGLE, target, now) != 0)
            snprintf(g_toast, sizeof g_toast, "无法启动双卡模式切换");
    } else {
        g_sim_mode_confirm = target;
        g_sim_mode_confirm_until = now + 4000U;
        g_sim_confirm_slot = 0;
        snprintf(g_toast, sizeof g_toast,
                 dual_mode ? "请再次点击确认双卡双待" : "请再次点击确认单卡模式");
    }
    g_toast_until = now + 1800;
}

static void sim_auto_action(int enabled, uint32_t now)
{
    (void)sim_slot_refresh(now, 1);
    if (g_sim_dual != 1)
        snprintf(g_toast, sizeof g_toast, "当前设备不支持双卡管理");
    else if (g_sim_switching)
        snprintf(g_toast, sizeof g_toast, "已有双卡操作正在执行");
    else if (g_sim_auto == enabled)
        snprintf(g_toast, sizeof g_toast, enabled ? "智能切换已经开启" : "智能切换已经关闭");
    else if (sim_operation_start(SIM_OPERATION_AUTO, enabled, now) != 0)
        snprintf(g_toast, sizeof g_toast, "无法修改智能切换状态");
    g_toast_until = now + 1800;
}

static int dual_sim_management_status(void)
{
    (void)sim_slot_refresh(millis(), 0);
    return g_sim_dual;
}

static int sim_traffic_page_status(void)
{
    devui_data_t data;
    if (!data_refresh_live(&data)) return -1;
    return data.sim_traffic_available ? 1 : 0;
}

static int band_count(const char *s) { if (!s[0]) return 0; int n = 1; for (; *s; s++) if (*s == ',') n++; return n; }

static int band_in(const char *list, const char *b)
{
    size_t bl = strlen(b);
    for (const char *p = list; *p; ) {
        const char *c = p; while (*c && *c != ',') c++;
        if ((size_t)(c - p) == bl && !strncmp(p, b, bl)) return 1;
        p = *c ? c + 1 : c;
    }
    return 0;
}
static void band_toggle(char *list, size_t cap, const char *b)
{
    if (band_in(list, b)) {
        char out[256]; int o = 0; size_t bl = strlen(b);
        for (const char *p = list; *p; ) {
            const char *c = p; while (*c && *c != ',') c++;
            if (!((size_t)(c - p) == bl && !strncmp(p, b, bl))) {
                if (o) out[o++] = ',';
                int n = (int)(c - p); if (o + n < (int)sizeof out) { memcpy(out + o, p, n); o += n; }
            }
            p = *c ? c + 1 : c;
        }
        out[o] = 0; snprintf(list, cap, "%s", out);
    } else {
        size_t l = strlen(list);
        snprintf(list + l, cap - l, "%s%s", l ? "," : "", b);
    }
}
/* Render bands as pill chips (selected = highlighted) with the given classes. */
static void build_chips_cls(char *buf, size_t cap, const char *uni, const char *sel,
                            const char *act, const char *pfx, const char *on, const char *off)
{
    char u[256]; snprintf(u, sizeof u, "%s", uni);
    int o = 0;
    for (char *tk = strtok(u, ","); tk; tk = strtok(NULL, ",")) {
        o += snprintf(buf + o, cap - o, "<a href='act:%s:%s' class='%s'>%s%s</a>",
                      act, tk, band_in(sel, tk) ? on : off, pfx, tk);
    }
    if (o == 0) snprintf(buf, cap, "<span class='muted'>暂无可选频段</span>");
}

/* One-line summary of a card's current band selection. */
static void band_summary(char *buf, size_t cap, const char *uni, const char *sel, const char *pfx)
{
    if (!sel[0]) { snprintf(buf, cap, "未选择频段"); return; }
    if (!strcmp(uni, sel)) { snprintf(buf, cap, "全部频段（未锁定）"); return; }
    char s[256]; snprintf(s, sizeof s, "%s", sel);
    int o = 0;
    for (char *tk = strtok(s, ","); tk; tk = strtok(NULL, ","))
        o += snprintf(buf + o, cap - o, "%s%s%s", o ? " " : "已锁 ", pfx, tk);
}

/* segmented-control cells (shared with the touch handler). */
static const struct { const char *v, *lab; } g_netmodes[4] = {
    { "WL_AND_5G", "自动" }, { "Only_5G", "5G SA" },
    { "LTE_AND_5G", "5G NSA" }, { "Only_LTE", "4G" } };
static const struct { int ms; const char *lab; } g_autooffs[6] = {
    { 0, "关" }, { 30000, "30秒" }, { 60000, "1分" },
    { 120000, "2分" }, { 300000, "5分" }, { 600000, "10分" } };
static const struct { int ms; const char *lab; } g_refresh_rates[5] = {
    { 0, "停止" }, { 500, "0.5s" }, { 1000, "1s" }, { 2000, "2s" }, { 5000, "5s" } };
static int normalize_refresh_ms(int ms)
{
    switch (ms) {
    case 0:
    case 500:
    case 1000:
    case 2000:
    case 5000:
        return ms;
    default:
        return 1000;
    }
}

static int normalize_chart_sec(int sec)
{
    switch (sec) {
    case 30:
    case 48:
    case 60:
    case 120:
    case 300:
        return sec;
    default:
        return 48;
    }
}

static int datad_set_refresh_ms(int ms)
{
    char cmd[192];

    ms = normalize_refresh_ms(ms);
    if (snprintf(cmd, sizeof cmd,
                 "/usr/bin/wget -qO- --post-data='' 'http://%s:%d/modem/control?poll_ms=%d' >/dev/null 2>&1",
                 DATAD_HTTP_ADDR, DATAD_HTTP_PORT, ms) >= (int)sizeof cmd)
        return -1;
    return system(cmd) == 0 ? 0 : -1;
}

static int datad_set_signal_read(int on)
{
    char cmd[224];
    int active = on || g_sig_parse;

    if (snprintf(cmd, sizeof cmd,
                 "/usr/bin/wget -qO- --post-data='' 'http://%s:%d/modem/control?ml1_raw=%d&active=%d' >/dev/null 2>&1",
                 DATAD_HTTP_ADDR, DATAD_HTTP_PORT, on ? 1 : 0, active ? 1 : 0) >= (int)sizeof cmd)
        return -1;
    return system(cmd) == 0 ? 0 : -1;
}

static int datad_set_signal_parse(int on)
{
    char cmd[256];
    int active = g_sig_read || on;

    if (snprintf(cmd, sizeof cmd,
                 "/usr/bin/wget -qO- --post-data='' 'http://%s:%d/modem/control?lte=%d&nr=%d&active=%d' >/dev/null 2>&1",
                 DATAD_HTTP_ADDR, DATAD_HTTP_PORT, on ? 1 : 0, on ? 1 : 0, active ? 1 : 0) >= (int)sizeof cmd)
        return -1;
    return system(cmd) == 0 ? 0 : -1;
}

#define SIGNAL_CACHE_PATH "/data/plugins/u60pro-devui/signal.cache"

static int signal_cache_get(const char *key, char *dst, size_t cap)
{
    FILE *fp;
    char line[192];
    size_t key_len;
    int found = 0;

    if (!key || !dst || cap == 0)
        return 0;
    key_len = strlen(key);
    fp = fopen(SIGNAL_CACHE_PATH, "r");
    if (!fp)
        return 0;
    while (fgets(line, sizeof line, fp)) {
        char *v;
        char *e;

        if (strncmp(line, key, key_len) || line[key_len] != '=')
            continue;
        v = line + key_len + 1;
        e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
            *--e = '\0';
        if (*v) {
            snprintf(dst, cap, "%s", v);
            found = 1;
        }
    }
    fclose(fp);
    return found;
}

static void signal_cache_put(const char *key, const char *value)
{
    FILE *in;
    FILE *out;
    char line[256];
    char old[128];
    char tmp_path[sizeof(SIGNAL_CACHE_PATH) + 8];
    size_t key_len;

    if (!key || !value || !value[0] || !strcmp(value, "-") ||
        !strcmp(value, "已关闭"))
        return;
    if (strchr(key, '=') || strchr(key, '\n') || strchr(value, '\n') ||
        strchr(value, '\r'))
        return;
    if (signal_cache_get(key, old, sizeof old) && !strcmp(old, value))
        return;
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", SIGNAL_CACHE_PATH);
    out = fopen(tmp_path, "w");
    if (!out)
        return;
    key_len = strlen(key);
    in = fopen(SIGNAL_CACHE_PATH, "r");
    if (in) {
        while (fgets(line, sizeof line, in)) {
            if (!strncmp(line, key, key_len) && line[key_len] == '=')
                continue;
            fputs(line, out);
        }
        fclose(in);
    }
    fprintf(out, "%s=%s\n", key, value);
    if (fclose(out) != 0 || rename(tmp_path, SIGNAL_CACHE_PATH) != 0)
        unlink(tmp_path);
}

struct signal_bundle {
    char cell_key[64];
    char ta[32];
    char lte_ta[32];
    char lte_rrc[128];
    char lte_nas[128];
    char lte_mcs[32];
    char lte_modulation[32];
    char lte_rb[32];
    char lte_grants[32];
    char lte_bler[32];
    char lte_pusch[96];
    char lte_pucch[96];
    char nr_ta[32];
    char nr_rrc[128];
    char nr_nas[128];
    char nr_mcs[32];
    char nr_modulation[32];
    char nr_mimo[64];
    char nr_layers[32];
    char nr_dl_rb[32];
    char nr_dl_grants[32];
    char nr_ul_rb[32];
    char nr_ul_grants[32];
    char nr_bler[32];
    char nr_pusch[96];
    char nr_pucch[96];
    char nr_ports[64];
    char nr_beam[128];
    char nr_serving_ssb[32];
    char nr_vendor[64];
    int lte_carrier_n;
    int nr_carrier_n;
    int lte_neighbor_n;
    int nr_neighbor_n;
    struct signal_carrier_metric lte_carriers[SIGNAL_MAX_CARRIERS];
    struct signal_carrier_metric nr_carriers[SIGNAL_MAX_CARRIERS];
    struct signal_neighbor_metric lte_neighbors[SIGNAL_MAX_NEIGHBORS];
    struct signal_neighbor_metric nr_neighbors[SIGNAL_MAX_NEIGHBORS];
};

static void signal_bundle_reset(struct signal_bundle *b)
{
    if (!b) return;
    snprintf(b->cell_key, sizeof b->cell_key, "-");
    snprintf(b->ta, sizeof b->ta, "-");
    snprintf(b->lte_ta, sizeof b->lte_ta, "-");
    snprintf(b->lte_rrc, sizeof b->lte_rrc, "-");
    snprintf(b->lte_nas, sizeof b->lte_nas, "-");
    snprintf(b->lte_mcs, sizeof b->lte_mcs, "-");
    snprintf(b->lte_modulation, sizeof b->lte_modulation, "-");
    snprintf(b->lte_rb, sizeof b->lte_rb, "-");
    snprintf(b->lte_grants, sizeof b->lte_grants, "-");
    snprintf(b->lte_bler, sizeof b->lte_bler, "-");
    snprintf(b->lte_pusch, sizeof b->lte_pusch, "-");
    snprintf(b->lte_pucch, sizeof b->lte_pucch, "-");
    snprintf(b->nr_ta, sizeof b->nr_ta, "-");
    snprintf(b->nr_rrc, sizeof b->nr_rrc, "-");
    snprintf(b->nr_nas, sizeof b->nr_nas, "-");
    snprintf(b->nr_mcs, sizeof b->nr_mcs, "-");
    snprintf(b->nr_modulation, sizeof b->nr_modulation, "-");
    snprintf(b->nr_mimo, sizeof b->nr_mimo, "-");
    snprintf(b->nr_layers, sizeof b->nr_layers, "-");
    snprintf(b->nr_dl_rb, sizeof b->nr_dl_rb, "-");
    snprintf(b->nr_dl_grants, sizeof b->nr_dl_grants, "-");
    snprintf(b->nr_ul_rb, sizeof b->nr_ul_rb, "-");
    snprintf(b->nr_ul_grants, sizeof b->nr_ul_grants, "-");
    snprintf(b->nr_bler, sizeof b->nr_bler, "-");
    snprintf(b->nr_pusch, sizeof b->nr_pusch, "-");
    snprintf(b->nr_pucch, sizeof b->nr_pucch, "-");
    snprintf(b->nr_ports, sizeof b->nr_ports, "-");
    snprintf(b->nr_beam, sizeof b->nr_beam, "-");
    snprintf(b->nr_serving_ssb, sizeof b->nr_serving_ssb, "-");
    snprintf(b->nr_vendor, sizeof b->nr_vendor, "-");
    b->lte_carrier_n = 0;
    b->nr_carrier_n = 0;
    b->lte_neighbor_n = 0;
    b->nr_neighbor_n = 0;
    for (int i = 0; i < SIGNAL_MAX_CARRIERS; i++) {
        signal_carrier_metric_reset(&b->lte_carriers[i]);
        signal_carrier_metric_reset(&b->nr_carriers[i]);
    }
    for (int i = 0; i < SIGNAL_MAX_NEIGHBORS; i++) {
        signal_neighbor_metric_reset(&b->lte_neighbors[i]);
        signal_neighbor_metric_reset(&b->nr_neighbors[i]);
    }
}

static int signal_value_present(const char *s)
{
    return s && s[0] && strcmp(s, "-") != 0 && strcmp(s, "已关闭") != 0;
}

static void signal_value_keep_last(char *cur, size_t cur_cap, char *sticky, size_t sticky_cap)
{
    if (!cur || cur_cap == 0 || !sticky || sticky_cap == 0)
        return;
    if (signal_value_present(cur)) {
        snprintf(sticky, sticky_cap, "%s", cur);
        return;
    }
    if (signal_value_present(sticky))
        snprintf(cur, cur_cap, "%s", sticky);
}

static void signal_metric_get(const char *obj, const char *key, char *dst, size_t cap)
{
    char tmp[128];

    if (!obj || !key || !dst || cap == 0)
        return;
    if (json_get(obj, key, tmp, sizeof tmp) && signal_value_present(tmp))
        snprintf(dst, cap, "%s", tmp);
}

static void signal_copy_value(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (!dst || cap == 0)
        return;
    if (!src)
        src = "";
    n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static int signal_json_next_object(const char **pp, char *dst, size_t cap)
{
    const char *p;
    size_t n = 0;
    int depth = 0;
    int in_str = 0;

    if (!pp || !*pp || !dst || cap == 0)
        return 0;
    p = *pp;
    while (*p && *p != '{')
        p++;
    if (*p != '{')
        return 0;
    while (*p && n < cap - 1) {
        char ch = *p;
        if (in_str) {
            if (ch == '\\' && p[1]) {
                dst[n++] = *p++;
                if (n < cap - 1)
                    dst[n++] = *p++;
                continue;
            }
            if (ch == '"')
                in_str = 0;
        } else {
            if (ch == '"')
                in_str = 1;
            else if (ch == '{')
                depth++;
            else if (ch == '}') {
                depth--;
                dst[n++] = *p++;
                if (depth == 0)
                    break;
                continue;
            }
        }
        dst[n++] = *p++;
    }
    dst[n] = 0;
    *pp = p;
    return depth == 0 && n > 0;
}

static void signal_carrier_metric_get(const char *obj, const char *key,
                                      char *dst, size_t cap)
{
    char tmp[96];

    if (!obj || !key || !dst || cap == 0)
        return;
    if (json_get(obj, key, tmp, sizeof tmp) && signal_value_present(tmp))
        signal_copy_value(dst, cap, tmp);
}

static int signal_parse_carriers(const char *parent, int is_nr,
                                 struct signal_carrier_metric *out, int max)
{
    char arr[16384];
    char obj[1536];
    const char *p;
    int n = 0;

    if (!parent || !out || max <= 0)
        return 0;
    if (!json_get(parent, "carriers", arr, sizeof arr))
        return 0;
    p = arr;
    while (n < max && signal_json_next_object(&p, obj, sizeof obj)) {
        struct signal_carrier_metric *m = &out[n];
        signal_carrier_metric_reset(m);
        m->valid = 1;
        signal_carrier_metric_get(obj, "name", m->name, sizeof m->name);
        signal_carrier_metric_get(obj, "type", m->type, sizeof m->type);
        signal_carrier_metric_get(obj, "pci", m->pci, sizeof m->pci);
        signal_carrier_metric_get(obj, is_nr ? "nrarfcn" : "earfcn", m->arfcn, sizeof m->arfcn);
        signal_carrier_metric_get(obj, "rb", m->rb, sizeof m->rb);
        signal_carrier_metric_get(obj, "grants", m->grants, sizeof m->grants);
        signal_carrier_metric_get(obj, "dl_rb", m->dl_rb, sizeof m->dl_rb);
        signal_carrier_metric_get(obj, "dl_grants", m->dl_grants, sizeof m->dl_grants);
        signal_carrier_metric_get(obj, "ul_rb", m->ul_rb, sizeof m->ul_rb);
        signal_carrier_metric_get(obj, "ul_grants", m->ul_grants, sizeof m->ul_grants);
        signal_carrier_metric_get(obj, "mcs", m->mcs, sizeof m->mcs);
        signal_carrier_metric_get(obj, "modulation", m->modulation, sizeof m->modulation);
        signal_carrier_metric_get(obj, "mimo", m->mimo, sizeof m->mimo);
        signal_carrier_metric_get(obj, "layers", m->layers, sizeof m->layers);
        signal_carrier_metric_get(obj, "bler_pct", m->bler, sizeof m->bler);
        signal_carrier_metric_get(obj, "ssb", m->ssb, sizeof m->ssb);
        signal_carrier_metric_get(obj, "serving_ssb", m->serving_ssb, sizeof m->serving_ssb);
        signal_carrier_metric_get(obj, "ml1_rsrp", m->ml1_rsrp, sizeof m->ml1_rsrp);
        signal_carrier_metric_get(obj, "ml1_rsrq", m->ml1_rsrq, sizeof m->ml1_rsrq);
        signal_carrier_metric_get(obj, "ml1_sinr", m->ml1_sinr, sizeof m->ml1_sinr);
        n++;
    }
    return n;
}

static void signal_lte_band_from_earfcn(const char *earfcn, char *dst, size_t cap)
{
    struct range { long lo, hi; const char *band; };
    static const struct range ranges[] = {
        { 0, 599, "B1" }, { 600, 1199, "B2" }, { 1200, 1949, "B3" },
        { 1950, 2399, "B4" }, { 2400, 2649, "B5" }, { 2750, 3449, "B7" },
        { 3450, 3799, "B8" }, { 5010, 5179, "B12" }, { 5180, 5279, "B13" },
        { 5730, 5849, "B17" }, { 5850, 5999, "B18" }, { 6000, 6149, "B19" },
        { 6150, 6449, "B20" }, { 8690, 9039, "B26" }, { 9210, 9659, "B28" },
        { 36200, 36349, "B34" }, { 37750, 38249, "B38" },
        { 38250, 38649, "B39" }, { 38650, 39649, "B40" },
        { 39650, 41589, "B41" },
    };
    char *end = NULL;
    long v;

    if (!dst || cap == 0) return;
    if (!signal_value_present(earfcn)) { snprintf(dst, cap, "-"); return; }
    v = strtol(earfcn, &end, 10);
    if (end == earfcn || v <= 0) { snprintf(dst, cap, "-"); return; }
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
        if (v >= ranges[i].lo && v <= ranges[i].hi) {
            snprintf(dst, cap, "%s", ranges[i].band);
            return;
        }
    snprintf(dst, cap, "E%.20s", earfcn);
}

static void signal_nr_band_from_nrarfcn(const char *nrarfcn, char *dst, size_t cap)
{
    struct range { long lo, hi; const char *band; };
    static const struct range ranges[] = {
        { 151600, 160600, "n28" }, { 173800, 178800, "n5" },
        { 185000, 192000, "n8" }, { 361000, 376000, "n3" },
        { 422000, 434000, "n1" }, { 460000, 480000, "n40" },
        { 499200, 537999, "n41" }, { 514000, 524000, "n38" },
        { 620000, 653333, "n78" }, { 653334, 680000, "n77" },
        { 636667, 646666, "n48" }, { 693334, 733333, "n79" },
    };
    char *end = NULL;
    long v;

    if (!dst || cap == 0) return;
    if (!signal_value_present(nrarfcn)) { snprintf(dst, cap, "-"); return; }
    v = strtol(nrarfcn, &end, 10);
    if (end == nrarfcn || v <= 0) { snprintf(dst, cap, "-"); return; }
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
        if (v >= ranges[i].lo && v <= ranges[i].hi) {
            snprintf(dst, cap, "%s", ranges[i].band);
            return;
        }
    snprintf(dst, cap, "N%.20s", nrarfcn);
}

static int signal_parse_neighbors(const char *parent, int is_nr,
                                  struct signal_neighbor_metric *out, int max)
{
    char arr[8192];
    char obj[1024];
    const char *p;
    int n = 0;

    if (!parent || !out || max <= 0)
        return 0;
    if (!json_get(parent, "neighbors", arr, sizeof arr))
        return 0;
    p = arr;
    while (n < max && signal_json_next_object(&p, obj, sizeof obj)) {
        struct signal_neighbor_metric *m = &out[n];
        signal_neighbor_metric_reset(m);
        signal_carrier_metric_get(obj, "pci", m->pci, sizeof m->pci);
        signal_carrier_metric_get(obj, "band", m->band, sizeof m->band);
        signal_carrier_metric_get(obj, is_nr ? "nrarfcn" : "earfcn", m->arfcn, sizeof m->arfcn);
        if (!signal_value_present(m->arfcn))
            signal_carrier_metric_get(obj, "arfcn", m->arfcn, sizeof m->arfcn);
        signal_carrier_metric_get(obj, "rsrp", m->rsrp, sizeof m->rsrp);
        signal_carrier_metric_get(obj, "rsrq", m->rsrq, sizeof m->rsrq);
        if (!signal_value_present(m->band)) {
            if (is_nr)
                signal_nr_band_from_nrarfcn(m->arfcn, m->band, sizeof m->band);
            else
                signal_lte_band_from_earfcn(m->arfcn, m->band, sizeof m->band);
        }
        if (signal_value_present(m->pci) ||
            signal_value_present(m->rsrp) ||
            signal_value_present(m->rsrq)) {
            m->valid = 1;
            n++;
        }
    }
    return n;
}

static int signal_carrier_is_pcell(const struct signal_carrier_metric *m)
{
    if (!m || !m->valid)
        return 0;
    return !strcasecmp(m->type, "PCell") || !strcasecmp(m->name, "PCell");
}

static int signal_carrier_same_text(const char *a, const char *b)
{
    return signal_value_present(a) && signal_value_present(b) && !strcmp(a, b);
}

static struct signal_carrier_metric *signal_pick_carrier_metric(struct signal_carrier_metric *list,
                                                               int n,
                                                               const char *pci,
                                                               const char *arfcn,
                                                               int prefer_pcell)
{
    struct signal_carrier_metric *pcell_fallback = NULL;

    if (!list || n <= 0)
        return NULL;

    for (int i = 0; i < n; i++) {
        struct signal_carrier_metric *m = &list[i];
        if (!m->valid || m->used)
            continue;
        if (prefer_pcell && !signal_carrier_is_pcell(m))
            continue;
        if (!prefer_pcell && signal_carrier_is_pcell(m))
            continue;
        if (signal_carrier_same_text(m->arfcn, arfcn)) {
            if (signal_value_present(m->pci) && signal_value_present(pci) &&
                !signal_carrier_same_text(m->pci, pci))
                continue;
            m->used = 1;
            return m;
        }
        if (prefer_pcell && signal_carrier_same_text(m->pci, pci)) {
            m->used = 1;
            return m;
        }
        if (prefer_pcell && !pcell_fallback)
            pcell_fallback = m;
    }

    if (pcell_fallback)
        pcell_fallback->used = 1;
    return prefer_pcell ? pcell_fallback : NULL;
}

static void signal_bundle_apply_metrics(struct signal_bundle *b)
{
    static char json[32768];
    static char lte[8192];
    static char nr[24576];
    FILE *fp;
    size_t n;
    int rc;

    if (!b || !g_sig_read)
        return;
    fp = popen("/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/signal-metrics' 2>/dev/null", "r");
    if (!fp)
        return;
    n = fread(json, 1, sizeof(json) - 1, fp);
    rc = pclose(fp);
    if (rc != 0 || n == 0)
        return;
    json[n] = 0;

    signal_metric_get(json, "cell_vendor", b->nr_vendor, sizeof b->nr_vendor);
    signal_metric_get(json, "cell_key", b->cell_key, sizeof b->cell_key);
    signal_metric_get(json, "ta", b->ta, sizeof b->ta);
    if (json_get(json, "lte", lte, sizeof lte)) {
        signal_metric_get(lte, "ta", b->lte_ta, sizeof b->lte_ta);
        signal_metric_get(lte, "mcs", b->lte_mcs, sizeof b->lte_mcs);
        signal_metric_get(lte, "modulation", b->lte_modulation, sizeof b->lte_modulation);
        signal_metric_get(lte, "rb", b->lte_rb, sizeof b->lte_rb);
        signal_metric_get(lte, "grants", b->lte_grants, sizeof b->lte_grants);
        signal_metric_get(lte, "bler_pct", b->lte_bler, sizeof b->lte_bler);
        signal_metric_get(lte, "pusch", b->lte_pusch, sizeof b->lte_pusch);
        signal_metric_get(lte, "pucch", b->lte_pucch, sizeof b->lte_pucch);
        b->lte_carrier_n = signal_parse_carriers(lte, 0, b->lte_carriers,
                                                 SIGNAL_MAX_CARRIERS);
        b->lte_neighbor_n = signal_parse_neighbors(lte, 0, b->lte_neighbors,
                                                   SIGNAL_MAX_NEIGHBORS);
    }
    if (json_get(json, "nr", nr, sizeof nr)) {
        signal_metric_get(nr, "ta", b->nr_ta, sizeof b->nr_ta);
        signal_metric_get(nr, "mcs", b->nr_mcs, sizeof b->nr_mcs);
        signal_metric_get(nr, "modulation", b->nr_modulation, sizeof b->nr_modulation);
        signal_metric_get(nr, "mimo", b->nr_mimo, sizeof b->nr_mimo);
        signal_metric_get(nr, "layers", b->nr_layers, sizeof b->nr_layers);
        signal_metric_get(nr, "dl_rb", b->nr_dl_rb, sizeof b->nr_dl_rb);
        signal_metric_get(nr, "dl_grants", b->nr_dl_grants, sizeof b->nr_dl_grants);
        signal_metric_get(nr, "ul_rb", b->nr_ul_rb, sizeof b->nr_ul_rb);
        signal_metric_get(nr, "ul_grants", b->nr_ul_grants, sizeof b->nr_ul_grants);
        signal_metric_get(nr, "bler_pct", b->nr_bler, sizeof b->nr_bler);
        signal_metric_get(nr, "pusch", b->nr_pusch, sizeof b->nr_pusch);
        signal_metric_get(nr, "pucch", b->nr_pucch, sizeof b->nr_pucch);
        signal_metric_get(nr, "ports", b->nr_ports, sizeof b->nr_ports);
        signal_metric_get(nr, "ssb", b->nr_beam, sizeof b->nr_beam);
        signal_metric_get(nr, "serving_ssb", b->nr_serving_ssb, sizeof b->nr_serving_ssb);
        b->nr_carrier_n = signal_parse_carriers(nr, 1, b->nr_carriers,
                                                SIGNAL_MAX_CARRIERS);
        b->nr_neighbor_n = signal_parse_neighbors(nr, 1, b->nr_neighbors,
                                                  SIGNAL_MAX_NEIGHBORS);
    }
    if (!signal_value_present(b->ta)) {
        if (signal_value_present(b->nr_ta))
            snprintf(b->ta, sizeof b->ta, "%s", b->nr_ta);
        else if (signal_value_present(b->lte_ta))
            snprintf(b->ta, sizeof b->ta, "%s", b->lte_ta);
    }
}

static void signal_slot_label(const char *slot_json, char *dst, size_t cap)
{
    char data[8192];
    char decoded[8192];
    char message[1024];
    char name[256];
    char scope[256];
    char summary[256];

    if (!dst || cap == 0) return;
    if (!slot_json || !slot_json[0] || !strncmp(slot_json, "null", 4)) {
        snprintf(dst, cap, "-");
        return;
    }
    if (json_get(slot_json, "data", data, sizeof data)) {
        if (json_get(data, "decoded", decoded, sizeof decoded)) {
            if (json_get(decoded, "message", message, sizeof message) && message[0]) {
                if (message[0] == '{') {
                    if (json_get(message, "name", name, sizeof name) && name[0]) {
                        snprintf(dst, cap, "%s", name);
                        return;
                    }
                } else {
                    snprintf(dst, cap, "%s", message);
                    return;
                }
            }
            if (json_get(decoded, "scope", scope, sizeof scope) && scope[0]) {
                snprintf(dst, cap, "%s", scope);
                return;
            }
        }
        if (json_get(data, "summary", summary, sizeof summary) && summary[0]) {
            snprintf(dst, cap, "%s", summary);
            return;
        }
    }
    snprintf(dst, cap, "-");
}

static int json_pick_scalar_string(const char *obj, const char *const *keys, size_t nkeys,
                                   char *dst, size_t cap);
static int fetch_signal_raw_field(const char *kind, const char *const *keys, size_t nkeys,
                                  char *dst, size_t cap);
static int fetch_signal_raw_vendor(const char *kind, char *dst, size_t cap);

static int fetch_signal_raw_int_field(const char *kind, const char *const *keys, size_t nkeys,
                                      char *dst, size_t cap)
{
    char json[4096];
    char data[3600];
    char cmd[256];
    FILE *fp;
    size_t n;
    int rc;

    if (!dst || cap == 0 || !kind || !keys || nkeys == 0 || !g_sig_read)
        return 0;
    if (snprintf(cmd, sizeof cmd,
                 "/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/latest?kind=%s' 2>/dev/null",
                 kind) >= (int)sizeof cmd)
        return 0;

    fp = popen(cmd, "r");
    if (!fp) return 0;
    n = fread(json, 1, sizeof json - 1, fp);
    rc = pclose(fp);
    if (rc != 0 || n == 0) return 0;
    json[n] = 0;
    if (!json_get(json, "data", data, sizeof data))
        return 0;

    for (size_t i = 0; i < nkeys; i++) {
        long v = json_get_int(data, keys[i], -2147483647L);
        if (v != -2147483647L) {
            snprintf(dst, cap, "%ld", v);
            return 1;
        }
    }
    return 0;
}

static int signal_key_tail_is_boundary(char c)
{
    return !((c >= 'A' && c <= 'Z') ||
             (c >= 'a' && c <= 'z') ||
             (c >= '0' && c <= '9') ||
             c == '_' || c == '-');
}

static int signal_parse_deltaf_number(const char *p, int *out)
{
    int sign = 1;
    int v = 0;
    int any = 0;

    if (!p || !out)
        return 0;
    if (*p == '-') {
        sign = -1;
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        any = 1;
        v = v * 10 + (*p - '0');
        p++;
    }
    if (!any)
        return 0;
    *out = sign * v;
    return 1;
}

static int signal_extract_deltaf_value(const char *text, const char *key, int *out)
{
    const size_t key_len = strlen(key);
    const char *p;

    if (!text || !key || !out)
        return 0;
    for (p = strstr(text, key); p; p = strstr(p + 1, key)) {
        const char *q;
        const char *limit;
        const char *v;
        const char *colon;

        if (!signal_key_tail_is_boundary(p[key_len]))
            continue;
        q = p + key_len;
        limit = q + 160;
        v = strstr(q, "deltaF");
        if (v && v < limit && signal_parse_deltaf_number(v + 6, out))
            return 1;
        colon = strchr(q, ':');
        if (!colon || colon >= limit)
            continue;
        colon++;
        while (*colon == ' ' || *colon == '\t' || *colon == '"' || *colon == '\\')
            colon++;
        if (signal_parse_deltaf_number(colon, out))
            return 1;
    }
    return 0;
}

static int signal_detect_vendor_from_text(const char *text, char *dst, size_t cap)
{
    static const char *const keys[5] = {
        "deltaF-PUCCH-Format1",
        "deltaF-PUCCH-Format1b",
        "deltaF-PUCCH-Format2",
        "deltaF-PUCCH-Format2a",
        "deltaF-PUCCH-Format2b"
    };
    int v[5];

    if (!text || !dst || cap == 0)
        return 0;
    if (signal_extract_deltaf_value(text, keys[0], &v[0]) &&
        signal_extract_deltaf_value(text, keys[1], &v[1]) &&
        signal_extract_deltaf_value(text, keys[2], &v[2]) &&
        signal_extract_deltaf_value(text, keys[3], &v[3]) &&
        signal_extract_deltaf_value(text, keys[4], &v[4])) {
        if (v[0] == 0 && v[1] == 3 && v[2] == 0 && v[3] == 0 && v[4] == 0)
            snprintf(dst, cap, "Ericsson");
        else if (v[0] == 0 && v[1] == 3 && v[2] == 1 && v[3] == 2 && v[4] == 2)
            snprintf(dst, cap, "Huawei");
        else if (v[0] == 0 && v[1] == 1 && v[2] == 0 && v[3] == 0 && v[4] == 0)
            snprintf(dst, cap, "Nokia");
        else if (v[0] == 2 && v[1] == 3 && v[2] == 1 && v[3] == 2 && v[4] == 2)
            snprintf(dst, cap, "ZTE");
        else
            return 0;
        return 1;
    }
    if (strstr(text, "maxCarriersRequestedDL") &&
        strstr(text, "maxCarriersRequestedUL")) {
        snprintf(dst, cap, "Huawei");
        return 1;
    }
    return 0;
}

static int fetch_signal_bundle(struct signal_bundle *out)
{
    static uint32_t cached_at;
    static int cached_valid;
    static struct signal_bundle cached;
    char json[16384];
    char slot[8192];
    char data[8192];
    char decoded[8192];
    uint32_t now = millis();
    FILE *fp;
    size_t n;
    int rc;
    static const char *const nr_port_keys[] = {
        "csi_rs_ports", "csi_rs_nrof_ports", "nrofPorts", "ports", "num_ports"
    };
    static const char *const nr_ssb_keys[] = {
        "ssb_positions_in_burst_active", "ssb_positions_in_burst", "ssb_index",
        "csi_ssb_resource_set", "ssb_positions_in_burst_short_bitmap",
        "ssb_positions_in_burst_medium_bitmap", "ssb_positions_in_burst_long_bitmap"
    };

    if (out) signal_bundle_reset(out);
    if (cached_at && (uint32_t)(now - cached_at) < 50) {
        if (out) *out = cached;
        return cached_valid;
    }

    cached_at = now;
    cached_valid = 0;
    signal_bundle_reset(&cached);
    fp = popen("/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/latest-signals' 2>/dev/null", "r");
    if (!fp) {
        if (out) *out = cached;
        return 0;
    }
    n = fread(json, 1, sizeof(json) - 1, fp);
    rc = pclose(fp);
    if (rc == 0 && n > 0) {
        json[n] = 0;
        if (json_get(json, "lte_rrc", slot, sizeof slot)) {
            signal_slot_label(slot, cached.lte_rrc, sizeof cached.lte_rrc);
            if (json_get(slot, "data", data, sizeof data)) {
                if (json_get(data, "decoded", decoded, sizeof decoded))
                    (void)signal_detect_vendor_from_text(decoded, cached.nr_vendor, sizeof cached.nr_vendor);
                if (!signal_value_present(cached.nr_vendor))
                    (void)signal_detect_vendor_from_text(data, cached.nr_vendor, sizeof cached.nr_vendor);
            }
        }
        if (json_get(json, "lte_nas", slot, sizeof slot)) signal_slot_label(slot, cached.lte_nas, sizeof cached.lte_nas);
        if (json_get(json, "nr_rrc", slot, sizeof slot)) {
            signal_slot_label(slot, cached.nr_rrc, sizeof cached.nr_rrc);
            if (json_get(slot, "data", data, sizeof data)) {
                if (json_get(data, "decoded", decoded, sizeof decoded)) {
                    (void)json_pick_scalar_string(decoded, nr_port_keys,
                                                  sizeof nr_port_keys / sizeof nr_port_keys[0],
                                                  cached.nr_ports, sizeof cached.nr_ports);
                    (void)json_pick_scalar_string(decoded, nr_ssb_keys,
                                                  sizeof nr_ssb_keys / sizeof nr_ssb_keys[0],
                                                  cached.nr_beam, sizeof cached.nr_beam);
                    (void)signal_detect_vendor_from_text(decoded, cached.nr_vendor, sizeof cached.nr_vendor);
                }
                if (!strcmp(cached.nr_ports, "-")) {
                    (void)json_pick_scalar_string(data, nr_port_keys,
                                                  sizeof nr_port_keys / sizeof nr_port_keys[0],
                                                  cached.nr_ports, sizeof cached.nr_ports);
                }
                if (!strcmp(cached.nr_beam, "-")) {
                    (void)json_pick_scalar_string(data, nr_ssb_keys,
                                                  sizeof nr_ssb_keys / sizeof nr_ssb_keys[0],
                                                  cached.nr_beam, sizeof cached.nr_beam);
                }
                if (!strcmp(cached.nr_ports, "-")) {
                    fetch_signal_raw_field("nr_rrc_full&scope=rrc_reconfiguration",
                                           nr_port_keys,
                                           sizeof nr_port_keys / sizeof nr_port_keys[0],
                                           cached.nr_ports, sizeof cached.nr_ports);
                }
                if (!strcmp(cached.nr_beam, "-")) {
                    fetch_signal_raw_field("nr_rrc_full&scope=rrc_reconfiguration",
                                           nr_ssb_keys,
                                           sizeof nr_ssb_keys / sizeof nr_ssb_keys[0],
                                           cached.nr_beam, sizeof cached.nr_beam);
                }
                if (!signal_value_present(cached.nr_vendor))
                    (void)signal_detect_vendor_from_text(data, cached.nr_vendor, sizeof cached.nr_vendor);
            }
        }
        if (json_get(json, "nr_nas", slot, sizeof slot)) signal_slot_label(slot, cached.nr_nas, sizeof cached.nr_nas);
        signal_bundle_apply_metrics(&cached);
        if (!signal_value_present(cached.nr_vendor)) {
            if (!fetch_signal_raw_vendor("lte_rrc_full&scope=rrc_system_information",
                                         cached.nr_vendor, sizeof cached.nr_vendor) &&
                !fetch_signal_raw_vendor("lte_rrc_full&scope=rrc_capability_enquiry",
                                         cached.nr_vendor, sizeof cached.nr_vendor))
                (void)fetch_signal_raw_vendor("nr_rrc_full&scope=rrc_capability_enquiry",
                                              cached.nr_vendor, sizeof cached.nr_vendor);
        }
        cached_valid = 1;
    }
    if (out) *out = cached;
    return cached_valid;
}

static pthread_mutex_t g_signal_async_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_signal_async_thread;
static int g_signal_async_started;
static int g_signal_async_valid;
static uint32_t g_signal_async_at;
static struct signal_bundle g_signal_async_bundle;

static int signal_async_snapshot(struct signal_bundle *out)
{
    int ok;
    uint32_t max_age;

    if (!out)
        return 0;
    pthread_mutex_lock(&g_signal_async_lock);
    ok = g_signal_async_valid;
    max_age = (uint32_t)((g_refresh_ms > 0 ? g_refresh_ms : 1000) * 2 + 250);
    if (ok && g_signal_async_at &&
        (uint32_t)(millis() - g_signal_async_at) > max_age)
        ok = 0;
    if (ok)
        *out = g_signal_async_bundle;
    pthread_mutex_unlock(&g_signal_async_lock);
    return ok;
}

static void signal_async_publish(const struct signal_bundle *src, int valid)
{
    pthread_mutex_lock(&g_signal_async_lock);
    if (src && valid) {
        g_signal_async_bundle = *src;
        g_signal_async_valid = 1;
        g_signal_async_at = millis();
    } else {
        signal_bundle_reset(&g_signal_async_bundle);
        g_signal_async_valid = 0;
        g_signal_async_at = 0;
    }
    pthread_mutex_unlock(&g_signal_async_lock);
}

static void *signal_async_worker(void *arg)
{
    uint32_t next_at = 0;
    int was_enabled = 0;
    int last_wait_ms = 0;

    (void)arg;
    signal_async_publish(NULL, 0);
    while (g_run) {
        if (!g_ui_awake) {
            if (was_enabled) signal_async_publish(NULL, 0);
            was_enabled = 0;
            next_at = 0;
            usleep(200000);
            continue;
        }
        int enabled = signal_live_enabled();
        int wait_ms = g_refresh_ms > 0 ? g_refresh_ms : 1000;
        uint32_t now = millis();

        if (!enabled) {
            if (was_enabled)
                signal_async_publish(NULL, 0);
            was_enabled = 0;
            next_at = 0;
            usleep(100000);
            continue;
        }
        if (g_refresh_ms <= 0) {
            next_at = 0;
            usleep(100000);
            continue;
        }
        if (wait_ms != last_wait_ms) {
            next_at = 0;
            last_wait_ms = wait_ms;
        }

        was_enabled = 1;
        if (next_at == 0 || (int32_t)(now - next_at) >= 0) {
            struct signal_bundle tmp;
            int ok;

            signal_bundle_reset(&tmp);
            ok = fetch_signal_bundle(&tmp);
            if (ok)
                signal_async_publish(&tmp, 1);
            next_at = millis() + (uint32_t)wait_ms;
        }
        usleep(50000);
    }
    return NULL;
}

static void signal_async_start(void)
{
    if (g_signal_async_started)
        return;
    signal_bundle_reset(&g_signal_async_bundle);
    if (pthread_create(&g_signal_async_thread, NULL, signal_async_worker, NULL) == 0)
        g_signal_async_started = 1;
}

static void signal_async_stop(void)
{
    if (!g_signal_async_started)
        return;
    (void)pthread_join(g_signal_async_thread, NULL);
    g_signal_async_started = 0;
}

static int json_pick_int_string(const char *obj, const char *const *keys, size_t nkeys,
                                char *dst, size_t cap)
{
    const long miss = -2147483647L;

    if (!dst || cap == 0) return 0;
    for (size_t i = 0; i < nkeys; i++) {
        long v = json_get_int(obj, keys[i], miss);
        if (v != miss) {
            snprintf(dst, cap, "%ld", v);
            return 1;
        }
    }
    snprintf(dst, cap, "-");
    return 0;
}

static int json_pick_scalar_string(const char *obj, const char *const *keys, size_t nkeys,
                                   char *dst, size_t cap)
{
    char tmp[512];

    if (!dst || cap == 0) return 0;
    for (size_t i = 0; i < nkeys; i++) {
        if (json_get(obj, keys[i], tmp, sizeof tmp) && tmp[0]) {
            snprintf(dst, cap, "%s", tmp);
            return 1;
        }
    }
    return json_pick_int_string(obj, keys, nkeys, dst, cap);
}

static int fetch_signal_raw_field(const char *kind, const char *const *keys, size_t nkeys,
                                  char *dst, size_t cap)
{
    char cmd[256];
    char json[4096];
    char data[3600];
    char decoded[3200];
    FILE *fp;
    size_t n;
    int rc;

    if (!dst || cap == 0) return 0;
    snprintf(dst, cap, "-");
    if (!g_sig_read) return 0;
    if (snprintf(cmd, sizeof cmd,
                 "/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/latest?kind=%s' 2>/dev/null",
                 kind) >= (int)sizeof cmd)
        return 0;
    fp = popen(cmd, "r");
    if (!fp) return 0;
    n = fread(json, 1, sizeof(json) - 1, fp);
    rc = pclose(fp);
    if (rc != 0 || n == 0) return 0;
    json[n] = 0;
    if (!strncmp(json, "null", 4)) return 0;
    if (!json_get(json, "data", data, sizeof data)) return 0;
    if (json_get(data, "decoded", decoded, sizeof decoded) &&
        json_pick_scalar_string(decoded, keys, nkeys, dst, cap))
        return 1;
    return json_pick_scalar_string(data, keys, nkeys, dst, cap);
}

static int json_scan_last_scalar_string(const char *json, const char *const *keys, size_t nkeys,
                                        char *dst, size_t cap)
{
    char needle[128];

    if (!json || !dst || cap == 0) return 0;
    for (size_t i = 0; i < nkeys; i++) {
        const char *p = json;
        char last[256] = "";
        int found = 0;

        if (snprintf(needle, sizeof needle, "\"%s\":", keys[i]) >= (int)sizeof needle)
            continue;
        while ((p = strstr(p, needle)) != NULL) {
            const char *v = p + strlen(needle);
            char tmp[256];
            size_t o = 0;

            while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')
                v++;
            if (*v == '"') {
                v++;
                while (*v && o + 1 < sizeof tmp) {
                    if (*v == '\\' && v[1]) {
                        v++;
                        tmp[o++] = *v++;
                        continue;
                    }
                    if (*v == '"')
                        break;
                    tmp[o++] = *v++;
                }
            } else {
                while (*v && *v != ',' && *v != '}' && *v != ']' &&
                       *v != ' ' && *v != '\t' && *v != '\r' && *v != '\n' &&
                       o + 1 < sizeof tmp)
                    tmp[o++] = *v++;
            }
            tmp[o] = 0;
            if (tmp[0] && strcmp(tmp, "null") != 0) {
                snprintf(last, sizeof last, "%s", tmp);
                found = 1;
            }
            p += strlen(needle);
        }
        if (found) {
            snprintf(dst, cap, "%s", last);
            return 1;
        }
    }
    return 0;
}

static int fetch_signal_recent_raw_field(const char *kind, const char *const *keys, size_t nkeys,
                                         char *dst, size_t cap)
{
    char cmd[256];
    static char json[65536];
    FILE *fp;
    size_t n;
    int rc;

    if (!dst || cap == 0) return 0;
    if (!g_sig_read) return 0;
    if (snprintf(cmd, sizeof cmd,
                 "/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/recent?kind=%s&limit=96' 2>/dev/null",
                 kind) >= (int)sizeof cmd)
        return 0;
    fp = popen(cmd, "r");
    if (!fp) return 0;
    n = fread(json, 1, sizeof(json) - 1, fp);
    rc = pclose(fp);
    if (rc != 0 || n == 0) return 0;
    json[n] = 0;
    return json_scan_last_scalar_string(json, keys, nkeys, dst, cap);
}

static int fetch_signal_raw_vendor(const char *kind, char *dst, size_t cap)
{
    char cmd[256];
    char json[4096];
    char data[3600];
    char decoded[3200];
    FILE *fp;
    size_t n;
    int rc;

    if (!dst || cap == 0 || !g_sig_read)
        return 0;
    if (snprintf(cmd, sizeof cmd,
                 "/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/latest?kind=%s' 2>/dev/null",
                 kind) >= (int)sizeof cmd)
        return 0;
    fp = popen(cmd, "r");
    if (!fp)
        return 0;
    n = fread(json, 1, sizeof(json) - 1, fp);
    rc = pclose(fp);
    if (rc != 0 || n == 0)
        return 0;
    json[n] = 0;
    if (!strncmp(json, "null", 4))
        return 0;
    if (!json_get(json, "data", data, sizeof data))
        return 0;
    if (json_get(data, "decoded", decoded, sizeof decoded) &&
        signal_detect_vendor_from_text(decoded, dst, cap))
        return 1;
    return signal_detect_vendor_from_text(data, dst, cap);
}

static void signal_settings_summary(char *dst, size_t cap)
{
    if (!dst || cap == 0) return;
    snprintf(dst, cap, "读取 %s · 解析 %s",
             g_sig_read ? "已开启" : "已关闭",
             g_sig_parse ? "已开启" : "已关闭");
}
/* escape &,<,> for safe litehtml parsing of arbitrary SMS text */
static void html_esc(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *s = src; *s && o + 7 < cap; s++) {
        const char *r = NULL;
        if      (*s == '&') r = "&amp;";
        else if (*s == '<') r = "&lt;";
        else if (*s == '>') r = "&gt;";
        if (r) { size_t L = strlen(r); memcpy(dst + o, r, L); o += L; }
        else dst[o++] = *s;
    }
    dst[o] = 0;
}

/* SMS bodies are text nodes too, but preserve their original line breaks. */
static void html_esc_breaks(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *s = src; *s && o + 7 < cap; s++) {
        const char *r = NULL;
        if      (*s == '&')  r = "&amp;";
        else if (*s == '<')  r = "&lt;";
        else if (*s == '>')  r = "&gt;";
        else if (*s == '\n') r = "<br>";
        else if (*s == '\r') continue;
        if (r) { size_t L = strlen(r); memcpy(dst + o, r, L); o += L; }
        else dst[o++] = *s;
    }
    dst[o] = 0;
}

/* Copy at most maxb bytes of UTF-8 without splitting a multibyte char; stop at
 * a newline. Sets *cut if the source was longer, so the caller can add ellipsis. */
static void utf8_trunc(char *dst, size_t cap, const char *src, int maxb, int *cut)
{
    int o = 0; *cut = 0;
    const unsigned char *s = (const unsigned char *)src;
    while (*s) {
        if (*s == '\n' || *s == '\r') { *cut = 1; break; }
        int len = (*s >= 0xF0) ? 4 : (*s >= 0xE0) ? 3 : (*s >= 0xC0) ? 2 : 1;
        if (o + len > maxb || (size_t)(o + len) >= cap) { *cut = 1; break; }
        for (int k = 0; k < len && *s; k++) dst[o++] = (char)*s++;
    }
    dst[o] = 0;
}

static void signal_ui_compact(char *dst, size_t cap, const char *src)
{
    int cut = 0;

    if (!dst || cap == 0) return;
    if (!src || !src[0]) {
        snprintf(dst, cap, "-");
        return;
    }

    if (strstr(src, "rrc_reconfiguration_complete") || strstr(src, "RRCReconfigurationComplete"))
        snprintf(dst, cap, "RRC重配完成");
    else if (strstr(src, "rrc_reconfiguration") || strstr(src, "RRCReconfiguration"))
        snprintf(dst, cap, "RRC重配");
    else if (strstr(src, "rrc_setup_complete") || strstr(src, "RRCSetupComplete"))
        snprintf(dst, cap, "RRC建立完成");
    else if (strstr(src, "rrc_setup") || strstr(src, "RRCSetup"))
        snprintf(dst, cap, "RRC建立");
    else if (strstr(src, "ueCapabilityEnquiry") || strstr(src, "rrc_capability_enquiry"))
        snprintf(dst, cap, "UE能力查询");
    else if (strstr(src, "ueCapabilityInformation") || strstr(src, "rrc_capability_information"))
        snprintf(dst, cap, "UE能力上报");
    else if (strstr(src, "SystemInformation") || strstr(src, "rrc_system_information"))
        snprintf(dst, cap, "系统信息");
    else if (strstr(src, "MasterInformationBlock") || strstr(src, "rrc_mib"))
        snprintf(dst, cap, "MIB");
    else if (strstr(src, "securityModeCommand"))
        snprintf(dst, cap, "安全模式命令");
    else if (strstr(src, "securityModeComplete"))
        snprintf(dst, cap, "安全模式完成");
    else if (strstr(src, "registration_accept"))
        snprintf(dst, cap, "注册接受");
    else if (strstr(src, "registration_request"))
        snprintf(dst, cap, "注册请求");
    else if (strstr(src, "service_request"))
        snprintf(dst, cap, "业务请求");
    else {
        utf8_trunc(dst, cap, src, 18, &cut);
        if (cut && strlen(dst) + 3 < cap)
            strcat(dst, "...");
    }
}

static void signal_bler_compact(char *dst, size_t cap, const char *src)
{
    char *endp = NULL;
    double v;

    if (!dst || cap == 0) return;
    if (!src || !src[0]) {
        snprintf(dst, cap, "-");
        return;
    }
    v = strtod(src, &endp);
    if (endp && endp != src && *endp == '\0') {
        snprintf(dst, cap, "%.1f%%", v);
        return;
    }
    signal_ui_compact(dst, cap, src);
}

/* Second-level SMS detail dialog (number + date + full text), overlaid dimmed. */
static const char *sms_modal_html(void)
{
    static char out[DEVUI_SMS_TEXT_MAX * 6 + 1536];
    static char esc[DEVUI_SMS_TEXT_MAX * 6];
    html_esc_breaks(esc, sizeof esc, g_sms_text);
    snprintf(out, sizeof out,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><link rel='stylesheet' href='style.css'></head>"
        "<body class='mo'><div class='modal %s'>"
        "<div class='mtitle'>%s</div>"
        "<div class='smsmdate'>%s</div>"
        "<div id='smsview' class='smsmview'><div id='smsbody' class='smsmbody' style='top:-%dpx'>%s</div></div>"
        "<div class='mbtns'><a href='act:smsclose' class='mbtn2 prim mfull'>关闭</a></div>"
        "</div></body></html>",
        g_theme ? "light" : "dark", g_sms_num, g_sms_date, g_sms_scroll, esc);
    return out;
}

/* Build the second-level band-lock dialog (overlaid on the dimmed page). */
static const char *modal_html(void)
{
    static char out[4200];
    const char *uni, *sel, *pfx, *title, *act;
    if (g_modal == 4) {
        snprintf(out, sizeof out,
            "<!DOCTYPE html><html><head><meta charset='UTF-8'><link rel='stylesheet' href='style.css'></head>"
            "<body class='mo'><div class='modal %s'>"
            "<div class='mtitle'>高级设置</div>"
            "<a href='act:sigread' class='row'><span class='lab'>读取信令<span class='st'>控制 ML1 / 原始信令读取，影响 TA / Ports / Beam</span></span>"
            "<span class='ctrl'><span class='sw %s'><span class='sw-track'></span><span class='kn'></span></span></span></a>"
            "<a href='act:sigparse' class='row'><span class='lab'>解析信令<span class='st'>控制 LTE / NR RRC / NAS 解析，并决定第二页是否显示</span></span>"
            "<span class='ctrl'><span class='sw %s'><span class='sw-track'></span><span class='kn'></span></span></span></a>"
            "<div class='sec'>读取关闭时 TA / Ports / SSB Index 会显示为空。</div>"
            "<div class='mbtns'><a href='act:closemodal' class='mbtn2 prim mfull'>关闭</a></div>"
            "</div></body></html>",
            g_theme ? "light" : "dark",
            g_sig_read ? "on" : "off",
            g_sig_parse ? "on" : "off");
        return out;
    }
    if (g_modal == 1)      { uni = g_uni_sa;  sel = g_sel_sa;  pfx = "n"; act = "bsa";  title = "5G SA 锁频"; }
    else if (g_modal == 2) { uni = g_uni_nsa; sel = g_sel_nsa; pfx = "n"; act = "bnsa"; title = "5G NSA 锁频"; }
    else                   { uni = g_uni_lte; sel = g_sel_lte; pfx = "B"; act = "blte"; title = "4G 锁频"; }
    char chips[2200];
    build_chips_cls(chips, sizeof chips, uni, sel, act, pfx, "bchip-on", "bchip");
    snprintf(out, sizeof out,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><link rel='stylesheet' href='style.css'></head>"
        "<body class='mo'><div class='modal %s'>"
        "<div class='mtitle'>%s</div><div class='mbands'>%s</div>"
        "<div class='mbtns'>"
        "<a href='act:mall' class='mbtn2'>全选/全不选</a>"
        "<a href='act:minv' class='mbtn2'>反选</a>"
        "<a href='act:mapply' class='mbtn2 prim'>应用</a></div>"
        "</div></body></html>",
        g_theme ? "light" : "dark", title, chips);
    return out;
}

static int fetch_lte_ta(int *ta_out)
{
    static uint32_t cached_at;
    static uint32_t enable_req_at;
    static int cached_valid;
    static int cached_ta = -1;
    char json[2048];
    char data[1800];
    char cmd[192];
    uint32_t now = millis();
    FILE *fp;
    size_t n;
    int rc;
    int fresh_ta = -1;

    if (ta_out) *ta_out = -1;
    if (!g_sig_read) return 0;
    if (cached_ta < 0) {
        char cached_text[24];
        if (signal_cache_get("ta", cached_text, sizeof cached_text)) {
            char *endp = NULL;
            long v = strtol(cached_text, &endp, 10);

            if (endp && *endp == '\0' && v >= 0 && v <= 4096) {
                cached_ta = (int)v;
                cached_valid = 1;
            }
        }
    }
    if (cached_at && (uint32_t)(now - cached_at) < 900) {
        if (cached_valid && ta_out) *ta_out = cached_ta;
        return cached_valid;
    }

    cached_at = now;
    cached_valid = cached_ta >= 0;
    fp = popen("/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/latest?kind=lte_ml1_raw' 2>/dev/null", "r");
    if (fp) {
        n = fread(json, 1, sizeof(json) - 1, fp);
        rc = pclose(fp);
        if (rc == 0 && n > 0) {
            json[n] = 0;
            if (!strncmp(json, "null", 4)) {
                if ((uint32_t)(now - enable_req_at) >= 5000 &&
                    snprintf(cmd, sizeof cmd,
                            "/usr/bin/wget -qO- --post-data='' 'http://127.0.0.1:9460/modem/control?ml1_raw=1&active=1' >/dev/null 2>&1")
                        < (int)sizeof cmd) {
                    (void)system(cmd);
                    enable_req_at = now;
                }
            } else if (json_get(json, "data", data, sizeof data)) {
                fresh_ta = (int)json_get_int(data, "ta", -1);
            }
        }
    }
    if (fresh_ta < 0) {
        fp = popen("/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/recent?kind=lte_ml1_raw&log_origin=direct&limit=64' 2>/dev/null", "r");
        if (fp) {
            n = fread(json, 1, sizeof(json) - 1, fp);
            rc = pclose(fp);
            if (rc == 0 && n > 0) {
                const char *p;
                json[n] = 0;
                p = json;
                while ((p = strstr(p, "\"ta\":")) != NULL) {
                    char *endp = NULL;
                    long v = strtol(p + 5, &endp, 10);

                    if (endp && endp != p + 5 && v >= 0 && v <= 4096)
                        fresh_ta = (int)v;
                    p = endp && endp > p ? endp : p + 5;
                }
            }
        }
    }
    if (fresh_ta >= 0) {
        char ta_text[24];

        cached_ta = fresh_ta;
        cached_valid = 1;
        snprintf(ta_text, sizeof ta_text, "%d", fresh_ta);
        signal_cache_put("ta", ta_text);
    }
    if (cached_ta < 0) return 0;

    cached_valid = 1;
    if (ta_out) *ta_out = cached_ta;
    return 1;
}

static double nr_ta_unit_m(const devui_data_t *d, const struct signal_bundle *sig)
{
    long arfcn = d ? d->nr_channel : 0;

    if (sig && signal_value_present(sig->cell_key)) {
        const char *p = strrchr(sig->cell_key, ':');
        if (p && p[1]) {
            char *endp = NULL;
            long v = strtol(p + 1, &endp, 10);
            if (endp && endp != p + 1 && v > 0) arfcn = v;
        }
    }
    if (arfcn >= 499200 && arfcn <= 537999) return 39.06; /* n41, 30 kHz */
    if (arfcn >= 620000 && arfcn <= 653333) return 39.06; /* n78, 30 kHz */
    if (arfcn >= 693334 && arfcn <= 733333) return 39.06; /* n79, 30 kHz */
    if (d && (strstr(d->nr_band, "n41") || strstr(d->nr_band, "n77") ||
              strstr(d->nr_band, "n78") || strstr(d->nr_band, "n79")))
        return 39.06;
    return 78.12;
}

static double ta_unit_m_for_signal(const devui_data_t *d, const struct signal_bundle *sig, int ta)
{
    if (sig && signal_value_present(sig->nr_ta)) {
        char *endp = NULL;
        long nr_ta = strtol(sig->nr_ta, &endp, 10);
        if (endp && endp != sig->nr_ta && nr_ta == ta)
            return nr_ta_unit_m(d, sig);
    }
    return 78.12; /* LTE / 15 kHz */
}

static void fmt_ta_distance(char *dst, size_t cap, int ta, double unit_m)
{
    double meters;

    if (!dst || cap == 0) return;
    if (ta < 0) { snprintf(dst, cap, "-"); return; }
    if (unit_m <= 0.0) unit_m = 78.12;
    meters = (double)ta * unit_m;
    if (meters >= 1000.0) snprintf(dst, cap, "约 %.1f km", meters / 1000.0);
    else                  snprintf(dst, cap, "约 %.0f m", meters);
}

static int signal_neighbor_count(const struct signal_neighbor_metric *list, int n)
{
    int count = 0;

    if (!list || n <= 0) return 0;
    for (int i = 0; i < n; i++)
        if (list[i].valid)
            count++;
    return count;
}

static const char *signal_rsrp_class(const char *rsrp)
{
    double v;

    if (!signal_value_present(rsrp)) return "q-off";
    v = atof(rsrp);
    if (v >= -85.0) return "q-good";
    if (v >= -105.0) return "q-mid";
    return "q-bad";
}

static const char *signal_rsrq_class(const char *rsrq)
{
    double v;

    if (!signal_value_present(rsrq)) return "q-off";
    v = atof(rsrq);
    if (v >= -10.0) return "q-good";
    if (v >= -15.0) return "q-mid";
    return "q-bad";
}

static int signal_neighbor_row(char *buf, int o, int cap,
                               const struct signal_neighbor_metric *n)
{
    char pci[32], band[48], rsrp[32], rsrq[32];

    if (!buf || cap <= 0 || o >= cap || !n || !n->valid)
        return o;
    html_esc(pci, sizeof pci, signal_carrier_val(n->pci));
    html_esc(band, sizeof band, signal_carrier_val(n->band));
    html_esc(rsrp, sizeof rsrp, signal_carrier_val(n->rsrp));
    html_esc(rsrq, sizeof rsrq, signal_carrier_val(n->rsrq));
    return o + snprintf(buf + o, cap - o,
                        "<div class='nrow'><span>%s</span><span>%s</span>"
                        "<span class='%s'>%s</span><span class='%s'>%s</span></div>",
                        pci, band, signal_rsrp_class(n->rsrp), rsrp,
                        signal_rsrq_class(n->rsrq), rsrq);
}

static int signal_neighbor_section(char *buf, int o, int cap, const char *title,
                                   const struct signal_neighbor_metric *list, int n)
{
    int count = signal_neighbor_count(list, n);

    if (!count || !buf || cap <= 0 || o >= cap)
        return o;
    o += snprintf(buf + o, cap - o,
                  "<div class='nsec'>%s <span>%d</span></div>"
                  "<div class='nhead'><span>PCI</span><span>频段</span><span>RSRP</span><span>RSRQ</span></div>",
                  title, count);
    for (int i = 0; i < n; i++)
        o = signal_neighbor_row(buf, o, cap, &list[i]);
    return o;
}

static const char *signal_neighbor_empty_text(void)
{
    return "等待 LTE / NR ML1 邻区测量。";
}

static const char *signal_neighbors_html(const struct signal_bundle *sig)
{
    static char buf[7000];
    int nr_count = sig ? signal_neighbor_count(sig->nr_neighbors, sig->nr_neighbor_n) : 0;
    int lte_count = sig ? signal_neighbor_count(sig->lte_neighbors, sig->lte_neighbor_n) : 0;
    int total = nr_count + lte_count;
    const char *source = g_sig_parse ? "信令解析" : "";
    int o = 0;

    if (!g_sig_parse) {
        buf[0] = 0;
        return buf;
    }

    if (!g_neighbor_open) {
        if (source[0])
            snprintf(buf, sizeof buf,
                     "<a href='act:neighbors' class='card neigh-toggle'>"
                     "<span class='title'>邻小区</span><span class='r muted'>%s · %d 个 · 点击展开</span>"
                     "</a>",
                     source, total);
        else
            snprintf(buf, sizeof buf,
                     "<a href='act:neighbors' class='card neigh-toggle'>"
                     "<span class='title'>邻小区</span><span class='r muted'>%d 个 · 点击展开</span>"
                     "</a>",
                     total);
        return buf;
    }

    if (source[0])
        o += snprintf(buf + o, sizeof buf - o,
                      "<div class='card neigh-card'>"
                      "<div class='title'>邻小区 <span class='muted nsrc'>%s</span>"
                      "<a href='act:neighbors' class='rev r'>收起</a></div>",
                      source);
    else
        o += snprintf(buf + o, sizeof buf - o,
                      "<div class='card neigh-card'>"
                      "<div class='title'>邻小区 <a href='act:neighbors' class='rev r'>收起</a></div>");
    if (total <= 0) {
        o += snprintf(buf + o, sizeof buf - o,
                      "<div class='sec'>%s</div>",
                      signal_neighbor_empty_text());
    } else {
        o = signal_neighbor_section(buf, o, sizeof buf, "NR", sig->nr_neighbors, sig->nr_neighbor_n);
        o = signal_neighbor_section(buf, o, sizeof buf, "LTE", sig->lte_neighbors, sig->lte_neighbor_n);
    }
    snprintf(buf + o, sizeof buf - o, "</div>");
    return buf;
}

static int timezone_draft_offset(void)
{
    int offset = g_tz_draft_hour * 60;
    if (g_tz_draft_hour < 0) offset -= g_tz_draft_minute;
    else offset += g_tz_draft_minute;
    return offset;
}

static void timezone_offset_label(char *out, size_t outlen, int offset)
{
    char sign = offset < 0 ? '-' : '+';
    int abs_offset = offset < 0 ? -offset : offset;
    snprintf(out, outlen, "UTC%c%02d:%02d", sign, abs_offset / 60, abs_offset % 60);
}

static void timezone_sync_draft(const devui_data_t *data)
{
    int offset;
    if (!data) return;
    offset = data->timezone_available ? data->timezone_offset_minutes : 480;
    if (g_tz_draft_dirty) {
        if (data->timezone_available && offset == timezone_draft_offset() &&
            data->timezone_dst_minutes == (g_tz_draft_dst ? 60 : 0))
            g_tz_draft_dirty = 0;
        else
            return;
    }
    g_tz_draft_hour = offset / 60;
    g_tz_draft_minute = abs(offset % 60);
    g_tz_draft_dst = data->timezone_available ? data->timezone_dst_minutes == 60 : 0;
}

static const char *timezone_hours_html(void)
{
    static char html[4096];
    int o = 0;
    for (int hour = -12; hour <= 14; hour++) {
        o += snprintf(html + o, sizeof html - (size_t)o,
                      "<a href='act:tzhour:%d' class='tz-hour%s'>%+d</a>",
                      hour, hour == g_tz_draft_hour ? " on" : "", hour);
        if (o >= (int)sizeof html - 80) break;
    }
    return html;
}

static const devui_sim_traffic_t *sim_traffic_for_slot(const devui_data_t *data, int slot)
{
    const devui_sim_traffic_t *best = NULL;
    if (!data) return NULL;
    for (int i = 0; i < data->sim_traffic_n; i++) {
        const devui_sim_traffic_t *sim = &data->sim_traffic[i];
        if (sim->slot != slot) continue;
        if (sim->active) return sim;
        if (!best || sim->last_seen > best->last_seen) best = sim;
    }
    return best;
}

static int sim_traffic_slot_visible(const devui_data_t *data, int slot)
{
    const devui_sim_traffic_t *sim;
    if (slot == 1) return 1;
    if (slot != 2 || g_sim_dual != 1) return 0;
    if (g_sim_p2 == 1 || g_sim_slot == 2) return 1;
    sim = sim_traffic_for_slot(data, 2);
    return sim && sim->active;
}

static const char *sim_operator_display(const char *raw)
{
    if (!raw || !raw[0] || !strcmp(raw, "-")) return "未识别";
    if (!strcmp(raw, "CMCC") || !strcmp(raw, "China Mobile")) return "中国移动";
    if (!strcmp(raw, "CUCC") || !strcmp(raw, "China Unicom")) return "中国联通";
    if (!strcmp(raw, "CTCC") || !strcmp(raw, "China Telecom")) return "中国电信";
    if (!strcmp(raw, "CBN") || !strcmp(raw, "China Broadnet")) return "中国广电";
    return raw;
}

static const char *sim_traffic_cards_html(const devui_data_t *data)
{
    static char html[8192];
    int o = 0;
    for (int slot = 1; slot <= 2; slot++) {
        const devui_sim_traffic_t *sim = sim_traffic_for_slot(data, slot);
        char today[32] = "尚无统计", cycle[32] = "尚无统计", remaining[32] = "未设置", lifetime[32] = "尚无统计";
        char operator_esc[128], tail_esc[32], state_esc[96];
        const char *operator_name = slot == 1 ? g_sim_operator1 : g_sim_operator2;
        const char *state = slot == 1 ? g_sim_state1 : g_sim_state2;
        if (!sim_traffic_slot_visible(data, slot)) continue;
        if (sim) {
            if (sim->operator_name[0]) operator_name = sim->operator_name;
            fmt_bytes_u64(today, sizeof today, sim->today_bytes);
            fmt_bytes_u64(cycle, sizeof cycle, sim->cycle_bytes);
            fmt_bytes_u64(lifetime, sizeof lifetime, sim->lifetime_bytes);
            if (sim->remaining_valid) fmt_bytes_u64(remaining, sizeof remaining, sim->remaining_bytes);
            else snprintf(remaining, sizeof remaining, "未启用套餐");
        }
        html_esc(operator_esc, sizeof operator_esc, sim_operator_display(operator_name));
        html_esc(tail_esc, sizeof tail_esc, sim && sim->iccid_tail[0] ? sim->iccid_tail : "-");
        html_esc(state_esc, sizeof state_esc, state && state[0] ? state : "-");
        o += snprintf(html + o, sizeof html - (size_t)o,
                      "<div class='card sim-traffic-card'>"
                      "<div class='title'>%s <span class='r %s'>%s</span></div>"
                      "<table><tr><td class='kv-l'>运营商</td><td class='val'>%s</td></tr>"
                      "<tr><td class='kv-l'>卡号尾号</td><td class='val'>%s</td></tr>"
                      "<tr><td class='kv-l'>今日用量</td><td class='val'>%s</td></tr>"
                      "<tr><td class='kv-l'>本周期</td><td class='val'>%s</td></tr>"
                      "<tr><td class='kv-l'>套餐剩余</td><td class='val'>%s</td></tr>"
                      "<tr><td class='kv-l'>长期累计</td><td class='val'>%s</td></tr></table>"
                      "<div class='func-note'>状态 %s%s</div></div>",
                      slot == 1 ? "自插卡" : "内置卡",
                      g_sim_slot == slot ? "ok" : "muted",
                      g_sim_slot == slot ? "使用中" : "待机",
                      operator_esc, tail_esc, today, cycle, remaining, lifetime,
                      state_esc, sim ? "" : " · 使用该卡后开始统计");
        if (o >= (int)sizeof html - 800) break;
    }
    return html;
}

static const char *sim_traffic_plan_selector_html(const devui_data_t *data)
{
    static char html[512];
    if (!sim_traffic_slot_visible(data, 2)) {
        snprintf(html, sizeof html,
                 "<div class='seg'><span class='segc seg-on'>自插卡</span></div>");
        return html;
    }
    snprintf(html, sizeof html,
             "<div class='seg seg2'>"
             "<a href='act:trafficselect:1' class='segc%s'>自插卡</a>"
             "<a href='act:trafficselect:2' class='segc%s'>内置卡</a></div>",
             g_plan_edit_slot == 1 ? " seg-on" : "",
             g_plan_edit_slot == 2 ? " seg-on" : "");
    return html;
}

static void traffic_plan_sync_draft(const devui_data_t *data)
{
    const devui_sim_traffic_t *sim = sim_traffic_for_slot(data, g_plan_edit_slot);
    if (g_plan_draft_dirty) {
        uint64_t allowance = g_plan_draft_gib << 30;
        if (sim && !strcmp(sim->id, g_plan_draft_sim_id) &&
            sim->package_enabled == g_plan_draft_enabled &&
            sim->allowance_bytes == allowance &&
            sim->reset_day == g_plan_draft_reset_day)
            g_plan_draft_dirty = 0;
        else
            return;
    }
    g_plan_draft_sim_id[0] = 0;
    g_plan_draft_enabled = 0;
    g_plan_draft_gib = 0;
    g_plan_draft_reset_day = 1;
    if (!sim) return;
    snprintf(g_plan_draft_sim_id, sizeof g_plan_draft_sim_id, "%s", sim->id);
    g_plan_draft_enabled = sim->package_enabled;
    g_plan_draft_gib = sim->allowance_bytes ?
                       (sim->allowance_bytes + ((1ULL << 30) - 1)) >> 30 : 0;
    g_plan_draft_reset_day = sim->reset_day > 0 ? sim->reset_day : 1;
}

static void timezone_apply_action(uint32_t now)
{
    char path[160], response[512];
    int offset = timezone_draft_offset();
    int dst = g_tz_draft_dst ? 60 : 0;
    int effective = offset + dst;
    int status;

    if (offset < -720 || offset > 840 || offset % 15 ||
        effective < -720 || effective > 840) {
        snprintf(g_toast, sizeof g_toast, "时区超出 UTC-12 至 +14");
        g_toast_until = now + 2000;
        return;
    }
    snprintf(path, sizeof path,
             "/settings/timezone?offset_minutes=%d&dst_minutes=%d", offset, dst);
    status = data_backend_request("POST", path, response, sizeof response);
    if (status == 200) {
        g_display_offset_minutes = (int)json_get_int(
            response, "effective_offset_minutes", effective) - clock_basis_minutes(NULL);
        g_last_clock_min = -1;
        snprintf(g_toast, sizeof g_toast, "时区已保存");
    } else {
        snprintf(g_toast, sizeof g_toast, "时区保存失败");
    }
    g_toast_until = now + 2000;
}

static void traffic_plan_apply_action(uint32_t now)
{
    char path[320], response[512];
    unsigned long long allowance;
    int status;

    if (!g_plan_draft_sim_id[0]) {
        snprintf(g_toast, sizeof g_toast, "该卡尚无流量记录");
        g_toast_until = now + 2000;
        return;
    }
    if (g_plan_draft_enabled && g_plan_draft_gib == 0) {
        snprintf(g_toast, sizeof g_toast, "套餐流量不能为 0");
        g_toast_until = now + 2000;
        return;
    }
    if (g_plan_draft_gib > 16384ULL) {
        snprintf(g_toast, sizeof g_toast, "套餐流量设置过大");
        g_toast_until = now + 2000;
        return;
    }
    allowance = g_plan_draft_gib << 30;
    snprintf(path, sizeof path,
             "/sim-traffic/config?sim_id=%s&enabled=%d&allowance_bytes=%llu&reset_day=%d",
             g_plan_draft_sim_id, g_plan_draft_enabled, allowance, g_plan_draft_reset_day);
    status = data_backend_request("POST", path, response, sizeof response);
    snprintf(g_toast, sizeof g_toast, "%s", status == 200 ? "套餐设置已保存" : "套餐设置保存失败");
    g_toast_until = now + 2000;
}

/* Fill a kv table from the current device state. Buffers are static. */
static int build_kv(struct kv *t, const char *path)
{
    int is_cellinfo = path_is_signal_detail(path);
    int is_signalhome = path_is_signal_home(path);
    int is_charts = path && strstr(path, "charts.html") != NULL;
    g_phase++;

    static char s_time[8], s_bat[8], s_rsrp[12], s_rsrq[12], s_sinr[12], s_bw[12];
    static char s_cellid[20], s_pci[12], s_clients[8], s_up[24], s_rxs[20], s_txs[20];
    static char s_rxb[16], s_txb[16], s_cpu[8], s_mem[8], w_rsrp[6], w_rsrq[6], w_sinr[6], w_bw[6];
    static char s_oper[48], s_ssid[64], s_key[64], s_page[8], s_np[8], s_model[64], s_fw[80];
    static char s_qci[8], s_ambr[24], s_sbar[640], s_dots[320], s_ta[16], s_tadist[24];
    static char s_nrports[64], s_nrbeam[128], s_nrvendor[64], s_sigadvstate[96], s_sigrefresh[16], s_signalcards[10000], s_neighborcards[7000];
    static char s_last_lte_nas[128] = "-";
        static char s_last_nr_nas[128] = "-";
        static char s_last_nrports[64] = "-";
        static char s_last_nrbeam[128] = "-";
        static char s_last_nrvendor[64] = "-";
        static char s_nrrows[4096], s_lterows[4096], s_carriers[9000], s_nr_block[6000], s_lte_block[5000], s_sigcards[11000], s_gen[8];

    static devui_data_t d;
    static struct signal_bundle sig;
    int ta = -1;
    if (!data_refresh(&d)) memset(&d, 0, sizeof d);
    if (d.timezone_available)
        g_display_offset_minutes =
            d.timezone_effective_minutes - clock_basis_minutes(&d);
    plugin_status_refresh(path, 0);
    timezone_sync_draft(&d);
    traffic_plan_sync_draft(&d);
    g_charging = d.charger_connect;
    snprintf(s_page, sizeof s_page, "%d", g_cur + 1);
    snprintf(s_np, sizeof s_np, "%d", g_npages);

    time_t now = time(NULL); struct tm tmv; devui_localtime(now, &tmv);
    snprintf(s_time, sizeof s_time, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    snprintf(s_sigrefresh, sizeof s_sigrefresh, "%02d:%02d:%02d",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    snprintf(s_bat, sizeof s_bat, "%d", d.bat_percent);
    snprintf(s_rsrp, sizeof s_rsrp, "%d", d.nr_rsrp);
    snprintf(s_rsrq, sizeof s_rsrq, "%d", d.nr_rsrq);
    snprintf(s_sinr, sizeof s_sinr, "%s", d.nr_snr[0] ? d.nr_snr : "-");
    snprintf(s_bw, sizeof s_bw, "%s", d.nr_bw[0] ? d.nr_bw : "-");
    if (g_show_cellid) snprintf(s_cellid, sizeof s_cellid, "%ld", d.nr_cell_id);
    else               strcpy(s_cellid, "********");
    snprintf(s_pci, sizeof s_pci, "%d", d.nr_pci);
    snprintf(s_clients, sizeof s_clients, "%d", d.clients_total);
    fmt_uptime(s_up, sizeof s_up, d.uptime);
    fmt_speed_u(s_rxs, sizeof s_rxs, d.rx_speed, g_speed_bits);
    fmt_speed_u(s_txs, sizeof s_txs, d.tx_speed, g_speed_bits);
    fmt_bytes(s_rxb, sizeof s_rxb, d.rx_bytes);
    fmt_bytes(s_txb, sizeof s_txb, d.tx_bytes);
    snprintf(s_cpu, sizeof s_cpu, "%ld", d.cpu_temp);
    snprintf(s_mem, sizeof s_mem, "%ld", d.mem_used_pct);
    snprintf(s_oper, sizeof s_oper, "%s", d.operator_name[0] ? d.operator_name : "--");
    snprintf(s_ssid, sizeof s_ssid, "%s", d.wifi_ssid[0] ? d.wifi_ssid : "-");
    if (!d.wifi_key[0]) strcpy(s_key, "-");
    else if (g_show_key) snprintf(s_key, sizeof s_key, "%s", d.wifi_key);
    else { int kn = (int)strlen(d.wifi_key); if (kn > 16) kn = 16; memset(s_key, '*', kn); s_key[kn] = 0; }
    snprintf(s_model, sizeof s_model, "%s", d.model[0] ? d.model : "-");
    snprintf(s_fw, sizeof s_fw, "%s", d.fw[0] ? d.fw : "-");
    if (is_cellinfo && !g_sig_read) {
        snprintf(s_ta, sizeof s_ta, "已关闭");
        snprintf(s_tadist, sizeof s_tadist, "已关闭");
    } else if (is_cellinfo && fetch_lte_ta(&ta)) {
        snprintf(s_ta, sizeof s_ta, "%d", ta);
        fmt_ta_distance(s_tadist, sizeof s_tadist, ta, 78.12);
    } else if (is_cellinfo) {
        snprintf(s_ta, sizeof s_ta, "-");
        snprintf(s_tadist, sizeof s_tadist, "-");
    } else {
        snprintf(s_ta, sizeof s_ta, "-");
        snprintf(s_tadist, sizeof s_tadist, "-");
    }
    signal_settings_summary(s_sigadvstate, sizeof s_sigadvstate);
    snprintf(w_rsrp, sizeof w_rsrp, "%d", clampi((d.nr_rsrp + 120) * 2, 3, 100));
    snprintf(w_rsrq, sizeof w_rsrq, "%d", clampi((d.nr_rsrq + 20) * 100 / 17, 3, 100));
    snprintf(w_sinr, sizeof w_sinr, "%d", clampi((int)((atof(d.nr_snr) + 10) * 100 / 40), 3, 100));
    snprintf(w_bw, sizeof w_bw, "%d", clampi(atoi(d.nr_bw), 3, 100));

    /* qci / ambr (from modem qos): unified Mbps, e.g. 3000/200 Mbps */
    if (d.qci > 0) snprintf(s_qci, sizeof s_qci, "%d", d.qci);
    else           snprintf(s_qci, sizeof s_qci, "-");
    if (d.ambr_dl > 0.0 || d.ambr_ul > 0.0) {
        char dl[12], ul[12];
        if (d.ambr_dl > 0.0) snprintf(dl, sizeof dl, "%.0f", d.ambr_dl);
        else                 snprintf(dl, sizeof dl, "-");
        if (d.ambr_ul > 0.0) snprintf(ul, sizeof ul, "%.0f", d.ambr_ul);
        else                 snprintf(ul, sizeof ul, "-");
        snprintf(s_ambr, sizeof s_ambr, "%s/%s Mbps", dl, ul);
    } else {
        snprintf(s_ambr, sizeof s_ambr, "-");
    }

    /* ---- per-carrier display + generation badge ----
     * nrca group: idx,pci,?,band,arfcn,bw,?,rsrp,rsrq,sinr,rssi
     * lteca is seen in both legacy 5-field and newer 11-field variants.
     * Some firmwares expose LTE SCC signal details separately via ltecasig. */
    int is_endc = strstr(d.net_type, "ENDC") || strstr(d.net_type, "EN-DC");
    int is_nsa  = strstr(d.net_type, "NSA") != NULL;
    int is_sa   = strstr(d.net_type, "SA") && !is_nsa;
    int is_lte  = !is_nsa && !is_sa && (strstr(d.net_type, "LTE") || strstr(d.net_type, "4G"));
    int show_nr  = is_sa || is_nsa || is_endc;
    int show_lte = is_nsa || is_endc || is_lte;
    int show_nr_nas = is_sa;
    int nr_cc = 0, nr_bw = 0, lte_cc = 0, lte_bw = 0;
    int nr_show_cc = 0, nr_show_bw = 0, lte_show_cc = 0, lte_show_bw = 0, no = 0, lo = 0;
    char rp[12] = "-", pc[12] = "-", ac[16] = "-";
    const char *nr_band = d.nr_band[0] ? d.nr_band : d.band;

    signal_bundle_reset(&sig);
    if (signal_live_enabled() && (is_cellinfo || is_signalhome)) {
        if (!signal_async_snapshot(&sig)) {
            if (fetch_signal_bundle(&sig))
                signal_async_publish(&sig, 1);
        }
    }
    snprintf(s_neighborcards, sizeof s_neighborcards, "%s",
             is_signalhome ? signal_neighbors_html(&sig) : "");

    if (is_cellinfo) {
        char c_lte_rrc[96], c_lte_nas[96], c_nr_rrc[96], c_nr_nas[96];
        char c_lteta[96], c_nrta[96];
        char c_nrports[96], c_nrbeam[96], c_nrservssb[96], c_nrbler[96], c_ltebler[96];
        char c_nrmcs[96], c_nrmod[96], c_nrmimo[96], c_nrlayers[96], c_nrdlgrant[96], c_nrdlrb[96], c_nrulgrant[96], c_nrulrb[96], c_nrpusch[128], c_nrpucch[128];
        char c_ltemcs[96], c_ltemod[96], c_ltegrant[96], c_lterb[96], c_ltepusch[128], c_ltepucch[128];
        char e_lte_rrc[256], e_lte_nas[256], e_nr_rrc[256], e_nr_nas[256];
        char e_lteta[128], e_nrta[128];
        char e_nrports[160], e_nrbeam[256], e_nrservssb[128], e_nrbler[128], e_ltebler[128], e_nrvendor[128];
        char e_nrmcs[128], e_nrmod[128], e_nrmimo[128], e_nrlayers[128], e_nrdlgrant[128], e_nrdlrb[128], e_nrulgrant[128], e_nrulrb[128], e_nrpusch[192], e_nrpucch[192];
        char e_ltemcs[128], e_ltemod[128], e_ltegrant[128], e_lterb[128], e_ltepusch[192], e_ltepucch[192];
        char s_lteta[64], s_nrta[64], s_nrservssb[64], s_nrbler[64], s_ltebler[64];
        char s_nrmcs[64], s_nrmod[64], s_nrmimo[64], s_nrlayers[64], s_nrdlgrant[64], s_nrdlrb[64], s_nrulgrant[64], s_nrulrb[64], s_nrpusch[96], s_nrpucch[96];
        char s_ltemcs[64], s_ltemod[64], s_ltegrant[64], s_lterb[64], s_ltepusch[96], s_ltepucch[96];
        static char s_last_signal_cell_key[64] = "";
        static char s_last_nrservssb[64] = "-";
        static char s_last_nrbler[64] = "-";
        static char s_last_ltebler[64] = "-";
        int so = 0;

        if (signal_value_present(sig.cell_key) &&
            strcmp(sig.cell_key, s_last_signal_cell_key) != 0) {
            snprintf(s_last_signal_cell_key, sizeof s_last_signal_cell_key, "%s", sig.cell_key);
            snprintf(s_last_nrports, sizeof s_last_nrports, "-");
            snprintf(s_last_nrbeam, sizeof s_last_nrbeam, "-");
            snprintf(s_last_nrservssb, sizeof s_last_nrservssb, "-");
            snprintf(s_last_nrbler, sizeof s_last_nrbler, "-");
            snprintf(s_last_ltebler, sizeof s_last_ltebler, "-");
            snprintf(s_last_nrvendor, sizeof s_last_nrvendor, "-");
        }
        signal_value_keep_last(sig.lte_nas, sizeof sig.lte_nas, s_last_lte_nas, sizeof s_last_lte_nas);
        signal_value_keep_last(sig.nr_nas, sizeof sig.nr_nas, s_last_nr_nas, sizeof s_last_nr_nas);
        if (!signal_live_enabled()) {
            snprintf(s_lteta, sizeof s_lteta, "已关闭");
            snprintf(s_nrta, sizeof s_nrta, "已关闭");
            snprintf(s_nrports, sizeof s_nrports, "已关闭");
            snprintf(s_nrbeam, sizeof s_nrbeam, "已关闭");
            snprintf(s_nrservssb, sizeof s_nrservssb, "已关闭");
            snprintf(s_nrbler, sizeof s_nrbler, "已关闭");
            snprintf(s_ltebler, sizeof s_ltebler, "已关闭");
            snprintf(s_nrmcs, sizeof s_nrmcs, "已关闭");
            snprintf(s_nrmod, sizeof s_nrmod, "已关闭");
            snprintf(s_nrmimo, sizeof s_nrmimo, "已关闭");
            snprintf(s_nrlayers, sizeof s_nrlayers, "已关闭");
            snprintf(s_nrdlgrant, sizeof s_nrdlgrant, "已关闭");
            snprintf(s_nrdlrb, sizeof s_nrdlrb, "已关闭");
            snprintf(s_nrulgrant, sizeof s_nrulgrant, "已关闭");
            snprintf(s_nrulrb, sizeof s_nrulrb, "已关闭");
            snprintf(s_nrpusch, sizeof s_nrpusch, "已关闭");
            snprintf(s_nrpucch, sizeof s_nrpucch, "已关闭");
            snprintf(s_ltemcs, sizeof s_ltemcs, "已关闭");
            snprintf(s_ltemod, sizeof s_ltemod, "已关闭");
            snprintf(s_ltegrant, sizeof s_ltegrant, "已关闭");
            snprintf(s_lterb, sizeof s_lterb, "已关闭");
            snprintf(s_ltepusch, sizeof s_ltepusch, "已关闭");
            snprintf(s_ltepucch, sizeof s_ltepucch, "已关闭");
            snprintf(s_nrvendor, sizeof s_nrvendor, "-");
        } else {
            snprintf(s_lteta, sizeof s_lteta, "%s", sig.lte_ta);
            snprintf(s_nrta, sizeof s_nrta, "%s", sig.nr_ta);
            snprintf(s_nrports, sizeof s_nrports, "%s", sig.nr_ports);
            snprintf(s_nrbeam, sizeof s_nrbeam, "%s", sig.nr_beam);
            snprintf(s_nrservssb, sizeof s_nrservssb, "-");
            snprintf(s_nrbler, sizeof s_nrbler, "-");
            snprintf(s_ltebler, sizeof s_ltebler, "-");
            snprintf(s_nrmcs, sizeof s_nrmcs, "%s", sig.nr_mcs);
            snprintf(s_nrmod, sizeof s_nrmod, "%s", sig.nr_modulation);
            snprintf(s_nrmimo, sizeof s_nrmimo, "%s", sig.nr_mimo);
            snprintf(s_nrlayers, sizeof s_nrlayers, "%s", sig.nr_layers);
            snprintf(s_nrdlgrant, sizeof s_nrdlgrant, "%s", sig.nr_dl_grants);
            snprintf(s_nrdlrb, sizeof s_nrdlrb, "%s", sig.nr_dl_rb);
            snprintf(s_nrulgrant, sizeof s_nrulgrant, "%s", sig.nr_ul_grants);
            snprintf(s_nrulrb, sizeof s_nrulrb, "%s", sig.nr_ul_rb);
            snprintf(s_nrpusch, sizeof s_nrpusch, "%s", sig.nr_pusch);
            snprintf(s_nrpucch, sizeof s_nrpucch, "%s", sig.nr_pucch);
            snprintf(s_ltemcs, sizeof s_ltemcs, "%s", sig.lte_mcs);
            snprintf(s_ltemod, sizeof s_ltemod, "%s", sig.lte_modulation);
            snprintf(s_ltegrant, sizeof s_ltegrant, "%s", sig.lte_grants);
            snprintf(s_lterb, sizeof s_lterb, "%s", sig.lte_rb);
            snprintf(s_ltepusch, sizeof s_ltepusch, "%s", sig.lte_pusch);
            snprintf(s_ltepucch, sizeof s_ltepucch, "%s", sig.lte_pucch);
            snprintf(s_nrservssb, sizeof s_nrservssb, "%s", sig.nr_serving_ssb);
            snprintf(s_nrbler, sizeof s_nrbler, "%s", sig.nr_bler);
            snprintf(s_ltebler, sizeof s_ltebler, "%s", sig.lte_bler);
            snprintf(s_nrvendor, sizeof s_nrvendor, "%s", sig.nr_vendor);
            if (!signal_value_present(sig.nr_ta))
                snprintf(s_nrta, sizeof s_nrta, "等待ML1");
            if (!signal_value_present(sig.lte_ta))
                snprintf(s_lteta, sizeof s_lteta, "等待ML1");
            signal_value_keep_last(s_nrports, sizeof s_nrports, s_last_nrports, sizeof s_last_nrports);
            signal_value_keep_last(s_nrbeam, sizeof s_nrbeam, s_last_nrbeam, sizeof s_last_nrbeam);
            signal_value_keep_last(s_nrservssb, sizeof s_nrservssb,
                                   s_last_nrservssb, sizeof s_last_nrservssb);
            signal_value_keep_last(s_nrbler, sizeof s_nrbler,
                                   s_last_nrbler, sizeof s_last_nrbler);
            signal_value_keep_last(s_ltebler, sizeof s_ltebler,
                                   s_last_ltebler, sizeof s_last_ltebler);
            signal_value_keep_last(s_nrvendor, sizeof s_nrvendor,
                                   s_last_nrvendor, sizeof s_last_nrvendor);
            if (!strcmp(s_nrports, "-"))
                snprintf(s_nrports, sizeof s_nrports, "等待RRC CSI-RS");
            if (!strcmp(s_nrservssb, "-"))
                snprintf(s_nrservssb, sizeof s_nrservssb, "等待ML1");
            if (!strcmp(s_nrbeam, "-"))
                snprintf(s_nrbeam, sizeof s_nrbeam, "等待RRC配置");
            if (!strcmp(s_nrbler, "-"))
                snprintf(s_nrbler, sizeof s_nrbler, "等待PDSCH");
            if (!strcmp(s_ltebler, "-"))
                snprintf(s_ltebler, sizeof s_ltebler, "等待PDSCH");
            if (!strcmp(s_nrmcs, "-"))
                snprintf(s_nrmcs, sizeof s_nrmcs, "等待PDSCH");
            if (!strcmp(s_nrmod, "-"))
                snprintf(s_nrmod, sizeof s_nrmod, "等待PDSCH");
            if (!strcmp(s_nrmimo, "-"))
                snprintf(s_nrmimo, sizeof s_nrmimo, "等待PDSCH");
            if (!strcmp(s_nrlayers, "-"))
                snprintf(s_nrlayers, sizeof s_nrlayers, "等待PDSCH");
            if (!signal_value_present(sig.nr_dl_grants))
                snprintf(s_nrdlgrant, sizeof s_nrdlgrant, "等待PDSCH");
            if (!signal_value_present(sig.nr_dl_rb))
                snprintf(s_nrdlrb, sizeof s_nrdlrb, "等待PDSCH");
            if (!signal_value_present(sig.nr_ul_grants))
                snprintf(s_nrulgrant, sizeof s_nrulgrant, "等待UL调度");
            if (!signal_value_present(sig.nr_ul_rb))
                snprintf(s_nrulrb, sizeof s_nrulrb, "等待UL调度");
            if (!strcmp(s_nrpusch, "-"))
                snprintf(s_nrpusch, sizeof s_nrpusch, "等待UL功控");
            if (!strcmp(s_nrpucch, "-"))
                snprintf(s_nrpucch, sizeof s_nrpucch, "等待UL功控");
            if (!strcmp(s_ltemcs, "-"))
                snprintf(s_ltemcs, sizeof s_ltemcs, "等待PDSCH");
            if (!strcmp(s_ltemod, "-"))
                snprintf(s_ltemod, sizeof s_ltemod, "等待PDSCH");
            if (!signal_value_present(sig.lte_grants))
                snprintf(s_ltegrant, sizeof s_ltegrant, "等待PDSCH");
            if (!signal_value_present(sig.lte_rb))
                snprintf(s_lterb, sizeof s_lterb, "等待PDSCH");
            if (!strcmp(s_ltepusch, "-"))
                snprintf(s_ltepusch, sizeof s_ltepusch, "等待PUSCH");
            if (!strcmp(s_ltepucch, "-"))
                snprintf(s_ltepucch, sizeof s_ltepucch, "等待PUCCH");
            if (signal_value_present(sig.ta)) {
                char *endp = NULL;
                long ta_val = strtol(sig.ta, &endp, 10);
                if (endp && endp != sig.ta && ta_val >= 0 && ta_val <= 65535) {
                    snprintf(s_ta, sizeof s_ta, "%ld", ta_val);
                    fmt_ta_distance(s_tadist, sizeof s_tadist, (int)ta_val,
                                    ta_unit_m_for_signal(&d, &sig, (int)ta_val));
                } else {
                    snprintf(s_ta, sizeof s_ta, "%.15s", sig.ta);
                    snprintf(s_tadist, sizeof s_tadist, "-");
                }
            }
        }
        signal_ui_compact(c_lte_rrc, sizeof c_lte_rrc, sig.lte_rrc);
        signal_ui_compact(c_lte_nas, sizeof c_lte_nas, sig.lte_nas);
        signal_ui_compact(c_nr_rrc, sizeof c_nr_rrc, sig.nr_rrc);
        signal_ui_compact(c_nr_nas, sizeof c_nr_nas, sig.nr_nas);
        signal_ui_compact(c_lteta, sizeof c_lteta, s_lteta);
        signal_ui_compact(c_nrta, sizeof c_nrta, s_nrta);
        signal_ui_compact(c_nrports, sizeof c_nrports, s_nrports);
        signal_ui_compact(c_nrbeam, sizeof c_nrbeam, s_nrbeam);
        signal_ui_compact(c_nrservssb, sizeof c_nrservssb, s_nrservssb);
        signal_bler_compact(c_nrbler, sizeof c_nrbler, s_nrbler);
        signal_bler_compact(c_ltebler, sizeof c_ltebler, s_ltebler);
        signal_ui_compact(c_nrmcs, sizeof c_nrmcs, s_nrmcs);
        signal_ui_compact(c_nrmod, sizeof c_nrmod, s_nrmod);
        signal_ui_compact(c_nrmimo, sizeof c_nrmimo, s_nrmimo);
        signal_ui_compact(c_nrlayers, sizeof c_nrlayers, s_nrlayers);
        signal_ui_compact(c_nrdlgrant, sizeof c_nrdlgrant, s_nrdlgrant);
        signal_ui_compact(c_nrdlrb, sizeof c_nrdlrb, s_nrdlrb);
        signal_ui_compact(c_nrulgrant, sizeof c_nrulgrant, s_nrulgrant);
        signal_ui_compact(c_nrulrb, sizeof c_nrulrb, s_nrulrb);
        signal_ui_compact(c_nrpusch, sizeof c_nrpusch, s_nrpusch);
        signal_ui_compact(c_nrpucch, sizeof c_nrpucch, s_nrpucch);
        signal_ui_compact(c_ltemcs, sizeof c_ltemcs, s_ltemcs);
        signal_ui_compact(c_ltemod, sizeof c_ltemod, s_ltemod);
        signal_ui_compact(c_ltegrant, sizeof c_ltegrant, s_ltegrant);
        signal_ui_compact(c_lterb, sizeof c_lterb, s_lterb);
        signal_ui_compact(c_ltepusch, sizeof c_ltepusch, s_ltepusch);
        signal_ui_compact(c_ltepucch, sizeof c_ltepucch, s_ltepucch);
        html_esc(e_lte_rrc, sizeof e_lte_rrc, c_lte_rrc);
        html_esc(e_lte_nas, sizeof e_lte_nas, c_lte_nas);
        html_esc(e_nr_rrc, sizeof e_nr_rrc, c_nr_rrc);
        html_esc(e_nr_nas, sizeof e_nr_nas, c_nr_nas);
        html_esc(e_lteta, sizeof e_lteta, c_lteta);
        html_esc(e_nrta, sizeof e_nrta, c_nrta);
        html_esc(e_nrports, sizeof e_nrports, c_nrports);
        html_esc(e_nrbeam, sizeof e_nrbeam, c_nrbeam);
        html_esc(e_nrservssb, sizeof e_nrservssb, c_nrservssb);
        html_esc(e_nrbler, sizeof e_nrbler, c_nrbler);
        html_esc(e_ltebler, sizeof e_ltebler, c_ltebler);
        html_esc(e_nrmcs, sizeof e_nrmcs, c_nrmcs);
        html_esc(e_nrmod, sizeof e_nrmod, c_nrmod);
        html_esc(e_nrmimo, sizeof e_nrmimo, c_nrmimo);
        html_esc(e_nrlayers, sizeof e_nrlayers, c_nrlayers);
        html_esc(e_nrdlgrant, sizeof e_nrdlgrant, c_nrdlgrant);
        html_esc(e_nrdlrb, sizeof e_nrdlrb, c_nrdlrb);
        html_esc(e_nrulgrant, sizeof e_nrulgrant, c_nrulgrant);
        html_esc(e_nrulrb, sizeof e_nrulrb, c_nrulrb);
        html_esc(e_nrpusch, sizeof e_nrpusch, c_nrpusch);
        html_esc(e_nrpucch, sizeof e_nrpucch, c_nrpucch);
        html_esc(e_ltemcs, sizeof e_ltemcs, c_ltemcs);
        html_esc(e_ltemod, sizeof e_ltemod, c_ltemod);
        html_esc(e_ltegrant, sizeof e_ltegrant, c_ltegrant);
        html_esc(e_lterb, sizeof e_lterb, c_lterb);
        html_esc(e_ltepusch, sizeof e_ltepusch, c_ltepusch);
        html_esc(e_ltepucch, sizeof e_ltepucch, c_ltepucch);
        html_esc(e_nrvendor, sizeof e_nrvendor, s_nrvendor);
        s_signalcards[0] = 0;
        so += snprintf(s_signalcards + so, sizeof s_signalcards - so,
                       "<div class='card'><div class='title'>基站品牌识别</div><table class='sigtab'>"
                       "<tr><td class='kv-l'>刷新时间</td><td class='val'>%s</td></tr>"
                       "<tr><td class='kv-l'>识别结果</td><td class='val sm'>%s</td></tr>"
                       "<tr><td class='kv-l'>TA</td><td class='val'>%s</td></tr>"
                       "<tr><td class='kv-l'>估算距离</td><td class='val'>%s</td></tr>"
                       "</table></div>",
                       s_sigrefresh, e_nrvendor, s_ta, s_tadist);
        if (show_nr) {
            if (show_nr_nas) {
                so += snprintf(s_signalcards + so, sizeof s_signalcards - so,
                               "<div class='card'><div class='title'>NR 信令 <span class='r muted'>5G</span></div><table class='sigtab'>"
                               "<tr><td class='kv-l'>最近 RRC</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>最近 NAS</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>TA</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>MCS</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>调制</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>MIMO</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>layers</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>DL/UL Grant</td><td class='val sm'>%s/%s</td></tr>"
                               "<tr><td class='kv-l'>DL/UL RB</td><td class='val sm'>%s/%s</td></tr>"
                               "<tr><td class='kv-l'>BLER</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>PUSCH</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>PUCCH</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>Ports</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>SSB Index</td><td class='val sm'>%s</td></tr>"
                               "</table></div>",
                               e_nr_rrc, e_nr_nas, e_nrta, e_nrmcs, e_nrmod, e_nrmimo, e_nrlayers,
                               e_nrdlgrant, e_nrulgrant, e_nrdlrb, e_nrulrb, e_nrbler,
                               e_nrpusch, e_nrpucch, e_nrports, e_nrservssb);
            } else {
                so += snprintf(s_signalcards + so, sizeof s_signalcards - so,
                               "<div class='card'><div class='title'>NR 信令 <span class='r muted'>5G</span></div><table class='sigtab'>"
                               "<tr><td class='kv-l'>最近 RRC</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>TA</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>MCS</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>调制</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>MIMO</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>layers</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>DL/UL Grant</td><td class='val sm'>%s/%s</td></tr>"
                               "<tr><td class='kv-l'>DL/UL RB</td><td class='val sm'>%s/%s</td></tr>"
                               "<tr><td class='kv-l'>BLER</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>PUSCH</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>PUCCH</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>Ports</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>SSB Index</td><td class='val sm'>%s</td></tr>"
                               "</table></div>",
                               e_nr_rrc, e_nrta, e_nrmcs, e_nrmod, e_nrmimo, e_nrlayers,
                               e_nrdlgrant, e_nrulgrant, e_nrdlrb, e_nrulrb, e_nrbler, e_nrpusch,
                               e_nrpucch, e_nrports, e_nrservssb);
            }
        }
        if (show_lte) {
            so += snprintf(s_signalcards + so, sizeof s_signalcards - so,
                           "<div class='card'><div class='title'>LTE 信令 <span class='r muted'>4G</span></div><table class='sigtab'>"
                           "<tr><td class='kv-l'>最近 RRC</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>最近 NAS</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>TA</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>MCS</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>调制</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>Grant</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>RB</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>BLER</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>PUSCH</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>PUCCH</td><td class='val sm'>%s</td></tr>"
                           "</table></div>",
                           e_lte_rrc, e_lte_nas, e_lteta, e_ltemcs, e_ltemod,
                           e_ltegrant, e_lterb, e_ltebler, e_ltepusch, e_ltepucch);
        }
        if (so == 0) {
            snprintf(s_signalcards, sizeof s_signalcards,
                     "<div class='card'><div class='title'>信令信息</div><div class='sec'>当前网络没有可展示的 LTE / NR 信令卡片。</div></div>");
            so = (int)strlen(s_signalcards);
        }
    } else {
        snprintf(s_nrports, sizeof s_nrports, "-");
        snprintf(s_nrbeam, sizeof s_nrbeam, "-");
        snprintf(s_nrvendor, sizeof s_nrvendor, "-");
        s_signalcards[0] = 0;
    }

    /* NR carriers: PCell from main fields + nrca SCells */
    s_nrrows[0] = 0;
    if (show_nr && nr_band[0] && d.nr_bw[0]) {
        struct signal_carrier_metric *metric;
        snprintf(rp, sizeof rp, "%d", d.nr_rsrp); snprintf(pc, sizeof pc, "%d", d.nr_pci);
        snprintf(ac, sizeof ac, "%ld", d.nr_channel);
        metric = signal_pick_carrier_metric(sig.nr_carriers, sig.nr_carrier_n, pc, ac, 1);
        no = car_row(s_nrrows, no, sizeof s_nrrows, nr_band, d.nr_bw, ac, pc, rp, d.nr_snr,
                     hsr_freq_hint(d.mcc, ac), metric);
        nr_show_cc = 1; nr_show_bw = atoi(d.nr_bw);
        if (!carrier_is_inactive(rp)) { nr_cc = 1; nr_bw = atoi(d.nr_bw); }
    }
    if (show_nr) {
        char nrca[256]; strncpy(nrca, d.nrca, sizeof nrca - 1); nrca[sizeof nrca - 1] = 0;
        for (char *grp = strtok(nrca, ";"); grp; grp = strtok(NULL, ";")) {
            char g[96]; strncpy(g, grp, sizeof g - 1); g[sizeof g - 1] = 0;
            char *f[12]; int nf = ca_split(g, f, 12);
            if (nf > 5 && atoi(f[5]) > 0) {
                double rpv = (nf > 7 && f[7] && f[7][0]) ? atof(f[7]) : 0.0;
                char bn[12]; snprintf(bn, sizeof bn, "n%s", f[3]);
                const char *nr_ac = nf > 4 ? f[4] : "-";
                const char *nr_pci = nf > 1 ? f[1] : "-";
                struct signal_carrier_metric *metric =
                    signal_pick_carrier_metric(sig.nr_carriers, sig.nr_carrier_n,
                                               nr_pci, nr_ac, 0);
                no = car_row(s_nrrows, no, sizeof s_nrrows, bn, f[5], nr_ac,
                             nr_pci, nf > 7 ? f[7] : "-", nf > 9 ? f[9] : "-",
                             hsr_freq_hint(d.mcc, nr_ac), metric);
                nr_show_cc++;
                nr_show_bw += atoi(f[5]);
                if (rpv > -140.0) { nr_cc++; nr_bw += atoi(f[5]); }
            }
        }
    }
    /* LTE carriers: support both legacy 5-field and newer 11-field groups. */
    s_lterows[0] = 0;
    if (show_lte) {
        char lteca[256]; strncpy(lteca, d.lteca, sizeof lteca - 1); lteca[sizeof lteca - 1] = 0;
        char ltecasig[256]; strncpy(ltecasig, d.ltecasig, sizeof ltecasig - 1); ltecasig[sizeof ltecasig - 1] = 0;
        char *lgrps[8], *sgrps[8];
        int lgn = ca_groups(lteca, lgrps, 8);
        int sgn = ca_groups(ltecasig, sgrps, 8);
        int sig_off = (sgn > 0 && lgn == sgn + 1);   /* ltecasig often contains SCC only */
        for (int gi = 0; gi < lgn; gi++) {
            char *grp = lgrps[gi];
            const char *siggrp = NULL;
            char g[96]; strncpy(g, grp, sizeof g - 1); g[sizeof g - 1] = 0;
            char *f[12]; int nf = ca_split(g, f, 12);
            const char *f_band = NULL, *f_bw = NULL, *f_arfcn = NULL, *f_pci = NULL, *f_rp = NULL, *f_sn = NULL;
            const char *sig_rp = NULL, *sig_sn = NULL;
            char sig_rp_buf[16] = "", sig_sn_buf[16] = "";
            if (nf >= 11) {
                f_band = f[3]; f_arfcn = f[4]; f_bw = f[5]; f_pci = f[1]; f_rp = f[7]; f_sn = f[9];
            } else if (nf >= 5) {
                f_band = f[1]; f_arfcn = f[3]; f_bw = f[4]; f_pci = f[0];
            }
            if (sgn == lgn && gi < sgn) siggrp = sgrps[gi];
            else if (sig_off && gi > 0 && gi - 1 < sgn) siggrp = sgrps[gi - 1];
            else if (gi < sgn) siggrp = sgrps[gi];
            if (siggrp && siggrp[0]) {
                char sg[96]; strncpy(sg, siggrp, sizeof sg - 1); sg[sizeof sg - 1] = 0;
                if (ca_sig_pick(sg, &sig_rp, &sig_sn)) {
                    if (sig_rp && sig_rp[0]) snprintf(sig_rp_buf, sizeof sig_rp_buf, "%s", sig_rp);
                    if (sig_sn && sig_sn[0]) snprintf(sig_sn_buf, sizeof sig_sn_buf, "%s", sig_sn);
                }
            }
            if ((!f_rp || !f_rp[0]) && sig_rp_buf[0]) f_rp = sig_rp_buf;
            if ((!f_sn || !f_sn[0]) && sig_sn_buf[0]) f_sn = sig_sn_buf;
            if (f_bw && atoi(f_bw) > 0) {
                char bn[12];
                if (f_band && (f_band[0] == 'B' || f_band[0] == 'b')) snprintf(bn, sizeof bn, "%s", f_band);
                else snprintf(bn, sizeof bn, "B%s", (f_band && f_band[0]) ? f_band : "-");
                char lr[12] = "-", ls[12] = "-";
                if (f_rp && f_rp[0]) snprintf(lr, sizeof lr, "%s", f_rp);
                if (f_sn && f_sn[0]) snprintf(ls, sizeof ls, "%s", f_sn);
                if (lte_cc == 0) {   /* serving-cell signal from main fields */
                    if (d.lte_rsrp) snprintf(lr, sizeof lr, "%d", d.lte_rsrp);
                    if (d.lte_snr[0]) snprintf(ls, sizeof ls, "%s", d.lte_snr);
                }
                struct signal_carrier_metric *metric =
                    signal_pick_carrier_metric(sig.lte_carriers, sig.lte_carrier_n,
                                               f_pci ? f_pci : "-", f_arfcn ? f_arfcn : "-",
                                               gi == 0);
                lo = car_row(s_lterows, lo, sizeof s_lterows, bn, f_bw, f_arfcn ? f_arfcn : "-",
                             f_pci ? f_pci : "-", lr, ls, hsr_freq_hint(d.mcc, f_arfcn),
                             metric);
                lte_show_cc++;
                lte_show_bw += atoi(f_bw);
                if (!carrier_is_inactive(lr)) { lte_cc++; lte_bw += atoi(f_bw); }
            }
        }
    }
    (void)no; (void)lo; (void)pc; (void)ac;

    /* Display summary includes inactive/configured carriers; generation badge
     * still uses active carrier stats above. */
    int total_bw, total_show_bw; const char *cmode;
    if (is_endc)     { total_bw = nr_bw + lte_bw; total_show_bw = nr_show_bw + lte_show_bw; cmode = "EN-DC"; }
    else if (is_nsa) { total_bw = nr_bw + lte_bw; total_show_bw = nr_show_bw + lte_show_bw; cmode = "5G NSA"; }
    else if (is_sa)  { total_bw = nr_bw;          total_show_bw = nr_show_bw;               cmode = "5G SA"; }
    else if (is_lte) { total_bw = lte_bw;         total_show_bw = lte_show_bw;              cmode = "4G LTE"; }
    else             { total_bw = 0;              total_show_bw = 0;                         cmode = d.net_type[0] ? d.net_type : "No Service"; }
    { char cnt[48];
      if (lte_show_cc && nr_show_cc) snprintf(cnt, sizeof cnt, "%d LTE + %d NR 载波", lte_show_cc, nr_show_cc);
      else if (nr_show_cc)           snprintf(cnt, sizeof cnt, "%d NR 载波", nr_show_cc);
      else if (lte_show_cc)          snprintf(cnt, sizeof cnt, "%d LTE 载波", lte_show_cc);
      else                 snprintf(cnt, sizeof cnt, "无载波");
      snprintf(s_carriers, sizeof s_carriers, "<div class='sec'>%s · %s · %d MHz</div>%s%s",
               cmode, cnt, total_show_bw, s_nrrows, s_lterows); }
    const char *hsr_card_cls = d.hsr ? " hsrmode" : "";
    const char *hsr_mode_note = d.hsr ? "<div class='hsrmode-note'>高铁模式</div>" : "";
    if ((is_endc || is_nsa) && s_nrrows[0] && s_lterows[0]) {
        snprintf(s_nr_block, sizeof s_nr_block,
                 "<div class='card%s'>"
                 "<div class='title'>%s <span class='sub'>%s</span>"
                 "<span class='qa'><b>QCI %s</b><br>AMBR %s</span></div>"
                 "%s"
                 "<div class='sec'>%s · %d LTE + %d NR 载波 · %d MHz</div>"
                 "<div class='ctitle'>NR</div>%s"
                 "</div>",
                 hsr_card_cls, s_oper, d.net_type, s_qci, s_ambr, hsr_mode_note,
                 cmode, lte_show_cc, nr_show_cc, total_show_bw, s_nrrows);
        snprintf(s_lte_block, sizeof s_lte_block,
                 "<div class='card'><div class='ctitle'>LTE</div>"
                 "<div class='sec'>%d LTE 载波 · %d MHz</div>%s</div>",
                 lte_show_cc, lte_show_bw, s_lterows);
        snprintf(s_sigcards, sizeof s_sigcards, "%s%s", s_nr_block, s_lte_block);
    } else {
        snprintf(s_sigcards, sizeof s_sigcards,
                 "<div class='card%s'>"
                 "<div class='title'>%s <span class='sub'>%s</span>"
                 "<span class='qa'><b>QCI %s</b><br>AMBR %s</span></div>"
                 "%s"
                 "%s"
                 "</div>",
                 hsr_card_cls, s_oper, d.net_type, s_qci, s_ambr, hsr_mode_note, s_carriers);
    }

    /* generation badge: 5GA / 5G+ / 5G / 4G / LTE / 3G */
    int gen_nr_cc = nr_show_cc;
    int gen_nr_bw = nr_show_bw;
    int op = 0;   /* 1 mobile, 2 unicom, 3 telecom, 4 broadnet, 5 other-mainland */
    if (d.mcc == 460) {
        int m = d.mnc;
        if (m == 0 || m == 2 || m == 4 || m == 7 || m == 8) op = 1;
        else if (m == 1 || m == 6 || m == 9) op = 2;
        else if (m == 3 || m == 5 || m == 11) op = 3;
        else if (m == 15) op = 4;
        else op = 5;
    }
    if (d.mcc == 460) {
        const char *cn = op == 1 ? "中国移动" : op == 2 ? "中国联通" :
                         op == 3 ? "中国电信" : op == 4 ? "中国广电" : NULL;
        if (cn) snprintf(s_oper, sizeof s_oper, "%s", cn);
    }
    const char *gen = "--";
    if (is_sa) {
        if ((op == 1 || op == 4) && gen_nr_cc >= 3) gen = "5GA";
        else if ((op == 2 || op == 3) && gen_nr_bw >= 200) gen = "5GA";
        else if (gen_nr_bw > 100) gen = "5G+";
        else gen = "5G";
    } else if (is_nsa || is_endc) gen = "5G";
    else if (is_lte) gen = (d.mcc == 460) ? "4G" : "LTE";
    else if (d.net_type[0]) gen = "3G";
    snprintf(s_gen, sizeof s_gen, "%s", gen);
    const char *genc = "g3";
    if      (!strcmp(gen, "5GA")) genc = "g5ga";
    else if (!strcmp(gen, "5G+")) genc = "g5p";
    else if (!strcmp(gen, "5G"))  genc = "g5";
    else if (!strcmp(gen, "4G"))  genc = "g4";
    else if (!strcmp(gen, "LTE")) genc = "glte";
    snprintf(g_stat_time, sizeof g_stat_time, "%s", s_time);
    g_stat_sig = signal_level(d.nr_rsrp ? d.nr_rsrp : d.lte_rsrp);
    snprintf(g_stat_gen, sizeof g_stat_gen, "%s", gen);

    /* ---- shared status bar: native-drawn content over a simple 26px slot ---- */
    {
        char sp[40];
        fmt_speed_pair(sp, sizeof sp, d.tx_speed, d.rx_speed, g_speed_bits);
        snprintf(g_stat_speed, sizeof g_stat_speed, "%s", sp);
        g_stat_bat = clampi(d.bat_percent, 0, 100);
        g_stat_lowbat = d.bat_percent <= 20;
        g_sms_unread_now = d.sms_unread > 0;
        snprintf(s_sbar, sizeof s_sbar, "<div class='sbar'></div>");
    }

    /* ---- bottom page dots ---- */
    {
        int o = snprintf(s_dots, sizeof s_dots, "<div class='dots'>");
        for (int p = 0; p < g_npages; p++)
            o += snprintf(s_dots + o, sizeof s_dots - o, "<span class='dot%s'></span>", p == g_cur ? " on" : "");
        snprintf(s_dots + o, sizeof s_dots - o, "</div>");
    }

    /* charts are drawn natively after render (see draw_charts), not via tokens. */

    /* ---- band lock: universe grows to the largest set seen; selection mirrors
     * the live lock unless the user is editing in the modal ---- */
    static char s_netseg[640], s_simswitch[1200], s_cursa[300], s_curnsa[300], s_curlte[300], s_toast[120];
    static char s_ts_action_log[2200], s_mh_action_log[2200], s_cpu_action_log[2200];
    static char s_wg_action_log[2200], s_op_action_log[2200];
    static char s_sim_action_log[2200], s_fm_action_log[2200];
    static char s_wg_peers[24], s_wg_active[24], s_op_selected[16], s_op_job[440];
    static char s_wg_iface[80], s_wg_address[220], s_wg_port[48], s_wg_mode[64];
    static char s_op_sim[80], s_op_operator[96], s_op_rat[80], s_op_mode[64];
    static char s_op_rat_pref[48], s_op_policy[64], s_op_job_raw[220];
    static char s_fm_provider[96], s_fm_nettype[64], s_fm_band[80], s_fm_signal[48], s_fm_plmn[48];
    if (band_count(d.sa_bands)  >= band_count(g_uni_sa))  snprintf(g_uni_sa,  sizeof g_uni_sa,  "%s", d.sa_bands);
    if (band_count(d.nsa_bands) >= band_count(g_uni_nsa)) snprintf(g_uni_nsa, sizeof g_uni_nsa, "%s", d.nsa_bands);
    if (band_count(d.lte_bands) >= band_count(g_uni_lte)) snprintf(g_uni_lte, sizeof g_uni_lte, "%s", d.lte_bands);
    if (!g_modal) {
        if (d.sa_bands[0])  snprintf(g_sel_sa,  sizeof g_sel_sa,  "%s", d.sa_bands);
        if (d.nsa_bands[0]) snprintf(g_sel_nsa, sizeof g_sel_nsa, "%s", d.nsa_bands);
        if (d.lte_bands[0]) snprintf(g_sel_lte, sizeof g_sel_lte, "%s", d.lte_bands);
    }
    band_summary(s_cursa,  sizeof s_cursa,  g_uni_sa,  g_sel_sa,  "n");
    band_summary(s_curnsa, sizeof s_curnsa, g_uni_nsa, g_sel_nsa, "n");
    band_summary(s_curlte, sizeof s_curlte, g_uni_lte, g_sel_lte, "B");
    {   /* segmented control: highlight current mode (pending overrides until it applies) */
        if (g_net_pending[0] && !strcmp(g_net_pending, d.net_select)) g_net_pending[0] = 0;
        const char *cur = g_net_pending[0] ? g_net_pending : d.net_select;
        int active = 0;
        for (int k = 0; k < 4; k++) if (!strcmp(g_netmodes[k].v, cur)) active = k;
        int hl = active;
        int o = snprintf(s_netseg, sizeof s_netseg, "<div id='netseg' class='seg seg4'>");
        for (int k = 0; k < 4; k++)
            o += snprintf(s_netseg + o, sizeof s_netseg - o, "<a href='act:net:%s' class='segc%s'>%s</a>",
                          g_netmodes[k].v, k == hl ? " seg-on" : "", g_netmodes[k].lab);
        snprintf(s_netseg + o, sizeof s_netseg - o, "</div>");
    }
    s_simswitch[0] = 0;
    if (g_sim_dual == 1) {
        const char *hint = "切卡会短暂断网，点击目标卡后需在 4 秒内再次点击确认";
        char dynamic_hint[160];
        int o = snprintf(s_simswitch, sizeof s_simswitch,
                         "<div class='card simcard'><div class='title'>SIM 卡切换</div>"
                         "<div class='seg seg2 simseg'>");
        for (int slot = 1; slot <= 2; slot++) {
            const char *cls = slot == g_sim_slot ? " seg-on" :
                              g_sim_switching && slot == g_sim_switch_target ? " sim-busy" :
                              g_sim_confirm_slot == slot ? " sim-armed" : "";
            const char *sub = g_sim_switching && slot == g_sim_switch_target ? "切换中" :
                              g_sim_confirm_slot == slot ? "再次按下" : "";
            o += snprintf(s_simswitch + o, sizeof s_simswitch - o,
                          "<a href='act:simslot:%d' class='segc%s'><span class='sim-main'>%s</span>%s%s%s</a>",
                          slot, cls, sim_slot_label(slot), sub[0] ? "<br><span class='sim-sub'>" : "",
                          sub, sub[0] ? "</span>" : "");
        }
        if (g_sim_switching) {
            snprintf(dynamic_hint, sizeof dynamic_hint, "正在切换到%s，蜂窝网络会短暂中断。",
                     sim_slot_label(g_sim_switch_target));
            hint = dynamic_hint;
        } else if (g_sim_confirm_slot) {
            snprintf(dynamic_hint, sizeof dynamic_hint, "4 秒内再次点击%s确认切换。",
                     sim_slot_label(g_sim_confirm_slot));
            hint = dynamic_hint;
        }
        snprintf(s_simswitch + o, sizeof s_simswitch - o,
                 "</div><div class='simhint'>%s</div></div>", hint);
    }
    s_toast[0] = 0;
    if (g_toast[0]) snprintf(s_toast, sizeof s_toast, "<div class='toast'>%s</div>", g_toast);

    /* ---- system extras: usage, temps, version, imei, brightness, auto-off ---- */
    static char s_cusage[8], s_ctemp[8], s_btemp[8], s_swver[80], s_imei[24], s_spu[8], s_bright[8];
    static char s_memdet[24], s_upshort[16], s_autooff[420], s_refreshseg[480];
    { int bmax = backlight_max(); if (bmax <= 0) bmax = 255;
      snprintf(s_bright, sizeof s_bright, "%d", clampi(backlight_get() * 100 / bmax, 0, 100)); }
    { long mt = d.mem_total, ma = d.mem_avail;
      snprintf(s_memdet, sizeof s_memdet, "%ld/%ld MB", mt ? (mt - ma) / 1048576 : 0, mt / 1048576); }
    static char s_chgv[8], s_chgi[10], s_batv[8], s_bati[10], s_pwr[10], s_batpwr[10], s_batpwrlbl[8];
    snprintf(s_chgv, sizeof s_chgv, "%.2f", d.chg_uv / 1e6);
    snprintf(s_chgi, sizeof s_chgi, "%ld", d.chg_ua / 1000);
    snprintf(s_batv, sizeof s_batv, "%.2f", d.bat_uv / 1e6);
    /* signed: + while charging, - while discharging (matches PWRLBL) */
    snprintf(s_bati, sizeof s_bati, "%s%ld", d.charger_connect ? "+" : "-", labs(d.bat_ua) / 1000);
    { double pw = d.charger_connect ? (d.chg_uv / 1e6) * (d.chg_ua / 1e6)
                                    : (d.bat_uv / 1e6) * (labs(d.bat_ua) / 1e6);
      snprintf(s_pwr, sizeof s_pwr, "%.1f", pw); }
    {
        double bat_pw = (d.bat_uv / 1e6) * (labs(d.bat_ua) / 1e6);
        snprintf(s_batpwr, sizeof s_batpwr, "%.1f", bat_pw);
        snprintf(s_batpwrlbl, sizeof s_batpwrlbl, "%s", d.bat_ua >= 0 ? "充电" : "放电");
    }
    fmt_uptime_s(s_upshort, sizeof s_upshort, d.uptime);
    {   /* auto-off segmented control (#autoseg), same UI as net mode */
        int active = 0;
        for (int k = 0; k < 6; k++) if (g_autooffs[k].ms == g_autooff_ms) active = k;
        int hl = active;
        int o = snprintf(s_autooff, sizeof s_autooff, "<div id='autoseg' class='seg seg6'>");
        for (int k = 0; k < 6; k++)
            o += snprintf(s_autooff + o, sizeof s_autooff - o,
                "<a href='act:autooff:%d' class='segc%s'>%s</a>",
                g_autooffs[k].ms, k == hl ? " seg-on" : "", g_autooffs[k].lab);
        snprintf(s_autooff + o, sizeof s_autooff - o, "</div>");
    }
    {   /* refresh segmented control (#refreshseg), same UI as auto-off */
        int active = 0;
        for (int k = 0; k < 5; k++) if (g_refresh_rates[k].ms == g_refresh_ms) active = k;
        int hl = active;
        int o = snprintf(s_refreshseg, sizeof s_refreshseg, "<div id='refreshseg' class='seg seg5'>");
        for (int k = 0; k < 5; k++)
            o += snprintf(s_refreshseg + o, sizeof s_refreshseg - o,
                "<a href='act:refreshms:%d' class='segc%s'>%s</a>",
                g_refresh_rates[k].ms, k == hl ? " seg-on" : "", g_refresh_rates[k].lab);
        snprintf(s_refreshseg + o, sizeof s_refreshseg - o, "</div>");
    }
    snprintf(s_cusage, sizeof s_cusage, "%ld", d.cpu_usage < 0 ? 0 : d.cpu_usage);
    snprintf(s_ctemp, sizeof s_ctemp, "%ld", d.cpu_temp);
    snprintf(s_btemp, sizeof s_btemp, "%d", d.bat_temp);
    snprintf(s_swver, sizeof s_swver, "%s", d.sw_version[0] ? d.sw_version : "-");
    if (g_show_imei && d.imei[0]) snprintf(s_imei, sizeof s_imei, "%s", d.imei);
    else { int n = d.imei[0] ? (int)strlen(d.imei) : 15; if (n > 23) n = 23; memset(s_imei, '*', n); s_imei[n] = 0; }
    snprintf(s_spu, sizeof s_spu, "%s", g_speed_bits ? "Mbps" : "MB/s");

    /* ---- connected device list (page 2): all of them, filtered to currently
     * WiFi-associated MACs (leases linger after a device leaves) ---- */
    static char s_clist[2600];
    {
        int o = 0, shown = 0;
        for (int i = 0; i < d.client_n; i++) {
            if (!mac_assoc(d.client[i].mac)) continue;
            o += snprintf(s_clist + o, sizeof s_clist - o,
                "<div class='cli'><span class='cn'>%s</span><span class='cip'>%s</span></div>",
                d.client[i].name, d.client[i].ip);
            shown++;
        }
        if (shown == 0)
            snprintf(s_clist, sizeof s_clist, "<div class='cli muted'>暂无已连接设备</div>");
        /* keep the header count consistent with the filtered list */
        if (g_assoc_macs[0]) snprintf(s_clients, sizeof s_clients, "%d", shown);
    }

    /* ---- sms list (read-only page): collapsed cards, newest first. Each card is
     * a tap target (act:sms:ID) that opens the full message in a dialog. ---- */
    static char s_smslist[131072];
    {
        int o = 0;
        for (int si = 0; si < d.sms_n; si++) {
            char prev[80]; int cut;
            utf8_trunc(prev, sizeof prev, d.sms[si].text, 40, &cut);
            char esc[512];
            html_esc(esc, sizeof esc, prev);
            if (o >= (int)sizeof(s_smslist)) break;
            int wr = snprintf(s_smslist + o, sizeof(s_smslist) - o,
                "<a href='act:sms:%ld' class='sms%s'>"
                "<div class='smsh'><span class='smsn'>%s</span><span class='smsd'>%s</span></div>"
                "<div class='smsp'>%s%s</div></a>",
                d.sms[si].id, d.sms[si].unread ? " un" : "", d.sms[si].num, d.sms[si].date,
                esc, cut ? "…" : "");
            if (wr < 0) wr = 0;
            if (wr >= (int)(sizeof(s_smslist) - o)) {
                o = (int)sizeof(s_smslist) - 1;
                s_smslist[o] = 0;
                break;
            }
            o += wr;
        }
        if (d.sms_n == 0)
            snprintf(s_smslist, sizeof s_smslist, "<div class='sms muted'>暂无短信</div>");
    }

    /* ---- dhcp summary (page 2) ---- */
    static char s_pool[64], s_lease[24];
    snprintf(s_pool, sizeof s_pool, "%s · 共 %s",
             d.dhcp_start[0] ? d.dhcp_start : "-", d.dhcp_limit[0] ? d.dhcp_limit : "-");
    { long lt = atol(d.dhcp_leasetime);
      if (lt >= 3600)     snprintf(s_lease, sizeof s_lease, "%ld 小时", lt / 3600);
      else if (lt >= 60)  snprintf(s_lease, sizeof s_lease, "%ld 分钟", lt / 60);
      else if (lt > 0)    snprintf(s_lease, sizeof s_lease, "%ld 秒", lt);
      else                strcpy(s_lease, "-"); }

    int adb_on = !strcmp(d.usb_mode, "debug");
    int usb_net_on = (g_usb_net == 1) || usb_pid_is("9057") || usb_pid_is("90b1") || usb_pid_is("90B1");
    int usb_reverse = (g_typec_source == 1);
    uint32_t now_ms = millis();
    if (g_adb_pending >= 0) {
        if (g_adb_pending == adb_on) g_adb_pending = -1;
        else                         adb_on = g_adb_pending;
    }
    if (g_usb_net_pending >= 0) {
        if (g_usb_net_pending == usb_net_on || now_ms > g_usb_net_until) g_usb_net_pending = -1;
        else                                                            usb_net_on = g_usb_net_pending;
    }
    if (g_typec_pending >= 0) {
        if (g_typec_pending == usb_reverse || now_ms > g_typec_until) g_typec_pending = -1;
        else                                                         usb_reverse = g_typec_pending;
    }
    const char *adb_state = g_adb_pending >= 0 ? "切换中" : (adb_on ? "已开启" : "已关闭");
    const char *usb_pwr_state = g_typec_pending >= 0 ? "切换中" : (usb_reverse ? "反向供电" : "给U60充电");
    const char *usb_net_state = g_usb_net_pending >= 0 ? "切换中" : (usb_net_on ? "共享中" : "仅充电");

    /* ---- bundled timezone, dual-SIM management and persistent traffic ---- */
    static char s_sim_mode[32], s_sim_current[128], s_sim_auto[24];
    static char s_sim1op[128], s_sim2op[128], s_sim1state[96], s_sim2state[96];
    static char s_tz_current[32], s_tz_draft[32], s_tz_effective[32];
    static char s_tz_minutes[560], s_tz_dst[320];
    static char s_plan_gib[24], s_plan_day[8], s_plan_title[128], s_plan_state[160];
    const devui_sim_traffic_t *plan_sim = sim_traffic_for_slot(&d, g_plan_edit_slot);
    int sim_mode_single = (g_sim_p1 + g_sim_p2) == 1 &&
                          (g_sim_p1 == 1 || g_sim_p2 == 1);
    int sim_mode_dual = g_sim_p1 == 1 && g_sim_p2 == 1;
    int sim_mode_armed = g_sim_mode_confirm &&
                         (int32_t)(g_sim_mode_confirm_until - now_ms) > 0;
    int sim_slot_armed = g_sim_confirm_slot &&
                         (int32_t)(g_sim_confirm_until - now_ms) > 0;

    snprintf(s_sim_mode, sizeof s_sim_mode, "%s",
             sim_mode_dual ? "双卡双待" : sim_mode_single ? "单卡模式" : "状态未知");
    {
        char raw[96];
        snprintf(raw, sizeof raw, "%s · %s", sim_slot_label(g_sim_slot),
                 sim_operator_display(g_sim_slot == 2 ? g_sim_operator2 : g_sim_operator1));
        html_esc(s_sim_current, sizeof s_sim_current, raw);
    }
    snprintf(s_sim_auto, sizeof s_sim_auto, "%s",
             g_sim_auto == 1 ? "已开启" : g_sim_auto == 0 ? "已关闭" : "状态未知");
    html_esc(s_sim1op, sizeof s_sim1op, sim_operator_display(g_sim_operator1));
    html_esc(s_sim2op, sizeof s_sim2op, sim_operator_display(g_sim_operator2));
    html_esc(s_sim1state, sizeof s_sim1state, g_sim_state1);
    html_esc(s_sim2state, sizeof s_sim2state, g_sim_state2);
    html_esc(s_fm_provider, sizeof s_fm_provider, g_fm_provider);
    html_esc(s_fm_nettype, sizeof s_fm_nettype, g_fm_nettype);
    html_esc(s_fm_band, sizeof s_fm_band, g_fm_band);
    html_esc(s_fm_signal, sizeof s_fm_signal, g_fm_signal);
    html_esc(s_fm_plmn, sizeof s_fm_plmn, g_fm_plmn);

    snprintf(s_tz_current, sizeof s_tz_current, "%s",
             d.timezone_available && d.timezone_label[0] ? d.timezone_label : "UTC+08:00");
    timezone_offset_label(s_tz_draft, sizeof s_tz_draft, timezone_draft_offset());
    timezone_offset_label(s_tz_effective, sizeof s_tz_effective,
                          timezone_draft_offset() + (g_tz_draft_dst ? 60 : 0));
    {
        static const int minutes[] = { 0, 15, 30, 45 };
        int o = snprintf(s_tz_minutes, sizeof s_tz_minutes, "<div class='seg seg4'>");
        for (size_t k = 0; k < ARRAY_LEN(minutes); k++)
            o += snprintf(s_tz_minutes + o, sizeof s_tz_minutes - (size_t)o,
                          "<a href='act:tzminute:%d' class='segc%s'>:%02d</a>",
                          minutes[k], minutes[k] == g_tz_draft_minute ? " seg-on" : "", minutes[k]);
        snprintf(s_tz_minutes + o, sizeof s_tz_minutes - (size_t)o, "</div>");
    }
    snprintf(s_tz_dst, sizeof s_tz_dst,
             "<div class='seg seg2'>"
             "<a href='act:tzdst:0' class='segc%s'>标准时间</a>"
             "<a href='act:tzdst:1' class='segc%s'>夏令时 +1h</a></div>",
             g_tz_draft_dst ? "" : " seg-on", g_tz_draft_dst ? " seg-on" : "");

    snprintf(s_plan_gib, sizeof s_plan_gib, "%llu GiB", g_plan_draft_gib);
    snprintf(s_plan_day, sizeof s_plan_day, "%d", g_plan_draft_reset_day);
    if (plan_sim) {
        char raw[128];
        snprintf(raw, sizeof raw, "%s · %s · 尾号 %s",
                 sim_slot_label(g_plan_edit_slot), sim_operator_display(plan_sim->operator_name),
                 plan_sim->iccid_tail[0] ? plan_sim->iccid_tail : "-");
        html_esc(s_plan_title, sizeof s_plan_title, raw);
        snprintf(s_plan_state, sizeof s_plan_state, "%s · 每月 %d 日重置",
                 g_plan_draft_enabled ? "套餐统计已开启" : "套餐统计已关闭",
                 g_plan_draft_reset_day);
    } else {
        snprintf(s_plan_title, sizeof s_plan_title, "%s · 尚无 ICCID 记录",
                 sim_slot_label(g_plan_edit_slot));
        snprintf(s_plan_state, sizeof s_plan_state, "请先使用该卡联网，后端识别 ICCID 后即可设置套餐");
    }

    int connected = strstr(d.wan_status, "connect") != NULL;
    int wifi_master = (g_w24 == 1 || g_w5 == 1);
    int i = 0;
    t[i++] = (struct kv){ "STATUSBAR", s_sbar };   t[i++] = (struct kv){ "DOTS", s_dots };
    t[i++] = (struct kv){ "SIGBARS", "" };
    t[i++] = (struct kv){ "TIME", s_time };        t[i++] = (struct kv){ "BAT", s_bat };
    t[i++] = (struct kv){ "OPER", s_oper };        t[i++] = (struct kv){ "NETTYPE", d.net_type };
    t[i++] = (struct kv){ "BAND", d.band };        t[i++] = (struct kv){ "WAN", connected ? "已连接" : "未连接" };
    t[i++] = (struct kv){ "RSRP", s_rsrp };        t[i++] = (struct kv){ "RSRP_W", w_rsrp };
    t[i++] = (struct kv){ "RSRQ", s_rsrq };        t[i++] = (struct kv){ "RSRQ_W", w_rsrq };
    t[i++] = (struct kv){ "SINR", s_sinr };        t[i++] = (struct kv){ "SINR_W", w_sinr };
    t[i++] = (struct kv){ "BW", s_bw };            t[i++] = (struct kv){ "BW_W", w_bw };
    t[i++] = (struct kv){ "CELLID", s_cellid };    t[i++] = (struct kv){ "PCI", s_pci };
    t[i++] = (struct kv){ "TA", s_ta };            t[i++] = (struct kv){ "TADIST", s_tadist };
    t[i++] = (struct kv){ "SIGDETAILCARDS", s_signalcards };
    t[i++] = (struct kv){ "CELLBTN", g_show_cellid ? "隐藏" : "显示" };
    t[i++] = (struct kv){ "CLIENTS", s_clients };  t[i++] = (struct kv){ "UPTIME", s_up };
    t[i++] = (struct kv){ "RXSPEED", s_rxs };      t[i++] = (struct kv){ "TXSPEED", s_txs };
    t[i++] = (struct kv){ "RXBYTES", s_rxb };      t[i++] = (struct kv){ "TXBYTES", s_txb };
    t[i++] = (struct kv){ "CPU", s_cpu };          t[i++] = (struct kv){ "MEM", s_mem };
    t[i++] = (struct kv){ "QCI", s_qci };          t[i++] = (struct kv){ "AMBR", s_ambr };
    t[i++] = (struct kv){ "SSID", s_ssid };        t[i++] = (struct kv){ "KEY", s_key };
    t[i++] = (struct kv){ "ENC", d.wifi_enc[0] ? d.wifi_enc : "-" };
    t[i++] = (struct kv){ "MODEL", s_model };      t[i++] = (struct kv){ "FW", s_fw };
    t[i++] = (struct kv){ "PAGE", s_page };        t[i++] = (struct kv){ "NPAGES", s_np };
    t[i++] = (struct kv){ "CARRIERS", s_carriers }; t[i++] = (struct kv){ "GEN", s_gen };
    t[i++] = (struct kv){ "SIGNALCARDS", s_sigcards };
    t[i++] = (struct kv){ "NEIGHBORCARDS", s_neighborcards };
    t[i++] = (struct kv){ "GENCLASS", genc };
    t[i++] = (struct kv){ "THEME", g_theme ? "light" : "dark" };
    t[i++] = (struct kv){ "BATCLASS", d.bat_percent <= 20 ? "low" : "" };
    t[i++] = (struct kv){ "KEYBTN", g_show_key ? "隐藏密码" : "显示密码" };
    t[i++] = (struct kv){ "ADBCLASS", adb_on ? "on" : "off" };
    t[i++] = (struct kv){ "ADBSTATE", adb_state };
    t[i++] = (struct kv){ "USBPWRCLASS", usb_reverse ? "on" : "off" };
    t[i++] = (struct kv){ "USBPWRSTATE", usb_pwr_state };
    t[i++] = (struct kv){ "USBNETCLASS", usb_net_on ? "on" : "off" };
    t[i++] = (struct kv){ "USBNETSTATE", usb_net_state };
    t[i++] = (struct kv){ "BATPCTCLASS", g_show_batpct ? "on" : "off" };
    t[i++] = (struct kv){ "BATPCTSTATE", g_show_batpct ? "已显示" : "已隐藏" };
    t[i++] = (struct kv){ "THEMECLASS", g_theme ? "on" : "off" };
    t[i++] = (struct kv){ "THEMESTATE", g_theme ? "浅色模式" : "深色模式" };
    t[i++] = (struct kv){ "SPUNITCLASS", g_speed_bits ? "on" : "off" };
    t[i++] = (struct kv){ "SPUNITSTATE", g_speed_bits ? "比特率 Mbps" : "字节率 MB/s" };
    t[i++] = (struct kv){ "SPUNIT", s_spu };
    t[i++] = (struct kv){ "CPUUSAGE", s_cusage }; t[i++] = (struct kv){ "CPUTEMP", s_ctemp };
    t[i++] = (struct kv){ "BATTEMP", s_btemp };   t[i++] = (struct kv){ "SWVER", s_swver };
    t[i++] = (struct kv){ "IMEI", s_imei };
    t[i++] = (struct kv){ "IMEIBTN", g_show_imei ? "隐藏" : "显示" };
    t[i++] = (struct kv){ "BRIGHT", s_bright };   t[i++] = (struct kv){ "AUTOOFF", s_autooff };
    t[i++] = (struct kv){ "REFRESHSEG", s_refreshseg };
    t[i++] = (struct kv){ "CHARTINTERVALS", chart_intervals_html() };
    t[i++] = (struct kv){ "STSRCSEG", speedtest_src_html() };
    t[i++] = (struct kv){ "STDIRSEG", speedtest_dir_html() };
    t[i++] = (struct kv){ "STDURSEG", speedtest_dur_html() };
    t[i++] = (struct kv){ "STLOOPHINT", speedtest_loop_hint_html() };
    t[i++] = (struct kv){ "STSTATUS", speedtest_status_html() };
    t[i++] = (struct kv){ "STRESULT", speedtest_result_html() };
    t[i++] = (struct kv){ "STLOGCARD", speedtest_log_card_html() };
    t[i++] = (struct kv){ "STACTION", speedtest_action_html() };
    t[i++] = (struct kv){ "STINSTALL", speedtest_install_html() };
    t[i++] = (struct kv){ "STGAUGE", speedtest_dial_html() };
    t[i++] = (struct kv){ "STCHART", speedtest_lines_html() };
    t[i++] = (struct kv){ "STHOMEBTN", speedtest_home_button_html() };
    t[i++] = (struct kv){ "STHOMEINLINE", speedtest_home_inline_html() };
    t[i++] = (struct kv){ "STFUNCTIONTILE", speedtest_function_tile_html() };
    t[i++] = (struct kv){ "SYSTEMFUNCTIONTILES", system_function_tiles_html() };
    t[i++] = (struct kv){ "CUSTOMFUNCTIONTILES", custom_function_tiles_html() };
    t[i++] = (struct kv){ "TIMEZONEFUNCTIONTILE", timezone_function_tile_html() };
    t[i++] = (struct kv){ "SIGADVSTATE", s_sigadvstate };
    t[i++] = (struct kv){ "MEMDETAIL", s_memdet }; t[i++] = (struct kv){ "UPSHORT", s_upshort };
    t[i++] = (struct kv){ "CHGV", s_chgv };       t[i++] = (struct kv){ "CHGI", s_chgi };
    t[i++] = (struct kv){ "BATV", s_batv };       t[i++] = (struct kv){ "BATI", s_bati };
    t[i++] = (struct kv){ "PWR", s_pwr };         t[i++] = (struct kv){ "PWRLBL", d.charger_connect ? "充电" : "放电" };
    if (is_charts) {
        t[i++] = (struct kv){ "BATPWR", s_batpwr };
        t[i++] = (struct kv){ "BATPWRLBL", s_batpwrlbl };
    } else {
        t[i++] = (struct kv){ "BATPWR", "" };
        t[i++] = (struct kv){ "BATPWRLBL", "" };
    }
    t[i++] = (struct kv){ "NETSEG", s_netseg };
    t[i++] = (struct kv){ "SIMSWITCH", s_simswitch };
    t[i++] = (struct kv){ "SIMREADY", g_sim_dual == 1 ? (g_sim_switching ? "操作中" : "已就绪") : g_sim_dual == 0 ? "单卡设备" : "读取中" };
    t[i++] = (struct kv){ "SIMREADYCLASS", g_sim_dual == 1 ? (g_sim_switching ? "warn" : "ok") : "muted" };
    t[i++] = (struct kv){ "SIMMODE", s_sim_mode };
    t[i++] = (struct kv){ "SIMCURRENT", s_sim_current };
    t[i++] = (struct kv){ "SIMAUTO", s_sim_auto };
    t[i++] = (struct kv){ "SIMSINGLECLASS", g_sim_switching ? "disabled" : sim_mode_armed && g_sim_mode_confirm == 1 ? "armed" : sim_mode_single ? "seg-on" : "" };
    t[i++] = (struct kv){ "SIMDUALCLASS", g_sim_switching ? "disabled" : sim_mode_armed && g_sim_mode_confirm == 2 ? "armed" : sim_mode_dual ? "seg-on" : "" };
    t[i++] = (struct kv){ "SIMSINGLELABEL", g_sim_switching && g_sim_operation == SIM_OPERATION_SINGLE ? "切换中" : sim_mode_armed && g_sim_mode_confirm == 1 ? "再次确认单卡" : "单卡模式" };
    t[i++] = (struct kv){ "SIMDUALLABEL", g_sim_switching && g_sim_operation == SIM_OPERATION_DUAL ? "切换中" : sim_mode_armed && g_sim_mode_confirm == 2 ? "再次确认双卡" : "双卡双待" };
    t[i++] = (struct kv){ "SIMAUTOONCLASS", !g_sim_switching && g_sim_auto == 1 ? "seg-on" : g_sim_switching ? "disabled" : "" };
    t[i++] = (struct kv){ "SIMAUTOOFFCLASS", !g_sim_switching && g_sim_auto == 0 ? "seg-on" : g_sim_switching ? "disabled" : "" };
    t[i++] = (struct kv){ "SIM1CLASS", g_sim_switching ? (g_sim_switch_target == 1 ? "armed" : "disabled") : sim_slot_armed && g_sim_confirm_slot == 1 ? "armed" : g_sim_slot == 1 ? "seg-on" : "" };
    t[i++] = (struct kv){ "SIM2CLASS", g_sim_switching ? (g_sim_switch_target == 2 ? "armed" : "disabled") : sim_slot_armed && g_sim_confirm_slot == 2 ? "armed" : g_sim_slot == 2 ? "seg-on" : "" };
    t[i++] = (struct kv){ "SIM1LABEL", g_sim_switching && g_sim_switch_target == 1 ? "切换中" : sim_slot_armed && g_sim_confirm_slot == 1 ? "再次确认自插卡" : "自插卡" };
    t[i++] = (struct kv){ "SIM2LABEL", g_sim_switching && g_sim_switch_target == 2 ? "切换中" : sim_slot_armed && g_sim_confirm_slot == 2 ? "再次确认内置卡" : "内置卡" };
    t[i++] = (struct kv){ "SIM1OP", s_sim1op };
    t[i++] = (struct kv){ "SIM2OP", s_sim2op };
    t[i++] = (struct kv){ "SIM1STATE", s_sim1state };
    t[i++] = (struct kv){ "SIM2STATE", s_sim2state };
    t[i++] = (struct kv){ "SIM1PROVISION", g_sim_p1 == 1 ? "已启用" : g_sim_p1 == 0 ? "未启用" : "未知" };
    t[i++] = (struct kv){ "SIM2PROVISION", g_sim_p2 == 1 ? "已启用" : g_sim_p2 == 0 ? "未启用" : "未知" };
    plugin_action_log_html(s_sim_action_log, sizeof s_sim_action_log, SIM_ACTION_LOG);
    t[i++] = (struct kv){ "SIMACTIONLOG", s_sim_action_log };
    t[i++] = (struct kv){ "SIMTRAFFICSTATE", d.sim_traffic_available ? "持久统计正常" : "等待后端" };
    t[i++] = (struct kv){ "SIMTRAFFICSTATECLASS", d.sim_traffic_available ? "ok" : "muted" };
    t[i++] = (struct kv){ "SIMTRAFFICCARDS", sim_traffic_cards_html(&d) };
    t[i++] = (struct kv){ "PLANSLOTSELECTOR", sim_traffic_plan_selector_html(&d) };
    t[i++] = (struct kv){ "PLANENABLECLASS", g_plan_draft_enabled ? "seg-on" : "" };
    t[i++] = (struct kv){ "PLANDISABLECLASS", g_plan_draft_enabled ? "" : "seg-on" };
    t[i++] = (struct kv){ "PLANGIB", s_plan_gib };
    t[i++] = (struct kv){ "PLANRESETDAY", s_plan_day };
    t[i++] = (struct kv){ "PLANTITLE", s_plan_title };
    t[i++] = (struct kv){ "PLANSTATE", s_plan_state };
    t[i++] = (struct kv){ "PLANAPPLYCLASS", plan_sim ? "" : "disabled" };
    t[i++] = (struct kv){ "FMSTATE", g_fm_switching ? "切换中" : g_fm_available ? "已识别" : "未检测到" };
    t[i++] = (struct kv){ "FMSTATECLASS", g_fm_switching ? "warn" : g_fm_available ? "ok" : "muted" };
    t[i++] = (struct kv){ "FMPROVIDER", s_fm_provider };
    t[i++] = (struct kv){ "FMNETTYPE", s_fm_nettype };
    t[i++] = (struct kv){ "FMBAND", s_fm_band };
    t[i++] = (struct kv){ "FMSIGNAL", s_fm_signal };
    t[i++] = (struct kv){ "FMPLMN", s_fm_plmn };
    t[i++] = (struct kv){ "FMYDCLASS", fm_button_class("0200", now_ms) };
    t[i++] = (struct kv){ "FMDXCLASS", fm_button_class("0300", now_ms) };
    t[i++] = (struct kv){ "FMLTCLASS", fm_button_class("0100", now_ms) };
    t[i++] = (struct kv){ "FMYDLABEL", fm_button_label("0200", now_ms) };
    t[i++] = (struct kv){ "FMDXLABEL", fm_button_label("0300", now_ms) };
    t[i++] = (struct kv){ "FMLTLABEL", fm_button_label("0100", now_ms) };
    plugin_action_log_html(s_fm_action_log, sizeof s_fm_action_log, FM_ACTION_LOG);
    t[i++] = (struct kv){ "FMACTIONLOG", s_fm_action_log };
    t[i++] = (struct kv){ "TZSTATE", d.timezone_available ? "设置已持久化" : "等待后端" };
    t[i++] = (struct kv){ "TZSTATECLASS", d.timezone_available ? "ok" : "muted" };
    t[i++] = (struct kv){ "TZCURRENT", s_tz_current };
    t[i++] = (struct kv){ "TZDRAFT", s_tz_draft };
    t[i++] = (struct kv){ "TZEFFECTIVE", s_tz_effective };
    t[i++] = (struct kv){ "TZHOURS", timezone_hours_html() };
    t[i++] = (struct kv){ "TZMINUTES", s_tz_minutes };
    t[i++] = (struct kv){ "TZDST", s_tz_dst };
    t[i++] = (struct kv){ "TOAST", s_toast };
    t[i++] = (struct kv){ "PWROFFLBL",  g_pwr_confirm == 1 ? "再按一次" : "关机" };
    t[i++] = (struct kv){ "PWROFFCLS",  g_pwr_confirm == 1 ? "armed" : "" };
    t[i++] = (struct kv){ "PWRREBLBL",  g_pwr_confirm == 2 ? "再按一次" : "重启" };
    t[i++] = (struct kv){ "PWRREBCLS",  g_pwr_confirm == 2 ? "armed" : "" };
    t[i++] = (struct kv){ "CURSA", s_cursa };     t[i++] = (struct kv){ "CURNSA", s_curnsa };
    t[i++] = (struct kv){ "CURLTE", s_curlte };
    t[i++] = (struct kv){ "NFCCLASS", d.nfc_switch ? "on" : "off" };
    t[i++] = (struct kv){ "NFCSTATE", d.nfc_switch ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "WIFICLASS", wifi_master ? "on" : "off" };
    t[i++] = (struct kv){ "WIFISTATE", (g_w24 < 0 && g_w5 < 0) ? "—" : wifi_master ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "WIFI24CLASS", g_w24 == 1 ? "on" : "off" };
    t[i++] = (struct kv){ "WIFI24STATE", g_w24 < 0 ? "—" : g_w24 ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "WIFI5CLASS", g_w5 == 1 ? "on" : "off" };
    t[i++] = (struct kv){ "WIFI5STATE", g_w5 < 0 ? "—" : g_w5 ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "DPSCLASS", g_dps == 1 ? "on" : "off" };
    t[i++] = (struct kv){ "DPSSTATE", g_dps < 0 ? "—" : g_dps ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "PSMCLASS", g_wpsm == 1 ? "on" : "off" };
    t[i++] = (struct kv){ "PSMSTATE", g_wpsm == -2 ? "状态不一致" : g_wpsm < 0 ? "—" : g_wpsm ? "已开启（省电）" : "已关闭（高性能）" };
    t[i++] = (struct kv){ "CLIENTLIST", s_clist }; t[i++] = (struct kv){ "DHCP_IP", d.dhcp_ip[0] ? d.dhcp_ip : "-" };
    t[i++] = (struct kv){ "SMSLIST", s_smslist };
    t[i++] = (struct kv){ "DHCP_POOL", g_dhcp_pool[0] ? g_dhcp_pool : s_pool };
    t[i++] = (struct kv){ "DHCP_LEASE", s_lease };

    /* ---- lock screen (PIN pad): dots reflect typed digits. ---- */
    static char s_pindots[512], s_lockaux[140];
    {
        int filled = (int)strlen(g_pin_entry);
        int o = snprintf(s_pindots, sizeof s_pindots, "<div class='pin-slots'>");
        for (int k = 0; k < 4; k++)
            o += (k < filled)
               ? snprintf(s_pindots + o, sizeof s_pindots - o, "<span class='ps'><span class='dot'></span></span>")
               : snprintf(s_pindots + o, sizeof s_pindots - o,
                          "<span class='ps'><span class='ring'><span class='ri'></span></span></span>");
        snprintf(s_pindots + o, sizeof s_pindots - o, "</div>");
    }
    if (g_lock_state == 2)
        snprintf(s_lockaux, sizeof s_lockaux, "<a href='act:lockcancel' class='kp kpx'>取消</a>");
    else s_lockaux[0] = 0;
    t[i++] = (struct kv){ "PINDOTS", s_pindots };
    t[i++] = (struct kv){ "LOCKAUX", s_lockaux };
    t[i++] = (struct kv){ "LOCKTITLE", g_lock_state == 2 ? "设置锁屏密码" : "请输入锁屏密码" };
    t[i++] = (struct kv){ "LOCKMSG", g_lock_err ? "<div class='lock-err'>密码错误</div>" : "" };
    t[i++] = (struct kv){ "LOCKCLASS", lock_enabled() ? "on" : "off" };
    t[i++] = (struct kv){ "LOCKSTATE", lock_enabled() ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "TSSTATE", !g_ts_installed ? "未安装" : g_ts_connected ? "已连接" : g_ts_running ? "等待连接" : "已停止" };
    t[i++] = (struct kv){ "TSSTATECLASS", g_ts_connected ? "ok" : "muted" };
    t[i++] = (struct kv){ "TSRUNCLASS", g_ts_running ? "seg-on" : "" };
    t[i++] = (struct kv){ "TSSTOPCLASS", g_ts_installed && !g_ts_running ? "seg-on" : "" };
    t[i++] = (struct kv){ "TSIP", g_ts_ip };
    t[i++] = (struct kv){ "TSPID", g_ts_pid };
    t[i++] = (struct kv){ "TSVERSION", g_ts_version };
    t[i++] = (struct kv){ "TSHOST", g_ts_host };
    t[i++] = (struct kv){ "TSROUTES", g_ts_routes };
    t[i++] = (struct kv){ "TSBOOT", g_ts_boot ? "已开启" : "已关闭" };
    plugin_action_log_html(s_ts_action_log, sizeof s_ts_action_log, TAILSCALE_ACTION_LOG);
    t[i++] = (struct kv){ "TSACTIONLOG", s_ts_action_log };
    t[i++] = (struct kv){ "MHSTATE", !g_mh_installed ? "未安装" : g_mh_running && g_mh_tun && g_mh_rules ? "代理运行中" : g_mh_running ? "进程运行，规则未就绪" : "已停止" };
    t[i++] = (struct kv){ "MHSTATECLASS", g_mh_running && g_mh_tun && g_mh_rules ? "ok" : "muted" };
    t[i++] = (struct kv){ "MHRUNCLASS", g_mh_running ? "seg-on" : "" };
    t[i++] = (struct kv){ "MHSTOPCLASS", g_mh_installed && !g_mh_running ? "seg-on" : "" };
    t[i++] = (struct kv){ "MHPID", g_mh_pid };
    t[i++] = (struct kv){ "MHVERSION", g_mh_version };
    t[i++] = (struct kv){ "MHMODE", g_mh_mode };
    t[i++] = (struct kv){ "MHPORT", g_mh_port };
    t[i++] = (struct kv){ "MHTUN", g_mh_tun ? "已就绪" : "未就绪" };
    t[i++] = (struct kv){ "MHRULES", g_mh_rules ? "已接管" : "未接管" };
    t[i++] = (struct kv){ "MHIPSET", g_mh_ipset };
    plugin_action_log_html(s_mh_action_log, sizeof s_mh_action_log, MIHOMO_ACTION_LOG);
    t[i++] = (struct kv){ "MHACTIONLOG", s_mh_action_log };
    t[i++] = (struct kv){ "CPUSTATE", !g_cpu_installed ? "控制器未安装" : !strcmp(g_cpu_mode, "powersave") ? "省电模式" : !strcmp(g_cpu_mode, "balance") ? "均衡模式" : !strcmp(g_cpu_mode, "performance") ? "性能模式" : !strcmp(g_cpu_mode, "extreme") ? "极致模式" : "自定义状态" };
    t[i++] = (struct kv){ "CPUSTATECLASS", g_cpu_installed ? "ok" : "muted" };
    t[i++] = (struct kv){ "CPUPOWERSAVECLASS", !strcmp(g_cpu_mode, "powersave") ? "seg-on" : "" };
    t[i++] = (struct kv){ "CPUBALANCECLASS", !strcmp(g_cpu_mode, "balance") ? "seg-on" : "" };
    t[i++] = (struct kv){ "CPUPERFORMANCECLASS", !strcmp(g_cpu_mode, "performance") ? "seg-on" : "" };
    t[i++] = (struct kv){ "CPUEXTREMECLASS", !strcmp(g_cpu_mode, "extreme") ? "seg-on" : "" };
    t[i++] = (struct kv){ "CPUGOV", g_cpu_gov };
    t[i++] = (struct kv){ "CPUCUR", g_cpu_cur };
    t[i++] = (struct kv){ "CPUMIN", g_cpu_min };
    t[i++] = (struct kv){ "CPUMAX", g_cpu_max };
    plugin_action_log_html(s_cpu_action_log, sizeof s_cpu_action_log, CPU_ACTION_LOG);
    t[i++] = (struct kv){ "CPUACTIONLOG", s_cpu_action_log };
    snprintf(s_wg_peers, sizeof s_wg_peers, "%d", g_wg_peer_total);
    snprintf(s_wg_active, sizeof s_wg_active, "%d", g_wg_peer_active);
    html_esc(s_wg_iface, sizeof s_wg_iface, g_wg_iface);
    html_esc(s_wg_address, sizeof s_wg_address, g_wg_address);
    html_esc(s_wg_port, sizeof s_wg_port, g_wg_port);
    html_esc(s_wg_mode, sizeof s_wg_mode, g_wg_mode);
    t[i++] = (struct kv){ "WGSTATE", !g_wg_installed ? "未安装" : g_wg_running ? "运行中" : g_wg_deps ? "已停止" : "依赖未就绪" };
    t[i++] = (struct kv){ "WGSTATECLASS", g_wg_running ? "ok" : g_wg_deps ? "muted" : "warn" };
    t[i++] = (struct kv){ "WGRUNCLASS", g_wg_running ? "seg-on" : "" };
    t[i++] = (struct kv){ "WGSTOPCLASS", g_wg_installed && !g_wg_running ? "seg-on" : "" };
    t[i++] = (struct kv){ "WGIFACE", s_wg_iface };
    t[i++] = (struct kv){ "WGADDRESS", s_wg_address };
    t[i++] = (struct kv){ "WGPORT", s_wg_port };
    t[i++] = (struct kv){ "WGMODE", s_wg_mode };
    t[i++] = (struct kv){ "WGUPTIME", g_wg_uptime };
    t[i++] = (struct kv){ "WGBOOT", g_wg_boot ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "WGPEERS", s_wg_peers };
    t[i++] = (struct kv){ "WGACTIVE", s_wg_active };
    t[i++] = (struct kv){ "WGPEERLIST", wireguard_peer_html() };
    plugin_action_log_html(s_wg_action_log, sizeof s_wg_action_log, WIREGUARD_ACTION_LOG);
    t[i++] = (struct kv){ "WGACTIONLOG", s_wg_action_log };
    snprintf(s_op_selected, sizeof s_op_selected, "%s", g_op_selected[0] ? g_op_selected : "未选择");
    snprintf(s_op_job_raw, sizeof s_op_job_raw, "%s%s%s",
             g_op_job_status, strcmp(g_op_job_message, "-") ? " · " : "",
             strcmp(g_op_job_message, "-") ? g_op_job_message : "");
    html_esc(s_op_job, sizeof s_op_job, s_op_job_raw);
    html_esc(s_op_sim, sizeof s_op_sim, g_op_sim);
    html_esc(s_op_operator, sizeof s_op_operator, g_op_operator);
    html_esc(s_op_rat, sizeof s_op_rat, g_op_rat);
    html_esc(s_op_mode, sizeof s_op_mode,
             !strcmp(g_op_mode, "1") ? "手动选网" : !strcmp(g_op_mode, "0") ? "自动选网" : g_op_mode);
    html_esc(s_op_rat_pref, sizeof s_op_rat_pref, g_op_rat_pref);
    html_esc(s_op_policy, sizeof s_op_policy, g_op_failure_policy);
    t[i++] = (struct kv){ "OPSTATE", !g_op_installed ? "未安装" : g_op_job_running ? "任务运行中" : g_op_registered ? "已注册" : "未注册" };
    t[i++] = (struct kv){ "OPSTATECLASS", g_op_registered ? "ok" : g_op_job_running ? "warn" : "muted" };
    t[i++] = (struct kv){ "OPSIM", s_op_sim };
    t[i++] = (struct kv){ "OPOPERATOR", s_op_operator };
    t[i++] = (struct kv){ "OPRAT", s_op_rat };
    t[i++] = (struct kv){ "OPMODE", s_op_mode };
    t[i++] = (struct kv){ "OPJOB", s_op_job };
    t[i++] = (struct kv){ "OPSELECTED", s_op_selected };
    t[i++] = (struct kv){ "OPRATPREF", s_op_rat_pref };
    t[i++] = (struct kv){ "OPPOLICY", s_op_policy };
    t[i++] = (struct kv){ "OPLIST", operator_list_html() };
    t[i++] = (struct kv){ "OPLOCKLABEL", g_op_confirm_until && (int32_t)(g_op_confirm_until - millis()) > 0 ? "再次确认锁定" : "锁定选中" };
    t[i++] = (struct kv){ "OPLOCKCLASS", g_op_confirm_until && (int32_t)(g_op_confirm_until - millis()) > 0 ? "armed" : "" };
    t[i++] = (struct kv){ "OPCANCELCLASS", g_op_job_running ? "" : "disabled" };
    plugin_action_log_html(s_op_action_log, sizeof s_op_action_log, OPERATOR_ACTION_LOG);
    t[i++] = (struct kv){ "OPACTIONLOG", s_op_action_log };
    return i;
}

static void refresh_status_cache(void)
{
    struct kv t[KV_MAX];
    (void)build_kv(t, NULL);
}

static const char *kv_get(struct kv *t, int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(t[i].k, key)) return t[i].v ? t[i].v : "";
    return "";
}

static const char *custom_page_html(const char *path)
{
    static char out[PAGE_HTML_CACHE_CAP];
    const char *tmpl = read_template_cached(path);
    const char *start, *end, *body, *gt;
    char *body_src;
    size_t len;
    struct kv t[KV_MAX];
    int n;
    char raw_title[96], title[160];
    const char *base;
    char *filled;
    int o;

    if (!tmpl) return NULL;
    start = tmpl;
    end = tmpl + strlen(tmpl);
    body = strcasestr(tmpl, "<body");
    if (body) {
        gt = strchr(body, '>');
        if (gt) {
            start = gt + 1;
            end = strcasestr(start, "</body>");
            if (!end) end = tmpl + strlen(tmpl);
        }
    }
    len = (size_t)(end - start);
    body_src = malloc(len + 1);
    if (!body_src) return NULL;
    memcpy(body_src, start, len);
    body_src[len] = 0;

    n = build_kv(t, path);
    filled = apply_template(body_src, t, n);
    free(body_src);

    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!function_title_from_html(tmpl, raw_title, sizeof raw_title))
        function_label_from_name(raw_title, sizeof raw_title, base);
    html_esc(title, sizeof title, raw_title);

    o = snprintf(out, sizeof out,
                 "<!DOCTYPE html><html lang=\"zh-CN\">"
                 "<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"style.css\"></head>"
                 "<body class=\"%s\">"
                 "%s"
                 "<div class=\"subtop\"><a href=\"act:backfunc\" class=\"subback\">返回</a>"
                 "<span class=\"subname\">%s</span></div>"
                 "<div class=\"custom-page\">",
                 g_theme ? "light" : "dark", kv_get(t, n, "STATUSBAR"), title);
    if (o < 0) o = 0;
    if (o < (int)sizeof(out) - 1) {
        size_t room = sizeof(out) - (size_t)o - 1;
        size_t fl = strlen(filled);
        if (fl > room) fl = room;
        memcpy(out + o, filled, fl);
        o += (int)fl;
        out[o] = 0;
    }
    if (o < (int)sizeof(out) - 1)
        snprintf(out + o, sizeof(out) - (size_t)o,
                 "</div>%s<div class=\"pgpad\"></div></body></html>",
                 kv_get(t, n, "TOAST"));
    return out;
}

/* Build the data-filled HTML for a page (returns a static buffer). */
static const char *page_html(const char *path)
{
    if (path_is_function_page(path)) return custom_page_html(path);
    const char *tmpl = read_template_cached(path);   /* cache templates with mtime invalidation */
    if (!tmpl) return NULL;
    struct kv t[KV_MAX];
    int n = build_kv(t, path);
    char *html = apply_template(tmpl, t, n);
    return html;
}

static void invalidate_render_html_cache(void)
{
    free(g_render_html_cache);
    g_render_html_cache = NULL;
    g_render_html_cache_cap = 0;
    g_render_html_cache_len = 0;
    g_render_html_cache_path[0] = 0;
    g_render_html_cache_css_mtime = -1;
}

/* ---- power menu: three large round buttons with native-drawn glyphs
 * (litehtml can't render a power symbol / circular arrow / X reliably). The
 * HTML provides the circle placeholders (#pmc-*) + labels + tap targets; we
 * fill the raised disc and draw the glyph centered over each. ---- */
#define PM_PI 3.14159265358979

/* Plain native disc remains shared by non-menu widgets such as the speed-test
 * gauge. Power-menu controls use html_view_fill_bevel_circle instead. */
static void pm_disc(int cx, int cy, int rad, int r, int g, int b, int a)
{
    html_view_fill_round_rect(cx - rad, cy - rad, rad * 2, rad * 2,
                              rad, r, g, b, a);
}

/* thick line segment (a-b) as a filled quad */
static void pm_line(double x1, double y1, double x2, double y2, double th, int r, int g, int b)
{
    double dx = x2 - x1, dy = y2 - y1, len = sqrt(dx * dx + dy * dy);
    if (len < 0.001) return;
    double nx = -dy / len * th / 2.0, ny = dx / len * th / 2.0;
    int xs[4] = { (int)(x1 + nx + 0.5), (int)(x2 + nx + 0.5), (int)(x2 - nx + 0.5), (int)(x1 - nx + 0.5) };
    int ys[4] = { (int)(y1 + ny + 0.5), (int)(y2 + ny + 0.5), (int)(y2 - ny + 0.5), (int)(y1 - ny + 0.5) };
    html_view_fill_poly(xs, ys, 4, r, g, b, 255);
}

/* annular sector (ring band) from a0..a1 degrees (math angles; screen y down) */
static void pm_arc(int cx, int cy, double ro, double ri, int a0, int a1, int r, int g, int b)
{
    int xs[160], ys[160], N = 0;
    for (int a = a0; a <= a1 && N < 158; a += 8) {
        double t = a * PM_PI / 180.0;
        xs[N] = cx + (int)(ro * cos(t) + 0.5); ys[N] = cy - (int)(ro * sin(t) + 0.5); N++;
    }
    for (int a = a1; a >= a0 && N < 160; a -= 8) {
        double t = a * PM_PI / 180.0;
        xs[N] = cx + (int)(ri * cos(t) + 0.5); ys[N] = cy - (int)(ri * sin(t) + 0.5); N++;
    }
    html_view_fill_poly(xs, ys, N, r, g, b, 255);
}

/* kind: 0=power, 1=reboot, 2=cancel */
static void pm_glyph(const char *sel, int kind, int cr, int cg, int cb)
{
    int x, y, w, h;
    if (!html_view_rect(sel, &x, &y, &w, &h)) return;
    int rad = (w < h ? w : h) / 2;
    int cx = x + w / 2, cy = y + h / 2;
    int armed = (kind == 0 && g_pwr_confirm == 1) || (kind == 1 && g_pwr_confirm == 2);
    html_view_fill_bevel_circle(cx, cy, rad, cr, cg, cb, g_theme, armed);

    /* Both passes use subpixel coverage. Keep the colored shadow restrained so
     * it adds depth without turning the white edge into a dark staircase. */
    int icon_y = cy + (armed ? 1 : 0);
    html_view_draw_power_glyph(cx, icon_y + 1, rad, kind,
                               cr / 2, cg / 2, cb / 2, 96);
    html_view_draw_power_glyph(cx, icon_y, rad, kind,
                               0xf4, 0xf8, 0xfa, 255);
}

static void draw_power_menu(void)
{
    pm_glyph("#pmc-off",    0, 0xd4, 0x5c, 0x55);   /* red */
    pm_glyph("#pmc-reboot", 1, 0xd5, 0xa6, 0x3d);   /* amber */
    pm_glyph("#pmc-cancel", 2, 0x3a, 0x48, 0x59);   /* cool gray */
}

static int st_hist_max(const int *hist, int n)
{
    int mx = 10;
    for (int i = 0; i < n; i++) if (hist[i] > mx) mx = hist[i];
    return mx;
}

static void st_chart_grid(int x, int y, int w, int h)
{
    const int dark = !g_theme;
    const int r = dark ? 0x26 : 0xc8;
    const int g = dark ? 0x34 : 0xd4;
    const int b = dark ? 0x45 : 0xe2;
    const int a = dark ? 150 : 180;

    html_view_fill_rect(x + 1, y + h / 3, w - 2, 1, r, g, b, a);
    html_view_fill_rect(x + 1, y + h * 2 / 3, w - 2, 1, r, g, b, a);
}

static void st_draw_one_chart(const char *sel, const int *hist, int n, int r, int g, int b)
{
    int x, y, w, h;
    if (!html_view_rect(sel, &x, &y, &w, &h)) return;
    st_chart_grid(x, y, w, h);
    if (n >= 2) {
        html_view_polyline(x + 3, y + 3, w - 6, h - 6, hist, n, 0, st_hist_max(hist, n), r, g, b, 2, 24);
    } else if (n == 1) {
        int cy = y + h - 6 - (hist[0] * (h - 12)) / st_hist_max(hist, n);
        html_view_fill_round_rect(x + w - 10, cy - 3, 6, 6, 3, r, g, b, 255);
    }
}

static void draw_speedtest_widgets(void)
{
    int x, y, w, h;
    html_view_set_clip_top(26);
    if (html_view_rect("#st-gauge-dial", &x, &y, &w, &h)) {
        const int dark = !g_theme;
        double cur = speedtest_display_mbps();
        double scale = speedtest_display_peak_mbps();
        int pct = speedtest_pct(cur, scale);
        int rad = (w < h ? w : h) / 2 - 3;
        int cx = x + w / 2, cy = y + h / 2;
        double ang = (210.0 - 240.0 * pct / 100.0) * PM_PI / 180.0;
        double vx = cos(ang), vy = -sin(ang);
        double len = rad * 0.58, back = rad * 0.14, half = rad * 0.055;
        double tipx = cx + vx * len, tipy = cy + vy * len;
        double basex = cx - vx * back, basey = cy - vy * back;
        double px = -vy, py = vx;
        int nx[3] = {
            (int)(tipx + 0.5),
            (int)(basex + px * half + 0.5),
            (int)(basex - px * half + 0.5)
        };
        int ny[3] = {
            (int)(tipy + 0.5),
            (int)(basey + py * half + 0.5),
            (int)(basey - py * half + 0.5)
        };
        char num[32];
        const int outer_r = dark ? 0x0f : 0xe3;
        const int outer_g = dark ? 0x17 : 0xeb;
        const int outer_b = dark ? 0x20 : 0xf4;
        const int inner_r = dark ? 0x15 : 0xfb;
        const int inner_g = dark ? 0x1d : 0xfd;
        const int inner_b = dark ? 0x28 : 0xff;
        const int track_r = dark ? 0x2d : 0xbd;
        const int track_g = dark ? 0x3a : 0xc9;
        const int track_b = dark ? 0x4a : 0xd6;
        const int tick_r = dark ? 0xb8 : 0x52;
        const int tick_g = dark ? 0xc7 : 0x62;
        const int tick_b = dark ? 0xd7 : 0x77;
        const int num_r = dark ? 0xee : 0x17;
        const int num_g = dark ? 0xf4 : 0x22;
        const int num_b = dark ? 0xfb : 0x32;
        const int unit_r = dark ? 0x71 : 0x65;
        const int unit_g = dark ? 0x81 : 0x75;
        const int unit_b = dark ? 0x96 : 0x8a;
        const int hub_outer_r = dark ? 0xee : 0x17;
        const int hub_outer_g = dark ? 0xf4 : 0x22;
        const int hub_outer_b = dark ? 0xfb : 0x32;
        const int hub_inner_r = dark ? 0x15 : 0xfb;
        const int hub_inner_g = dark ? 0x1d : 0xfd;
        const int hub_inner_b = dark ? 0x28 : 0xff;

        pm_disc(cx, cy, rad, outer_r, outer_g, outer_b, 255);
        pm_disc(cx, cy, rad - 5, inner_r, inner_g, inner_b, 255);
        pm_arc(cx, cy, rad - 12, rad - 20, -30, 210, track_r, track_g, track_b);
        if (pct > 0)
            pm_arc(cx, cy, rad - 12, rad - 20, (int)(210.0 - 240.0 * pct / 100.0), 210, 0x4f, 0x8f, 0xe8);
        for (int i = 0; i <= 10; i++) {
            double a = (210.0 - 240.0 * i / 10.0) * PM_PI / 180.0;
            double tx = cos(a), ty = -sin(a);
            pm_line(cx + tx * (rad - 30), cy + ty * (rad - 30),
                    cx + tx * (rad - 22), cy + ty * (rad - 22),
                    i % 5 == 0 ? 3.0 : 2.0, tick_r, tick_g, tick_b);
        }
        html_view_fill_poly(nx, ny, 3, 0xd5, 0xa6, 0x3d, 255);
        pm_disc(cx, cy, 8, hub_outer_r, hub_outer_g, hub_outer_b, 255);
        pm_disc(cx, cy, 4, hub_inner_r, hub_inner_g, hub_inner_b, 255);
        snprintf(num, sizeof num, "%.1f", cur);
        draw_center_text_px(cx, cy + 38, num, 28, 1, num_r, num_g, num_b, 255);
        draw_center_text_px(cx, cy + 62, "Mbps", 12, 0, unit_r, unit_g, unit_b, 255);
    }
    st_draw_one_chart("#st-chart-dl", g_st_dl_hist, g_st_dl_n, 0x4f, 0x8f, 0xe8);
    st_draw_one_chart("#st-chart-ul", g_st_ul_hist, g_st_ul_n, 0xd5, 0xa6, 0x3d);
    html_view_set_clip_top(0);
}

static void capture_fb(drm_disp_t *d);   /* fwd: fb snapshot for modal overlay */
static void render_sms_overlay(drm_disp_t *d, int restore_background);

static void maybe_dump_fb(drm_disp_t *d)
{
    if (access("/tmp/u60-dumpfb", F_OK) != 0) return;
    FILE *f = fopen("/tmp/fb.dump", "wb");
    if (!f) return;
    const int W = d->width, H = d->height, pp = d->pitch_px;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t px = d->fb[(size_t)(H - 1 - y) * pp + (W - 1 - x)];
            fwrite(&px, sizeof px, 1, f);
        }
    }
    fclose(f);
}

/* litehtml scrolls the whole document. Preserve the secondary-page navigation
 * strip in framebuffer coordinates so it stays visible during render and drag. */
static void pinned_header_capture(drm_disp_t *d)
{
    if (d->width != 320 || d->height < PINNED_HEADER_TOP + PINNED_HEADER_H) return;
    for (int i = 0; i < PINNED_HEADER_H; i++) {
        int y = PINNED_HEADER_TOP + i;
        memcpy(g_pinned_header + (size_t)i * 320,
               d->fb + (size_t)(d->height - 1 - y) * d->pitch_px,
               320 * sizeof(uint16_t));
    }
    g_pinned_header_valid = 1;
}

static void pinned_header_draw(drm_disp_t *d)
{
    if (!g_pinned_header_valid || d->width != 320 ||
        d->height < PINNED_HEADER_TOP + PINNED_HEADER_H) return;
    for (int i = 0; i < PINNED_HEADER_H; i++) {
        int y = PINNED_HEADER_TOP + i;
        memcpy(d->fb + (size_t)(d->height - 1 - y) * d->pitch_px,
               g_pinned_header + (size_t)i * 320,
               320 * sizeof(uint16_t));
    }
}

/* The quick-shade affordance belongs to the moving bottom edge, rather than
 * the fixed top header. Coordinates here are logical screen coordinates; the
 * DRM buffer itself is rotated 180 degrees. */
static void draw_quick_grabber(drm_disp_t *d, int top)
{
    const int w = 48, h = 4;
    if (d->width < w || top < 27 || top + h > d->height) return;
    const int r = g_theme ? 0x48 : 0x8d;
    const int g = g_theme ? 0x89 : 0xc4;
    const int b = g_theme ? 0x96 : 0xce;
    const uint16_t color = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    const int left = (d->width - w) / 2;
    for (int yy = 0; yy < h; yy++) {
        const int inset = (yy == 0 || yy == h - 1) ? 2 : 0;
        uint16_t *row = d->fb + (size_t)(d->height - 1 - (top + yy)) * d->pitch_px;
        for (int xx = inset; xx < w - inset; xx++)
            row[d->width - 1 - (left + xx)] = color;
    }
}

static void render(drm_disp_t *disp, const char *path)
{
    const char *html = page_html(path);
    if (!html) return;
    int special_page = strstr(path, "menu.html") || strstr(path, "lockscreen.html");
    int scroll = special_page ? 0 : g_scroll;
    size_t html_len = strlen(html);
    int is_speedtest = path_is_speedtest(path);
    int has_charts = strstr(path, "charts.html") != NULL || is_speedtest;
    long long css_mtime = file_mtime_ns(UI_DIR "/style.css");
    if (is_speedtest) css_mtime ^= file_mtime_ns(UI_DIR "/speedtest.css");
    int cacheable = html_len > 0 && html_len < PAGE_HTML_CACHE_CAP;
    int reuse = cacheable && !has_charts &&
                 g_render_html_cache_scroll == scroll &&
                 g_render_html_cache_modal == g_modal &&
                 g_render_html_cache_sms_open == g_sms_open &&
                 g_render_html_cache_lock == g_lock_state &&
                 g_render_html_cache_css_mtime == css_mtime &&
                 g_render_html_cache_len == html_len &&
                 !strcmp(g_render_html_cache_path, path) &&
                 g_render_html_cache && !memcmp(g_render_html_cache, html, html_len);
    html_view_set_scroll(scroll);
    if (!reuse) {
        g_page_h = html_view_render_html(html);
        if (cacheable) {
            if (g_render_html_cache_cap != html_len + 1) {
                char *next = realloc(g_render_html_cache, html_len + 1);
                if (next) {
                    g_render_html_cache = next;
                    g_render_html_cache_cap = html_len + 1;
                }
            }
            if (g_render_html_cache && g_render_html_cache_cap >= html_len + 1) {
                memcpy(g_render_html_cache, html, html_len + 1);
                g_render_html_cache_len = html_len;
                snprintf(g_render_html_cache_path, sizeof g_render_html_cache_path, "%s", path);
                g_render_html_cache_scroll = scroll;
                g_render_html_cache_modal = g_modal;
                g_render_html_cache_sms_open = g_sms_open;
                g_render_html_cache_lock = g_lock_state;
                g_render_html_cache_css_mtime = css_mtime;
            } else {
                invalidate_render_html_cache();
            }
        } else {
            invalidate_render_html_cache();
        }
    }
    draw_charts();      /* native polylines into any #chart-* placeholders */
    if (!g_lock_state && (is_speedtest || g_st_home_open)) draw_speedtest_widgets();
    draw_native_statusbar();
    draw_sms_icon();    /* native envelope in the status bar when unread SMS */
    if (g_subpage[0]) {
        if (scroll == 0) pinned_header_capture(disp);
        else pinned_header_draw(disp);
    }
    if (strstr(path, "menu.html")) draw_power_menu();   /* round power buttons */
    if (g_lock_state == 1) draw_lock_icon();   /* locked preview: lock glyph */
    if (g_quick_open) draw_quick_grabber(disp, disp->height - 9);
    int H = disp->height, maxs = g_page_h - H;
    (void)maxs;
    if (g_modal) {                                    /* dim page + second-level dialog */
        html_view_fill_rect(0, 28, disp->width, H - 28, 0, 0, 0, 150);
        capture_fb(disp);                             /* snapshot page+dim for fast modal refresh */
        html_view_render_overlay(modal_html());
    } else if (g_sms_open >= 0) {                     /* dim page + SMS detail dialog */
        html_view_fill_rect(0, 28, disp->width, H - 28, 0, 0, 0, 150);
        capture_fb(disp);
        render_sms_overlay(disp, 0);
    }
    drm_disp_dirty(disp, 0, 0, disp->width - 1, disp->height - 1);
    maybe_dump_fb(disp);
}

static void render_ext_view(drm_disp_t *disp, devui_ext_t *ext)
{
    devui_ext_render(ext, disp);
    draw_native_statusbar();
    draw_sms_icon();
    drm_disp_dirty(disp, 0, 0, disp->width - 1, disp->height - 1);
    maybe_dump_fb(disp);
}

/* Offscreen page bitmaps (logical, no rotation) for slide transitions.
 * During a drag: g_bufA = left page, g_bufB = right page (windowed by offset o:
 * window column x shows [left|right][x+o], o in 0..W). */
static uint16_t *g_bufA, *g_bufB;

/* Status bar stays pinned during a swipe (only the content below slides), so
 * we keep the already-rendered native status bar rows untouched and only
 * update the content area beneath it. */
#define SBAR_H DEVUI_EXT_STATUSBAR_H
/* Keep only a tiny hysteresis so page/scroll drags respond immediately,
 * without turning every resting-finger jitter into a swipe. */
#define DRAG_START_PX 2
#define DRAG_CANCEL_PX 8
#define SCROLL_INERTIA_MIN_V 180.0f
#define SCROLL_INERTIA_MAX_V 2200.0f
#define SCROLL_INERTIA_DECEL 2800.0f
#define STATE_RENDER_THROTTLE_MS 1500
#define STATE_RENDER_IDLE_THROTTLE_MS 2500
#define EXT_BACK_EDGE_PX 24
#define EXT_BACK_COMMIT_PX 56
#define EXT_BACK_MAX_DY 44
#define TOUCH_FRAME_MIN_MS 22  /* interaction cap: about 45 FPS */
#define QUEUED_SWIPE_PX 52
#define QUEUED_SCROLL_PX 28
#define IDLE_SLEEP_ON_US 8000
#define IDLE_SLEEP_OFF_US 30000
#define GESTURE_CACHE_TTL_MS 5000
#define INTERACTION_BOOST_MS 1500

static uint32_t g_gesture_used_at;
static uint32_t g_boost_until;
static int g_boosted;

struct devui_sched_attr {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime, sched_deadline, sched_period;
    uint32_t sched_util_min, sched_util_max;
};

static void set_interaction_boost(int enabled)
{
    const unsigned long SCHED_FLAG_UTIL_CLAMP_MIN_LOCAL = 0x20;
    struct devui_sched_attr a;
    if (g_boosted == enabled) return;
    (void)setpriority(PRIO_PROCESS, 0, enabled ? -5 : 0);
#ifdef SYS_sched_setattr
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.sched_flags = SCHED_FLAG_UTIL_CLAMP_MIN_LOCAL;
    a.sched_nice = enabled ? -5 : 0;
    a.sched_util_min = enabled ? 256 : 0;
    (void)syscall(SYS_sched_setattr, 0, &a, 0);
#endif
    g_boosted = enabled;
}

static void interaction_pulse(uint32_t now)
{
    g_boost_until = now + INTERACTION_BOOST_MS;
    set_interaction_boost(1);
}

static void *gesture_alloc(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static void gesture_free(void **p, size_t bytes)
{
    if (*p) munmap(*p, bytes);
    *p = NULL;
}

static int ensure_pair_buffers(void)
{
    const size_t bytes = 320u * 480u * sizeof(uint16_t);
    if (!g_bufA) g_bufA = gesture_alloc(bytes);
    if (!g_bufB) g_bufB = gesture_alloc(bytes);
    return g_bufA && g_bufB;
}

static void capture_fb_logical(drm_disp_t *d, uint16_t *dst)
{
    for (int y = 0; y < d->height; y++)
        for (int x = 0; x < d->width; x++)
            dst[(size_t)y * d->width + x] =
                d->fb[(size_t)(d->height - 1 - y) * d->pitch_px + (d->width - 1 - x)];
}

static int motion_frame_due(uint32_t now, uint32_t *last)
{
    if (*last == 0 || now - *last >= TOUCH_FRAME_MIN_MS) {
        *last = now;
        return 1;
    }
    return 0;
}

static inline uint16_t *copy_rev_span(uint16_t *dp, const uint16_t *src, int n)
{
    for (int i = 0; i < n; i++) *dp-- = src[i];
    return dp;
}

static inline void fill_rev_span(uint16_t *dp, uint16_t px, int n)
{
    while (n-- > 0) *dp-- = px;
}

static float clamp_scroll_v(float v)
{
    if (v >  SCROLL_INERTIA_MAX_V) return  SCROLL_INERTIA_MAX_V;
    if (v < -SCROLL_INERTIA_MAX_V) return -SCROLL_INERTIA_MAX_V;
    return v;
}

static void compose_frame(drm_disp_t *d, int o)
{
    const int W = d->width, H = d->height, pp = d->pitch_px;
    int left;
    if (o < 0) o = 0; if (o > W) o = W;
    left = W - o;
    for (int y = SBAR_H; y < H; y++) {
        uint16_t *dp = d->fb + (size_t)(H - 1 - y) * pp + (W - 1);
        const uint16_t *lr = g_bufA + (size_t)y * W, *rr = g_bufB + (size_t)y * W;
        if (left > 0) dp = copy_rev_span(dp, lr + o, left);
        if (o > 0)    copy_rev_span(dp, rr, o);
    }
    /* Only the content area changes during a horizontal swipe. With the
     * framebuffer rotated 180 degrees that maps to the top physical rows. */
    drm_disp_dirty(d, 0, 0, W - 1, H - SBAR_H - 1);
    maybe_dump_fb(d);
}

/* Render the page pair for a drag direction into A(left)/B(right). dir>0 = next.
 * The current page keeps its scroll; the target page comes in at the top. */
static void render_page_to_pair_buf(uint16_t *buf, const char *path, const char *html, int scroll)
{
    if (!html) return;
    html_view_render_to_scroll(buf, html, scroll);
    html_view_target_begin(buf, scroll);
    draw_charts();
    if (!g_lock_state && (path_is_speedtest(path) || g_st_home_open)) draw_speedtest_widgets();
    draw_native_statusbar();
    draw_sms_icon();
    html_view_target_end();
}

/* The quick shade follows the finger and settles as a top-down reveal.  Its
 * pixels are rendered offscreen so opening never flashes the final page first. */
static int quick_pair_prepare(drm_disp_t *d, int opening)
{
    const char *path;
    const char *html;
    int scroll = 0;
    if (!ensure_pair_buffers()) return 0;
    path = opening ? UI_DIR "/subpages/quick.html" :
           g_quick_previous_path[0] ? g_quick_previous_path : g_pages[g_cur];
    if (access(path, R_OK) != 0) return 0;
    capture_fb_logical(d, g_bufA);
    if (!opening) scroll = g_quick_previous_scroll;
    html = page_html(path);
    if (!html) return 0;
    render_page_to_pair_buf(g_bufB, path, html, scroll);
    if (!opening && scroll > 0 && g_quick_previous_header_valid) {
        for (int y = 0; y < PINNED_HEADER_H; y++)
            for (int x = 0; x < 320; x++)
                g_bufB[(size_t)(PINNED_HEADER_TOP + y) * 320 + x] =
                    g_quick_previous_header[(size_t)y * 320 + (319 - x)];
    }
    g_gesture_used_at = millis();
    return 1;
}

static void quick_pair_frame(drm_disp_t *d, int opening, int revealed)
{
    const int W = d->width, Hh = d->height, pp = d->pitch_px;
    const int full = Hh - SBAR_H;
    if (revealed < 0) revealed = 0;
    if (revealed > full) revealed = full;
    int edge = opening ? SBAR_H + revealed : Hh - revealed;
    for (int y = SBAR_H; y < Hh; y++) {
        const uint16_t *src = (opening ? y < edge : y >= edge) ? g_bufB : g_bufA;
        uint16_t *dst = d->fb + (size_t)(Hh - 1 - y) * pp + (W - 1);
        copy_rev_span(dst, src + (size_t)y * W, W);
    }
    if (edge - SBAR_H >= 14) draw_quick_grabber(d, edge - 9);
    drm_disp_dirty(d, 0, 0, W - 1, Hh - SBAR_H - 1);
    maybe_dump_fb(d);
}

static void quick_pair_settle(drm_disp_t *d, int opening, int current, int commit)
{
    const int full = d->height - SBAR_H;
    const int target = commit ? full : 0;
    for (int frame = 1; frame <= 6; frame++) {
        int t = frame * frame * (18 - 2 * frame); /* smoothstep on 0..6, /216 */
        int revealed = current + (target - current) * t / 216;
        quick_pair_frame(d, opening, revealed);
        usleep(18000);
    }
    quick_pair_frame(d, opening, target);
}

static int quick_panel_switch_animated(drm_disp_t *d, int opening,
                                       int prepared, int current, int commit)
{
    if (prepared) quick_pair_settle(d, opening, current, commit);
    if (!commit) return 0;
    if (opening) return quick_panel_open();
    if (!g_quick_open) return 0;
    subpage_close();
    return 1;
}

static int prep_pair(drm_disp_t *d, int target, int dir)
{
    const char *h;
    if (!ensure_pair_buffers()) return 0;
    if (dir > 0) {   /* next: left=current, right=target */
        capture_fb_logical(d, g_bufA);
        html_view_set_scroll(0);        h = page_html(g_pages[target]); render_page_to_pair_buf(g_bufB, g_pages[target], h, 0);
    } else {         /* prev: left=target, right=current */
        html_view_set_scroll(0);        h = page_html(g_pages[target]); render_page_to_pair_buf(g_bufA, g_pages[target], h, 0);
        capture_fb_logical(d, g_bufB);
    }
    html_view_set_scroll(g_scroll);
    g_gesture_used_at = millis();
    return 1;
}

static int prep_subpage_back(drm_disp_t *d)
{
    const char *h;
    if (!g_subpage[0] || g_npages <= 0 || !ensure_pair_buffers()) return 0;
    html_view_set_scroll(0);
    const char *target = g_nav_depth > 0 ? g_nav_stack[g_nav_depth - 1].path : g_pages[g_cur];
    int target_scroll = g_nav_depth > 0 ? g_nav_stack[g_nav_depth - 1].scroll : 0;
    h = page_html(target);
    render_page_to_pair_buf(g_bufA, target, h, target_scroll);
    if (g_nav_depth > 0 && target_scroll > 0 &&
        g_nav_stack[g_nav_depth - 1].header_valid) {
        const uint16_t *saved = g_nav_stack[g_nav_depth - 1].header;
        for (int y = 0; y < PINNED_HEADER_H; y++)
            for (int x = 0; x < 320; x++)
                g_bufA[(size_t)(PINNED_HEADER_TOP + y) * 320 + x] =
                    saved[(size_t)y * 320 + (319 - x)];
    }
    capture_fb_logical(d, g_bufB);
    html_view_set_scroll(g_scroll);
    g_gesture_used_at = millis();
    return 1;
}

/* Settle the offset from o0 to o1 over a few frames. */
static void anim_o(drm_disp_t *d, int o0, int o1)
{
    const int FR = 3;
    for (int f = 1; f <= FR; f++) compose_frame(d, o0 + (o1 - o0) * f / FR);
}

/* Smooth vertical scrolling: the full page is pre-rendered once into g_scrollbuf
 * (logical, unrotated); each drag frame just blits the visible window + scrollbar
 * without re-parsing/re-layout, so it keeps up like the horizontal swipe. */
#define SCROLLMAX 2048
static uint16_t *g_scrollbuf;
static int g_scroll_h;

static int ensure_scroll_buffer(void)
{
    if (!g_scrollbuf)
        g_scrollbuf = gesture_alloc(320u * SCROLLMAX * sizeof(uint16_t));
    return g_scrollbuf != NULL;
}

static int prepare_scroll_buffer(int bufh)
{
    int rh;
    if (!ensure_scroll_buffer()) return -1;
    rh = html_view_draw_current_tall(g_scrollbuf, bufh);
    if (rh <= 0) return -1;

    /* Native charts and speed-test widgets are not part of the litehtml DOM.
     * Bake them into the tall cache once so each drag frame is a pure blit. */
    html_view_target_begin_size(g_scrollbuf, bufh, 0);
    draw_charts();
    if (!g_lock_state && (path_is_speedtest(active_page_path()) || g_st_home_open))
        draw_speedtest_widgets();
    html_view_target_end();
    return rh;
}

static void scroll_blit(drm_disp_t *d, int scroll)
{
    const int W = d->width, Hh = d->height, pp = d->pitch_px;
    for (int y = SBAR_H; y < Hh; y++) {
        int sy = y + scroll;
        const uint16_t *src = (sy >= 0 && sy < g_scroll_h) ? &g_scrollbuf[(size_t)sy * W] : NULL;
        uint16_t *dp = d->fb + (size_t)(Hh - 1 - y) * pp + (W - 1);
        if (src) copy_rev_span(dp, src, W);
        else     fill_rev_span(dp, 0, W);
    }
    if (g_subpage[0] && scroll > 0) pinned_header_draw(d);
    html_view_set_scroll(scroll);
    /* The native status bar is pinned; dragging only changes the content below it.
     * With the framebuffer rotated 180 degrees, that content maps to the top
     * framebuffer rows, so the dirty rectangle intentionally excludes the tail. */
    drm_disp_dirty(d, 0, 0, W - 1, Hh - SBAR_H - 1);
}

/* fb snapshot for fast overlay refresh (modal toggles, segmented-control drag):
 * avoids re-laying-out the whole page on every interaction. */
static uint16_t *g_overbg;
static void capture_fb(drm_disp_t *d) {
    size_t bytes = (size_t)d->pitch_px * d->height * sizeof(uint16_t);
    if (!g_overbg) g_overbg = gesture_alloc(bytes);
    if (g_overbg) memcpy(g_overbg, d->fb, bytes);
}
static void restore_fb(drm_disp_t *d) {
    if (g_overbg) memcpy(d->fb, g_overbg, (size_t)d->pitch_px * d->height * sizeof(uint16_t));
}

static void release_gesture_caches(void)
{
    const size_t frame_bytes = 320u * 480u * sizeof(uint16_t);
    gesture_free((void **)&g_bufA, frame_bytes);
    gesture_free((void **)&g_bufB, frame_bytes);
    gesture_free((void **)&g_scrollbuf, 320u * SCROLLMAX * sizeof(uint16_t));
    gesture_free((void **)&g_overbg, frame_bytes);
    g_scroll_h = 0;
}

static int sms_scroll_metrics(void)
{
    int bx, by, bw, bh;
    int vx, vy, vw, vh;
    int old_scroll = g_sms_scroll;

    if (html_view_rect("#smsview", &vx, &vy, &vw, &vh) &&
        html_view_rect("#smsbody", &bx, &by, &bw, &bh)) {
        g_sms_view_x = vx;
        g_sms_view_y = vy;
        g_sms_view_w = vw;
        g_sms_view_h = vh;
        g_sms_scroll_max = bh > vh ? bh - vh : 0;
    } else {
        g_sms_view_x = g_sms_view_y = g_sms_view_w = g_sms_view_h = 0;
        g_sms_scroll_max = 0;
    }
    g_sms_scroll = clampi(g_sms_scroll, 0, g_sms_scroll_max);
    return g_sms_scroll != old_scroll;
}

static int sms_point_in_view(int x, int y)
{
    return g_sms_view_w > 0 && g_sms_view_h > 0 &&
           x >= g_sms_view_x && x < g_sms_view_x + g_sms_view_w &&
           y >= g_sms_view_y && y < g_sms_view_y + g_sms_view_h;
}

/* Redraw only the SMS dialog while dragging; the dimmed page remains cached. */
static void render_sms_overlay(drm_disp_t *d, int restore_background)
{
    if (restore_background) restore_fb(d);
    html_view_render_overlay(sms_modal_html());
    if (sms_scroll_metrics()) {
        restore_fb(d);
        html_view_render_overlay(sms_modal_html());
        (void)sms_scroll_metrics();
    }
    if (g_sms_scroll_max > 0 && g_sms_view_h > 0) {
        int content_h = g_sms_view_h + g_sms_scroll_max;
        int thumb_h = g_sms_view_h * g_sms_view_h / content_h;
        int travel, thumb_y;
        if (thumb_h < 18) thumb_h = 18;
        if (thumb_h > g_sms_view_h) thumb_h = g_sms_view_h;
        travel = g_sms_view_h - thumb_h;
        thumb_y = g_sms_view_y + g_sms_scroll * travel / g_sms_scroll_max;
        html_view_fill_round_rect(g_sms_view_x + g_sms_view_w - 3, thumb_y,
                                  3, thumb_h, 2, 0x71, 0x81, 0x96, 190);
    }
    drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
}

/* Redraw the modal over the cached dimmed page (fast: no page relayout). */
static void render_modal_overlay(drm_disp_t *d)
{
    restore_fb(d);
    html_view_render_overlay(modal_html());
    drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
}

static void handle_modal_tap(drm_disp_t *disp, int x, int y, uint32_t now,
                             int *need_render, int *animating)
{
    const char *act = html_view_click((float)x, (float)y);

    if (g_modal == 4) {
        if (strncmp(act, "act:", 4)) return;
        const char *a = act + 4;
        if (!strcmp(a, "sigread")) {
            g_sig_read = !g_sig_read;
            (void)datad_set_signal_read(g_sig_read);
            signal_async_publish(NULL, 0);
            save_conf();
            snprintf(g_toast, sizeof g_toast, "信令读取已%s", g_sig_read ? "开启" : "关闭");
            g_toast_until = now + 1600;
            invalidate_render_html_cache();
            *need_render = 1;
        } else if (!strcmp(a, "sigparse")) {
            g_sig_parse = !g_sig_parse;
            (void)datad_set_signal_parse(g_sig_parse);
            signal_async_publish(NULL, 0);
            save_conf();
            rescan_pages_keep_current();
            invalidate_render_html_cache();
            snprintf(g_toast, sizeof g_toast, "信令解析已%s", g_sig_parse ? "开启" : "关闭");
            g_toast_until = now + 1600;
            *need_render = 1;
        } else if (!strcmp(a, "closemodal")) {
            g_modal = 0;
            *need_render = 1;
        }
        return;
    }

    char *sel = g_modal == 1 ? g_sel_sa : g_modal == 2 ? g_sel_nsa : g_sel_lte;
    const char *uni = g_modal == 1 ? g_uni_sa : g_modal == 2 ? g_uni_nsa : g_uni_lte;
    if (!strncmp(act, "act:", 4)) {
        const char *a = act + 4;
        if (!strncmp(a, "bsa:", 4) || !strncmp(a, "bnsa:", 5) || !strncmp(a, "blte:", 5)) {
            band_toggle(sel, 256, strchr(a, ':') + 1);
            render_modal_overlay(disp);
            *animating = 1;
        } else if (!strcmp(a, "mall")) {
            if (!strcmp(sel, uni)) sel[0] = 0;
            else snprintf(sel, 256, "%s", uni);
            render_modal_overlay(disp);
            *animating = 1;
        } else if (!strcmp(a, "minv")) {
            char u[256], out[256];
            int o = 0;
            snprintf(u, sizeof u, "%s", uni);
            for (char *tk = strtok(u, ","); tk; tk = strtok(NULL, ","))
                if (!band_in(sel, tk))
                    o += snprintf(out + o, sizeof out - o, "%s%s", o ? "," : "", tk);
            out[o] = 0;
            snprintf(sel, 256, "%s", out);
            render_modal_overlay(disp);
            *animating = 1;
        } else if (!strcmp(a, "mapply")) {
            if (sel[0]) {
                char cmd[360];
                if (g_modal == 3)
                    snprintf(cmd, sizeof cmd,
                             "ubus call zte_nwinfo_api nwinfo_set_lte_ext_band '{\"lte_band\":\"%s\"}' >/dev/null 2>&1 &",
                             sel);
                else
                    snprintf(cmd, sizeof cmd,
                             "ubus call zte_nwinfo_api nwinfo_set_nrbandlock '{\"nr5g_type\":\"%s\",\"nr5g_band\":\"%s\"}' >/dev/null 2>&1 &",
                             g_modal == 1 ? "0" : "1", sel);
                system(cmd);
            }
            snprintf(g_toast, sizeof g_toast, "锁频成功");
            g_toast_until = now + 1600;
            g_modal = 0;
            *need_render = 1;
        }
    } else {
        g_modal = 0;   /* band-lock dialogs keep their existing tap-outside behavior */
        *need_render = 1;
    }
}

/* Set backlight from a tap/drag x within the brightness bar [bx, bx+bw). */
static void set_bright_x(int x, int bx, int bw)
{
    if (bw <= 0) return;
    int f = (x - bx) * 100 / bw; if (f < 3) f = 3; if (f > 100) f = 100;
    int m = backlight_max(); if (m <= 0) m = 255;
    backlight_set(f * m / 100);
}

/* Screen off: fade backlight down (content still shown) then blank the fb. */
static void screen_off(drm_disp_t *d)
{
    backlight_fade_off();
    g_ui_awake = 0;
    data_backend_suspend();
    invalidate_render_html_cache();
    html_view_suspend();
    release_gesture_caches();
    set_interaction_boost(0);
    memset(d->fb, 0, (size_t)d->pitch_px * d->height * sizeof(uint16_t));
    drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
}
/* Screen on: render, then "warm up" the command-mode panel with several frame
 * pushes while the backlight is still 0; this drives it past the idle-exit
 * transient invisibly, then fades the backlight up from 0. */
static void screen_on(drm_disp_t *d, const char *path)
{
    g_ui_awake = 1;
    (void)data_backend_resume();
    (void)data_backend_commit_latest();
    interaction_pulse(millis());
    if (g_charge_boot) render_charge_boot(d);
    else               render(d, path);       /* backlight still 0 from screen_off */
    for (int k = 0; k < 3; k++) {            /* ~75ms command-mode wake settling */
        usleep(25000);
        drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
    }
    backlight_fade_on();   /* 0 -> user level */
}

static void screen_on_ext(drm_disp_t *d, devui_ext_t *ext)
{
    g_ui_awake = 1;
    (void)data_backend_resume();
    (void)data_backend_commit_latest();
    interaction_pulse(millis());
    render_ext_view(d, ext);                  /* backlight still 0 from screen_off */
    for (int k = 0; k < 3; k++) {
        usleep(25000);
        drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
    }
    backlight_fade_on();
}

int main(void)
{
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGUSR1, on_plugin_diag_sig);

    g_charge_boot = boot_is_charge_mode();
    if (!g_charge_boot) {
        scan_pages();
        if (g_npages == 0) { fprintf(stderr, "no pages in %s\n", UI_DIR); return 1; }
    }

    drm_disp_t disp;
    if (drm_disp_init(&disp) != 0) { fprintf(stderr, "drm init failed\n"); return 1; }
    html_view_init(disp.fb, disp.width, disp.height, disp.pitch_px, 1, UI_FONT);
    html_view_set_uidir(UI_DIR);

    touch_input_t touch;
    int touch_ok = touch_input_init(&touch, disp.width, disp.height) == 0;
    key_input_t key;     key_input_init(&key);
    backlight_init();
    (void)data_backend_init();
    (void)data_backend_commit_latest();

    if (!g_charge_boot && !touch_ok && boot_has_external_power()) {
        fprintf(stderr, "boot: forcing charge UI (touch missing, external power present)\n");
        g_charge_boot = 1;
    }

    int menu = 0, prev_press = 0, prev_lock = 0, was_on = 1, down_x = 0, down_y = 0;
    int dragging = 0, drag_dir = 0, drag_target = 0, back_drag = 0, quick_drag = 0, quick_prepared = 0;
    int scroll_dir = 0, scroll_start = 0;
    int sms_dragging = 0, sms_scroll_start = 0;
    int scroll_inertia = 0, scroll_track_valid = 0;
    int scroll_track_pos = 0;
    uint32_t drag_down_ms = 0;
    uint32_t scroll_track_ms = 0;
    uint32_t inertia_frame_at = 0;
    uint32_t last_motion_frame = 0;
    float scroll_v = 0.0f, scroll_pos = 0.0f;
    int sliding = 0, bar_x = 0, bar_w = 0, bright_before_drag = 0;   /* brightness slider drag */
    int segging = 0, seg_x = 0, seg_w = 0;   /* segmented control selection */
    int seg_which = 0, seg_n = 4;   /* 1 = net mode, 2 = auto-off, 3 = refresh */
    const int W = disp.width, H = disp.height;
    devui_ext_t ext;
    memset(&ext, 0, sizeof ext);
    ext.srv_fd = -1;
    int ext_ok = 0;
    if (!g_charge_boot) {
        ext_ok = devui_ext_init(&ext, W, H) == 0;
        if (!ext_ok) fprintf(stderr, "warning: external display socket disabled\n");
    }
    char menu_path[300], lock_path[300];
    snprintf(menu_path, sizeof menu_path, "%s/menu.html", UI_DIR);
    snprintf(lock_path, sizeof lock_path, "%s/lockscreen.html", UI_DIR);

    /* lock pad takes priority over the power menu and the normal pages */
    /* lock states: 1=preview (page 1), 2=setup pad, 3=unlock pad */
    #define CUR_PATH (g_lock_state == 1 ? g_pages[0] : g_lock_state ? lock_path : menu ? menu_path : active_page_path())

    load_conf();                          /* restore persisted UI settings */
    fm_display_load();                    /* migrate prior auto-detection, then obey the user switch */
    mkdir(FUNCTIONS_DIR, 0755);
    plugin_detect_log_all(1);
    speedtest_apply_saved_prefs();
    speedtest_poll(millis());
    if (!g_charge_boot) rescan_pages_keep_current();
    g_pages_dir_mtime = ui_scan_mtime_ns();
    g_pages_scan_at = millis();
    (void)datad_set_refresh_ms(g_refresh_ms);
    (void)datad_set_signal_read(g_sig_read);
    (void)datad_set_signal_parse(g_sig_parse);
    if (!g_charge_boot)
        signal_async_start();
    if (g_saved_bright >= 0) backlight_set(g_saved_bright);   /* restore brightness */
    if (!g_charge_boot) {
        load_pin();
        wifi_psm_prepare();                     /* migrate old plugin rules and restore PSM */
        wifi_aux_refresh();                   /* prime page-2 switch states */
        if (usb_pid_is("9057") || usb_pid_is("90b1") || usb_pid_is("90B1"))
            usb_net_watchdog_start();
        if (lock_enabled()) enter_lock(0);   /* boot straight into the unlock pad */
        render(&disp, CUR_PATH);
    } else {
        render_charge_boot(&disp);
    }
    uint32_t last_data = millis(), last_act = millis();
    int state_pending = 0;

    while (g_run) {
        uint32_t now = millis();
        int need_render = 0, animating = 0;
        if (g_plugin_diag_requested) {
            g_plugin_diag_requested = 0;
            plugin_detect_log_all(1);
        }
        int live_changed = data_backend_poll(now);
        if (live_changed) state_pending = 1;
        chart_sample_tick(backlight_is_on());

        if (backlight_is_on() && ext_ok && g_lock_state && devui_ext_active(&ext)) {
            devui_ext_deactivate(&ext);
            touch_input_clear_taps(&touch);
            need_render = 1;
        }
        if (backlight_is_on() && ext_ok && !g_lock_state) {
            int ext_changed = devui_ext_poll(&ext, now);
            if (ext_changed) {
                dragging = 0; drag_dir = 0; back_drag = 0; scroll_dir = 0; sliding = 0; segging = 0;
                scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
                menu = 0; g_modal = 0; g_sms_open = -1; subpage_close();
                touch_input_clear_taps(&touch);
                if (devui_ext_active(&ext)) {
                    refresh_status_cache();
                    if (backlight_is_on()) {
                        render_ext_view(&disp, &ext);
                        animating = 1;
                    }
                } else {
                    need_render = 1;
                }
            }
        }

        /* power key */
        int ev = key_input_poll(&key, now);
        if (ev == KEY_EV_SHORT || ev == KEY_EV_LONG) last_act = now;
        if (ev == KEY_EV_SHORT) {
            scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
            if (backlight_is_on()) {
                screen_off(&disp);
                if (!g_charge_boot && lock_enabled()) {
                    if (ext_ok) devui_ext_deactivate(&ext);
                    enter_lock(0);   /* power key locks the screen */
                }
            } else {
                if (ext_ok && devui_ext_active(&ext) && !g_lock_state)
                    screen_on_ext(&disp, &ext);
                else
                    screen_on(&disp, g_charge_boot ? NULL : CUR_PATH);
            }
        } else if (ev == KEY_EV_LONG) {
            scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
            if (ext_ok && devui_ext_active(&ext) && !g_lock_state) {
                if (!backlight_is_on()) screen_on_ext(&disp, &ext);
                else { devui_ext_deactivate(&ext); need_render = 1; }
            } else if (g_charge_boot) {
                if (!backlight_is_on()) screen_on(&disp, NULL);
            } else if (g_lock_state) {               /* no power menu while locked */
                if (!backlight_is_on()) screen_on(&disp, CUR_PATH);
            } else {
                menu = !menu; g_pwr_confirm = 0;
                if (!backlight_is_on()) screen_on(&disp, CUR_PATH);
                else need_render = 1;
            }
        }

        if (!backlight_is_on()) {
            touch_input_clear_taps(&touch);
            prev_press = 0;
            was_on = 0;
            usleep(IDLE_SLEEP_OFF_US);
            continue;
        }

        if (g_charge_boot) {
            if (live_changed && data_backend_commit_latest()) {
                state_pending = 0;
                need_render = 1;
            }
            touch_input_clear_taps(&touch);
            prev_press = 0;
            if (now - last_data >= 120) { need_render = 1; last_data = now; }
            if (need_render && backlight_is_on()) render_charge_boot(&disp);
            was_on = backlight_is_on();
            if (!animating) usleep(backlight_is_on() ? IDLE_SLEEP_ON_US : IDLE_SLEEP_OFF_US);
            continue;
        }

        /* touch: follow-finger swipe (pages) / drag (scroll) + tap actions */
        int x, y, pressed;
        int replay_release = 0;
        touch_input_read(&touch, &x, &y, &pressed);

        int on_now = backlight_is_on();
        if (on_now && !was_on) {          /* first frame after waking: drop the wake touch */
            touch_input_clear_taps(&touch);
            pressed = 0;
        }
        if (pressed && on_now) last_act = now;
        if (pressed && on_now) {
            interaction_pulse(now);
            g_gesture_used_at = now;
        }
        /* Do not paint a generic touch rectangle over controls. It exposed the
         * square hit box for one frame before the rounded selected state was
         * rendered. State changes now redraw directly on release. */

        if (!on_now) {
            /* screen off: ignore touch entirely and discard any taps the panel
             * reported while dark, so they can't replay on wake. Power key is the
             * only way to wake (double-tap-to-wake removed). */
            touch_input_clear_taps(&touch);
            pressed = 0;
            scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
        } else if (g_lock_state == 1) {
            /* locked preview: page 1 with live data, no actions. A swipe in any
             * direction opens the PIN pad. */
            if (pressed && !prev_press) { down_x = x; down_y = y; }
            else if (pressed && prev_press) {
                int dx = x - down_x, dy = y - down_y;
                if (dx * dx + dy * dy > 22 * 22) {
                    g_lock_state = 3; g_pin_entry[0] = 0; g_lock_err = 0; g_scroll = 0;
                    need_render = 1;
                }
            }
        } else if (g_lock_state) {
            /* PIN pad (3=unlock / 2=setup): tap only, no confirm key; a 4th digit
             * auto-submits. Drain the tap queue so a fast burst of taps all land
             * (the per-digit re-render would otherwise let polls miss presses). */
            int tx, ty;
            while (g_lock_state && touch_input_take_tap(&touch, &tx, &ty)) {
                const char *act = html_view_click((float)tx, (float)ty);
                if (strncmp(act, "act:", 4)) continue;
                const char *a = act + 4;
                if (!strncmp(a, "pin:", 4)) {
                    const char *k = a + 4;
                    int len = (int)strlen(g_pin_entry);
                    if (!strcmp(k, "del")) {
                        if (len > 0) g_pin_entry[len - 1] = 0;
                        g_lock_err = 0;
                    } else if (k[0] >= '0' && k[0] <= '9' && !k[1]) {
                        if (len < 4) { g_pin_entry[len] = k[0]; g_pin_entry[len + 1] = 0; len++; g_lock_err = 0; }
                        if (len == 4) {                       /* auto-submit */
                            if (g_lock_state == 2) {          /* setup: store + enable */
                                save_pin(g_pin_entry);
                                g_pin_entry[0] = 0; g_lock_state = 0;
                            } else if (!strcmp(g_pin_entry, g_pin)) {   /* unlock ok */
                                g_pin_entry[0] = 0; g_lock_err = 0; g_lock_state = 0;
                            } else {                          /* wrong: keep pad, show error */
                                g_pin_entry[0] = 0; g_lock_err = 1;
                            }
                        }
                    }
                    need_render = 1;
                } else if (!strcmp(a, "lockcancel")) {        /* abort PIN setup */
                    g_pin_entry[0] = 0; g_lock_err = 0; g_lock_state = 0; need_render = 1;
                }
            }
        } else if (ext_ok && devui_ext_active(&ext)) {
            if (!pressed) {
                int sx, sy, ex, ey;
                while (devui_ext_active(&ext) &&
                       touch_input_take_stroke(&touch, &sx, &sy, &ex, &ey)) {
                    int dx = ex - sx, dy = ey - sy;
                    int ady = dy < 0 ? -dy : dy;
                    if (sx <= EXT_BACK_EDGE_PX && sy >= ext.content_y &&
                        dx > EXT_BACK_COMMIT_PX && ady <= EXT_BACK_MAX_DY) {
                        devui_ext_deactivate(&ext);
                        touch_input_clear_taps(&touch);
                        need_render = 1;
                        break;
                    }
                }
            }

            if (devui_ext_active(&ext) && !pressed) {
                int tx, ty, cx, cy;
                while (touch_input_take_tap(&touch, &tx, &ty))
                    if (devui_ext_content_point(&ext, tx, ty, &cx, &cy))
                        devui_ext_handle_tap(&ext, cx, cy, now);
            }
            dragging = 0; drag_dir = 0; back_drag = 0; scroll_dir = 0; sliding = 0; segging = 0;
            scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
        } else if (g_sms_open >= 0) {
            if (pressed && !prev_press) {
                down_x = x;
                down_y = y;
                sms_scroll_start = g_sms_scroll;
                sms_dragging = 0;
            } else if (pressed && prev_press) {
                int dy = y - down_y;
                int ady = dy < 0 ? -dy : dy;
                int next = clampi(sms_scroll_start - dy, 0, g_sms_scroll_max);
                if (!sms_dragging && ady > 6 && next != g_sms_scroll &&
                    sms_point_in_view(down_x, down_y))
                    sms_dragging = 1;
                if (sms_dragging) {
                    if (next != g_sms_scroll && motion_frame_due(now, &last_motion_frame)) {
                        g_sms_scroll = next;
                        render_sms_overlay(&disp, 1);
                        animating = 1;
                    }
                }
            } else if (!pressed && prev_press) {
                int dx = x - down_x, dy = y - down_y;
                int release_was_tap = dx * dx + dy * dy <= 14 * 14;
                if (sms_dragging) {
                    int next = clampi(sms_scroll_start - dy, 0, g_sms_scroll_max);
                    if (next != g_sms_scroll) {
                        g_sms_scroll = next;
                        render_sms_overlay(&disp, 1);
                        animating = 1;
                    }
                } else if (release_was_tap) {
                    const char *act = html_view_click((float)down_x, (float)down_y);
                    if (!strcmp(act, "act:smsclose")) {
                        sms_close(&touch, &need_render);
                    }
                }
                if (g_sms_open >= 0)
                    touch_input_drop_replayed_release(&touch, release_was_tap);
                sms_dragging = 0;
            } else if (!pressed) {
                /* A complete gesture can land while layout is busy. Collapse queued
                 * input to its newest stroke so it cannot replay below the dialog. */
                int sx, sy, ex, ey;
                if (touch_input_take_latest_stroke(&touch, &sx, &sy, &ex, &ey)) {
                    int tx = sx, ty = sy;
                    int dx = ex - sx, dy = ey - sy;
                    int was_tap = dx * dx + dy * dy <= 14 * 14;
                    int have_tap = touch_input_take_latest_tap(&touch, &tx, &ty);
                    if (was_tap) {
                        const char *act = html_view_click((float)(have_tap ? tx : sx),
                                                          (float)(have_tap ? ty : sy));
                        if (!strcmp(act, "act:smsclose")) {
                            sms_close(&touch, &need_render);
                        }
                    } else if (sms_point_in_view(sx, sy) && g_sms_scroll_max > 0) {
                        int next = clampi(g_sms_scroll - dy, 0, g_sms_scroll_max);
                        if (next != g_sms_scroll) {
                            g_sms_scroll = next;
                            render_sms_overlay(&disp, 1);
                            animating = 1;
                        }
                    }
                } else {
                    int tx, ty;
                    if (touch_input_take_latest_tap(&touch, &tx, &ty)) {
                        const char *act = html_view_click((float)tx, (float)ty);
                        if (!strcmp(act, "act:smsclose")) {
                            sms_close(&touch, &need_render);
                        }
                    }
                }
            }
        } else if (g_modal) {
            /* second-level band-lock dialog: tap only (level-1 is disabled) */
            if (pressed && !prev_press) {
                down_x = x;
                down_y = y;
            } else if (!pressed && prev_press) {
                int mdx = x - down_x, mdy = y - down_y;
                int modal_was_tap = mdx * mdx + mdy * mdy <= 14 * 14;
                if (modal_was_tap)
                    handle_modal_tap(&disp, x, y, now, &need_render, &animating);
                /* The renderer has already handled this release. Remove its matching
                 * queue entries before a closed modal exposes the button underneath. */
                touch_input_drop_replayed_release(&touch, modal_was_tap);
            } else if (!pressed) {
                int sx, sy, ex, ey, tx, ty;
                int have_stroke = touch_input_take_latest_stroke(&touch, &sx, &sy, &ex, &ey);
                int have_tap = touch_input_take_latest_tap(&touch, &tx, &ty);
                if (have_tap && (!have_stroke ||
                    (ex - sx) * (ex - sx) + (ey - sy) * (ey - sy) <= 14 * 14))
                    handle_modal_tap(&disp, tx, ty, now, &need_render, &animating);
            }
        } else {
            int maxs = g_page_h > H ? g_page_h - H : 0;
            if (pressed && scroll_inertia) {
                scroll_inertia = 0;
                scroll_track_valid = 0;
                scroll_v = 0.0f;
            }
            if (scroll_inertia && !pressed && !dragging && !menu && maxs > 0) {
                if ((int32_t)(now - inertia_frame_at) >= 0) {
                    uint32_t dtm = now > scroll_track_ms ? now - scroll_track_ms : TOUCH_FRAME_MIN_MS;
                    if (dtm > 50) dtm = 50;
                    float dt = dtm / 1000.0f;
                    float dv = SCROLL_INERTIA_DECEL * dt;
                    float old_v = scroll_v;
                    if (scroll_v > 0.0f) {
                        scroll_v -= dv;
                        if (scroll_v < 0.0f) scroll_v = 0.0f;
                    } else if (scroll_v < 0.0f) {
                        scroll_v += dv;
                        if (scroll_v > 0.0f) scroll_v = 0.0f;
                    }
                    scroll_pos += (old_v + scroll_v) * 0.5f * dt;
                    int ns = (int)(scroll_pos + (scroll_pos >= 0.0f ? 0.5f : -0.5f));
                    if (ns < 0) { ns = 0; scroll_pos = 0.0f; scroll_v = 0.0f; }
                    if (ns > maxs) { ns = maxs; scroll_pos = (float)maxs; scroll_v = 0.0f; }
                    if (ns != g_scroll) {
                        g_scroll = ns;
                        scroll_blit(&disp, g_scroll);
                        animating = 1;
                    }
                    if (scroll_v == 0.0f || ns == 0 || ns == maxs) scroll_inertia = 0;
                    scroll_track_ms = now;
                    inertia_frame_at += TOUCH_FRAME_MIN_MS;
                    if ((int32_t)(now - inertia_frame_at) >= TOUCH_FRAME_MIN_MS)
                        inertia_frame_at = now + TOUCH_FRAME_MIN_MS;
                }
            }
            if (!pressed && !prev_press && !dragging && !menu) {
                int sx, sy, ex, ey, queued_motion = 0;
                if (touch_input_take_latest_stroke(&touch, &sx, &sy, &ex, &ey)) {
                    int qdx = ex - sx, qdy = ey - sy;
                    int qadx = qdx < 0 ? -qdx : qdx;
                    int qady = qdy < 0 ? -qdy : qdy;
                    if (qady >= (g_quick_open ? 40 : 65) && qady > qadx &&
                        ((g_quick_open && qdy < 0) || (!g_quick_open && qdy > 0 && sy <= 40))) {
                        int opening = !g_quick_open;
                        int prepared = quick_pair_prepare(&disp, opening);
                        int changed = quick_panel_switch_animated(&disp, opening, prepared, 0, 1);
                        if (changed) {
                            scroll_inertia = 0;
                            scroll_track_valid = 0;
                            scroll_v = 0.0f;
                            invalidate_render_html_cache();
                            need_render = 1;
                            queued_motion = 1;
                            last_act = now;
                            touch_input_clear_taps(&touch);
                        }
                    } else if (g_subpage[0] && !g_quick_open && qdx >= W * 30 / 100 && qadx > qady) {
                        int o_now = W - qdx;
                        if (o_now < 0) o_now = 0;
                        if (o_now > W) o_now = W;
                        scroll_inertia = 0;
                        scroll_track_valid = 0;
                        scroll_v = 0.0f;
                        if (prep_subpage_back(&disp)) {
                            anim_o(&disp, o_now, 0);
                            subpage_close();
                            invalidate_render_html_cache();
                            need_render = 1;
                            animating = 1;
                            last_motion_frame = now;
                            queued_motion = 1;
                            touch_input_clear_taps(&touch);
                        }
                    } else if (!g_subpage[0] && g_npages > 1 && qadx >= QUEUED_SWIPE_PX && qadx > qady) {
                        int dir = qdx < 0 ? 1 : -1;
                        int target = (g_cur + (dir > 0 ? 1 : g_npages - 1)) % g_npages;
                        int o_now = dir > 0 ? -qdx : (W - qdx);
                        if (o_now < 0) o_now = 0;
                        if (o_now > W) o_now = W;
                        scroll_inertia = 0;
                        scroll_track_valid = 0;
                        scroll_v = 0.0f;
                        if (prep_pair(&disp, target, dir)) {
                            anim_o(&disp, o_now, dir > 0 ? W : 0);
                            g_cur = target;
                            g_scroll = 0;
                            invalidate_render_html_cache();
                            need_render = 1;
                            animating = 1;
                            last_motion_frame = now;
                            queued_motion = 1;
                            touch_input_clear_taps(&touch);
                        }
                    } else if (qady >= QUEUED_SCROLL_PX && qady >= qadx && maxs > 0) {
                        int bufh = g_page_h > SCROLLMAX ? SCROLLMAX : g_page_h;
                        int rh;
                        if (!ensure_scroll_buffer()) goto queued_done;
                        rh = prepare_scroll_buffer(bufh);
                        if (rh <= 0) goto queued_done;
                        int ns;
                        g_scroll_h = rh > bufh ? bufh : rh;
                        maxs = g_scroll_h > H ? g_scroll_h - H : 0;
                        ns = g_scroll - qdy;
                        if (ns < 0) ns = 0;
                        if (ns > maxs) ns = maxs;
                        scroll_inertia = 0;
                        scroll_track_valid = 0;
                        scroll_v = 0.0f;
                        if (ns != g_scroll) {
                            g_scroll = ns;
                            scroll_pos = (float)g_scroll;
                            scroll_blit(&disp, g_scroll);
                            animating = 1;
                            last_motion_frame = now;
                        }
                        queued_motion = 1;
                        touch_input_clear_taps(&touch);
                    }
                }
queued_done:
                if (!queued_motion) {
                    int tx, ty;
                    if (touch_input_take_latest_tap(&touch, &tx, &ty)) {
                        x = tx; y = ty;
                        down_x = tx; down_y = ty;
                        dragging = 1; drag_dir = 0; back_drag = 0; quick_drag = 0; quick_prepared = 0; scroll_dir = 0; scroll_start = g_scroll; sliding = 0; segging = 0;
                        drag_down_ms = now;
                        scroll_inertia = 0;
                        scroll_track_valid = 0;
                        scroll_v = 0.0f;
                        last_motion_frame = 0;
                        replay_release = 1;
                        prev_press = 1;  /* reuse the normal release/action path below */
                    }
                }
            }
            if (pressed && !prev_press) {
                down_x = x; down_y = y; dragging = 1; drag_dir = 0; back_drag = 0; quick_drag = 0; quick_prepared = 0; scroll_dir = 0; scroll_start = g_scroll; sliding = 0; segging = 0;
                last_motion_frame = 0;
                drag_down_ms = now;
                scroll_track_valid = 0; scroll_v = 0.0f;
                int bx, by, bw, bh;   /* grab the brightness slider / segmented control if pressed on it */
                int top_gesture_zone = y < (g_subpage[0] ?
                    PINNED_HEADER_TOP + PINNED_HEADER_H : 40);
                if (!top_gesture_zone && html_view_rect("#bright-bar", &bx, &by, &bw, &bh) &&
                    x >= bx && x < bx + bw && y >= by - 5 && y < by + bh + 5) {
                    sliding = 1; bar_x = bx; bar_w = bw;
                    bright_before_drag = backlight_get();
                    /* Wait for direction: a vertical swipe should close the
                     * shade without first changing brightness. */
                } else {   /* segmented controls: net mode / auto-off / refresh */
                    int which = 0, n = 4;
                    if (!top_gesture_zone && html_view_rect("#netseg", &bx, &by, &bw, &bh) &&
                        x >= bx && x < bx + bw && y >= by - 4 && y < by + bh + 4) { which = 1; n = 4; }
                    else if (!top_gesture_zone && html_view_rect("#autoseg", &bx, &by, &bw, &bh) &&
                             x >= bx && x < bx + bw && y >= by - 4 && y < by + bh + 4) { which = 2; n = 6; }
                    else if (!top_gesture_zone && html_view_rect("#refreshseg", &bx, &by, &bw, &bh) &&
                             x >= bx && x < bx + bw && y >= by - 4 && y < by + bh + 4) { which = 3; n = 5; }
                    if (which) {
                        segging = 1; seg_which = which; seg_n = n;
                        seg_x = bx; seg_w = bw;
                        /* Keep the current rounded state visible while the
                         * finger is down. The new state appears on release. */
                    }
                }
            }
            else if (pressed && dragging && !menu) {
                int qdx = x - down_x, qdy = y - down_y;
                int qad_x = qdx < 0 ? -qdx : qdx;
                if ((sliding || segging) && (qdy < -DRAG_CANCEL_PX || qdy > DRAG_CANCEL_PX) &&
                    (qdy < 0 ? -qdy : qdy) > qad_x) {
                    if (sliding) backlight_set(bright_before_drag);
                    sliding = 0;
                    segging = 0;
                    if (g_quick_open && qdy < 0) {
                        quick_drag = -1;
                        quick_prepared = quick_pair_prepare(&disp, 0);
                    } else if (maxs > 0) {
                        scroll_dir = 1; scroll_start = g_scroll;
                        int bufh = g_page_h > SCROLLMAX ? SCROLLMAX : g_page_h;
                        int rh = prepare_scroll_buffer(bufh);
                        if (rh <= 0) {
                            scroll_dir = 0;
                            dragging = 0;
                        } else {
                            g_scroll_h = rh > bufh ? bufh : rh;
                            maxs = g_scroll_h > H ? g_scroll_h - H : 0;
                            scroll_track_pos = g_scroll;
                            scroll_track_ms = now;
                            scroll_track_valid = 0;
                            scroll_v = 0.0f;
                        }
                    } else {
                        dragging = 0;
                    }
                }
                if (sliding) {
                    set_bright_x(x, bar_x, bar_w);
                    if (motion_frame_due(now, &last_motion_frame)) {
                        render(&disp, CUR_PATH);
                        animating = 1;
                    }
                }
                else if (segging) {
                    /* No intermediate drag animation: it used a rectangular
                     * overlay that did not match the final rounded button. */
                }
                else {
                int dx = x - down_x, dy = y - down_y;
                int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                if (drag_dir == 0 && scroll_dir == 0 && quick_drag == 0) {
                    if (ady > DRAG_START_PX && ady > adx &&
                        ((g_quick_open && dy < 0) ||
                         (!g_quick_open && down_y <= 40 && dy > 0))) {
                        quick_drag = dy > 0 ? 1 : -1;
                        quick_prepared = quick_pair_prepare(&disp, quick_drag > 0);
                    } else if (g_subpage[0] && !g_quick_open && dx > 0 && adx > DRAG_START_PX && adx > ady) {
                        back_drag = 1;
                        drag_dir = -1;
                        if (!prep_subpage_back(&disp)) {
                            back_drag = 0;
                            drag_dir = 0;
                            dragging = 0;
                        }
                    } else if (!g_subpage[0] && g_npages > 1 && adx > DRAG_START_PX && adx > ady) {
                        drag_dir = dx < 0 ? 1 : -1;
                        drag_target = (g_cur + (drag_dir > 0 ? 1 : g_npages - 1)) % g_npages;
                        if (!prep_pair(&disp, drag_target, drag_dir)) {
                            drag_dir = 0;
                            dragging = 0;
                        }
                    } else if (ady > DRAG_START_PX && ady >= adx && maxs > 0) {
                        scroll_dir = 1; scroll_start = g_scroll;
                        int bufh = g_page_h > SCROLLMAX ? SCROLLMAX : g_page_h;  /* prerender once */
                        int rh = prepare_scroll_buffer(bufh);
                        if (rh <= 0) {
                            scroll_dir = 0;
                            dragging = 0;
                        } else {
                            g_scroll_h = rh > bufh ? bufh : rh;
                            maxs = g_scroll_h > H ? g_scroll_h - H : 0;
                            scroll_track_pos = g_scroll;
                            scroll_track_ms = now;
                            scroll_track_valid = 0;
                            scroll_v = 0.0f;
                        }
                    } else if (ady > DRAG_CANCEL_PX ||
                               (g_subpage[0] && adx > DRAG_CANCEL_PX && adx > ady)) dragging = 0;
                }
                if (drag_dir != 0) {
                    if (motion_frame_due(now, &last_motion_frame)) {
                        compose_frame(&disp, drag_dir > 0 ? -dx : (W - dx));
                        animating = 1;
                    }
                } else if (quick_drag != 0 && quick_prepared) {
                    int revealed = quick_drag > 0 ? dy : -dy;
                    if (motion_frame_due(now, &last_motion_frame)) {
                        quick_pair_frame(&disp, quick_drag > 0, revealed);
                        animating = 1;
                    }
                } else if (scroll_dir != 0) {
                    int ns = scroll_start - dy;
                    if (ns < 0) ns = 0;
                    if (ns > maxs) ns = maxs;
                    if (ns != g_scroll) {
                        if (scroll_track_ms && now > scroll_track_ms) {
                            float inst = (ns - scroll_track_pos) * 1000.0f / (float)(now - scroll_track_ms);
                            scroll_v = scroll_track_valid ? (scroll_v * 0.65f + inst * 0.35f) : inst;
                            scroll_v = clamp_scroll_v(scroll_v);
                            scroll_track_valid = 1;
                        }
                        g_scroll = ns;
                        scroll_pos = (float)g_scroll;
                        scroll_track_pos = g_scroll;
                        scroll_track_ms = now;
                        if (motion_frame_due(now, &last_motion_frame)) {
                            scroll_blit(&disp, g_scroll);
                            animating = 1;
                        }
                    }
                }
                }
            }
            else if (!pressed && prev_press) {
                int rel_dx = x - down_x, rel_dy = y - down_y;
                int release_was_tap = dragging && !sliding && !segging &&
                                      drag_dir == 0 && scroll_dir == 0 && quick_drag == 0 &&
                                      rel_dx * rel_dx + rel_dy * rel_dy <= 14 * 14;
                int dx = rel_dx;
                if (sliding) {
                    set_bright_x(x, bar_x, bar_w);
                    save_conf();
                    need_render = 1;
                }
                else if (segging) {   /* snap to nearest cell + apply */
                    int c = clampi((x - seg_x) * seg_n / (seg_w > 0 ? seg_w : 1), 0, seg_n - 1);
                    if (seg_which == 1) {
                        char cmd[160];
                        snprintf(cmd, sizeof cmd,
                            "ubus call zte_nwinfo_api nwinfo_set_netselect '{\"net_select\":\"%s\"}' >/dev/null 2>&1 &", g_netmodes[c].v);
                        system(cmd);
                        snprintf(g_net_pending, sizeof g_net_pending, "%s", g_netmodes[c].v);
                    } else if (seg_which == 2) {
                        g_autooff_ms = g_autooffs[c].ms; last_act = now; save_conf();
                    } else {
                        g_refresh_ms = normalize_refresh_ms(g_refresh_rates[c].ms);
                        (void)datad_set_refresh_ms(g_refresh_ms);
                        if (g_refresh_ms > 0) {
                            if (data_backend_commit_latest()) need_render = 1;
                            state_pending = 0;
                        }
                        last_data = 0;
                        save_conf();
                    }
                    need_render = 1;
                }
                else if (dragging && quick_drag != 0) {
                    int opening = quick_drag > 0;
                    int revealed = opening ? rel_dy : -rel_dy;
                    int commit = opening ? rel_dy >= 65 : rel_dy <= -40;
                    int changed = quick_panel_switch_animated(&disp, opening, quick_prepared,
                                                              revealed, commit);
                    if (changed || quick_prepared) {
                        scroll_inertia = 0;
                        scroll_v = 0.0f;
                        invalidate_render_html_cache();
                        need_render = 1;
                        last_act = now;
                        touch_input_clear_taps(&touch);
                    }
                }
                else if (dragging && drag_dir != 0) {            /* finish or snap back */
                    int adx = dx < 0 ? -dx : dx;
                    int commit = back_drag ? dx > W * 30 / 100 : adx > W * 30 / 100;
                    int o_now = drag_dir > 0 ? -dx : (W - dx);
                    if (o_now < 0) o_now = 0; if (o_now > W) o_now = W;
                    anim_o(&disp, o_now, drag_dir > 0 ? (commit ? W : 0) : (commit ? 0 : W));
                    if (commit) {
                        if (back_drag) subpage_close();
                        else { g_cur = drag_target; g_scroll = 0; }
                    }
                    /* After a page-swipe preview or snap-back, the on-screen pixels no
                     * longer match the cached HTML of the current page. Force a full
                     * re-render so native-drawn content from the neighbor page (for
                     * example charts) cannot leak onto the restored page. */
                    invalidate_render_html_cache();
                    need_render = 1;
                } else if (dragging && scroll_dir != 0) {   /* scroll settled */
                    uint32_t gdt = (drag_down_ms && now > drag_down_ms) ? (now - drag_down_ms) : 0;
                    if (gdt > 0) {
                        float rel_v = (-(float)(y - down_y)) * 1000.0f / (float)gdt;
                        if (scroll_track_valid) scroll_v = clamp_scroll_v(scroll_v * 0.85f + rel_v * 0.15f);
                        else                    scroll_v = clamp_scroll_v(rel_v);
                    }
                    if (fabsf(scroll_v) >= SCROLL_INERTIA_MIN_V && maxs > 0) {
                        scroll_inertia = 1;
                        scroll_pos = (float)g_scroll;
                        scroll_track_ms = now;
                        inertia_frame_at = now;
                        last_motion_frame = 0;
                    } else {
                        scroll_inertia = 0;
                        scroll_v = 0.0f;
                    }
                } else if (dragging) {                      /* tap -> action */
                    const char *act;
                    if (g_subpage[0] && g_scroll > 0 &&
                        y >= PINNED_HEADER_TOP && y < PINNED_HEADER_TOP + PINNED_HEADER_H) {
                        if (x < 86)
                            act = g_quick_open ? "" : "act:backfunc";
                        else act = "";
                    } else {
                        act = html_view_click((float)x, (float)y);
                    }
                    if (!strncmp(act, "act:", 4)) {
                        const char *a = act + 4;
                        if (!strcmp(a, "poweroff")) {
                            if (g_pwr_confirm == 1) {        /* confirmed: blank first (instant feedback), then power off */
                                screen_off(&disp); system("poweroff");
                            } else { g_pwr_confirm = 1; g_pwr_until = now + 4000; need_render = 1; }
                        }
                        else if (!strcmp(a, "reboot")) {
                            if (g_pwr_confirm == 2) { screen_off(&disp); system("reboot"); }
                            else { g_pwr_confirm = 2; g_pwr_until = now + 4000; need_render = 1; }
                        }
                        else if (!strcmp(a, "close"))     { menu = 0; g_pwr_confirm = 0; need_render = 1; }
                        else if (!strcmp(a, "quicksettings")) {
                            if (g_quick_open) subpage_close();
                            if (!strcmp(g_subpage, "settings.html") || subpage_open("settings.html")) {
                                scroll_inertia = 0;
                                invalidate_render_html_cache();
                                need_render = 1;
                            }
                        }
                        else if (!strcmp(a, "menu"))      { backlight_on(); menu = 1; g_pwr_confirm = 0; need_render = 1; }
                        else if (!strncmp(a, "sub:", 4)) {
                            if (subpage_open(a + 4)) {
                                if (!strcmp(a + 4, "lock.html")) (void)sim_slot_refresh(now, 1);
                                menu = 0;
                                g_modal = 0;
                                g_sms_open = -1;
                                invalidate_render_html_cache();
                            } else {
                                snprintf(g_toast, sizeof g_toast, "页面不可用");
                                g_toast_until = now + 1600;
                            }
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "func:", 5)) {
                            if (function_page_open(a + 5)) {
                                if (!strcmp(a + 5, "sim-switch.html") ||
                                    !strcmp(a + 5, "sim-traffic.html"))
                                    (void)sim_slot_refresh(now, 1);
                                if (!strcmp(a + 5, "sim-traffic.html")) {
                                    g_plan_edit_slot = g_sim_slot == 2 ? 2 : 1;
                                    g_plan_draft_dirty = 0;
                                    g_plan_draft_sim_id[0] = 0;
                                }
                                if (!strcmp(a + 5, "timezone.html"))
                                    g_tz_draft_dirty = 0;
                                menu = 0;
                                g_modal = 0;
                                g_sms_open = -1;
                                invalidate_render_html_cache();
                            } else {
                                snprintf(g_toast, sizeof g_toast, "页面不可用");
                                g_toast_until = now + 1600;
                            }
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "backfunc")) {
                            subpage_close();
                            menu = 0;
                            g_modal = 0;
                            g_sms_open = -1;
                            invalidate_render_html_cache();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "theme"))     { g_theme = !g_theme; save_conf(); need_render = 1; }
                        else if (!strcmp(a, "revealkey")) { g_show_key = !g_show_key; need_render = 1; }
                        else if (!strcmp(a, "revealcell")) { g_show_cellid = !g_show_cellid; need_render = 1; }
                        else if (!strcmp(a, "revealimei")) { g_show_imei = !g_show_imei; need_render = 1; }
                        else if (!strcmp(a, "refreshambr")) {
                            system("(kill -USR1 $(pidof zwrt-datad 2>/dev/null) >/dev/null 2>&1) || true");
                            snprintf(g_toast, sizeof g_toast, "已请求刷新 AMBR");
                            g_toast_until = now + 1600;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "neighbors")) {
                            g_neighbor_open = !g_neighbor_open;
                            invalidate_render_html_cache();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "spunit"))    { g_speed_bits = !g_speed_bits; save_conf(); need_render = 1; }
                        else if (!strcmp(a, "batpct"))    { g_show_batpct = !g_show_batpct; save_conf(); need_render = 1; }
                        else if (!strcmp(a, "nfc")) {
                            static devui_data_t dd; char cmd[120];
                            int on = (data_refresh_live(&dd) && dd.nfc_switch) ? 0 : 1;
                            snprintf(cmd, sizeof cmd,
                                "ubus call zwrt_nfc zwrt_nfc_wifi_set '{\"switch\":%d,\"flag\":2}'", on);
                            system(cmd);
                            need_render = 1;
                        }
                        else if (!strcmp(a, "wifi")) {   /* master: both main bands (wlan0+wlan2) */
                            int on = (g_w24 == 1 || g_w5 == 1);
                            const char *ic = on ? "down" : "up";
                            char cmd[160];
                            snprintf(cmd, sizeof cmd,
                                "(ifconfig wlan0 %s; ifconfig wlan2 %s) >/dev/null 2>&1 &", ic, ic);
                            system(cmd);
                            g_w24 = g_w5 = !on;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "wifi24") || !strcmp(a, "wifi5")) {
                            int is24 = !strcmp(a, "wifi24");
                            int on = (is24 ? g_w24 : g_w5) == 1;
                            char cmd[120];
                            snprintf(cmd, sizeof cmd, "ifconfig %s %s >/dev/null 2>&1 &",
                                     is24 ? "wlan0" : "wlan2", on ? "down" : "up");
                            system(cmd);
                            if (is24) g_w24 = !on; else g_w5 = !on;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "dps")) {   /* power direct-supply mode */
                            int on = (g_dps == 1);
                            char cmd[160];
                            snprintf(cmd, sizeof cmd,
                                "ubus call zwrt_bsp.charger set '{\"direct_power_supply_mode\":\"%s\"}' >/dev/null 2>&1 &",
                                on ? "disable" : "enable");
                            system(cmd);
                            g_dps = !on;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "psm")) {
                            int turn_on = g_wpsm == 1 ? 0 : 1;   /* power_save on = battery saving */
                            if (wifi_psm_set_target(turn_on) == 0)
                                snprintf(g_toast, sizeof g_toast, "WiFi %s已开启", turn_on ? "节能模式" : "高性能模式");
                            else
                                snprintf(g_toast, sizeof g_toast, "WiFi 模式设置失败");
                            g_toast_until = now + 1800;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "adb")) {
                            static devui_data_t dd;
                            const char *mode = "debug";
                            if (data_refresh_live(&dd) && !strcmp(dd.usb_mode, "debug") &&
                                !usb_pid_is("90b1") && !usb_pid_is("90B1"))
                                mode = "user";
                            g_adb_pending = !strcmp(mode, "debug");
                            char cmd[160];
                            if (g_adb_pending)
                                snprintf(cmd, sizeof cmd,
                                    "/sbin/usb_composition 9059 n n y y >/dev/null 2>&1 &");
                            else
                                snprintf(cmd, sizeof cmd,
                                    "ubus call zwrt_bsp.usb set '{\"mode\":\"user\"}' >/dev/null 2>&1 &");
                            system(cmd);
                            need_render = 1;
                        }
                        else if (!strcmp(a, "usbpower")) {
                            if (g_typec_pending >= 0 && now < g_typec_until) {
                                need_render = 1;
                                goto action_done;
                            }
                            int cur = g_typec_source == 1;
                            int target = !cur;
                            int net_active = ((g_usb_net_pending >= 0 ? g_usb_net_pending : g_usb_net) == 1 ||
                                              usb_pid_is("9057") ||
                                              usb_pid_is("90b1") || usb_pid_is("90B1"));
                            if (net_active) {
                                system("touch /tmp/u60-usbnet-switching");
                                if (target) usb_net_apply_source_async();
                                else        usb_net_apply_sink_async();
                                usb_net_watchdog_start();
                                g_usb_net = 1;
                            } else {
                                usb_power_only_apply_async(target);
                            }
                            g_typec_source = target;
                            g_typec_pending = target;
                            g_typec_until = now + 20000;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "usbnet")) {
                            char cmd[900];
                            int cur = g_usb_net_pending >= 0 ? g_usb_net_pending :
                                      ((g_usb_net == 1) || usb_pid_is("9057") ||
                                       usb_pid_is("90b1") || usb_pid_is("90B1"));
                            int target = !cur;
                            if (g_usb_net_pending >= 0 && now < g_usb_net_until) {
                                need_render = 1;
                                goto action_done;
                            }
                            if (!target) {
                                snprintf(cmd, sizeof cmd,
                                    "(rm -f /tmp/u60-usbnet-switching; "
                                    "SER=$(cat /sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber 2>/dev/null); "
                                    "sh /sbin/usb/compositions/usb_switch 0x19d2 0x1225 mass_storage \"$SER\") >/dev/null 2>&1 &");
                                g_usb_net = 0;
                                g_usb_net_pending = 0;
                                g_usb_net_until = now + 20000;
                                usb_net_watchdog_stop();
                                system(cmd);
                            } else {
                                int want_source = g_typec_pending >= 0 ? g_typec_pending : (g_typec_source == 1);
                                g_usb_net = 1;
                                g_typec_source = want_source;
                                g_usb_net_pending = 1;
                                g_typec_pending = want_source;
                                g_usb_net_until = now + 20000;
                                g_typec_until = now + 20000;
                                system("touch /tmp/u60-usbnet-switching");
                                usb_net_watchdog_start();
                                if (want_source) usb_net_apply_source_async();
                                else             usb_net_apply_sink_async();
                            }
                            need_render = 1;
                        }
                        else if (!strcmp(a, "bright")) {    /* tap position on the bar = level */
                            int bx, by, bw, bh;
                            if (html_view_rect("#bright-bar", &bx, &by, &bw, &bh) && bw > 0) {
                                int frac = (x - bx) * 100 / bw;
                                if (frac < 3) frac = 3; if (frac > 100) frac = 100;
                                int bmax = backlight_max(); if (bmax <= 0) bmax = 255;
                                backlight_set(frac * bmax / 100);
                                save_conf();   /* persist brightness */
                            }
                            need_render = 1;
                        }
                        else if (!strncmp(a, "autooff:", 8)) {   /* preset select */
                            g_autooff_ms = atoi(a + 8); last_act = now; save_conf(); need_render = 1;
                        }
                        else if (!strncmp(a, "refreshms:", 10)) {   /* preset select */
                            g_refresh_ms = normalize_refresh_ms(atoi(a + 10));
                            (void)datad_set_refresh_ms(g_refresh_ms);
                            if (g_refresh_ms > 0) {
                                if (data_backend_commit_latest()) need_render = 1;
                                state_pending = 0;
                            }
                            last_data = 0;
                            save_conf();
                            need_render = 1;
                        }
                        else if (!strncmp(a, "chartsec:", 9)) {
                            char which[8];
                            int sec;
                            if (sscanf(a + 9, "%7[^:]:%d", which, &sec) == 2) {
                                sec = normalize_chart_sec(sec);
                                if (!strcmp(which, "cpu")) g_chart_cpu_sec = sec;
                                else if (!strcmp(which, "mem")) g_chart_mem_sec = sec;
                                else if (!strcmp(which, "net")) g_chart_net_sec = sec;
                                else if (!strcmp(which, "batt")) g_chart_batt_sec = sec;
                                else goto action_done;
                                save_conf();
                                invalidate_render_html_cache();
                                need_render = 1;
                            }
                        }
                        else if (!strcmp(a, "stpage")) {
                            if (subpage_open("speedtest.html")) {
                                menu = 0;
                                g_modal = 0;
                                g_sms_open = -1;
                                g_st_home_open = 0;
                            } else {
                                int pi = speedtest_page_index();
                                if (pi >= 0) {
                                    subpage_close_all();
                                    g_cur = pi;
                                    g_scroll = 0;
                                    menu = 0;
                                }
                            }
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "sttoggle")) {
                            if (speedtest_binary_ready())
                                g_st_home_open = !g_st_home_open;
                            else
                                g_st_home_open = 0;
                            subpage_close_all();
                            g_cur = 0;
                            g_scroll = 0;
                            menu = 0;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "stclose")) {
                            g_st_home_open = 0;
                            subpage_close_all();
                            g_cur = 0;
                            g_scroll = 0;
                            menu = 0;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "ststart")) {
                            speedtest_start();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "ststop")) {
                            speedtest_stop();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "tsstart") || !strcmp(a, "tsstop") ||
                                 !strcmp(a, "tsrestart") || !strcmp(a, "tsrefresh")) {
                            const struct plugin_candidate *pc = plugin_script_select(g_ts_candidates, ARRAY_LEN(g_ts_candidates), 1);
                            const char *verb = !strcmp(a, "tsstart") ? "start" :
                                               !strcmp(a, "tsstop") ? "stop" : "restart";
                            if (!strcmp(a, "tsrefresh")) {
                                plugin_status_refresh(CUR_PATH, 1);
                                plugin_action_note(TAILSCALE_ACTION_LOG, "手动刷新状态");
                                snprintf(g_toast, sizeof g_toast, "Tailscale 状态已刷新");
                            } else if (!g_ts_installed) {
                                snprintf(g_toast, sizeof g_toast, "Tailscale 尚未安装");
                            } else if (!pc) {
                                snprintf(g_toast, sizeof g_toast, "Tailscale 控制接口未初始化");
                            } else {
                                const char *label = !strcmp(verb, "start") ? "启动" :
                                                    !strcmp(verb, "stop") ? "停止" : "重启";
                                plugin_action_submit(TAILSCALE_ACTION_LOG, "sh ", pc->ctl, verb, label);
                                snprintf(g_toast, sizeof g_toast, "Tailscale %s已提交", label);
                                g_plugin_status_at = 0;
                            }
                            g_toast_until = now + 1800;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "mhstart") || !strcmp(a, "mhstop") ||
                                 !strcmp(a, "mhrestart") || !strcmp(a, "mhrefresh")) {
                            const struct plugin_candidate *pc = plugin_script_select(g_mh_candidates, ARRAY_LEN(g_mh_candidates), 1);
                            const char *verb = !strcmp(a, "mhstart") ? "start" :
                                               !strcmp(a, "mhstop") ? "stop" : "restart";
                            if (!strcmp(a, "mhrefresh")) {
                                plugin_status_refresh(CUR_PATH, 1);
                                plugin_action_note(MIHOMO_ACTION_LOG, "手动刷新状态");
                                snprintf(g_toast, sizeof g_toast, "Mihomo 状态已刷新");
                            } else if (!g_mh_installed) {
                                snprintf(g_toast, sizeof g_toast, "Mihomo 尚未安装");
                            } else if (!pc) {
                                snprintf(g_toast, sizeof g_toast, "Mihomo 控制接口未初始化");
                            } else {
                                const char *label = !strcmp(verb, "start") ? "启动" :
                                                    !strcmp(verb, "stop") ? "停止" : "重启";
                                plugin_action_submit(MIHOMO_ACTION_LOG, "sh ", pc->ctl, verb, label);
                                snprintf(g_toast, sizeof g_toast, "Mihomo %s已提交", label);
                                g_plugin_status_at = 0;
                            }
                            g_toast_until = now + 1800;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "wgstart") || !strcmp(a, "wgstop") ||
                                 !strcmp(a, "wgrestart") || !strcmp(a, "wgrefresh")) {
                            const struct plugin_candidate *pc = plugin_complete_select(g_wg_candidates, ARRAY_LEN(g_wg_candidates));
                            const char *verb = !strcmp(a, "wgstart") ? "start" :
                                               !strcmp(a, "wgstop") ? "stop" : "restart";
                            if (!strcmp(a, "wgrefresh")) {
                                plugin_status_refresh(CUR_PATH, 1);
                                plugin_action_note(WIREGUARD_ACTION_LOG, "手动刷新状态");
                                snprintf(g_toast, sizeof g_toast, "WireGuard 状态已刷新");
                            } else if (!g_wg_installed || !pc) {
                                snprintf(g_toast, sizeof g_toast, "WireGuard 尚未安装");
                            } else {
                                const char *label = !strcmp(verb, "start") ? "启动" :
                                                    !strcmp(verb, "stop") ? "停止" : "重启";
                                plugin_action_submit(WIREGUARD_ACTION_LOG, "sh ", pc->ctl, verb, label);
                                snprintf(g_toast, sizeof g_toast, "WireGuard %s已提交", label);
                                g_plugin_status_at = 0;
                            }
                            g_toast_until = now + 1800;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "fmrefresh")) {
                            (void)refresh_fm_status(now, 1);
                            plugin_action_note(FM_ACTION_LOG, "手动刷新飞猫分身卡状态");
                            snprintf(g_toast, sizeof g_toast, "%s",
                                     g_fm_available ? "飞猫分身卡状态已刷新" : "未检测到飞猫分身卡");
                            g_toast_until = now + 1800;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "fmswitch:", 9)) {
                            fm_switch_action(a + 9, now);
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "opselect:", 9)) {
                            const char *plmn = a + 9;
                            size_t len = strlen(plmn);
                            if ((len == 5 || len == 6) && strspn(plmn, "0123456789") == len) {
                                snprintf(g_op_selected, sizeof g_op_selected, "%s", plmn);
                                g_op_confirm_until = 0;
                                snprintf(g_toast, sizeof g_toast, "已选择运营商 %s", plmn);
                            } else snprintf(g_toast, sizeof g_toast, "运营商编号无效");
                            g_toast_until = now + 1800;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "oprefresh") || !strcmp(a, "opscan") ||
                                 !strcmp(a, "opapply") || !strcmp(a, "opauto") ||
                                 !strcmp(a, "opcancel")) {
                            const struct plugin_candidate *pc = operator_complete_select();
                            if (!pc) {
                                snprintf(g_toast, sizeof g_toast, "漫游锁卡插件尚未安装");
                            } else if (!strcmp(a, "oprefresh")) {
                                plugin_action_submit(OPERATOR_ACTION_LOG, "sh ", pc->ctl, "status", "刷新运营商状态");
                                snprintf(g_toast, sizeof g_toast, "状态刷新已提交");
                                g_plugin_status_at = 0;
                            } else if (!strcmp(a, "opscan")) {
                                plugin_action_submit(OPERATOR_ACTION_LOG, "sh ", pc->ctl, "scan-start", "扫描运营商");
                                snprintf(g_toast, sizeof g_toast, "运营商扫描已提交");
                                g_plugin_status_at = 0;
                            } else if (!strcmp(a, "opauto")) {
                                plugin_action_submit(OPERATOR_ACTION_LOG, "sh ", pc->ctl, "auto-start", "恢复自动选网");
                                snprintf(g_toast, sizeof g_toast, "恢复自动选网已提交");
                                g_op_confirm_until = 0;
                                g_plugin_status_at = 0;
                            } else if (!strcmp(a, "opcancel")) {
                                if (!g_op_job_running) snprintf(g_toast, sizeof g_toast, "当前没有运行中的任务");
                                else {
                                    plugin_action_submit(OPERATOR_ACTION_LOG, "sh ", pc->ctl, "cancel", "取消当前任务");
                                    snprintf(g_toast, sizeof g_toast, "取消任务已提交");
                                    g_plugin_status_at = 0;
                                }
                            } else if (!g_op_selected[0]) {
                                snprintf(g_toast, sizeof g_toast, "请先选择运营商");
                            } else if (!g_op_confirm_until || (int32_t)(g_op_confirm_until - now) <= 0) {
                                g_op_confirm_until = now + 4000;
                                snprintf(g_toast, sizeof g_toast, "请再次点击确认锁定 %s", g_op_selected);
                            } else {
                                char verb[128];
                                snprintf(verb, sizeof verb, "apply-start %s %s %s",
                                         g_op_selected, g_op_rat_pref, g_op_failure_policy);
                                plugin_action_submit(OPERATOR_ACTION_LOG, "sh ", pc->ctl, verb, "锁定运营商");
                                snprintf(g_toast, sizeof g_toast, "运营商锁定已提交");
                                g_op_confirm_until = 0;
                                g_plugin_status_at = 0;
                            }
                            g_toast_until = now + 1800;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "cpupowersave") || !strcmp(a, "cpubalance") ||
                                 !strcmp(a, "cpuperformance") || !strcmp(a, "cpuextreme") ||
                                 !strcmp(a, "cpurefresh")) {
                            if (!strcmp(a, "cpurefresh")) {
                                plugin_status_refresh(CUR_PATH, 1);
                                plugin_action_note(CPU_ACTION_LOG, "手动刷新状态");
                                snprintf(g_toast, sizeof g_toast, "CPU 状态已刷新");
                            } else if (!g_cpu_installed) {
                                snprintf(g_toast, sizeof g_toast, "CPU 控制器未安装");
                            } else {
                                const char *mode = !strcmp(a, "cpupowersave") ? "powersave" :
                                                   !strcmp(a, "cpubalance") ? "balance" :
                                                   !strcmp(a, "cpuperformance") ? "performance" : "extreme";
                                const char *label = !strcmp(mode, "powersave") ? "省电模式" :
                                                    !strcmp(mode, "balance") ? "均衡模式" :
                                                    !strcmp(mode, "performance") ? "性能模式" : "极致模式";
                                plugin_action_submit(CPU_ACTION_LOG, "sh ", cpu_ctl_path(), mode, label);
                                snprintf(g_toast, sizeof g_toast, "CPU %s已提交", label);
                                g_plugin_status_at = 0;
                            }
                            g_toast_until = now + 1800;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "simmoderefresh") ||
                                 !strcmp(a, "simmanage:single") || !strcmp(a, "simmanage:dual") ||
                                 !strcmp(a, "simsmart:0") || !strcmp(a, "simsmart:1")) {
                            if (!strcmp(a, "simmoderefresh")) {
                                (void)sim_slot_refresh(now, 1);
                                plugin_action_note(SIM_ACTION_LOG, "手动刷新双卡状态");
                                snprintf(g_toast, sizeof g_toast, "双卡状态已刷新");
                            } else if (!strcmp(a, "simmanage:single")) {
                                sim_mode_action(0, now);
                            } else if (!strcmp(a, "simmanage:dual")) {
                                sim_mode_action(1, now);
                            } else {
                                sim_auto_action(a[strlen(a) - 1] == '1', now);
                            }
                            g_toast_until = now + 1800;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "tzhour:", 7)) {
                            char *end = NULL;
                            long hour = strtol(a + 7, &end, 10);
                            if (end != a + 7 && !*end && hour >= -12 && hour <= 14) {
                                g_tz_draft_hour = (int)hour;
                                if (hour == -12 || hour == 14) g_tz_draft_minute = 0;
                                if (hour == 14) g_tz_draft_dst = 0;
                                g_tz_draft_dirty = 1;
                            }
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "tzminute:", 9)) {
                            int minute = atoi(a + 9);
                            if ((minute == 0 || minute == 15 || minute == 30 || minute == 45) &&
                                !((g_tz_draft_hour == -12 || g_tz_draft_hour == 14) && minute != 0)) {
                                g_tz_draft_minute = minute;
                                g_tz_draft_dirty = 1;
                            } else {
                                snprintf(g_toast, sizeof g_toast, "边界时区只能选择整点");
                                g_toast_until = now + 1800;
                            }
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "tzdst:", 6)) {
                            int enabled = a[6] == '1';
                            if (enabled && timezone_draft_offset() > 780) {
                                snprintf(g_toast, sizeof g_toast, "当前时区无法再加夏令时");
                                g_toast_until = now + 1800;
                            } else {
                                g_tz_draft_dst = enabled;
                                g_tz_draft_dirty = 1;
                            }
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "tzapply")) {
                            timezone_apply_action(now);
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "trafficselect:", 14)) {
                            int slot = atoi(a + 14);
                            if (slot == 1 ||
                                (slot == 2 && g_sim_dual == 1 &&
                                 (g_sim_p2 == 1 || g_sim_slot == 2))) {
                                g_plan_edit_slot = slot;
                                g_plan_draft_dirty = 0;
                                g_plan_draft_sim_id[0] = 0;
                            }
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "trafficenabled:", 15)) {
                            g_plan_draft_enabled = a[15] == '1';
                            if (g_plan_draft_enabled && g_plan_draft_gib == 0)
                                g_plan_draft_gib = 100;
                            g_plan_draft_dirty = 1;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "trafficgib:", 11)) {
                            long delta = strtol(a + 11, NULL, 10);
                            long long next = (long long)g_plan_draft_gib + delta;
                            if (next < 0) next = 0;
                            if (next > 16384) next = 16384;
                            g_plan_draft_gib = (unsigned long long)next;
                            g_plan_draft_dirty = 1;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "trafficday:", 11)) {
                            int delta = atoi(a + 11);
                            int next = g_plan_draft_reset_day + delta;
                            if (next < 1) next = 31;
                            if (next > 31) next = 1;
                            g_plan_draft_reset_day = next;
                            g_plan_draft_dirty = 1;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "trafficapply")) {
                            traffic_plan_apply_action(now);
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "stsrc:", 6)) {
                            snprintf(g_st_src, sizeof g_st_src, "%s", speedtest_norm_src(a + 6));
                            save_conf();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "stdir:", 6)) {
                            snprintf(g_st_dir, sizeof g_st_dir, "%s", speedtest_norm_dir(a + 6));
                            save_conf();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "stdur:", 6)) {
                            g_st_dur = speedtest_norm_dur(atoi(a + 6));
                            save_conf();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "locktoggle")) {   /* on->off clears PIN; off->on opens setup pad */
                            if (lock_enabled()) clear_pin();
                            else                enter_lock(1);
                            need_render = 1;
                        }
                        else if (!strncmp(a, "net:", 4)) {   /* segmented control tap fallback */
                            char cmd[160];
                            snprintf(cmd, sizeof cmd,
                                "ubus call zte_nwinfo_api nwinfo_set_netselect '{\"net_select\":\"%s\"}' >/dev/null 2>&1 &", a + 4);
                            system(cmd);
                            snprintf(g_net_pending, sizeof g_net_pending, "%s", a + 4);
                            snprintf(g_toast, sizeof g_toast, "网络模式已切换"); g_toast_until = now + 1600;
                        }
                        else if (!strncmp(a, "simslot:", 8)) {
                            sim_slot_action(atoi(a + 8), now);
                            need_render = 1;
                        }
                        else if (!strncmp(a, "openmodal:", 10)) {   /* open band-lock dialog */
                            const char *r = a + 10;
                            if (!strcmp(r, "sig")) g_modal = 4;
                            else g_modal = !strcmp(r, "sa") ? 1 : !strcmp(r, "nsa") ? 2 : 3;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "sms:", 4)) {   /* open SMS detail + mark read */
                            const char *id_txt = a + 4;
                            char *endp = NULL;
                            long sid = strtol(id_txt, &endp, 10);
                            static devui_data_t sd;
                            g_sms_open = -1;
                            g_sms_scroll = g_sms_scroll_max = 0;
                            g_sms_view_x = g_sms_view_y = g_sms_view_w = g_sms_view_h = 0;
                            g_sms_num[0] = g_sms_date[0] = g_sms_text[0] = 0;
                            need_render = 1;
                            if (id_txt != endp && endp && *endp == '\0' && data_refresh(&sd)) {
                                if (sid > 0) {
                                    for (int i = 0; i < sd.sms_n; i++) {
                                        if (sd.sms[i].id != sid) continue;
                                        snprintf(g_sms_num,  sizeof g_sms_num,  "%s", sd.sms[i].num);
                                        snprintf(g_sms_date, sizeof g_sms_date, "%s", sd.sms[i].date);
                                        snprintf(g_sms_text, sizeof g_sms_text, "%s", sd.sms[i].text);
                                        g_sms_open = sid;
                                        if (sd.sms[i].unread && sd.sms[i].id > 0) {
                                            char cmd[128];
                                            snprintf(cmd, sizeof cmd,
                                                "ubus call zwrt_wms zwrt_wms_modify_tag '{\"id\":\"%ld;\",\"tag\":0}' >/dev/null 2>&1 &",
                                                sd.sms[i].id);
                                            system(cmd);
                                        }
                                        need_render = 1;
                                        break;
                                    }
                                }
                            }
                        }
                        else if (!strcmp(a, "resetband")) {
                            system("ubus call zte_nwinfo_api nwinfo_reset_band_cell_setting '{}' >/dev/null 2>&1 &");
                            snprintf(g_toast, sizeof g_toast, "锁频已恢复默认"); g_toast_until = now + 1600;
                            need_render = 1;   /* selection re-syncs from the live lock automatically */
                        }
                    }
                }
action_done:
                if (!replay_release)
                    touch_input_drop_replayed_release(&touch, release_was_tap);
                last_motion_frame = 0;
                dragging = 0; drag_dir = 0; back_drag = 0; quick_drag = 0; quick_prepared = 0; scroll_dir = 0; sliding = 0; segging = 0;
                scroll_track_valid = 0;
            }
        }
        prev_press = pressed;

        /* when the lock pad first appears, drop taps that were queued before it
         * (e.g. the toggle tap that opened it) so they don't phantom-press keys */
        if (g_lock_state != prev_lock) touch_input_clear_taps(&touch);
        prev_lock = g_lock_state;

        /* dismiss the toast after its timeout */
        if (g_toast[0] && now >= g_toast_until) { g_toast[0] = 0; need_render = 1; }

        /* power-menu armed state auto-reverts if the second tap doesn't come */
        if (g_pwr_confirm && now >= g_pwr_until) { g_pwr_confirm = 0; if (menu) need_render = 1; }

        /* SIM target confirmation uses the same four-second safety window. */
        if (g_sim_confirm_slot && (int32_t)(now - g_sim_confirm_until) >= 0) {
            g_sim_confirm_slot = 0;
            g_sim_confirm_until = 0;
            if (path_is_lock_page(CUR_PATH)) need_render = 1;
        }

        /* auto screen-off after the configured idle timeout (locks if enabled) */
        if (g_autooff_ms > 0 && backlight_is_on() && now - last_act >= (uint32_t)g_autooff_ms) {
            screen_off(&disp);
            if (lock_enabled()) {
                if (ext_ok) devui_ext_deactivate(&ext);
                enter_lock(0);
            }
        }

        /* periodic clock/state check (not mid-drag). refresh=0 pauses promotion of
         * live backend updates, but the minute clock still ticks once per second. */
        {
            uint32_t poll_ms = g_refresh_ms > 0 ? (uint32_t)g_refresh_ms : 1000;
            int keep_fast = (g_npages > 0 && !strcmp(CUR_PATH, g_pages[0])) ||
                            path_is_speedtest(CUR_PATH) || g_st_home_open;
            if (plugin_status_page(CUR_PATH))
                poll_ms = plugin_page_named(CUR_PATH, "operator-lock.html") && !g_op_job_running ? 10000 : 2000;
            if (!plugin_status_page(CUR_PATH) && !keep_fast && g_refresh_ms > 0 && g_refresh_ms < 2000 && now - last_act >= 10000)
                poll_ms = 2000;
            if (!dragging && !scroll_inertia && now - last_data >= poll_ms) {
                time_t now_t = time(NULL);
                int state_changed = 0;
                int clock_changed = 0;
                int state_render = 0;
                int cur_min = (int)(now_t / 60);
                if (cur_min != g_last_clock_min) {
                    g_last_clock_min = cur_min;
                    clock_changed = 1;
                    need_render = 1;   /* update HH:MM and any minute-level derived tokens */
                }

                if (g_refresh_ms > 0 && state_pending && data_backend_commit_latest()) {
                    state_changed = 1;
                    state_render = 1;
                    state_pending = 0;
                }
                if (g_refresh_ms > 0 && signal_live_enabled() && path_is_signal_page(CUR_PATH))
                    state_render = 1;
                if (plugin_status_page(CUR_PATH)) state_render = 1;
                if (state_render) need_render = 1;
                if (ext_ok && devui_ext_active(&ext) && !g_lock_state &&
                    (clock_changed || (g_refresh_ms > 0 && state_changed)))
                    refresh_status_cache();
                last_data = now;
            }
        }

        /* reconcile page-2 WiFi switch states from uci/iw on a slow throttle */
        if (!dragging && !scroll_inertia && now - g_wifi_aux_at >= 4000) { g_wifi_aux_at = now; wifi_aux_refresh(); }

        /* SIM polling is page-local and pauses during gestures and while dark. */
        if (backlight_is_on() &&
            (path_is_lock_page(CUR_PATH) || plugin_page_named(CUR_PATH, "sim-switch.html") ||
             plugin_page_named(CUR_PATH, "sim-traffic.html")) &&
            !dragging && !scroll_inertia) {
            if (sim_slot_refresh(now, 0)) need_render = 1;
        }

        if (!g_charge_boot && now - g_pages_scan_at >= 1000) {
            g_pages_scan_at = now;
            if (rescan_pages_if_changed()) need_render = 1;
        }

        int st_poll_ms = (g_st_state == ST_STATE_RUNNING || g_st_home_open) ? 300 : 900;
        if (!dragging && !scroll_inertia && now - g_st_poll_at >= (uint32_t)st_poll_ms) {
            int st_changed = speedtest_poll(now);
            if (!g_lock_state && (path_is_speedtest(CUR_PATH) || g_st_home_open) && (st_changed || g_st_state == ST_STATE_RUNNING))
                need_render = 1;
        }

        if (need_render && backlight_is_on()) {
            if (ext_ok && devui_ext_active(&ext) && !g_lock_state)
                render_ext_view(&disp, &ext);
            else
                render(&disp, CUR_PATH);
        }
        if (animating) {
            interaction_pulse(now);
            g_gesture_used_at = now;
        }
        if (g_boosted && (int32_t)(now - g_boost_until) >= 0)
            set_interaction_boost(0);
        if (!dragging && !scroll_inertia && !g_modal && g_sms_open < 0 && !segging &&
            g_gesture_used_at && now - g_gesture_used_at >= GESTURE_CACHE_TTL_MS) {
            release_gesture_caches();
            g_gesture_used_at = 0;
        }
        was_on = backlight_is_on();   /* track for next frame's wake-touch discard */
        if (!animating) {
            if (scroll_inertia && backlight_is_on()) {
                uint32_t sleep_now = millis();
                uint32_t wait_ms = (int32_t)(inertia_frame_at - sleep_now) > 0 ?
                                   inertia_frame_at - sleep_now : 1;
                if (wait_ms > IDLE_SLEEP_ON_US / 1000) wait_ms = IDLE_SLEEP_ON_US / 1000;
                usleep(wait_ms * 1000);
            } else {
                usleep(backlight_is_on() ? IDLE_SLEEP_ON_US : IDLE_SLEEP_OFF_US);
            }
        }
    }

    if (ext_ok) devui_ext_close(&ext);
    signal_async_stop();
    data_backend_close();
    drm_disp_close(&disp);
    return 0;
}
