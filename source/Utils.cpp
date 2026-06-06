#include <switch.h>
#include <map>
#include <math.h>
#include <memory>
#include <cstring>
#include <algorithm>
#include <charconv>
#include <dirent.h>
#include "Utils.hpp"
#include <ini_funcs.hpp>

namespace tsl::hlp::ini {
    using IniData = std::map<std::string, std::map<std::string, std::string>>;
}

#if !defined(__SWITCH__) && !defined(__OUNCE__)
uint64_t systemtickfrequency = 0;
#endif

//System
std::string keyCombo = "L+DDOWN+RSTICK"; // default Tesla Menu combo
LEvent threadexit = {0};
uint32_t threadexit2 = 0;
PwmChannelSession g_ICon;
std::string folderpath = "sdmc:/switch/.overlays/";
std::string filename = "";
std::string filepath = "";
int64_t batteryTimeLeftRefreshRate = 10;
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
std::map<std::string, std::map<std::string, std::string>> config;
LocalTimeType LocalTime;
std::unordered_map<std::string, std::string> locale;
bool teslaCombo = false;
bool ultrahandCombo = false;
int64_t keyComboTimeDelay = 200'000'000;

std::array<unsigned char, sizeof(impl_defaultLocale)> defaultLocale = std::to_array(impl_defaultLocale);

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
Result pwmDutyCycleCheck = 1;

CpuDataType CpuData = {0};
GpuDataType GpuData = {0};
RamDataType RamData = {0};
BoardDataType BoardData = {0};
GameDataType GameData = {0};
SystemDataType SystemData = {0};
MiscDataType MiscData = {0};

FieldDescriptor fd = 0;

//Sixaxis
std::string leftJoyconMotionKeyCombo = "ZL+L+LSTICK";
std::string rightJoyconMotionKeyCombo = "ZR+R+RSTICK";
std::string proControllerMotionKeyCombo = "ZR+R+RSTICK";

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

//[21.0.0+]
typedef struct {
    u32 input_current_limit;          ///< Input (Sink) current limit in mA
    u32 boost_mode_current_limit;     ///< Output (Source/VBUS/OTG) current limit in mA
    u32 fast_charge_current_limit;    ///< Battery charging current limit in mA
    u32 charge_voltage_limit;         ///< Battery charging voltage limit in mV
    PsmChargerType charger_type;
    u8 hi_z_mode;
    bool battery_charging;
    u8 pad[2];
    PsmVdd50State vdd50_state;        ///< Power Delivery Controller State
    u32 temperature_celcius;          ///< Battery temperature in milli C
    u32 battery_charge_percentage;    ///< Raw battery charged capacity per cent-mille
    u32 battery_charge_milli_voltage; ///< Voltage average in mV
    u32 battery_age_percentage;       ///< Battery age per cent-mille
	u32 unk_x2C;                      ///< the same as charger_input_voltage_limit?
    u32 usb_power_role;
    u32 usb_charger_type;
    u32 charger_input_voltage_limit;  ///< Charger and external device voltage limit in mV
    u32 charger_input_current_limit;  ///< Charger and external device current limit in mA
    bool fast_battery_charging;
    bool controller_power_supply;
    bool otg_request;
    u8 reserved;
    u8 unk_x44[0x10];
} PsmBatteryChargeInfoFieldsNew;

