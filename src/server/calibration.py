'''Seat calibration adjustment'''

import logging
import gevent
import json
import RHUtils
from eventmanager import Evt
from RHUtils import catchLogExceptionsWrapper
from filtermanager import Flt

logger = logging.getLogger(__name__)

# Where the calibrated levels land on the corrected scale is derived from the
#  captures themselves, not from fixed fractions: every fleet has different
#  receivers, so any constant chosen here would be right for one timer and
#  wrong for the next. The destination is the widest span any node showed, so
#  the node that already resolves best is left alone and every other node is
#  stretched up to match it. No node is ever compressed, and the result scales
#  automatically with the width of the pipeline, since a narrower one reports
#  proportionally narrower spans.
#
# The one thing that cannot come from the captures is headroom: "high" is the
#  quad at the gate, not saturation, and a closer pass has to stay on scale.
#  Cap the top of the mapped range at this fraction of full scale.
EQ_HEADROOM_FRACTION = 0.5

# What a node's reading can reach. The node pipeline is a byte wide, so this is
#  a byte; a wider pipeline would raise it, and the destination below follows
#  the captures rather than this number, so nothing else has to change.
EQ_FULL_SCALE = 255

# Minimum gap between adjacent captured levels, as a fraction of full scale.
#  A node that never saw the quad reads only noise and would otherwise get an
#  absurd slope. Deliberately loose: a node high on its detector curve
#  compresses legitimately - one measured fleet spanned 4.6% to 7.1% of scale
#  between its low and high levels, so anything above a few percent is real.
#  A fraction rather than a count, so it follows the width of the pipeline.
EQ_MIN_LEVEL_FRACTION = 0.015

