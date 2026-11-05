#pragma once
namespace plus_board {
// BQ27220: write the Plus pack values (1871 mAh, EDV 3300/3400/3500 mV) when the
// gauge still has another configuration. Returns true when the gauge holds ours.
bool configureGauge();
// True once configureGauge() verified the pack values this boot.
bool gaugeConfigured();
}  // namespace plus_board
