#include "remote_lockout_switch.h"

namespace esphome {
namespace broan {

void RemoteLockoutSwitch::write_state(bool state) {
  this->parent_->setRemoteLockout(state);
  this->publish_state(state);
}

}  // namespace broan
}  // namespace esphome
