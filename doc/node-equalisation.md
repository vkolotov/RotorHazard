# Equalisation and 12-bit RSSI on integration-v2

This branch starts at upstream `cd6ebc19` and merges
`feature/node-equalisation` and `feature/node-12bit-rssi`. It does not inherit
the superseded integration implementation. Upstream was fetched on 2026-09-24;
its main branch was still at that base.

Retained fixes include `a3501593` (RSSI-history pruning infinite loop), the
upstream marshal fixes, and the node-array bounds correction from the 12-bit
branch. The 16-bit packet sizes and offsets fixed in the original integration
are also present. Processor metadata is propagated to all receivers sharing a
serial board, so all eight receivers use the correct packet width.

## Firmware protocol

The combined firmware uses **API 38**. API 37 was already deployed with a
different equalisation protocol; the new commands must not be sent to it.

| Operation | Read | Write | Payload |
|---|---|---|---|
| Equalisation pivot | 0x34 | 0x64 | uint16 |
| Upper offset | 0x35 | 0x65 | int16 |
| Upper slope | 0x36 | 0x66 | uint16, Q8 |
| Lower offset | 0x37 | 0x67 | int16 |
| Lower slope | 0x38 | 0x68 | uint16, Q8 |
| ADC resolution | 0x41 | 0x6A | uint8 (10 or 12) |
| Reset extrema | — | 0x69 | uint8, ignored |

STM32 RSSI transport always uses 16-bit fields, even when sampling in legacy
mode. AVR remains 8-bit. Both the pass-statistics and extremum responses are
11 bytes on STM32. The sentinel is 65535, distinct from real ADC values.

Legacy sampling is the default. Settings > Sensor Tuning > RSSI Resolution
selects full 12-bit sampling on STM32. Changing resolution changes the raw scale
by about eight. Stored race traces and thresholds are not rewritten. Calibration
records its ADC mode; coefficients from another mode are disabled until that
mode is restored or calibration is reset. Changing resolution is refused during
a race, staging, or an active calibration capture.

## Equalisation

Keep the quad in one fixed position. Capture noise with the VTX completely off,
then low (PIT) and high (race power) for every distinct receiver channel. Each
capture resets extrema and measures for five seconds. Apply fits two Q8 segments:

    corrected = ((raw - offset) * slope) >> 8

The pivot chooses the upper or lower segment. Correction runs in firmware before
filtering and crossing detection. The server chooses target levels at 0.7%, 7%,
and 20% of the active raw range (255 or 4095). The lower segment keeps the idle
trace visibly above zero. Reset clears both saved and hardware coefficients.

Coefficients are specific to each receiver's calibrated channel. Retuning a
receiver requires recalibration. See the retained measurement reports
[12-bit measurements](12bit-measurements.md) and
[channel survey](rssi-channel-survey.md) for the earlier hardware experiments.
Those reports describe historical firmware, not the API 38 command layout.

## Upgrading the original integration deployment

Back up the database with SQLite's backup API (including live WAL contents),
the config, and the old firmware before deployment. Pull this branch using Git.
Before restarting the server, run:

    python3 src/server/util/add_equalisation_columns.py ~/rh-data/database.db

The migration adds the five profile columns and converts existing `eq_kups` /
`eq_klos` calibrations once. It preserves their original target of 300 and the
existing thresholds, rather than rescaling them to the new wizard targets.
Integer offsets introduce small rounding differences (up to two RSSI counts for
the deployed calibration). Old columns remain for rollback.

Keep `GENERAL.FULL_RSSI_RESOLUTION` true for this previously 12-bit deployment.
Flash the API 38 firmware using the existing server's node-update function, then
restart into the new server. The updater stops hardware polling while flashing.
Verify all eight receivers, the resolution setting, calibration, thresholds,
heartbeat values, and communication errors. A live quad pass remains the final
check of detection under race conditions.

Build STM32F411 firmware with core **2.12.0**, which has already run successfully
on this timer. The firmware artifact under `firmware/integration-v2` records the
source commit, build options, and SHA-256. AVR is build-checked but is not deployed
to this STM32 timer.
