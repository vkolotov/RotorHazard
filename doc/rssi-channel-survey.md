# RSSI channel survey - why equalisation must be per-channel

Measured 2026-09-22 on the 8-node S32_BPill (BlackPill STM32F411) timer with
12-bit RSSI firmware (API 39). Equalisation was cleared first, so every value
below is **raw ADC**, 0-4095.

## Question

Per-node equalisation (one floor offset + one scale factor per node) visibly
improved node agreement, but only partly. Switching the quad between PIT and
25 mW showed different deltas per node, and R1 in particular behaved oddly.
Hypothesis: the correction is channel-dependent, so one constant pair per node
cannot hold across the band.

## Method

The quad's channel is the outer loop. For each channel R1..R8:

1. all 8 nodes tuned to that channel (`rssi_survey.py at Rn <level>`)
2. quad on the same channel, held in one fixed position throughout
3. three levels captured: **floor** (VTX fully off), **pit**, **race** (25 mW)

Each measurement is an 8 s sample (~79 heartbeats) after a 3 s settle; the mean
is recorded. Raw data: `~/rh-data/rssi_survey.csv` on the timer.

Holding the quad in one position for all 24 captures is what makes the numbers
comparable. Tuning every node to the quad's channel removes VTX output
variation between channels as a confounder - each node is measured against the
same transmitter at the same frequency at the same instant.

**Caveat:** with all 8 nodes on one frequency they desense each other somewhat.
Comparisons here are per node across levels and channels, which is unaffected;
absolute level comparisons between nodes on one channel would not be safe.

## Findings

**1. Single-channel calibration does not transfer.** Constants fitted on R1 and
applied band-wide reduce cross-node spread from 21.3% to 20.0% - and make five
of eight channels *worse* than no correction at all. It is effectively useless.

**2. Front-end sensitivity is channel-dependent for half the fleet.** Nodes 1,
6, 7, 8 are close to frequency-flat (5.8-9.1% variation in span). Nodes 2, 3, 4,
5 vary 17.5-24.1%, and not in the same direction: node 4 dips mid-band then
climbs to R8, node 5 and node 7 rise toward the top of the band, node 1 falls
from R1 then flattens. This is ordinary bandpass-filter and antenna response,
which differs part to part - not a difference in detector behaviour.

**3. The detector is logarithmic, and nodes sit at different points on it.**
Every RX5808 has the same log-detector response - that part is not node
specific. What differs is front-end gain, so the same input power lands at a
different place on each node's curve. Measured as the ratio of the two steps,
(pit-floor)/(race-pit), this varies 4.8x across the fleet: node 1 averages 3.10
(high gain, already compressing by 25 mW, so a small race-pit step) while node 5
averages 0.64 (lower gain, still in its linear region at 25 mW, so a large
step). Slope alone varies 2.1x between nodes on one channel and up to 60% for
one node across the band.

A two-point affine fit `(raw - floor) * scale` therefore pins its two fitted
levels and diverges in between, which is exactly the PIT/25 mW behaviour that
prompted this survey. Correcting properly would have to account both for where
a node sits on the curve and for how that shifts with frequency.

**4. Noise floor is channel-dependent too.** Node 4 moves 172 counts across the
band, node 2 141, while node 3 moves 14. Shapes again differ per node, so this
is per-node filter/antenna response, not common-mode interference.

## Outcome

The non-linearity in finding 3 was resolved by replacing the single-gain fit
with a two-segment piecewise correction fitted on three levels - see
`node-equalisation.md`. Finding 1 was not resolved and became a documented
constraint: each node must stay on the channel it was calibrated on.

## Conclusion

Equalisation constants must be stored **per node per channel** (8 x 8 = 64
pairs), not per node. The alternative is to keep every node on the channel it
was calibrated on, in which case per-node constants measured on that node's own
channel are correct and sufficient.

Cost: the floor sweep is unattended and takes ~5 minutes with no quad. The
scale capture needs the quad on each channel at race power - 16 manual steps,
but only once per hardware set.

## Captured data

### Noise floor (VTX off), raw ADC

