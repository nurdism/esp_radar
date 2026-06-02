#include "flight_data.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "flight_data";

/* adsb.lol responses for a busy area are tens of KB; cap to keep memory bounded. */
#define RESPONSE_MAX_BYTES   (128 * 1024)
#define HTTP_TIMEOUT_MS      10000

/* Response accumulator passed to the HTTP event handler. */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    response_t *r = evt->user_data;
    if (!r || !r->buf) {
        return ESP_OK;
    }
    size_t room = r->cap - r->len - 1;          /* keep one byte for NUL */
    size_t n = evt->data_len < room ? evt->data_len : room;
    if (n) {
        memcpy(r->buf + r->len, evt->data, n);
        r->len += n;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

/* Copy @src into @dst[dst_sz], trimming trailing spaces. */
static void copy_trim(char *dst, size_t dst_sz, const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_sz);
    for (int i = (int)strlen(dst) - 1; i >= 0 && dst[i] == ' '; i--) {
        dst[i] = '\0';
    }
}

static void parse_aircraft(const cJSON *ac, aircraft_t *out)
{
    memset(out, 0, sizeof(*out));
    out->altitude_ft = -1;

    const cJSON *flight = cJSON_GetObjectItemCaseSensitive(ac, "flight");
    if (cJSON_IsString(flight) && flight->valuestring[0]) {
        copy_trim(out->callsign, sizeof(out->callsign), flight->valuestring);
    } else {
        const cJSON *hex = cJSON_GetObjectItemCaseSensitive(ac, "hex");
        copy_trim(out->callsign, sizeof(out->callsign),
                  cJSON_IsString(hex) ? hex->valuestring : "----");
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(ac, "t");
    copy_trim(out->type, sizeof(out->type), cJSON_IsString(type) ? type->valuestring : "");

    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(ac, "lat");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(ac, "lon");
    if (cJSON_IsNumber(lat)) out->lat = lat->valuedouble;
    if (cJSON_IsNumber(lon)) out->lon = lon->valuedouble;

    const cJSON *track = cJSON_GetObjectItemCaseSensitive(ac, "track");
    if (cJSON_IsNumber(track)) out->track = (float)track->valuedouble;

    const cJSON *gs = cJSON_GetObjectItemCaseSensitive(ac, "gs");
    if (cJSON_IsNumber(gs)) out->ground_speed = (float)gs->valuedouble;

    /* alt_baro is a number in feet, or the string "ground". */
    const cJSON *alt = cJSON_GetObjectItemCaseSensitive(ac, "alt_baro");
    if (cJSON_IsNumber(alt)) {
        out->altitude_ft = alt->valueint;
    } else if (cJSON_IsString(alt) && strcmp(alt->valuestring, "ground") == 0) {
        out->on_ground = true;
    }
}

esp_err_t flight_data_fetch(double lat, double lon, int radius_nm,
                            aircraft_t *out, size_t max, size_t *out_count)
{
    *out_count = 0;

    char url[96];
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.5f/%.5f/%d",
             lat, lon, radius_nm);

    response_t resp = {
        .buf = heap_caps_malloc(RESPONSE_MAX_BYTES, MALLOC_CAP_SPIRAM),
        .len = 0,
        .cap = RESPONSE_MAX_BYTES,
    };
    if (!resp.buf) {
        return ESP_ERR_NO_MEM;
    }
    resp.buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        heap_caps_free(resp.buf);
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request failed: %s", esp_err_to_name(err));
        heap_caps_free(resp.buf);
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d", status);
        heap_caps_free(resp.buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    heap_caps_free(resp.buf);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed");
        return ESP_FAIL;
    }

    const cJSON *ac_array = cJSON_GetObjectItemCaseSensitive(root, "ac");
    size_t n = 0;
    const cJSON *ac = NULL;
    cJSON_ArrayForEach(ac, ac_array) {
        if (n >= max) {
            break;
        }
        /* Skip entries with no position. */
        const cJSON *lat_item = cJSON_GetObjectItemCaseSensitive(ac, "lat");
        if (!cJSON_IsNumber(lat_item)) {
            continue;
        }
        parse_aircraft(ac, &out[n]);
        n++;
    }
    cJSON_Delete(root);

    *out_count = n;
    return ESP_OK;
}
