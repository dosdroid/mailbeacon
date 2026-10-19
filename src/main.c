#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_bit_defs.h"
#include "esp_bt.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ota_server.h"
#include "services/gap/ble_svc_gap.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SMALL_DOOR_GPIO GPIO_NUM_4
#define BIG_DOOR_GPIO GPIO_NUM_3
#define MAIL_LED_GPIO GPIO_NUM_5

#include "esp_attr.h"

static const char *TAG = "MAILBEACON";
RTC_DATA_ATTR static bool g_mail_state = false;

// NVS Keys
#define NVS_NAMESPACE "mailbox"
#define NVS_KEY_STATE "mail_state"

// BTHome V2 Unencrypted Advertisement Data
// HA Autodiscovery utilizes this format natively via the BTHome integration
static uint8_t bthome_adv_data[] = {
    // 1. Flags: LE General Discoverable, BR/EDR Not Supported
    0x02, 0x01, 0x06,
    // 2. Service Data (BTHome UUID 0xFCD2)
    0x08, 0x16, 0xD2, 0xFC,

    // BTHome V2 (Unencrypted) | Bit 2 Enabled (Trigger-Based Device Flag).
    // Instructs Home Assistant the device sleeps deeply, preventing timeout
    // disconnects.
    0x44,

    0x23,
    0x00, // bthome_adv_data[12] = Occupancy sensor logic (false = 0x00, true =
          // 0x01)

    // 3. Local Name "Mailbox"
    0x08, 0x09, 'M', 'a', 'i', 'l', 'b', 'o', 'x'};

void nvs_init_and_load() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  nvs_handle_t nvs_handle;
  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err == ESP_OK) {
    uint8_t state = 0;
    err = nvs_get_u8(nvs_handle, NVS_KEY_STATE, &state);
    if (err == ESP_OK && state == 1) {
      g_mail_state = true;
    }
    nvs_close(nvs_handle);
  }
}

void nvs_save_state(bool state) {
  nvs_handle_t nvs_handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
    nvs_set_u8(nvs_handle, NVS_KEY_STATE, state ? 1 : 0);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
  }
}

void prepare_ble_payload() {
  // Inject the real mail state into the BTHome occupancy object
  bthome_adv_data[9] = g_mail_state ? 0x01 : 0x00;
}

static uint8_t own_addr_type;

static void ble_app_advertise(void) {
  struct ble_gap_adv_params adv_params;
  memset(&adv_params, 0, sizeof(adv_params));

  // Rapid broadcasting interval: 50ms to 100ms
  adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(50);
  adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(100);
  // BTHome specifies non-connectable undirected advertising
  adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  // Set raw advertisement data directly bypassing standard formatting structs
  ble_gap_adv_set_data(bthome_adv_data, sizeof(bthome_adv_data));
  ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, NULL,
                    NULL);
}

static void ble_app_on_sync(void) {
  // Generate UUID address and begin pushing packets
  ble_hs_id_infer_auto(0, &own_addr_type);
  ble_app_advertise();
}

void nimble_host_task(void *param) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void setup_gpio() {
  gpio_config_t io_conf = {.pin_bit_mask = (1ULL << SMALL_DOOR_GPIO) |
                                           (1ULL << BIG_DOOR_GPIO),
                           .mode = GPIO_MODE_INPUT,
                           .pull_down_en = GPIO_PULLDOWN_ENABLE,
                           .pull_up_en = GPIO_PULLUP_DISABLE,
                           .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&io_conf);

  io_conf.pin_bit_mask = (1ULL << MAIL_LED_GPIO);
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_config(&io_conf);

  // Weakest pad drive for the LED. While mail is waiting the LED is by far the
  // largest load in deep sleep; the higher output impedance of the weak
  // driver trims its current (and brightness) a little. Drive strength is part
  // of the state latched by gpio_hold_en(), so it survives deep sleep.
  gpio_set_drive_capability(MAIL_LED_GPIO, GPIO_DRIVE_CAP_0);
}

