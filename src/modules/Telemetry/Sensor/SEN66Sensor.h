#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && __has_include(<SensirionI2cSen66.h>)

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "TelemetrySensor.h"
#include <SensirionI2cSen66.h>

class SEN66Sensor : public TelemetrySensor
{
  private:
    SensirionI2cSen66 sensor;
    uint32_t lastFanCleanTime = 0;
    static const uint32_t FAN_CLEANING_INTERVAL_MS = 7 * 24 * 60 * 60 * 1000; // 7 days
    uint32_t readCount = 0;

  public:
    SEN66Sensor();
    virtual bool getMetrics(meshtastic_Telemetry *measurement) override;
    virtual bool initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev) override;
    virtual int32_t runOnce() override;

  private:
    void performFanCleaning();
    void setupTemperatureCompensation();
    void checkDeviceStatus();
};

#endif