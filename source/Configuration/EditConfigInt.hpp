#pragma once

class EditConfigInt : public tsl::Gui {
private:
	std::string m_key;
	std::string min_str;
	std::string max_str;
	std::string defaultvalue_str;
	int64_t min;
	int64_t max;
	int64_t current_value;
	int64_t default_value;
	std::string buttons = "\uE0B1/\uE0C1      \uE0C1/\uE0B2";
	tsl::elm::ListItem* m_item;
	std::string m_localName;
	bool isFont = false;
	int64_t* m_out;
	std::map<std::string, std::string>* m_config;
	std::string footerBackup;
	std::pair<u32, u32> buttonsSize{};
	std::pair<u32, u32> separatorSize{};
	std::pair<u32, u32> arrowsSize{};
	std::pair<u32, u32> minSize{};
	std::pair<u32, u32> resetSize{};
	int64_t m_offset_l = 0;
	int64_t m_offset_r = 0;
	u32 left_offset = 0;
	u32 right_offset = 0;
	std::string reset_str =  "\uE0E2 ";
	int64_t m_step;
public:
	EditConfigInt(std::string key, std::string value, std::string rangeMin, std::string rangeMax, std::string defaultValue, tsl::elm::ListItem* item, std::string localName, std::string type, int64_t* out = nullptr, std::map<std::string, std::string>* config = nullptr, int64_t step = 1) {
		m_out = out;
		m_config = config;
		footerBackup = defaultButtonView;
		defaultButtonView = locale["FooterWithReset"];
		if (localName.length() > 0) m_localName = localName;
		else m_localName = key;
		tsl::hlp::requestForeground(true);
		m_item = item;
		m_key = key;
		isNumeric(value, &current_value);
		isNumeric(rangeMin, &min);
		isNumeric(rangeMax, &max);
		if (defaultValue.length() > 0) {
			isNumeric(defaultValue, &default_value);
			defaultvalue_str = std::to_string(default_value);
		}
		min_str = std::to_string(min);
		max_str = std::to_string(max);
		if (type.compare("font") == 0) isFont = true;
		reset_str += defaultvalue_str;
		m_step = step;
	}

	~EditConfigInt() {
		defaultButtonView = footerBackup;
	}

