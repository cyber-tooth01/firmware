// SEN66Sensor.h - Corrected I2C Master Implementation
// Replace your existing src/modules/Telemetry/Sensor/SEN66Sensor.h with this

#pragma once

#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "TelemetrySensor.h"

class SEN66Sensor : public TelemetrySensor
{
  private:
    TwoWire *i2cBus = nullptr;

  public:
    SEN66Sensor();
    virtual bool getMetrics(meshtastic_Telemetry *measurement) override;
    virtual bool initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev) override;
    virtual int32_t runOnce() override;
};

#endif
