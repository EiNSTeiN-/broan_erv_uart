#pragma once

#include "esphome/components/number/number.h"
#include "../broan.h"

class BroanComponent;

namespace esphome {
namespace broan {

class TargetCFMNumber : public number::Number, public Parented<BroanComponent> {
 public:
	TargetCFMNumber() = default;
	void set_opcode(uint8_t opcode_high, uint8_t opcode_low) {
		this->opcode_high_ = opcode_high;
		this->opcode_low_ = opcode_low;
	}

 protected:
	void control(float value) override;

	uint8_t opcode_high_{0};
	uint8_t opcode_low_{0};
};

}  // namespace broan
}  // namespace esphome
