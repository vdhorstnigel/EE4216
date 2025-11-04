#pragma once
#include "who_frame_cap.hpp"
#include "who_lcd.hpp"
#include <vector>

namespace who {
namespace lcd_disp {
class WhoFrameLCDDisp : public task::WhoTask {
public:
    static inline constexpr EventBits_t NEW_FRAME = frame_cap::WhoFrameCapNode::NEW_FRAME;

    WhoFrameLCDDisp(const std::string &name, frame_cap::WhoFrameCapNode *frame_cap_node, int peek_index = 0);
    ~WhoFrameLCDDisp();
    void set_lcd_disp_cb(const std::function<void(who::cam::cam_fb_t *)> &lcd_disp_cb);
#if !BSP_CONFIG_NO_GRAPHIC_LIB
    lv_obj_t *get_canvas();
#endif

private:
    void task() override;
    lcd::WhoLCD *m_lcd;
#if !BSP_CONFIG_NO_GRAPHIC_LIB
    lv_obj_t *m_canvas;
    uint8_t *m_canvas_buf;
    int m_canvas_width;
    int m_canvas_height;
#endif
    frame_cap::WhoFrameCapNode *m_frame_cap_node;
    int m_peek_index;
    std::function<void(who::cam::cam_fb_t *)> m_lcd_disp_cb;
};
} // namespace lcd_disp
} // namespace who
