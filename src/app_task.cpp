/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "board/board.h"
#include "lib/core/CHIPError.h"
#include "lib/support/CodeUtils.h"

#include <setup_payload/OnboardingCodesUtil.h>

#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor.h>

#ifdef CONFIG_FUEL_GAUGE
#include <zephyr/drivers/fuel_gauge.h>
#endif


#include <app-common/zap-generated/attributes/Accessors.h>

#include <cmath>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace {
	// Set the measurement interval.
	constexpr size_t kMeasurementsIntervalMs = 30000; // 30 seconds

	// Delay before the first reading on boot, to give the I2C sensor time to power up and settle.
	constexpr size_t kStartupDelayMs = 3000;

	// Endpoint and attribute constants for Temperature Measurement
	constexpr uint8_t kTemperatureMeasurementEndpointId = 1;
	constexpr int16_t kTemperatureMeasurementAttributeMaxValue = 0x7fff;
	constexpr int16_t kTemperatureMeasurementAttributeMinValue = 0x954d;
	constexpr int16_t kTemperatureMeasurementAttributeInvalidValue = 0x8000;

	// Endpoint and attribute constants for Relative Humidity Measurement
	constexpr uint8_t kHumidityMeasurementEndpointId = 2;
	constexpr uint16_t kHumidityMeasurementAttributeMaxValue = 10000; // 100.00%
	constexpr uint16_t kHumidityMeasurementAttributeMinValue = 0; // 0.00%
	constexpr uint16_t kHumidityMeasurementAttributeInvalidValue = 0xffff; // Invalid value

	k_timer sMeasurementsTimer;
	bool sIsMeasurementTimerStarted = false;

	const device *sht4x_dev = DEVICE_DT_GET_ONE(sensirion_sht4x);

#ifdef CONFIG_FUEL_GAUGE
	constexpr uint8_t kPowerSourceEndpointId = 0;
	const device *max17048_dev = DEVICE_DT_GET_ONE(maxim_max17048);
#endif


} //namespace

void AppTask::MatterEventHandler(const ChipDeviceEvent *event, intptr_t arg)
{
	// On initial commissioning, start the timer.
	// We use a flag to ensure this only happens once per boot.
	if (event->Type == DeviceEventType::kCommissioningComplete && !sIsMeasurementTimerStarted) {
		LOG_INF("Commissioning complete, starting measurements.");

		// Take an immediate reading by calling UpdateClustersState directly.
		// This is posted to the event queue to ensure it runs in the correct context.
		Nrf::PostTask([] { Instance().UpdateClustersState(); });

		// Start the periodic timer. K_NO_WAIT is NOT used here because we just
		// triggered the first reading manually. The timer will fire after the first interval.
		k_timer_start(&sMeasurementsTimer, K_MSEC(kMeasurementsIntervalMs), K_MSEC(kMeasurementsIntervalMs));
		sIsMeasurementTimerStarted = true;
	}
}

void AppTask::UpdateTemperatureClusterState()
{
	struct sensor_value sTemperature;
	Protocols::InteractionModel::Status status;
	int result = sensor_channel_get(sht4x_dev, SENSOR_CHAN_AMBIENT_TEMP, &sTemperature);

	if (result == 0) {
		// The MeasuredValue attribute is in 1/100ths of a degree Celsius.
		// Round the fractional (val2, in millionths) part to the nearest 1/100th degree.
		int32_t hundredths_frac = (sTemperature.val2 >= 0) ? (sTemperature.val2 + 5000) / 10000
								  : (sTemperature.val2 - 5000) / 10000;
		int16_t newValue = static_cast<int16_t>(sTemperature.val1 * 100 + hundredths_frac);

		if (newValue > kTemperatureMeasurementAttributeMaxValue || newValue < kTemperatureMeasurementAttributeMinValue) {
			newValue = kTemperatureMeasurementAttributeInvalidValue;
		}
		LOG_DBG("New temperature measurement: %d.%06d *C, attribute value: %d", sTemperature.val1,
			sTemperature.val2, newValue);

		status = Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(kTemperatureMeasurementEndpointId, newValue);
		if (status != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Updating temperature measurement failed: %x", to_underlying(status));
		}
	} else {
		LOG_ERR("Getting temperature measurement data from BME280 failed with: %d", result);
	}
}

