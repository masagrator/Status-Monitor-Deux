#include <charconv>
int64_t COMMON_MARGIN;

class RenderingPipeline : public tsl::Gui {
private:
	uint64_t mappedButtons = HidNpadButton_B;
	uint64_t leftJoyconMotionMappedButtons = MapButtons(leftJoyconMotionKeyCombo);
	uint64_t rightJoyconMotionMappedButtons = MapButtons(rightJoyconMotionKeyCombo);
	uint64_t proControllerMotionMappedButtons = MapButtons(proControllerMotionKeyCombo);
	uint8_t* displayRefreshRate = 0;
	bool UseCustomExitCombo = false;
	bool focus = true;
	std::string name;
	std::string rel_filepath;
	std::string error;
	smd::Document doc;
	bool HeaderText = true;
	bool FooterText = true;
	bool Movable = false;
	bool ClampToLayerRight = false;
	bool ClampToLayerBottom = false;
	size_t threads_count = 0;
	Thread threads[6];
	uint64_t timeout = 15'000'000;
	std::string ComboButtonFooter;
	bool changingPos = false;
	bool sixaxisChangingPos = false;
	bool changedPos = false;
	bool reachedMaxX = false;
	bool reachedMaxY = false;
	int64_t touch_pos_x = -1;
	int64_t touch_pos_y = -1;
	uint32_t m_base_x = 0;
	uint32_t m_base_y = 0;
	int64_t m_anchor_offset_x = 0;
	int64_t m_anchor_offset_y = 0;
	// Saved screen-space top-left to restore on first frame with valid bounds.
	// -1 means no restore pending.
	int64_t m_saved_base_x = -1;
	int64_t m_saved_base_y = -1;

	struct TouchRect { int64_t x, y, w, h; };
	static inline std::vector<TouchRect> s_rects{};

	int64_t m_layer_pos_x_window = 0;
	int64_t m_layer_pos_y_window = 0;
	
	static inline int64_t m_obj_offset_x_screen = 0;
	static inline int64_t m_obj_offset_y_screen = 0;
	
	static void TrackRect(int64_t x, int64_t y, int64_t w, int64_t h) {
		if (w <= 0 || h <= 0) return;
		for (const auto& r : s_rects) {
			if (x >= r.x && y >= r.y
			 && x + w <= r.x + r.w && y + h <= r.y + r.h) return;
		}
		s_rects.push_back({x, y, w, h});
	}

	static void RenderGraphLineChart(smd::RenderCommand& cmd, tsl::gfx::Renderer* m_renderer) {
		uint16_t lineColor = cmd.color;
		uint16_t fillColor = cmd.fillColor;
		uint16_t previousLineColor1 = lineColor;
		uint16_t previousLineColor2 = fillColor;

		const int64_t denomX = std::max<int64_t>((int64_t)cmd.sampleCapacity - 1, 1);
		const int64_t denomY = std::max<int64_t>(cmd.maxClamp - cmd.minClamp, 1);
		const int64_t spanX  = std::max<int64_t>(cmd.width  - 1, 0);
		auto scaleY = [&](int64_t v) -> int64_t {
			return (v - cmd.minClamp) * cmd.height / denomY;
		};

		auto scaleX_LTR = [&](int64_t i) -> int64_t {
			int64_t age = (int64_t)cmd.sampleCount - 1 - i;
			return age * spanX / denomX;
		};
		auto scaleX_RTL = [&](int64_t i) -> int64_t {
			int64_t age = (int64_t)cmd.sampleCount - 1 - i;
			return spanX - age * spanX / denomX;
		};

		auto drawing_impl = [&](smd::GraphDirection direction, int64_t i) {
			bool conditionMet = false;
			for (size_t c = 0; c < cmd.condCount; c++) {
				const auto& cond = cmd.conditions[c];
				if (cond.matches(cond.state, cmd.sampleType == smd::HistorySampleType::Int ? cmd.samples[i] : cmd.samplesD[i])) {
					conditionMet = true;
					if (cond.lineColor == previousLineColor1 && cond.fillColor == previousLineColor2) {
						lineColor = cond.lineColor;
						fillColor = cond.fillColor;
					}
					previousLineColor1 = cond.lineColor;
					previousLineColor2 = cond.fillColor;
					break;
				}
			}
			if (!conditionMet) {
				lineColor = cmd.color;
				fillColor = cmd.fillColor;
				previousLineColor1 = lineColor;
				previousLineColor2 = fillColor;
			}
			int64_t x1 = COMMON_MARGIN + cmd.x + (cmd.direction == smd::GraphDirection::LeftToRight ? scaleX_LTR(i) : scaleX_RTL(i));
			int64_t x2 = COMMON_MARGIN + cmd.x + (cmd.direction == smd::GraphDirection::LeftToRight ? scaleX_LTR(i+1) : scaleX_RTL(i+1));
			if (cmd.sampleType == smd::HistorySampleType::Int) {
				auto value1 = std::clamp(cmd.samples[i], cmd.minClamp, cmd.maxClamp);
				auto value2 = std::clamp(cmd.samples[i+1], cmd.minClamp, cmd.maxClamp);
				int64_t y1 = cmd.y + (cmd.height - scaleY(value1));
				int64_t y2 = cmd.y + (cmd.height - scaleY(value2));
				m_renderer->drawLine(x1, y1, x2, y2, lineColor);
			}
			else {
				auto value1 = std::round(std::clamp(cmd.samplesD[i], (double)cmd.minClamp, (double)cmd.maxClamp));
				auto value2 = std::round(std::clamp(cmd.samplesD[i+1], (double)cmd.minClamp, (double)cmd.maxClamp));
				int64_t y1 = cmd.y + (cmd.height - scaleY((int64_t)value1));
				int64_t y2 = cmd.y + (cmd.height - scaleY((int64_t)value2));
				m_renderer->drawLine(x1, y1, x2, y2, lineColor);
			}
		};

		if (cmd.direction == smd::GraphDirection::LeftToRight) {
			for (int64_t i = 0; i < (int64_t)cmd.sampleCount - 1; i++) {
				drawing_impl(cmd.direction, i);
			}
		}
		else for (int64_t i = cmd.sampleCount - 2; i >= 0; i--) {
			drawing_impl(cmd.direction, i);
		}
	}

