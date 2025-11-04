#include "who_frame_lcd_disp.hpp"
#include "esp_heap_caps.h"
#if !BSP_CONFIG_NO_GRAPHIC_LIB
#include "esp_lvgl_port.h"
#endif
#include <cstring>
#include "telemetry_flags.h"

using namespace who::lcd;

namespace who {
namespace lcd_disp {
WhoFrameLCDDisp::WhoFrameLCDDisp(const std::string &name, frame_cap::WhoFrameCapNode *frame_cap_node, int peek_index) :
    task::WhoTask(name), m_lcd(new lcd::WhoLCD()), m_frame_cap_node(frame_cap_node), m_peek_index(peek_index)
{
    frame_cap_node->add_new_frame_signal_subscriber(this);
#if !BSP_CONFIG_NO_GRAPHIC_LIB
    bsp_display_lock(0);
    m_canvas = lv_canvas_create(lv_scr_act());
    m_canvas_width = frame_cap_node->get_fb_width();
    m_canvas_height = frame_cap_node->get_fb_height();
    lv_obj_set_size(m_canvas, m_canvas_width, m_canvas_height);
    // Allocate a persistent canvas buffer; prefer DMA-capable internal RAM to avoid SPI DMA failures
    size_t bytes = (size_t)m_canvas_width * m_canvas_height * 2;
    m_canvas_buf = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!m_canvas_buf) {
        // Fallback to default heap if DMA-capable internal allocation fails
        m_canvas_buf = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT);
    }
    // Set canvas buffer once; later we'll memcpy camera frames into it
    lv_canvas_set_buffer(m_canvas, m_canvas_buf, m_canvas_width, m_canvas_height, LV_COLOR_FORMAT_NATIVE);
    bsp_display_unlock();
#endif
}

WhoFrameLCDDisp::~WhoFrameLCDDisp()
{
#if !BSP_CONFIG_NO_GRAPHIC_LIB
    bsp_display_lock(0);
    lv_obj_del(m_canvas);
    bsp_display_unlock();
    if (m_canvas_buf) {
        free(m_canvas_buf);
        m_canvas_buf = nullptr;
    }
#endif
    delete m_lcd;
}

void WhoFrameLCDDisp::set_lcd_disp_cb(const std::function<void(who::cam::cam_fb_t *)> &lcd_disp_cb)
{
    m_lcd_disp_cb = lcd_disp_cb;
}

#if !BSP_CONFIG_NO_GRAPHIC_LIB
lv_obj_t *WhoFrameLCDDisp::get_canvas()
{
    return m_canvas;
}
#endif

void WhoFrameLCDDisp::task()
{
    // Short boot-time grace period: allow subsystems to settle to reduce early SPI queue failures
    static bool s_warmed_up = false;
    if (!s_warmed_up) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        s_warmed_up = true;
    }
    while (true) {
        EventBits_t event_bits =
            xEventGroupWaitBits(m_event_group, NEW_FRAME | TASK_PAUSE | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
        if (event_bits & TASK_STOP) {
            break;
        } else if (event_bits & TASK_PAUSE) {
            xEventGroupSetBits(m_event_group, TASK_PAUSED);
            EventBits_t pause_event_bits =
                xEventGroupWaitBits(m_event_group, TASK_RESUME | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
            if (pause_event_bits & TASK_STOP) {
                break;
            } else {
                continue;
            }
        }
        auto fb = m_frame_cap_node->cam_fb_peek(m_peek_index);
        if (!fb || !fb->buf) {
            // Wait for the first valid frame before starting LVGL activity
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
#if BSP_CONFIG_NO_GRAPHIC_LIB
        if (m_lcd_disp_cb) {
            m_lcd_disp_cb(fb);
        }
        m_lcd->draw_bitmap(fb->buf, (int)fb->width, (int)fb->height, 0, 0);
#else
        // If a Telegram send is in progress, pause LCD updates to free SPI and CPU
        {
            // Manage LVGL task stop/resume around high-traffic network sends
            static bool s_prev_sending = false;
            static bool s_lvgl_stopped = false;
            static TickType_t s_resume_at = 0;
            TickType_t now = xTaskGetTickCount();

            if (!s_prev_sending && g_sending_telegram) {
#if !BSP_CONFIG_NO_GRAPHIC_LIB
                // Stop LVGL timer/task to avoid blocking flush and WDT during network send
                (void)lvgl_port_stop();
                s_lvgl_stopped = true;
#endif
            }
            if (s_prev_sending && !g_sending_telegram) {
                // sending just finished, set a short cooldown
                s_resume_at = now + pdMS_TO_TICKS(250);
            }

            if (g_sending_telegram) {
                // Do not touch LVGL or the display while sending to prevent flush stalls
                s_prev_sending = g_sending_telegram;
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            }

            if (now < s_resume_at) {
                // Cooldown window after send
                s_prev_sending = g_sending_telegram;
                vTaskDelay(pdMS_TO_TICKS(60));
                continue;
            }

            if (s_lvgl_stopped) {
#if !BSP_CONFIG_NO_GRAPHIC_LIB
                // Resume LVGL task after cooldown
                (void)lvgl_port_resume();
#endif
                s_lvgl_stopped = false;
            }
            s_prev_sending = g_sending_telegram;

            // Throttle LVGL canvas updates to reduce SPI queue pressure and avoid flush stalls
            static uint8_t s_frame_mod = 0; // only update ~every 20th frame during normal run
            s_frame_mod = (s_frame_mod + 1) % 20;
            if (s_frame_mod == 0) {
                // Copy camera FB into our persistent LVGL canvas buffer
                if (m_canvas_buf && (fb->width == m_canvas_width) && (fb->height == m_canvas_height)) {
                    memcpy(m_canvas_buf, fb->buf, (size_t)m_canvas_width * m_canvas_height * 2);
                }
                bsp_display_lock(0);
                if (m_lcd_disp_cb) {
                    m_lcd_disp_cb(fb);
                }
                bsp_display_unlock();
            }
        }
    // Small pacing delay to avoid saturating SPI/LVGL flush queues
    vTaskDelay(pdMS_TO_TICKS(180));
#endif
    }
    xEventGroupSetBits(m_event_group, TASK_STOPPED);
    vTaskDelete(NULL);
}
} // namespace lcd_disp
} // namespace who