void BatteryChecker(void*) {
	if (R_FAILED(psmCheck) || R_FAILED(i2cCheck)){
		return;
	}
	constexpr int cacheElements = 120;
	auto BatteryTimeCache = std::make_unique<int32_t[]>(cacheElements);
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
			if (hosversionAtLeast(17, 0, 0)) {
				PsmBatteryChargeInfoFieldsNew* new_data = (PsmBatteryChargeInfoFieldsNew*)&_batteryChargeInfoFields;
				BoardData.ChargerVoltageLimit_int = new_data->charger_input_voltage_limit;
				BoardData.ChargerCurrentLimit_int = new_data->charger_input_current_limit;
				BoardData.ChargerConnected_int = new_data->usb_charger_type;			
			}
			else {
				BoardData.ChargerVoltageLimit_int = _batteryChargeInfoFields.charger_input_voltage_limit;
				BoardData.ChargerCurrentLimit_int = _batteryChargeInfoFields.charger_input_current_limit;
				BoardData.ChargerConnected_int = _batteryChargeInfoFields.usb_charger_type;
			}
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
			BatteryTimeCache[itr++ % cacheElements] = (int32_t)batteryTimeEstimateInMinutes;
			uint64_t new_tick_TTE = svcGetSystemTick();
			if (armTicksToNs(new_tick_TTE - tick_TTE) / 1'000'000'000 >= (uint64_t)batteryTimeLeftRefreshRate) {
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
			PID = 0;
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

int compare(const void* elem1, const void* elem2) {
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
							pass_temp1[24] = 0;
							MiscData.WiFiPassphrase_str = pass_temp1;
							MiscData.WiFiPassphrase_str += "\n";
							memcpy(&pass_temp2, &(Nifm_profile.wireless_setting_data.passphrase[24]), 24);
							pass_temp2[24] = 0;
							MiscData.WiFiPassphrase_str += pass_temp2;
							MiscData.WiFiPassphrase_str += "\n";
							memcpy(&pass_temp3, &(Nifm_profile.wireless_setting_data.passphrase[48]), 16);
							pass_temp3[16] = 0;
							MiscData.WiFiPassphrase_str += pass_temp3;
						}
						else if (password_len > 24) {
							memcpy(&pass_temp1, &(Nifm_profile.wireless_setting_data.passphrase[0]), 24);
							pass_temp1[24] = 0;
							MiscData.WiFiPassphrase_str = pass_temp1;
							MiscData.WiFiPassphrase_str += "\n";
							memcpy(&pass_temp2, &(Nifm_profile.wireless_setting_data.passphrase[24]), 24);
							pass_temp2[24] = 0;
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

void gyroCursor_init(GyroCursor& cur, float startX, float startY, float sensitivity) {
    cur.x           = startX;
    cur.y           = startY;
    cur.sensitivity = sensitivity;
}

void gyroCursor_update(GyroCursor& cur, HidSixAxisSensorState& state, int64_t* x, int64_t* y) {

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
bool CheckPort() {
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

void convertHidnpadKeyToButtonCombination (u64 bitfield, std::string& buttonCombinationToShow, std::string& buttonCombinationToConfig) {
	buttonCombinationToShow = "";
	buttonCombinationToConfig = "";
	std::unordered_map<HidNpadButton, std::string> replacesEmoji{
		{HidNpadButton_L, "\uE0E4"},
		{HidNpadButton_R, "\uE0E5"},
		{HidNpadButton_ZL, "\uE0E6"},
		{HidNpadButton_ZR, "\uE0E7"},
		{HidNpadButton_A, "\uE0E0"},
		{HidNpadButton_B, "\uE0E1"},
		{HidNpadButton_X, "\uE0E2"},
		{HidNpadButton_Y, "\uE0E3"},
		{HidNpadButton_Up, "\uE0EB"},
		{HidNpadButton_Down, "\uE0EC"},
		{HidNpadButton_Left, "\uE0ED"},
		{HidNpadButton_Right, "\uE0EE"},
		{HidNpadButton_Plus, "\uE0EF"},
		{HidNpadButton_Minus, "\uE0F0"},
		{HidNpadButton_StickL, "\uE104"},
		{HidNpadButton_StickR, "\uE105"}
	};

	std::unordered_map<HidNpadButton, std::string> replaces{
		{HidNpadButton_L, "L"},
		{HidNpadButton_R, "R"},
		{HidNpadButton_ZL, "ZL"},
		{HidNpadButton_ZR, "ZR"},
		{HidNpadButton_A, "A"},
		{HidNpadButton_B, "B"},
		{HidNpadButton_X, "X"},
		{HidNpadButton_Y, "Y"},
		{HidNpadButton_Up, "DUP"},
		{HidNpadButton_Down, "DDOWN"},
		{HidNpadButton_Left, "DLEFT"},
		{HidNpadButton_Right, "DRIGHT"},
		{HidNpadButton_Plus, "PLUS"},
		{HidNpadButton_Minus, "MINUS"},
		{HidNpadButton_StickL, "LSTICK"},
		{HidNpadButton_StickR, "RSTICK"}
	};

	size_t count = 0;

	for (const auto& [key, value] : replaces) {
		if (bitfield & key) {
			bool addPlus = buttonCombinationToShow.length() > 0;
			if (addPlus == true) {
				buttonCombinationToShow += " + ";
				buttonCombinationToConfig += "+";
			}
			buttonCombinationToShow += replacesEmoji[key];
			buttonCombinationToConfig += value;
			count++;
			if (count >= 4) break;
		}
	}
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
	std::string comboCopy = buttonCombo;

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

void createDefaultFile(std::string filepath) {
	mkdir("sdmc:/config/", 69);
	mkdir("sdmc:/config/status-monitor-deux/", 420);
	setIniFile(filepath, "status-monitor-deux", ";key_combo", "L+DDOWN+RSTICK", "");
	setIniFile(filepath, "status-monitor-deux", "key_combo_time_delay_ms", "200", "");
	setIniFile(filepath, "status-monitor-deux", "battery_avg_iir_filter", "false", "");
	setIniFile(filepath, "status-monitor-deux", "battery_time_left_refreshrate", "10", "");
	setIniFile(filepath, "status-monitor-deux", "touch_screen", "true", "");
	setIniFile(filepath, "status-monitor-deux", "motion_control", "true", "");
	setIniFile(filepath, "status-monitor-deux", "left_joycon_motion_key_combo", "ZL+L+LSTICK", "");
	setIniFile(filepath, "status-monitor-deux", "right_joycon_motion_key_combo", "ZR+R+RSTICK", "");
	setIniFile(filepath, "status-monitor-deux", "pro_controller_motion_key_combo", "ZR+R+RSTICK", "");
	setIniFile(filepath, "status-monitor-deux", "jump_immediately_to_single_smd", "true", "");
	setIniFile(filepath, "status-monitor-deux", "save_and_load_movable_overlay_position", "true", "");
	setIniFile(filepath, "status-monitor-deux", "override_language", "false", "");
	setIniFile(filepath, "status-monitor-deux", "override_language_ietf_code", "EN-US", "");
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
		
		std::unordered_map<std::string, std::unordered_map<std::string, std::string>> parsedData = parseIni(fileDataString);
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

std::string resolveHexEscapes(const std::string& s) {
    std::string result;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 <= s.size() && s[i + 1] == 'x' &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 3]))) {
            result += static_cast<char>(std::stoul(s.substr(i + 2, 2), nullptr, 16));
            i += 3;
        } 
		else if (s[i] == '\\' && s[i+1] == 'n') {
			result += "\n";
			i++;
		}
		else {
            result += s[i];
        }
    }
    return result;
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
	std::string localeIniPath = directoryPath + "locale.ini";
	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> parsedData;
	
	struct stat st;
	if (stat(directoryPath.c_str(), &st) != 0) {
		mkdir(directoryPath.c_str(), 0777);
	}
	
	bool readExternalCombo = true;

	// Open the INI file
	config = getParsedDataFromIniFile(configIniPath.c_str());
	auto it = config.find("status-monitor-deux");
	bool override_check = false;
	std::string temp_overrideLanguage;
	if (it != config.end()) {
		auto settings = it->second;
		for (const auto& [key, value] : settings) {
			if (key.compare("key_combo") == 0 and value.length() > 0) {
				keyCombo = value;
				removeSpaces(keyCombo);
				convertToUpper(keyCombo);
				readExternalCombo = false;
			}
			else if (key.compare("battery_avg_iir_filter") == 0 and value.length() > 0) {
				std::string temp = value;
				convertToUpper(temp);
				BoardData.IsBatteryFiltered = !temp.compare("TRUE");
			}
			else if (key.compare("battery_time_left_refreshrate") == 0 and value.length() > 0) {
				constexpr uint32_t maxSeconds = 60;
				constexpr uint32_t minSeconds = 1;
		
				uint32_t rate;
				auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), rate);

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
			else if (key.compare("touch_screen") == 0 and value.length() > 0) {
				std::string temp = value;
				convertToUpper(temp);
				touchScreen = temp.compare("FALSE");
			}
			else if (key.compare("motion_control") == 0 and value.length() > 0) {
				std::string temp = value;
				convertToUpper(temp);
				motionControl = temp.compare("FALSE");
			}
			else if (key.compare("left_joycon_motion_key_combo") == 0 and value.length() > 0) {
				leftJoyconMotionKeyCombo = value;
				removeSpaces(leftJoyconMotionKeyCombo);
				convertToUpper(leftJoyconMotionKeyCombo);
			}
			else if (key.compare("right_joycon_motion_key_combo") == 0 and value.length() > 0) {
				rightJoyconMotionKeyCombo = value;
				removeSpaces(rightJoyconMotionKeyCombo);
				convertToUpper(rightJoyconMotionKeyCombo);
			}
			else if (key.compare("pro_controller_motion_key_combo") == 0 and value.length() > 0) {
				proControllerMotionKeyCombo = value;
				removeSpaces(proControllerMotionKeyCombo);
				convertToUpper(proControllerMotionKeyCombo);
			}
			else if (key.compare("jump_immediately_to_single_smd") == 0 and value.length() > 0) {
				std::string temp = value;
				convertToUpper(temp);
				jumpImmediatelyToSingleSmd = temp.compare("FALSE");
			}
			else if (key.compare("save_and_load_movable_overlay_position") == 0 and value.length() > 0) {
				std::string temp = value;
				convertToUpper(temp);
				saveAndLoadMovableOverlayPosition = temp.compare("FALSE");
			}
			else if (key.compare("override_language") == 0 and value.length() > 0) {
				std::string temp = value;
				convertToUpper(temp);
				override_check = !temp.compare("TRUE");
			}		
			else if (key.compare("override_language_ietf_code") == 0 && value.length() > 0) {
				temp_overrideLanguage = value;
				convertToUpper(temp_overrideLanguage);
			}
			else if (key.compare("key_combo_time_delay_ms") == 0 and value.length() > 0) {
				constexpr uint64_t maxMiliseconds = 1000;
				constexpr uint64_t minMiliseconds = 20;
		
				uint64_t rate;
				auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), rate);

				if (ec == std::errc{}) {
					if (rate > maxMiliseconds) {
						rate = maxMiliseconds;
					}
					else if (rate < minMiliseconds) {
						rate = minMiliseconds;
					}
					keyComboTimeDelay = rate * 1'000'000;
				}
			}
		}
	}
	else createDefaultFile(configIniPath);

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
			
			parsedData = parseIni(ultrahandFileData);
			if (parsedData.find("ultrahand") != parsedData.end() &&
				parsedData["ultrahand"].find("key_combo") != parsedData["ultrahand"].end()) {
				keyCombo = parsedData["ultrahand"]["key_combo"];
				removeSpaces(keyCombo);
				convertToUpper(keyCombo);
				ultrahandCombo = true;
			}
			
		} else if (teslaConfigFileIn) {
			std::string teslaFileData;
			char buffer[256];
			while (fgets(buffer, sizeof(buffer), teslaConfigFileIn) != NULL) {
				teslaFileData += buffer;
			}
			fclose(teslaConfigFileIn);
			
			parsedData = parseIni(teslaFileData);
			if (parsedData.find("tesla") != parsedData.end() &&
				parsedData["tesla"].find("key_combo") != parsedData["tesla"].end()) {
				keyCombo = parsedData["tesla"]["key_combo"];
				removeSpaces(keyCombo);
				convertToUpper(keyCombo);
				teslaCombo = true;
			}
		}
	}

	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> defaultIni = parseIni(std::string((const char*)impl_defaultLocale, sizeof(impl_defaultLocale)));
	std::unordered_map<std::string, std::string> m_defaultLocale = defaultIni["EN-US"];
	std::map<std::string, std::map<std::string, std::string>> temp = getParsedDataFromIniFile(localeIniPath.c_str());

	if (override_check == true && temp_overrideLanguage.length() > 0) {
		overrideLanguage = temp_overrideLanguage;
		auto it = temp.find(overrideLanguage);
		bool isGood = false;
		if (it != temp.end()) {
			isGood = true;
			locale = std::unordered_map<std::string, std::string>(it->second.begin(), it->second.end());
			for (const auto& [key, data] : m_defaultLocale) {
				if (locale.find(key) == locale.end()) {
					isGood = false;
					break;
				}
			}
			if (isGood == true) for (const auto& [key, data] : m_defaultLocale) {
				auto it2 = locale.find(key);
				if (it2 != locale.end()) it2->second = resolveHexEscapes(locale[key]);
				else locale[key] = resolveHexEscapes(data);
			}
		}
		if (isGood == false) {
			for (const auto& [key, data] : m_defaultLocale) {
				locale[key] = resolveHexEscapes(data);
			}
		}
	}
	else {
		smInitialize();
		Result rc = setInitialize();
		smExit();
		if (R_SUCCEEDED(rc)) {
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
				case SetLanguage_FRCA:   {overrideLanguage = "FR-CA"; break;}
				case SetLanguage_ES419:  {overrideLanguage = "ES-419"; break;}
				case SetLanguage_PTBR:   {overrideLanguage = "PT-BR"; break;}
				default:                 {overrideLanguage = "EN-US"; break;}
			}

			auto it = temp.find(overrideLanguage);
			bool isGood = false;
			if (it != temp.end()) {
				locale = std::unordered_map<std::string, std::string>(it->second.begin(), it->second.end());
				isGood = true;
				for (const auto& [key, data] : m_defaultLocale) {
					if (locale.find(key) == locale.end()) {
						isGood = false;
						break;
					}
				}
				if (isGood == true) for (const auto& [key, data] : m_defaultLocale) {
					auto it2 = locale.find(key);
					if (it2 != locale.end()) it2->second = resolveHexEscapes(locale[key]);
					else locale[key] = resolveHexEscapes(data);
				}
			}
			if (isGood == false) {
				for (const auto& [key, data] : m_defaultLocale) {
					locale[key] = resolveHexEscapes(data);
				}				
			}
		}
	}
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