	static void PreRecordCallback(std::string& outLocaleCode, void* user) {
		outLocaleCode = overrideLanguage;
		return;
	}

	static void RecordCallback(smd::RenderCommand& cmd, void* user) {
		auto* m_renderer = static_cast<tsl::gfx::Renderer*>(user);

		const int64_t orig_x = cmd.x, orig_y = cmd.y;
		cmd.x += m_obj_offset_x_screen;
		cmd.y += m_obj_offset_y_screen;
		const int64_t orig_x2 = cmd.x2, orig_y2 = cmd.y2;
		if (cmd.type == smd::RenderCmdType::DashedLine) {
			cmd.x2 += m_obj_offset_x_screen;
			cmd.y2 += m_obj_offset_y_screen;
		}
		switch(cmd.type) {
			case smd::RenderCmdType::Text: {
				auto dims = m_renderer->drawString(cmd.text.c_str(), false, COMMON_MARGIN + cmd.x, cmd.y, cmd.fontSize, m_renderer->a(cmd.color));
				TrackRect(COMMON_MARGIN + orig_x, orig_y, dims.first, dims.second);
				break;
			}
			case smd::RenderCmdType::Box:
				m_renderer->drawRect(COMMON_MARGIN + cmd.x, cmd.y, cmd.width, cmd.height, m_renderer->a(cmd.color));
				TrackRect(COMMON_MARGIN + orig_x, orig_y, cmd.width, cmd.height);
				break;
			case smd::RenderCmdType::RoundedBox:
				m_renderer->drawRoundRect(COMMON_MARGIN + cmd.x, cmd.y, cmd.width, cmd.height, cmd.roundnessTl, cmd.roundnessTr, cmd.roundnessBl, cmd.roundnessBr, m_renderer->a(cmd.color));
				TrackRect(COMMON_MARGIN + orig_x, orig_y, cmd.width, cmd.height);
				break;
			case smd::RenderCmdType::EmptyBox:
				m_renderer->drawEmptyRect(COMMON_MARGIN + cmd.x, cmd.y, cmd.width, cmd.height, m_renderer->a(cmd.color));
				TrackRect(COMMON_MARGIN + orig_x, orig_y, cmd.width, cmd.height);
				break;
			case smd::RenderCmdType::DashedLine: {
				m_renderer->drawDashedLine(COMMON_MARGIN + cmd.x, cmd.y, cmd.x2, cmd.y2, cmd.dashOn, cmd.dashOff, m_renderer->a(cmd.color));

				int64_t a = COMMON_MARGIN + orig_x, b = orig_x2;
				int64_t c = orig_y,                 d = orig_y2;
				int64_t x0 = a < b ? a : b, x1 = a < b ? b : a;
				int64_t y0 = c < d ? c : d, y1 = c < d ? d : c;
				TrackRect(x0, y0, x1 - x0 > 0 ? x1 - x0 : 1, y1 - y0 > 0 ? y1 - y0 : 1);

				break;
			}
			case smd::RenderCmdType::GetDimensions: {
				auto dimensions = m_renderer->drawString(cmd.text.c_str(), false, 0, cmd.fontSize, cmd.fontSize, m_renderer->a(0x0000));
				cmd.outDims->x = dimensions.first;
				cmd.outDims->y = dimensions.second;
				break;
			}
			case smd::RenderCmdType::GraphLineChart:
				RenderGraphLineChart(cmd, m_renderer);
				TrackRect(COMMON_MARGIN + orig_x, orig_y, cmd.width, cmd.height);
				break;
		}
		cmd.x = orig_x; cmd.y = orig_y;
		cmd.x2 = orig_x2; cmd.y2 = orig_y2;
	}

