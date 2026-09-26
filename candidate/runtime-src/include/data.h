/*
 * data.h - device state consumed from the zwrt-datad backend.
 *
 * Reads the zwrt-datad HTTP/SSE backend snapshot.
 * The GUI never calls ubus directly. If the backend isn't running, refresh
 * returns 0 and the UI shows placeholders.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_DATA_H
#define U60PRO_DATA_H

#include <stddef.h>
#include <stdint.h>

#define DEVUI_SMS_MAX 32
#define DEVUI_SMS_TEXT_MAX 16384
#define DEVUI_SIM_TRAFFIC_MAX 16

typedef struct {
    char id[24];
    int slot;
    char iccid_tail[12];
    char operator_name[48];
    int active;
    uint64_t today_rx_bytes, today_tx_bytes, today_bytes;
    uint64_t cycle_rx_bytes, cycle_tx_bytes, cycle_bytes;
    uint64_t lifetime_bytes;
    int package_enabled;
    uint64_t allowance_bytes;
    int remaining_valid;
    uint64_t remaining_bytes;
    int reset_day;
    int64_t cycle_start, cycle_end, last_seen;
} devui_sim_traffic_t;

typedef struct {
    int  valid;

    /* network */
    char net_type[16];
    int  bars;
    char operator_name[48];
    char band[16];
    char nr_band[16];
    int  nr_rsrp, nr_rsrq, nr_rssi;
    char nr_snr[12];
    int  lte_rsrp, lte_rsrq, lte_rssi;
    char lte_snr[12];
    int  rssi, mcc, mnc, nr_pci;
    long nr_cell_id, nr_channel;
    char nr_bw[12];
    char nrca[256], lteca[256], ltecasig[256];
    char wan_status[32];
    char net_select[16];
    int  hsr;
    char sa_bands[256], nsa_bands[256], lte_bands[256];

    /* battery */
    int  bat_percent, bat_temp, charging, charger_connect;
    long chg_uv, chg_ua, bat_uv, bat_ua;   /* charger/battery voltage(µV)/current(µA) */

    /* clients */
    int  clients_total, clients_wifi, clients_lan;
    struct { char name[40], ip[24], mac[20]; } client[16];
    int  client_n;

    /* sms (read-only) */
    int  sms_unread;
    struct { long id; char num[40], date[16], text[DEVUI_SMS_TEXT_MAX]; int unread; } sms[DEVUI_SMS_MAX];
    int  sms_n;

    /* wifi (main SSID) */
    char wifi_ssid[64], wifi_key[64], wifi_enc[24];
    int  wifi_enabled;

    /* nfc tap-to-share */
    int  nfc_switch;

    /* dhcp / lan */
    char dhcp_ip[24], dhcp_start[24], dhcp_limit[8], dhcp_leasetime[12];

    /* traffic (bytes, bytes/s) */
    long rx_speed, tx_speed, rx_bytes, tx_bytes;

    /* Shared fixed-offset timezone and persistent per-SIM traffic. */
    int timezone_available;
    int timezone_offset_minutes, timezone_dst_minutes, timezone_effective_minutes;
    int timezone_clock_basis_available;
    int timezone_system_offset_minutes, timezone_clock_adjust_minutes;
    char timezone_label[24];
    int sim_traffic_available;
    char sim_traffic_active_id[24];
    devui_sim_traffic_t sim_traffic[DEVUI_SIM_TRAFFIC_MAX];
    int sim_traffic_n;

    /* qos (parsed from modem key.log by the backend) */
    int    qci;
    double ambr_dl, ambr_ul;   /* Mbps */
    char   usb_mode[16];       /* "user" = adb off, "debug" = adb on */

    /* system */
    long uptime, cpu_temp, cpu_usage, mem_used_pct, mem_total, mem_avail;
    char model[64], fw[80], sw_version[80], imei[24];
} devui_data_t;

/* Start the backend transport and seed the first visible snapshot if available. */
int  data_backend_init(void);

/* Drain SSE events and reconnect as needed. Returns 1 when the live snapshot changed. */
int  data_backend_poll(uint32_t now_ms);

/* Promote the latest live snapshot to the UI-visible snapshot. */
int  data_backend_commit_latest(void);

/* Drop transport/parser working memory while the panel is dark, then rebuild it
 * and fetch a fresh snapshot on wake. The last committed UI snapshot is kept. */
void data_backend_suspend(void);
int  data_backend_resume(void);

/* Close the transport socket. Safe to call during shutdown. */
void data_backend_close(void);

/* Copy the current UI-visible snapshot. Returns 1 on success. */
int data_refresh(devui_data_t *d);

/* Copy the latest live snapshot even if UI-visible refresh is paused. */
int data_refresh_live(devui_data_t *d);

/* Fetch the compact cached chart snapshot without restoring the suspended SSE
 * transport or its large parser buffers. Intended for low-power dark-screen
 * sampling; returns 1 only when a complete sample was received. */
int data_chart_metrics(devui_data_t *d);

/* Small synchronous loopback request used by fixed DevUI control actions. */
int data_backend_request(const char *method, const char *path, char *body, size_t body_cap);

#endif /* U60PRO_DATA_H */
