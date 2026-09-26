// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// c6-rcp-installer: one-off P4 app for the Guition JC-ESP32P4-M3-DEV.
// Replaces the on-module C6's factory ESP-Hosted slave with ot_rcp, over the
// existing SDIO link, via ESP-Hosted slave OTA. Then proves the C6 answers
// spinel on the traces zhac-wired-core uses (P4 TX15/RX14 @460800).
//
// Flow (safety first; every failure stops before the next step):
//   1. validate the embedded C6 app image (magic, chip id, app desc, hashes)
//   2. connect to the hosted slave; if that fails, see whether the C6 already
//      speaks spinel (already installed -> nothing to do), else stop
//   3. log the slave's version / app description
//   4. ota_begin / write / end -> activate
//   5. drop esp_hosted, reset the C6 (GPIO54), query spinel NCP version
// The final line is always "C6-INSTALL: SUCCESS <version>" or
// "C6-INSTALL: FAILED <reason>". NVS is never touched.

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

static const char *TAG = "c6-install";

// Board wiring (zhac-wired-core: ZHAC_RCP_UART_*, ZHAC_RCP_RESET_GPIO).
#define C6_RESET_GPIO   54   // -> C6 CHIP_PU (10k pull-up + 1uF)
#define RCP_UART        UART_NUM_1
#define RCP_TX_GPIO     15   // -> C6 GPIO21 (ot_rcp RX)
#define RCP_RX_GPIO     14   // <- C6 GPIO20 (ot_rcp TX)
#define RCP_BAUD        460800

#define OTA_CHUNK       1500 // CrowPanel tool's proven chunk size

extern const uint8_t rcp_start[] asm("_binary_rcp_app_bin_start");
extern const uint8_t rcp_end[] asm("_binary_rcp_app_bin_end");

static void halt(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void halt(const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("\n==========================================================\n"
           "C6-INSTALL: %s\n"
           "==========================================================\n", msg);
    for (;;) vTaskDelay(portMAX_DELAY);
}

// ---------------------------------------------------------------- spinel ---
// HDLC-lite (RFC 1662 framing as OpenThread uses it): 0x7E flags, 0x7D escape
// (byte ^ 0x20), FCS = CRC-16/X-25 appended little-endian.

static uint16_t fcs16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
    }
    return c ^ 0xFFFF;
}

static size_t hdlc_put(uint8_t *out, uint8_t b)
{
    if (b == 0x7E || b == 0x7D || b == 0x11 || b == 0x13 || b == 0xF8) {
        out[0] = 0x7D;
        out[1] = b ^ 0x20;
        return 2;
    }
    out[0] = b;
    return 1;
}

static size_t hdlc_encode(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    uint16_t f = fcs16(in, n);
    out[o++] = 0x7E;
    for (size_t i = 0; i < n; i++) o += hdlc_put(out + o, in[i]);
    o += hdlc_put(out + o, f & 0xFF);
    o += hdlc_put(out + o, f >> 8);
    out[o++] = 0x7E;
    return o;
}

typedef struct {
    uint8_t buf[256];
    size_t len;
    bool esc;
} hdlc_rx_t;

// Feeds one byte; returns the payload length of a complete, FCS-valid frame
// (payload in h->buf), else 0.
static size_t hdlc_feed(hdlc_rx_t *h, uint8_t b)
{
    if (b == 0x7E) {
        size_t n = h->len;
        h->len = 0;
        h->esc = false;
        if (n > 2 && fcs16(h->buf, n - 2) == (h->buf[n - 2] | h->buf[n - 1] << 8)) return n - 2;
        return 0;
    }
    if (b == 0x7D) {
        h->esc = true;
        return 0;
    }
    if (h->esc) {
        b ^= 0x20;
        h->esc = false;
    }
    if (h->len < sizeof h->buf) h->buf[h->len++] = b;
    else h->len = 0;  // oversize: drop, resync on next flag
    return 0;
}