	static void DryRunCallback(smd::RenderCommand& cmd, void* user) {
		auto* m_renderer = static_cast<tsl::gfx::Renderer*>(user);
		switch(cmd.type) {
			case smd::RenderCmdType::Text: {
				auto dims = m_renderer->drawString(cmd.text.c_str(), false, COMMON_MARGIN + cmd.x, cmd.y, cmd.fontSize, m_renderer->a(0x0000));
				TrackRect(COMMON_MARGIN + cmd.x, cmd.y, dims.first, dims.second);
				break;
			}
			case smd::RenderCmdType::Box:
			case smd::RenderCmdType::RoundedBox:
			case smd::RenderCmdType::EmptyBox:
				TrackRect(COMMON_MARGIN + cmd.x, cmd.y, cmd.width, cmd.height);
				break;
			case smd::RenderCmdType::DashedLine: {
				int64_t a = COMMON_MARGIN + cmd.x, b = cmd.x2;
				int64_t c = cmd.y,                 d = cmd.y2;
				int64_t x0 = a < b ? a : b, x1 = a < b ? b : a;
				int64_t y0 = c < d ? c : d, y1 = c < d ? d : c;
				TrackRect(x0, y0, x1 - x0 > 0 ? x1 - x0 : 1, y1 - y0 > 0 ? y1 - y0 : 1);
				break;
			}
			case smd::RenderCmdType::GetDimensions: {
				auto dimensions = m_renderer->drawString(cmd.text.c_str(), false, 0, cmd.fontSize, cmd.fontSize, m_renderer->a(0x0000));
				cmd.outDims->x = dimensions.first;
				cmd.outDims->y = dimensions.second;
				break;
			}
			case smd::RenderCmdType::GraphLineChart:
				TrackRect(COMMON_MARGIN + cmd.x, cmd.y, cmd.width, cmd.height);
				break;
		}
	}

