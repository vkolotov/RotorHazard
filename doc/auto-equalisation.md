# Automatic equalisation

Equalising a fleet needs the calibration quad on every node's channel at two
signal levels. By hand that is a channel change and a power change per channel,
and the operator spends the run at the radio rather than at the timer.

This is the work to command the quad's video transmitter from RotorHazard
instead, so the operator only has to place the quad twice. **It is not
finished.** What follows is what was measured, what works, and what does not,
so the next attempt does not rediscover it.

Status at the time of writing: the control path works and is proven on
hardware; switch detection works for a channel the quad moves to and fails for
one it is already on. See [Where it stands](#where-it-stands).

## The control path

RotorHazard can set a pilot's VTX channel with no firmware change anywhere.

```
RH plugin -> USB/MSP -> timer backpack -> ESP-NOW (pilot UID)
  -> pilot's TX backpack -> serial -> handset's ELRS module
  -> OTA -> receiver -> VTX
```

Each hop, with the code that makes it work:

| hop | mechanism |
| --- | --- |
| RH to timer backpack | `MSP_SET_VTX_CONFIG` (89), payload `[index, 0, power, pitmode]` |
| timer backpack out | `Timer_main.cpp` forwards any MSP function it does not recognise verbatim over ESP-NOW |
| addressing | `MSP_ELRS_SET_SEND_UID` sets the ESP-NOW peer to md5 of the pilot's bind phrase |
| TX backpack to handset | `Tx_main.cpp` `ProcessMSPPacketFromPeer` forwards `MSP_SET_VTX_CONFIG` to serial |
| handset to quad | `tx_main.cpp` `ProcessMSPPacket` stores band and channel, then `VtxTriggerSend()` |

The channel index is `(band - 1) * 8 + (channel - 1)` with bands ordered
`A B E F R L`, from the ELRS VTX administrator's own list
(`Disabled;A;B;E;F;R;L`). R1 is 32, L1 is 40.

**Power cannot be set this way.** `tx_main.cpp` reads only `payload[0]` from a
backpack packet and takes its power from the handset's own configuration, so
the power and pit mode bytes go on the wire and are ignored. Setting power
remotely needs a handset firmware patch - about five lines - which was ruled
out. This is why the two signal levels come from moving the quad rather than
from changing its power.

### Timing

A commanded channel lands **about three seconds** after the command. The
handset waits a second before its first send, repeats twice more at half-second
intervals, and the receiver then has to pass the change to the VTX.

## What the handset does behind the change

Every channel command makes the handset write the receiver's configuration to
flash, and that has consequences worth knowing before running sweeps often.

`VtxTriggerSend()` is called unconditionally on any `MSP_SET_VTX_CONFIG` from a
backpack, including a repeat of the channel already set. It ends in
`eepromWriteToMSPOut()`, which sends `MSP_EEPROM_WRITE` to the flight
controller. On Betaflight that writes flash and commonly restarts the board.

- A repeat of the same channel does **not** write the handset's own config -
  `SetVtxChannel` only stores a value that differs - but it **does** send the
  EEPROM write. The two stores behave differently; do not assume one from the
  other.
- ELRS already suppresses this for pit mode switching ("No forced EEPROM saving
  of Pit Mode"), but not for channel changes from a backpack.
- A 16-step sweep is 16 flash writes on the quad, each run.

When the flight controller restarts the link drops, and the handset then:

- calls `clearOTAQueue()` and refuses to send VTX configuration for **ten
  seconds** (`VTX_DISCONNECT_DEBOUNCE_MS`);
- if the link returns *inside* that debounce, moves to `VTXSS_CONFIRMED`, which
  means it believes the quad already has the configuration and stops sending
  it.

That last path is a silent divergence: the quad stays on its old channel, the
handset reports the new one, and nothing reconciles them. It is the best
explanation found for the quad becoming unresponsive until power cycled, which
happened once after roughly eight rapid changes.

Suppressing the EEPROM write would remove the restart, the debounce and the
flash wear together. It needs the same handset firmware patch as power control.

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

## What works, and what does not

### Detecting a switch needs no noise floor

This was the most expensive wrong turn. A floor is needed to turn a reading
into a signal level, which the captures want; a switch is a *change*, and the
floor is the same in the before and after readings, so it cancels.

Passing a floor into detection made it depend on a separate measurement that
could be stale or wrong. A floor captured while the quad was transmitting put
the signal into the occupied channel's baseline: that channel then read as
quiet, a bleeding neighbour with an honest baseline read as loud, and the sweep
retried a channel it had already switched to until it gave up - reporting
`on air: R7, not R2` while the quad sat correctly on R2.

Read raw levels before and after, take the largest rise. Bleed rises too, but
always less than the channel the transmitter actually moved to.

### Rules that were tried and failed

Each fixed the previous one's failure and introduced its own. Recorded so they
are not tried again.

| rule | fails on |
| --- | --- |
| Highest node wins | Idle high-floor node outranks a lit low-floor one |
| Highest excess over floor, clear of the next | Bleed puts a neighbour within 26 counts |
| Target's rise over a fixed threshold | Insensitive node rises 30 where the bar is 46 |
| Target rises *and* source falls by the same threshold | Source has not finished emptying; rejected a rise of 78 |
| Per-channel HIGH/LOW state against the loudest | Needs a floor, so inherits every floor problem |
| Largest rise, no floor | Cannot see a channel the quad is **already on** |

The last of these is where the work stopped. A quad already on the commanded
channel produces no rise, so there is nothing to detect. The sweep visits
channels in order, so this happens whenever the quad starts on the first
channel of the scope. It needs a separate case: if the target is already the
loudest and nothing else rose, it is already there.

### Do not clear the node extremes to take a reading

An early version called `eq_reset_extremums()` on every poll of the
confirmation loop - several times a second, each a write to all eight nodes. It
destroyed the peak and nadir tracking the rest of the system displays, loaded
the node bus, and read badly: a peak cleared half a second ago holds only what
arrived since, which during a change is as likely to be the channel being left
as the one being joined. Sample `current_rssi` instead.

The extremes **do** have to be cleared once per capture, after the channel is
confirmed. A peak only ever rises, so without it every capture carries the
highest reading from every channel before it. One run recorded the same 191 on
node 1 for all three channels; that was what it saw while the quad was on the
first of them.

## Where it stands

Working and proven on hardware:

- commanding a channel, and the whole path to the VTX
- a 3-channel sweep capturing all three, each capture its own channel
- clean noise floor capture with the quad powered off
- cancel, scope selection, and the two-phase progress display

Not working:

- a channel the quad is **already on** is never confirmed
- the `low` pass with the quad moved away is untested
- Apply is untested against a sweep's captures

Deliberately left as it is:

- power stays manual; remote power needs a handset firmware patch
- the sweep waits 12 seconds between channels, past the handset's debounce.
  Two attempts to measure that window from the timer failed to reproduce it,
  but both only asked whether the change eventually arrived, which a dropped
  and re-sent command also passes. The firmware has the debounce in plain
  sight, so the wait stays.

## Constants and where they came from

| constant | value | basis |
| --- | --- | --- |
| `VTX_SWITCH_RISE_FRACTION` | 0.08 | 30-count rise on the weakest node against a couple of counts of jitter |
| `VTX_CONFIRM_TIMEOUT_SECONDS` | 30 | outlasts the handset's 10 s debounce with room for a resend |
| `VTX_RESEND_SECONDS` | 12 | past the debounce; resending sooner is discarded and costs a flash write |
| `EQ_CHANNEL_SETTLE_SECONDS` | 12 | same debounce, between channels |
| `EQ_PEAK_SETTLE_SECONDS` | 3 | a few passes of the node's filtering after clearing the peaks |
| `EQ_SCOPE_BANDS` | R, L | each extra band is a full pass of captures |

`EQ_SCOPE_LIMIT` exists only to shorten a run while testing. It must be `None`
in anything shipped.

## Related

- `node-equalisation.md` - what the captures are for and how the fit works
- `rssi-channel-survey.md` - why constants are channel-specific