void AppTask::UpdateHumidityClusterState()
{
	struct sensor_value sHumidity;
	Protocols::InteractionModel::Status status;
	int result = sensor_channel_get(sht4x_dev, SENSOR_CHAN_HUMIDITY, &sHumidity);

	if (result == 0) {
		// The RelativeHumidity MeasuredValue attribute is in 1/100ths of a percent.
		// Round the fractional (val2, in millionths) part to the nearest 1/100th percent.
		int32_t hundredths_frac = (sHumidity.val2 + 5000) / 10000;
		uint16_t newValue = static_cast<uint16_t>(sHumidity.val1 * 100 + hundredths_frac);

		LOG_DBG("New humidity measurement: %d.%06d %%RH, attribute value: %u", sHumidity.val1,
			sHumidity.val2, newValue);

		// Validate the reading is within the defined range for the attribute.
		if (newValue > kHumidityMeasurementAttributeMaxValue || newValue < kHumidityMeasurementAttributeMinValue) {
			newValue = kHumidityMeasurementAttributeInvalidValue;
			LOG_WRN("Humidity value out of range, setting to invalid.");
		}

		status = Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Set(kHumidityMeasurementEndpointId, newValue);
		if (status != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Updating humidity measurement failed: %x", to_underlying(status));
		}
	} else {
		LOG_ERR("Getting humidity measurement data from BME280 failed with: %d", result);
	}
}
#ifdef CONFIG_FUEL_GAUGE
void AppTask::UpdateBatteryClusterState()
{
	union fuel_gauge_prop_val val_pct;
	union fuel_gauge_prop_val val_volt;
	union fuel_gauge_prop_val val_runtime_full; // NEW: Changed variable name
	Protocols::InteractionModel::Status status;

	// 1. Get Battery Percentage
	int rc = fuel_gauge_get_prop(max17048_dev, FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE, &val_pct);

	if (rc == 0) {
		// Driver gives 0-100 (val_pct.relative_state_of_charge)
		// Matter spec (BatPercentRemaining) is 0-200 (in 0.5% units)
		uint8_t matter_pct = static_cast<uint8_t>(val_pct.relative_state_of_charge * 2);

		LOG_DBG("New battery percentage: %d%%, attribute value: %d", val_pct.relative_state_of_charge, matter_pct);

		status = Clusters::PowerSource::Attributes::BatPercentRemaining::Set(kPowerSourceEndpointId, matter_pct);
		if (status != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Updating BatPercentRemaining failed: %x", to_underlying(status));
		}
	} else {
		LOG_ERR("Getting battery percentage failed with: %d", rc);
		// Set to "unknown" (0xFF) on failure
		status = Clusters::PowerSource::Attributes::BatPercentRemaining::Set(kPowerSourceEndpointId, 0xFF);
		if (status != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Setting BatPercentRemaining to 'unknown' failed: %x", to_underlying(status));
		}
	}

	// 2. Get Battery Voltage
	rc = fuel_gauge_get_prop(max17048_dev, FUEL_GAUGE_VOLTAGE, &val_volt);
	if (rc == 0) {
		// Driver gives uV. Matter attribute (BatVoltage) is in mV.
		uint32_t matter_volt_mv = val_volt.voltage / 1000;
		LOG_DBG("New battery voltage: %u uV, attribute value: %u mV", val_volt.voltage, matter_volt_mv);

		status = Clusters::PowerSource::Attributes::BatVoltage::Set(kPowerSourceEndpointId, matter_volt_mv);
		if (status != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Updating BatVoltage failed: %x", to_underlying(status));
		}
	} else {
		LOG_ERR("Getting battery voltage failed with: %d", rc);
		// Set to "unknown" (0xFFFFFFFF) on failure
		status = Clusters::PowerSource::Attributes::BatVoltage::Set(kPowerSourceEndpointId, 0xFFFFFFFF);
		if (status != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Setting BatVoltage to 'unknown' failed: %x", to_underlying(status));
		}
	}

	// 3. Get Charging State
	// The driver returns 'true' if charging
	rc = fuel_gauge_get_prop(max17048_dev, FUEL_GAUGE_RUNTIME_TO_FULL, &val_runtime_full);
	if (rc == 0) {
		Clusters::PowerSource::BatChargeStateEnum charge_state;

		if (val_runtime_full.runtime_to_full > 0) {
			// If time to full is > 0, we are charging
			charge_state = Clusters::PowerSource::BatChargeStateEnum::kIsCharging;
		} else {
			// Otherwise, we are not charging (or are full)
			charge_state = Clusters::PowerSource::BatChargeStateEnum::kIsNotCharging;
		}
		// NOTE: This driver doesn't seem to have a "battery full" state.
		// kNotCharging is the best fit for discharging or full.

		LOG_DBG("New battery runtime to full: %d, attribute value: %d", val_runtime_full.runtime_to_full, (uint8_t)charge_state);
		status = Clusters::PowerSource::Attributes::BatChargeState::Set(kPowerSourceEndpointId, charge_state);
		if (status != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Updating BatChargeState failed: %x", to_underlying(status));
		}

	} else {
		LOG_ERR("Getting battery runtime to full failed with: %d", rc);
		// Set to "unknown" (0x03) on failure
		status = Clusters::PowerSource::Attributes::BatChargeState::Set(kPowerSourceEndpointId,
										Clusters::PowerSource::BatChargeStateEnum::kUnknown);
		if (status != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Setting BatChargeState to 'unknown' failed: %x", to_underlying(status));
		}
	}
}
#endif