void find_smd_files(const std::string& base_path, std::vector<Designs>& filesChecked) {
    DIR *dir = opendir(base_path.c_str());
    if (!dir) return;

    std::vector<Designs> files;
    std::vector<Designs> dirs;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (entry->d_type == DT_DIR) {
            dirs.push_back({entry->d_name, true});
        } 
        else if (entry->d_type == DT_REG) {
            int name_len = strlen(entry->d_name);
            if (name_len > 4 && strcmp(entry->d_name + name_len - 4, ".smd") == 0) {
                files.push_back({entry->d_name, false});
            }
        }
    }
    closedir(dir);

    auto compareName = [](const Designs& a, const Designs& b) {
        return a.name < b.name;
    };

    std::sort(files.begin(), files.end(), compareName);
    std::sort(dirs.begin(), dirs.end(), compareName);

    filesChecked.insert(filesChecked.end(), files.begin(), files.end());
    filesChecked.insert(filesChecked.end(), dirs.begin(), dirs.end());
}

std::string lookupSMF(const std::string& folderPath) {
    std::string path = folderPath;
    if (!path.empty() && path.back() != '/') {
        path += '/';
	}

	path += "_folder.ini";

	FILE* file = fopen(path.c_str(), "r");
	if (file) {
		std::string searchKey = "EN-US";
		if (overrideLanguage.length() != 0) searchKey = overrideLanguage;

		std::string temp = parseValueFromIniSectionF(file, "_folder", searchKey);
		fclose(file);
		return temp;
	}

	return "";
}