	virtual tsl::elm::Element* createUI() override {
		rootFrame = new tsl::elm::OverlayFrame(APP_TITLE, m_localName);
		auto Status = new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *renderer, u16 x, u16 y, u16 w, u16 h) {
			const size_t fontsize2 = 60;
			const size_t m_height = (360+fontsize2) - (fontsize2 / 2);
			std::string current = std::to_string(current_value);
			auto [width2, height2] = renderer->drawString(current.c_str(), false, 0, fontsize2, fontsize2, renderer->a(0x0000));
			renderer->drawString(current.c_str(), false, (tsl::cfg::FramebufferWidth - width2) / 2, m_height, fontsize2, renderer->a(0xFFFF));

			const size_t fontsize = 30;
			if (buttonsSize.first == 0) {
				buttonsSize = renderer->drawString(buttons.c_str(), false, 0, fontsize, fontsize, renderer->a(0x0000));
				separatorSize = renderer->drawString("\uE023", false, 0, fontsize, fontsize, renderer->a(0x0000));
				arrowsSize = renderer->drawString("\uE091      \uE090", false, 0, fontsize, fontsize, renderer->a(0x0000));
				left_offset = (tsl::cfg::FramebufferWidth - buttonsSize.first) / 2;
				right_offset = tsl::cfg::FramebufferWidth - left_offset;
				minSize = renderer->drawString(min_str.c_str(), false, 0, fontsize, fontsize, renderer->a(0x0000));
				auto [maxSizeWidth, maxSizeHeight] = renderer->drawString(max_str.c_str(), false, 0, fontsize, fontsize, renderer->a(0x0000));
				auto [rSizeWidth, rSizeHeight] = renderer->drawString("\uE0A5", false, 0, fontsize, fontsize, renderer->a(0x0000));
				auto [lSizeWidth, lSizeHeight] = renderer->drawString("\uE0A4", false, 0, fontsize, fontsize, renderer->a(0x0000));
				m_offset_l = ((s32)minSize.first - (s32)lSizeWidth) / 2;
				if (m_offset_l < 0) m_offset_l = 0;
				m_offset_r = ((s32)maxSizeWidth - (s32)rSizeWidth) / 2;
				if (m_offset_r < 0) m_offset_r = 0;
				resetSize = renderer->drawString(reset_str.c_str(), false, 0, fontsize, fontsize, renderer->a(0x0000));
			}
			renderer->drawString(buttons.c_str(), false, (tsl::cfg::FramebufferWidth - buttonsSize.first) / 2, m_height+60, fontsize, renderer->a(0xFFFF));

			renderer->drawString("\uE023", false, (tsl::cfg::FramebufferWidth - separatorSize.first) / 2 - 2, m_height+60, fontsize, renderer->a(0xFFFF));

			renderer->drawString("\uE091      \uE090", false, (tsl::cfg::FramebufferWidth - arrowsSize.first) / 2, m_height+36, fontsize, renderer->a(0xFFFF));
			
			renderer->drawString(min_str.c_str(), false, left_offset - 20 - minSize.first, m_height+60, fontsize, renderer->a(0xFFFF));
			renderer->drawString("\uE0A4", false, left_offset - 20 - minSize.first + m_offset_l, m_height+60+fontsize, fontsize, renderer->a(0xFFFF));
			renderer->drawString(max_str.c_str(), false, right_offset + 20, m_height+60, fontsize, renderer->a(0xFFFF));
			renderer->drawString("\uE0A5", false, right_offset + 20 + m_offset_r, m_height+60+fontsize, fontsize, renderer->a(0xFFFF));

			renderer->drawString(reset_str.c_str(), false, (tsl::cfg::FramebufferWidth - resetSize.first) / 2, m_height+120, fontsize, renderer->a(0xFFFF));
			if (isFont) {
				auto [width8, height8] = renderer->drawString("67", false, 0, current_value, current_value, renderer->a(0x0000));
				renderer->drawString("67", false, (tsl::cfg::FramebufferWidth - width8) / 2, y + current_value, current_value, renderer->a(0xFFFF));
			}
        });
		rootFrame->setContent(Status);
		return rootFrame;
	}

	virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState joyStickPosLeft, HidAnalogStickState joyStickPosRight) override {
		float joyX = (float)joyStickPosLeft.x / 32768.0;
		float joyY = (float)joyStickPosLeft.y / 32768.0;
		if (keysDown & KEY_R) {
			current_value = max;
			return true;
		}
		else if (keysDown & KEY_L) {
			current_value = min;
			return true;
		}
		if ((joyX < -0.25 && (joyY > -0.25 && joyY < 0.25)) || (keysDown & KEY_DLEFT) || (keysDown & HidNpadButton_StickLLeft)) {
			if (current_value > min) current_value -= m_step;
			return true;
		}
		else if ((joyX > 0.25 && (joyY > -0.25 && joyY < 0.25)) || (keysDown & KEY_DRIGHT) || (keysDown & HidNpadButton_StickLRight)) {
			if (current_value < max) current_value += m_step;
			return true;
		}
		if (keysDown & KEY_X) {
			current_value = default_value;
			return true;
		}
		if (keysDown & KEY_B) {
			tsl::hlp::requestForeground(true);
			tsl::goBack();
			return true;
		}
		if (keysDown & KEY_A) {
			std::string temp = std::to_string(current_value);
			auto it = configs.find(m_key);
			if (m_out != nullptr) *m_out = current_value;
			if (it != configs.end()) configs[m_key].value = temp;
			if (m_config != nullptr) m_config->at(m_key) = temp;
			m_item->setValue(temp, false);
			tsl::hlp::requestForeground(true);
			tsl::goBack();
			return true;
		}
		return false;
	}
};
