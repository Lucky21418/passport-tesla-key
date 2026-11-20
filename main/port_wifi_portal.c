#include "port_internal.h"
#include "pk_protocol.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <ctype.h>
#include <sys/select.h>
#include <string.h>

/* ==========================================================================
 * 3. SoftAP + HTTP VIN Entry Portal (pk_softap_port_t)
 *    Only active during PK_PAGE_VIN (120 s max). Never always-on.
 * ========================================================================== */

static const char *TAG = "port_wifi";

static bool s_wifi_inited;
static bool s_ap_running;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_httpd;
static char s_submit_buf[64];
static uint8_t s_dns_rx[256];
static uint8_t s_dns_tx[300];
static volatile bool s_dns_running;
static SemaphoreHandle_t s_dns_done_sem;

static void dns_portal_task(void *arg)
{
    int sock;
    struct sockaddr_in addr;
    struct sockaddr_in cli;
    socklen_t cli_len;

    (void)arg;
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        if (s_dns_done_sem) {
            xSemaphoreGive(s_dns_done_sem);
        }
        vTaskDelete(NULL);
        return;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        if (s_dns_done_sem) {
            xSemaphoreGive(s_dns_done_sem);
        }
        vTaskDelete(NULL);
        return;
    }

    while (s_dns_running) {
        fd_set rfds;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        int ready;
        int n;
        size_t pos;

        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        ready = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) {
            continue;
        }
        cli_len = sizeof(cli);
        n = recvfrom(sock, s_dns_rx, sizeof(s_dns_rx), 0, (struct sockaddr *)&cli, &cli_len);
        if (n < 12 || n > 200 || (s_dns_rx[2] & 0x80u) != 0) {
            continue;
        }
        pos = 12;
        while (pos < (size_t)n && s_dns_rx[pos] != 0) {
            uint8_t l = s_dns_rx[pos];
            if ((l & 0xC0u) != 0 || pos + 1 + l >= (size_t)n) {
                pos = 0;
                break;
            }
            pos += 1 + l;
        }
        if (pos == 0 || pos + 5 > (size_t)n) {
            continue;
        }
        /* Answer only Class IN(1), Type A(1) or ANY(255) */
        uint16_t qtype = ((uint16_t)s_dns_rx[pos + 1] << 8) | s_dns_rx[pos + 2];
        uint16_t qclass = ((uint16_t)s_dns_rx[pos + 3] << 8) | s_dns_rx[pos + 4];
        if ((qtype != 1u && qtype != 255u) || qclass != 1u) {
            continue;
        }
        size_t q_end = pos + 5;
        memcpy(s_dns_tx, s_dns_rx, q_end);
        s_dns_tx[2] = 0x81; /* QR=1, RD=1 */
        s_dns_tx[3] = 0x80; /* RA=1, RCODE=0 */
        s_dns_tx[6] = 0x00; s_dns_tx[7] = 0x01; /* ANCOUNT = 1 */
        s_dns_tx[8] = 0x00; s_dns_tx[9] = 0x00;
        s_dns_tx[10] = 0x00; s_dns_tx[11] = 0x00;
        static const uint8_t ans[16] = {
            0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
            192, 168, 4, 1
        };
        memcpy(s_dns_tx + q_end, ans, sizeof(ans));
        (void)sendto(sock, s_dns_tx, q_end + sizeof(ans), 0, (struct sockaddr *)&cli, cli_len);
    }
    close(sock);
    if (s_dns_done_sem) {
        xSemaphoreGive(s_dns_done_sem);
    }
    vTaskDelete(NULL);
}

