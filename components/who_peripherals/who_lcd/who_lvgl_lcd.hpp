#pragma once
#include "esp_lcd_types.h"
#include "bsp/esp-bsp.h"
#if !BSP_CONFIG_NO_GRAPHIC_LIB
#include "lvgl.h"
#include "esp_lvgl_port.h"

namespace who {
namespace lcd {
class WhoLCD {
public:
    // Use ESP default LVGL task config (un-pinned by default) unless user overrides
    WhoLCD(const lvgl_port_cfg_t &lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG()) { init(lvgl_port_cfg); }
    ~WhoLCD() { deinit(); }
    void init(const lvgl_port_cfg_t &lvgl_port_cfg);
    void deinit();

private:
    lv_display_t *m_disp;
};
} // namespace lcd
} // namespace who
#endif
