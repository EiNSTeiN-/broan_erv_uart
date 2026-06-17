#include "broan.h"

namespace esphome {
namespace broan {

void BroanComponent::setFanMode( std::string mode )
{
	uint8_t value = 0x01;

	if( mode == "min")
		value = BroanFanMode::Min;
	else if (mode == "max" )
		value = BroanFanMode::Max;
	else if( mode == "medium" || mode == "manual" )
		value = BroanFanMode::Medium;
	else if( mode == "int" )
		value = BroanFanMode::Intermittent;
	else if( mode == "turbo" )
		value = BroanFanMode::Turbo;
	else if( mode == "humidity" )
		value = BroanFanMode::Humidity;
	else if( mode == "ovr" )
	{
		ESP_LOGW("broan", "Ignoring request to set fan mode to ovr; override is a report-only state from external controls");
		m_vecFields[FanMode].markDirty();
		return;
	}
	else if( mode == "recirculate" )
		value = BroanFanMode::Recirculate;
	else
		value = BroanFanMode::Off;


	publishFanModeSource("esphome");

	std::vector<BroanField_t> vecFields;
	vecFields.push_back( m_vecFields[FanMode].copyForUpdate( value ) );

	m_vecFields[FanMode].markDirty();

	writeRegisters( vecFields );
	startFanModeOptimistic( value );

}

void BroanComponent::setTargetCFMRegister( uint8_t opcodeHigh, uint8_t opcodeLow, float flTargetCFM )
{
	uint32_t unField = lookupFieldIndex( opcodeHigh, opcodeLow );
	if( unField == INVALID_FIELD )
	{
		ESP_LOGW("broan","Refusing to set unmapped target CFM register %02X%02X", opcodeHigh, opcodeLow );
		return;
	}

	switch( unField )
	{
		case BroanField::CFMIn_Medium:
		case BroanField::CFMOut_Medium:
		case BroanField::CFMIn_Max:
		case BroanField::CFMOut_Max:
		case BroanField::CFMIn_Min:
		case BroanField::CFMOut_Min:
			break;

		default:
			ESP_LOGW("broan","Refusing to set non-target-CFM register %02X%02X", opcodeHigh, opcodeLow );
			return;
	}

	if( flTargetCFM < 0 )
		flTargetCFM = 0;
	else if( flTargetCFM > 255 )
		flTargetCFM = 255;

	std::vector<BroanField_t> vecFields;
	vecFields.push_back( m_vecFields[unField].copyForUpdate( flTargetCFM ) );
	m_vecFields[unField].markDirty();

	writeRegisters( vecFields );
}

void BroanComponent::resetFilter()
{
	std::vector<BroanField_t> vecFields;

	uint32_t unNewFilterLife = FILTER_LIFE_MAX;
	uint8_t unFilterReset = 0;

	vecFields.push_back( m_vecFields[FilterLife].copyForUpdate( unNewFilterLife ) );
	vecFields.push_back( m_vecFields[FilterReset].copyForUpdate( unFilterReset ) );

	m_vecFields[FilterReset].markDirty();
	m_vecFields[FilterLife].markDirty();

	writeRegisters( vecFields );
}

void BroanComponent::setHumidityControl( bool enable ) {
	std::vector<BroanField_t> vecFields;

	uint8_t value = 0;

	if (enable) {
		value = 0x01;
	}

	vecFields.push_back( m_vecFields[HumidityControl].copyForUpdate( value ) );

	m_vecFields[HumidityControl].markDirty();

	writeRegisters( vecFields );
}

void BroanComponent::setHumiditySetpoint( float humidity ) {
	std::vector<BroanField_t> vecFields;

	vecFields.push_back( m_vecFields[TargetHumidityA].copyForUpdate( humidity ) );
	vecFields.push_back( m_vecFields[TargetHumidityB].copyForUpdate( humidity ) );

	m_vecFields[TargetHumidityA].markDirty();
	m_vecFields[TargetHumidityB].markDirty();

	writeRegisters( vecFields );
}

void BroanComponent::setCurrentHumidity( float humidity ) {
	std::vector<BroanField_t> vecFields;
  
	ESP_LOGI("broan_control", "Set current humidity: %0.1f%%", humidity);

	vecFields.push_back( m_vecFields[ControllerHumidity].copyForUpdate( humidity ) );
	m_vecFields[ControllerHumidity].markDirty();

	writeRegisters( vecFields );
}

void BroanComponent::setIntermittentPeriod( uint32_t period ) {
	std::vector<BroanField_t> vecFields;

	// S -> MS
	//period *= 1000;
  
	ESP_LOGI("broan_control", "Set int period: %i", period);

	vecFields.push_back( m_vecFields[IntModeDuration].copyForUpdate( period ) );
	m_vecFields[IntModeDuration].markDirty();

	writeRegisters( vecFields );
}

}  // namespace broan
}  // namespace esphome
