# 12-bit RSSI measurements (2026-09-21)

## Firmware under test

`RH_S32_BPill_node_STM32F4_v2_12bit.bin`, API 37, built on STM32 core 2.12.0.
Server patched (16-bit RSSI path). 8 nodes, all enumerated, zero comm errors.

## Ratio verification vs old 8-bit firmware

Predicted ratio 8.0 (10-bit ADC then `>>1` -> 12-bit raw).

Measured: mean floor ratio **7.935**, mean peak ratio **7.924** (stdev 0.21 / 0.12).
Confirms the pipeline is faithful - no clipping, truncation or wraparound.

| node | old floor | new floor | old peak | new peak | old span | new span | new span / 8 |
|------|-----------|-----------|----------|----------|----------|----------|--------------|
| 1    |  91 |  715 | 189 | 1500 |  98 | 785 |  98.1 |
| 2    |  95 |  730 | 179 | 1420 |  84 | 690 |  86.2 |
| 3    |  70 |  560 | 167 | 1350 |  97 | 790 |  98.8 |
| 4    | 116 |  875 | 203 | 1615 |  87 | 740 |  92.5 |
| 5    |  95 |  772 | 205 | 1630 | 110 | 858 | 107.2 |
| 6    | 109 |  890 | 208 | 1650 |  99 | 760 |  95.0 |
| 7    |  88 |  700 | 194 | 1545 | 106 | 845 | 105.6 |
| 8    |  89 |  725 | 190 | 1450 | 101 | 725 |  90.6 |

Mean span 97.8 (old) vs 96.8 (new/8) - same physical measurement.

## Loop time (442 heartbeat samples, 45 s)

| node | mean us | median | min | max | stdev |
|------|---------|--------|-----|-----|-------|
| 1 | 1000.6 | 1001 | 942 | 1055 | 16.5 |
| 2 |  996.5 |  996 | 926 | 1066 | 22.8 |
| 3 |  996.5 |  996 | 918 | 1071 | 28.3 |
| 4 |  996.1 |  995 | 914 | 1086 | 32.5 |
| 5 |  986.1 |  987 | 878 | 1127 | 37.4 |
| 6 |  987.0 |  985 | 873 | 1100 | 39.1 |
| 7 |  984.6 |  984 | 818 | 1135 | 43.9 |
| 8 |  984.9 |  984 | 850 | 1106 | 44.3 |

**Fleet: mean 991.5 us, median 993.0, stdev 34.9**

Compare against the 8-bit baseline build (`_v2_baseline.bin`) using the same
45 s heartbeat capture to get a true delta. The concern was that the 255-wide
running median's early-out (`if (new_value == old_value) return;`) fires less
often once values rarely repeat, forcing more O(N) passes.

## Idle RSSI jitter (same capture)

| node | mean | min | max | jitter | stdev | jitter / 8 |
|------|------|-----|-----|--------|-------|------------|
| 1 | 729.8 | 718 | 743 | 25 | 3.7 | 3.1 |
| 2 | 772.4 | 763 | 780 | 17 | 2.8 | 2.1 |
| 3 | 554.4 | 547 | 561 | 14 | 2.5 | 1.8 |
| 4 | 912.4 | 904 | 921 | 17 | 2.9 | 2.1 |
| 5 | 756.4 | 747 | 766 | 19 | 2.9 | 2.4 |
| 6 | 875.6 | 867 | 884 | 17 | 3.0 | 2.1 |
| 7 | 689.1 | 680 | 696 | 16 | 2.7 | 2.0 |
| 8 | 715.3 | 707 | 722 | 15 | 2.6 | 1.9 |

A shorter earlier sample on node 8 showed 7 counts peak-to-peak; over 45 s it
widens to 15. **Use the stdev (~2.6-3.7 counts) as the noise figure** - peak-to-peak
grows with sample count and overstates the noise.

## Resolution gain

Using stdev as the noise measure, ~3 counts on a ~774-count span:

- old: span 98, noise ~0.4 old counts (3/8) -> quantisation-limited at 1 count
- new: span 774, noise ~3 counts -> ~258 distinguishable levels

The analog noise sits **below one old quantisation step** (5.6 mV measured vs
6.45 mV old effective step), which is the signature of a quantisation-limited
measurement - exactly the case where more ADC bits recover real information.

Conservative estimate of usable discrimination improvement: **~1.5-2x**.
Not the 8x the raw count ratio suggests, since noise scales too.

## Equalisation constants

Re-measure floor/peak directly on 12-bit firmware at a marked quad position
rather than scaling the 8-bit numbers - direct readings already differ from
x8 predictions by up to 6% (node 4 floor: 875 measured vs 928 predicted).
