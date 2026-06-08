#pragma once
#include "EditConfigLanguage.hpp"
#include "EditConfigKeyCombo.hpp"
#include "ConfigurationServiceCheck.hpp"

class ConfigurationMainMenu : public tsl::Gui {
private:
	std::map<std::string, std::string> m_configs;
	std::string buttonBackup;
	bool skipConfigSaving = false;

	void setDataToIniFile(const std::string& fileToEdit, const std::string& desiredSection) {
		FILE* configFile = fopen(fileToEdit.c_str(), "r");
		if (!configFile) {
			configFile = fopen(fileToEdit.c_str(), "w");
			if (!configFile) return;
			fprintf(configFile, "[%s]\n", desiredSection.c_str());
			for (const auto& [key, data] : m_configs)
				fprintf(configFile, "%s = %s\n", key.c_str(), trim(data).c_str());
			fclose(configFile);
			return;
		}

		std::string updatedContent;
		std::string currentSection;
		std::set<std::string> keysFound;
		char line[131072];

		bool sectionFound = false;
		bool addNewLine   = false;

		// Appends any keys from configs that haven't been written yet.
		auto appendMissingKeys = [&]() {
			for (const auto& [key, data] : m_configs) {
				if (keysFound.count(key)) continue;
				if (!updatedContent.empty() && updatedContent.substr(updatedContent.length() - 2) == "\n\n") {
					updatedContent = updatedContent.substr(0, updatedContent.length() - 1);
					addNewLine = true;
				}
				if (data.length() > 0) {
					updatedContent += key + " = " + trim(data) + "\n";
					keysFound.insert(key);
					if (addNewLine) { updatedContent += "\n"; addNewLine = false; }
				}
			}
		};

		while (fgets(line, sizeof(line), configFile)) {
			std::string trimmedLine = trim(line);

			// Entering a new section header.
			if (trimmedLine.size() >= 2 && trimmedLine[0] == '[' && trimmedLine.back() == ']') {
				// Leaving the desired section — flush any keys not yet written.
				if (sectionFound && trim(currentSection) == trim(desiredSection))
					appendMissingKeys();

				currentSection = removeQuotes(trimmedLine.substr(1, trimmedLine.length() - 2));
			}

			// Inside the desired section — check if this line holds one of our keys.
			if (trim(currentSection) == trim(desiredSection)) {
				sectionFound = true;
				std::string::size_type delimiterPos = trimmedLine.find('=');

				if (delimiterPos != std::string::npos) {
					std::string lineKey = trim(trimmedLine.substr(0, delimiterPos));

					// Strip prefix for map lookup — file stores "User_key", configs is keyed by "key"
					std::string lookupKey = lineKey;
					auto it = m_configs.find(lookupKey);

					if (it != m_configs.end()) {
						keysFound.insert(lookupKey); // track by unprefixed key, same as configs
						if (!updatedContent.empty() && updatedContent.substr(updatedContent.length() - 2) == "\n\n") {
							updatedContent = updatedContent.substr(0, updatedContent.length() - 1);
							addNewLine = true;
						}
						if (it->second.length() > 0) {
							updatedContent += lookupKey + " = " + trim(it->second) + "\n";
							if (addNewLine) { updatedContent += "\n"; addNewLine = false; }
						}
						continue;
					}
				}
			}

			updatedContent += line;
		}

		fclose(configFile);

		// EOF while still inside the desired section.
		if (sectionFound)
			appendMissingKeys();

		// Section was never encountered — append it at the end.
		if (!sectionFound) {
			updatedContent += "\n[" + desiredSection + "]\n";
			for (const auto& [key, data] : m_configs)
				if (data.length() > 0) updatedContent += key + " = " + trim(data) + "\n";
		}

		configFile = fopen(fileToEdit.c_str(), "w");
		if (!configFile) return;
		fprintf(configFile, "%s", updatedContent.c_str());
		fclose(configFile);
	}
public:
	ConfigurationMainMenu() {
		buttonBackup = defaultButtonView;
		defaultButtonView = locale["Footer"];
		auto it = config.find("status-monitor-deux");
		if (it != config.end()) m_configs = it->second;

		if (m_configs.find("battery_avg_iir_filter") == m_configs.end()) m_configs["battery_avg_iir_filter"] = "false";
		if (m_configs.find("battery_time_left_refreshrate") == m_configs.end()) m_configs["battery_time_left_refreshrate"] = "10";
		if (m_configs.find("touch_screen") == m_configs.end()) m_configs["touch_screen"] = "true";
		if (m_configs.find("motion_control") == m_configs.end()) m_configs["motion_control"] = "true";
		if (m_configs.find("left_joycon_motion_key_combo") == m_configs.end()) m_configs["left_joycon_motion_key_combo"] = "ZL+L+LSTICK";
		if (m_configs.find("right_joycon_motion_key_combo") == m_configs.end()) m_configs["right_joycon_motion_key_combo"] = "ZR+R+RSTICK";
		if (m_configs.find("pro_controller_motion_key_combo") == m_configs.end()) m_configs["pro_controller_motion_key_combo"] = "ZR+R+RSTICK";
		if (m_configs.find("jump_immediately_to_single_smd") == m_configs.end()) m_configs["jump_immediately_to_single_smd"] = "true";
		if (m_configs.find("save_and_load_movable_overlay_position") == m_configs.end()) m_configs["save_and_load_movable_overlay_position"] = "true";
		if (m_configs.find("override_language") == m_configs.end()) m_configs["override_language"] = "false";
		if (m_configs.find("override_language_ietf_code") == m_configs.end()) m_configs["override_language_ietf_code"] = "EN-US";
		if (m_configs.find("key_combo_time_delay_ms") == m_configs.end()) m_configs["key_combo_time_delay_ms"] = "200";
	}

