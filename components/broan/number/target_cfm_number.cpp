#include "target_cfm_number.h"

namespace esphome {
namespace broan {

void TargetCFMNumber::control(float value)
{
	this->parent_->setTargetCFMRegister( this->opcode_high_, this->opcode_low_, value );
}

}  // namespace broan
}  // namespace esphome