static esp_err_t http_404_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t http_favicon(httpd_req_t *req)
{
    /* Browsers request it automatically; answer without a redirect. */
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static const char k_portal_html[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Passport 钥匙录入</title>"
    "<style>"
    "body{background:#050507;color:#fff;font-family:-apple-system,sans-serif;margin:0;padding:20px;}"
    ".card{background:#0C0D12;border:1px solid #181B24;border-radius:8px;padding:18px;max-width:360px;margin:0 auto;}"
    "h2{margin:0 0 8px;font-size:18px;}"
    "p{color:#8A90A2;font-size:13px;line-height:1.5;margin:0 0 14px;}"
    "input{width:100%;box-sizing:border-box;background:#11131C;border:1px solid #8A90A2;color:#fff;"
    "font-family:monospace;font-size:16px;padding:10px;border-radius:4px;text-transform:uppercase;margin-bottom:12px;}"
    "button{width:100%;background:#fff;color:#050507;border:0;font-weight:700;font-size:15px;padding:11px;border-radius:4px;cursor:pointer;margin-bottom:8px;}"
    "</style></head><body><div class='card'>"
    "<h2>录入 17 位车架号 (VIN)</h2>"
    "<p>仅保存在本机闪存，用于计算车辆蓝牙广播标识与签名验证，不含 I / O / Q。</p>"
    "<form method='POST' action='/vin'>"
    "<input id='v' name='vin' maxlength='17' minlength='17' placeholder='输入 17 位 VIN' required>"
    "<button type='submit'>提交并进入卡片配对</button>"
    "</form>"
    "</div></body></html>";

static esp_err_t http_get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, k_portal_html, HTTPD_RESP_USE_STRLEN);
}

static void url_decode_vin(const char *src, char *dst, size_t dst_cap)
{
    const char *p = strstr(src, "vin=");
    size_t i = 0;
    if (p) {
        p += 4;
    } else {
        p = src;
    }
    while (*p && *p != '&' && i + 1 < dst_cap) {
        if (*p == '+') {
            dst[i++] = ' ';
            p++;
        } else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], '\0' };
            dst[i++] = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            dst[i++] = *p++;
        }
    }
    dst[i] = '\0';
}

static esp_err_t http_post_vin(httpd_req_t *req)
{
    char raw[96];
    char clean_vin[PK_VIN_BUF];
    int total = 0;
    port_event_t ev;
    esp_err_t send_rc;

    if (req->content_len <= 0 || req->content_len >= (int)sizeof(raw)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid length");
        return ESP_OK;
    }
    while (total < req->content_len) {
        int n = httpd_req_recv(req, raw + total, req->content_len - total);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete body");
            return ESP_OK;
        }
        total += n;
    }
    raw[total] = '\0';
    url_decode_vin(raw, s_submit_buf, sizeof(s_submit_buf));
    memset(raw, 0, sizeof(raw));

    if (!pk_vin_valid(s_submit_buf, clean_vin)) {
        memset(s_submit_buf, 0, sizeof(s_submit_buf));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_sendstr(
            req,
            "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<body style='background:#050507;color:#fff;font-family:sans-serif;padding:24px;text-align:center'>"
            "<h3>VIN 格式无效</h3><p style='color:#8A90A2'>须为 17 位字母数字（不含 I / O / Q），请返回重试。</p></body></html>");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    send_rc = httpd_resp_sendstr(
        req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<body style='background:#050507;color:#fff;font-family:sans-serif;padding:24px;text-align:center'>"
        "<h3>已提交至随身钥匙</h3><p style='color:#8A90A2'>热点即将关闭，请回到钥匙屏幕按提示完成配对。</p></body></html>");

    memset(&ev, 0, sizeof(ev));
    ev.type = PORT_EV_HTTP_VIN_SUBMIT;
    memcpy(ev.u.vin, clean_vin, PK_VIN_BUF);
    ev.len = PK_VIN_LEN;
    memset(clean_vin, 0, sizeof(clean_vin));
    memset(s_submit_buf, 0, sizeof(s_submit_buf));
    (void)port_post_event(&ev);
    return send_rc;
}

