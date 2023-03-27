
#include "../plugin.hpp" 

using  DrawArgs = Widget::DrawArgs;

namespace rage {
void draw_line(const DrawArgs& args, NVGcolor color, rack::math::Vec start, rack::math::Vec stop) {
    nvgStrokeColor(args.vg, color);
    {
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, start.x, start.y);
        nvgLineTo(args.vg, stop.x, stop.y);
        nvgClosePath(args.vg);
    }
    nvgStroke(args.vg);
}

void draw_rect(const DrawArgs& args, NVGcolor color, rack::math::Rect rect, bool fill = false) {
    auto pos = rect.pos;
    auto size = rect.size;
    if (fill) {
        nvgBeginPath(args.vg);
        nvgFillColor(args.vg, color);
        nvgRect(args.vg, rect.pos.x, rect.pos.y, rect.size.x, rect.size.y);
        nvgFill(args.vg);
        nvgClosePath(args.vg);
    }
    draw_line(args, color, pos, pos + size * Vec(1, 0));
    draw_line(args, color, pos, pos + size * Vec(0, 1));
    draw_line(args, color, pos + size * Vec(1, 0), pos + size * Vec(1, 1));
    draw_line(args, color, pos + size * Vec(0, 1), pos + size * Vec(1, 1));
}

void draw_h_line(const DrawArgs& args, NVGcolor color, Rect rect, float pos_ratio) {
    const float y_line = floor(pos_ratio * rect.size.y);
    nvgStrokeWidth(args.vg, 0.8);
    draw_line(args, color, rect.pos + Vec(0, y_line), rect.pos + Vec(rect.size.x, y_line));
}

void draw_v_line(const DrawArgs& args, NVGcolor color, Rect rect, float pos_ratio) {
    const float x_line = floor(pos_ratio * rect.size.x);
    nvgStrokeWidth(args.vg, 0.8);
    draw_line(args, color, rect.pos + Vec(x_line, 0), rect.pos + Vec(x_line, rect.size.y));
}

void draw_text(const DrawArgs& args, NVGcolor color, Rect rect, std::string text) {
    nvgFontSize(args.vg, 8);
    nvgTextLetterSpacing(args.vg, 0);
    nvgFillColor(args.vg, color);
    nvgTextBox(args.vg, rect.pos.x + 2, rect.pos.y + rect.size.y - 2, rect.size.x, text.c_str(), NULL);
}

struct SplitResult {
    Rect A;
    Rect B;
};

static auto split_rect_h(const Rect rect, float ratio) -> SplitResult {
    const float h = rect.getHeight() * ratio;
    return SplitResult {A: Rect(rect.pos, Vec(rect.size.x, h)), B: Rect(rect.pos + Vec(0, h), rect.size - Vec(0, h))};
}

static auto split_rect_v(const Rect rect, float ratio) -> SplitResult {
    const float w = rect.getWidth() * ratio;
    return SplitResult {A: Rect(rect.pos, Vec(w, rect.size.y)), B: Rect(rect.pos + Vec(w, 0), rect.size - Vec(w, 0))};
}
};  // namespace rage