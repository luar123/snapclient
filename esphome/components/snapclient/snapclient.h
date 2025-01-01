#pragma once

#include "esphome.h"

namespace esphome {
namespace snapclient {

static const char *const TAG = "snapclient";

class SnapClientComponent : public Component {
 public:
  void setup() override;
  //void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
};


}  // namespace snapclient
}  // namespace esphome