void AppTask::MeasurementsTimerHandler()
{
	Instance().UpdateClustersState();
}

void AppTask::UpdateClustersState()
{
	// Fetch a new sample from the sensor. This updates all channels.
	const int result_bme = sensor_sample_fetch(sht4x_dev);

	if (result_bme == 0) {
		// Update both clusters with the new data.
		UpdateTemperatureClusterState();
		UpdateHumidityClusterState();
	} else {
		LOG_ERR("Fetching data from SHT4X sensor failed with: %d", result_bme);
	}
#ifdef CONFIG_FUEL_GAUGE
	UpdateBatteryClusterState();
#endif
}

CHIP_ERROR AppTask::Init()
{
	/* Initialize Matter stack */
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer());

	if (!Nrf::GetBoard().Init()) {
		LOG_ERR("User interface initialization failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	/* Register Matter event handler that controls the connectivity status LED based on the captured Matter network
	 * state. */
	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(Nrf::Board::DefaultMatterEventHandler, 0));

	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(MatterEventHandler, 0));

	if (!device_is_ready(sht4x_dev)) {
		LOG_ERR("SHT4X sensor device not ready");
		return chip::System::MapErrorZephyr(-ENODEV);
	}

	k_timer_init(&sMeasurementsTimer, [](k_timer *) { Nrf::PostTask([] { MeasurementsTimerHandler(); }); }, nullptr);

	ReturnErrorOnFailure(Nrf::Matter::StartServer());

	// On reboot, if the device is already commissioned, start the timer directly.
	if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0 && !sIsMeasurementTimerStarted) {
		LOG_INF("Device is already commissioned on boot, starting measurements.");
		k_timer_start(&sMeasurementsTimer, K_MSEC(kStartupDelayMs), K_MSEC(kMeasurementsIntervalMs));
		sIsMeasurementTimerStarted = true;
	}

	return CHIP_NO_ERROR;
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	while (true) {
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}