	bool IsInsideTouchRange(int64_t screen_x, int64_t screen_y) const {
		if (screen_x < 0 || screen_y < 0) return false;

		int64_t lx = screen_x - (m_layer_pos_x_window * 2 / 3) - m_obj_offset_x_screen;
		int64_t ly = screen_y - (m_layer_pos_y_window * 2 / 3) - m_obj_offset_y_screen;
		if (lx < 0 || ly < 0) return false;
		if (backgroundColor != 0x0000)
			return lx < tsl::cfg::FramebufferWidth && ly < tsl::cfg::FramebufferHeight;
		for (const auto& r : s_rects)
			if (lx >= r.x && ly >= r.y && lx < r.x+r.w && ly < r.y+r.h) return true;
		return false;
	}

public:
    RenderingPipeline(std::string filepath) {
		m_layer_pos_x_window  = 0;
		m_layer_pos_y_window  = 0;
		m_obj_offset_x_screen = 0;
		m_obj_offset_y_screen = 0;
		tsl::gfx::Renderer::getRenderer().setLayerPos(0, 0);
		if (SaltySD) {
			uintptr_t base = (uintptr_t)shmemGetAddr(&_sharedmemory);
			if (base) displayRefreshRate = (uint8_t*)(base + 1);
		}
		tsl::hlp::requestForeground(true);
		ApmPerformanceMode performanceMode;
		SystemData.IsDocked = false;
		if (R_SUCCEEDED(apmGetPerformanceMode(&performanceMode))) {
			if (performanceMode == ApmPerformanceMode_Boost) SystemData.IsDocked = true;
		}
		std::string formattedKeyCombo = keyCombo;
		formatButtonCombination(formattedKeyCombo);
		SystemData.formattedKeyCombo = formattedKeyCombo;
		smd::Document::PeekInfo info;
		smd::Document::Peek(filepath.c_str(), info, overrideLanguage.c_str());
		name = info.name;
		doc.Free();
		doc.SetRecordCallback(PreRecordCallback, nullptr);
		if (doc.LoadFromFile(filepath.c_str()) == false) {
			error = doc.LastError();
			return;
		}
		BindAllPredefined(doc);
		if (doc.Compile() == false) {
			error = doc.LastError();
			return;
		}
		Movable = doc.GetConfigBool("Movable", false);
		if (Movable && saveAndLoadMovableOverlayPosition) {
			uint32_t crc32 = doc.GetFileHash();
			uint16_t saved_x_pos;
			uint16_t saved_y_pos;
			rel_filepath = filepath.erase(0, strlen("sdmc:/config/status-monitor-deux/modes/"));
			bool wasSuccess = ProcessSmdSettings(rel_filepath, crc32, &saved_x_pos, &saved_y_pos);
			if (wasSuccess == false) {
				saved_x_pos = 0;
				saved_y_pos = 0;
				char buffer[10] = "";
				auto [ptr, ec] = std::to_chars(&buffer[0], &buffer[sizeof(buffer)], crc32, 16);
				if (ec == std::errc{}) {
					std::string value = std::string(&buffer[0], ptr - &buffer[0]);
					setIniFile("sdmc:/config/status-monitor-deux/config.ini", rel_filepath, "hash", value, "");
					setIniFile("sdmc:/config/status-monitor-deux/config.ini", rel_filepath, "x", "0", "");
					setIniFile("sdmc:/config/status-monitor-deux/config.ini", rel_filepath, "y", "0", "");
				}
			}
			else if (saved_x_pos > 1280 || saved_y_pos > 720) {
				saved_x_pos = 0;
				saved_y_pos = 0;
			}
			// saved_x/y_pos is the screen-space top-left (m_base_x/y) recorded at
			// last close. We can't decompose it into layer+obj yet because s_rects
			// (needed for mnx/mny) don't exist until the first Evaluate. Store it
			// and apply in handleInput on the first frame that has valid bounds.
			m_saved_base_x = (int64_t)saved_x_pos;
			m_saved_base_y = (int64_t)saved_y_pos;
			if (saved_x_pos == 1280) reachedMaxX = true;
			if (saved_y_pos == 720) reachedMaxY = true;
		}
		HeaderText = doc.GetConfigBool("HeaderText", true);
		FooterText = doc.GetConfigBool("FooterText", true);
		focus = doc.GetConfigBool("EnableControls", true);
		tsl::hlp::requestForeground(focus);
		UseCustomExitCombo = doc.GetConfigBool("UseCustomExitCombo", false);
		if (FooterText == false) {
			deactivateOriginalFooter = true;
			FullMode = false;
		}
		if (UseCustomExitCombo == true) {
			deactivateOriginalFooter = true;
			mappedButtons = MapButtons(keyCombo);
			if (doc.FormatConfigString("ComboButtonFooter", ComboButtonFooter) == false) {
				ComboButtonFooter = "Hold ";
				ComboButtonFooter += keyCombo + " to Exit+";
			}
		}
		COMMON_MARGIN = doc.GetConfigInt("COMMON_MARGIN", 20);
		auto test = doc.GetConfigInt("User_BackgroundColor", 0xFFFFFF);
		if (test == 0xFFFFFF) backgroundColor = (uint16_t)doc.GetConfigInt("BackgroundColor", 0xD000);
		else backgroundColor = (uint16_t)test;
		ClampToLayerRight  = doc.GetConfigBool("ClampToLayerRight",  false);
		ClampToLayerBottom = doc.GetConfigBool("ClampToLayerBottom", false);
		if (Movable && motionControl) {
			hidsysSetAppletResourceUserId();
			hidStartSixAxisSensor(sixaxisHandles[Controller_ProController]);
			hidStartSixAxisSensor(sixaxisHandles[Controller_JoyConL]);
			hidStartSixAxisSensor(sixaxisHandles[Controller_JoyConR]);
		}
		int64_t overlayRefreshRate = doc.GetConfigInt("User_RefreshRate", 0);
		if (overlayRefreshRate > 0) timeout = 1'000'000'000 / overlayRefreshRate;
		if (timeout < 15'000'000) timeout = 15'000'000;
		bool EnableCPU = doc.GetConfigBool("EnableCPU", false);
		bool EnableGPU = doc.GetConfigBool("EnableGPU", false);
		bool EnableRAM = doc.GetConfigBool("EnableRAM", false);
		bool EnableBoard = doc.GetConfigBool("EnableBoard", false);		
		bool EnableMisc = doc.GetConfigBool("EnableMisc", false);
		bool EnableGame = doc.GetConfigBool("EnableGame", false);
		if (EnableCPU)   threadCreate(&threads[threads_count++], CpuThreadLoop, (void*)overlayRefreshRate, NULL, 0x1000, 0x3F, -2);
		if (EnableGPU)   threadCreate(&threads[threads_count++], GpuThreadLoop, NULL, NULL, 0x1000, 0x3F, -2);
		if (EnableRAM)   threadCreate(&threads[threads_count++], RamThreadLoop, NULL, NULL, 0x1000, 0x3F, -2);
		if (EnableBoard) threadCreate(&threads[threads_count++], BoardThreadLoop, NULL, NULL, 0x1000, 0x3F, -2);
		if (EnableMisc)  threadCreate(&threads[threads_count++], MiscThreadLoop, NULL, NULL, 0x1000, 0x3F, -2);
		if (EnableGame)  threadCreate(&threads[threads_count++], GameThreadLoop, (void*)overlayRefreshRate, NULL, 0x1000, 0x3F, -2);
		if (threads_count > 0) {
			threadexit2 = 0;
			leventClear(&threadexit);
		}
		for (size_t i = 0; i < threads_count; i++) {
			threadStart(&threads[i]);
		}
	}

