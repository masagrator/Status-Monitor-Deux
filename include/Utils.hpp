#pragma once
#define ALWAYS_INLINE inline __attribute__((always_inline))
#include "System/SaltyNX.h"

#include "System/max17050.h"
#include "System/tmp451.h"
#include "System/pwm.h"
#include <numeric>
#include <sys/stat.h>
#include "smd_parser.hpp"
#include <array>

#define NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD 0x80044715
#define FieldDescriptor uint32_t
#define BASE_SNS_UOHM 5000


#ifdef __SWITCH__
	#define systemtickfrequency 19200000
#elif __OUNCE__
	#define systemtickfrequency 31250000
#else
	extern uint64_t systemtickfrequency;
#endif

//NX-FPS
struct resolutionCalls {
	uint16_t width;
	uint16_t height;
	uint16_t calls;
};

struct NxFpsSharedBlock {
	uint32_t MAGIC;
	uint8_t FPS;
	float FPSavg;
	bool pluginActive;
	uint8_t FPSlocked;
	uint8_t FPSmode;
	uint8_t ZeroSync;
	uint8_t patchApplied;
	uint8_t API;
	uint32_t FPSticks[10];
	uint8_t Buffers;
	uint8_t SetBuffers;
	uint8_t ActiveBuffers;
	uint8_t SetActiveBuffers;
	union {
		struct {
			bool handheld: 1;
			bool docked: 1;
			bool reserved: 6;
		} ds;
		uint8_t general;
	} displaySync;
	resolutionCalls renderCalls[8];
	resolutionCalls viewportCalls[8];
	bool forceOriginalRefreshRate;
	bool dontForce60InDocked;
	bool forceSuspend;
	uint8_t currentRefreshRate;
	float readSpeedPerSecond;
	uint8_t FPSlockedDocked;
	uint64_t frameNumber;
	int8_t expectedSetBuffers;
} NX_PACKED;

static_assert(sizeof(NxFpsSharedBlock) == 174);

typedef struct {
    u8 ssid_len;                                         ///< NifmSfWirelessSettingData::ssid_len
    char ssid[0x21];                                     ///< NifmSfWirelessSettingData::ssid
    u8 unk_x22;                                          ///< NifmSfWirelessSettingData::unk_x21
    u8 pad;                                              ///< Padding
    u32 unk_x24;                                         ///< NifmSfWirelessSettingData::unk_x22
    u32 unk_x28;                                         ///< NifmSfWirelessSettingData::unk_x23
    u8 passphrase_len;                                   ///< Passphrase length
    u8 passphrase[0x41];                                 ///< NifmSfWirelessSettingData::passphrase
    u8 pad2[0x2];                                        ///< Padding
} NifmWirelessSettingData_new;

/// NetworkProfileData. Converted from/to \ref NifmSfNetworkProfileData.
typedef struct {
    Uuid uuid;                                           ///< NifmSfNetworkProfileData::uuid
    char network_name[0x40];                             ///< NifmSfNetworkProfileData::network_name
    u32 unk_x50;                                         ///< NifmSfNetworkProfileData::unk_x112
    u32 unk_x54;                                         ///< NifmSfNetworkProfileData::unk_x113
    u8 unk_x58;                                          ///< NifmSfNetworkProfileData::unk_x114
    u8 unk_x59;                                          ///< NifmSfNetworkProfileData::unk_x115
    u8 pad[2];                                           ///< Padding
    NifmWirelessSettingData_new wireless_setting_data;   ///< \ref NifmWirelessSettingData
    NifmIpSettingData ip_setting_data;                   ///< \ref NifmIpSettingData
} NifmNetworkProfileData_new;

// Named struct types for global data variables
struct LocalTimeType {
	time_t timestamp;
	uint64_t relative_tick;
};

struct CpuDataType {
	int64_t Hz_int;
	double Core0Load_double;
	double Core1Load_double;
	double Core2Load_double;
	double Core3Load_double;
};

