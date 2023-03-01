/*
struct ScopeDisplay : LedDisplay {
	Scope* module;
	ModuleWidget* moduleWidget;
	int statsFrame = 0;
	std::string fontPath;

	struct Stats {
		float min = INFINITY;
		float max = -INFINITY;
	};
	Stats statsX;
	Stats statsY;

	ScopeDisplay() {
		fontPath = asset::system("res/fonts/ShareTechMono-Regular.ttf");
	}

	void calculateStats(Stats& stats, int wave, int channels) {
		if (!module)
			return;

		stats = Stats();
		for (int i = 0; i < BUFFER_SIZE; i++) {
			Scope::Point point = module->pointBuffer[i];
			for (int c = 0; c < channels; c++) {
				float max = (wave == 0) ? point.maxX[c] : point.maxY[c];
				float min = (wave == 0) ? point.minX[c] : point.minY[c];

				stats.max = std::fmax(stats.max, max);
				stats.min = std::fmin(stats.min, min);
			}
		}
	}

	void drawWave(const DrawArgs& args, int wave, int channel, float offset, float gain) {
		if (!module)
			return;

		// Copy point buffer to stack to prevent min/max values being different phase.
		// This is currently only 256*4*16*4 = 64 kiB.
		Scope::Point pointBuffer[BUFFER_SIZE];
		std::copy(std::begin(module->pointBuffer), std::end(module->pointBuffer), std::begin(pointBuffer));

		nvgSave(args.vg);
		Rect b = box.zeroPos().shrink(Vec(0, 15));
		nvgScissor(args.vg, RECT_ARGS(b));
		nvgBeginPath(args.vg);
		// Draw max points on top
		for (int i = 0; i < BUFFER_SIZE; i++) {
			const Scope::Point& point = pointBuffer[i];
			float max = (wave == 0) ? point.maxX[channel] : point.maxY[channel];
			if (!std::isfinite(max))
				max = 0.f;

			Vec p;
			p.x = (float) i / (BUFFER_SIZE - 1);
			p.y = (max + offset) * gain * -0.5f + 0.5f;
			p = b.interpolate(p);
			p.y -= 1.0;
			if (i == 0)
				nvgMoveTo(args.vg, p.x, p.y);
			else
				nvgLineTo(args.vg, p.x, p.y);
		}
		// Draw min points on bottom
		for (int i = BUFFER_SIZE - 1; i >= 0; i--) {
			const Scope::Point& point = pointBuffer[i];
			float min = (wave == 0) ? point.minX[channel] : point.minY[channel];
			if (!std::isfinite(min))
				min = 0.f;

			Vec p;
			p.x = (float) i / (BUFFER_SIZE - 1);
			p.y = (min + offset) * gain * -0.5f + 0.5f;
			p = b.interpolate(p);
			p.y += 1.0;
			nvgLineTo(args.vg, p.x, p.y);
		}
		nvgClosePath(args.vg);
		// nvgLineCap(args.vg, NVG_ROUND);
		// nvgMiterLimit(args.vg, 2.f);
		nvgGlobalCompositeOperation(args.vg, NVG_LIGHTER);
		nvgFill(args.vg);
		nvgResetScissor(args.vg);
		nvgRestore(args.vg);
	}

	void drawLissajous(const DrawArgs& args, int channel, float offsetX, float gainX, float offsetY, float gainY) {
		if (!module)
			return;

		Scope::Point pointBuffer[BUFFER_SIZE];
		std::copy(std::begin(module->pointBuffer), std::end(module->pointBuffer), std::begin(pointBuffer));

		nvgSave(args.vg);
		Rect b = box.zeroPos().shrink(Vec(0, 15));
		nvgScissor(args.vg, RECT_ARGS(b));
		nvgBeginPath(args.vg);
		int bufferIndex = module->bufferIndex;
		for (int i = 0; i < BUFFER_SIZE; i++) {
			// Get average point
			const Scope::Point& point = pointBuffer[(i + bufferIndex) % BUFFER_SIZE];
			float avgX = (point.minX[channel] + point.maxX[channel]) / 2;
			float avgY = (point.minY[channel] + point.maxY[channel]) / 2;
			if (!std::isfinite(avgX) || !std::isfinite(avgY))
				continue;

			Vec p;
			p.x = (avgX + offsetX) * gainX * 0.5f + 0.5f;
			p.y = (avgY + offsetY) * gainY * -0.5f + 0.5f;
			p = b.interpolate(p);
			if (i == 0)
				nvgMoveTo(args.vg, p.x, p.y);
			else
				nvgLineTo(args.vg, p.x, p.y);
		}
		nvgLineCap(args.vg, NVG_ROUND);
		nvgMiterLimit(args.vg, 2.f);
		nvgStrokeWidth(args.vg, 1.5f);
		nvgGlobalCompositeOperation(args.vg, NVG_LIGHTER);
		nvgStroke(args.vg);
		nvgResetScissor(args.vg);
		nvgRestore(args.vg);
	}

	void drawTrig(const DrawArgs& args, float value) {
		Rect b = Rect(Vec(0, 15), box.size.minus(Vec(0, 15 * 2)));
		nvgScissor(args.vg, b.pos.x, b.pos.y, b.size.x, b.size.y);

		value = value / 2.f + 0.5f;
		Vec p = Vec(box.size.x, b.pos.y + b.size.y * (1.f - value));

		// Draw line
		nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, 0x10));
		{
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, p.x - 13, p.y);
			nvgLineTo(args.vg, 0, p.y);
		}
		nvgStroke(args.vg);

		// Draw indicator
		nvgFillColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, 0x60));
		{
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, p.x - 2, p.y - 4);
			nvgLineTo(args.vg, p.x - 9, p.y - 4);
			nvgLineTo(args.vg, p.x - 13, p.y);
			nvgLineTo(args.vg, p.x - 9, p.y + 4);
			nvgLineTo(args.vg, p.x - 2, p.y + 4);
			nvgClosePath(args.vg);
		}
		nvgFill(args.vg);

		std::shared_ptr<Font> font = APP->window->loadFont(fontPath);
		if (font) {
			nvgFontSize(args.vg, 9);
			nvgFontFaceId(args.vg, font->handle);
			nvgFillColor(args.vg, nvgRGBA(0x1e, 0x28, 0x2b, 0xff));
			nvgText(args.vg, p.x - 8, p.y + 3, "T", NULL);
		}
		nvgResetScissor(args.vg);
	}

	void drawStats(const DrawArgs& args, Vec pos, const char* title, const Stats& stats) {
		std::shared_ptr<Font> font = APP->window->loadFont(fontPath);
		if (!font)
			return;
		nvgFontSize(args.vg, 13);
		nvgFontFaceId(args.vg, font->handle);
		nvgTextLetterSpacing(args.vg, -2);

		nvgFillColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, 0x40));
		nvgText(args.vg, pos.x + 6, pos.y + 11, title, NULL);

		nvgFillColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, 0x80));
		pos = pos.plus(Vec(22, 11));

		std::string text;
		text = "pp ";
		float pp = stats.max - stats.min;
		text += isNear(pp, 0.f, 100.f) ? string::f("% 6.2f", pp) : "  ---";
		nvgText(args.vg, pos.x, pos.y, text.c_str(), NULL);
		text = "max ";
		text += isNear(stats.max, 0.f, 100.f) ? string::f("% 6.2f", stats.max) : "  ---";
		nvgText(args.vg, pos.x + 58 * 1, pos.y, text.c_str(), NULL);
		text = "min ";
		text += isNear(stats.min, 0.f, 100.f) ? string::f("% 6.2f", stats.min) : "  ---";
		nvgText(args.vg, pos.x + 58 * 2, pos.y, text.c_str(), NULL);
	}

	void drawBackground(const DrawArgs& args) {
		Rect b = box.zeroPos().shrink(Vec(0, 15));

		nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, 0x10));
		for (int i = 0; i < 5; i++) {
			nvgBeginPath(args.vg);

			Vec p;
			p.x = 0.0;
			p.y = float(i) / (5 - 1);
			nvgMoveTo(args.vg, VEC_ARGS(b.interpolate(p)));

			p.x = 1.0;
			nvgLineTo(args.vg, VEC_ARGS(b.interpolate(p)));
			nvgStroke(args.vg);
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;

		// Background lines
		drawBackground(args);

		if (!module)
			return;

		float gainX = std::pow(2.f, std::round(module->params[Scope::X_SCALE_PARAM].getValue())) / 10.f;
		float gainY = std::pow(2.f, std::round(module->params[Scope::Y_SCALE_PARAM].getValue())) / 10.f;
		float offsetX = module->params[Scope::X_POS_PARAM].getValue();
		float offsetY = module->params[Scope::Y_POS_PARAM].getValue();

		// Get input colors
		PortWidget* inputX = moduleWidget->getInput(Scope::X_INPUT);
		PortWidget* inputY = moduleWidget->getInput(Scope::Y_INPUT);
		CableWidget* inputXCable = APP->scene->rack->getTopCable(inputX);
		CableWidget* inputYCable = APP->scene->rack->getTopCable(inputY);
		NVGcolor inputXColor = inputXCable ? inputXCable->color : color::WHITE;
		NVGcolor inputYColor = inputYCable ? inputYCable->color : color::WHITE;

		// Draw waveforms
		if (module->isLissajous()) {
			// X x Y
			int lissajousChannels = std::min(module->channelsX, module->channelsY);
			for (int c = 0; c < lissajousChannels; c++) {
				nvgStrokeColor(args.vg, SCHEME_YELLOW);
				drawLissajous(args, c, offsetX, gainX, offsetY, gainY);
			}
		}
		else {
			// Y
			for (int c = 0; c < module->channelsY; c++) {
				nvgFillColor(args.vg, inputYColor);
				drawWave(args, 1, c, offsetY, gainY);
			}

			// X
			for (int c = 0; c < module->channelsX; c++) {
				nvgFillColor(args.vg, inputXColor);
				drawWave(args, 0, c, offsetX, gainX);
			}

			// Trigger
			float trigThreshold = module->params[Scope::THRESH_PARAM].getValue();
			trigThreshold = (trigThreshold + offsetX) * gainX;
			drawTrig(args, trigThreshold);
		}

		// Calculate and draw stats
		if (++statsFrame >= 4) {
			statsFrame = 0;
			calculateStats(statsX, 0, module->channelsX);
			calculateStats(statsY, 1, module->channelsY);
		}
		drawStats(args, Vec(0, 0 + 1), "1", statsX);
		drawStats(args, Vec(0, box.size.y - 15 - 1), "2", statsY);
	}
};
*/