	~RenderingPipeline() {
		if (Movable && saveAndLoadMovableOverlayPosition) {
			char buffer[10] = {0};
			char buffer2[10] = {0};
			if (reachedMaxX == true) m_base_x = 1280;
			if (reachedMaxY == true) m_base_y = 720;
			auto [ptr, ec] = std::to_chars(&buffer[0], &buffer[sizeof(buffer)], m_base_x, 10);
			std::string value = buffer;
			auto [ptr2, ec2] = std::to_chars(&buffer2[0], &buffer2[sizeof(buffer2)], m_base_y, 10);
			std::string value2 = buffer2;
			if (ec == std::errc{} && ec2 == std::errc{}) {
				setIniFile("sdmc:/config/status-monitor-deux/config.ini", rel_filepath, "x", value, "");
				setIniFile("sdmc:/config/status-monitor-deux/config.ini", rel_filepath, "y", value2, "");
			}
		}
		m_obj_offset_x_screen = 0;
		m_obj_offset_y_screen = 0;
		tsl::gfx::Renderer::getRenderer().setLayerPos(0, 0);
		svcSignalToAddress(&threadexit2, SignalType_SignalAndIncrementIfEqual, 0, 4);
		leventSignal(&threadexit);
		if (Movable & motionControl) {
			hidStopSixAxisSensor(sixaxisHandles[Controller_ProController]);
			hidStopSixAxisSensor(sixaxisHandles[Controller_JoyConL]);
			hidStopSixAxisSensor(sixaxisHandles[Controller_JoyConR]);
		}
		backgroundColor = 0xD000;
		deactivateOriginalFooter = false;
		FullMode = true;
		tsl::hlp::requestForeground(true);
		for (size_t i = 0; i < threads_count; i++) {
			threadWaitForExit(&threads[i]);
			threadClose(&threads[i]);
		}
		s_rects.clear();
	}

