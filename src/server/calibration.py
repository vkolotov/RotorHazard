'''Seat calibration adjustment'''

import logging
import gevent
import json
import RHUtils
from eventmanager import Evt
from RHUtils import catchLogExceptionsWrapper
from filtermanager import Flt

logger = logging.getLogger(__name__)

# The gate is where a pass is decided, so it is where the correction has to be
#  a pure translation: above the pivot the offset applies alone and the gain is
#  exactly one, which keeps peak amplitude, local slope and the timing of the
#  maximum as the receiver reported them. A fluctuation of n raw counts comes
#  out as n counts on every node. The shipped fit reached cross-node agreement
#  instead by stretching each node's whole curve, which amplified gate noise by
#  1.0 to 2.85x depending on the node - different distortion per seat, in the
#  one region lap detection reads.
#
# Where the gain still earns its place is further down, where matching receiver
#  sensitivity matters more than preserving shape. Below the pivot the lower
#  segment bends each node's reading onto a common noise floor.
#
# Two captures per node - floor and gate - and everything else follows.

# Where the pivot sits on each node's own floor-to-gate range. The upper 40% is
#  left at unity gain; the lower 60% is scaled. Measured over 8 nodes x 8 R-band
#  channels, 0.60 roughly halves the cross-node error against offset-only at
#  30-50% of range and costs nothing above it. Swept over 0.0, 0.3, 0.6, 0.8 and
#  1.0: gate agreement is exact at every value, so the ratio only trades
#  convergence lower down.
#
# Not 0. A ratio of 0 makes the whole curve offset-only, which measures better
#  on the survey's `pit` level - but `pit` is a transmit-power setting that
#  lands anywhere from 28% to 80% of the floor-to-gate span, so spread measured
#  there reports where each node's PIT happens to fall, not fit quality.
NORM_PIVOT_RATIO = 0.60

# Where the levelled floors land: the reference node's own floor, carried
#  through its own offset. The reference node is the one whose gate is highest,
#  so it defines the gate target and takes offset 0 - and pinning the floors to
#  it as well means it takes scale x1.00 too. Its curve is then a pure
#  translation end to end, and every other node is bent onto it.
#
# The alternative - a small fixed target, near the bottom of the scale - was
#  tried and rejected. The floor-to-pivot span is whatever the receiver reports,
#  typically 40 to 60 counts, so dragging it down to 3 forces a gain of x2.5 to
#  x5.2 across the fleet. That amplifies each node's own noise below the pivot,
#  and it drives any signal below a node's floor to the clamp: at a low
#  transmit power some nodes read a number while others read 0. Pinning to the
#  reference node's floor keeps the worst gain under x2 and leaves headroom
#  underneath, so a quiet signal still reads as a value.

# What a node's reading can reach. The node pipeline is a byte wide, so this is
#  a byte; a wider pipeline would raise it, and the fit follows the captures
#  rather than this number, so nothing else has to change.
NORM_FULL_SCALE = 255

# Minimum gap between the floor and the gate, as a fraction of full scale. A
#  node that never saw the quad reads only noise and would otherwise get an
#  absurd scale. A fraction rather than a count, so it follows the width of the
#  pipeline.
#
# The lower segment stretches floor-to-pivot onto the shared destination, so a
#  narrow range buys a large gain that amplifies the node's own noise. A
#  measured fleet spanned 34 to 49 counts with the quad properly placed, so 30
#  sits below every real measurement while still catching a pass flown too
#  close to the gate.
NORM_MIN_LEVEL_FRACTION = 30.0 / 255

# The Q8 scale that changes nothing: the node multiplies by scale >> 8, so 256
#  is exactly one. Both the default for a node that has never been fitted and
#  the value an operator types to undo a correction by hand.
NORM_UNITY_SLOPE = 256

# How far below EnterAt the suggested ExitAt sits, as a fraction of EnterAt.
#  Measured static noise at the gate is under two counts peak to peak, so a
#  tenth is ample hysteresis without being so wide that a fast pass fails to
#  release before the next lap.
NORM_HYSTERESIS_FRACTION = 0.10

# What an operator may type into a scale field. A gain far outside this is a bad
#  capture rather than a real receiver difference, and amplifies the node's own
#  noise with the signal.
NORM_SLOPE_MIN = 32       # x0.125
NORM_SLOPE_MAX = 2048     # x8.00

# The bands a sweep can cover, and the frequency of every channel in them. Held
#  here rather than read from the page's own table, because the sweep commands
#  the channels itself and cannot depend on a browser being open.
NORM_BANDS = {
    'R': ((1, 5658), (2, 5695), (3, 5732), (4, 5769),
          (5, 5806), (6, 5843), (7, 5880), (8, 5917)),
    'L': ((1, 5362), (2, 5399), (3, 5436), (4, 5473),
          (5, 5510), (6, 5547), (7, 5584), (8, 5621)),
    }

# What a sweep may cover. 'current' keeps whatever the nodes are tuned to and
#  calibrates only those channels; the others sweep a whole band, or both.
#
# A fit is per [node, frequency]: a receiver's sensitivity differs channel to
#  channel, so one fit reused across a band left a measurable error - calibrate
#  on one channel and move the fleet and cross-node agreement degrades to 6-11%
#  within R band alone. R and L are 555 MHz apart, so across both it is worse.
NORM_SCOPES = ('current', 'R', 'RL')

# How long to wait after retuning before the extremes are even cleared. The
#  RX5808's VCO and the filter behind it need time to settle on a new channel,
#  and a reading taken during that is neither the old channel nor the new one.
#  Separate from the settle below, which is about watching for a peak once the
#  receiver is already stable.
NORM_RETUNE_SECONDS = 2.0

# How long to watch a node after clearing its extremes, before reading them.
#  The clear has to happen after the operator has set the condition up, not
#  before: a peak only ever rises, so extremes cleared at the end of the
#  previous step would already hold whatever the VTX did while its channel was
#  being changed.
NORM_SETTLE_SECONDS = 5.0

