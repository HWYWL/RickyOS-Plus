#!/usr/bin/env python3
"""Replay production Plus battery code on the host; never access a device.

Prints synthetic scheduling evidence as JSON, not measured device consumption.
Requires a restored upstream tree and a local C++17 compiler.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
FIRMWARE = PORT / 'firmware/rickyos-plus-1.2'
UPSTREAM = FIRMWARE / 'upstream'


def method(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def audit():
    power = UPSTREAM / 'lib/hal/HalPowerManager.cpp'
    gpio = UPSTREAM / 'lib/hal/HalGPIO.cpp'
    estimator = FIRMWARE / 'components/rickyos_platform/include/PlusBatteryEstimator.h'
    program = r'''
#include "PlusBatteryEstimator.h"
#include <cstdio>
#define FREEINK_DEVICE_READPICO 0
#define FREEINK_DEVICE_METALIO_PLUS 1
namespace BoardConfig {
enum class Board { MetalioPlus, XteinkX3, XteinkX3Uc8279, XteinkX4 };
struct Profile { Board board; struct { int power; } input; };
constexpr Profile ACTIVE{Board::MetalioPlus, {-1}};
}
struct HalGPIO { bool isXteinkDevice() const; };
struct Retention { int pin; };
Retention standbyRetention() { return {-1}; }
namespace plus_board { bool standbySleepReady() { return true; } }
struct HalPowerManager {
  bool canStandbyLightSleep(const HalGPIO&) const;
  static bool sideKeyBusy() { return false; }
};
GPIO_METHOD
POWER_METHOD
using namespace plus_board;
void replay(const char* name, unsigned interval, unsigned duration, int current=12) {
  BatteryEstimator estimator;
  unsigned first=0, samples=0;
  bool anchored=false;
  BatteryEstimate result;
  for (unsigned t=0;t<=duration;t+=interval) {
    result=estimator.update({4366,int16_t(current),int16_t(current),65531,true,true,true,true},t);
    ++samples;
    if (!anchored && result.source==BatteryEstimateSource::CoulombCount) { first=t; anchored=true; }
  }
  std::printf("{\"case\":\"%s\",\"samples\":%u,\"first_anchor_ms\":",name,samples);
  if (anchored) std::printf("%u",first); else std::printf("null");
  std::printf(",\"last_model\":%u,\"last_percent\":%u}\n",unsigned(result.source),result.percent);
}
int main() {
  replay("boot_only",1000,0);
  replay("full_every_60s",60000,600000);
  replay("full_every_90s",90000,630000);
  replay("full_every_120s",120000,600000);
  replay("full_every_180s",180000,720000);
  replay("high_voltage_discharge_every_60s",60000,600000,-30);
  HalGPIO gpio;
  std::printf("{\"case\":\"plus_standby_gate_with_ready_and_idle_stubs\",\"allowed\":%s}\n",
              HalPowerManager{}.canStandbyLightSleep(gpio)?"true":"false");
}
'''.replace('GPIO_METHOD', method(gpio.read_text(), 'bool HalGPIO::isXteinkDevice() const'))
    program = program.replace('POWER_METHOD', method(
        power.read_text(), 'bool HalPowerManager::canStandbyLightSleep(const HalGPIO& gpio) const'))
    with tempfile.TemporaryDirectory(prefix='plus-battery-offline-') as directory:
        source = Path(directory) / 'audit.cpp'
        binary = Path(directory) / 'audit'
        source.write_text(program)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I' + str(estimator.parent), str(source), '-o', str(binary)], check=True)
        rows = [json.loads(row) for row in subprocess.check_output([str(binary)], text=True).splitlines()]
    return {
        'evidence_type': 'synthetic host replay of production code, NOT device measurement',
        'profile_inputs': {'board': 'MetalioPlus', 'input_power': -1,
                           'source': 'components/rickyos_platform/include/BoardConfig.h:2107-2147'},
        'source_sha256': {str(p.relative_to(FIRMWARE)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (power, gpio, estimator)},
        'cases': rows,
        'limits': ['No physical current, capacity, or battery accuracy was measured.',
                   'The sleep replay supplies ready=true and side-key-busy=false stubs; it does not exercise the board sleep backend.',
                   'Synthetic full samples assume valid telemetry and unchanged current/voltage.'],
    }


if __name__ == '__main__':
    print(json.dumps(audit(), ensure_ascii=False, indent=2))