    virtual tsl::elm::Element* createUI() override {
		if (HeaderText == true)
			rootFrame = new tsl::elm::OverlayFrame(APP_TITLE, name);
		else rootFrame = new tsl::elm::OverlayFrame("", "");
		auto Status = new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *renderer, u16 x, u16 y, u16 w, u16 h) {
			if (error.length() > 0) {
				renderer->drawString(error.c_str(), false, 20, 120, 20, renderer->a(0xFFFF));
			}
			else {
				// A DryRun is needed when clamp flags require it, or when a
				// saved position is pending (always the case on frame 1, since
				// update() has already cleared s_rects before createUI runs).
				// Run it once and use the results for both purposes.
				bool needsDryRun = ((ClampToLayerRight || ClampToLayerBottom) && !changingPos)
				                 || (m_saved_base_x >= 0);
				if (needsDryRun) {
					doc.Evaluate(DryRunCallback, renderer);
					if (!s_rects.empty()) {
						int64_t mnx = s_rects[0].x, mny = s_rects[0].y;
						int64_t mxx = s_rects[0].x + s_rects[0].w;
						int64_t mxy = s_rects[0].y + s_rects[0].h;
						for (const auto& r : s_rects) {
							if (r.x < mnx) mnx = r.x;
							if (r.y < mny) mny = r.y;
							if (r.x + r.w > mxx) mxx = r.x + r.w;
							if (r.y + r.h > mxy) mxy = r.y + r.h;
						}
						int64_t layer_w = (int64_t)tsl::cfg::FramebufferWidth;
						int64_t layer_h = (int64_t)tsl::cfg::FramebufferHeight;
						// Apply saved position first (frame 1 restore).
						if (m_saved_base_x >= 0) {
							touch_pos_x    = m_saved_base_x + mnx;
							touch_pos_y    = m_saved_base_y + mny;
							m_anchor_offset_x = 0;
							m_anchor_offset_y = 0;
							// Decompose into layer + obj offset (same split as a real drag).
							int64_t T_screen_x = touch_pos_x - mnx;
							int64_t T_screen_y = touch_pos_y - mny;
							int64_t T_win_x = T_screen_x * 3 / 2;
							int64_t T_win_y = T_screen_y * 3 / 2;
							int64_t max_x = tsl::cfg::ScreenWidth  - (int)(1.5 * tsl::cfg::FramebufferWidth);
							int64_t max_y = tsl::cfg::ScreenHeight - (int)(1.5 * tsl::cfg::FramebufferHeight);
							if (max_x < 0) max_x = 0;
							if (max_y < 0) max_y = 0;
							m_layer_pos_x_window = std::clamp<int64_t>(T_win_x, 0, max_x);
							m_layer_pos_y_window = std::clamp<int64_t>(T_win_y, 0, max_y);
							int64_t raw_obj_x = T_screen_x - (m_layer_pos_x_window * 2 / 3);
							int64_t raw_obj_y = T_screen_y - (m_layer_pos_y_window * 2 / 3);
							int64_t obj_max_x = layer_w - mxx, obj_min_x = -mnx;
							int64_t obj_max_y = layer_h - mxy, obj_min_y = -mny;
							m_obj_offset_x_screen = std::clamp(raw_obj_x, obj_min_x, obj_max_x >= obj_min_x ? obj_max_x : obj_min_x);
							m_obj_offset_y_screen = std::clamp(raw_obj_y, obj_min_y, obj_max_y >= obj_min_y ? obj_max_y : obj_min_y);
							reachedMaxX = (m_obj_offset_x_screen + mxx >= layer_w);
							reachedMaxY = (m_obj_offset_y_screen + mxy >= layer_h);
							tsl::gfx::Renderer::getRenderer().setLayerPos((uint32_t)m_layer_pos_x_window, (uint32_t)m_layer_pos_y_window);
							touch_pos_x = -1;
							touch_pos_y = -1;
							m_saved_base_x = -1;
							m_saved_base_y = -1;
						}
						// Then apply edge clamps.
						if (ClampToLayerRight  && reachedMaxX) m_obj_offset_x_screen = layer_w - mxx;
						if (ClampToLayerBottom && reachedMaxY) m_obj_offset_y_screen = layer_h - mxy;
					}
					s_rects.clear();
					doc.Reset(true);
				}
				if (doc.Evaluate(RecordCallback, renderer) == false) {
					error = doc.LastError();
				}
			}
			if (deactivateOriginalFooter == true && FullMode == true) renderer->drawString(ComboButtonFooter.c_str(), false, 30, 693, 23, a(rootFrame->defaultTextColor));
		});

		rootFrame->setContent(Status);
		return rootFrame;
	}

	virtual void update() override {
		if (error.length() != 0) return;
		s_rects.clear();
		doc.Reset(changingPos);
		ApmPerformanceMode performanceMode;
		if (R_SUCCEEDED(apmGetPerformanceMode(&performanceMode))) {
			if (performanceMode == ApmPerformanceMode_Boost) SystemData.IsDocked = true;
			else SystemData.IsDocked = false;
		}
		if (displayRefreshRate) {
			SystemData.DisplayRefreshRate_int = *displayRefreshRate;
		}
	}

	virtual bool handleInput(uint64_t keysDown, uint64_t keysHeld, touchPosition touchInput, JoystickPosition leftJoyStick, JoystickPosition rightJoyStick) override {
		if (error.length() != 0) {
			if (isKeyComboPressed(keysHeld, keysDown, mappedButtons, 20'000'000)) {
				tsl::goBack();
				return true;
			}
			return false;			
		}
		static uint64_t last_time = armTicksToNs(svcGetSystemTick());

		int64_t mnx = 0, mny = 0;
		bool haveBounds = !s_rects.empty();
		if (haveBounds) {
			mnx = s_rects[0].x; mny = s_rects[0].y;
			for (const auto& r : s_rects) {
				if (r.x < mnx) mnx = r.x;
				if (r.y < mny) mny = r.y;
			}
			m_base_x = (uint32_t)std::max<int64_t>(0,
				mnx + (m_layer_pos_x_window * 2 / 3) + m_obj_offset_x_screen);
			m_base_y = (uint32_t)std::max<int64_t>(0,
				mny + (m_layer_pos_y_window * 2 / 3) + m_obj_offset_y_screen);
		}
		bool m_touchScreen = false;

		auto applyDragFromTouchPos = [&]() {
			if (!haveBounds) return;

			if (touch_pos_y >= 704) touch_pos_y = 720;
			else if (touch_pos_y <= 15) touch_pos_y = 0;
			if (touch_pos_x >= 1264) touch_pos_x = 1280;
			else if (touch_pos_x <= 15) touch_pos_x = 0;
			int64_t T_screen_x = touch_pos_x - m_anchor_offset_x - mnx;
			int64_t T_screen_y = touch_pos_y - m_anchor_offset_y - mny;

			int64_t mxx = s_rects[0].x + s_rects[0].w;
			int64_t mxy = s_rects[0].y + s_rects[0].h;
			for (const auto& r : s_rects) {
				if (r.x + r.w > mxx) mxx = r.x + r.w;
				if (r.y + r.h > mxy) mxy = r.y + r.h;
			}
			int64_t layer_w = (int64_t)tsl::cfg::FramebufferWidth;
			int64_t layer_h = (int64_t)tsl::cfg::FramebufferHeight;

			int64_t T_win_x = T_screen_x * 3 / 2;
			int64_t T_win_y = T_screen_y * 3 / 2;
			int64_t max_x = tsl::cfg::ScreenWidth  - (int)(1.5 * tsl::cfg::FramebufferWidth);
			int64_t max_y = tsl::cfg::ScreenHeight - (int)(1.5 * tsl::cfg::FramebufferHeight);
			if (max_x < 0) max_x = 0;
			if (max_y < 0) max_y = 0;
			m_layer_pos_x_window = std::clamp<int64_t>(T_win_x, 0, max_x);
			m_layer_pos_y_window = std::clamp<int64_t>(T_win_y, 0, max_y);

			int64_t raw_obj_x = T_screen_x - (m_layer_pos_x_window * 2 / 3);
			int64_t raw_obj_y = T_screen_y - (m_layer_pos_y_window * 2 / 3);

			int64_t obj_max_x = layer_w - mxx;
			int64_t obj_max_y = layer_h - mxy;
			int64_t obj_min_x = -mnx;
			int64_t obj_min_y = -mny;
			m_obj_offset_x_screen = std::clamp(raw_obj_x, obj_min_x, obj_max_x >= obj_min_x ? obj_max_x : obj_min_x);
			m_obj_offset_y_screen = std::clamp(raw_obj_y, obj_min_y, obj_max_y >= obj_min_y ? obj_max_y : obj_min_y);

			reachedMaxX = (m_obj_offset_x_screen + mxx >= layer_w);
			reachedMaxY = (m_obj_offset_y_screen + mxy >= layer_h);
			tsl::gfx::Renderer::getRenderer().setLayerPos((uint32_t)m_layer_pos_x_window, (uint32_t)m_layer_pos_y_window);
		};

		if (Movable == true) {
			if ((ClampToLayerRight || ClampToLayerBottom) && !changingPos && haveBounds) {
				int64_t mxx = s_rects[0].x + s_rects[0].w;
				int64_t mxy = s_rects[0].y + s_rects[0].h;
				for (const auto& r : s_rects) {
					if (r.x + r.w > mxx) mxx = r.x + r.w;
					if (r.y + r.h > mxy) mxy = r.y + r.h;
				}
				int64_t layer_w = (int64_t)tsl::cfg::FramebufferWidth;
				int64_t layer_h = (int64_t)tsl::cfg::FramebufferHeight;
				bool changed = false;
				if (ClampToLayerRight && reachedMaxX) {
					int64_t target_x = layer_w - mxx;
					if (m_obj_offset_x_screen != target_x) {
						m_obj_offset_x_screen = target_x;
						changed = true;
					}
				}
				if (ClampToLayerBottom && reachedMaxY) {
					int64_t target_y = layer_h - mxy;
					if (m_obj_offset_y_screen != target_y) {
						m_obj_offset_y_screen = target_y;
						changed = true;
					}
				}
				(void)changed;
			}
			m_touchScreen = touchScreen;
			if (__builtin_expect(m_touchScreen && (sixaxisChangingPos == false), false)) {
				if (!changingPos && *touchInput.delta_time != 0) {
					if (IsInsideTouchRange(*touchInput.x, *touchInput.y)) {
						changingPos = true;
						m_anchor_offset_x = (int64_t)*touchInput.x - (int64_t)m_base_x;
						m_anchor_offset_y = (int64_t)*touchInput.y - (int64_t)m_base_y;
					}
				}
				else if (changingPos && *touchInput.delta_time == 0) {
					touch_pos_x = -1;
					touch_pos_y = -1;
					changingPos = false;
				}
				if (changingPos) {
					touch_pos_x = *touchInput.x;
					touch_pos_y = *touchInput.y;
					applyDragFromTouchPos();
				}
			}
			if (changingPos == false || sixaxisChangingPos == true) {
				HidSixAxisSensorState sixaxis = {0};
				static bool start = false;
				s32 id = -1;
				u64 style_set = padGetStyleSet(&pad);
				if (style_set & HidNpadStyleTag_NpadJoyDual) {
					if ((keysHeld & leftJoyconMotionMappedButtons) == leftJoyconMotionMappedButtons) id = Controller_JoyConL;
					else if ((keysHeld & rightJoyconMotionMappedButtons) == rightJoyconMotionMappedButtons) id = Controller_JoyConR;
				}
				else if (style_set & HidNpadStyleTag_NpadJoyLeft) {
					if ((keysHeld & leftJoyconMotionMappedButtons) == leftJoyconMotionMappedButtons) id = Controller_JoyConL;
				}
				else if (style_set & HidNpadStyleTag_NpadJoyRight) {
					if ((keysHeld & rightJoyconMotionMappedButtons) == rightJoyconMotionMappedButtons) id = Controller_JoyConR;
				}
				else if (style_set & HidNpadStyleTag_NpadJoyRight) {
					if ((keysHeld & proControllerMotionMappedButtons) == proControllerMotionMappedButtons) id = Controller_ProController;
				}
				if (id < 0) {
					start = false;
					changingPos = false;
					sixaxisChangingPos = false;
				}
				else {
					static GyroCursor cursor;
					hidGetSixAxisSensorStates(sixaxisHandles[id], &sixaxis, 1);
					if (sixaxis.acceleration.x == 0.f && sixaxis.acceleration.y == 0.f && sixaxis.acceleration.z == -1.f) {
						hidsysSetAppletResourceUserId();
						hidGetSixAxisSensorStates(sixaxisHandles[id], &sixaxis, 1);
					}
					if (sixaxis.acceleration.x != 0.f || sixaxis.acceleration.y != 0.f || sixaxis.acceleration.z != -1.f) {
						if (start == false) {
							start = true;
							float sensitivity = 200;
							gyroCursor_init(cursor, (float)m_base_x, (float)m_base_y, sensitivity);
							m_anchor_offset_x = 0;
							m_anchor_offset_y = 0;
						}
						changingPos = true;
						sixaxisChangingPos = true;
						gyroCursor_update(cursor, sixaxis, &touch_pos_x, &touch_pos_y);
						applyDragFromTouchPos();
					}
					else {
						start = false;
						changingPos = false;
						sixaxisChangingPos = false;
					}
				}
			}
		}
		if (!changingPos) {
			uint64_t new_time = armTicksToNs(svcGetSystemTick());
			do {
				if (Movable == true && m_touchScreen == true) {
					HidTouchScreenState state = {0};
					if (hidGetTouchScreenStates(&state, 1) && state.count && IsInsideTouchRange(state.touches[0].x, state.touches[0].y)) {
						break;
					}
				}
				u64 style_set = padGetStyleSet(&pad);
				if (style_set & HidNpadStyleTag_NpadJoyDual) {
					if ((keysHeld & leftJoyconMotionMappedButtons) == leftJoyconMotionMappedButtons) break;
					else if ((keysHeld & rightJoyconMotionMappedButtons) == rightJoyconMotionMappedButtons) break;
				}
				else if (style_set & HidNpadStyleTag_NpadJoyLeft) {
					if ((keysHeld & leftJoyconMotionMappedButtons) == leftJoyconMotionMappedButtons) break;
				}
				else if (style_set & HidNpadStyleTag_NpadJoyRight) {
					if ((keysHeld & rightJoyconMotionMappedButtons) == rightJoyconMotionMappedButtons) break;
				}
				else if (style_set & HidNpadStyleTag_NpadJoyRight) {
					if ((keysHeld & proControllerMotionMappedButtons) == proControllerMotionMappedButtons) break;
				}
				if (isKeyComboPressed(keysHeld, keysDown, mappedButtons, UseCustomExitCombo ? 200'000'000 : 20'000'000)) {
					tsl::goBack();
					return true;
				}
				padUpdate(&pad);
				keysHeld = padGetButtons(&pad);
				keysDown = padGetButtonsDown(&pad);
				svcSleepThread(1000000);
				new_time = armTicksToNs(svcGetSystemTick());
			} while (new_time - last_time < timeout);
			last_time = new_time;
		}
		SystemData.KeysHeld_int = keysHeld;
		SystemData.KeysDown_int = keysDown;
		return false;
	}
};