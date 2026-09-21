'''Seat calibration adjustment'''

import logging
import json
import RHUtils
from eventmanager import Evt
from RHUtils import catchLogExceptionsWrapper
from filtermanager import Flt

logger = logging.getLogger(__name__)

EQUALISED_FLOOR = 50

# Targets for the piecewise fit. The PIT level maps to PIVOT_TARGET and the
#  race level to RACE_TARGET - that upper segment is the accurate one and is
#  where crossing detection operates. FLOOR_TARGET only keeps the idle trace
#  off zero so an operator can see the node is alive; nothing depends on it.
PIVOT_TARGET = 300
RACE_TARGET = 800
FLOOR_TARGET = 30

# Minimum separation between adjacent calibration levels. A node that missed
#  the quad reads only noise and gives a span near zero; a node high on its
#  detector curve legitimately compresses to ~190, so this is deliberately
#  looser than MIN_VALID_SPAN.
MIN_LEVEL_SEPARATION = 60

# How long to watch the nodes after resetting the trackers, before reading
#  the captured extreme. Long enough for the RX to settle and for a peak to
#  build, short enough not to be tedious.
EQ_SETTLE_SECONDS = 5.0

# A node that never saw the quad reports only noise. Its span would be tiny,
#  which turns into an absurd scale factor, so reject it loudly instead.
MIN_VALID_SPAN = 200

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

    @catchLogExceptionsWrapper
    def set_floor_offset(self, seat_index, offset_input):
        '''Set per-node RSSI equalisation floor offset.'''
        self._set_eq_value(seat_index, 'floor_offsets', int(offset_input or 0),
                           self._racecontext.interface.set_floor_offset, 'floor offset')
    @catchLogExceptionsWrapper
    def set_scale_factor(self, seat_index, factor_input):
        '''Set per-node RSSI equalisation scale factor (Q8; 256 = 1.0).'''
        factor = int(factor_input) if factor_input else 256
        factor = max(1, min(65535, factor))
        self._set_eq_value(seat_index, 'scale_factors', factor,
                           self._racecontext.interface.set_scale_factor, 'scale factor')
    def _set_eq_value(self, seat_index, field, value, hw_setter, label):
        '''Shared persist-then-transmit for the equalisation constants.'''
        if seat_index < 0 or seat_index >= self._racecontext.race.num_nodes:
            logger.info('Unable to set {0} ({1}) on node {2}; node index out of range'.format(
                label, value, seat_index+1))
            return

        profile = self._racecontext.race.profile
        stored = getattr(profile, field, None)
        values = json.loads(stored) if stored else {"v": []}

        while seat_index >= len(values["v"]):
            values["v"].append(None)

        values["v"][seat_index] = value

        profile = self._racecontext.rhdata.alter_profile({
            'profile_id': profile.id,
            field: values
            })
        self._racecontext.race.profile = profile

        hw_setter(seat_index, value)
        self._racecontext.rhui.emit_equalisation_values()

        logger.info('Node {0} set: Node {1} Value {2}'.format(label, seat_index+1, value))
    def hardware_set_all_equalisation(self):
        '''Send stored equalisation constants to all nodes.'''
        profile = self._racecontext.race.profile
        raw_off = getattr(profile, 'floor_offsets', None)
        raw_fac = getattr(profile, 'scale_factors', None)
        offsets = json.loads(raw_off) if raw_off else {"v": []}
        factors = json.loads(raw_fac) if raw_fac else {"v": []}
        for idx in range(self._racecontext.race.num_nodes):
            off = offsets["v"][idx] if idx < len(offsets["v"]) and offsets["v"][idx] is not None else 0
            fac = factors["v"][idx] if idx < len(factors["v"]) and factors["v"][idx] is not None else 256
            self._racecontext.interface.set_floor_offset(idx, off)
            self._racecontext.interface.set_scale_factor(idx, fac)
        logger.debug("Sent equalisation values to nodes: offsets={0} factors={1}".format(
            offsets.get("v"), factors.get("v")))
    def _stored_eq(self, field, default):
        """Read a stored per-node equalisation list, padded to num_nodes."""
        profile = self._racecontext.race.profile
        raw = getattr(profile, field, None)
        vals = json.loads(raw)["v"] if raw else []
        out = []
        for idx in range(self._racecontext.race.num_nodes):
            v = vals[idx] if idx < len(vals) else None
            out.append(default if v is None else v)
        return out
    def eq_wizard_state(self):
        """Current wizard position: the next step, or None when complete.

        The sequence is one noise capture (VTX off, no quad needed) followed by
        a low/high pair for every distinct node channel. Channels come from the
        nodes' own frequencies, so the wizard follows whatever assignment is in
        use rather than assuming a fixed band.
        """
        cap = getattr(self, '_eq_capture', None) or {}
        busy = getattr(self, '_eq_busy', False)

        # Nothing captured but constants already on the nodes: the wizard has
        #  been run and applied. Report that rather than arming the first step,
        #  so a stray click cannot start overwriting a good calibration.
        if not cap and any(p for p in self._stored_eq('eq_pivots', 0)):
            return {'done': False, 'applied': True, 'level': None, 'channel': None,
                    'key': None, 'index': 0, 'total': 0, 'busy': busy,
                    'settle': EQ_SETTLE_SECONDS}

        steps = [('noise', None)]
        for label in self._eq_channel_labels():
            steps.append(('low', label))
            steps.append(('high', label))
        for level, chan in steps:
            key = level if chan is None else '{0}:{1}'.format(level, chan)
            if key not in cap:
                return {'done': False, 'applied': False, 'level': level, 'channel': chan, 'key': key,
                        'index': len(cap), 'total': len(steps), 'busy': busy,
                        'settle': EQ_SETTLE_SECONDS}
        return {'done': True, 'applied': False, 'level': None, 'channel': None, 'key': None,
                'index': len(steps), 'total': len(steps), 'busy': busy,
                'settle': EQ_SETTLE_SECONDS}
    def eq_capture_table(self):
        """Per-node view for the UI.

        While a capture is in progress this shows the raw readings recorded so
        far. Once applied the capture is discarded, so fall back to the stored
        constants - otherwise a restart leaves the readout blank even though
        the correction is live on the nodes.
        """
        cap = getattr(self, '_eq_capture', None) or {}
        num = self._racecontext.race.num_nodes
        labels = self._eq_node_labels()

        if cap:
            noise = cap.get('noise', [None] * num)
            rows = []
            for idx in range(num):
                label = labels[idx]
                rows.append({
                    'channel': label,
                    'mode': 'capture',
                    'noise': noise[idx],
                    'low': cap.get('low:{0}'.format(label), [None] * num)[idx],
                    'high': cap.get('high:{0}'.format(label), [None] * num)[idx],
                })
            return rows

        pivots = self._stored_eq('eq_pivots', 0)
        kups = self._stored_eq('eq_kups', 256)
        klos = self._stored_eq('eq_klos', 256)
        rows = []
        for idx in range(num):
            applied = bool(pivots[idx])
            rows.append({
                'channel': labels[idx],
                'mode': 'applied' if applied else 'empty',
                'pivot': pivots[idx] if applied else None,
                'kup': kups[idx] if applied else None,
                'klo': klos[idx] if applied else None,
            })
        return rows
    @catchLogExceptionsWrapper
    def eq_wizard_capture(self):
        """Capture the next step: reset, settle, then read.

        The reset has to happen AFTER the operator has set up the condition,
        not before. A node peak only ever rises, so if the trackers were reset
        at the end of the previous step they would already hold whatever the
        VTX was doing while the channel was being changed - typically still at
        race power - and a later low reading could never pull them back down.
        Resetting here, then waiting, means the captured value can only come
        from the condition that is set up right now.
        """
        state = self.eq_wizard_state()
        if state['done']:
            self._racecontext.rhui.emit_priority_message('Calibration already complete')
            return False

        if getattr(self, '_eq_busy', False):
            return False
        self._eq_busy = True
        try:
            self._racecontext.rhui.emit_eq_wizard_state()

            # discard whatever the trackers hold from the setup fiddling
            self.reset_node_extremums()
            gevent.sleep(EQ_SETTLE_SECONDS)

            nodes = self._racecontext.interface.nodes
            num = self._racecontext.race.num_nodes
            level, key = state['level'], state['key']
            vals = []
            for idx in range(num):
                node = nodes[idx]
                v = node.node_nadir_rssi if level == 'noise' else node.node_peak_rssi
                vals.append(int(v) if v and v < node.max_rssi_value else None)

            if level == 'noise' and any(v is None for v in vals):
                msg = 'Noise capture failed: no reading on node {0}'.format(
                    vals.index(None) + 1)
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False

            self._eq_capture = getattr(self, '_eq_capture', {})
            self._eq_capture[key] = vals
            logger.info('Wizard captured %s: %s', key, vals)
            return True
        finally:
            self._eq_busy = False
            self._racecontext.rhui.emit_eq_wizard_state()
    @catchLogExceptionsWrapper
    def eq_wizard_back(self):
        """Discard the most recently captured step and return to it."""
        cap = getattr(self, '_eq_capture', None) or {}
        if not cap:
            self._racecontext.rhui.emit_priority_message('Nothing to undo')
            return False

        steps = ['noise']
        for label in self._eq_channel_labels():
            steps.append('low:{0}'.format(label))
            steps.append('high:{0}'.format(label))
        captured = [k for k in steps if k in cap]
        last = captured[-1]
        del cap[last]
        logger.info('Wizard stepped back, discarded %s', last)

        self.reset_node_extremums()
        self._eq_last_reset = monotonic()
        self._racecontext.rhui.emit_eq_wizard_state()
        return True
    @catchLogExceptionsWrapper
    def eq_wizard_reset(self):
        """Clear the calibration entirely and arm the wizard from the start.

        This drops the applied constants as well as any part-finished
        capture - the readings are only meaningful against uncorrected
        values, so a fresh run has to begin from raw.
        """
        self._eq_capture = {}
        num = self._racecontext.race.num_nodes
        profile = self._racecontext.race.profile
        profile = self._racecontext.rhdata.alter_profile({
            'profile_id': profile.id,
            'eq_pivots': {"v": [0] * num},
            'eq_kups': {"v": [256] * num},
            'eq_klos': {"v": [256] * num},
            })
        self._racecontext.race.profile = profile
        for idx in range(num):
            self._racecontext.interface.set_eq_piecewise(idx, 0, 256, 256)

        self.reset_node_extremums()
        self._eq_last_reset = monotonic()
        self._racecontext.rhui.emit_equalisation_values()
        self._racecontext.rhui.emit_eq_wizard_state()
        logger.info('Equalisation cleared, wizard armed')
        return True
    @catchLogExceptionsWrapper
    def eq_wizard_apply(self):
        """Fit and apply once every step has been captured.

        Each node takes its levels from the channel it is tuned to, so a sweep
        that visits every channel gives every node an on-channel measurement.
        """
        if not self.eq_wizard_state()['done']:
            self._racecontext.rhui.emit_priority_message('Calibration not finished')
            return False

        cap = self._eq_capture
        num = self._racecontext.race.num_nodes
        labels = self._eq_channel_labels()
        node_labels = self._eq_node_labels()
        noise = cap['noise']

        pivots, kups, klos = [], [], []
        for idx in range(num):
            label = node_labels[idx]
            lo = cap.get('low:{0}'.format(label), [None] * num)[idx]
            hi = cap.get('high:{0}'.format(label), [None] * num)[idx]
            f = noise[idx]
            if lo is None or hi is None:
                msg = 'Node {0} has no reading on its own channel ({1})'.format(idx+1, label)
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False
            if (hi - lo) < MIN_LEVEL_SEPARATION or (lo - f) < MIN_LEVEL_SEPARATION:
                msg = ('Node {0} levels are too close together '
                       '(noise={1}, low={2}, high={3})').format(idx+1, f, lo, hi)
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False
            pivots.append(lo)
            kups.append(max(1, min(65535, int(round((RACE_TARGET - PIVOT_TARGET) * 256.0 / (hi - lo))))))
            klos.append(max(1, min(65535, int(round((PIVOT_TARGET - FLOOR_TARGET) * 256.0 / (lo - f))))))

        profile = self._racecontext.race.profile
        profile = self._racecontext.rhdata.alter_profile({
            'profile_id': profile.id,
            'eq_pivots': {"v": pivots},
            'eq_kups': {"v": kups},
            'eq_klos': {"v": klos},
            })
        self._racecontext.race.profile = profile

        for idx in range(num):
            self._racecontext.interface.set_eq_piecewise(idx, pivots[idx], kups[idx], klos[idx])

        gevent.sleep(0.5)
        self.reset_node_extremums()
        gevent.sleep(0.5)
        self.reset_node_extremums()

        self._eq_capture = {}
        self._racecontext.rhui.emit_equalisation_values()
        self._racecontext.rhui.emit_eq_wizard_state()
        logger.info('Equalisation applied: pivots=%s kups=%s klos=%s', pivots, kups, klos)
        self._racecontext.rhui.emit_priority_message(
            'Equalisation applied to {0} nodes'.format(num))
        return True
    def _eq_channel_labels(self):
        """Distinct node channels, in node order, as 'R1'-style labels."""
        labels = []
        for label in self._eq_node_labels():
            if label not in labels:
                labels.append(label)
        return labels
    def _eq_node_labels(self):
        """The channel label each node is tuned to, one per node."""
        profile = self._racecontext.race.profile
        freqs = json.loads(profile.frequencies)
        out = []
        for idx in range(self._racecontext.race.num_nodes):
            band = freqs.get('b', [])[idx] if idx < len(freqs.get('b', []) or []) else None
            chan = freqs.get('c', [])[idx] if idx < len(freqs.get('c', []) or []) else None
            out.append('{0}{1}'.format(band, chan) if band and chan else 'N{0}'.format(idx+1))
        return out
    @catchLogExceptionsWrapper
    def capture_eq_level(self, level):
        """Record the current per-node readings as one of the calibration levels.

        level is 'floor', 'pit' or 'race'. Readings are taken from the tracked
        extremes: the nadir for 'floor' (lowest seen) and the peak otherwise
        (highest seen), so a channel sweep can be captured in one pass.
        """
        if level not in ('floor', 'pit', 'race'):
            logger.warning('Unknown equalisation level: %s', level)
            return False

        nodes = self._racecontext.interface.nodes
        num = self._racecontext.race.num_nodes
        vals = []
        for idx in range(num):
            node = nodes[idx]
            v = node.node_nadir_rssi if level == 'floor' else node.node_peak_rssi
            if not v or v >= node.max_rssi_value:
                msg = 'Capture failed: node {0} has no valid {1} reading ({2})'.format(
                    idx+1, level, v)
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False
            vals.append(int(v))

        self._eq_capture = getattr(self, '_eq_capture', {})
        self._eq_capture[level] = vals
        logger.info('Captured %s level: %s', level, vals)
        self._racecontext.rhui.emit_priority_message(
            '{0} level captured'.format(level.capitalize()))
        return True
    @catchLogExceptionsWrapper
    def apply_eq_piecewise(self):
        """Fit the two-segment correction from the three captured levels.

        Above the pivot the fit is anchored on two real signal levels (PIT and
        race), which is the region crossing detection uses. Below it the slope
        is chosen only so the idle reading lands near FLOOR_TARGET instead of
        clamping to zero - the noise floor carries nothing useful, but a trace
        pinned flat at 0 reads as a broken node.
        """
        cap = getattr(self, '_eq_capture', {})
        missing = [l for l in ('floor', 'pit', 'race') if l not in cap]
        if missing:
            msg = 'Cannot apply: still need {0}'.format(', '.join(missing))
            logger.warning(msg)
            self._racecontext.rhui.emit_priority_message(msg)
            return False

        num = self._racecontext.race.num_nodes
        F, P, R = cap['floor'], cap['pit'], cap['race']
        pivots, kups, klos = [], [], []
        for idx in range(num):
            up_span = R[idx] - P[idx]
            lo_span = P[idx] - F[idx]
            if up_span < MIN_LEVEL_SEPARATION or lo_span < MIN_LEVEL_SEPARATION:
                msg = ('Cannot apply: node {0} levels are too close together '
                       '(floor={1}, pit={2}, race={3})').format(idx+1, F[idx], P[idx], R[idx])
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False
            kup = int(round((RACE_TARGET - PIVOT_TARGET) * 256.0 / up_span))
            klo = int(round((PIVOT_TARGET - FLOOR_TARGET) * 256.0 / lo_span))
            pivots.append(P[idx])
            kups.append(max(1, min(65535, kup)))
            klos.append(max(1, min(65535, klo)))

        profile = self._racecontext.race.profile
        profile = self._racecontext.rhdata.alter_profile({
            'profile_id': profile.id,
            'eq_pivots': {"v": pivots},
            'eq_kups': {"v": kups},
            'eq_klos': {"v": klos},
            })
        self._racecontext.race.profile = profile

        for idx in range(num):
            self._racecontext.interface.set_eq_piecewise(
                idx, pivots[idx], kups[idx], klos[idx])

        gevent.sleep(0.5)
        self.reset_node_extremums()
        gevent.sleep(0.5)
        self.reset_node_extremums()
        self._racecontext.rhui.emit_equalisation_values()

        logger.info('Applied piecewise equalisation: pivots=%s kups=%s klos=%s',
                    pivots, kups, klos)
        self._racecontext.rhui.emit_priority_message(
            'Equalisation applied to {0} nodes'.format(num))
        return True
    def hardware_set_all_eq_piecewise(self):
        """Re-send the stored piecewise constants to every node."""
        pivots = self._stored_eq('eq_pivots', 0)
        kups = self._stored_eq('eq_kups', 256)
        klos = self._stored_eq('eq_klos', 256)
        for idx in range(self._racecontext.race.num_nodes):
            self._racecontext.interface.set_eq_piecewise(
                idx, pivots[idx], kups[idx], klos[idx])
        logger.debug('Sent piecewise equalisation to nodes: pivots=%s kups=%s klos=%s',
                     pivots, kups, klos)
    @catchLogExceptionsWrapper
    def calibrate_floor(self):
        """Set only the floor offsets, from the captured nadirs.

        Run with the VTX powered off. The noise floor drifts with temperature
        and time, so this is the part worth redoing often - and it needs no
        quad, so it is cheap. Scale factors are left untouched.
        """
        nodes = self._racecontext.interface.nodes
        num = self._racecontext.race.num_nodes
        offsets = self._stored_eq('floor_offsets', 0)
        factors = self._stored_eq('scale_factors', 256)

        for idx in range(num):
            nadir = nodes[idx].node_nadir_rssi
            if not nadir or nadir >= nodes[idx].max_rssi_value:
                msg = 'Floor calibration failed: node {0} has no valid nadir ({1})'.format(
                    idx+1, nadir)
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False
            # the reported nadir is already corrected; shift the stored offset by
            #  however far it sits from the target, in raw counts
            drift_raw = (nadir - EQUALISED_FLOOR) * 256.0 / factors[idx]
            self.set_floor_offset(idx, int(round(offsets[idx] + drift_raw)))

        gevent.sleep(0.5)
        self.reset_node_extremums()
        gevent.sleep(0.5)
        self.reset_node_extremums()

        logger.info('Floor calibrated on {0} nodes to {1}'.format(num, EQUALISED_FLOOR))
        self._racecontext.rhui.emit_priority_message(
            'Floor calibrated on {0} nodes'.format(num))
        return True
    @catchLogExceptionsWrapper
    def calibrate_scale(self):
        """Set only the scale factors, from the captured peaks.

        Run after stepping the quad through every channel. The floor offsets
        are kept - gain is a property of the receiver and drifts far less than
        the noise floor, so the two are worth calibrating separately.
        """
        nodes = self._racecontext.interface.nodes
        num = self._racecontext.race.num_nodes
        offsets = self._stored_eq('floor_offsets', 0)
        factors = self._stored_eq('scale_factors', 256)

        # the nodes report corrected values, so a span is simply the distance
        #  above the known corrected floor
        spans = []
        for idx in range(num):
            span = nodes[idx].node_peak_rssi - EQUALISED_FLOOR
            if span < MIN_VALID_SPAN:
                msg = ('Scale calibration failed: node {0} span {1} is too small - '
                       'it did not see the quad').format(idx+1, span)
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False
            spans.append(span)

        ordered = sorted(spans)
        mid = len(ordered) // 2
        median_span = ordered[mid] if len(ordered) % 2 else (ordered[mid-1] + ordered[mid]) / 2.0

        for idx in range(num):
            # adjust the existing gain by the ratio rather than recomputing it
            factor = int(round(factors[idx] * median_span / spans[idx]))
            factor = max(1, min(65535, factor))

            # the floor sits EQUALISED_FLOOR above the raw noise level, and that
            #  gap is scaled too, so the offset has to be restated for the new gain
            nadir_raw = offsets[idx] + (EQUALISED_FLOOR * 256.0 / factors[idx])
            offset = int(round(nadir_raw - (EQUALISED_FLOOR * 256.0 / factor)))

            self.set_scale_factor(idx, factor)
            self.set_floor_offset(idx, offset)

        gevent.sleep(0.5)
        self.reset_node_extremums()
        gevent.sleep(0.5)
        self.reset_node_extremums()

        logger.info('Scale calibrated on {0} nodes to median span {1:.0f}'.format(num, median_span))
        self._racecontext.rhui.emit_priority_message(
            'Scale calibrated on {0} nodes to a common span of {1:.0f}'.format(num, median_span))
        return True
    @catchLogExceptionsWrapper
    def equalise_nodes(self):
        '''Compute per-node equalisation from the captured nodeNadir/nodePeak.'''
        nodes = self._racecontext.interface.nodes
        num = self._racecontext.race.num_nodes
        spans = []
        for idx in range(num):
            node = nodes[idx]
            nadir = node.node_nadir_rssi
            peak = node.node_peak_rssi
            if not peak or not nadir or (peak - nadir) < MIN_VALID_SPAN:
                msg = ('Equalise failed: node {0} span {1} is too small - it did not '
                       'see the quad (nadir={2}, peak={3})').format(
                    idx+1, peak - nadir, nadir, peak)
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False
            spans.append(peak - nadir)

        ordered = sorted(spans)
        mid = len(ordered) // 2
        median_span = ordered[mid] if len(ordered) % 2 else (ordered[mid-1] + ordered[mid]) // 2

        for idx in range(num):
            nadir = nodes[idx].node_nadir_rssi
            factor = int(round(median_span * 256.0 / spans[idx]))
            factor = max(1, min(65535, factor))
            # Land the corrected floor on EQUALISED_FLOOR rather than 0.
            #  rssiRead() clamps negatives to 0, so a floor of 0 truncates the
            #  lower half of the noise distribution - that biases the mean up and
            #  hides genuine dips below baseline. Offsetting by a few sigma keeps
            #  the jitter intact and symmetric.
            offset = int(round(nadir - (EQUALISED_FLOOR * 256.0 / factor)))
            self.set_floor_offset(idx, offset)
            self.set_scale_factor(idx, factor)

        # the tracked extremes were measured before correction, so they no longer
        #  describe what the nodes now report - clear them so the displayed
        #  peak/nadir rebuild on the equalised scale.
        # Writing the constants above takes many serial round-trips (each one
        #  writes then reads back to verify). The update thread keeps polling
        #  throughout, so a poll issued before the reset can land after it and
        #  restore the stale peak. Let the in-flight polls drain first, then
        #  reset, then drain and reset once more to close the window.
        gevent.sleep(0.5)
        self.reset_node_extremums()
        gevent.sleep(0.5)
        self.reset_node_extremums()

        logger.info('Equalised {0} nodes to median span {1}'.format(num, median_span))
        self._racecontext.rhui.emit_priority_message(
            'Equalised {0} nodes to a common span of {1}'.format(num, median_span))
        return True
    @catchLogExceptionsWrapper
    def reset_equalisation(self):
        '''Clear all equalisation constants back to the identity (no correction).'''
        num = self._racecontext.race.num_nodes
        for idx in range(num):
            self.set_floor_offset(idx, 0)
            self.set_scale_factor(idx, 256)

        # readings revert to raw, so the tracked extremes no longer apply
        self.reset_node_extremums()

        logger.info('Equalisation reset to identity on {0} nodes'.format(num))
        self._racecontext.rhui.emit_priority_message(
            'Equalisation cleared on {0} nodes'.format(num))
        return True
    def reset_node_extremums(self):
        '''Restart per-node peak/nadir tracking.

        The extremes are tracked on the node, and a peak only ever rises, so
        clearing the server-side copy alone is useless - the node overwrites it
        on the next update. The reset has to be sent to the hardware.
        '''
        for idx in range(self._racecontext.race.num_nodes):
            self._racecontext.interface.reset_node_extremums(idx)
        logger.info('Node peak/nadir tracking reset')

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

    def find_best_calibration_values(self, node, seat_index):
        ''' Search race history for best tuning values '''

        # get commonly used values
        heat = self._racecontext.rhdata.get_heat(self._racecontext.race.current_heat)
        pilot = self._racecontext.rhdata.get_pilot_from_heatNode(self._racecontext.race.current_heat, seat_index)
        current_class = heat.class_id
        races = self._racecontext.rhdata.get_savedRaceMetas()
        races.sort(key=lambda x: x.id, reverse=True)
        pilotRaces = self._racecontext.rhdata.get_savedPilotRaces()
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
    