void app_main(void) {
  ESP_LOGI(TAG, "MailBeacon booting...");

  // Deep sleep resets the IO MUX/GPIO registers while the pads themselves stay
  // latched by the hold function. Re-apply the pad configuration first, then
  // release the holds, so the pads go straight from their held state to the
  // configured one. g_mail_state lives in RTC memory and is already valid
  // after a deep-sleep wake, so the LED is re-driven to its held level and
  // does not glitch off.
  setup_gpio();
  gpio_set_level(MAIL_LED_GPIO, g_mail_state ? 1 : 0);
  gpio_hold_dis(MAIL_LED_GPIO);
  gpio_hold_dis(SMALL_DOOR_GPIO);
  gpio_hold_dis(BIG_DOOR_GPIO);
  gpio_deep_sleep_hold_dis();

  // ESP-IDF 6.0 replaced the single wake cause with a bitmap of all causes.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
  uint32_t wake_causes = esp_sleep_get_wakeup_causes();
  bool woke_by_gpio = (wake_causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) != 0;
  bool woke_by_timer = (wake_causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) != 0;
#else
  esp_sleep_source_t wake_cause = esp_sleep_get_wakeup_cause();
  bool woke_by_gpio = (wake_cause == ESP_SLEEP_WAKEUP_GPIO);
  bool woke_by_timer = (wake_cause == ESP_SLEEP_WAKEUP_TIMER);
#endif
  if (!woke_by_gpio && !woke_by_timer) {
    nvs_init_and_load();
  }

  // 1. Evaluate Wakeup Reason
  if (woke_by_gpio) {
    uint64_t wake_pins = esp_sleep_get_gpio_wakeup_status();
    
    vTaskDelay(pdMS_TO_TICKS(10)); // tiny debounce settlement

    
    // Check if pins are currently LOW, OR if they were the precise reason we woke up.
    // The hardware latches the wakeup reason, so even if the user closed the door
    // faster than the 100ms debounce + boot time, we still know it opened!
    bool s_door_open = (gpio_get_level(SMALL_DOOR_GPIO) == 0) || ((wake_pins & (1ULL << SMALL_DOOR_GPIO)) != 0);
    bool b_door_open = (gpio_get_level(BIG_DOOR_GPIO) == 0) || ((wake_pins & (1ULL << BIG_DOOR_GPIO)) != 0);

    if (s_door_open && !g_mail_state) {
      ESP_LOGI(TAG, "Hardware Wakeup: Small Door - Mail Delivered!");
      g_mail_state = true;
      nvs_save_state(g_mail_state);
    } else if (b_door_open && g_mail_state) {
      ESP_LOGI(TAG, "Hardware Wakeup: Big Door - Mail Collected!");
      g_mail_state = false;
      nvs_save_state(g_mail_state);
    }
  } else {
    ESP_LOGI(TAG, "Timer/Standard Wakeup. Current State: %s",
             g_mail_state ? "FULL" : "EMPTY");
  }

  if (g_mail_state)
    gpio_set_level(MAIL_LED_GPIO, 1);
  else
    gpio_set_level(MAIL_LED_GPIO, 0);

  // 2. NimBLE Fast Initialization
  prepare_ble_payload();
  nimble_port_init();
  ble_hs_cfg.sync_cb = ble_app_on_sync;
  nimble_port_freertos_init(nimble_host_task);

  // Increase Bluetooth TX Power to maximum (+9 dBm)
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);

  ESP_LOGI(TAG, "Broadcasting BTHome V2 in rapid 50ms intervals at max TX "
                "power (+9dBm)...");

  // 3. Maintenance Check at the end of the 5.5s broadcast loop
  vTaskDelay(pdMS_TO_TICKS(5500));

  bool req_maintenance = (gpio_get_level(SMALL_DOOR_GPIO) == 0) &&
                         (gpio_get_level(BIG_DOOR_GPIO) == 0);
  if (req_maintenance) {
    ESP_LOGI(TAG, "BOTH DOORS OPEN: entering maintenance mode (Wi-Fi AP + OTA server)");
    start_ota_server();

    // Timeout logic: 10 minutes (600 seconds) max.
    // Loop interval is 1s (500ms OFF + 500ms ON), so we iterate 600 times.
    int timeout_seconds = 600;

    while (timeout_seconds > 0) {
      vTaskDelay(pdMS_TO_TICKS(500));
      gpio_set_level(MAIL_LED_GPIO, 0);
      vTaskDelay(pdMS_TO_TICKS(500));
      gpio_set_level(MAIL_LED_GPIO, 1);
      timeout_seconds--;
    }

    ESP_LOGW(TAG, "OTA Maintenance AP timed out after 10 minutes! Shutting "
                  "down Wi-Fi and sleeping to save battery.");
    // Reboot rather than falling through to deep sleep: a clean restart is the
    // simplest way to tear down the Wi-Fi stack and HTTP server completely.
    esp_restart();
  }

  // 4. Proceed to immediate deep sleep
  ESP_LOGI(TAG,
           "Broadcast complete. Arming GPIO wakeups and halting execution.");

  nimble_port_stop();
  nimble_port_deinit();

  // Dynamically configure ESP32-C3 GPIO logic (Low = Wakeup)
  // Only arm wakeups for doors that are currently CLOSED.
  // This prevents infinite wake-loops if a door is jammed open.
  uint64_t wakeup_mask = 0;
  if (gpio_get_level(SMALL_DOOR_GPIO) == 1) { // If closed
    wakeup_mask |= (1ULL << SMALL_DOOR_GPIO);
  }
  if (gpio_get_level(BIG_DOOR_GPIO) == 1) { // If closed
    wakeup_mask |= (1ULL << BIG_DOOR_GPIO);
  }

  if (wakeup_mask > 0) {
    // ESP-IDF 6.0 renamed the deep-sleep GPIO wakeup API.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(wakeup_mask,
                                                         ESP_GPIO_WAKEUP_GPIO_LOW);
