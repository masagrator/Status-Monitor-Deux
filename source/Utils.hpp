#pragma once
#define ALWAYS_INLINE inline __attribute__((always_inline))
#include "SaltyNX.h"

#include "max17050.h"
#include "tmp451.h"
#include "pwm.h"
#include <numeric>
#include <sys/stat.h>
#include "smd_parser.hpp"

#if defined(__cplusplus)
extern "C"
{
#endif

#include <sysclk/client/ipc.h>
#include <hocclk/client/ipc.h>

#if defined(__cplusplus)
}
#endif

#define NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD 0x80044715
#define FieldDescriptor uint32_t
#define BASE_SNS_UOHM 5000


#ifdef __SWITCH__
	#define systemtickfrequency 19200000
#elif __OUNCE__
	#define systemtickfrequency 31250000
#else
	uint64_t systemtickfrequency = 0;
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

Result getNvChannelClockRate(NvChannel *channel, u32 module_id, u32 *clock_rate) {
	struct nvhost_clk_rate_args {
	    uint32_t rate;
	    uint32_t moduleid;
	} args = {
		.rate     = 0,
		.moduleid = module_id,
	};

	u32 id = hosversionBefore(8,0,0) ? _NV_IOWR(0, 0x14, args) : _NV_IOWR(0, 0x23, args);
	Result rc = nvIoctl(channel->fd, id, &args);
	if (R_SUCCEEDED(rc) && clock_rate)
		*clock_rate = args.rate;

	return rc;
}

//System
std::string keyCombo = "L+DDOWN+RSTICK"; // default Tesla Menu combo

//System
LEvent threadexit = {0};
uint32_t threadexit2 = 0;
PwmChannelSession g_ICon;
std::string folderpath = "sdmc:/switch/.overlays/";
std::string filename = "";
std::string filepath = "";
uint8_t batteryTimeLeftRefreshRate = 60;
bool touchScreen = true;
NxFpsSharedBlock* NxFps = 0;
bool SaltySD = false;
uint64_t PID = 0;
uint32_t FPS = 0xFE;
SharedMemory _sharedmemory = {};
bool SharedMemoryUsed = false;
Handle remoteSharedMemory = 1;
bool motionControl = true;
bool jumpImmediatelyToSingleSmd = true;
bool saveAndLoadMovableOverlayPosition = true;
std::string overrideLanguage;

//Checks
Result clkrstCheck = 1;
Result nvCheck = 1;
Result pcvCheck = 1;
Result i2cCheck = 1;
Result pwmCheck = 1;
Result tcCheck = 1;
Result Hinted = 1;
Result pmdmntCheck = 1;
Result psmCheck = 1;
Result sysclkCheck = 1;
Result hocclkCheck = 1;
Result pwmDutyCycleCheck = 1;

struct {
	int64_t Hz_int;
	int64_t RealHz_int;
	int64_t DeltaHz_int;
	double Core0Load_double;
	double Core1Load_double;
	double Core2Load_double;
	double Core3Load_double;
} CpuData = {0};

struct {
	int64_t Hz_int;
	int64_t RealHz_int;
	int64_t DeltaHz_int;
	int64_t Load_int;
} GpuData = {0};

struct {
	int64_t Hz_int;
	int64_t RealHz_int;
	int64_t DeltaHz_int;
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
	int64_t HocClkRamBWAll_int;
	int64_t HocClkRamBWCpu_int;
	int64_t HocClkRamBWGpu_int;
	int64_t HocClkRamBWPeak_int;
} RamData = {0};

struct {
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
	int64_t HocClkThermalSensorCPU_int;
	int64_t HocClkThermalSensorGPU_int;
	int64_t HocClkThermalSensorMEM_int;
	int64_t HocClkThermalSensorPLLX_int;
	int64_t HocClkThermalSensorAO_int;
	int64_t HocClkThermalSensorBQ24193_int;
	int64_t HocClkVoltageSOC_int;
	int64_t HocClkVoltageEMCVDD2_int;
	int64_t HocClkVoltageCPU_int;
	int64_t HocClkVoltageGPU_int;
	int64_t HocClkVoltageEMCVDDQ_int;
	int64_t HocClkVoltageDisplay_int;
} BoardData = {0};

struct {
	int64_t LastFrameNumber_int;
	bool IsGameRunning;
	int64_t FPS_int;
	float FpsAvgOld_float;
	float FpsAvg_float;
	float ReadSpeedPerSecond_float;
	smd::ResolutionEntry ResolutionRenderCalls_int[8];
	smd::ResolutionEntry ResolutionViewportCalls_int[8];
} GameData = {0};

struct {
	int64_t DisplayRefreshRate_int;
	bool IsDocked;
	int64_t KeysDown_int;
	int64_t KeysHeld_int;
	std::string formattedKeyCombo;
} SystemData = {0};

struct {
	bool IsWiFiPassphrase;
	std::string WiFiPassphrase_str;
	int64_t NvDecHz_int;
	int64_t NvEncHz_int;
	int64_t NvJpgHz_int;
	int64_t NetworkConnectionType_int;
} MiscData = {0};

inline void BindAllPredefined(smd::Document& doc) {
    doc.BindInt64 ("CPU_Hz_int",                          		&CpuData.Hz_int);
    doc.BindInt64 ("CPU_RealHz_int",                      		&CpuData.RealHz_int);
    doc.BindInt64 ("CPU_DeltaHz_int",                     		&CpuData.DeltaHz_int);
    doc.BindDouble("CPU_Core0Load_double",                		&CpuData.Core0Load_double);
    doc.BindDouble("CPU_Core1Load_double",                		&CpuData.Core1Load_double);
    doc.BindDouble("CPU_Core2Load_double",                		&CpuData.Core2Load_double);
    doc.BindDouble("CPU_Core3Load_double",                		&CpuData.Core3Load_double);
    doc.BindInt64 ("GPU_Hz_int",                          		&GpuData.Hz_int);
    doc.BindInt64 ("GPU_RealHz_int",                      		&GpuData.RealHz_int);
    doc.BindInt64 ("GPU_DeltaHz_int",                     		&GpuData.DeltaHz_int);
    doc.BindInt64 ("GPU_Load_int",                        		&GpuData.Load_int);
    doc.BindInt64 ("RAM_Hz_int",                          		&RamData.Hz_int);
    doc.BindInt64 ("RAM_RealHz_int",                      		&RamData.RealHz_int);
    doc.BindInt64 ("RAM_DeltaHz_int",                     		&RamData.DeltaHz_int);
    doc.BindInt64 ("RAM_LoadAll_int",                     		&RamData.LoadAll_int);
    doc.BindInt64 ("RAM_LoadCPU_int",                     		&RamData.LoadCPU_int);
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
    doc.BindInt64 ("RAM_HocClkRamBWAll_int",                    &RamData.HocClkRamBWAll_int);
	doc.BindInt64 ("RAM_HocClkRamBWCpu_int",                    &RamData.HocClkRamBWCpu_int);
	doc.BindInt64 ("RAM_HocClkRamBWGpu_int",                    &RamData.HocClkRamBWGpu_int);
	doc.BindInt64 ("RAM_HocClkRamBWPeak_int",                   &RamData.HocClkRamBWPeak_int);
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
    doc.BindInt64 ("Board_HocClkThermalSensorCPU_int",	        &BoardData.HocClkThermalSensorCPU_int);
    doc.BindInt64 ("Board_HocClkThermalSensorGPU_int",	        &BoardData.HocClkThermalSensorGPU_int);
    doc.BindInt64 ("Board_HocClkThermalSensorMEM_int",	        &BoardData.HocClkThermalSensorMEM_int);
    doc.BindInt64 ("Board_HocClkThermalSensorPLLX_int",	        &BoardData.HocClkThermalSensorPLLX_int);
    doc.BindInt64 ("Board_HocClkThermalSensorAO_int",	        &BoardData.HocClkThermalSensorAO_int);
	//BQ24193: 0 - Normal, 1 - Warm, 2 - Hot, 3 - Overheat
    doc.BindInt64 ("Board_HocClkThermalSensorBQ24193_int",	    &BoardData.HocClkThermalSensorBQ24193_int);
    doc.BindInt64 ("HocClkVoltageSOC_int",	                    &BoardData.HocClkVoltageSOC_int);
    doc.BindInt64 ("HocClkVoltageCPU_int",	                    &BoardData.HocClkVoltageCPU_int);
    doc.BindInt64 ("HocClkVoltageGPU_int",	                    &BoardData.HocClkVoltageGPU_int);
    doc.BindInt64 ("HocClkVoltageEMCVDD2_int",	                &BoardData.HocClkVoltageEMCVDD2_int);
    doc.BindInt64 ("HocClkVoltageEMCVDDQ_int",	                &BoardData.HocClkVoltageEMCVDDQ_int);
	doc.BindInt64 ("HocClkVoltageDisplay_int",	                &BoardData.HocClkVoltageDisplay_int);
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
    doc.BindString("formattedKeyCombo",                  		&SystemData.formattedKeyCombo);
    doc.BindBool  ("Misc_IsWiFiPassphrase",               		&MiscData.IsWiFiPassphrase);
    doc.BindInt64 ("Misc_NvDecHz_int",                    		&MiscData.NvDecHz_int);
    doc.BindInt64 ("Misc_NvEncHz_int",                    		&MiscData.NvEncHz_int);
    doc.BindInt64 ("Misc_NvJpgHz_int",                    		&MiscData.NvJpgHz_int);
    doc.BindInt64 ("Misc_NetworkConnectionType_int",      		&MiscData.NetworkConnectionType_int);
    doc.BindString("Misc_WiFiPassphrase_str",             		&MiscData.WiFiPassphrase_str);
}

