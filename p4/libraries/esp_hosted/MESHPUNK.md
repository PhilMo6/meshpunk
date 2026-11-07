# esp_hosted — vendored for the T-Display P4

Upstream: `espressif/esp_hosted` **2.12.13** from the ESP Component Registry
(esp-hosted-mcu commit `dd0176e5fc959d79b306f6677a2f878123c58e9a`),
Apache-2.0. Used by `p4/main/idf_component.yml` through `override_path`.

Copied: `CMakeLists.txt`, `Kconfig`, `idf_component.yml`, `LICENSE`,
`README.md`, `CHANGELOG.md`, `sdkconfig.rename`, `host/`, `common/`.
Left out (not part of the host build): `docs/`, `examples/`, `slave/`,
`tools/`, `sdkconfig.ci.all_features_enabled`.

The C6 runs LilyGo's esp-hosted 2.12.3 slave image
(`p4/c6_fw/`, written by rung 7d).

## MESHPUNK patch: co-processor reset callback

The T-Display P4 drives the C6's EN line through an XL9535 expander (IO14),
not a GPIO. Upstream resets the co-processor by pulsing its configured reset
GPIO (54 by default on the P4, a user-header pin on this board) and asserts
that one is set. The patch follows the shape of LilyGo's fork
(Llgok/esp-hosted-mcu, Apache-2.0, commits e47ae3c1 and 719343ba):

- `Kconfig`: `ESP_HOSTED_AUTO_INIT` (default y) — off = the boot-time
  constructor that calls `esp_hosted_init()` is not built.
- `CMakeLists.txt`: `port_esp_hosted_host_init.c` built only with
  `CONFIG_ESP_HOSTED_AUTO_INIT`.
- `host/api/include/esp_hosted_transport_config.h`:
  `esp_hosted_sdio_reset_fn_t`, `esp_hosted_sdio_set_reset_callback()`,
  `esp_hosted_sdio_get_reset_callback()`.
- `host/api/src/esp_hosted_transport_config.c`: the callback storage and
  the two functions.
- `host/drivers/transport/sdio/sdio_drv.c`: `transport_gpio_reset()` drives
  each level through the callback when one is registered and configures the
  reset GPIO only otherwise; both callers report a failed reset and stop.
- `host/port/esp/freertos/src/port_esp_hosted_host_sdio.c`: the transport
  log names `Slave_Reset[callback]` instead of the unused GPIO.

Every changed spot carries a `MESHPUNK` comment.