struct GpuDataType {
	int64_t Hz_int;
	int64_t Load_int;
};

struct RamDataType {
	int64_t Hz_int;
	int64_t LoadAll_int;
	int64_t LoadCPU_int;
	float UsedAllMB_float;
	float TotalAllMB_float;
	float UsedApplicationMB_float;
	float TotalApplicationMB_float;
	float UsedAppletMB_float;
	float TotalAppletMB_float;
	float UsedSystemMB_float;
	float TotalSystemMB_float;
	float UsedSystemUnsafeMB_float;
	float TotalSystemUnsafeMB_float;
};

struct BoardDataType {
	int64_t ChargerCurrentLimit_int;
	int64_t ChargerVoltageLimit_int;
	int64_t ChargerConnected_int;
	float BatteryCurrentAvg_float;
	float BatteryVoltageAvg_float;
	bool IsBatteryFiltered;
	float BatteryAgePercentage_float;
	float BatteryChargePercentage_float;
	float BatteryTemperatureCelcius_float;
	float DesignedFullBatteryCapacity_float;
	float ActualFullBatteryCapacity_float;
	float PowerConsumption_float;
	int64_t BatteryTimeEstimateInMinutes_int;
	float SocTemperatureCelsius_float;
	float PcbTemperatureCelsius_float;
	int64_t SkinTemperatureMiliCelsius_int;
	float FanRotationPercentageLevel_float;
};

struct GameDataType {
	int64_t LastFrameNumber_int;
	bool IsGameRunning;
	int64_t FPS_int;
	float FpsAvgOld_float;
	float FpsAvg_float;
	float ReadSpeedPerSecond_float;
	smd::ResolutionEntry ResolutionRenderCalls_int[8];
	smd::ResolutionEntry ResolutionViewportCalls_int[8];
};

struct SystemDataType {
	int64_t DisplayRefreshRate_int;
	bool IsDocked;
	int64_t KeysDown_int;
	int64_t KeysHeld_int;
	std::string formattedKeyCombo;
	int64_t ClockHour;
	int64_t ClockMinute;
	int64_t ClockSecond;
	int64_t CalendarYear;
	int64_t CalendarMonth;
	int64_t CalendarDay;
	int64_t OverlayRenderingFrameTimeInNs;
	int64_t OverlayMemoryLeftInB;
};

struct MiscDataType {
	bool IsWiFiPassphrase;
	std::string WiFiPassphrase_str;
	int64_t NvDecHz_int;
	int64_t NvEncHz_int;
	int64_t NvJpgHz_int;
	int64_t NetworkConnectionType_int;
};

//System
extern std::string keyCombo;
extern LEvent threadexit;
extern uint32_t threadexit2;
extern PwmChannelSession g_ICon;
extern std::string folderpath;
extern std::string filename;
extern std::string filepath;
extern int64_t batteryTimeLeftRefreshRate;
extern bool touchScreen;
extern NxFpsSharedBlock* NxFps;
extern bool SaltySD;
extern uint64_t PID;
extern uint32_t FPS;
extern SharedMemory _sharedmemory;
extern bool SharedMemoryUsed;
extern Handle remoteSharedMemory;
extern bool motionControl;
extern bool jumpImmediatelyToSingleSmd;
extern bool saveAndLoadMovableOverlayPosition;
extern std::string overrideLanguage;
extern std::map<std::string, std::map<std::string, std::string>> config;
extern LocalTimeType LocalTime;
extern std::unordered_map<std::string, std::string> locale;
extern bool teslaCombo;
extern bool ultrahandCombo;
extern int64_t keyComboTimeDelay;

//Checks
extern Result clkrstCheck;
extern Result nvCheck;
extern Result pcvCheck;
extern Result i2cCheck;
extern Result pwmCheck;
extern Result tcCheck;
extern Result Hinted;
extern Result pmdmntCheck;
extern Result psmCheck;
extern Result pwmDutyCycleCheck;

