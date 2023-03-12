#pragma once

#include <functional>
#include <string>
#include <iostream>

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
const float UI_update_time = 1.f / 60.f;

struct RoundGrayKnob: app::SvgKnob {
    RoundGrayKnob() {
        minAngle = -0.75 * M_PI;
        maxAngle = 0.75 * M_PI;
        setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Components/RoundGrayKnob.svg")));
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

template<class ModuleType>
struct LoadButton: RubberSmallButton {
    void onDragEnd(const event::DragEnd& e) override {
        ParamQuantity* paramQuantity = getParamQuantity();
        ModuleType* module = static_cast<ModuleType*>(paramQuantity->module);
        if (module) {
            std::string dir = module->get_last_directory();
            std::string filename;


            if (dir == "") {
                dir = asset::user("");
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

}  // namespace rage