| node | R1 | R2 | R3 | R4 | R5 | R6 | R7 | R8 | min | max | spread | %var |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 730 | 763 | 786 | 792 | 789 | 784 | 786 | 790 | 730 | 792 | 63 | 8.1% |
| 2 | 713 | 770 | 796 | 784 | 733 | 687 | 666 | 654 | 654 | 796 | 141 | 19.5% |
| 3 | 564 | 564 | 558 | 551 | 551 | 553 | 553 | 557 | 551 | 564 | 14 | 2.4% |
| 4 | 767 | 827 | 875 | 909 | 886 | 824 | 766 | 736 | 736 | 909 | 172 | 20.9% |
| 5 | 774 | 770 | 766 | 762 | 757 | 750 | 742 | 730 | 730 | 774 | 44 | 5.8% |
| 6 | 876 | 858 | 848 | 857 | 874 | 874 | 864 | 851 | 848 | 876 | 29 | 3.3% |
| 7 | 733 | 741 | 740 | 728 | 713 | 700 | 689 | 677 | 677 | 741 | 64 | 9.0% |
| 8 | 674 | 670 | 671 | 677 | 685 | 697 | 708 | 715 | 670 | 715 | 45 | 6.5% |

### PIT level, raw ADC

| node | R1 | R2 | R3 | R4 | R5 | R6 | R7 | R8 | min | max | spread | %var |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1296 | 1322 | 1349 | 1310 | 1331 | 1350 | 1371 | 1379 | 1296 | 1379 | 83 | 6.2% |
| 2 | 1109 | 1113 | 1204 | 1214 | 1107 | 987 | 908 | 1032 | 908 | 1214 | 306 | 28.2% |
| 3 | 823 | 901 | 923 | 921 | 917 | 951 | 962 | 848 | 823 | 962 | 139 | 15.3% |
| 4 | 1141 | 1298 | 1397 | 1437 | 1416 | 1355 | 1298 | 1230 | 1141 | 1437 | 297 | 22.5% |
| 5 | 1068 | 1007 | 965 | 997 | 1055 | 1147 | 1170 | 1146 | 965 | 1170 | 205 | 19.2% |
| 6 | 1318 | 1221 | 1174 | 1168 | 1338 | 1372 | 1380 | 1387 | 1168 | 1387 | 219 | 16.9% |
| 7 | 1164 | 1228 | 1245 | 1115 | 1198 | 1201 | 1205 | 1173 | 1115 | 1245 | 129 | 10.9% |
| 8 | 1126 | 1109 | 1091 | 1004 | 1068 | 1167 | 1185 | 1154 | 1004 | 1185 | 181 | 16.2% |

### Race level (25 mW), raw ADC

| node | R1 | R2 | R3 | R4 | R5 | R6 | R7 | R8 | min | max | spread | %var |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1520 | 1524 | 1526 | 1526 | 1523 | 1525 | 1525 | 1526 | 1520 | 1526 | 6 | 0.4% |
| 2 | 1408 | 1418 | 1431 | 1436 | 1421 | 1385 | 1356 | 1410 | 1356 | 1436 | 80 | 5.7% |
| 3 | 1252 | 1326 | 1339 | 1346 | 1343 | 1367 | 1381 | 1324 | 1252 | 1381 | 130 | 9.7% |
| 4 | 1552 | 1607 | 1619 | 1625 | 1622 | 1623 | 1618 | 1608 | 1552 | 1625 | 73 | 4.5% |
| 5 | 1532 | 1511 | 1475 | 1501 | 1552 | 1626 | 1640 | 1633 | 1475 | 1640 | 165 | 10.6% |
| 6 | 1650 | 1625 | 1614 | 1612 | 1655 | 1666 | 1667 | 1666 | 1612 | 1667 | 55 | 3.3% |
| 7 | 1529 | 1543 | 1554 | 1528 | 1543 | 1550 | 1556 | 1549 | 1528 | 1556 | 29 | 1.9% |
| 8 | 1490 | 1491 | 1489 | 1451 | 1476 | 1517 | 1524 | 1518 | 1451 | 1524 | 73 | 4.9% |

### Span (race - floor)

| node | R1 | R2 | R3 | R4 | R5 | R6 | R7 | R8 | min | max | spread | %var |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 791 | 761 | 741 | 734 | 734 | 741 | 739 | 735 | 734 | 791 | 57 | 7.7% |
| 2 | 695 | 648 | 636 | 651 | 687 | 698 | 690 | 755 | 636 | 755 | 120 | 17.5% |
| 3 | 688 | 762 | 782 | 795 | 792 | 814 | 828 | 766 | 688 | 828 | 140 | 18.0% |
| 4 | 785 | 780 | 743 | 716 | 736 | 799 | 852 | 872 | 716 | 872 | 156 | 19.9% |
| 5 | 758 | 741 | 709 | 739 | 795 | 876 | 898 | 902 | 709 | 902 | 193 | 24.1% |
| 6 | 774 | 767 | 767 | 756 | 780 | 792 | 803 | 815 | 756 | 815 | 59 | 7.6% |
| 7 | 796 | 802 | 814 | 800 | 830 | 850 | 867 | 872 | 796 | 872 | 76 | 9.1% |
| 8 | 816 | 820 | 818 | 774 | 790 | 820 | 816 | 803 | 774 | 820 | 47 | 5.8% |

