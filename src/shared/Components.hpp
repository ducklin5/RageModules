#pragma once

#include <functional>
#include <iostream>
#include <string>

#include "../plugin.hpp"
#include "osdialog.h"
#include "rack.hpp"

/*
References:
    - LOMAS Components: https://github.com/LomasModules/LomasModules/blob/b1e8edf1e11e2f725b8f27c8091613f9eda86e37/src/components.hpp
        - Used as place holder logic

TODO: Replace placeholder 
*/

namespace rage {
const float UI_update_time = 1.f / 15.f;

struct RoundGrayKnob: app::SvgKnob {
    RoundGrayKnob() {
        minAngle = -0.75 * M_PI;
        maxAngle = 0.75 * M_PI;
        setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RoundGrayKnob.svg")));
    }
};

struct RoundSmallGraySnapKnob: app::SvgKnob {
    RoundSmallGraySnapKnob() {
        snap = true;
        minAngle = -0.75 * M_PI;
        maxAngle = 0.75 * M_PI;
        setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RoundSmallGrayKnob.svg")));
    }
};

template<class ParentModule>
struct RoundSmallGrayOmniKnob: app::SvgKnob {
    float previousValue = 0;

    RoundSmallGrayOmniKnob() {
        minAngle = -0.75 * M_PI;
        maxAngle = 0.75 * M_PI;
        speed = 0.1;
        setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RoundSmallGrayOmniKnob.svg")));
    }

    void initParamQuantity() override {
        ParamQuantity* paramQuantity = getParamQuantity();
        if (paramQuantity) {
            paramQuantity->maxValue = INFINITY;
            paramQuantity->minValue = -INFINITY;
        }
    }

    void onChange(const ChangeEvent& e) override {
        SvgKnob::onChange(e);
        ParamQuantity* paramQuantity = getParamQuantity();
        ParentModule* module = static_cast<ParentModule*>(paramQuantity->module);
        const int id = paramQuantity->paramId;
        const float value = paramQuantity->getValue();
        const float delta = value - previousValue;
        module->on_omni_knob_changed(id, delta);
        previousValue = value;
    }
};

struct RoundSmallGrayKnob: app::SvgKnob {
    RoundSmallGrayKnob() {
        minAngle = -0.75 * M_PI;
        maxAngle = 0.75 * M_PI;
        setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RoundSmallGrayKnob.svg")));
    }
};

struct RubberButton: app::SvgSwitch {
    RubberButton() {
        momentary = true;
        addFrame(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RubberButton.svg")));
        addFrame(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RubberButton1.svg")));
    }
};

struct RubberSmallButton: app::SvgSwitch {
    RubberSmallButton() {
        momentary = true;
        addFrame(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RubberSmallButton.svg")));
        addFrame(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RubberSmallButton1.svg")));
    }
};

template<typename BASE>
struct RubberButtonLed: BASE {
    RubberButtonLed() {
        this->borderColor = color::BLACK_TRANSPARENT;
        this->bgColor = color::BLACK_TRANSPARENT;
        this->box.size = window::mm2px(math::Vec(8, 8));
    }
};

template<typename BASE>
struct RubberSmallButtonLed: BASE {
    RubberSmallButtonLed() {
        this->borderColor = color::BLACK_TRANSPARENT;
        this->bgColor = color::BLACK_TRANSPARENT;
        this->box.size = window::mm2px(math::Vec(5, 5));
    }
};

template<class ParentModule>
struct LoadButton: RubberSmallButton {
    void onDragEnd(const event::DragEnd& e) override {
        ParamQuantity* paramQuantity = getParamQuantity();
        ParentModule* module = static_cast<ParentModule*>(paramQuantity->module);
        if (module) {
            std::string dir = module->get_last_directory();
            std::string filename;

            if (dir == "") {
                dir = asset::user("./Music/");
                filename = "Untitled";
            } else {
                filename = system::getFilename("Untitled");
            }

            char* path = osdialog_file(OSDIALOG_OPEN, dir.c_str(), filename.c_str(), NULL);
            std::cout << "Os Dialog selected: " << path << "\n";

            if (path) {
                module->load_file(std::string(path));
                free(path);
            }
        }

        RubberSmallButton::onDragEnd(e);
    }
};

template<class ParentModule>
struct SaveButton: RubberSmallButton {
    void onDragEnd(const event::DragEnd& e) override {
        ParamQuantity* paramQuantity = getParamQuantity();
        ParentModule* module = static_cast<ParentModule*>(paramQuantity->module);
        if (module) {
            if (module->can_save()) {
                std::string dir = module->get_last_directory();
                std::string filename;

                if (dir == "") {
                    dir = asset::user("./Music/");
                    filename = "Untitled";
                } else {
                    filename = system::getFilename("Untitled");
                }

                char* path = osdialog_file(OSDIALOG_SAVE, dir.c_str(), filename.c_str(), NULL);
                std::cout << "Os Dialog selected: " << path << "\n";

                if (path) {
                    module->save_file(std::string(path));
                    free(path);
                }
            }
        }

        RubberSmallButton::onDragEnd(e);
    }
};

enum WidgetType {
    WTRegularButton,
    WTLoadButton,
    WTSaveButton,
    WTSnapKnob,
    WTOmniKnob,
    WTInputPort
};

template<class ParentModule>
Widget* create_centered_widget(const WidgetType wtype, Vec pos, Module* module, int param_id) {
    switch (wtype) {
        case WTRegularButton:
            return createParamCentered<RubberSmallButton>(pos, module, param_id);
        case WTLoadButton:
            return createParamCentered<LoadButton<ParentModule>>(pos, module, param_id);
        case WTSaveButton:
            return createParamCentered<SaveButton<ParentModule>>(pos, module, param_id);
        case WTSnapKnob:
            return createParamCentered<RoundSmallGraySnapKnob>(pos, module, param_id);
        case WTOmniKnob:
            return createParamCentered<RoundSmallGrayOmniKnob<ParentModule>>(pos, module, param_id);
        case WTInputPort:
            return createInputCentered<PJ301MPort>(pos, module, param_id);
    }
    return nullptr;
}
}  // namespace rage
