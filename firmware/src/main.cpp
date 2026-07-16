// MXChip AZ3166 home telemetry — Milestone 1
// Read onboard sensors (HTS221 temp/humidity, LPS22HB pressure), show them on
// the OLED, and print them over serial. No Wi-Fi yet — this proves the toolchain,
// the Microsoft AZ3166 SDK, and the physical sensors all work end to end.

#include "Arduino.h"
#include "OledDisplay.h"
#include "HTS221Sensor.h"
#include "LPS22HBSensor.h"

// Onboard sensors share the I2C bus on pins D14 (SDA) / D15 (SCL).
static DevI2C *i2c = NULL;
static HTS221Sensor *tempHumSensor = NULL;   // temperature + humidity
static LPS22HBSensor *pressureSensor = NULL;  // barometric pressure

static const int SAMPLE_INTERVAL_MS = 2000;

void setup()
{
    Serial.begin(115200);

    Screen.init();
    Screen.clean();
    Screen.print(0, "IoT Telemetry");
    Screen.print(1, "Init sensors...");

    i2c = new DevI2C(D14, D15);

    tempHumSensor = new HTS221Sensor(*i2c);
    tempHumSensor->init(NULL);
    tempHumSensor->enable();

    pressureSensor = new LPS22HBSensor(*i2c);
    pressureSensor->init(NULL);

    Serial.println("Sensors initialized. Sampling...");
}

void loop()
{
    float temperature = 0.0f;
    float humidity = 0.0f;
    float pressure = 0.0f;

    tempHumSensor->getTemperature(&temperature);
    tempHumSensor->getHumidity(&humidity);
    pressureSensor->getPressure(&pressure);

    char line[24];

    Screen.clean();
    snprintf(line, sizeof(line), "Temp:  %.1f C", temperature);
    Screen.print(0, line);
    snprintf(line, sizeof(line), "Humid: %.1f %%", humidity);
    Screen.print(1, line);
    snprintf(line, sizeof(line), "Press: %.1f hPa", pressure);
    Screen.print(2, line);

    // hPa is the SDK's native pressure unit for LPS22HB.
    Serial.printf("temp=%.2f C  humidity=%.2f %%  pressure=%.2f hPa\r\n",
                  temperature, humidity, pressure);

    delay(SAMPLE_INTERVAL_MS);
}