//Check each core for idled ticks in intervals, they cannot read info about other core than they are assigned
//In case of getting more than systemtickfrequency in idle, make it equal to systemtickfrequency to get 0% as output and nothing less
//This is because making each loop also takes time, which is not considered because this will take also additional time


SysClkContext sysclkCTX;
HocClkContext hocclkCTX;
Result updateExtClk() {
	static uint64_t last_tick = 0;
	Result rc = 0;
	uint64_t new_tick = svcGetSystemTick();
	if (new_tick - last_tick < systemtickfrequency) {
		return 0;
	}
	if (R_SUCCEEDED(sysclkCheck)) {
		rc = sysclkIpcGetCurrentContext(&sysclkCTX);
	}
	else if (R_SUCCEEDED(hocclkCheck))
		rc = hocclkIpcGetCurrentContext(&hocclkCTX);
	else return (Result)2137;
	if (R_SUCCEEDED(rc)) last_tick = new_tick;
	return rc;
}

void CheckCore(void* idletick_ptr) {
	uint64_t* idletick = (uint64_t*)idletick_ptr;
	uint64_t FPS = *idletick;
	*idletick = 19200000;
	uint64_t timeout = 1'000'000'000 / FPS;
	while(true) {
		uint64_t idletick_a;
		uint64_t idletick_b;
		svcGetInfo(&idletick_b, InfoType_IdleTickCount, INVALID_HANDLE, -1);
		Result rc_break = svcWaitForAddress(&threadexit2, ArbitrationType_WaitIfEqual, 0, timeout);
		svcGetInfo(&idletick_a, InfoType_IdleTickCount, INVALID_HANDLE, -1);
		if (R_SUCCEEDED(rc_break)) return;
		*idletick = idletick_a - idletick_b;
	}
}

FieldDescriptor fd = 0;

void gpuLoadThread(void*) {
	#define gpu_samples_average 8
	uint32_t gpu_load_array[gpu_samples_average] = {0};
	size_t i = 0;
	FieldDescriptor m_fd = fd;
	if (R_SUCCEEDED(nvCheck)) do {
		u32 temp;
		if (R_SUCCEEDED(nvIoctl(m_fd, NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD, &temp))) {
			gpu_load_array[i++] = temp;
			if (i >= gpu_samples_average) i = 0;
			GpuData.Load_int = std::accumulate(&gpu_load_array[0], &gpu_load_array[gpu_samples_average], 0) / gpu_samples_average;
		}
	} while(!leventWait(&threadexit, 16'666'000));
}