#else
    esp_deep_sleep_enable_gpio_wakeup(wakeup_mask, ESP_GPIO_WAKEUP_GPIO_LOW);
#endif
  } else {
    // If both doors are open, only wake up via the 24h timer failsafe
    ESP_LOGW(TAG, "Both doors are currently open. Disabling GPIO wakeup to "
                  "prevent bootloop.");
  }

  // Failsafe timer: Wake up every 24 hours to broadcast heartbeat
  esp_sleep_enable_timer_wakeup(86400 * 1000000ULL);

  // Blink 3 times distinctly before sleep (forced OFF for visibility if already
  // ON)
  for (int i = 0; i < 3; i++) {
    gpio_set_level(MAIL_LED_GPIO, 0); // Force OFF for a long, visible gap
    vTaskDelay(pdMS_TO_TICKS(300));
    gpio_set_level(MAIL_LED_GPIO, 1); // Turn ON for a brief, sharp flash
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // Restore the LED's final level and latch it. The pad is held in both
  // states: GPIO0-5 lose their IO MUX configuration in deep sleep, so an
  // unheld LED pad would float instead of being driven low.
  gpio_set_level(MAIL_LED_GPIO, g_mail_state ? 1 : 0);
  gpio_hold_en(MAIL_LED_GPIO);

  // Latch the door inputs (input enable + pull-down) for deep sleep.
  // Note: with CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS enabled,
  // esp_deep_sleep_start() would switch low-level wake pins to pull-UP right
  // before sleeping, the opposite of what this wiring needs. That option is
  // disabled in sdkconfig so the pull-down configured here is what the pads
  // keep while asleep.
  gpio_hold_en(BIG_DOOR_GPIO);
  gpio_hold_en(SMALL_DOOR_GPIO);
  gpio_deep_sleep_hold_en();

  // Halt CPU
  esp_deep_sleep_start();
}
