#include "ota_server.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include <string.h>
#include <sys/param.h>

static const char *TAG = "OTA_SERVER";

#define AP_SSID "Mailbox-OTA"
#define AP_PASS "CHANGE-ME-before-flashing"
#define AP_MAX_CONN 4

static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *update_partition = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event =
        (wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac),
             event->aid);
  } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event =
        (wifi_event_ap_stadisconnected_t *)event_data;
    ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac),
             event->aid);
  }
}

static void wifi_init_softap(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_ap();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

  wifi_config_t wifi_config = {
      .ap =
          {
              .ssid = AP_SSID,
              .ssid_len = strlen(AP_SSID),
              .channel = 1,
              .password = AP_PASS,
              .max_connection = AP_MAX_CONN,
              .authmode = WIFI_AUTH_WPA2_PSK,
              .pmf_cfg =
                  {
                      .required = false,
                  },
          },
  };

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "Wi-Fi AP started. SSID: %s Password: %s", AP_SSID, AP_PASS);
}

static esp_err_t index_get_handler(httpd_req_t *req) {
  const char *resp_str =
      "<!DOCTYPE html><html><head><title>Mailbox OTA</title>"
      "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
      "<style>body{font-family:sans-serif; text-align:center; "
      "padding-top:50px; background:#111; color:#fff;}"
      "input[type='button'], input[type='submit']{padding:10px 20px; "
      "font-size:18px; "
      "margin-top:20px; background:#007BFF; color:white; border:none; "
      "border-radius:5px; cursor:pointer}"
      "#pbox{display:none; width:80%; max-width:400px; margin:20px auto; "
      "border:2px solid #555; border-radius:10px; overflow:hidden;}"
      "#pbar{width:0%; height:30px; background:#0f0; transition: width "
      "0.3s;}</style>"
      "</head><body>"
      "<h1>Mailbox Sensor OTA Utility</h1>"
      "<p>Upload the <b>.bin</b> firmware file below to update the device.</p>"
      "<form id='upform'>"
      "<input type='file' id='file' accept='.bin' required><br>"
      "<input type='button' value='Upload & Reboot' onclick='startUpload()'>"
      "</form>"
      "<div id='pbox'><div id='pbar'></div></div>"
      "<h3 id='status'></h3>"
      "<br><br>"
      "<form method='POST' action='/reboot'>"
      "<input type='submit' value='Reboot Device' style='background:#DC3545;'>"
      "</form>"
      "<script>"
      "function startUpload() {"
      "  var file = document.getElementById('file').files[0];"
      "  if(!file) { alert('Please select a file!'); return; }"
      "  document.getElementById('upform').style.display='none';"
      "  document.getElementById('pbox').style.display='block';"
      "  document.getElementById('status').innerHTML = 'Uploading...';"
      "  var fd = new FormData(); fd.append('update', file);"
      "  var xhr = new XMLHttpRequest();"
      "  xhr.upload.addEventListener('progress', function(e) {"
      "    if(e.lengthComputable) {"
      "      var pct = (e.loaded / e.total) * 100;"
      "      document.getElementById('pbar').style.width = pct + '%';"
      "      document.getElementById('status').innerHTML = 'Uploading: ' + "
      "Math.round(pct) + '%';"
      "    }"
      "  }, false);"
      "  xhr.onreadystatechange = function() {"
      "    if (xhr.readyState == 4) {"
      "      if(xhr.status == 200) {"
      "        document.getElementById('status').innerHTML = 'Success! Device "
      "Rebooting...';"
      "      } else {"
      "        document.getElementById('status').innerHTML = 'Failed! Check "
      "console.';"
      "        document.getElementById('pbar').style.background = 'red';"
      "      }"
      "    }"
      "  };"
      "  xhr.open('POST', '/update', true);"
      "  xhr.send(fd);"
      "}"
      "</script></body></html>";

  httpd_resp_send(req, resp_str, strlen(resp_str));
  return ESP_OK;
}