class Calibration:
    def __init__(self, racecontext):
        self._racecontext = racecontext

    @catchLogExceptionsWrapper
    def set_enter_at_level(self, seat_index, enter_at_level_input, emit_levels=True):
        '''Set node enter-at level.'''
        enter_at_level = int(enter_at_level_input or 0)

        if seat_index < 0 or seat_index >= self._racecontext.race.num_nodes:
            logger.info('Unable to set enter-at ({0}) on node {1}; node index out of range'.format(enter_at_level, seat_index+1))
            return

        if not enter_at_level:
            logger.info('Node enter-at set null; getting from node: Node {0}'.format(seat_index+1))
            enter_at_level = self._racecontext.interface.nodes[seat_index].enter_at_level

        profile = self._racecontext.race.profile
        enter_ats = json.loads(profile.enter_ats)

        # handle case where more nodes were added
        while seat_index >= len(enter_ats["v"]):
            enter_ats["v"].append(None)

        enter_ats["v"][seat_index] = enter_at_level
        # Re-stamp the axis: the value just written was measured against the
        #  correction in force now, whatever the rest of the record still says.
        enter_ats.update(self.threshold_scale_id())

        profile = self._racecontext.rhdata.alter_profile({
            'profile_id': profile.id,
            'enter_ats': enter_ats
            })
        self._racecontext.race.profile = profile

        self._racecontext.interface.set_enter_at_level(seat_index, enter_at_level)

        self._racecontext.events.trigger(Evt.ENTER_AT_LEVEL_SET, {
            'nodeIndex': seat_index,
            'enter_at_level': enter_at_level,
            })

        logger.info('Node enter-at set: Node {0} Level {1}'.format(seat_index+1, enter_at_level))
        if emit_levels:
            self._racecontext.rhui.emit_enter_and_exit_at_levels()

    @catchLogExceptionsWrapper
    def set_exit_at_level(self, seat_index, exit_at_level_input, emit_levels=True):
        '''Set node exit-at level.'''
        exit_at_level = int(exit_at_level_input or 0)

        if seat_index < 0 or seat_index >= self._racecontext.race.num_nodes:
            logger.info('Unable to set exit-at ({0}) on node {1}; node index out of range'.format(exit_at_level, seat_index+1))
            return

        if not exit_at_level:
            logger.info('Node exit-at set null; getting from node: Node {0}'.format(seat_index+1))
            exit_at_level = self._racecontext.interface.nodes[seat_index].exit_at_level

        profile = self._racecontext.race.profile
        exit_ats = json.loads(profile.exit_ats)

        # handle case where more nodes were added
        while seat_index >= len(exit_ats["v"]):
            exit_ats["v"].append(None)

        exit_ats["v"][seat_index] = exit_at_level
        exit_ats.update(self.threshold_scale_id())

        profile = self._racecontext.rhdata.alter_profile({
            'profile_id': profile.id,
            'exit_ats': exit_ats
            })
        self._racecontext.race.profile = profile

        self._racecontext.interface.set_exit_at_level(seat_index, exit_at_level)

        self._racecontext.events.trigger(Evt.EXIT_AT_LEVEL_SET, {
            'nodeIndex': seat_index,
            'exit_at_level': exit_at_level,
            })

        logger.info('Node exit-at set: Node {0} Level {1}'.format(seat_index+1, exit_at_level))
        if emit_levels:
            self._racecontext.rhui.emit_enter_and_exit_at_levels()

    # --- normalisation -----------------------------------------------------

    def _norm_stored(self, field, default):
        """A stored per-node calibration list, padded to the node count."""
        profile = self._racecontext.race.profile
        raw = getattr(profile, field, None)
        try:
            vals = json.loads(raw)["v"] if raw else []
        except (TypeError, ValueError, KeyError):
            vals = []  # never calibrated, or written by older code
        out = []
        for idx in range(self._racecontext.race.num_nodes):
            v = vals[idx] if idx < len(vals) else None
            out.append(default if v is None else v)
        return out

    def _norm_participants(self):
        """Which nodes the wizard calibrates.

        A node with no frequency is not receiving anything, and a node whose
        firmware predates the protocol will ignore the coefficients. Neither
        can contribute a capture, so neither should be able to hold the wizard
        open or have a fit computed for it.

        Taken from the run's own assignment while a run is under way: the sweep
        parks every node on one channel, so the live profile would say they are
        all on the same one and a node disabled by the operator would look active.
        """
        saved = getattr(self, '_norm_saved_freqs', None)
        if saved:
            f = saved.get('f') or []
        else:
            freqs = json.loads(self._racecontext.race.profile.frequencies)
            f = freqs.get('f') or []
        nodes = self._racecontext.interface.nodes
        out = []
        for idx in range(self._racecontext.race.num_nodes):
            if idx < len(f) and not f[idx]:
                continue
            node = nodes[idx] if idx < len(nodes) else None
            if node is not None and getattr(node, 'api_level', 0) < 37:
                continue
            out.append(idx)
        return out

    def _norm_node_channels(self):
        """The channel label each node is tuned to, one per node.

        Nodes that are not participating get None, so they raise no step of
        their own and are skipped by the fit.

        This is a node's own channel - the one it races on - so during a run it
        comes from the saved assignment rather than from wherever the sweep has
        currently parked the fleet.
        """
        saved = getattr(self, '_norm_saved_freqs', None)
        if saved:
            bands, chans = saved.get('b') or [], saved.get('c') or []
        else:
            freqs = json.loads(self._racecontext.race.profile.frequencies)
            bands, chans = freqs.get('b') or [], freqs.get('c') or []
        taking_part = set(self._norm_participants())
        out = []
        for idx in range(self._racecontext.race.num_nodes):
            if idx not in taking_part:
                out.append(None)
                continue
            band = bands[idx] if idx < len(bands) else None
            chan = chans[idx] if idx < len(chans) else None
            out.append('{0}{1}'.format(band, chan) if band and chan
                       else 'Node {0}'.format(idx + 1))
        return out

    def _norm_scope(self):
        """The scope the current run was started with, defaulting to current."""
        scope = getattr(self, '_norm_scope_sel', None)
        return scope if scope in NORM_SCOPES else 'current'

    def _norm_sweep_channels(self):
        """The channels this run calibrates, in capture order.

        Frozen at run start. The sweep retunes the profile as it goes, so
        re-deriving the list from it mid-run would collapse the 'current' scope
        to whatever channel was last captured.

        Every node is tuned to the same channel for each capture, so a step
        yields a reading for every node on that one frequency. That is what makes
        the fit per [node, frequency]: each node ends up measured on every
        channel rather than only on its own.

        'current' calibrates the distinct channels the nodes are already tuned
        to; the band scopes calibrate every channel in the band, whatever the
        nodes happen to be set to.

        :return: ((band, channel, frequency), ...)
        """
        frozen = getattr(self, '_norm_channels', None)
        if frozen:
            return frozen
        scope = self._norm_scope()
        if scope == 'current':
            out, seen = [], set()
            profile_freqs = self._norm_profile_freqs()
            for idx in self._norm_participants():
                band = profile_freqs['b'][idx]
                chan = profile_freqs['c'][idx]
                freq = profile_freqs['f'][idx]
                if not band or not chan or not freq:
                    continue
                if (band, chan) in seen:
                    continue
                seen.add((band, chan))
                out.append((band, chan, freq))
            return tuple(out)
        bands = ('R',) if scope == 'R' else ('R', 'L')
        return tuple((b, c, f) for b in bands for c, f in NORM_BANDS[b])

    def _norm_profile_freqs(self):
        """The profile's band/channel/frequency lists, padded to the node count."""
        num = self._racecontext.race.num_nodes
        try:
            freqs = json.loads(self._racecontext.race.profile.frequencies)
        except (TypeError, ValueError):
            freqs = {}
        out = {}
        for key, default in (('b', None), ('c', None),
                             ('f', RHUtils.FREQUENCY_ID_NONE)):
            vals = freqs.get(key) or []
            out[key] = [vals[i] if i < len(vals) else default for i in range(num)]
        return out

    def _norm_steps(self):
        """Two passes over every channel in scope: noise, then the gate.

        A fit pairs a floor with a gate on the same channel, so the floor is
        swept exactly as the gate is - a node's own noise floor moves by up to 11
        counts across R band, which is the same order as the gate variation that
        makes a per-channel fit necessary at all.

        Level is the outer loop so the quad is powered off once for the whole
        noise pass and on once for the whole gate pass, rather than switched per
        channel. The mid-power level the old three-point fit needed is gone: the
        pivot is derived from the floor and the gate, and that level was the
        least reproducible of the three anyway, depending on the operator judging
        a distance rather than using a mark.

        R band is 16 steps, R and L together 32.
        """
        channels = ['{0}{1}'.format(band, chan)
                    for band, chan, _ in self._norm_sweep_channels()]
        steps = []
        for level in ('noise', 'high'):
            steps.extend((level, label) for label in channels)
        return steps

    def _norm_scale(self, node_index):
        """What a node's corrected reading can reach.

        Based on what the pipeline can actually carry, not on max_rssi_value -
        that is the "no nadir recorded" sentinel and sits above the real range.
        """
        return NORM_FULL_SCALE

    def _norm_targets(self, levels):
        """Where the fleet's gates and floors should land.

        Both come from the reference node - the one reporting the highest gate.
        It keeps its own readings, taking offset 0 and scale x1.00, and every
        other node is translated and bent onto it. Offsetting the fleet down to
        the weakest node instead would push its whole curve towards zero for no
        gain in resolution.

        :param levels: (floor_raw, gate_raw) per node, for participants only
        :return: (target_gate, target_floor)
        """
        floor_raw, gate_raw = max(levels, key=lambda fg: fg[1])
        # The reference node's own floor is already on target, since its offset
        #  is zero by construction. Clamped above zero because rssi 0 is the
        #  node's "no peak recorded" sentinel.
        return gate_raw, max(1, floor_raw)

    def _norm_fit(self, floor_raw, gate_raw, target_gate, target_floor):
        """One node's coefficients from its two captures.

        `target_gate` and `target_floor` are shared across the fleet, so every
        node's gate comes out on one value and every node's floor on another.
        Above the pivot the offset alone applies, so that is a translation and
        nothing more. Below it the scale carries floor-to-pivot onto
        target_floor-to-pivot_target.

        Returns (pivot, offset, scale) ready for the node: it computes
        `raw - offset` above the pivot and pivots the lower segment around the
        same point, so the two meet there by construction rather than by the
        server and the node agreeing on a third number.
        """
        offset = gate_raw - target_gate
        pivot = int(round(floor_raw + (gate_raw - floor_raw) * NORM_PIVOT_RATIO))
        # The pivot has to sit strictly above the floor or the lower segment has
        #  no span to scale across. A capture pair this tight is rejected before
        #  it reaches here, so this only guards the arithmetic.
        pivot = max(pivot, floor_raw + 1)
        pivot_target = pivot - offset
        span = pivot - floor_raw
        scale = int(round((pivot_target - target_floor) * 256.0 / span))
        scale = max(1, min(65535, scale))
        return pivot, offset, scale

    def norm_suggested_thresholds(self):
        """EnterAt/ExitAt suggested from the fit, or None when there is none.

        One pair for the whole fleet. That is what normalisation buys: after it,
        the same number means the same signal on every seat, so per-node
        thresholds stop being necessary.

        EnterAt is the **highest** corrected pivot in the fleet, not the mean.
        Above its own pivot a node is at unity gain and agrees exactly with the
        others; below it the nodes diverge. The mean would sit under the pivot of
        every node above average - on a measured fleet that was three of eight -
        putting their trigger in the scaled region where the agreement the fit
        exists for does not hold.

        ExitAt is a tenth below EnterAt. Measured static noise at the gate is
        under 2 counts peak to peak, so that is ample hysteresis, and since the
        floors now land on the reference node's own floor rather than near zero,
        a tenth of EnterAt stays well above them - a pass whose exit threshold
        sat at or below the floor would start and never end.

        :return: (enter_at, exit_at) or None
        """
        pivots = self._norm_stored('norm_pivots', 0)
        offsets = self._norm_stored('norm_offsets', 0)
        scales = self._norm_stored('norm_scales', NORM_UNITY_SLOPE)
        corrected = [self._corrected(pivots[i], (pivots[i], offsets[i], scales[i]))
                     for i in self._norm_participants() if pivots[i]]
        if not corrected:
            return None

        enter_at = max(corrected)
        exit_at = max(1, enter_at - int(round(NORM_HYSTERESIS_FRACTION * enter_at)))
        return enter_at, exit_at

    @catchLogExceptionsWrapper
    def norm_apply_thresholds(self, enter_at, exit_at):
        """Write one EnterAt/ExitAt pair to every participating node.

        The operator may have edited what was suggested, so the values arrive as
        arguments rather than being recomputed here.

        :param enter_at: EnterAt on the corrected axis
        :param exit_at: ExitAt on the corrected axis
        :return: True when every node took both
        """
        try:
            enter_at = int(enter_at)
            exit_at = int(exit_at)
        except (TypeError, ValueError):
            return False  # came from the page, so treat junk as a no-op
        if not 0 < exit_at < enter_at <= NORM_FULL_SCALE:
            self._racecontext.rhui.emit_priority_message(
                'EnterAt must be above ExitAt, and both within 1 to {0}'.format(
                    NORM_FULL_SCALE))
            return False
        if getattr(self, '_norm_busy', False):
            return False

        taking_part = self._norm_participants()
        if not taking_part:
            return False
        for idx in taking_part:
            # Emit once at the end rather than per node and per level, so the
            #  page is not redrawn eight times mid-write.
            self.set_enter_at_level(idx, enter_at, emit_levels=False)
            self.set_exit_at_level(idx, exit_at, emit_levels=False)
        self._racecontext.rhui.emit_enter_and_exit_at_levels()
        logger.info('Thresholds applied to %d nodes: EnterAt=%d ExitAt=%d',
                    len(taking_part), enter_at, exit_at)
        self._racecontext.rhui.emit_priority_message(
            'EnterAt {0} and ExitAt {1} applied to {2} nodes'.format(
                enter_at, exit_at, len(taking_part)))
        return True

    def norm_start(self, scope):
        """Begin a run at the given scope, remembering the channel assignment.

        The sweep retunes every node, so what they were set to has to be kept:
        it is the operator's race configuration and has to come back whether the
        run finishes, is reset, or is abandoned.

        :param scope: One of NORM_SCOPES
        :return: True when the run was armed
        """
        if scope not in NORM_SCOPES:
            return False
        if getattr(self, '_norm_busy', False):
            return False
        if not self._norm_participants():
            self._racecontext.rhui.emit_priority_message(
                'No node is available to calibrate')
            return False

        # Keep the assignment before anything is retuned. Kept even for the
        #  'current' scope: that scope still parks every node on one channel at a
        #  time, so the per-node assignment is just as disturbed.
        self._norm_saved_freqs = self._norm_profile_freqs()
        self._norm_scope_sel = scope
        self._norm_captured = {}
        self._norm_applied_captures = False
        self._norm_levelled_only = False
        self._norm_invalidate_session()
        self._norm_channels = None
        channels = self._norm_sweep_channels()
        self._norm_channels = channels
        if not channels:
            self._racecontext.rhui.emit_priority_message(
                'No channel to calibrate: assign frequencies first')
            self._norm_scope_sel = None
            return False
        # Park the whole fleet on the first channel straight away, so what the
        #  operator sees on the nodes matches what the wizard is about to
        #  measure. Waiting until the first gate capture would leave them on
        #  their race channels through the noise step, and the noise floor has
        #  to be read on the same channel as the gate it is paired with.
        # No settle wait here: this only parks the fleet so the page and the
        #  hardware agree before the operator sets the quad up. Every capture
        #  retunes and waits on its own, so blocking the click would buy nothing
        #  and hold the socket handler for seconds.
        self._norm_tune_all(*channels[0])
        logger.info('Normalisation run started, scope=%s, %d channels',
                    scope, len(channels))
        self._racecontext.rhui.emit_norm_wizard_state()
        return True

    def _norm_tune_all(self, band, chan, freq):
        """Put every participating node on one channel, page included.

        Each capture reads every node on the same frequency, which is what makes
        the fit per [node, frequency] rather than per node.

        The profile is written as well as the hardware. The page reads the band
        and channel from the profile, so leaving it alone shows the operator the
        race assignment while the nodes are somewhere else entirely - and the
        restore path looks a node's fit up by its profile frequency, so a stale
        profile would hand every node the wrong channel's fit.
        """
        taking_part = self._norm_participants()
        profile = self._racecontext.race.profile
        try:
            freqs = json.loads(profile.frequencies)
        except (TypeError, ValueError):
            freqs = {}
        for key in ('b', 'c', 'f'):
            freqs.setdefault(key, [])
            while len(freqs[key]) < self._racecontext.race.num_nodes:
                freqs[key].append(None)
        for idx in taking_part:
            self._racecontext.interface.set_frequency(idx, freq, band, chan)
            freqs['b'][idx], freqs['c'][idx], freqs['f'][idx] = band, chan, freq
        self._racecontext.race.profile = self._racecontext.rhdata.alter_profile({
            'profile_id': profile.id,
            'frequencies': freqs,
            })
        self._racecontext.rhui.emit_frequency_data()
        logger.info('Normalisation tuned every node to %s%s (%d MHz)',
                    band, chan, freq)

    def norm_restore_frequencies(self):
        """Put the nodes back on the assignment the run started with.

        Called when a run ends, however it ends. Without it the fleet is left on
        whatever channel the last capture used, which is not a race
        configuration.
        """
        saved = getattr(self, '_norm_saved_freqs', None)
        if not saved:
            return False
        profile = self._racecontext.race.profile
        self._racecontext.race.profile = self._racecontext.rhdata.alter_profile({
            'profile_id': profile.id,
            'frequencies': {'b': saved['b'], 'c': saved['c'], 'f': saved['f']},
            })
        for idx in range(self._racecontext.race.num_nodes):
            freq = saved['f'][idx]
            if freq:
                self._racecontext.interface.set_frequency(
                    idx, freq, saved['b'][idx], saved['c'][idx])
        self._racecontext.rhui.emit_frequency_data()
        logger.info('Normalisation restored the channel assignment')
        self._norm_saved_freqs = None
        return True

    def norm_wizard_state(self):
        """Where the wizard is: the next step, or done.

        A run that has not chosen a scope reports 'choosing', which is what puts
        the scope buttons in front of the operator instead of a capture step.
        """
        captured = getattr(self, '_norm_captured', None) or {}
        busy = getattr(self, '_norm_busy', False)
        steps = self._norm_steps()

        if captured and not self._norm_captures_are_current():
            # measured against a configuration that is no longer loaded
            logger.info('Discarding normalisation captures: configuration changed')
            captured = {}
            self._norm_captured = {}
            self._norm_applied_captures = False

        vtx = self.norm_vtx_available()

        # Nothing is armed until a scope is chosen: the sweep retunes the whole
        #  fleet, so which channels it will visit has to be settled first.
        if not getattr(self, '_norm_scope_sel', None) and not captured \
                and not any(self._norm_stored('norm_pivots', 0)):
            return {'state': 'choosing', 'level': None, 'channel': None,
                    'index': 0, 'total': 0, 'busy': busy,
                    'settle': NORM_SETTLE_SECONDS, 'vtx': vtx,
                    'noise_ready': False, 'scope': None, 'sweeping': False}

        # Floor levelling stores a fit too, but it is a starting point with the
        #  sweep still ahead of it, so it must not park the wizard the way a
        #  finished calibration does.
        applied = any(self._norm_stored('norm_pivots', 0)) \
            and not getattr(self, '_norm_levelled_only', False)
        if applied and (not captured or getattr(self, '_norm_applied_captures', False)):
            # already calibrated - do not arm the first step, so a stray click
            #  cannot start overwriting a good calibration. The captures behind
            #  the fit are kept so a level can still be corrected and re-applied.
            return {'state': 'applied', 'level': None, 'channel': None,
                    'index': 0, 'total': len(steps), 'busy': busy,
                    'settle': NORM_SETTLE_SECONDS, 'vtx': vtx,
                    'noise_ready': False, 'scope': self._norm_scope(),
                    'sweeping': False}

        # Noise alone is enough to level the floors: with no quad there is no
        #  second point and so no slope, but the offsets can still line every
        #  node's floor up on one value. Offer that as soon as noise is in,
        #  since it needs nothing else and the rest of the sweep is long.
        # Floor levelling works off the noise pass, so offer it once that pass is
        #  complete rather than after a single channel.
        noise_ready = all('noise:{0}'.format(label) in captured
                          for _, label in self._norm_steps()
                          if label is not None) if captured else False

        for level, chan in steps:
            key = level if chan is None else '{0}:{1}'.format(level, chan)
            if key not in captured:
                return {'state': 'capturing', 'level': level, 'channel': chan,
                        'index': len(captured), 'total': len(steps),
                        'busy': busy, 'settle': NORM_SETTLE_SECONDS, 'vtx': vtx,
                        'noise_ready': noise_ready, 'scope': self._norm_scope(),
                        'sweeping': bool(getattr(self, '_norm_pass_running', False))}
        return {'state': 'ready', 'level': None, 'channel': None,
                'index': len(steps), 'total': len(steps), 'busy': busy,
                'settle': NORM_SETTLE_SECONDS, 'vtx': vtx,
                'noise_ready': noise_ready, 'scope': self._norm_scope(),
                'sweeping': bool(getattr(self, '_norm_pass_running', False))}

    def norm_captured_table(self):
        """Per-node view for the UI.

        Carries the captured levels for the readout, and always the offset and
        the scale, which are what the operator edits. The offset is in counts
        and decides where the gate lands; the scale is the Q8 gain below the
        pivot, so 256 means x1.00 and changes nothing. A node with no fit reads
        0 and 256 rather than blank, since "no correction" is a real, editable
        state rather than missing data.

        The pivot travels with them for the readout: it is derived, not edited,
        but it is where the offset stops applying alone and the scale takes
        over, so the two numbers mean little without it.
        """
        captured = getattr(self, '_norm_captured', None) or {}
        num = self._racecontext.race.num_nodes
        labels = self._norm_node_channels()
        pivots = self._norm_stored('norm_pivots', 0)
        offsets = self._norm_stored('norm_offsets', 0)
        scales = self._norm_stored('norm_scales', NORM_UNITY_SLOPE)

        if captured:
            mode = 'applied-capture' \
                if getattr(self, '_norm_applied_captures', False) else 'capture'
        else:
            mode = None
        return [{
            'channel': labels[i],
            'mode': mode or ('applied' if pivots[i] else 'empty'),
            'pivot': pivots[i],
            'offset': offsets[i],
            'scale': scales[i],
            } for i in range(num)]

    @catchLogExceptionsWrapper
    def _norm_session(self):
        """Identifies the configuration a capture belongs to.

        Actual frequencies rather than channel labels, so a retune that keeps
        the label - or one the label cannot express - still counts as a
        different configuration.

        While a run is under way that means the assignment the run started from,
        not the live one: the sweep retunes the whole fleet for every step, so
        fingerprinting what the nodes are tuned to right now would make every
        capture invalidate itself. What must still be caught is the operator
        changing the assignment underneath the run, and that changes the saved
        copy's counterpart, not the sweep's own parking.
        """
        saved = getattr(self, '_norm_saved_freqs', None)
        if saved:
            tuning = tuple(saved.get('f') or [])
        else:
            try:
                freqs = json.loads(self._racecontext.race.profile.frequencies)
                tuning = tuple(freqs.get('f') or [])
            except (TypeError, ValueError, AttributeError):
                tuning = ()
        return (getattr(self, '_norm_epoch', 0),
                getattr(self._racecontext.race.profile, 'id', None),
                tuning)

    def _norm_captures_are_current(self):
        """True when the captures on hand belong to the configuration in use."""
        captured = getattr(self, '_norm_captured', None)
        if not captured:
            return True
        return getattr(self, '_norm_captured_session', None) == self._norm_session()

    def _norm_note_capture_session(self):
        """Record which configuration the current capture set belongs to."""
        self._norm_captured_session = self._norm_session()

    def _norm_invalidate_session(self):
        """Drop any capture still settling."""
        self._norm_epoch = getattr(self, '_norm_epoch', 0) + 1

    def norm_wizard_capture(self):
        """Capture the next step: clear the extremes, settle, then read."""
        state = self.norm_wizard_state()
        if state['state'] != 'capturing' or getattr(self, '_norm_busy', False):
            return False

        self._norm_busy = True
        # Anything that changes what a capture would mean - a reset, a step
        #  back, a profile change - bumps this. The sleep below is long enough
        #  for that to happen underneath us, and a reading taken before the
        #  change must not be filed against the state after it.
        session = self._norm_session()
        try:
            self._racecontext.rhui.emit_norm_wizard_state()
            # Every node reads the same frequency for this step, so retune before
            #  clearing the extremes: a peak only rises, and extremes cleared
            #  before the change would hold whatever the old channel was giving.
            if state['channel'] is not None:
                for band, chan, freq in self._norm_sweep_channels():
                    if '{0}{1}'.format(band, chan) == state['channel']:
                        self._norm_tune_all(band, chan, freq)
                        # Let the receivers settle on the new channel before the
                        #  extremes are cleared, or the peak recorded is partly
                        #  the old channel's.
                        gevent.sleep(NORM_RETUNE_SECONDS)
                        break
            self.norm_reset_extremums()
            gevent.sleep(NORM_SETTLE_SECONDS)
            if self._norm_session() != session:
                logger.info('Normalisation capture discarded: state changed while settling')
                return False

            level = state['level']
            # Both levels are swept per channel now, so every key carries one.
            key = level if state['channel'] is None \
                else '{0}:{1}'.format(level, state['channel'])
            nodes = self._racecontext.interface.nodes
            vals = []
            for idx in range(self._racecontext.race.num_nodes):
                node = nodes[idx]
                v = node.node_nadir_rssi if level == 'noise' else node.node_peak_rssi
                vals.append(int(v) if v and v < node.max_rssi_value else None)

            taking_part = self._norm_participants()
            missing = [i + 1 for i in taking_part if vals[i] is None]
            if level == 'noise' and missing:
                msg = 'Noise capture failed: no reading on node {0}'.format(missing[0])
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False

            rejected = self._norm_reject_reason(level, state['channel'], vals)
            if rejected:
                # One placement of the quad serves the whole pass, so a gap
                #  this small condemns the placement rather than the step: the
                #  channels already captured at this level were measured from
                #  the same wrong distance. Drop them all and restart the pass
                #  from its first channel.
                discarded = self._norm_discard_level(level)
                logger.warning('Normalisation %s pass restarted (%d discarded): %s',
                               level, discarded, rejected)
                self._racecontext.rhui.emit_priority_message(rejected)
                return False

            self._norm_captured = getattr(self, '_norm_captured', {})
            # a fresh reading supersedes whatever fit was applied from the old
            #  set, so these captures are a new run rather than its record
            self._norm_applied_captures = False
            self._norm_captured[key] = vals
            self._norm_note_capture_session()
            logger.info('Normalisation captured %s: %s', key, vals)
            return True
        finally:
            self._norm_busy = False
            self._racecontext.rhui.emit_norm_wizard_state()

    @catchLogExceptionsWrapper
    def norm_cancel_pass(self):
        """Ask a running pass to stop after the channel it is on.

        The loop checks this between channels rather than being killed outright,
        so it stops with every capture it has taken either filed or discarded -
        never with a reading half-read off the nodes.
        """
        if not getattr(self, '_norm_pass_running', False):
            return False
        self._norm_pass_cancel = True
        logger.info('Normalisation pass cancellation requested')
        return True

    @catchLogExceptionsWrapper
    def norm_capture_pass(self):
        """Capture the whole noise pass across every channel, unattended.

        With the VTX off there is nothing for the operator to do between channels
        - no quad to place, no channel to command - so the sweep walks them
        itself: tune the whole fleet, settle, read, repeat.

        The gate pass is deliberately not automated. Each channel there needs the
        quad at the mark and the VTX moved onto that channel, which is the
        operator's work, and a gate captured without them having confirmed the
        quad is in place is a reading nobody checked.

        Stops at the first channel that will not capture, leaving the channels
        already taken in place, so a rejected reading names its channel instead of
        being buried in a run that carried on regardless.

        :return: True when the whole pass was captured
        """
        state = self.norm_wizard_state()
        if state['state'] != 'capturing' or getattr(self, '_norm_busy', False):
            return False
        level = state['level']
        if level != 'noise':
            return False
        remaining = [chan for lvl, chan in self._norm_steps()
                     if lvl == level
                     and '{0}:{1}'.format(lvl, chan) not in self._norm_captured]
        if not remaining:
            return False

        logger.info('Normalisation %s pass: %d channels to capture',
                    level, len(remaining))
        self._norm_pass_running = True
        self._norm_pass_cancel = False
        try:
            for _ in list(remaining):
                if getattr(self, '_norm_pass_cancel', False):
                    logger.info('Normalisation %s pass cancelled', level)
                    self._racecontext.rhui.emit_priority_message(
                        '{0} pass cancelled; captures so far are kept'.format(
                            level.capitalize()))
                    return False
                step = self.norm_wizard_state()
                if step['state'] != 'capturing' or step['level'] != level:
                    break  # the pass finished, or something moved underneath us
                if not self.norm_wizard_capture():
                    logger.warning('Normalisation %s pass stopped at %s',
                                   level, step['channel'])
                    return False
        finally:
            self._norm_pass_running = False
            self._norm_pass_cancel = False
        done = self.norm_wizard_state()
        self._racecontext.rhui.emit_priority_message(
            '{0} pass captured on {1} channels'.format(
                level.capitalize(), len(remaining))
            if done['level'] != level or done['state'] != 'capturing'
            else '{0} pass incomplete'.format(level.capitalize()))
        return True

    def norm_wizard_apply_noise(self):
        """Level every node's noise floor, using only the noise capture.

        With no quad in the air there is one measured point per node, which is
        not enough for a slope - so the scales stay at unity and the offsets do
        the work: the correction becomes plain subtraction that puts every
        floor on the same value. Nodes then sit at a common idle level without
        anyone having to fly the calibration pass.

        A later full sweep overwrites this; it is a starting point, not a
        substitute for a two-point fit.
        """
        captured = getattr(self, '_norm_captured', None) or {}
        if getattr(self, '_norm_busy', False):
            return False

        num = self._racecontext.race.num_nodes
        # A floor belongs to a channel, so take each node's floor from the
        #  channel it will be running on - the one it is about to be restored to.
        saved = getattr(self, '_norm_saved_freqs', None) or self._norm_profile_freqs()
        by_freq = {f: '{0}{1}'.format(b, c)
                   for b, c, f in self._norm_sweep_channels()}
        noise = [None] * num
        for idx in range(num):
            label = by_freq.get(saved['f'][idx])
            row = captured.get('noise:{0}'.format(label)) if label else None
            if row and idx < len(row):
                noise[idx] = row[idx]
        taking_part = [i for i in self._norm_participants()
                       if noise[i] is not None]
        if not taking_part:
            self._racecontext.rhui.emit_priority_message(
                'No node has a noise reading to level')
            return False

        # Land every floor on the quietest node's own floor, so the correction
        #  only ever subtracts. Lifting a node instead would push its whole
        #  range towards the ceiling for no gain in resolution.
        target = min(noise[i] for i in taking_part)

        pivots = self._norm_stored('norm_pivots', 0)
        offsets = self._norm_stored('norm_offsets', 0)
        scales = self._norm_stored('norm_scales', NORM_UNITY_SLOPE)

        for idx in range(num):
            if idx not in taking_part:
                continue
            # Pivot 1 rather than 0: zero disables the correction outright, and
            #  every reading is at or above 1, so the upper segment is the one
            #  in use and the lower one never fires. That upper segment is the
            #  offset alone, which is exactly the correction one point supports.
            pivots[idx] = 1
            offsets[idx] = noise[idx] - target
            scales[idx] = NORM_UNITY_SLOPE

        self._norm_busy = True
        failed = []
        try:
            self._racecontext.rhui.emit_norm_wizard_state()
            for idx in taking_part:
                if not self._racecontext.interface.set_normalisation(
                        idx, pivots[idx], offsets[idx], scales[idx]):
                    failed.append(idx + 1)
            if not failed:
                self.norm_reset_extremums()
        finally:
            self._norm_busy = False

        if failed:
            self._norm_unresolved = list(failed)
            msg = ('Noise levelling was not accepted by node(s) {0}; '
                   'their correction is unknown - retry before racing').format(
                       ', '.join(str(n) for n in failed))
            logger.warning(msg)
            self._racecontext.rhui.emit_priority_message(msg)
            self._racecontext.rhui.emit_norm_wizard_state()
            return False

        self._norm_unresolved = []
        self._norm_store(pivots, offsets, scales)
        # Every reading the nodes give from here on is corrected, so nothing
        #  captured before this point can be compared with anything captured
        #  after it - the two sit on different axes, and a fit across the join
        #  measures the levelling rather than the receivers. Start the captures
        #  over against the levelled nodes.
        self._norm_captured = {}
        self._norm_invalidate_session()
        # Deliberately not _norm_applied_captures: the sweep is not finished, and
        #  marking it applied would park the wizard and refuse the real fit.
        self._norm_levelled_only = True
        self._racecontext.rhui.emit_norm_wizard_state()
        logger.info('Noise floors levelled to %d: offsets=%s',
                    target, [noise[i] - target for i in taking_part])
        self._racecontext.rhui.emit_priority_message(
            'Noise floors levelled on {0} nodes'.format(len(taking_part)))
        return True

    @catchLogExceptionsWrapper
    def norm_wizard_set_coefficient(self, node_index, which, value):
        """Set one node's pivot, offset or scale by hand and send it to the node.

        The offset moves the whole curve up or down - it is what decides where
        the gate lands, and it applies alone above the pivot. The scale is the
        Q8 gain below the pivot, so 256 is x1.00 and leaves the lower segment
        straight through. The pivot is where the two meet: raise it and more of
        the curve keeps unity gain, lower it and more of the curve is pulled
        onto the common floor.

        Editing the offset keeps the scale as it is: the lower segment is
        anchored on the pivot, which the offset carries with it, so the shape
        below the pivot rides along instead of needing a second edit.

        Editing the pivot leaves the scale alone too, which tilts the lower
        segment rather than preserving where the floor lands - moving the pivot
        changes the span the same gain has to cover. Re-deriving the scale here
        would silently undo a scale the operator had just set by hand, so the
        two stay independent and a pivot edit is followed by a scale edit when
        the floor matters.

        There is deliberately no field for a gain above the pivot. That region
        is a pure translation by design, and the node has no coefficient for it.

        :param node_index: Zero-based node
        :param which: 'pivot', 'offset' or 'scale'
        :param value: The pivot or offset in counts, or the Q8 scale
        :return: True when the node took it
        """
        if which not in ('pivot', 'offset', 'scale'):
            return False
        try:
            node_index = int(node_index)
            value = int(value)
        except (TypeError, ValueError):
            return False  # came from the page, so treat junk as a no-op
        num = self._racecontext.race.num_nodes
        if not 0 <= node_index < num:
            return False
        if which == 'scale' and not NORM_SLOPE_MIN <= value <= NORM_SLOPE_MAX:
            self._racecontext.rhui.emit_priority_message(
                'Node {0}: scale must be between {1} (x{2:.2f}) and {3} (x{4:.2f})'
                .format(node_index + 1, NORM_SLOPE_MIN,
                        NORM_SLOPE_MIN / float(NORM_UNITY_SLOPE), NORM_SLOPE_MAX,
                        NORM_SLOPE_MAX / float(NORM_UNITY_SLOPE)))
            return False
        # The node carries the offset as a signed 16-bit value, and an offset
        #  wider than the scale is a typo rather than a correction.
        if which == 'offset' and not -NORM_FULL_SCALE <= value <= NORM_FULL_SCALE:
            self._racecontext.rhui.emit_priority_message(
                'Node {0}: offset must be between {1} and {2}'
                .format(node_index + 1, -NORM_FULL_SCALE, NORM_FULL_SCALE))
            return False
        # Pivot 0 is the node's "correction off" sentinel, so it is not a value
        #  to type here - Reset is how a correction is removed. Above full scale
        #  the pivot is unreachable and the lower segment would never fire.
        if which == 'pivot' and not 1 <= value <= NORM_FULL_SCALE:
            self._racecontext.rhui.emit_priority_message(
                'Node {0}: pivot must be between 1 and {1}; use Reset to turn '
                'the correction off'.format(node_index + 1, NORM_FULL_SCALE))
            return False
        if getattr(self, '_norm_busy', False):
            return False

        pivots = self._norm_stored('norm_pivots', 0)
        offsets = self._norm_stored('norm_offsets', 0)
        scales = self._norm_stored('norm_scales', NORM_UNITY_SLOPE)

        pivot = pivots[node_index]
        if not pivot:
            self._racecontext.rhui.emit_priority_message(
                'Node {0} has no calibration to adjust yet'.format(node_index + 1))
            return False

        if which == 'offset':
            offsets[node_index] = value
        elif which == 'pivot':
            pivots[node_index] = pivot = value
        else:
            scales[node_index] = value

        self._norm_busy = True
        try:
            ok = self._racecontext.interface.set_normalisation(
                node_index, pivot, offsets[node_index], scales[node_index])
        finally:
            self._norm_busy = False
        if not ok:
            self._racecontext.rhui.emit_priority_message(
                'Node {0} did not take the {1}'.format(node_index + 1, which))
            self._racecontext.rhui.emit_norm_wizard_state()
            return False

        self._norm_store(pivots, offsets, scales)
        logger.info('Node %d %s set to %d by hand', node_index + 1, which, value)
        self._racecontext.rhui.emit_norm_wizard_state()
        return True

    def _norm_discard_level(self, level):
        """Drop every capture taken at one level, keeping the other levels.

        The captures at a level all share one placement of the quad, so they
        stand or fall together. Noise was measured with no quad at all and is
        untouched.

        :param level: 'high'
        :return: How many captures were discarded
        """
        captured = getattr(self, '_norm_captured', None) or {}
        prefix = '{0}:'.format(level)
        doomed = [k for k in captured if k.startswith(prefix)]
        for key in doomed:
            del captured[key]
        if doomed:
            # Cancel a capture still settling, then re-stamp what survives, as
            #  stepping back does: the other levels remain valid for this
            #  configuration and must not be thrown away with these.
            self._norm_invalidate_session()
            self._norm_note_capture_session()
        return len(doomed)

    def _norm_min_gap(self, node_index=0):
        """The smallest floor-to-gate gap a node may report and still be fitted."""
        return max(1, int(round(NORM_MIN_LEVEL_FRACTION * self._norm_scale(node_index))))

    def _norm_reject_reason(self, level, channel, vals):
        """Why this capture cannot be filed, or None when it can.

        The gate is checked against the noise floor already captured, which is
        the one pair the fit divides by. It rejects a gate reading taken with
        the quad too far from the gate, or on the wrong channel - the case that
        otherwise fits a huge scale to a handful of counts and is not noticed
        until Apply.

        Noise is captured first and stands alone, so it is always accepted.

        :param level: 'noise' or 'high'
        :param channel: The channel being captured, or None for noise
        :param vals: This capture's reading per node
        :return: A message naming the node and what to do, or None
        """
        if level == 'noise' or channel is None:
            return None  # the floor is the reference; nothing to compare it to

        noise = (getattr(self, '_norm_captured', None) or {}).get(
            'noise:{0}'.format(channel))
        if not noise:
            return None  # no floor to compare against yet

        # Only nodes tuned to this channel measure it; the rest are bystanders
        #  reading bleed from an adjacent channel and say nothing useful here.
        labels = self._norm_node_channels()
        gap = self._norm_min_gap()
        for idx in self._norm_participants():
            if labels[idx] != channel:
                continue
            hi, fl = vals[idx], noise[idx]
            if hi is None or fl is None:
                continue
            if (hi - fl) < gap:
                return ('Node {0} ({1}): gate {2} is only {3} above its noise '
                        'floor {4}, need {5}. The quad is too far from the gate '
                        'or off channel - place it on the marked spot and '
                        'capture this pass again from the first '
                        'channel.').format(idx + 1, channel, hi, hi - fl, fl, gap)
        return None

    def _vtx(self):
        """The VTX controller, made on first use so import order cannot matter."""
        vtx = getattr(self, '_vtx_controller', None)
        if vtx is None:
            from vtx_control import VtxController
            vtx = self._vtx_controller = VtxController(self._racecontext)
        return vtx

    def norm_vtx_available(self):
        """True when the wizard can command the quad's channel."""
        try:
            return self._vtx().available()
        except Exception:  # noqa: BLE001 - a missing plugin is not an error here
            logger.debug('No VTX controller available', exc_info=True)
            return False

    @catchLogExceptionsWrapper
    def norm_vtx_switch(self):
        """Move the whole step onto its channel: the nodes and the quad.

        One button, because they are one action. The nodes have to be on the
        channel being measured or the capture reads the wrong frequency, and the
        quad has to be on it or there is nothing to measure - doing one without
        the other is never what the operator wants.

        Nothing here waits or checks the quad. The operator can see its OSD, which
        is a better witness than anything the timer can infer from its own
        receivers, so they decide when to capture.
        """
        state = self.norm_wizard_state()
        label = state.get('channel')
        if not label:
            # applied and ready steps have no channel to move to
            return False
        if getattr(self, '_norm_busy', False):
            return False

        # The nodes first: this is also the retune the capture would otherwise do
        #  for itself, so doing it here lets the receivers settle while the
        #  operator is walking the quad out to the gate.
        tuned = None
        for band, chan, freq in self._norm_sweep_channels():
            if '{0}{1}'.format(band, chan) == label:
                self._norm_tune_all(band, chan, freq)
                tuned = label
                break

        sent = False
        if self.norm_vtx_available():
            try:
                self._vtx().command_channel(label)
                sent = True
            except Exception as exc:  # noqa: BLE001 - reported, not raised
                logger.warning('VTX channel command failed: %s', exc)
                self._racecontext.rhui.emit_priority_message(
                    'Nodes moved to {0}, but the quad was not told: {1}'.format(
                        label, exc))
                self._racecontext.rhui.emit_norm_wizard_state()
                return False

        if not tuned and not sent:
            return False
        self._racecontext.rhui.emit_priority_message(
            'Nodes and quad moved to {0}'.format(label) if sent
            else 'Nodes moved to {0}; set the quad by hand'.format(label))
        self._racecontext.rhui.emit_norm_wizard_state()
        return True

    @catchLogExceptionsWrapper
    def norm_wizard_back(self):
        """Drop the most recent capture and return to that step."""
        captured = getattr(self, '_norm_captured', None) or {}
        if not captured:
            return False
        order = [l if c is None else '{0}:{1}'.format(l, c)
                 for l, c in self._norm_steps()]
        last = [k for k in order if k in captured][-1]
        del captured[last]
        # the set no longer matches the fit that was applied from it
        self._norm_applied_captures = False
        # Cancel a capture still settling, then re-stamp what remains: the
        #  earlier steps are still valid for this configuration and stepping
        #  back must not throw them away.
        self._norm_invalidate_session()
        self._norm_note_capture_session()
        logger.info('Normalisation stepped back, discarded %s', last)
        self.norm_reset_extremums()
        self._racecontext.rhui.emit_norm_wizard_state()
        return True

    @catchLogExceptionsWrapper
    def norm_wizard_reset(self):
        """Clear the calibration and arm the wizard from the start.

        This drops the applied constants too. A capture is only meaningful
        against uncorrected readings, so a fresh run has to start from raw.
        """
        self._norm_captured = {}
        self._norm_applied_captures = False
        self._norm_levelled_only = False
        self._norm_invalidate_session()
        # A run that is reset mid-sweep has left the fleet on a capture channel,
        #  so put the assignment back before anything else.
        self.norm_restore_frequencies()
        self._norm_scope_sel = None
        self._norm_channels = None
        num = self._racecontext.race.num_nodes
        self._norm_busy = True
        try:
            self._norm_store([0] * num, [0] * num, [NORM_UNITY_SLOPE] * num, {})
            failed = []
            for idx in range(num):
                if not self._racecontext.interface.set_normalisation(idx, 0, 0, NORM_UNITY_SLOPE):
                    failed.append(idx + 1)
            if not failed:
                # EnterAt/ExitAt are left exactly as the operator set them.
                #  Clearing the correction does move what a given number means,
                #  but rewriting a tuned threshold is worse than leaving it:
                #  the operator knows what their gate should trigger on.
                # The tracking reset is a hardware mutation, so it belongs
                #  inside the guard rather than after it.
                self.norm_reset_extremums()
        finally:
            self._norm_busy = False

        if failed:
            # A node that did not confirm may still be correcting, so the axis
            #  is unknown and the thresholds must not be moved as though it
            #  were not.
            self._norm_unresolved = list(failed)
            msg = ('Normalisation reset was not accepted by node(s) {0}; '
                   'their correction is unknown - retry before racing').format(
                       ', '.join(str(n) for n in failed))
            logger.warning(msg)
            self._racecontext.rhui.emit_priority_message(msg)
            self._racecontext.rhui.emit_norm_wizard_state()
            return False
        self._norm_unresolved = []
        self._racecontext.rhui.emit_norm_wizard_state()
        logger.info('Normalisation cleared')
        return True

    def _norm_store(self, pivots, offsets, scales, per_freq=None):
        """Persist the live coefficients, and optionally the per-frequency fits.

        The three lists are what the nodes are running now, one entry per node.
        `per_freq` maps a frequency to a fit per node, and is what a retune looks
        up: a receiver's sensitivity differs channel to channel, so a node moved
        to another channel needs that channel's own fit rather than this one.

        Passing per_freq=None leaves the stored table alone, which is what the
        paths that adjust the live fit by hand want - they change what a node is
        running without claiming to have re-measured any channel.
        """
        profile = self._racecontext.race.profile
        data = {
            'profile_id': profile.id,
            'norm_pivots': {"v": pivots},
            'norm_offsets': {"v": offsets},
            'norm_scales': {"v": scales},
            }
        if per_freq is not None:
            data['norm_per_freq'] = per_freq
        self._racecontext.race.profile = self._racecontext.rhdata.alter_profile(data)

    @catchLogExceptionsWrapper
    def norm_wizard_apply(self):
        """Fit every node on every captured channel and send the live one.

        A fit is per [node, frequency]: each capture step read every node on one
        channel, so every node has a floor and a gate for each channel in scope.
        A receiver's sensitivity differs channel to channel, and reusing one fit
        across a band left a measurable error - calibrate on one channel, move
        the fleet, and cross-node agreement degrades to 6-11% within R band
        alone. R and L are 555 MHz apart, so across both it is worse.

        The reference is global - the single highest gate anywhere in the sweep -
        so one number means one signal on every node and every channel, and a
        single EnterAt/ExitAt holds throughout. The target scale is folded into
        the offsets, which is why the node needs no notion of it.

        Only the fit for the channel a node is about to return to goes to the
        hardware. The rest are stored and written when that node is retuned.
        """
        if self.norm_wizard_state()['state'] != 'ready':
            return False

        captured = self._norm_captured
        num = self._racecontext.race.num_nodes
        channels = self._norm_sweep_channels()

        taking_part = self._norm_participants()
        if not taking_part:
            msg = 'No node is available to calibrate'
            logger.warning(msg)
            self._racecontext.rhui.emit_priority_message(msg)
            return False

        # Read every capture before fitting anything: the destination is the
        #  highest gate across the whole sweep, so no node on any channel can be
        #  fitted until all of them are known.
        levels = {}
        for band, chan, freq in channels:
            label = '{0}{1}'.format(band, chan)
            gates = captured.get('high:{0}'.format(label))
            floors = captured.get('noise:{0}'.format(label))
            if not gates or not floors:
                msg = 'No reading captured on {0}'.format(label)
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False
            for idx in taking_part:
                hi, fl = gates[idx], floors[idx]
                if hi is None or fl is None:
                    msg = 'Node {0} has no reading on {1}'.format(idx + 1, label)
                    logger.warning(msg)
                    self._racecontext.rhui.emit_priority_message(msg)
                    return False
                # Only the arithmetic floor is enforced: the fit divides by the
                #  floor-to-pivot span, so it has to be positive. How wide is
                #  worth having is the operator's judgement, not this function's.
                if hi <= fl:
                    msg = ('Node {0} levels are not in order on {1} '
                           '(floor={2}, gate={3})').format(idx + 1, label, fl, hi)
                    logger.warning(msg)
                    self._racecontext.rhui.emit_priority_message(msg)
                    return False
                levels[(idx, freq)] = (fl, hi)

        # One reference for the whole sweep, not one per band: R and L on
        #  separate axes would mean a threshold set for one is wrong for the
        #  other.
        target_gate, target_floor = self._norm_targets(list(levels.values()))
        logger.info('Normalisation destinations across %d channels: gate=%d floor=%d',
                    len(channels), target_gate, target_floor)

        # Keyed by frequency as a string, so it survives a JSON round trip and a
        #  retune can look its fit up directly.
        per_freq = {}
        for (idx, freq), (fl, hi) in levels.items():
            row = per_freq.setdefault(str(freq), [None] * num)
            row[idx] = list(self._norm_fit(fl, hi, target_gate, target_floor))

        # What goes to the hardware now: each node's fit for the channel it is
        #  about to be restored to. A node whose channel was never calibrated
        #  gets pivot 0 and runs uncorrected rather than on another channel's fit.
        saved = getattr(self, '_norm_saved_freqs', None) or self._norm_profile_freqs()
        pivots, offsets, scales = [], [], []
        for idx in range(num):
            fit = None
            freq = saved['f'][idx]
            if freq:
                fit = (per_freq.get(str(freq)) or [None] * num)[idx]
            if fit is None:
                pivots.append(0)
                offsets.append(0)
                scales.append(NORM_UNITY_SLOPE)
            else:
                pivots.append(fit[0])
                offsets.append(fit[1])
                scales.append(fit[2])

        self._norm_busy = True
        try:
            self._norm_store(pivots, offsets, scales, per_freq)
            failed = []
            for idx in range(num):
                if not self._racecontext.interface.set_normalisation(
                        idx, pivots[idx], offsets[idx], scales[idx]):
                    failed.append(idx + 1)

        finally:
            self._norm_busy = False

        if failed:
            # The stored fit no longer describes the hardware, so the axis is
            #  unknown rather than merely different.
            # Nodes that took the write are on the new correction while their
            #  thresholds are still on the old one, and the rest are in an
            #  unknown state. Neither is safe to time against, so record it
            #  and keep racing blocked until a run succeeds or clears it.
            self._norm_unresolved = list(failed)
            msg = ('Normalisation was not accepted by node(s) {0}; '
                   'their correction is unknown - re-run calibration or reset '
                   'it before racing').format(', '.join(str(n) for n in failed))
            logger.warning(msg)
            self._racecontext.rhui.emit_priority_message(msg)
            self._racecontext.rhui.emit_norm_wizard_state()
            return False
        self._norm_unresolved = []

        gevent.sleep(0.5)
        self.norm_reset_extremums()
        gevent.sleep(0.5)
        self.norm_reset_extremums()

        # Keep the captures. They are what the fit was made from, so holding
        #  them lets a level that read wrong be corrected and re-applied
        #  without sweeping the whole fleet again. They are raw readings taken
        #  before any correction was on the nodes, so they stay valid as the
        #  source for a new fit.
        # The sweep left every node on the last channel it captured, which is
        #  not a race configuration. Restoring also sends each node the fit for
        #  the channel it lands on.
        self.norm_restore_frequencies()
        self._norm_applied_captures = True
        self._norm_levelled_only = False
        self._racecontext.rhui.emit_norm_wizard_state()
        logger.info('Normalisation applied: pivots=%s offsets=%s scales=%s',
                    pivots, offsets, scales)
        self._racecontext.rhui.emit_priority_message(
            'Normalisation applied to {0} nodes'.format(num))
        return True

    def _norm_fit_for(self, node_index, freq):
        """The stored fit for one node on one frequency, or None.

        The table is keyed by frequency as a string, since it round-trips through
        JSON.
        """
        raw = getattr(self._racecontext.race.profile, 'norm_per_freq', None)
        if not raw:
            return None
        try:
            table = json.loads(raw) if isinstance(raw, str) else raw
        except (TypeError, ValueError):
            return None
        row = table.get(str(freq)) if isinstance(table, dict) else None
        if not row or node_index >= len(row) or not row[node_index]:
            return None
        return row[node_index]

    def norm_apply_for_frequency(self, node_index, freq):
        """Put the fit for `freq` on one node, after it has been retuned.

        A fit belongs to a [node, frequency] pair, so a node moved to another
        channel is running the wrong correction until this is called. A channel
        that was never calibrated gets pivot 0 - uncorrected is honest, where
        another channel's fit would be a silent error.

        :return: True when a fit was written, False when the node was cleared
        """
        fit = self._norm_fit_for(node_index, freq)
        pivots = self._norm_stored('norm_pivots', 0)
        offsets = self._norm_stored('norm_offsets', 0)
        scales = self._norm_stored('norm_scales', NORM_UNITY_SLOPE)
        if fit is None:
            # Nothing measured here. Only say so when the node was corrected
            #  until now, or every uncalibrated retune would log.
            if pivots[node_index]:
                logger.info('Node %d retuned to %s MHz, which has no fit: '
                            'running uncorrected', node_index + 1, freq)
            pivot, offset, scale = 0, 0, NORM_UNITY_SLOPE
        else:
            pivot, offset, scale = fit
        if not self._racecontext.interface.set_normalisation(
                node_index, pivot, offset, scale):
            logger.warning('Node %d did not take the fit for %s MHz',
                           node_index + 1, freq)
            return False
        pivots[node_index], offsets[node_index], scales[node_index] = \
            pivot, offset, scale
        self._norm_store(pivots, offsets, scales)
        return fit is not None

    def norm_state_is_unresolved(self):
        """True when the correction on the nodes is not known to be correct.

        Set when a coefficient write is not confirmed: some nodes may be on a
        new correction with thresholds still on the old one, and others in an
        unknown state. Timing against that is worse than refusing to start.
        """
        return bool(getattr(self, '_norm_unresolved', None))

    def norm_unresolved_nodes(self):
        return list(getattr(self, '_norm_unresolved', []) or [])

    def norm_reset_extremums(self):
        """Restart peak/nadir tracking on every node."""
        for idx in range(self._racecontext.race.num_nodes):
            self._racecontext.interface.reset_node_extremums(idx)

    def hardware_set_all_normalisation(self):
        """Re-send the stored calibration; nodes keep nothing across a power cycle."""
        pivots = self._norm_stored('norm_pivots', 0)
        offsets = self._norm_stored('norm_offsets', 0)
        scales = self._norm_stored('norm_scales', NORM_UNITY_SLOPE)
        failed = []
        for idx in range(self._racecontext.race.num_nodes):
            if not self._racecontext.interface.set_normalisation(
                    idx, pivots[idx], offsets[idx], scales[idx]):
                failed.append(idx + 1)
        if failed:
            msg = ('Normalisation was not accepted by node(s) {0}; '
                   'those nodes are running uncorrected').format(
                       ', '.join(str(n) for n in failed))
            logger.warning(msg)
            self._racecontext.rhui.emit_priority_message(msg)
        return not failed

    def threshold_scale_id(self):
        """Fingerprint of the axis stored EnterAt/ExitAt are measured on.

        A threshold is compared against whatever rssiRead() returns, which is
        the reading after normalisation, so the correction in force defines the
        axis and a stored value only means the same thing while it holds.

        The key is 'norm'. A profile carrying the older 'eq' marker was written
        against the five-coefficient fit, which no longer exists, so it reads as
        no marker at all - and that is the honest answer: those thresholds sit
        on an axis this code cannot reproduce.
        """
        return {'norm': self._norm_signature()}

    def _norm_signature(self):
        """The correction in force, per node, or None where there is none."""
        pivots = self._norm_stored('norm_pivots', 0)
        if not any(pivots):
            return None
        return [[pivots[i],
                 self._norm_stored('norm_offsets', 0)[i],
                 self._norm_stored('norm_scales', NORM_UNITY_SLOPE)[i]]
                for i in range(len(pivots))]

    def _corrected(self, raw, coeffs):
        """What the node reports for a raw reading under `coeffs`.

        Mirrors the node's own arithmetic, shift for shift, so a threshold
        converted here lands where the node will actually put it.
        """
        if not coeffs:
            return raw
        pivot, offset, scale = coeffs
        if not pivot:
            return raw
        if raw >= pivot:
            adj = raw - offset
        else:
            adj = (pivot - offset) + (((raw - pivot) * scale) >> 8)
        return max(0, adj)

    def _uncorrect(self, value, coeffs):
        """The raw reading that produces `value` under `coeffs`."""
        if not coeffs:
            return value
        pivot, offset, scale = coeffs
        if not pivot:
            return value
        pivot_target = pivot - offset
        if value >= pivot_target:
            return value + offset
        if not scale:
            return value
        return pivot + int(round((value - pivot_target) * 256.0 / scale))

    def _stored_scale_id(self, profile):
        """The axis the stored thresholds were written on, if recorded.

        Profiles written before this was tracked carry no marker; they predate
        normalisation, so they are uncorrected. So does a profile carrying the
        superseded 'eq' marker, whose fit this code can no longer evaluate.
        """
        raw = getattr(profile, 'enter_ats', None)
        if not raw:
            return None
        try:
            stored = json.loads(raw)
        except (TypeError, ValueError):
            return None
        return {'norm': stored.get('norm')}

    def hardware_set_all_enter_ats(self, enter_at_levels):
        '''send update to nodes'''
        logger.debug("Sending enter-at values to nodes: " + str(enter_at_levels))
        for idx in range(self._racecontext.race.num_nodes):
            if enter_at_levels[idx]:
                self._racecontext.interface.set_enter_at_level(idx, enter_at_levels[idx])
            else:
                self.set_enter_at_level(idx, self._racecontext.interface.nodes[idx].enter_at_level)

    def hardware_set_all_exit_ats(self, exit_at_levels):
        '''send update to nodes'''
        logger.debug("Sending exit-at values to nodes: " + str(exit_at_levels))
        for idx in range(self._racecontext.race.num_nodes):
            if exit_at_levels[idx]:
                self._racecontext.interface.set_exit_at_level(idx, exit_at_levels[idx])
            else:
                self.set_exit_at_level(idx, self._racecontext.interface.nodes[idx].exit_at_level)

    def auto_calibrate(self):
        ''' Apply best tuning values to nodes '''
        if self._racecontext.race.current_heat == RHUtils.HEAT_ID_NONE:
            logger.debug('Skipping auto calibration; server in practice mode')
            return None

        for seat_index, node in enumerate(self._racecontext.interface.nodes):
            calibration = self.find_best_calibration_values(node, seat_index)

            if node.enter_at_level is not calibration['enter_at_level']:
                self.set_enter_at_level(seat_index, calibration['enter_at_level'], emit_levels=False)

            if node.exit_at_level is not calibration['exit_at_level']:
                self.set_exit_at_level(seat_index, calibration['exit_at_level'], emit_levels=False)

        logger.info('Updated calibration with best discovered values')
        self._racecontext.rhui.emit_enter_and_exit_at_levels()  # one broadcast for all nodes

    @staticmethod
    def _norm_signature_key(signature):
        """A comparable form of a normalisation signature.

        Stored ones arrive as JSON text, live ones as lists; None and the
        string "null" both mean no correction.
        """
        if signature is None or signature == 'null':
            return None
        if isinstance(signature, str):
            try:
                signature = json.loads(signature)
            except (TypeError, ValueError):
                return None
        if not signature:
            return None
        return json.dumps(signature, sort_keys=True)

    def _race_matches_correction(self, race):
        """True if this saved race was timed under the correction in use.

        Adaptive calibration restores EnterAt/ExitAt straight out of race
        history, and a threshold only means the same signal while the
        correction that produced it still holds. Races saved before this was
        tracked carry no tag and are uncorrected, which is what the code of
        the time produced.
        """
        stored = self._racecontext.rhdata.get_savedrace_attribute_value(
            race, 'norm_signature', None)
        return self._norm_signature_key(stored) == self._norm_signature_key(
            self._norm_signature())

    def find_best_calibration_values(self, node, seat_index):
        ''' Search race history for best tuning values '''

        # get commonly used values
        heat = self._racecontext.rhdata.get_heat(self._racecontext.race.current_heat)
        pilot = self._racecontext.rhdata.get_pilot_from_heatNode(self._racecontext.race.current_heat, seat_index)
        current_class = heat.class_id
        races = self._racecontext.rhdata.get_savedRaceMetas()
        races.sort(key=lambda x: x.id, reverse=True)
        # Drop races timed under a different correction; their thresholds are
        #  on another axis and would put a node permanently in or out of
        #  crossing.
        usable_race_ids = set()
        skipped = 0
        for race in list(races):
            if self._race_matches_correction(race):
                usable_race_ids.add(race.id)
            else:
                races.remove(race)
                skipped += 1
        if skipped:
            logger.debug('Ignoring %d saved race(s) timed under a different '
                         'normalisation', skipped)
        pilotRaces = [p for p in self._racecontext.rhdata.get_savedPilotRaces()
                      if p.race_id in usable_race_ids]
        pilotRaces.sort(key=lambda x: x.id, reverse=True)

        # test for disabled node
        if pilot is RHUtils.PILOT_ID_NONE or node.frequency is RHUtils.FREQUENCY_ID_NONE:
            logger.debug('Node {0} calibration: skipping disabled node'.format(node.index+1))
            return {
                'enter_at_level': node.enter_at_level,
                'exit_at_level': node.exit_at_level
            }

        # test for same heat, same node
        for race in races:
            if race.heat_id == heat.id:
                for pilotRace in pilotRaces:
                    if pilotRace.race_id == race.id and \
                        pilotRace.node_index == seat_index and \
                        pilotRace.frequency == node.frequency:
                        logger.debug('Node {0} calibration: found same pilot+node in same heat'.format(node.index+1))
                        return {
                            'enter_at_level': pilotRace.enter_at,
                            'exit_at_level': pilotRace.exit_at
                        }
                break

        # test for same class, same pilot, same node
        for race in races:
            if race.class_id == current_class:
                for pilotRace in pilotRaces:
                    if pilotRace.race_id == race.id and \
                        pilotRace.node_index == seat_index and \
                        pilotRace.pilot_id == pilot and \
                        pilotRace.frequency == node.frequency:
                        logger.debug('Node {0} calibration: found same pilot+node in other heat with same class'.format(node.index+1))
                        return {
                            'enter_at_level': pilotRace.enter_at,
                            'exit_at_level': pilotRace.exit_at
                        }
                break

        # test for same pilot, same node
        for pilotRace in pilotRaces:
            if pilotRace.node_index == seat_index and \
                pilotRace.pilot_id == pilot and \
                pilotRace.frequency == node.frequency:
                logger.debug('Node {0} calibration: found same pilot+node in other heat with other class'.format(node.index+1))
                return {
                    'enter_at_level': pilotRace.enter_at,
                    'exit_at_level': pilotRace.exit_at
                }

        # test for same node
        for pilotRace in pilotRaces:
            if pilotRace.node_index == seat_index and \
                pilotRace.frequency == node.frequency:
                logger.debug('Node {0} calibration: found same node in other heat'.format(node.index+1))
                return {
                    'enter_at_level': pilotRace.enter_at,
                    'exit_at_level': pilotRace.exit_at
                }

        # fallback
        logger.debug('Node {0} calibration: no calibration hints found, no change'.format(node.index+1))
        context = {
            'seat_index': seat_index,
            'pilot': pilot,
            'enter_at_level': node.enter_at_level,
            'exit_at_level': node.exit_at_level
        }
        context = self._racecontext.filters.run_filters(Flt.CALIBRATION_FALLBACK, context, {
            'heat_id': heat.id,
            'pilot_id': pilot,
            'class_id': heat.class_id
        })
        return {
            'enter_at_level': context['enter_at_level'],
            'exit_at_level': context['exit_at_level']
        }
    