static pk_err_t softap_port_start(void *ctx, pk_ms_t deadline)
{
    wifi_config_t wcfg;
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    esp_err_t err;

    (void)ctx;
    (void)deadline;
    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "esp_netif_init: %s", esp_err_to_name(err));
            return PK_ERR_NOT_WIRED;
        }
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
            return PK_ERR_NOT_WIRED;
        }
        if (!s_ap_netif) {
            s_ap_netif = esp_netif_create_default_wifi_ap();
            if (!s_ap_netif) {
                return PK_ERR_NOT_WIRED;
            }
        }
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
            return PK_ERR_NOT_WIRED;
        }
        (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);
        s_wifi_inited = true;
    }

    memset(&wcfg, 0, sizeof(wcfg));
    strcpy((char *)wcfg.ap.ssid, "PASSPORT-KEY");
    wcfg.ap.ssid_len = (uint8_t)strlen("PASSPORT-KEY");
    wcfg.ap.channel = 1;
    wcfg.ap.max_connection = 2;
    wcfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_LOGI(TAG, "heap pre-SoftAP: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &wcfg) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi AP start failed");
        return PK_ERR_NOT_WIRED;
    }
    pk_proto_set_entropy_source(PK_ENTROPY_SRC_WIFI, true);
    s_ap_running = true;
    ESP_LOGI(TAG, "heap post-SoftAP: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (!s_httpd) {
        hcfg.server_port = 80;
        hcfg.lru_purge_enable = true;
        if (httpd_start(&s_httpd, &hcfg) == ESP_OK) {
            static const httpd_uri_t u_root = {
                .uri = "/", .method = HTTP_GET, .handler = http_get_root
            };
            static const httpd_uri_t u_vin = {
                .uri = "/vin", .method = HTTP_POST, .handler = http_post_vin
            };
            httpd_uri_t u_favicon = {
                .uri = "/favicon.ico", .method = HTTP_GET, .handler = http_favicon
            };
            (void)httpd_register_uri_handler(s_httpd, &u_favicon);
            (void)httpd_register_uri_handler(s_httpd, &u_root);
            (void)httpd_register_uri_handler(s_httpd, &u_vin);
            (void)httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, http_404_redirect);
        } else {
            ESP_LOGW(TAG, "httpd_start failed");
        }
    }
    if (!s_dns_done_sem) {
        s_dns_done_sem = xSemaphoreCreateBinary();
    }
    if (!s_dns_running && s_dns_done_sem) {
        (void)xSemaphoreTake(s_dns_done_sem, 0);
        s_dns_running = true;
        if (xTaskCreate(dns_portal_task, "dns_portal", 3584, NULL, 3, NULL) != pdPASS) {
            s_dns_running = false;
            ESP_LOGW(TAG, "dns_portal task create failed");
        }
    }
    return PK_OK;
}

static pk_err_t softap_port_stop(void *ctx, const char *reason)
{
    (void)ctx;
    (void)reason;
    if (s_dns_running) {
        s_dns_running = false;
        if (s_dns_done_sem) {
            (void)xSemaphoreTake(s_dns_done_sem, pdMS_TO_TICKS(300));
        }
    }
    if (s_httpd) {
        (void)httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_ap_running) {
        (void)esp_wifi_stop();
        pk_proto_set_entropy_source(PK_ENTROPY_SRC_WIFI, false);
        s_ap_running = false;
    }
    if (s_wifi_inited) {
        (void)esp_wifi_deinit();
        s_wifi_inited = false;
    }
    return PK_OK;
}

static void softap_port_wipe(void *ctx)
{
    (void)ctx;
    memset(s_submit_buf, 0, sizeof(s_submit_buf));
}

static const pk_softap_port_t k_softap_port = {
    .start = softap_port_start,
    .stop = softap_port_stop,
    .wipe_submit_buffer = softap_port_wipe,
};


const pk_softap_port_t *port_softap_ops(void)
{
    return &k_softap_port;
}