static esp_err_t update_post_handler(httpd_req_t *req) {
  esp_err_t err;
  int total_len = req->content_len;
  int cur_len = 0;
  char *buf = malloc(2048);

  if (!buf) {
    ESP_LOGE(TAG, "Failed to allocate memory for OTA buffer");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  // Identify the next update partition
  update_partition = esp_ota_get_next_update_partition(NULL);
  if (update_partition == NULL) {
    ESP_LOGE(TAG, "Failed to get next update partition");
    free(buf);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Starting OTA to partition %s at offset 0x%lx",
           update_partition->label, update_partition->address);

  err =
      esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
    free(buf);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  bool is_image_header_checked = false;
  int body_start_idx = 0;

  while (cur_len < total_len) {
    int received = httpd_req_recv(req, buf, MIN(total_len - cur_len, 2048));
    if (received < 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      ESP_LOGE(TAG, "File receive failed");
      esp_ota_abort(ota_handle);
      free(buf);
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }

    if (received > 0) {
      // Step 1: Strip formatting if it's the first packet
      if (!is_image_header_checked) {
        char *header_end = strstr(buf, "\r\n\r\n");
        if (header_end) {
          ESP_LOGI(TAG, "Found multipart header break in the stream.");
          body_start_idx = (header_end - buf) + 4;

          if (buf[body_start_idx] == (char)0xE9) {
            ESP_LOGI(TAG, "Confirmed 0xE9 Magic Byte! Proceeding...");
            is_image_header_checked = true;
          } else {
            ESP_LOGW(TAG,
                     "Warning: Multipart chunk detected, but Magic Byte is "
                     "missing at expected offset! Hex dump: %02x",
                     buf[body_start_idx]);
            // Wait for the next chunk or attempt a direct hit if it isn't
            // form-data
            if (buf[0] == (char)0xE9) {
              ESP_LOGI(TAG,
                       "Found Magic Byte at index 0. Reverting to raw stream.");
              is_image_header_checked = true;
              body_start_idx = 0;
            }
          }
        } else if (buf[0] == (char)0xE9) {
          ESP_LOGI(TAG, "Plain stream detected (0xE9 Magic Byte at index 0).");
          is_image_header_checked = true;
          body_start_idx = 0;
        } else {
          ESP_LOGE(
              TAG,
              "OTA image has invalid magic byte and no multipart signature.");
          esp_ota_abort(ota_handle);
          free(buf);
          httpd_resp_send_500(req);
          return ESP_FAIL;
        }
      }

      int write_len = is_image_header_checked && body_start_idx > 0
                          ? (received - body_start_idx)
                          : received;
      char *write_buf = is_image_header_checked && body_start_idx > 0
                            ? (buf + body_start_idx)
                            : buf;

      // Clear the modifier for subsequent packets
      if (is_image_header_checked) {
        body_start_idx = 0;
      }

      err = esp_ota_write(ota_handle, (const void *)write_buf, write_len);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
        esp_ota_abort(ota_handle);
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
      }
      cur_len += received;
      ESP_LOGI(TAG, "Written %d of %d bytes", cur_len, total_len);
    }
  }

  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
    free(buf);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)",
             esp_err_to_name(err));
    free(buf);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  free(buf);

  // Send success response to browser
  const char *resp_str =
      "<!DOCTYPE html><html><body style='background:#111; color:#0f0; "
      "text-align:center; padding-top:100px; font-family:sans-serif;'>"
      "<h1>Update Successful!</h1><p>The sensor is rebooting into the new "
      "firmware now...</p></body></html>";
  httpd_resp_send(req, resp_str, strlen(resp_str));

  ESP_LOGI(TAG, "OTA Update complete. Rebooting in 3 seconds.");
  vTaskDelay(pdMS_TO_TICKS(3000));
  esp_restart();

  return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req) {
  const char *resp_str =
      "<!DOCTYPE html><html><body style='background:#111; color:#0f0; "
      "text-align:center; padding-top:100px; font-family:sans-serif;'>"
      "<h1>Rebooting...</h1><p>The sensor is exiting maintenance mode "
      "now.</p></body></html>";
  httpd_resp_send(req, resp_str, strlen(resp_str));

  ESP_LOGI(TAG, "Reboot requested via web interface. Rebooting in 1 second.");
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();

  return ESP_OK;
}

static httpd_handle_t start_webserver(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 8;
  config.max_open_sockets = 7;
  config.stack_size = 10240;
  config.uri_match_fn = httpd_uri_match_wildcard;

  // Extend timeouts significantly to support slow OTA Wi-Fi uploads
  config.recv_wait_timeout = 60;
  config.send_wait_timeout = 60;

  ESP_LOGI(TAG, "Starting OTA HTTP Server on port %d", config.server_port);
  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_uri_t index_uri = {.uri = "/",
                             .method = HTTP_GET,
                             .handler = index_get_handler,
                             .user_ctx = NULL};
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t update_uri = {.uri = "/update",
                              .method = HTTP_POST,
                              .handler = update_post_handler,
                              .user_ctx = NULL};
    httpd_register_uri_handler(server, &update_uri);

    httpd_uri_t reboot_uri = {.uri = "/reboot",
                              .method = HTTP_POST,
                              .handler = reboot_post_handler,
                              .user_ctx = NULL};
    httpd_register_uri_handler(server, &reboot_uri);

    return server;
  }

  ESP_LOGE(TAG, "Failed to start HTTP server.");
  return NULL;
}

void start_ota_server(void) {
  ESP_LOGI(TAG, "Initializing AP mode for OTA...");
  wifi_init_softap();
  start_webserver();
}