std::string listToFlatList(const std::string& input) {
    size_t firstOpen = input.find('{');
    if (firstOpen == std::string::npos) return "";

    size_t innerOpen = input.find('{', firstOpen + 1);
    size_t innerClose = input.rfind('}');
    if (innerOpen == std::string::npos || innerClose == std::string::npos) return "";

    std::string body = input.substr(innerOpen + 1, innerClose - innerOpen - 1);

    std::string result;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t quoteOpen = body.find('"', pos);
        if (quoteOpen == std::string::npos) break;
        size_t quoteClose = body.find('"', quoteOpen + 1);
        if (quoteClose == std::string::npos) return "";

        if (!result.empty()) result += '+';
        result += body.substr(quoteOpen + 1, quoteClose - quoteOpen - 1);
        pos = quoteClose + 1;
    }
    return result;
}

std::string flatListToList(const std::string& input) {
    if (input.empty()) return "";

    std::string result = "LIST{str, {";
    size_t pos = 0;
    bool first = true;
    while (pos <= input.size()) {
        size_t next = input.find('+', pos);
        if (next == std::string::npos) next = input.size();

        std::string token = input.substr(pos, next - pos);
        if (!token.empty()) {
            if (!first) result += ", ";
            result += '"' + token + '"';
            first = false;
        }
        pos = next + 1;
    }

    if (first) return ""; // no tokens found
    result += "}}";
    return result;
}