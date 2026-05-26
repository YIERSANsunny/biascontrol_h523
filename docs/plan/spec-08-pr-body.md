# PR Body - MZM + DPMZM 单板集成

## Summary

This PR integrates the MZM and DPMZM bias-control flows into one STM32H523 firmware. The first integration target is **runtime-selectable operation**, not simultaneous dual closed-loop control.

The firmware keeps separate shell namespaces:

```text
mzm ...
dpmzm ...
mode mzm
mode dpmzm
```

This lets the same board firmware run either MZM or DPMZM control logic while keeping both algorithms independent.

## What Changed

- Migrated DPMZM modules into the unified `src/` layout.
- Added DPMZM shell namespace under the unified runtime shell.
- Routed shared SPI/DMA callbacks so DAC and DPMZM pilot paths can coexist.
- Added DPMZM DAC write pacing on the unified DMA path.
- Restored DPMZM onboard pilot generation after integration.
- Isolated DPMZM scan/Goertzel measurement constants to the validated 32 kSPS path.
- Added helper scripts for flashing and DPMZM scan capture/plotting.
- Added integration status and PR/merge documentation.

## Important Integration Fixes

### 1. DPMZM pilot output after integration

During integration, DPMZM status could report continuous pilot output while the oscilloscope showed no valid pilot waveform. The root cause was TIM6 being affected by the mainline low-rate timer configuration.

Fix:

- DPMZM pilot startup now checks and restores TIM6 to the expected pilot update rate.
- `dpmzm status` reports the effective TIM6 pilot rate.

Expected status:

```text
pilot tim6 fs: 16000.00 Hz (ok)
```

### 2. DPMZM scan tones about 30 dB too low

The first integrated DPMZM scan showed I/Q/P metrics about 28-32 dB lower than the known-good DPMZM branch. This was not a plotting issue; the firmware was measuring the wrong Goertzel bins.

Root cause:

- The known-good DPMZM chain uses a validated 32 kSPS measurement path.
- The integration branch temporarily inherited the mainline/global 64 kSPS DSP constants.
- The DPMZM Goertzel detector then looked for 1000/1200/2200 Hz using the wrong sample-rate assumption.

Fix:

- Added `src/control/ctrl_dpmzm_dsp.h`.
- DPMZM scan, raw capture, Goertzel block size, and status output now use DPMZM-specific constants:

```text
DPMZM_DSP_SAMPLE_RATE_HZ = 32000
DPMZM_DSP_GOERTZEL_BLOCK_SIZE = 640
```

This keeps DPMZM on the experimentally verified path without changing the MZM/global DSP assumptions.

## Validation

Build:

```text
cmake --build build -j 8
```

Board-level validation completed:

- DPMZM pilot output verified on oscilloscope.
- DPMZM status reports expected pilot timing and 32 kSPS DSP rate.
- DPMZM scan curves recovered to the same magnitude level as the known-good branch.
- DPMZM auto-find / auto-lock flow verified.
- Runtime mode switching smoke test verified.

Known limitation:

- Full MZM board-level closed-loop regression was not available in the current lab setup. The MZM command namespace remains integrated, and a hardware regression is recommended before production use.

## Scope Boundary

This PR does not:

- Run MZM and DPMZM closed loops at the same time.
- Convert DPMZM to 64 kSPS.
- Rewrite the existing MZM or DPMZM algorithms.
- Merge the two control state machines.

The intended V1 behavior is:

```text
same firmware + same board + selectable MZM/DPMZM runtime mode
```

## Rollback Points

Useful tags:

```text
dpmzm-known-good-2026-05-21
integration-step-04-dpmzm-pilot-dsp-ok
integration-v1-mzm-dpmzm-switchable
```

## Suggested Post-Merge Checks

```text
mode dpmzm
dpmzm status
dpmzm set pilot-open on
dpmzm auto lock
dpmzm auto status
dpmzm lock status

mode mzm
mzm status
```

Expected DPMZM checks:

- `pilot tim6 fs` is about `16000 Hz (ok)`.
- `adc dsp fs` is `32000 Hz`.
- I/Q first-order scan levels do not drop by 30 dB.
- P-QTP scan shows a recognizable 2200 Hz valley.