	~ConfigurationMainMenu() {
		defaultButtonView = buttonBackup;
		if (skipConfigSaving == false) {
			setDataToIniFile("sdmc:/config/status-monitor-deux/config.ini", "status-monitor-deux");
			for (const auto& [key, value] : m_configs) {
				if (value.length() > 0) config["status-monitor-deux"][key] = value;
			}
		}
		if (keyComboTimeDelay >= 20 && keyComboTimeDelay <= 1000) keyComboTimeDelay *= 1'000'000;
	}

	virtual tsl::elm::Element* createUI() override {
		rootFrame = new tsl::elm::OverlayFrame(APP_TITLE, locale["Settings"]);
		auto list = new tsl::elm::List();

		{
			auto Item = new tsl::elm::ListItem(locale["key_combo"]);
			Item->setClickListener([this](uint64_t keys) {
				if (keys & KEY_A) {
					tsl::changeTo<EditConfigKeyCombo>(true, "key_combo", m_configs["key_combo"], locale["key_combo"], &m_configs, &keyCombo);
					return true;
				}
				return false;
			});		
			list->addItem(Item);
		}

		{
			auto Item = new tsl::elm::ListItem(locale["key_combo_time_delay"], m_configs["key_combo_time_delay_ms"]);
			Item->setClickListener([this, Item](uint64_t keys) {
				if (keys & KEY_A) {
					tsl::changeTo<EditConfigInt>("key_combo_time_delay_ms", m_configs["key_combo_time_delay_ms"], "20", "1000", "200", Item, locale["key_combo_time_delay"], "int", &keyComboTimeDelay, &m_configs, 10);
					return true;
				}
				return false;
			});
			list->addItem(Item);
		}

		{
			auto Item = new tsl::elm::ToggleListItem(locale["battery_avg_iir_filter"], m_configs["battery_avg_iir_filter"] == "false" ? false : true);
			Item->setClickListener([this, Item](uint64_t keys) {
				if (keys & KEY_A) {
					BoardData.IsBatteryFiltered = Item->getState();
					m_configs["battery_avg_iir_filter"] = Item->getState() ? "true" : "false";
					return true;
				}
				return false;
			});		
			list->addItem(Item);
		}

		{
			auto Item = new tsl::elm::ListItem(locale["battery_time_left_refreshrate"], m_configs["battery_time_left_refreshrate"]);
			Item->setClickListener([this, Item](uint64_t keys) {
				if (keys & KEY_A) {
					tsl::changeTo<EditConfigInt>("battery_time_left_refreshrate", m_configs["battery_time_left_refreshrate"], "1", "60", "10", Item, locale["battery_time_left_refreshrate"], "int", &batteryTimeLeftRefreshRate, &m_configs);
					return true;
				}
				return false;
			});
			list->addItem(Item);
		}

		{
			auto Item = new tsl::elm::ToggleListItem(locale["touch_screen"], m_configs["touch_screen"] == "false" ? false : true);
			Item->setClickListener([this, Item](uint64_t keys) {
				if (keys & KEY_A) {
					touchScreen = Item->getState();
					m_configs["touch_screen"] = Item->getState() ? "true" : "false";
					return true;
				}
				return false;
			});
			list->addItem(Item);
		}

		{
			auto Item = new tsl::elm::ToggleListItem(locale["motion_control"], m_configs["motion_control"] == "false" ? false : true);
			Item->setClickListener([this, Item](uint64_t keys) {
				if (keys & KEY_A) {
					motionControl = Item->getState();
					m_configs["motion_control"] = Item->getState() ? "true" : "false";
					return true;
				}
				return false;
			});		
			list->addItem(Item);
		}

		{
			auto Item = new tsl::elm::ListItem(locale["left_joycon_motion_key_combo"]);
			Item->setClickListener([this](uint64_t keys) {
				if (keys & KEY_A) {
					tsl::changeTo<EditConfigKeyCombo>(false, "left_joycon_motion_key_combo", m_configs["left_joycon_motion_key_combo"], locale["left_joycon_motion_key_combo"], &m_configs, &leftJoyconMotionKeyCombo);
					return true;
				}
				return false;
			});		
			list->addItem(Item);
		}

		{
			auto Item = new tsl::elm::ListItem(locale["right_joycon_motion_key_combo"]);
			Item->setClickListener([this](uint64_t keys) {
				if (keys & KEY_A) {
					tsl::changeTo<EditConfigKeyCombo>(false, "right_joycon_motion_key_combo", m_configs["right_joycon_motion_key_combo"], locale["right_joycon_motion_key_combo"], &m_configs, &rightJoyconMotionKeyCombo);
					return true;
				}
				return false;
			});		
			list->addItem(Item);
		}

		{
			auto Item = new tsl::elm::ListItem(locale["pro_controller_motion_key_combo"]);
			Item->setClickListener([this](uint64_t keys) {
				if (keys & KEY_A) {
					tsl::changeTo<EditConfigKeyCombo>(false, "pro_controller_motion_key_combo", m_configs["pro_controller_motion_key_combo"], locale["pro_controller_motion_key_combo"], &m_configs, &proControllerMotionKeyCombo);
					return true;
				}
				return false;
			});		
			list->addItem(Item);
		}

		{
			auto Item = new tsl::elm::ToggleListItem(locale["jump_immediately_to_single_smd"], m_configs["jump_immediately_to_single_smd"] == "false" ? false : true);
			Item->setClickListener([this, Item](uint64_t keys) {
				if (keys & KEY_A) {
					jumpImmediatelyToSingleSmd = Item->getState();
					m_configs["jump_immediately_to_single_smd"] = Item->getState() ? "true" : "false";
					return true;
				}
				return false;
			});		
			list->addItem(Item);
		}

		{
			auto Item = new tsl::elm::ListItem(locale["override_language"]);
			Item->setClickListener([this](uint64_t keys) {
				if (keys & KEY_A) {
					tsl::changeTo<EditConfigLanguage>(&skipConfigSaving);
					return true;
				}
				return false;
			});		
			list->addItem(Item);
		}

		{
			auto Item = new tsl::elm::ListItem(locale["reset_settings"], "\uE141");
			Item->setClickListener([this](uint64_t keys) {
				if (keys & KEY_A) {
					skipConfigSaving = true;
					removeIniSection("sdmc:/config/status-monitor-deux/config.ini", "status-monitor-deux");
					tsl::setNextOverlay(filepath, "");
					tsl::goBack();
					tsl::goBack();
					return true;
				}
				return false;
			});		
			list->addItem(Item);
		}

		{
			auto Item = new tsl::elm::ListItem(locale["services_check"]);
			Item->setClickListener([this](uint64_t keys) {
				if (keys & KEY_A) {
					tsl::changeTo<ConfigurationServiceCheck>(locale["services_check"]);
					return true;
				}
				return false;
			});		
			list->addItem(Item);
		}

		rootFrame->setContent(list);
		return rootFrame;
	}

	virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState joyStickPosLeft, HidAnalogStickState joyStickPosRight) override {
		if (keysDown & KEY_B) {
			tsl::hlp::requestForeground(true);
			tsl::goBack();
			return true;
		}
		return false;
	}
};
