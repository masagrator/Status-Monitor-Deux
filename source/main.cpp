#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include "Utils.hpp"

#include <cstdlib>

static tsl::elm::OverlayFrame* rootFrame = nullptr;

std::string file_to_load = "";
HidSixAxisSensorHandle sixaxisHandles[Controller_Max];

#include "rendering_pipeline.hpp"

class MainMenu : public tsl::Gui {
public:
    
    const std::string root_path = "sdmc:/config/status-monitor-deux/modes/";
    std::string standard_path = root_path;
	std::vector<Designs> filesChecked;
	std::string formattedKeyCombo;

    MainMenu(std::string rel_path) {
		formattedKeyCombo = keyCombo;
		formatButtonCombination(formattedKeyCombo);
        if (!rel_path.empty()) {
            standard_path = rel_path;
        }
        find_smd_files(standard_path, filesChecked);
    }

    virtual tsl::elm::Element* createUI() override {

		if (jumpImmediatelyToSingleSmd == true && filesChecked.size() == 1) {
			if (filesChecked[0].is_directory == false && standard_path.compare(root_path) == 0) {		
				std::string full_path = standard_path + filesChecked[0].name;

				smd::Document::PeekInfo info;

				if (smd::Document::Peek(full_path.c_str(), info, overrideLanguage.c_str())) {
					std::string args = "--file " + filesChecked[0].name;
					tsl::setNextOverlay(filepath, args);
					tsl::Overlay::get()->close();
					backgroundColor = 0x0000;
					rootFrame = new tsl::elm::OverlayFrame("", "");
					return rootFrame;
				}
			}
		}

		std::string version = APP_VERSION;
		version += "\n" + formattedKeyCombo;

		rootFrame = new tsl::elm::OverlayFrame(APP_TITLE, version.c_str());
        auto list = new tsl::elm::List();

        std::string rel_dir = "";
        if (standard_path.length() > root_path.length()) {
            rel_dir = standard_path.substr(root_path.length());
        }

        if (!filesChecked.empty()) {
            for (const auto& item : filesChecked) {
                if (item.is_directory) {
					std::string localPath = standard_path + item.name + "/";
					std::string localName = lookupSMF(localPath);
					std::string name = localName.length() == 0 ? item.name : localName;
                    auto folderItem = new tsl::elm::ListItem(name, "\uE133");
                    folderItem->setClickListener([this, localPath](uint64_t keys) {
                        if (keys & KEY_A) {
                            tsl::changeTo<MainMenu>(localPath);
                            return true;
                        }
                        return false;
                    });
                    list->addItem(folderItem);
                } 
                else {
                    std::string full_path = standard_path + item.name;
                    
                    smd::Document::PeekInfo info;
                    smd::Document::Peek(full_path.c_str(), info, overrideLanguage.c_str());

                    auto fileItem = new tsl::elm::ListItem(info.name.empty() ? item.name : info.name, info.name.empty() ? "\uE098" : "");
                    fileItem->setClickListener([this, item, info, full_path, rel_dir](uint64_t keys) {
						if (info.name.empty() == false) {
							if (keys & KEY_A) {
								if (info.layerWidth != 0 && info.layerHeight != 0 && info.layerWidth != 448 && info.layerHeight != 720) {
									smd::Document doc;
									if (doc.LoadFromFile(full_path.c_str()) == false) {
										tsl::changeTo<RenderingPipeline>(full_path);
										return true;
									}
									BindAllPredefined(doc);
									if (doc.Compile() == false) {
										tsl::changeTo<RenderingPipeline>(full_path);
										return true;
									}
									std::string args = "--file " + rel_dir + item.name + " --submenu";
									tsl::setNextOverlay(filepath, args);
									tsl::Overlay::get()->close();
									return true;
								}
								tsl::changeTo<RenderingPipeline>(full_path);
								return true;
							}
						}
                        return false;
                    });
                    list->addItem(fileItem);
                }
            }
            rootFrame->setContent(list);
        }
        else {
            auto Status = new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *renderer, u16 x, u16 y, u16 w, u16 h) {
                renderer->drawString("No folders or .smd files found!", false, 20, 20, 20, renderer->a(0xFFFF));
            });
            rootFrame->setContent(Status);
        }

        return rootFrame;
    }

	virtual void update() override {

	}

	virtual bool handleInput(uint64_t keysDown, uint64_t keysHeld, touchPosition touchInput, JoystickPosition leftJoyStick, JoystickPosition rightJoyStick) override {
		if (keysDown & KEY_B) {
			tsl::goBack();
			return true;
		}
		return false;
	}
};

class MonitorOverlay : public tsl::Overlay {
public:

	virtual void initServices() override {
		//Initialize services
		tsl::hlp::doWithSmSession([this]{

			apmInitialize();
			if (hosversionAtLeast(8,0,0)) clkrstCheck = clkrstInitialize();
			else pcvCheck = pcvInitialize();

			if (hosversionAtLeast(5,0,0)) tcCheck = tcInitialize();

			if (hosversionAtLeast(6,0,0) && R_SUCCEEDED(pwmInitialize())) {
				pwmCheck = pwmOpenSession2(&g_ICon, 0x3D000001);
			}

			if (R_SUCCEEDED(nvInitialize())) nvCheck = nvOpen(&fd, "/dev/nvhost-ctrl-gpu");

			psmCheck = psmInitialize();
			i2cCheck = i2cInitialize();

			SaltySD = CheckPort();

			if (SaltySD) {
				LoadSharedMemoryAndRefreshRate();
			}
			if (sysclkIpcRunning() && R_SUCCEEDED(sysclkIpcInitialize())) {
				uint32_t sysClkApiVer = 0;
				sysclkIpcGetAPIVersion(&sysClkApiVer);
				if (sysClkApiVer < 4) {
					sysclkIpcExit();
				}
				else sysclkCheck = 0;
			}
			else if (hocclkIpcRunning() && R_SUCCEEDED(hocclkIpcInitialize())) {
				uint32_t hocClkApiVer = 0;
				hocclkIpcGetAPIVersion(&hocClkApiVer);
				if (hocClkApiVer < 2) {
					hocclkIpcExit();
				}
				else hocclkCheck = 0;
			}
			if (overrideLanguage.length() == 0) {
				if (R_SUCCEEDED(setInitialize())) {
					u64 languageCode;
					setGetSystemLanguage(&languageCode);
					SetLanguage language;
					setMakeLanguage(languageCode, &language);
					setExit();
					switch(language) {
						case SetLanguage_JA:     {overrideLanguage = "JA-JP"; break;}
						case SetLanguage_FR:     {overrideLanguage = "FR-FR"; break;}
						case SetLanguage_DE:     {overrideLanguage = "DE-DE"; break;}
						case SetLanguage_IT:     {overrideLanguage = "IT-IT"; break;}
						case SetLanguage_ES:     {overrideLanguage = "ES-ES"; break;}
						case SetLanguage_ZHCN:
						case SetLanguage_ZHHANS: {overrideLanguage = "ZH-CN"; break;}
						case SetLanguage_ZHTW:
						case SetLanguage_ZHHANT: {overrideLanguage = "ZH-TW"; break;}
						case SetLanguage_KO:     {overrideLanguage = "KO-KR"; break;}
						case SetLanguage_NL:     {overrideLanguage = "NL-NL"; break;}
						case SetLanguage_PT:     {overrideLanguage = "PT-PT"; break;}
						case SetLanguage_RU:     {overrideLanguage = "RU-RU"; break;}
						case SetLanguage_ENGB:   {overrideLanguage = "EN-GB"; break;}
						case SetLanguage_FRCA:   {overrideLanguage = "FR-CA"; break;}
						case SetLanguage_ES419:  {overrideLanguage = "ES-419"; break;}
						case SetLanguage_PTBR:   {overrideLanguage = "PT-BR"; break;}
						default:                 {overrideLanguage = "EN-US"; break;}
					}
				}
			}
		});
		Hinted = envIsSyscallHinted(0x6F);
		hidGetSixAxisSensorHandles(&sixaxisHandles[Controller_ProController], 1, HidNpadIdType_No1,      HidNpadStyleTag_NpadFullKey);
		hidGetSixAxisSensorHandles(&sixaxisHandles[Controller_JoyConL], 2, HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyDual);
		std::string tmp = lookupLocale("sdmc:/config/status-monitor-deux/locale.txt");
		if (tmp.length() > 0) defaultButtonView = tmp;
	}

	virtual void exitServices() override {
		if (R_SUCCEEDED(sysclkCheck)) {
			sysclkIpcExit();
		}
		else if (R_SUCCEEDED(hocclkCheck)) {
			hocclkIpcExit();
		}
		shmemClose(&_sharedmemory);
		//Exit services
		clkrstExit();
		pcvExit();
		tsExit();
		tcExit();
		pwmChannelSessionClose(&g_ICon);
		pwmExit();
		nvClose(fd);
		nvExit();
		psmExit();
		i2cExit();
		apmExit();
	}

    virtual void onShow() override {}
    virtual void onHide() override {}

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
		if (file_to_load.length() == 0)
        	return initially<MainMenu>("");
		else {
			return initially<RenderingPipeline>(file_to_load);
		}
    }
};

int main(int argc, char **argv) {
	#if !defined(__SWITCH__) && !defined(__OUNCE__)
		systemtickfrequency = armGetSystemTickFreq();
	#endif

	ParseIniFile();
    
	if (argc > 0) {
		filename = argv[0];
		filepath = folderpath + filename;
	}
	int arg = 1;
	while (arg < argc) {
		if (strcasecmp(argv[arg], "--file") == 0) {
			if (arg + 1 < argc) {
				const char* smd_filename = argv[arg+1];
				std::string path = "sdmc:/config/status-monitor-deux/modes/";
				path += smd_filename;
				
				struct stat filedata;
				if (stat(path.c_str(), &filedata) == 0) {
					smd::Document doc;
					if (doc.LoadFromFile(path.c_str()) == true) {
						doc.Free();
						smd::Document::PeekInfo peek;
						smd::Document::Peek(path.c_str(), peek);
						if (peek.layerWidth != 0 && peek.layerHeight != 0) {
							framebufferWidth = peek.layerWidth;
							framebufferHeight = peek.layerHeight;
						}
					}
					file_to_load = path;
				}
			}
		}
		else if (strcasecmp(argv[arg], "--submenu") == 0) {
			tsl::setNextOverlay(filepath, "");
		};
		arg++;
	}
    return tsl::loop<MonitorOverlay>(argc, argv);
}