# How long to watch a node after clearing its extremes, before reading them.
#  The clear has to happen after the operator has set the condition up, not
#  before: a peak only ever rises, so extremes cleared at the end of the
#  previous step would already hold whatever the VTX did while its channel was
#  being changed.
EQ_SETTLE_SECONDS = 5.0

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

    # --- equalisation -----------------------------------------------------

    def _eq_stored(self, field, default):
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

    def _eq_participants(self):
        """Which nodes the wizard calibrates.

        A node with no frequency is not receiving anything, and a node whose
        firmware predates the protocol will ignore the coefficients. Neither
        can contribute a capture, so neither should be able to hold the wizard
        open or have a fit computed for it.
        """
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

    def _eq_node_channels(self):
        """The channel label each node is tuned to, one per node.

        Nodes that are not participating get None, so they raise no step of
        their own and are skipped by the fit.
        """
        freqs = json.loads(self._racecontext.race.profile.frequencies)
        bands, chans = freqs.get('b') or [], freqs.get('c') or []
        taking_part = set(self._eq_participants())
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

    def _eq_steps(self):
        """The capture sequence: one noise step, then low/high per channel.

        Noise needs no quad and no channel change, so it is captured once.
        Every distinct channel then needs the quad on it at two power levels.
        """
        steps = [('noise', None)]
        seen = []
        for label in self._eq_node_channels():
            if label is None:
                continue  # node is not taking part
            if label not in seen:
                seen.append(label)
                steps.append(('low', label))
                steps.append(('high', label))
        return steps

    def _eq_scale(self, node_index):
        """What a node's corrected reading can reach.

        Based on what the pipeline can actually carry, not on max_rssi_value -
        that is the "no nadir recorded" sentinel and sits above the real range.
        """
        return EQ_FULL_SCALE

    def _eq_destination(self, spans):
        """Where every node's levels should land, from the captured spans.

        `spans` is (low_span, band_span) per node - noise-to-low and
        low-to-high as that node actually reported them. The widest of each
        becomes the common destination, so the best node keeps its own scale
        and the rest are stretched onto it. Scaled down only if the result
        would not leave room above the calibrated high for a closer quad:
        headroom wins over stretch, because a reading that clips is lost
        outright while a slightly compressed one is merely coarser.
        """
        low_span = max(s[0] for s in spans)
        band_span = max(s[1] for s in spans)
        # The floor sits just clear of zero so an idle node still reads alive,
        #  and it counts against the headroom like everything else.
        floor_frac = 0.01
        top = (low_span + band_span) * (1.0 + floor_frac)
        limit = self._eq_scale(0) * EQ_HEADROOM_FRACTION
        if top > limit and top > 0:
            shrink = limit / top
            low_span *= shrink
            band_span *= shrink
        t_floor = max(1, int(round((low_span + band_span) * floor_frac)))
        return (t_floor,
                t_floor + int(round(low_span)),
                t_floor + int(round(low_span + band_span)))

    def eq_wizard_state(self):
        """Where the wizard is: the next step, or done."""
        captured = getattr(self, '_eq_captured', None) or {}
        busy = getattr(self, '_eq_busy', False)
        steps = self._eq_steps()

        if captured and not self._eq_captures_are_current():
            # measured against a configuration that is no longer loaded
            logger.info('Discarding equalisation captures: configuration changed')
            captured = {}
            self._eq_captured = {}

        if not captured and any(self._eq_stored('eq_pivots', 0)):
            # already calibrated - do not arm the first step, so a stray click
            #  cannot start overwriting a good calibration
            return {'state': 'applied', 'level': None, 'channel': None,
                    'index': 0, 'total': len(steps), 'busy': busy,
                    'settle': EQ_SETTLE_SECONDS}

        for level, chan in steps:
            key = level if chan is None else '{0}:{1}'.format(level, chan)
            if key not in captured:
                return {'state': 'capturing', 'level': level, 'channel': chan,
                        'index': len(captured), 'total': len(steps),
                        'busy': busy, 'settle': EQ_SETTLE_SECONDS}
        return {'state': 'ready', 'level': None, 'channel': None,
                'index': len(steps), 'total': len(steps), 'busy': busy,
                'settle': EQ_SETTLE_SECONDS}

    def eq_captured_table(self):
        """Per-node view for the UI: what has been captured, or what is applied."""
        captured = getattr(self, '_eq_captured', None) or {}
        num = self._racecontext.race.num_nodes
        labels = self._eq_node_channels()

        if captured:
            noise = captured.get('noise', [None] * num)
            return [{
                'channel': labels[i], 'mode': 'capture',
                'noise': noise[i],
                'low': captured.get('low:{0}'.format(labels[i]), [None] * num)[i],
                'high': captured.get('high:{0}'.format(labels[i]), [None] * num)[i],
            } for i in range(num)]

        pivots = self._eq_stored('eq_pivots', 0)
        ups = self._eq_stored('eq_slope_ups', 256)
        los = self._eq_stored('eq_slope_los', 256)
        return [{
            'channel': labels[i],
            'mode': 'applied' if pivots[i] else 'empty',
            'noise': None, 'low': pivots[i] or None,
            'high': None if not pivots[i] else ups[i],
            'slope_lo': None if not pivots[i] else los[i],
        } for i in range(num)]

    @catchLogExceptionsWrapper
    def _eq_session(self):
        """Identifies the configuration a capture belongs to.

        Actual frequencies rather than channel labels, so a retune that keeps
        the label - or one the label cannot express - still counts as a
        different configuration.
        """
        try:
            freqs = json.loads(self._racecontext.race.profile.frequencies)
            tuning = tuple(freqs.get('f') or [])
        except (TypeError, ValueError, AttributeError):
            tuning = ()
        return (getattr(self, '_eq_epoch', 0),
                getattr(self._racecontext.race.profile, 'id', None),
                tuning)

    def _eq_captures_are_current(self):
        """True when the captures on hand belong to the configuration in use."""
        captured = getattr(self, '_eq_captured', None)
        if not captured:
            return True
        return getattr(self, '_eq_captured_session', None) == self._eq_session()

    def _eq_note_capture_session(self):
        """Record which configuration the current capture set belongs to."""
        self._eq_captured_session = self._eq_session()

    def _eq_invalidate_session(self):
        """Drop any capture still settling."""
        self._eq_epoch = getattr(self, '_eq_epoch', 0) + 1

    def eq_wizard_capture(self):
        """Capture the next step: clear the extremes, settle, then read."""
        state = self.eq_wizard_state()
        if state['state'] != 'capturing' or getattr(self, '_eq_busy', False):
            return False

        self._eq_busy = True
        # Anything that changes what a capture would mean - a reset, a step
        #  back, a profile change - bumps this. The sleep below is long enough
        #  for that to happen underneath us, and a reading taken before the
        #  change must not be filed against the state after it.
        session = self._eq_session()
        try:
            self._racecontext.rhui.emit_eq_wizard_state()
            self.eq_reset_extremums()
            gevent.sleep(EQ_SETTLE_SECONDS)
            if self._eq_session() != session:
                logger.info('Equalisation capture discarded: state changed while settling')
                return False

            level = state['level']
            key = level if state['channel'] is None \
                else '{0}:{1}'.format(level, state['channel'])
            nodes = self._racecontext.interface.nodes
            vals = []
            for idx in range(self._racecontext.race.num_nodes):
                node = nodes[idx]
                v = node.node_nadir_rssi if level == 'noise' else node.node_peak_rssi
                vals.append(int(v) if v and v < node.max_rssi_value else None)

            taking_part = self._eq_participants()
            missing = [i + 1 for i in taking_part if vals[i] is None]
            if level == 'noise' and missing:
                msg = 'Noise capture failed: no reading on node {0}'.format(missing[0])
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False

            self._eq_captured = getattr(self, '_eq_captured', {})
            self._eq_captured[key] = vals
            self._eq_note_capture_session()
            logger.info('Equalisation captured %s: %s', key, vals)
            return True
        finally:
            self._eq_busy = False
            self._racecontext.rhui.emit_eq_wizard_state()

    @catchLogExceptionsWrapper
    def eq_wizard_back(self):
        """Drop the most recent capture and return to that step."""
        captured = getattr(self, '_eq_captured', None) or {}
        if not captured:
            return False
        order = [l if c is None else '{0}:{1}'.format(l, c)
                 for l, c in self._eq_steps()]
        last = [k for k in order if k in captured][-1]
        del captured[last]
        # Cancel a capture still settling, then re-stamp what remains: the
        #  earlier steps are still valid for this configuration and stepping
        #  back must not throw them away.
        self._eq_invalidate_session()
        self._eq_note_capture_session()
        logger.info('Equalisation stepped back, discarded %s', last)
        self.eq_reset_extremums()
        self._racecontext.rhui.emit_eq_wizard_state()
        return True

    @catchLogExceptionsWrapper
    def eq_wizard_reset(self):
        """Clear the calibration and arm the wizard from the start.

        This drops the applied constants too. A capture is only meaningful
        against uncorrected readings, so a fresh run has to start from raw.
        """
        self._eq_captured = {}
        self._eq_invalidate_session()
        previous_axis = self.threshold_scale_id()
        num = self._racecontext.race.num_nodes
        self._eq_busy = True
        try:
            self._eq_store([0] * num, [0] * num, [256] * num, [0] * num, [256] * num)
            failed = []
            for idx in range(num):
                if not self._racecontext.interface.set_equalisation(idx, 0, 0, 256, 0, 256):
                    failed.append(idx + 1)
            if not failed:
                # Clearing the correction moves the axis just as applying one
                #  does; convert inside the guard, since this writes to nodes.
                self.convert_thresholds_to_scale(from_axis=previous_axis)
        finally:
            self._eq_busy = False

        if failed:
            # A node that did not confirm may still be correcting, so the axis
            #  is unknown and the thresholds must not be moved as though it
            #  were not.
            self._eq_unresolved = list(failed)
            msg = ('Equalisation reset was not accepted by node(s) {0}; '
                   'their correction is unknown - retry before racing').format(
                       ', '.join(str(n) for n in failed))
            logger.warning(msg)
            self._racecontext.rhui.emit_priority_message(msg)
            self._racecontext.rhui.emit_eq_wizard_state()
            return False
        self._eq_unresolved = []
        self.eq_reset_extremums()
        self._racecontext.rhui.emit_eq_wizard_state()
        logger.info('Equalisation cleared')
        return True

    def _eq_store(self, pivots, offset_ups, slope_ups, offset_los, slope_los):
        profile = self._racecontext.race.profile
        self._racecontext.race.profile = self._racecontext.rhdata.alter_profile({
            'profile_id': profile.id,
            'eq_pivots': {"v": pivots},
            'eq_offset_ups': {"v": offset_ups},
            'eq_slope_ups': {"v": slope_ups},
            'eq_offset_los': {"v": offset_los},
            'eq_slope_los': {"v": slope_los},
            })

    @catchLogExceptionsWrapper
    def eq_wizard_apply(self):
        """Fit two segments per node from the captured levels and send them.

        Each node takes its levels from the channel it is tuned to, so a sweep
        that visits every channel gives every node an on-channel measurement.
        The target scale is folded into the offsets here, which is why the node
        needs no notion of it.
        """
        if self.eq_wizard_state()['state'] != 'ready':
            return False

        captured = self._eq_captured
        num = self._racecontext.race.num_nodes
        labels = self._eq_node_channels()
        noise = captured['noise']

        # Read every node's captures first: the destination is the widest span
        #  in the fleet, so no node can be fitted until all of them are known.
        #  Nodes not taking part get no fit and stay uncorrected.
        taking_part = self._eq_participants()
        if not taking_part:
            msg = 'No node is available to calibrate'
            logger.warning(msg)
            self._racecontext.rhui.emit_priority_message(msg)
            return False
        levels = {}
        for idx in taking_part:
            label = labels[idx]
            lo = captured.get('low:{0}'.format(label), [None] * num)[idx]
            hi = captured.get('high:{0}'.format(label), [None] * num)[idx]
            fl = noise[idx]
            if lo is None or hi is None or fl is None:
                msg = 'Node {0} has no reading on its own channel ({1})'.format(
                    idx + 1, label)
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False
            min_gap = max(1, EQ_MIN_LEVEL_FRACTION * self._eq_scale(idx))
            if (hi - lo) < min_gap or (lo - fl) < min_gap:
                msg = ('Node {0} levels are too close together '
                       '(noise={1}, low={2}, high={3})').format(idx + 1, fl, lo, hi)
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False
            levels[idx] = (fl, lo, hi)

        destination = self._eq_destination(
            [(lo - fl, hi - lo) for fl, lo, hi in levels.values()])
        logger.info('Equalisation destination from captured spans: %s', destination)

        pivots, offset_ups, slope_ups, offset_los, slope_los = [], [], [], [], []
        for idx in range(num):
            if idx not in levels:
                # not taking part: pivot 0 leaves this node uncorrected
                pivots.append(0)
                slope_ups.append(256)
                slope_los.append(256)
                offset_ups.append(0)
                offset_los.append(0)
                continue
            fl, lo, hi = levels[idx]

            t_floor, t_low, t_high = destination
            s_up = max(1, min(65535, int(round(
                (t_high - t_low) * 256.0 / (hi - lo)))))
            s_lo = max(1, min(65535, int(round(
                (t_low - t_floor) * 256.0 / (lo - fl)))))
            # fold the target into the offset: corrected = (raw - offset)*slope>>8
            #  passes through (lo -> t_low) for both segments, so they
            #  meet at the pivot
            pivots.append(lo)
            slope_ups.append(s_up)
            slope_los.append(s_lo)
            offset_ups.append(int(round(lo - t_low * 256.0 / s_up)))
            offset_los.append(int(round(lo - t_low * 256.0 / s_lo)))

        previous_axis = self.threshold_scale_id()

        self._eq_busy = True
        try:
            self._eq_store(pivots, offset_ups, slope_ups, offset_los, slope_los)
            failed = []
            for idx in range(num):
                if not self._racecontext.interface.set_equalisation(
                        idx, pivots[idx], offset_ups[idx], slope_ups[idx],
                        offset_los[idx], slope_los[idx]):
                    failed.append(idx + 1)

            if not failed:
                # Still inside the guard: the conversion writes thresholds to
                #  the nodes, so the window is not safe to race in either.
                self.convert_thresholds_to_scale(from_axis=previous_axis)
        finally:
            self._eq_busy = False

        if failed:
            # The stored fit no longer describes the hardware, so the axis is
            #  unknown rather than merely different.
            # Nodes that took the write are on the new correction while their
            #  thresholds are still on the old one, and the rest are in an
            #  unknown state. Neither is safe to time against, so record it
            #  and keep racing blocked until a run succeeds or clears it.
            self._eq_unresolved = list(failed)
            msg = ('Equalisation was not accepted by node(s) {0}; '
                   'their correction is unknown - re-run calibration or reset '
                   'it before racing').format(', '.join(str(n) for n in failed))
            logger.warning(msg)
            self._racecontext.rhui.emit_priority_message(msg)
            self._racecontext.rhui.emit_eq_wizard_state()
            return False
        self._eq_unresolved = []

        gevent.sleep(0.5)
        self.eq_reset_extremums()
        gevent.sleep(0.5)
        self.eq_reset_extremums()

        self._eq_captured = {}
        self._racecontext.rhui.emit_eq_wizard_state()
        logger.info('Equalisation applied: pivots=%s slopes=%s/%s',
                    pivots, slope_ups, slope_los)
        self._racecontext.rhui.emit_priority_message(
            'Equalisation applied to {0} nodes'.format(num))
        return True

    def eq_state_is_unresolved(self):
        """True when the correction on the nodes is not known to be correct.

        Set when a coefficient write is not confirmed: some nodes may be on a
        new correction with thresholds still on the old one, and others in an
        unknown state. Timing against that is worse than refusing to start.
        """
        return bool(getattr(self, '_eq_unresolved', None))

    def eq_unresolved_nodes(self):
        return list(getattr(self, '_eq_unresolved', []) or [])

    def eq_reset_extremums(self):
        """Restart peak/nadir tracking on every node."""
        for idx in range(self._racecontext.race.num_nodes):
            self._racecontext.interface.reset_node_extremums(idx)

    def hardware_set_all_equalisation(self):
        """Re-send the stored calibration; nodes keep nothing across a power cycle."""
        pivots = self._eq_stored('eq_pivots', 0)
        offset_ups = self._eq_stored('eq_offset_ups', 0)
        slope_ups = self._eq_stored('eq_slope_ups', 256)
        offset_los = self._eq_stored('eq_offset_los', 0)
        slope_los = self._eq_stored('eq_slope_los', 256)
        failed = []
        for idx in range(self._racecontext.race.num_nodes):
            if not self._racecontext.interface.set_equalisation(
                    idx, pivots[idx], offset_ups[idx], slope_ups[idx],
                    offset_los[idx], slope_los[idx]):
                failed.append(idx + 1)
        if failed:
            msg = ('Equalisation was not accepted by node(s) {0}; '
                   'those nodes are running uncorrected').format(
                       ', '.join(str(n) for n in failed))
            logger.warning(msg)
            self._racecontext.rhui.emit_priority_message(msg)
        return not failed

    def threshold_scale_id(self):
        """Fingerprint of the axis stored EnterAt/ExitAt are measured on.

        A threshold is compared against whatever rssiRead() returns, which is
        the reading after equalisation, so the correction in force defines the
        axis and a stored value only means the same thing while it holds.
        """
        return {'eq': self._eq_signature()}

    def _eq_signature(self):
        """The correction in force, per node, or None where there is none."""
        pivots = self._eq_stored('eq_pivots', 0)
        if not any(pivots):
            return None
        return [[pivots[i],
                 self._eq_stored('eq_offset_ups', 0)[i],
                 self._eq_stored('eq_slope_ups', 256)[i],
                 self._eq_stored('eq_offset_los', 0)[i],
                 self._eq_stored('eq_slope_los', 256)[i]]
                for i in range(len(pivots))]

    def _corrected(self, raw, coeffs):
        """What the node reports for a raw reading under `coeffs`."""
        if not coeffs:
            return raw
        pivot, off_up, slope_up, off_lo, slope_lo = coeffs
        if not pivot:
            return raw
        if raw >= pivot:
            adj = ((raw - off_up) * slope_up) >> 8
        else:
            adj = ((raw - off_lo) * slope_lo) >> 8
        return max(0, adj)

    def _uncorrect(self, value, coeffs):
        """The raw reading that produces `value` under `coeffs`."""
        if not coeffs:
            return value
        pivot, off_up, slope_up, off_lo, slope_lo = coeffs
        if not pivot:
            return value
        at_pivot = self._corrected(pivot, coeffs)
        if value >= at_pivot:
            return int(round(value * 256.0 / slope_up)) + off_up if slope_up else value
        return int(round(value * 256.0 / slope_lo)) + off_lo if slope_lo else value

    def _stored_scale_id(self, profile):
        """The axis the stored thresholds were written on, if recorded.

        Profiles written before this was tracked carry no marker; they predate
        equalisation, so they are uncorrected.
        """
        raw = getattr(profile, 'enter_ats', None)
        if not raw:
            return None
        try:
            stored = json.loads(raw)
        except (TypeError, ValueError):
            return None
        return {'eq': stored.get('eq')}

    def convert_thresholds_to_scale(self, from_axis=None):
        """Move stored EnterAt/ExitAt onto the axis now in force.

        A threshold is a corrected value, so when the correction changes the
        number has to change with it to keep meaning the same physical signal.
        Undo the old correction to recover the raw reading, then apply the new
        one. Idempotent: a profile already on this axis is left alone.
        """
        profile = self._racecontext.race.profile
        want = self.threshold_scale_id()
        have = from_axis if from_axis is not None else self._stored_scale_id(profile)
        if have == want:
            return None, None

        old_eq = (have or {}).get('eq')
        new_eq = want['eq']

        def convert(raw_json):
            try:
                vals = json.loads(raw_json)["v"] if raw_json else []
            except (TypeError, ValueError, KeyError):
                vals = []
            out = []
            for idx in range(self._racecontext.race.num_nodes):
                v = vals[idx] if idx < len(vals) else None
                if not v:
                    out.append(v)
                    continue
                raw = self._uncorrect(int(v), old_eq[idx] if old_eq else None)
                out.append(max(1, int(self._corrected(
                    raw, new_eq[idx] if new_eq else None))))
            return out

        enter_ats = convert(getattr(profile, 'enter_ats', None))
        exit_ats = convert(getattr(profile, 'exit_ats', None))
        self._racecontext.rhdata.alter_profile({
            'profile_id': profile.id,
            'enter_ats': dict(want, v=enter_ats),
            'exit_ats': dict(want, v=exit_ats),
        })
        self._racecontext.race.profile = self._racecontext.rhdata.get_profile(profile.id)
        self.hardware_set_all_enter_ats(enter_ats)
        self.hardware_set_all_exit_ats(exit_ats)
        logger.info('Converted EnterAt/ExitAt onto the new correction: enter=%s',
                    enter_ats)
        return enter_ats, exit_ats

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
    def _eq_signature_key(signature):
        """A comparable form of an equalisation signature.

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
            race, 'eq_signature', None)
        return self._eq_signature_key(stored) == self._eq_signature_key(
            self._eq_signature())

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
                         'equalisation', skipped)
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
    