// Known-good vector: spinel GET NCP_VERSION, TID 1 = 7E 81 02 02 5E 80 7E
// (FCS checked against CRC-16/X-25("123456789") = 0x906E). Also round-trips it.
static bool spinel_selftest(void)
{
    static const uint8_t req[] = {0x81, 0x02, 0x02};
    static const uint8_t want[] = {0x7E, 0x81, 0x02, 0x02, 0x5E, 0x80, 0x7E};
    uint8_t out[16];
    size_t n = hdlc_encode(req, sizeof req, out);
    if (n != sizeof want || memcmp(out, want, n) != 0) return false;
    hdlc_rx_t h = {0};
    size_t got = 0;
    for (size_t i = 0; i < n; i++) got = hdlc_feed(&h, out[i]) ?: got;
    return got == sizeof req && memcmp(h.buf, req, got) == 0;
}

static void c6_reset(void)
{
    gpio_reset_pin(C6_RESET_GPIO);
    gpio_set_direction(C6_RESET_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(C6_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(50));   // 10k + 1uF on CHIP_PU: ~10 ms RC
    gpio_set_level(C6_RESET_GPIO, 1);
}

// GET <prop> with <tid>; waits for PROP_VALUE_IS <prop>. Returns value length
// (copied to val) or -1.
static int spinel_get(uint8_t tid, uint8_t prop, uint8_t *val, size_t cap, int timeout_ms)
{
    const uint8_t req[] = {(uint8_t)(0x80 | tid), 0x02 /* PROP_VALUE_GET */, prop};
    uint8_t out[16];
    uart_flush_input(RCP_UART);
    uart_write_bytes(RCP_UART, out, hdlc_encode(req, sizeof req, out));

    hdlc_rx_t h = {0};
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint8_t b;
    while ((int32_t)(until - xTaskGetTickCount()) > 0) {
        if (uart_read_bytes(RCP_UART, &b, 1, pdMS_TO_TICKS(20)) != 1) continue;
        size_t n = hdlc_feed(&h, b);
        // header 10ii tttt (IID 0) | cmd 6 = PROP_VALUE_IS | prop (< 0x80: 1 byte)
        if (n >= 3 && (h.buf[0] & 0xCF) == (0x80 | tid) && h.buf[1] == 0x06 && h.buf[2] == prop) {
            size_t len = n - 3 < cap ? n - 3 : cap;
            memcpy(val, h.buf + 3, len);
            return (int)len;
        }
    }
    return -1;
}

// Resets the C6 and asks for its spinel NCP version, retrying for ~window_ms.
// Returns true with the version in ver. Leaves UART + pins released.
static bool spinel_probe(char *ver, size_t cap, int window_ms)
{
    const uart_config_t cfg = {
        .baud_rate = RCP_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(RCP_UART, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(RCP_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(RCP_UART, RCP_TX_GPIO, RCP_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    c6_reset();
    vTaskDelay(pdMS_TO_TICKS(500));

    bool ok = false;
    uint8_t v[96];
    for (int t = 0; t < window_ms / 500 && !ok; t++) {
        int n = spinel_get(1, 0x02 /* PROP_NCP_VERSION */, v, sizeof v - 1, 500);
        if (n > 0) {
            v[n] = 0;
            snprintf(ver, cap, "%s", (char *)v);
            ok = true;
        }
    }
    if (ok) {
        // PROP_PROTOCOL_VERSION: two packed uints (major, minor), each < 128.
        int n = spinel_get(2, 0x01, v, sizeof v, 500);
        if (n >= 2) ESP_LOGI(TAG, "spinel protocol version %u.%u", v[0], v[1]);
    }
    uart_driver_delete(RCP_UART);
    gpio_reset_pin(RCP_TX_GPIO);
    gpio_reset_pin(RCP_RX_GPIO);
    return ok;
}

// ----------------------------------------------------------------- image ---

static void sha256_hex(const uint8_t *p, size_t n, uint8_t dig[32], char hex[65])
{
    size_t olen = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, p, n, dig, 32, &olen) != PSA_SUCCESS || olen != 32)
        halt("FAILED sha256 unavailable");
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", dig[i]);
}

// Returns the image size or halts. The merged 0x0 image starts with the C6
// BOOTLOADER, which has the same magic and chip id but no app descriptor.
static size_t validate_image(void)
{
    const size_t len = rcp_end - rcp_start;
    const esp_image_header_t *hdr = (const esp_image_header_t *)rcp_start;
    const esp_app_desc_t *desc =
        (const esp_app_desc_t *)(rcp_start + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    uint8_t dig[32];
    char hex[65];

    ESP_LOGI(TAG, "embedded C6 image: %u bytes", (unsigned)len);
    if (len < 1024 || hdr->magic != ESP_IMAGE_HEADER_MAGIC)
        halt("FAILED embedded image: bad magic 0x%02x", len ? rcp_start[0] : 0);
    if (hdr->chip_id != ESP_CHIP_ID_ESP32C6)
        halt("FAILED embedded image: chip id %u, not ESP32-C6 (%u)", hdr->chip_id, ESP_CHIP_ID_ESP32C6);
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD)
        halt("FAILED embedded image is not an APP image (merged/bootloader image?)");
    ESP_LOGI(TAG, "  project '%s' version '%s' idf '%s' built %s %s", desc->project_name, desc->version,
             desc->idf_ver, desc->date, desc->time);
    ESP_LOGI(TAG, "  chip rev %u.%02u .. %u.%02u, %u segments", hdr->min_chip_rev_full / 100,
             hdr->min_chip_rev_full % 100, hdr->max_chip_rev_full / 100, hdr->max_chip_rev_full % 100,
             hdr->segment_count);
    if (strcmp(desc->project_name, "esp_ot_rcp") != 0)
        halt("FAILED embedded image is '%s', expected esp_ot_rcp", desc->project_name);

    // The appended SHA-256 covers everything before it: catches a truncated
    // or corrupted embed before the C6 ever sees a byte.
    if (!hdr->hash_appended) halt("FAILED embedded image has no appended SHA-256");
    sha256_hex(rcp_start, len - 32, dig, hex);
    if (memcmp(dig, rcp_start + len - 32, 32) != 0) halt("FAILED embedded image: appended SHA-256 mismatch");

    sha256_hex(rcp_start, len, dig, hex);
    ESP_LOGI(TAG, "  sha256 %s", hex);
    if (strcmp(hex, RCP_SHA256_HEX) != 0) halt("FAILED embedded image sha256 != build-time %s", RCP_SHA256_HEX);
    return len;
}

// ------------------------------------------------------------------- ota ---

static void log_slave(void)
{
    esp_hosted_coprocessor_fwver_t v = {0};
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < 3 && e != ESP_OK; i++) e = esp_hosted_get_coprocessor_fwversion(&v);
    if (e == ESP_OK)
        ESP_LOGI(TAG, "C6 ESP-Hosted slave firmware: %" PRIu32 ".%" PRIu32 ".%" PRIu32, v.major1, v.minor1,
                 v.patch1);
    else
        ESP_LOGW(TAG, "C6 slave did not answer the version RPC (%s) -- old slaves (v2.3.0) often don't; "
                      "ota_begin below is the real capability test", esp_err_to_name(e));

    esp_hosted_app_desc_t d = {0};
    e = esp_hosted_get_coprocessor_app_desc(&d);
    if (e == ESP_OK)
        ESP_LOGI(TAG, "C6 running app: project '%.32s' version '%.32s' idf '%.32s' built %.16s %.16s",
                 d.project_name, d.version, d.idf_ver, d.date, d.time);
    else
        ESP_LOGW(TAG, "C6 app-description RPC unavailable (%s)", esp_err_to_name(e));
    ESP_LOGI(TAG, "C6 partition/OTA state is not exposed over RPC; slave OTA writes the INACTIVE slot "
                  "only (esp_ota_begin on the slave refuses the running one)");
}

static void ota_install(const uint8_t *img, size_t len)
{
    static uint8_t chunk[OTA_CHUNK];  // the API takes a non-const pointer
    esp_err_t e;

    ESP_LOGW(TAG, "OTA begin (slave erases its inactive slot)");
    if ((e = esp_hosted_slave_ota_begin()) != ESP_OK)
        halt("FAILED ota_begin: %s -- nothing written, C6 unchanged", esp_err_to_name(e));

    int next_pct = 10;
    for (size_t off = 0; off < len; off += OTA_CHUNK) {
        size_t n = len - off < OTA_CHUNK ? len - off : OTA_CHUNK;
        memcpy(chunk, img + off, n);
        if ((e = esp_hosted_slave_ota_write(chunk, n)) != ESP_OK)
            halt("FAILED ota_write at %u/%u: %s -- not activated, C6 still boots its old slot",
                 (unsigned)off, (unsigned)len, esp_err_to_name(e));
        int pct = (int)((off + n) * 100 / len);
        if (pct >= next_pct) {
            ESP_LOGI(TAG, "OTA %3d%% (%u/%u)", pct, (unsigned)(off + n), (unsigned)len);
            next_pct = pct / 10 * 10 + 10;
        }
    }

    // The slave's esp_ota_end() verifies the image (chip id, rev, SHA) itself.
    // NB slaves < 2.6 also set the boot partition inside ota_end and reboot
    // ~5 s later; activate is the 2.6+ equivalent.
    if ((e = esp_hosted_slave_ota_end()) != ESP_OK)
        halt("FAILED ota_end (slave rejected the image): %s -- not activated", esp_err_to_name(e));
    ESP_LOGI(TAG, "OTA end OK: slave validated the image");

    e = esp_hosted_slave_ota_activate();
    if (e == ESP_OK)
        ESP_LOGI(TAG, "OTA activate OK: C6 boots ot_rcp on its next reset");
    else
        ESP_LOGW(TAG, "OTA activate returned %s: expected on slaves < 2.6, whose ota_end already set the "
                      "boot slot. The spinel check below decides.", esp_err_to_name(e));
}

// ------------------------------------------------------------------ main ---

void app_main(void)
{
    char ver[96] = "";

    printf("\n=== c6-rcp-installer (ESP-Hosted %d.%d.%d host, packet mode) ===\n", ESP_HOSTED_VERSION_MAJOR_1,
           ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1);
    if (!spinel_selftest()) halt("FAILED internal HDLC/spinel self-test");
    if (psa_crypto_init() != PSA_SUCCESS) halt("FAILED psa_crypto_init");
    esp_event_loop_create_default();  // esp_hosted posts transport events

    const size_t len = validate_image();

    // esp_hosted's constructor already set up the SDIO host; this resets the
    // C6 on GPIO54 and waits for the slave's init event.
    ESP_LOGI(TAG, "connecting to the C6 ESP-Hosted slave over SDIO...");
    ESP_LOGI(TAG, "(an assert in process_init_event here = streaming-mode slave: use sdkconfig.streaming)");
    if (esp_hosted_connect_to_slave() != ESP_OK) {
        ESP_LOGW(TAG, "no ESP-Hosted slave on SDIO; checking whether the C6 already runs ot_rcp");
        esp_hosted_deinit();
        if (spinel_probe(ver, sizeof ver, 3000)) {
            printf("C6 already answers spinel -- ot_rcp is installed, nothing written.\n");
            halt("SUCCESS %s", ver);
        }
        halt("FAILED C6 answers neither ESP-Hosted (SDIO) nor spinel (UART) -- nothing written");
    }
    ESP_LOGI(TAG, "ESP-Hosted transport up");
    log_slave();

    ota_install(rcp_start, len);

    esp_hosted_deinit();  // release SDIO before the C6 reboots under it
    ESP_LOGI(TAG, "resetting C6 and waiting for spinel on UART%d tx=%d rx=%d @%d", RCP_UART, RCP_TX_GPIO,
             RCP_RX_GPIO, RCP_BAUD);
    if (spinel_probe(ver, sizeof ver, 10000)) halt("SUCCESS %s", ver);
    halt("FAILED ot_rcp installed but no spinel reply on GPIO14/15 -- power-cycle the board and run this "
         "tool again: with no hosted slave left it goes straight to the spinel check");
}