extern CpuDataType CpuData;
extern GpuDataType GpuData;
extern RamDataType RamData;
extern BoardDataType BoardData;
extern GameDataType GameData;
extern SystemDataType SystemData;
extern MiscDataType MiscData;

extern FieldDescriptor fd;

static constexpr unsigned char impl_defaultLocale[] = {
	#embed "defaultLocale.ini"
	, 0
};
extern std::array<unsigned char, sizeof(impl_defaultLocale)> defaultLocale;

inline void BindAllPredefined(smd::Document& doc) {
    doc.BindInt64 ("CPU_Hz_int",                          		&CpuData.Hz_int);
    doc.BindDouble("CPU_Core0Load_double",                		&CpuData.Core0Load_double);
    doc.BindDouble("CPU_Core1Load_double",                		&CpuData.Core1Load_double);
    doc.BindDouble("CPU_Core2Load_double",                		&CpuData.Core2Load_double);
    doc.BindDouble("CPU_Core3Load_double",                		&CpuData.Core3Load_double);
    doc.BindInt64 ("GPU_Hz_int",                          		&GpuData.Hz_int);
    doc.BindInt64 ("GPU_Load_int",                        		&GpuData.Load_int);
    doc.BindInt64 ("RAM_Hz_int",                          		&RamData.Hz_int);
    doc.BindFloat ("RAM_UsedAllMB_float",                 		&RamData.UsedAllMB_float);
    doc.BindFloat ("RAM_TotalAllMB_float",                		&RamData.TotalAllMB_float);
    doc.BindFloat ("RAM_UsedApplicationMB_float",         		&RamData.UsedApplicationMB_float);
    doc.BindFloat ("RAM_TotalApplicationMB_float",        		&RamData.TotalApplicationMB_float);
    doc.BindFloat ("RAM_UsedAppletMB_float",              		&RamData.UsedAppletMB_float);
    doc.BindFloat ("RAM_TotalAppletMB_float",             		&RamData.TotalAppletMB_float);
    doc.BindFloat ("RAM_UsedSystemMB_float",              		&RamData.UsedSystemMB_float);
    doc.BindFloat ("RAM_TotalSystemMB_float",             		&RamData.TotalSystemMB_float);
    doc.BindFloat ("RAM_UsedSystemUnsafeMB_float",        		&RamData.UsedSystemUnsafeMB_float);
    doc.BindFloat ("RAM_TotalSystemUnsafeMB_float",       		&RamData.TotalSystemUnsafeMB_float);
    doc.BindInt64 ("Board_ChargerCurrentLimit_int",       		&BoardData.ChargerCurrentLimit_int);
    doc.BindInt64 ("Board_ChargerVoltageLimit_int",       		&BoardData.ChargerVoltageLimit_int);
    doc.BindInt64 ("Board_ChargerConnected_int",          		&BoardData.ChargerConnected_int);
    doc.BindFloat ("Board_BatteryCurrentAvg_float",       		&BoardData.BatteryCurrentAvg_float);
    doc.BindFloat ("Board_BatteryVoltageAvg_float",       		&BoardData.BatteryVoltageAvg_float);
    doc.BindBool  ("Board_IsBatteryFiltered",             		&BoardData.IsBatteryFiltered);
    doc.BindFloat ("Board_BatteryAgePercentage_float",    		&BoardData.BatteryAgePercentage_float);
    doc.BindFloat ("Board_BatteryChargePercentage_float", 		&BoardData.BatteryChargePercentage_float);
    doc.BindFloat ("Board_BatteryTemperatureCelcius_float", 	&BoardData.BatteryTemperatureCelcius_float);
    doc.BindFloat ("Board_DesignedFullBatteryCapacity_float", 	&BoardData.DesignedFullBatteryCapacity_float);
    doc.BindFloat ("Board_ActualFullBatteryCapacity_float", 	&BoardData.ActualFullBatteryCapacity_float);
    doc.BindFloat ("Board_PowerConsumption_float",        		&BoardData.PowerConsumption_float);
    doc.BindInt64 ("Board_BatteryTimeEstimateInMinutes_int",	&BoardData.BatteryTimeEstimateInMinutes_int);
    doc.BindFloat ("Board_SocTemperatureCelsius_float",   		&BoardData.SocTemperatureCelsius_float);
    doc.BindFloat ("Board_PcbTemperatureCelsius_float",   		&BoardData.PcbTemperatureCelsius_float);
    doc.BindInt64 ("Board_SkinTemperatureMiliCelsius_int", 		&BoardData.SkinTemperatureMiliCelsius_int);
    doc.BindFloat ("Board_FanRotationPercentageLevel_float",	&BoardData.FanRotationPercentageLevel_float);
    doc.BindInt64 ("Game_LastFrameNumber_int",            		&GameData.LastFrameNumber_int);
    doc.BindBool  ("Game_IsGameRunning",                  		&GameData.IsGameRunning);
    doc.BindInt64 ("Game_FPS_int",                        		&GameData.FPS_int);
    doc.BindFloat ("Game_FpsAvgOld_float",                		&GameData.FpsAvgOld_float);
    doc.BindFloat ("Game_FpsAvg_float",                   		&GameData.FpsAvg_float);
    doc.BindFloat ("Game_ReadSpeedPerSecond_float",       		&GameData.ReadSpeedPerSecond_float);
    doc.BindResolutionArray("Game_ResolutionRenderCalls_int",   GameData.ResolutionRenderCalls_int);
    doc.BindResolutionArray("Game_ResolutionViewportCalls_int", GameData.ResolutionViewportCalls_int);
    doc.BindInt64 ("System_DisplayRefreshRate_int",       		&SystemData.DisplayRefreshRate_int);
    doc.BindBool  ("System_IsDocked",                     		&SystemData.IsDocked);
    doc.BindInt64 ("System_KeysDown_int",                 		&SystemData.KeysDown_int);
    doc.BindInt64 ("System_KeysHeld_int",                 		&SystemData.KeysHeld_int);
	doc.BindInt64 ("System_ClockSecond_int",                 	&SystemData.ClockSecond);
	doc.BindInt64 ("System_ClockMinute_int",                 	&SystemData.ClockMinute);
	doc.BindInt64 ("System_ClockHour_int",                  	&SystemData.ClockHour);
	doc.BindInt64 ("System_CalendarYear_int",                   &SystemData.CalendarYear);
	doc.BindInt64 ("System_CalendarMonth_int",                 	&SystemData.CalendarMonth);
	doc.BindInt64 ("System_CalendarDay_int",                  	&SystemData.CalendarDay);
	doc.BindInt64 ("System_OverlayRenderingFrameTimeInNs_int",  &SystemData.OverlayRenderingFrameTimeInNs);
	doc.BindInt64 ("System_OverlayMemoryLeftInB_int",           &SystemData.OverlayMemoryLeftInB);
    doc.BindString("formattedKeyCombo",                  		&SystemData.formattedKeyCombo);
    doc.BindBool  ("Misc_IsWiFiPassphrase",               		&MiscData.IsWiFiPassphrase);
    doc.BindInt64 ("Misc_NvDecHz_int",                    		&MiscData.NvDecHz_int);
    doc.BindInt64 ("Misc_NvEncHz_int",                    		&MiscData.NvEncHz_int);
    doc.BindInt64 ("Misc_NvJpgHz_int",                    		&MiscData.NvJpgHz_int);
    doc.BindInt64 ("Misc_NetworkConnectionType_int",      		&MiscData.NetworkConnectionType_int);
    doc.BindString("Misc_WiFiPassphrase_str",             		&MiscData.WiFiPassphrase_str);
}

