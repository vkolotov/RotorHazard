# Automatic equalisation

## Current implementation (8-bit validation)

The automatic sweep uses the pilot's ELRS backpack to command the VTX, then
checks the receiver RSSI before recording a calibration measurement.

1. Capture noise with the quad powered **off**.
2. Power the quad and connect the radio. Keep the quad stationary near the timer
   for the high pass, then move it farther away for the low pass.
3. At the start of each pass, command a different watched channel and allow
   seven seconds to settle. This prepares a measurable transition even when
   the quad started on the first capture channel. The parking channel is not
   claimed as confirmed and is never captured at this step.
4. Command the capture channel and compare live RSSI with its pre-command
   baseline. Require two consecutive observations of a clear rise on that
   channel. Similar rises on competing channels are ambiguous, not success.
5. Retry an unconfirmed command at seven-second intervals, at most three
   additional sends within a bounded 28-second confirmation window.
6. Only after confirmation, clear peaks once and capture for three seconds.
   Stop on failure instead of recording or skipping past an uncertain channel.
7. Review both passes, then Apply explicitly.

The initial move is deliberately not replaced with “highest RSSI means the
right channel”: receiver sensitivity and adjacent-channel bleed invalidate that
shortcut. If the initial parking command fails and no target transition can be
established, the pass stops rather than accepting unchanged readings.

Automatic capture currently requires at least two distinct watched channels.
Use **Current channels**. A selected band scope is accepted only if every
channel in that scope is watched by an enabled node. Sweeping unwatched channels
would provide no trustworthy confirmation or useful per-node fit. This does not
implement retuning every receiver across multiple bands or storing a bank of
frequency-specific fits.

The pilot-address / command / address-reset sequence holds the ELRS plugin's
queue lock, preventing other addressed plugin operations from interleaving.

## Protocol findings

The radio Lua script invokes the transmitter's VTX send action. In ExpressLRS
3.5.6, `VtxTriggerSend()` schedules three sends: initially after one second,
then at 500 ms intervals. A backpack `MSP_SET_VTX_CONFIG` command invokes the
same action. Lua command-status polling is not an acknowledgement from the VTX.

- [Lua command handling](https://github.com/ExpressLRS/Lua/blob/670c8099a91da6c8f5baa44edbdecfacb0e0c6da/elrs.lua)
- [ELRS VTX sending](https://github.com/ExpressLRS/ExpressLRS/blob/3.5.6/src/lib/VTX/devVTX.cpp)
- [Backpack command entry](https://github.com/ExpressLRS/ExpressLRS/blob/3.5.6/src/src/tx_main.cpp)

The earlier blanket 10-second command-blackout explanation was not established.
The disconnect debounce depends on the ELRS version (one second in 3.5.6), and
explicit VTX commands set the state to MODIFIED. EEPROM writes are requested by
the ELRS sequence, but Betaflight's handler saves/reloads configuration rather
than explicitly rebooting. Do not equate a configuration write with a guaranteed
flight-controller restart, or a changed goggles channel with quad confirmation.

## Validation and limits

Simulation tests exercise all eight possible starting channels, lost commands,
exhausted retries, weak 8-bit signals, ambiguous rises, cancellation, independent
per-channel captures, and high + low + Apply through the actual sweep/detector.
Hardware validation is separate; simulations do not prove delivery over RF.
Only one quad should transmit during calibration. Noise capture while the quad
is on invalidates the calibration, even though transition detection itself does
not use the noise floor. Neither power control nor firmware flashing is needed.

## Earlier field observations

## Measured behaviour

All on the eight-node fleet with the quad at the gate at 25 mW.

**Noise floor, quad fully powered down:**

```
node    1    2    3    4    5    6    7    8
floor  90   94   69  111   93  108   85   88
```

**Adjacent channels bleed hard.** With the quad on R2, the R1 node read 58 over
its floor and R7 read 67, against R2's own 84 - a separation of 26. With the
quad on R6, R7 rose 64 while R6 rose 99. Any rule that asks "which node reads
highest, and is it clear of the next" is deciding on a small margin exactly
where the signal is strongest.

**Receivers differ by more than a signal does.** Floors span 69 to 111. On
arrival, the least sensitive node gains about 30 counts where the best gains
100. A fixed threshold that suits one calls the other a failure.

**A switch is large and unambiguous when read as a change.** Traced across one:

```
t=2.0s   node 8: 97 over floor    node 4: -2
t=2.8s   node 8:  1               node 4: 86
```
