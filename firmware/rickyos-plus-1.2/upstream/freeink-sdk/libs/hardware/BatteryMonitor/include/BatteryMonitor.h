#pragma once
#include <cstdint>

// FreeInk SDK — battery monitor.
//
// Two backends behind one API (chosen at compile time, so construction and the
// public methods below are identical for both):
//   * ADC (default) — reads a divided LiPo voltage off an ADC pin and maps it to
//     a percentage; an optional charge-status pin (MCP73832 /STAT, active-LOW)
//     drives isCharging().
//   * I2C fuel gauge (FREEINK_BATTERY_I2C_GAUGE) — reads SoC/voltage/charge from a
//     BQ27220 gauge (+ optional BQ25896 charger) over I2C; the ADC pin/divider are
//     ignored. Used by X3 and LilyGo T5 S3. Config comes from
//     BoardConfig::ACTIVE.batteryGauge, and gauge-vs-ADC is chosen at *runtime*
//     (gaugeAddr != 0), so X3 (gauge) and X4 (ADC) work from one C3 binary.
//     GaugeType::Cw32L010Pmu is a third variant of this backend: the CW32L010 PMU
//     on Read Pico is not a register map, so it arrives through the board-installed
//     PmuBatteryHook below instead of any register read.
class BatteryMonitor {
public:
    static constexpr int8_t PIN_NONE = -1;

    struct Status {
        enum class PercentageSource : uint8_t { Unknown, StateOfCharge, VoltageEstimate, CapacityEstimate };
        PercentageSource percentageSource = PercentageSource::Unknown;
        bool supported = false;
        bool percentageKnown = false;
        bool millivoltsKnown = false;
        bool chargingKnown = false;
        bool externalPowerKnown = false;
        uint16_t percentage = 0;
        uint16_t millivolts = 0;
        bool charging = false;
        bool externalPower = false;
        // Raw M5PM1 telemetry for diagnostics; -1 when not read (non-PM1 boards
        // or failed I/O). VIN is the DC input rail, 5VINOUT the bidirectional
        // USB-C rail, powerSource the PM1's PWR_SRC bitmap (reg 0x04 [2:0]:
        // bit0=5VIN, bit1=5VINOUT, bit2=BAT).
        int32_t pm1VinMv = -1;
        int32_t pm1VinOutMv = -1;
        int16_t pm1PowerSource = -1;
    };

    // Uses BoardConfig::ACTIVE's battery pins/backend.
    BatteryMonitor();

    // Optional divider multiplier defaults to 2.0; optional charge-status pin
    // defaults to unused.
    explicit BatteryMonitor(int8_t adcPin, float dividerMultiplier = 2.0f, int8_t chargeStatusPin = PIN_NONE);

    // Read voltage and return percentage (0-100)
    uint16_t readPercentage() const;

    // Like readPercentage(), but reports whether the read succeeded. An I2C gauge
    // can fail transiently; on failure this returns false and leaves `out`
    // unchanged, so a caller can keep its last good value. The ADC path always
    // succeeds.
    bool readPercentageChecked(uint16_t& out) const;

    // Read every battery field the active board can report. `supported` is false
    // when the board profile has no battery telemetry path. Per-field `Known`
    // flags distinguish a valid false/zero value from unsupported or failed I/O.
    Status readStatus() const;
#if FREEINK_DEVICE_METALIO_PLUS
    // Raw BQ27220 readings for power measurement (diagnostic builds): voltage, current and
    // average current (mA, negative = discharge), raw coulomb counter (mAh, rises on
    // discharge) and temperature (0.1 C). Each flag says whether its register read.
    struct PowerSnapshot {
        uint16_t mv = 0, counter = 0, soc = 0;
        int16_t current = 0, average = 0, deciCelsius = 0;
        bool mvOk = false, currentOk = false, averageOk = false, counterOk = false, temperatureOk = false, socOk = false;
    };
    PowerSnapshot readPowerSnapshot() const;
#endif

    // Read the battery voltage in millivolts (accounts for divider)
    uint16_t readMillivolts() const;

    // Read the battery voltage in volts (accounts for divider)
    double readVolts() const;

    // True when the battery is actively charging. Sources by backend:
    //   * ADC boards: the MCP73832-style charge-status pin (LOW = charging);
    //     always false when no charge-status pin is configured.
    //   * Gauge boards: a charger IC's status (BQ25896 CHRG_STAT) when present,
    //     else the gauge's own Current() sign (BQ27220, positive = charging), so
    //     a board with a gauge but no charger IC (e.g. X3) still reports it.
    bool isCharging() const;

    // True when external power (a USB cable or charger) is physically present.
    //
    // This is NOT isCharging(): a full battery stops charging while still
    // plugged in, so isCharging() goes false with the cable still attached.
    // Only a source that reports the input rail itself can answer this, so it
    // is currently the BQ25896's REG0B — VBUS_STAT[7:5] plus PG_STAT — and
    // nothing else. Boards without that charger IC cannot observe it.
    //
    // `known` (optional, out) is set false when the board has no way to tell,
    // or the read failed. Callers MUST branch on it: a bare false means "no
    // opinion" just as often as it means "unplugged", and treating the two the
    // same is how you get a device that thinks the cable is gone because it
    // never had a way to see it.
    bool isExternalPowerPresent(bool* known = nullptr) const;

    // Percentage from a millivolt value, off a standard 1S Li-ion discharge
    // curve. The result is always a multiple of 10: voltage cannot resolve a
    // Li-ion pack any finer than that, and pretending otherwise just produces a
    // number that wanders while the battery sits still. 0 mV (a failed read)
    // maps to 0%, not 100%.
    static uint16_t percentageFromMillivolts(uint16_t millivolts);

    // Same lookup, with a deadband around the notch boundaries so a reading that
    // hovers on a boundary keeps reporting `previousPercent` instead of
    // oscillating. Pass the last value this returned; pass > 100 for no history.
    static uint16_t percentageFromMillivolts(uint16_t millivolts, uint16_t previousPercent);

    // --- PMU battery source (BoardConfig::GaugeType::Cw32L010Pmu) --------------
    // Boards whose battery telemetry is not a gauge register map but a device
    // protocol register this hook; the board support library owns the transfer
    // (BoardReadPico::pmuBattery on Read Pico) so this class stays
    // device-agnostic — the same split as Rtc::setPmuTimeHooks.
    //
    //   * batteryMv: pack voltage in mV.
    //   * socPermille: state of charge, 0..1000; the device's "unknown" sentinel
    //     is reported as 0 (the callers then derive the level from batteryMv).
    //   * chargeState: the device's own charge-state code — 0 unknown,
    //     1 not charging, 2 charging, 3 full, 4 fault.
    // Returns false when the device is absent, the frame fails its checksum, or
    // it flags the value invalid. A false return is never turned into a
    // fabricated level: the field is reported unknown instead.
    using PmuBatteryHook = bool (*)(uint16_t& batteryMv, uint16_t& socPermille, uint8_t& chargeState);
    static void setPmuBatteryHook(PmuBatteryHook hook);

private:
    bool hasAdcBackend() const;
    bool hasGaugeBackend() const;
    bool hasM5Pm1Backend() const;
    bool readM5Pm1Status(Status& status) const;

    int8_t _adcPin;
    float _dividerMultiplier;
    int8_t _chargeStatusPin;
};