### Slope (race - pit), counts per fixed dB step

| node | R1 | R2 | R3 | R4 | R5 | R6 | R7 | R8 | min | max | spread | %var |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 225 | 202 | 177 | 216 | 192 | 175 | 154 | 146 | 146 | 225 | 78 | 42.2% |
| 2 | 299 | 304 | 227 | 221 | 313 | 398 | 447 | 378 | 221 | 447 | 226 | 69.8% |
| 3 | 428 | 426 | 416 | 425 | 426 | 416 | 419 | 476 | 416 | 476 | 60 | 14.1% |
| 4 | 412 | 309 | 222 | 187 | 206 | 268 | 320 | 379 | 187 | 412 | 224 | 77.9% |
| 5 | 464 | 504 | 510 | 505 | 497 | 479 | 470 | 487 | 464 | 510 | 46 | 9.5% |
| 6 | 332 | 404 | 440 | 444 | 317 | 294 | 287 | 279 | 279 | 444 | 165 | 47.2% |
| 7 | 365 | 315 | 310 | 412 | 345 | 349 | 352 | 376 | 310 | 412 | 103 | 29.1% |
| 8 | 364 | 381 | 398 | 446 | 407 | 350 | 339 | 364 | 339 | 446 | 107 | 28.1% |

### Operating point: (pit - floor) / (race - pit)

Higher = node is further up its log curve and compressing sooner.
Same detector in every node; the ratio differs because front-end gain does.

| node | R1 | R2 | R3 | R4 | R5 | R6 | R7 | R8 | mean |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 2.52 | 2.76 | 3.19 | 2.40 | 2.83 | 3.24 | 3.81 | 4.02 | **3.10** |
| 2 | 1.32 | 1.13 | 1.80 | 1.94 | 1.19 | 0.75 | 0.54 | 1.00 | **1.21** |
| 3 | 0.61 | 0.79 | 0.88 | 0.87 | 0.86 | 0.96 | 0.98 | 0.61 | **0.82** |
| 4 | 0.91 | 1.52 | 2.35 | 2.82 | 2.58 | 1.98 | 1.67 | 1.30 | **1.89** |
| 5 | 0.63 | 0.47 | 0.39 | 0.46 | 0.60 | 0.83 | 0.91 | 0.85 | **0.64** |
| 6 | 1.33 | 0.90 | 0.74 | 0.70 | 1.46 | 1.70 | 1.80 | 1.92 | **1.32** |
| 7 | 1.18 | 1.55 | 1.63 | 0.94 | 1.41 | 1.44 | 1.47 | 1.32 | **1.37** |
| 8 | 1.24 | 1.15 | 1.05 | 0.73 | 0.94 | 1.35 | 1.41 | 1.21 | **1.14** |

### Correction strategy comparison

| channel | uncorrected | calibrated on R1 only | per-channel |
|---|---|---|---|
| R1 | 16.8% | 0.0% | 0.0% |
| R2 | 22.6% | 17.5% | 0.0% |
| R3 | 24.3% | 22.5% | 0.0% |
| R4 | 19.9% | 24.8% | 0.0% |
| R5 | 18.5% | 22.1% | 0.0% |
| R6 | 22.3% | 23.4% | 0.0% |
| R7 | 25.7% | 25.2% | 0.0% |
| R8 | 20.5% | 24.4% | 0.0% |
| **mean** | **21.3%** | **20.0%** | **0.0%** |

## Reproducing

`src/server/rssi_survey.py` on the timer:

    ~/.venv/bin/python rssi_survey.py tune R1        # all nodes -> R1
    ~/.venv/bin/python rssi_survey.py at R1 floor    # tune + capture
    ~/.venv/bin/python rssi_survey.py sweep floor R  # all 8 channels, unattended
    ~/.venv/bin/python rssi_survey.py show           # summary table

`sweep` is only safe for the floor level, which needs no quad. The pit and race
levels need the quad retuned to each channel, so use `at` one channel at a time.

Clear equalisation first, or the captures record corrected values instead of raw.