//Sixaxis
extern std::string leftJoyconMotionKeyCombo;
extern std::string rightJoyconMotionKeyCombo;
extern std::string proControllerMotionKeyCombo;

enum Controller {
	Controller_ProController,
	Controller_JoyConL,
	Controller_JoyConR,
	Controller_Max
};

struct Vec3 {
	float x, y, z;
};

static inline Vec3 vec3_sub(Vec3 a, Vec3 b) {
    return (Vec3){ a.x - b.x, a.y - b.y, a.z - b.z };
}

static inline Vec3 vec3_scale(Vec3 v, float s) {
    return (Vec3){ v.x * s, v.y * s, v.z * s };
}

static inline float vec3_dot(Vec3 a, Vec3 b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

static inline Vec3 vec3_normalize(Vec3 v) {
    float len = sqrtf(vec3_dot(v, v));
    if (len < 1e-6f) return (Vec3){0, 0, 0};
    return vec3_scale(v, 1.0f / len);
}

struct GyroCursor {
    float x, y;
    float sensitivity;
};

// Strip a line-comment starting with ';', but ignore ';' inside "..." strings.
[[maybe_unused]] static std::string StripLineComment(const std::string& line) {
	bool inStr = false;
	for (size_t i = 0; i < line.size(); ++i) {
		char c = line[i];
		if (c == '"' && (i == 0 || line[i-1] != '\\')) inStr = !inStr;
		else if (c == ';' && !inStr) return line.substr(0, i);
	}
	return line;
}

ALWAYS_INLINE bool isKeyComboPressed(uint64_t keysHeld, uint64_t keysDown, uint64_t comboBitmask, uint64_t expectedPressTime) {
	static uint64_t first_time_checked = 0;
	if ((keysDown == comboBitmask) || (keysHeld == comboBitmask)) {
		if (!first_time_checked) {
			first_time_checked = armTicksToNs(svcGetSystemTick());
			return false;
		}
		uint64_t second_time_checked = armTicksToNs(svcGetSystemTick());
		if (second_time_checked - first_time_checked > expectedPressTime) return true;
	}
	else first_time_checked = 0;
	return false;
}

ALWAYS_INLINE bool isValidRGBA4Color(const std::string& hexColor) {
    for (char c : hexColor) {
        if (!isxdigit(c)) {
            return false;
        }
    }
    return true;
}

struct Designs {
    std::string name;
    bool is_directory;
};

// Non-inline function declarations
Result updateExtClk();
void CheckCore(void* idletick_ptr);
void gpuLoadThread(void*);
void BatteryChecker(void*);
void searchSharedMemoryBlock(uintptr_t base);
void CheckIfGameRunning(void*);
int compare(const void* elem1, const void* elem2);
void CpuThreadLoop(void* refreshRate);
void GpuThreadLoop(void*);
void RamThreadLoop(void*);
void BoardThreadLoop(void*);
void GameThreadLoop(void* data);
void MiscThreadLoop(void*);
void gyroCursor_init(GyroCursor& cur, float startX, float startY, float sensitivity);
void gyroCursor_update(GyroCursor& cur, HidSixAxisSensorState& state, int64_t* x, int64_t* y);
void LoadSharedMemoryAndRefreshRate();
void LoadSharedMemory();
bool CheckPort();
Result hidsysSetAppletResourceUserId();
void removeSpaces(std::string& str);
void convertToUpper(std::string& str);
void convertToLower(std::string& str);
void formatButtonCombination(std::string& line);
uint64_t MapButtons(const std::string& buttonCombo);
void createDefaultFile(std::string filepath);
bool ProcessSmdSettings(std::string filename, uint32_t crc32, uint16_t* x, uint16_t* y);
void ParseIniFile();
bool convertStrToRGBA4444(std::string hexColor, uint16_t* returnValue);
void find_smd_files(const std::string& base_path, std::vector<Designs>& filesChecked);
std::string lookupSMF(const std::string& folderPath);
std::string listToFlatList(const std::string& input);
std::string flatListToList(const std::string& input);
void convertHidnpadKeyToButtonCombination (u64 bitfield, std::string& buttonCombinationToShow, std::string& buttonCombinationToConfig);
std::string resolveHexEscapes(const std::string& s);

extern "C" void* sbrk(intptr_t increment);