void BatteryChecker(void*) {
	if (R_FAILED(psmCheck) || R_FAILED(i2cCheck)){
		return;
	}
	auto BatteryTimeCache = std::make_unique<int32_t[]>(120);
	PsmBatteryChargeInfoFields _batteryChargeInfoFields = {0};
	uint16_t data = 0;
	float tempV = 0.0;
	float tempA = 0.0;
	size_t ArraySize = 10;
	if (BoardData.IsBatteryFiltered) {
		ArraySize = 1;
	}
	auto readingsAmp = std::make_unique<float[]>(ArraySize);
	auto readingsVolt = std::make_unique<float[]>(ArraySize);

	Max17050ReadReg(MAX17050_AvgCurrent, &data);
	tempA = (1.5625 / (max17050SenseResistor * max17050CGain)) * (s16)data;
	for (size_t i = 0; i < ArraySize; i++) {
		readingsAmp[i] = tempA;
	}
	Max17050ReadReg(MAX17050_AvgVCELL, &data);
	tempV = 0.625 * (data >> 3);
	for (size_t i = 0; i < ArraySize; i++) {
		readingsVolt[i] = tempV;
	}
	if (!BoardData.ActualFullBatteryCapacity_float) {
		Max17050ReadReg(MAX17050_FullCAP, &data);
		BoardData.ActualFullBatteryCapacity_float = (float)(data * (BASE_SNS_UOHM / MAX17050_BOARD_SNS_RESISTOR_UOHM) / MAX17050_BOARD_CGAIN);
	}
	if (!BoardData.DesignedFullBatteryCapacity_float) {
		Max17050ReadReg(MAX17050_DesignCap, &data);
		BoardData.DesignedFullBatteryCapacity_float = (float)(data * (BASE_SNS_UOHM / MAX17050_BOARD_SNS_RESISTOR_UOHM) / MAX17050_BOARD_CGAIN);
	}
	if (readingsAmp[0] >= 0) {
		BoardData.BatteryTimeEstimateInMinutes_int = -1;
	}
	else {
		Max17050ReadReg(MAX17050_TTE, &data);
		float batteryTimeEstimateInMinutes = (5.625 * data) / 60;
		if (batteryTimeEstimateInMinutes > (99.0*60.0)+59.0) {
			BoardData.BatteryTimeEstimateInMinutes_int = (99*60)+59;
		}
		else BoardData.BatteryTimeEstimateInMinutes_int = (int16_t)batteryTimeEstimateInMinutes;
	}

	size_t counter = 0;
	uint64_t tick_TTE = svcGetSystemTick();
	uint64_t nanoseconds = 1000;
	do {
		uint64_t startTick = svcGetSystemTick();

		if (R_SUCCEEDED(psmGetBatteryChargeInfoFields(&_batteryChargeInfoFields))) {
			BoardData.ChargerConnected_int = _batteryChargeInfoFields.usb_charger_type;
			BoardData.ChargerVoltageLimit_int = _batteryChargeInfoFields.charger_input_voltage_limit;
			BoardData.ChargerCurrentLimit_int = _batteryChargeInfoFields.charger_input_current_limit;
			BoardData.BatteryTemperatureCelcius_float = (float)_batteryChargeInfoFields.temperature_celcius / 1000.0;
			BoardData.BatteryChargePercentage_float = (float)_batteryChargeInfoFields.battery_charge_percentage / 1000.0;
			BoardData.BatteryAgePercentage_float = (float)_batteryChargeInfoFields.battery_age_percentage / 1000.0;
		}

		// Calculation is based on Hekate's max17050.c
		// Source: https://github.com/CTCaer/hekate/blob/master/bdk/power/max17050.c

		if (!BoardData.IsBatteryFiltered) {
			Max17050ReadReg(MAX17050_Current, &data);
			tempA = (1.5625 / (max17050SenseResistor * max17050CGain)) * (s16)data;
			Max17050ReadReg(MAX17050_VCELL, &data);
			tempV = 0.625 * (data >> 3);
		} else {
			Max17050ReadReg(MAX17050_AvgCurrent, &data);
			tempA = (1.5625 / (max17050SenseResistor * max17050CGain)) * (s16)data;
			Max17050ReadReg(MAX17050_AvgVCELL, &data);
			tempV = 0.625 * (data >> 3);
		}

		if (tempA && tempV) {
			readingsAmp[counter % ArraySize] = tempA;
			readingsVolt[counter % ArraySize] = tempV;
			counter++;
		}

		float batCurrent = 0.0;
		float batVoltage = 0.0;
		float batPowerAvg = 0.0;
		for (size_t x = 0; x < ArraySize; x++) {
			batCurrent += readingsAmp[x];
			batVoltage += readingsVolt[x];
			batPowerAvg += (readingsAmp[x] * readingsVolt[x]) / 1'000;
		}
		batCurrent /= ArraySize;
		batVoltage /= ArraySize;
		BoardData.BatteryCurrentAvg_float = batCurrent;
		BoardData.BatteryVoltageAvg_float = batVoltage;
		batPowerAvg /= ArraySize * 1000;
		BoardData.PowerConsumption_float = batPowerAvg;

		if (batCurrent >= 0) {
			BoardData.BatteryTimeEstimateInMinutes_int = -1;
		} 
		else {
			static float batteryTimeEstimateInMinutes = 0;
			Max17050ReadReg(MAX17050_TTE, &data);
			batteryTimeEstimateInMinutes = (5.625 * data) / 60;
			if (batteryTimeEstimateInMinutes > (99.0*60.0)+59.0) {
				batteryTimeEstimateInMinutes = (99.0*60.0)+59.0;
			}
			static int itr = 0;
			int cacheElements = (sizeof(BatteryTimeCache) / sizeof(BatteryTimeCache[0]));
			BatteryTimeCache[itr++ % cacheElements] = (int32_t)batteryTimeEstimateInMinutes;
			uint64_t new_tick_TTE = svcGetSystemTick();
			if (armTicksToNs(new_tick_TTE - tick_TTE) / 1'000'000'000 >= batteryTimeLeftRefreshRate) {
				size_t to_divide = itr < cacheElements ? itr : cacheElements;
				BoardData.BatteryTimeEstimateInMinutes_int = (int16_t)(std::accumulate(&BatteryTimeCache[0], &BatteryTimeCache[to_divide], 0) / to_divide);
				tick_TTE = new_tick_TTE;
			}
		}

		nanoseconds = armTicksToNs(svcGetSystemTick() - startTick);
		if (nanoseconds < 1'000'000'000 / 2) {
			nanoseconds = (1'000'000'000 / 2) - nanoseconds;
		} else {
			nanoseconds = 1000;
		}
	} while(!leventWait(&threadexit, nanoseconds));

	BoardData.BatteryTimeEstimateInMinutes_int = -1;
}

void searchSharedMemoryBlock(uintptr_t base) {
	ptrdiff_t search_offset = 0;
	while(search_offset < 0x1000) {
		NxFps = (NxFpsSharedBlock*)(base + search_offset);
		if (NxFps -> MAGIC == 0x465053) {
			return;
		}
		else search_offset += 4;
	}
	NxFps = 0;
	return;
}

void CheckIfGameRunning(void*) {
	do {
		if (R_FAILED(pmdmntGetApplicationProcessId(&PID))) {
			GameData.IsGameRunning = false;
		}
		else if (!GameData.IsGameRunning && SharedMemoryUsed) {
				uintptr_t base = (uintptr_t)shmemGetAddr(&_sharedmemory);
				searchSharedMemoryBlock(base);
				if (NxFps) {
					(NxFps->pluginActive) = false;
					svcSleepThread(100'000'000);
					if ((NxFps->pluginActive)) {
						GameData.IsGameRunning = true;
					}
				}
		}
	} while (!leventWait(&threadexit, 1'000'000'000));
}

int compare (const void* elem1, const void* elem2) {
	if ((((resolutionCalls*)(elem1)) -> calls) > (((resolutionCalls*)(elem2)) -> calls)) return -1;
	else return 1;
}

void CpuThreadLoop(void* refreshRate) {
	u8 m_TeslaFPS = (uintptr_t)refreshRate;
	if (m_TeslaFPS > 10) m_TeslaFPS = 10;
	if (m_TeslaFPS == 0) m_TeslaFPS = 1;
	Thread threads[4];
	auto idletick = std::make_unique<uint64_t[]>(4);
	idletick[0] = m_TeslaFPS;
	idletick[1] = m_TeslaFPS;
	idletick[2] = m_TeslaFPS;
	idletick[3] = m_TeslaFPS;
	double systemtickfrequency_impl = (double)systemtickfrequency / (double)m_TeslaFPS;
	uint64_t timeout = 1'000'000'000 / m_TeslaFPS;
	threadCreate(&threads[0], CheckCore, &idletick[0], NULL, 0x1000, 0x10, 0);
	threadStart(&threads[0]);
	threadCreate(&threads[1], CheckCore, &idletick[1], NULL, 0x1000, 0x10, 1);
	threadStart(&threads[1]);
	threadCreate(&threads[2], CheckCore, &idletick[2], NULL, 0x1000, 0x10, 2);
	threadStart(&threads[2]);
	threadCreate(&threads[3], CheckCore, &idletick[3], NULL, 0x1000, 0x10, 3);
	threadStart(&threads[3]);

	double inv_frequency = 1.0 / systemtickfrequency_impl;
	
	do {
		if (R_SUCCEEDED(clkrstCheck)) {
			ClkrstSession clkSession;
			if (R_SUCCEEDED(clkrstOpenSession(&clkSession, PcvModuleId_CpuBus, 3))) {
				u32 Hz_int;
				if (R_SUCCEEDED(clkrstGetClockRate(&clkSession, &Hz_int)))
					CpuData.Hz_int = Hz_int;
				clkrstCloseSession(&clkSession);
			}
		}
		else if (R_SUCCEEDED(pcvCheck)) {
			u32 Hz_int;
			if (R_SUCCEEDED(pcvGetClockRate(PcvModule_CpuBus, &Hz_int)))
				CpuData.Hz_int = Hz_int;
		}
		if (R_SUCCEEDED(updateExtClk())) {
			if (R_SUCCEEDED(sysclkCheck))
				CpuData.RealHz_int = sysclkCTX.realFreqs[SysClkModule_CPU];
			else CpuData.RealHz_int = hocclkCTX.stable.realFreqs[HocClkModule_CPU];
			CpuData.DeltaHz_int = CpuData.RealHz_int - CpuData.Hz_int;
		}
		CpuData.Core0Load_double = std::clamp(floor((1.0 - ((double)idletick[0] * inv_frequency)) * 10000.0), 0.0, 10000.0) * 0.01;
		CpuData.Core1Load_double = std::clamp(floor((1.0 - ((double)idletick[1] * inv_frequency)) * 10000.0), 0.0, 10000.0) * 0.01;
		CpuData.Core2Load_double = std::clamp(floor((1.0 - ((double)idletick[2] * inv_frequency)) * 10000.0), 0.0, 10000.0) * 0.01;
		CpuData.Core3Load_double = std::clamp(floor((1.0 - ((double)idletick[3] * inv_frequency)) * 10000.0), 0.0, 10000.0) * 0.01;
	} while (!leventWait(&threadexit, timeout));

	threadWaitForExit(&threads[0]);
	threadClose(&threads[0]);
	threadWaitForExit(&threads[1]);
	threadClose(&threads[1]);
	threadWaitForExit(&threads[2]);
	threadClose(&threads[2]);
	threadWaitForExit(&threads[3]);
	threadClose(&threads[3]);
}

void GpuThreadLoop(void*) {
	Thread thread;
	threadCreate(&thread, gpuLoadThread, NULL, NULL, 0x1000, 0x3F, -2);
	threadStart(&thread);

	do {
		if (R_SUCCEEDED(clkrstCheck)) {
			ClkrstSession clkSession;
			if (R_SUCCEEDED(clkrstOpenSession(&clkSession, PcvModuleId_GPU, 3))) {
				u32 Hz_int;
				if (R_SUCCEEDED(clkrstGetClockRate(&clkSession, &Hz_int)))
					GpuData.Hz_int = Hz_int;
				clkrstCloseSession(&clkSession);
			}
		}
		else if (R_SUCCEEDED(pcvCheck)) {
			u32 Hz_int;
			if (R_SUCCEEDED(pcvGetClockRate(PcvModule_GPU, &Hz_int)))
				GpuData.Hz_int = Hz_int;
		}
		if (R_SUCCEEDED(updateExtClk())) {
			if (R_SUCCEEDED(sysclkCheck))
				GpuData.RealHz_int = sysclkCTX.realFreqs[SysClkModule_GPU];
			else GpuData.RealHz_int = hocclkCTX.stable.realFreqs[HocClkModule_GPU];
			GpuData.DeltaHz_int = GpuData.RealHz_int - GpuData.Hz_int;
		}
	} while (!leventWait(&threadexit, 1'000'000'000));

	threadWaitForExit(&thread);
	threadClose(&thread);
}

void RamThreadLoop(void*) {
	do {
		if (R_SUCCEEDED(clkrstCheck)) {
			ClkrstSession clkSession;
			if (R_SUCCEEDED(clkrstOpenSession(&clkSession, PcvModuleId_EMC, 3))) {
				u32 Hz_int;
				if (R_SUCCEEDED(clkrstGetClockRate(&clkSession, &Hz_int)))
					RamData.Hz_int = Hz_int;
				clkrstCloseSession(&clkSession);
			}
		}
		else if (R_SUCCEEDED(pcvCheck)) {
			u32 Hz_int;
			if (R_SUCCEEDED(pcvGetClockRate(PcvModule_EMC, &Hz_int)))
				RamData.Hz_int = Hz_int;
		}
		if (R_SUCCEEDED(updateExtClk())) {
			if (R_SUCCEEDED(sysclkCheck)) {
				RamData.RealHz_int = sysclkCTX.realFreqs[SysClkModule_MEM];
				RamData.LoadAll_int = sysclkCTX.ramLoad[SysClkRamLoad_All];
				RamData.LoadCPU_int = sysclkCTX.ramLoad[SysClkRamLoad_Cpu];
				RamData.DeltaHz_int = RamData.RealHz_int - RamData.Hz_int;
			}
			else {
				RamData.RealHz_int = hocclkCTX.stable.realFreqs[HocClkModule_MEM];
				RamData.LoadAll_int = hocclkCTX.stable.partLoad[HocClkPartLoad_EMC];
				RamData.LoadCPU_int = hocclkCTX.stable.partLoad[HocClkPartLoad_EMCCpu];
				RamData.DeltaHz_int = RamData.RealHz_int - RamData.Hz_int;
				RamData.HocClkRamBWAll_int = hocclkCTX.stable.partLoad[HocClkPartLoad_RamBWAll];
				RamData.HocClkRamBWCpu_int = hocclkCTX.stable.partLoad[HocClkPartLoad_RamBWCpu];
				RamData.HocClkRamBWGpu_int = hocclkCTX.stable.partLoad[HocClkPartLoad_RamBWGpu];
				RamData.HocClkRamBWPeak_int = hocclkCTX.stable.partLoad[HocClkPartLoad_RamBWPeak];
			}
		}

		if (R_SUCCEEDED(Hinted)) {
			
			uint64_t RAM_Total_application_u;
			uint64_t RAM_Total_applet_u;
			uint64_t RAM_Total_system_u;
			uint64_t RAM_Total_systemunsafe_u;
			uint64_t RAM_Used_application_u;
			uint64_t RAM_Used_applet_u;
			uint64_t RAM_Used_system_u;
			uint64_t RAM_Used_systemunsafe_u;

			if (R_SUCCEEDED(svcGetSystemInfo(&RAM_Total_application_u, 0, INVALID_HANDLE, 0)))
				RamData.TotalApplicationMB_float = (float)RAM_Total_application_u / 1024.f / 1024.f;
			else continue;
			if (R_SUCCEEDED(svcGetSystemInfo(&RAM_Total_applet_u, 0, INVALID_HANDLE, 1)))
				RamData.TotalAppletMB_float = (float)RAM_Total_applet_u / 1024.f / 1024.f;
			else continue;
			if (R_SUCCEEDED(svcGetSystemInfo(&RAM_Total_system_u, 0, INVALID_HANDLE, 2)))
				RamData.TotalSystemMB_float = (float)RAM_Total_system_u / 1024.f / 1024.f;
			else continue;
			if (R_SUCCEEDED(svcGetSystemInfo(&RAM_Total_systemunsafe_u, 0, INVALID_HANDLE, 3)))
				RamData.TotalSystemUnsafeMB_float = (float)RAM_Total_systemunsafe_u / 1024.f / 1024.f;
			else continue;
			if (R_SUCCEEDED(svcGetSystemInfo(&RAM_Used_application_u, 1, INVALID_HANDLE, 0)))
				RamData.UsedApplicationMB_float = (float)RAM_Used_application_u / 1024.f / 1024.f;
			else continue;
			if (R_SUCCEEDED(svcGetSystemInfo(&RAM_Used_applet_u, 1, INVALID_HANDLE, 1)))
				RamData.UsedAppletMB_float = (float)RAM_Used_applet_u / 1024.f / 1024.f;
			else continue;
			if (R_SUCCEEDED(svcGetSystemInfo(&RAM_Used_system_u, 1, INVALID_HANDLE, 2)))
				RamData.UsedSystemMB_float = (float)RAM_Used_system_u / 1024.f / 1024.f;
			else continue;
			if (R_SUCCEEDED(svcGetSystemInfo(&RAM_Used_systemunsafe_u, 1, INVALID_HANDLE, 3)))	
				RamData.UsedSystemUnsafeMB_float = (float)RAM_Used_systemunsafe_u / 1024.f / 1024.f;
			else continue;
			
			uint64_t RAM_Total_all_u = RAM_Total_application_u + RAM_Total_applet_u + RAM_Total_system_u + RAM_Total_systemunsafe_u;
			uint64_t RAM_Used_all_u = RAM_Used_application_u + RAM_Used_applet_u + RAM_Used_system_u + RAM_Used_systemunsafe_u;
			RamData.TotalAllMB_float = (float)RAM_Total_all_u / 1024.f / 1024.f;
			RamData.UsedAllMB_float = (float)RAM_Used_all_u / 1024.f / 1024.f;
		}
	} while (!leventWait(&threadexit, 1'000'000'000));
}

void BoardThreadLoop(void*) {
	Thread thread;
	threadCreate(&thread, BatteryChecker, NULL, NULL, 0x4000, 0x3F, -2);
	threadStart(&thread);

	do {
		//Temperatures
		if (R_SUCCEEDED(i2cCheck)) {
			Tmp451GetSocTemp(&BoardData.SocTemperatureCelsius_float);
			Tmp451GetPcbTemp(&BoardData.PcbTemperatureCelsius_float);
		}
		if (R_SUCCEEDED(tcCheck)) {
			s32 tmp;
			if (R_SUCCEEDED(tcGetSkinTemperatureMilliC(&tmp)))
				BoardData.SkinTemperatureMiliCelsius_int = tmp;
		}
		if (R_SUCCEEDED(pwmCheck)) {
			double temp = 0;
			if (R_SUCCEEDED(pwmChannelSessionGetDutyCycle(&g_ICon, &temp))) {
				temp *= 10;
				temp = trunc(temp);
				temp /= 10;
				float Rotation_Duty = 100.0 - temp;
				if (Rotation_Duty <= 0) Rotation_Duty = 0.0000001;
				BoardData.FanRotationPercentageLevel_float = Rotation_Duty;
			}
		}
		if (R_SUCCEEDED(hocclkCheck) && R_SUCCEEDED(updateExtClk())) {
			BoardData.HocClkThermalSensorCPU_int = hocclkCTX.stable.temps[HocClkThermalSensor_CPU];
			BoardData.HocClkThermalSensorGPU_int = hocclkCTX.stable.temps[HocClkThermalSensor_GPU];
			BoardData.HocClkThermalSensorMEM_int = hocclkCTX.stable.temps[HocClkThermalSensor_MEM];
			BoardData.HocClkThermalSensorPLLX_int = hocclkCTX.stable.temps[HocClkThermalSensor_PLLX];
			BoardData.HocClkThermalSensorAO_int = hocclkCTX.stable.temps[HocClkThermalSensor_AO];
			BoardData.HocClkThermalSensorBQ24193_int = hocclkCTX.stable.temps[HocClkThermalSensor_BQ24193];
			BoardData.HocClkVoltageSOC_int = hocclkCTX.stable.voltages[HocClkVoltage_SOC];
			BoardData.HocClkVoltageEMCVDD2_int = hocclkCTX.stable.voltages[HocClkVoltage_EMCVDD2];
			BoardData.HocClkVoltageCPU_int = hocclkCTX.stable.voltages[HocClkVoltage_CPU];
			BoardData.HocClkVoltageGPU_int = hocclkCTX.stable.voltages[HocClkVoltage_GPU];
			BoardData.HocClkVoltageEMCVDDQ_int = hocclkCTX.stable.voltages[HocClkVoltage_EMCVDDQ];
			BoardData.HocClkVoltageDisplay_int = hocclkCTX.stable.voltages[HocClkVoltage_Display];
		}
	} while (!leventWait(&threadexit, 1'000'000'000));	

	threadWaitForExit(&thread);
	threadClose(&thread);
}

void GameThreadLoop(void* data) {
	uint8_t refreshRate = (uintptr_t)data;
	if (refreshRate == 0) refreshRate = 1;
	uint64_t timeout = 1'000'000'000 / refreshRate;
	Thread thread;
	threadCreate(&thread, CheckIfGameRunning, NULL, NULL, 0x1000, 0x38, -2);
	threadStart(&thread);

	uint8_t resolutionLookup = 0;
	do {
		if (NxFps && GameData.IsGameRunning) {
			if (!resolutionLookup) {
				NxFps->renderCalls[0].calls = 0xFFFF;
				resolutionLookup = 1;
			}
			if (resolutionLookup == 1) {
				if ((NxFps->renderCalls[0].calls) != 0xFFFF) 
					resolutionLookup = 2;
			}
			GameData.FPS_int = (NxFps -> FPS);
			const size_t element_count = sizeof(NxFps->FPSticks) / sizeof(NxFps->FPSticks[0]);
			#pragma GCC diagnostic push
			#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
			float FPSavg_old = (float)systemtickfrequency / (std::accumulate<uint32_t*, float>(&NxFps->FPSticks[0], &NxFps->FPSticks[element_count], 0) / element_count);
			#pragma GCC diagnostic pop
			GameData.FpsAvgOld_float = FPSavg_old;
			float FPS_in = (float)FPS;
			float FPSavg;
			if (FPSavg_old >= (FPS_in-0.25) && FPSavg_old <= (FPS_in+0.25)) 
				FPSavg = FPS_in;
			else FPSavg = FPSavg_old;
			GameData.FpsAvg_float = FPSavg;
			GameData.LastFrameNumber_int = NxFps->frameNumber;
			GameData.ReadSpeedPerSecond_float = NxFps->readSpeedPerSecond;
			resolutionCalls m_resolutionRenderCalls[8];
			resolutionCalls m_resolutionViewportCalls[8];
			static_assert(sizeof(m_resolutionRenderCalls) == sizeof(GameData.ResolutionRenderCalls_int));
			static_assert(sizeof(m_resolutionRenderCalls) == sizeof(NxFps->renderCalls));
			memcpy(&m_resolutionRenderCalls, &(NxFps->renderCalls), sizeof(m_resolutionRenderCalls));
			memcpy(&m_resolutionViewportCalls, &(NxFps->viewportCalls), sizeof(m_resolutionViewportCalls));
			qsort(m_resolutionRenderCalls, 8, sizeof(resolutionCalls), compare);
			qsort(m_resolutionViewportCalls, 8, sizeof(resolutionCalls), compare);
			memcpy(&GameData.ResolutionRenderCalls_int, &m_resolutionRenderCalls, sizeof(m_resolutionRenderCalls));
			memcpy(&GameData.ResolutionViewportCalls_int, &m_resolutionViewportCalls, sizeof(m_resolutionViewportCalls));		
		}
		else {
			GameData.FpsAvg_float = 254.f;
			GameData.FpsAvgOld_float = 254.f;
			GameData.FPS_int = 0;
			GameData.LastFrameNumber_int = 0;
			GameData.ReadSpeedPerSecond_float = 0.f;
			resolutionLookup = 0;
		}
	} while (!leventWait(&threadexit, timeout));	

	threadWaitForExit(&thread);
	threadClose(&thread);
}

void MiscThreadLoop(void*) {
	MmuRequest nvdecRequest;
	MmuRequest nvencRequest;
	MmuRequest nvjpgRequest;

	smInitialize();
	if (R_FAILED(nifmInitialize(NifmServiceType_Admin))) return;
	if (R_FAILED(mmuInitialize())) return;
	smExit();
	Result nvdecCheck = mmuRequestInitialize(&nvdecRequest, MmuModuleId(5), 8, false);
	Result nvencCheck = mmuRequestInitialize(&nvencRequest, MmuModuleId(6), 8, false);
	Result nvjpgCheck = mmuRequestInitialize(&nvjpgRequest, MmuModuleId(7), 8, false);

	do {
		//Multimedia clock rates
		uint32_t tmp;
		if (R_SUCCEEDED(nvdecCheck)) 
			if (R_SUCCEEDED(mmuRequestGet(&nvdecRequest, &tmp))) MiscData.NvDecHz_int = tmp;
		if (R_SUCCEEDED(nvencCheck))
			if (R_SUCCEEDED(mmuRequestGet(&nvencRequest, &tmp))) MiscData.NvEncHz_int = tmp;
		if (R_SUCCEEDED(nvjpgCheck))
			if (R_SUCCEEDED(mmuRequestGet(&nvjpgRequest, &tmp))) MiscData.NvJpgHz_int = tmp;

		u32 dummy = 0;
		NifmInternetConnectionType NifmConnectionType;
		NifmInternetConnectionStatus NifmConnectionStatus;
		NifmNetworkProfileData_new Nifm_profile;
		Result Nifm_internet_rc = nifmGetInternetConnectionStatus(&NifmConnectionType, &dummy, &NifmConnectionStatus);
		if (R_SUCCEEDED(Nifm_internet_rc)) {
			MiscData.NetworkConnectionType_int = NifmConnectionType;
			if (NifmConnectionType == NifmInternetConnectionType_WiFi) {
				if (R_SUCCEEDED(nifmGetCurrentNetworkProfile((NifmNetworkProfileData*)&Nifm_profile))) {
					size_t password_len = Nifm_profile.wireless_setting_data.passphrase_len;
					if (password_len > 0) {
						MiscData.IsWiFiPassphrase = true;
						char pass_temp1[25];
						char pass_temp2[25];
						char pass_temp3[17];
						if (password_len > 48) {
							memcpy(&pass_temp1, &(Nifm_profile.wireless_setting_data.passphrase[0]), 24);
							MiscData.WiFiPassphrase_str = pass_temp1;
							MiscData.WiFiPassphrase_str += "\n";
							memcpy(&pass_temp2, &(Nifm_profile.wireless_setting_data.passphrase[24]), 24);
							MiscData.WiFiPassphrase_str += pass_temp2;
							MiscData.WiFiPassphrase_str += "\n";
							memcpy(&pass_temp3, &(Nifm_profile.wireless_setting_data.passphrase[48]), 16);
							MiscData.WiFiPassphrase_str += pass_temp3;
						}
						else if (password_len > 24) {
							memcpy(&pass_temp1, &(Nifm_profile.wireless_setting_data.passphrase[0]), 24);
							MiscData.WiFiPassphrase_str = pass_temp1;
							MiscData.WiFiPassphrase_str += "\n";
							memcpy(&pass_temp2, &(Nifm_profile.wireless_setting_data.passphrase[24]), 24);
							MiscData.WiFiPassphrase_str += pass_temp2;
						}
						else {
							MiscData.WiFiPassphrase_str = (const char*)&Nifm_profile.wireless_setting_data.passphrase;
						}
					}
					else MiscData.IsWiFiPassphrase = false;
				}
				else MiscData.IsWiFiPassphrase = false;
			}
		}
		else {
			MiscData.NetworkConnectionType_int = 0;
			MiscData.IsWiFiPassphrase = false;
		}
	} while (!leventWait(&threadexit, 1'000'000'000));	

	nifmExit();
	mmuRequestFinalize(&nvdecRequest);
	mmuRequestFinalize(&nvencRequest);
	mmuRequestFinalize(&nvjpgRequest);
	mmuExit();
}

//Sixaxis
std::string leftJoyconMotionKeyCombo = "ZL+L+LSTICK";
std::string rightJoyconMotionKeyCombo = "ZR+R+RSTICK";
std::string proControllerMotionKeyCombo = "ZR+R+RSTICK";

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

void gyroCursor_init(GyroCursor &cur, float startX, float startY, float sensitivity) {
    cur.x           = startX;
    cur.y           = startY;
    cur.sensitivity = sensitivity;
}

void gyroCursor_update(GyroCursor &cur, HidSixAxisSensorState &state, int64_t* x, int64_t* y) {

	float buffer = 100.f;

    Vec3 gyro  = { state.angular_velocity.x,
                   state.angular_velocity.y,
                   state.angular_velocity.z };

    Vec3 accel = { state.acceleration.x,
                   state.acceleration.y,
                   state.acceleration.z };

    Vec3 gravity = vec3_normalize((Vec3){ -accel.x, -accel.y, -accel.z });
    float worldYaw = vec3_dot(gyro, gravity);

    Vec3 yawPart   = vec3_scale(gravity, worldYaw);
    Vec3 flatGyro  = vec3_sub(gyro, yawPart);
    float worldPitch = flatGyro.x;

    cur.x -= worldYaw   * cur.sensitivity;
    cur.y -= worldPitch * cur.sensitivity;

    // Clamp to screen bounds
    if (cur.x < 0.0f)        *x = 0;
    else if (cur.x > 1264.f) *x = 1280;
	else                     *x = (int64_t)cur.x;
    if (cur.y < 0.0f)        *y = 0;
    else if (cur.y > 704.f)  *y = 720;
	else                     *y = (int64_t)cur.y;

	if (cur.x < -buffer) cur.x = -buffer;
	else if (cur.x > 1264.f + buffer) cur.x = 1264.f + buffer;
	if (cur.y < -buffer) cur.y = -buffer;
	else if (cur.y > 704.f + buffer) cur.y = 704.f + buffer;	
}

void LoadSharedMemoryAndRefreshRate() {
	if (SaltySD_Connect())
		return;

	SaltySD_GetSharedMemoryHandle(&remoteSharedMemory);
	u8 refreshRate = 60;
	SaltySD_GetDisplayRefreshRate(&refreshRate);
	SystemData.DisplayRefreshRate_int = refreshRate;
	SaltySD_Term();

	shmemLoadRemote(&_sharedmemory, remoteSharedMemory, 0x1000, Perm_Rw);
	if (!shmemMap(&_sharedmemory))
		SharedMemoryUsed = true;
	else FPS = 1234;
}

void LoadSharedMemory() {
	if (SaltySD_Connect())
		return;

	SaltySD_GetSharedMemoryHandle(&remoteSharedMemory);
	SaltySD_Term();

	shmemLoadRemote(&_sharedmemory, remoteSharedMemory, 0x1000, Perm_Rw);
	if (!shmemMap(&_sharedmemory))
		SharedMemoryUsed = true;
	else FPS = 1234;
}

//Check if SaltyNX is working
bool CheckPort () {
	Handle saltysd;
	bool connected = false;
	for (int i = 0; i < 67; i++) {
		if (R_SUCCEEDED(svcConnectToNamedPort(&saltysd, "InjectServ"))) {
			connected = true;
			svcCloseHandle(saltysd);
			break;
		}
		svcSleepThread(1'000'000);
	}
	if (connected == false) return false;
	for (int i = 0; i < 67; i++) {
		if (R_SUCCEEDED(svcConnectToNamedPort(&saltysd, "InjectServ"))) {
			svcCloseHandle(saltysd);
			return true;
		}
		svcSleepThread(1'000'000);
	}
	return false;
}

Result hidsysSetAppletResourceUserId() {
	u64 aruid = appletGetAppletResourceUserId();
    return serviceDispatchIn(hidsysGetServiceSession(), 500, aruid);
}

// String formatting functions
void removeSpaces(std::string& str) {
	str.erase(std::remove(str.begin(), str.end(), ' '), str.end());
}

void convertToUpper(std::string& str) {
	std::transform(str.begin(), str.end(), str.begin(), ::toupper);
}

void convertToLower(std::string& str) {
	std::transform(str.begin(), str.end(), str.begin(), ::tolower);
}

void formatButtonCombination(std::string& line) {
	std::map<std::string, std::string> replaces{
		{"A", "\uE0E0"},
		{"B", "\uE0E1"},
		{"X", "\uE0E2"},
		{"Y", "\uE0E3"},
		{"L", "\uE0E4"},
		{"R", "\uE0E5"},
		{"ZL", "\uE0E6"},
		{"ZR", "\uE0E7"},
		{"DUP", "\uE0EB"},
		{"DDOWN", "\uE0EC"},
		{"DLEFT", "\uE0ED"},
		{"DRIGHT", "\uE0EE"},
		{"PLUS", "\uE0EF"},
		{"MINUS", "\uE0F0"},
		{"LSTICK", "\uE104"},
		{"RSTICK", "\uE105"},
		{"RS", "\uE105"},
		{"LS", "\uE104"}
	};
	// Remove all spaces from the line
	line.erase(std::remove(line.begin(), line.end(), ' '), line.end());

	// Replace '+' with ' + '
	size_t pos = 0;
	size_t max_pluses = 3;
	while ((pos = line.find('+', pos)) != std::string::npos) {
		if (!max_pluses) {
			line = line.substr(0, pos);
			return;
		}
		if (pos > 0 && pos < line.size() - 1) {
			if (std::isalnum(line[pos - 1]) && std::isalnum(line[pos + 1])) {
				line.replace(pos, 1, " + ");
				pos += 3;
			}
		}
		++pos;
		max_pluses--;
	}
	pos = 0;
	size_t old_pos = 0;
	while ((pos = line.find(" + ", pos)) != std::string::npos) {

		std::string button = line.substr(old_pos, pos - old_pos);
		if (replaces.find(button) != replaces.end()) {
			line.replace(old_pos, button.length(), replaces[button]);
			pos = 0;
			old_pos = 0;
		}
		else pos += 3;
		old_pos = pos;
	}
	std::string button = line.substr(old_pos);
	if (replaces.find(button) != replaces.end()) {
		line.replace(old_pos, button.length(), replaces[button]);
	}	
}

uint64_t MapButtons(const std::string& buttonCombo) {
	std::map<std::string, uint64_t> buttonMap = {
		{"A", HidNpadButton_A},
		{"B", HidNpadButton_B},
		{"X", HidNpadButton_X},
		{"Y", HidNpadButton_Y},
		{"L", HidNpadButton_L},
		{"R", HidNpadButton_R},
		{"ZL", HidNpadButton_ZL},
		{"ZR", HidNpadButton_ZR},
		{"PLUS", HidNpadButton_Plus},
		{"MINUS", HidNpadButton_Minus},
		{"DUP", HidNpadButton_Up},
		{"DDOWN", HidNpadButton_Down},
		{"DLEFT", HidNpadButton_Left},
		{"DRIGHT", HidNpadButton_Right},
		{"LSTICK", HidNpadButton_StickL},
		{"RSTICK", HidNpadButton_StickR},
		{"LS", HidNpadButton_StickL},
		{"RS", HidNpadButton_StickR},
		{"UP", HidNpadButton_AnyUp},
		{"DOWN", HidNpadButton_AnyDown},
		{"LEFT", HidNpadButton_AnyLeft},
		{"RIGHT", HidNpadButton_AnyRight}
	};

	uint64_t comboBitmask = 0;
	std::string comboCopy = buttonCombo;  // Make a copy of buttonCombo

	std::string delimiter = "+";
	size_t pos = 0;
	std::string button;
	size_t max_delimiters = 4;
	while ((pos = comboCopy.find(delimiter)) != std::string::npos) {
		button = comboCopy.substr(0, pos);
		if (buttonMap.find(button) != buttonMap.end()) {
			comboBitmask |= buttonMap[button];
		}
		comboCopy.erase(0, pos + delimiter.length());
		if (!--max_delimiters) {
			return comboBitmask;
		}
	}
	if (buttonMap.find(comboCopy) != buttonMap.end()) {
		comboBitmask |= buttonMap[comboCopy];
	}
	return comboBitmask;
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

void createDefaultFile(std::string filepath) {
	mkdir("sdmc:/config/", 69);
	mkdir("sdmc:/config/status-monitor-deux/", 420);
	//setIniFile(filepath, "status-monitor-deux", "key_combo", "L+DDOWN+RSTICK", "");
	setIniFile(filepath, "status-monitor-deux", "battery_avg_iir_filter", "false", "");
	setIniFile(filepath, "status-monitor-deux", "battery_time_left_refreshrate", "60", "");
	setIniFile(filepath, "status-monitor-deux", "font_cache", "true", "");
	setIniFile(filepath, "status-monitor-deux", "touch_screen", "true", "");
	setIniFile(filepath, "status-monitor-deux", "motion_control", "true", "");
	setIniFile(filepath, "status-monitor-deux", "left_joycon_motion_key_combo", "ZL+L+LSTICK", "");
	setIniFile(filepath, "status-monitor-deux", "right_joycon_motion_key_combo", "ZR+R+RSTICK", "");
	setIniFile(filepath, "status-monitor-deux", "pro_controller_motion_key_combo", "ZR+R+RSTICK", "");
}

bool ProcessSmdSettings(std::string filename, uint32_t crc32, uint16_t* x, uint16_t* y) {
	std::string configIniPath = "sdmc:/config/status-monitor-deux/config.ini";
	FILE* configFileIn = fopen(configIniPath.c_str(), "r");
	if (configFileIn) {
		fseek(configFileIn, 0, SEEK_END);
		long fileSize = ftell(configFileIn);
		rewind(configFileIn);
		// Parse the INI data
		std::string fileDataString(fileSize, '\0');
		fread(&fileDataString[0], sizeof(char), fileSize, configFileIn);
		fclose(configFileIn);
		
		tsl::hlp::ini::IniData parsedData = tsl::hlp::ini::parseIni(fileDataString);
		if (parsedData.find(filename.c_str()) != parsedData.end()) {
			if (parsedData[filename.c_str()].find("hash") != parsedData[filename.c_str()].end()) {
				auto key = parsedData[filename.c_str()]["hash"];
				uint32_t crc32_to_compare;
				auto [ptr, ec] = std::from_chars(key.data(), key.data()+key.size(), crc32_to_compare, 16);
				if ((ec != std::errc{}) || (crc32 != crc32_to_compare)) return false;
			}
			else return false;
			if (parsedData[filename.c_str()].find("x") != parsedData[filename.c_str()].end()) {
				auto key = parsedData[filename.c_str()]["x"];
				auto [ptr, ec] = std::from_chars(key.data(), key.data()+key.size(), *x);
				if (ec != std::errc{}) return false;
			}
			else return false;
			if (parsedData[filename.c_str()].find("y") != parsedData[filename.c_str()].end()) {
				auto key = parsedData[filename.c_str()]["y"];
				auto [ptr, ec] = std::from_chars(key.data(), key.data()+key.size(), *y);
				if (ec != std::errc{}) return false;
				return true;
			}
			else return false;
		}
	}
	createDefaultFile(configIniPath);
	return false;
}

// Custom utility function for parsing an ini file
void ParseIniFile() {
	std::string overlayName;
	std::string directoryPath = "sdmc:/config/status-monitor-deux/";
	std::string ultrahandDirectoryPath = "sdmc:/config/ultrahand/";
	std::string teslaDirectoryPath = "sdmc:/config/tesla/";
	std::string configIniPath = directoryPath + "config.ini";
	std::string ultrahandConfigIniPath = ultrahandDirectoryPath + "config.ini";
	std::string teslaConfigIniPath = teslaDirectoryPath + "config.ini";
	tsl::hlp::ini::IniData parsedData;
	
	struct stat st;
	if (stat(directoryPath.c_str(), &st) != 0) {
		mkdir(directoryPath.c_str(), 0777);
	}

	
	bool readExternalCombo = false;
	// Open the INI file
	FILE* configFileIn = fopen(configIniPath.c_str(), "r");
	if (configFileIn) {
		// Determine the size of the INI file
		fseek(configFileIn, 0, SEEK_END);
		long fileSize = ftell(configFileIn);
		rewind(configFileIn);
			
		// Parse the INI data
		std::string fileDataString(fileSize, '\0');
		fread(&fileDataString[0], sizeof(char), fileSize, configFileIn);
		fclose(configFileIn);

		parsedData = tsl::hlp::ini::parseIni(fileDataString);
		
		// Access and use the parsed data as needed
		// For example, print the value of a specific section and key
		if (parsedData.find("status-monitor-deux") != parsedData.end()) {
			if (parsedData["status-monitor-deux"].find("key_combo") != parsedData["status-monitor-deux"].end()) {
				keyCombo = parsedData["status-monitor-deux"]["key_combo"]; // load keyCombo variable
				removeSpaces(keyCombo); // format combo
				convertToUpper(keyCombo);
			} 
			else {
				readExternalCombo = true;
			}
			if (parsedData["status-monitor-deux"].find("battery_avg_iir_filter") != parsedData["status-monitor-deux"].end()) {
				auto key = parsedData["status-monitor-deux"]["battery_avg_iir_filter"];
				convertToUpper(key);
				BoardData.IsBatteryFiltered = !key.compare("TRUE");
			}
			if (parsedData["status-monitor-deux"].find("font_cache") != parsedData["status-monitor-deux"].end()) {
				auto key = parsedData["status-monitor-deux"]["font_cache"];
				convertToUpper(key);
				fontCache = !key.compare("TRUE");
			}
			if (parsedData["status-monitor-deux"].find("battery_time_left_refreshrate") != parsedData["status-monitor-deux"].end()) {
				auto key = parsedData["status-monitor-deux"]["battery_time_left_refreshrate"];
				constexpr uint32_t maxSeconds = 60;
				constexpr uint32_t minSeconds = 1;
		
				uint32_t rate;
				auto [ptr, ec] = std::from_chars(key.data(), key.data() + key.size(), rate);

				if (ec == std::errc{}) {
					if (rate > maxSeconds) {
						rate = maxSeconds;
					}
					else if (rate < minSeconds) {
						rate = minSeconds;
					}
					batteryTimeLeftRefreshRate = rate;
				}
			}
			if (parsedData["status-monitor-deux"].find("touch_screen") != parsedData["status-monitor-deux"].end()) {
				auto key = parsedData["status-monitor-deux"]["touch_screen"];
				convertToUpper(key);
				touchScreen = key.compare("FALSE");
			}
			if (parsedData["status-monitor-deux"].find("motion_control") != parsedData["status-monitor-deux"].end()) {
				auto key = parsedData["status-monitor-deux"]["motion_control"];
				convertToUpper(key);
				motionControl = key.compare("FALSE");
			}
			if (motionControl == true) {
				if (parsedData["status-monitor-deux"].find("left_joycon_motion_key_combo") != parsedData["status-monitor-deux"].end()) {
					leftJoyconMotionKeyCombo = parsedData["status-monitor-deux"]["left_joycon_motion_key_combo"];
					removeSpaces(leftJoyconMotionKeyCombo);
					convertToUpper(leftJoyconMotionKeyCombo);
				}
				if (parsedData["status-monitor-deux"].find("right_joycon_motion_key_combo") != parsedData["status-monitor-deux"].end()) {
					rightJoyconMotionKeyCombo = parsedData["status-monitor-deux"]["right_joycon_motion_key_combo"];
					removeSpaces(rightJoyconMotionKeyCombo);
					convertToUpper(rightJoyconMotionKeyCombo);
				}
				if (parsedData["status-monitor-deux"].find("pro_controller_motion_key_combo") != parsedData["status-monitor-deux"].end()) {
					proControllerMotionKeyCombo = parsedData["status-monitor-deux"]["pro_controller_motion_key_combo"];
					removeSpaces(proControllerMotionKeyCombo);
					convertToUpper(proControllerMotionKeyCombo);
				}
			}
			if (parsedData["status-monitor-deux"].find("jump_immediately_to_single_smd") != parsedData["status-monitor-deux"].end()) {
				auto key = parsedData["status-monitor-deux"]["jump_immediately_to_single_smd"];
				convertToUpper(key);
				jumpImmediatelyToSingleSmd = key.compare("FALSE");
			}
			if (parsedData["status-monitor-deux"].find("save_and_load_movable_overlay_position") != parsedData["status-monitor-deux"].end()) {
				auto key = parsedData["status-monitor-deux"]["save_and_load_movable_overlay_position"];
				convertToUpper(key);
				saveAndLoadMovableOverlayPosition = key.compare("FALSE");
			}
			if (parsedData["status-monitor-deux"].find("override_language") != parsedData["status-monitor-deux"].end()) {
				auto key = parsedData["status-monitor-deux"]["override_language"];
				convertToUpper(key);
				bool override_check = !key.compare("TRUE");
				if (override_check) {
					if (parsedData["status-monitor-deux"].find("override_language_ietf_code") != parsedData["status-monitor-deux"].end()) {
						overrideLanguage = parsedData["status-monitor-deux"]["override_language_ietf_code"];
						convertToUpper(overrideLanguage);
						if (overrideLanguage.compare("ZH-TW") == 0) isChineseTraditionalOverride = true;
					}
				}
				
			}
		}
		else createDefaultFile(configIniPath);
		
	} else {
		createDefaultFile(configIniPath);
		readExternalCombo = true;
	}

	if (readExternalCombo) {
		FILE* ultrahandConfigFileIn = fopen(ultrahandConfigIniPath.c_str(), "r");
		FILE* teslaConfigFileIn = fopen(teslaConfigIniPath.c_str(), "r");
		if (ultrahandConfigFileIn) {
			if (teslaConfigFileIn)
				fclose(teslaConfigFileIn);
			
			std::string ultrahandFileData;
			char buffer[256];
			while (fgets(buffer, sizeof(buffer), ultrahandConfigFileIn) != NULL) {
				ultrahandFileData += buffer;
			}
			fclose(ultrahandConfigFileIn);
			
			parsedData = tsl::hlp::ini::parseIni(ultrahandFileData);
			if (parsedData.find("ultrahand") != parsedData.end() &&
				parsedData["ultrahand"].find("key_combo") != parsedData["ultrahand"].end()) {
				keyCombo = parsedData["ultrahand"]["key_combo"];
				removeSpaces(keyCombo);
				convertToUpper(keyCombo);
			}
			
		} else if (teslaConfigFileIn) {
			std::string teslaFileData;
			char buffer[256];
			while (fgets(buffer, sizeof(buffer), teslaConfigFileIn) != NULL) {
				teslaFileData += buffer;
			}
			fclose(teslaConfigFileIn);
			
			parsedData = tsl::hlp::ini::parseIni(teslaFileData);
			if (parsedData.find("tesla") != parsedData.end() &&
				parsedData["tesla"].find("key_combo") != parsedData["tesla"].end()) {
				keyCombo = parsedData["tesla"]["key_combo"];
				removeSpaces(keyCombo);
				convertToUpper(keyCombo);
			}
		}
	}
}


ALWAYS_INLINE bool isValidRGBA4Color(const std::string& hexColor) {
    for (char c : hexColor) {
        if (!isxdigit(c)) {
            return false;
        }
    }
    
    return true;
}

bool convertStrToRGBA4444(std::string hexColor, uint16_t* returnValue) {
	// Check if # is present
	if (hexColor.size() != 5 || hexColor[0] != '#')
		return false;
	
	hexColor = hexColor.substr(1);

	if (isValidRGBA4Color(hexColor)) {
		*returnValue = std::stoi(std::string(hexColor.rbegin(), hexColor.rend()), nullptr, 16);
		return true;
	}
	return false;
}

struct Designs {
    std::string name;
    bool is_directory;
};
void find_smd_files(const std::string& base_path, std::vector<Designs>& filesChecked) {
    DIR *dir = opendir(base_path.c_str());
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (entry->d_type == DT_DIR) {
            filesChecked.push_back({entry->d_name, true});
        } 
        else if (entry->d_type == DT_REG) {
            int name_len = strlen(entry->d_name);
            if (name_len > 4 && strcmp(entry->d_name + name_len - 4, ".smd") == 0) {
                filesChecked.push_back({entry->d_name, false});
            }
        }
    }
    closedir(dir);
}

static std::string resolveHexEscapes(const std::string& s) {
    std::string result;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 <= s.size() && s[i + 1] == 'x' &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 3]))) {
            result += static_cast<char>(std::stoul(s.substr(i + 2, 2), nullptr, 16));
            i += 3;
        } else {
            result += s[i];
        }
    }
    return result;
}

static std::string readLine(FILE* f) {
    std::string line;
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n')
        line += static_cast<char>(c);
    return resolveHexEscapes(line);
}

std::string lookupLocale(const std::string& path) {
	std::string searchKey = "EN-US";
	if (overrideLanguage.length() != 0) searchKey = overrideLanguage;
	FILE* f = fopen(path.c_str(), "r");
    if (!f)
        return "";

    std::string firstLine = trim(readLine(f));

    std::string line;
    while (true) {
        line = readLine(f);
        if (feof(f) && line.empty())
            break;

        size_t commaPos = line.find(',');
        if (commaPos != std::string::npos) {
            if (trim(line.substr(0, commaPos)) == searchKey) {
                fclose(f);
                return trim(line.substr(commaPos + 1));
            }
        }

        if (feof(f))
            break;
    }

    fclose(f);
    return firstLine;
}

std::string lookupSMF(const std::string& folderPath) {
    std::string path = folderPath;
    if (!path.empty() && path.back() != '/') {
        path += '/';
	}

	return lookupLocale(path + "_folder.smf");
}