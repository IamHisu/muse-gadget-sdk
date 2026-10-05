/* Local Hisu setup page and Wi-Fi wallpaper transport. */
#include "muse_web.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "muse_ble.h"
#include "muse_ui.h"

#define COMMAND_MAX 600
#define STILL_BYTES (240 * 240 * 2)
#define GIF_MAX (1024 * 1024)

static const char *TAG = "muse_web";
static httpd_handle_t s_server;

extern const unsigned char hisu_setup_html_start[] asm("_binary_ble_setup_html_start");
extern const unsigned char hisu_setup_html_end[] asm("_binary_ble_setup_html_end");

static void no_cache(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t send_text(httpd_req_t *req, const char *type, const char *text)
{
    no_cache(req);
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_get(httpd_req_t *req)
{
    no_cache(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)hisu_setup_html_start,
                           hisu_setup_html_end - hisu_setup_html_start);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char json[640];
    muse_ble_status_json(json, sizeof(json));
    return send_text(req, "application/json", json);
}

static esp_err_t networks_get(httpd_req_t *req)
{
    char json[640];
    muse_ble_networks_json(json, sizeof(json));
    return send_text(req, "application/json", json);
}

static esp_err_t command_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > COMMAND_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid command");
    }
    char *command = malloc((size_t)req->content_len + 1);
    if (!command) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, command + received, req->content_len - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(command);
            return ESP_FAIL;
        }
        received += n;
    }
    command[received] = '\0';
    muse_ble_command(command);
    free(command);
    return send_text(req, "text/plain", "ok");
}

static esp_err_t wallpaper_post(httpd_req_t *req)
{
    char query[32] = {0};
    char kind[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "type", kind, sizeof(kind)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing image type");
    }
    bool gif = strcmp(kind, "gif") == 0;
    bool still = strcmp(kind, "still") == 0;
    if ((!gif && !still) || req->content_len <= 0 ||
        (still && req->content_len != STILL_BYTES) ||
        (gif && req->content_len > GIF_MAX)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid image");
    }

    size_t size = (size_t)req->content_len;
    uint8_t *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not enough memory");
    }
    size_t received = 0;
    while (received < size) {
        int n = httpd_req_recv(req, (char *)data + received, size - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            heap_caps_free(data);
            return ESP_FAIL;
        }
        received += (size_t)n;
    }
    if (!muse_ui_wallpaper_submit(data, size, gif)) {
        heap_caps_free(data);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wallpaper rejected");
    }
    ESP_LOGI(TAG, "wallpaper received over Wi-Fi: %u bytes (%s)",
             (unsigned)size, gif ? "gif" : "still");
    return send_text(req, "text/plain", "shown");
}

esp_err_t muse_web_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "server start failed: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get },
        { .uri = "/index.html", .method = HTTP_GET, .handler = root_get },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get },
        { .uri = "/api/networks", .method = HTTP_GET, .handler = networks_get },
        { .uri = "/api/command", .method = HTTP_POST, .handler = command_post },
        { .uri = "/api/wallpaper", .method = HTTP_POST, .handler = wallpaper_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(s_server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "route %s failed: %s", routes[i].uri, esp_err_to_name(err));
            httpd_stop(s_server);
            s_server = NULL;
            return err;
        }
    }
    ESP_LOGI(TAG, "Hisu setup available at http://<board-ip>/");
    return ESP_OK;
}
