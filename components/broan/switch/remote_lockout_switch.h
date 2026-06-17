#pragma once

#include "esphome/components/switch/switch.h"
#include "../broan.h"

class BroanComponent;

namespace esphome {
namespace broan {

class RemoteLockoutSwitch : public switch_::Switch, public Parented<BroanComponent> {
 public:
  RemoteLockoutSwitch() = default;

 protected:
  void write_state(bool state) override;
};

}  // namespace broan
}  // namespace esphome
