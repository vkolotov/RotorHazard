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
#  automatically with the ADC width, since a narrower pipeline reports
#  proportionally narrower spans.
#
# The one thing that cannot come from the captures is headroom: "high" is the
#  quad at the gate, not saturation, and a closer pass has to stay on scale.
#  Cap the top of the mapped range at this fraction of full scale.
EQ_HEADROOM_FRACTION = 0.5

# What a node's reading can reach: a byte for the classic pipeline, the 12-bit
#  ADC range where the node reads at full width.
EQ_FULL_SCALE_BYTE = 255
EQ_FULL_SCALE_WIDE = 4095

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

def node_full_resolution(node):
    return bool(getattr(node, 'has_wide_rssi', lambda: False)()) and node.adc_resolution == 12


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

    # --- equalisation -----------------------------------------------------

    def _eq_stored(self, field, default):
        """A stored per-node calibration list, padded to the node count."""
        profile = self._racecontext.race.profile
        raw = getattr(profile, field, None)
        vals = json.loads(raw)["v"] if raw else []
        out = []
        for idx in range(self._racecontext.race.num_nodes):
            v = vals[idx] if idx < len(vals) else None
            out.append(default if v is None else v)
        return out

    def _eq_resolution_matches(self):
        raw = getattr(self._racecontext.race.profile, 'eq_pivots', None)
        stored_bits = json.loads(raw).get('adc_bits') if raw else None
        return stored_bits is None or stored_bits == [
            12 if node_full_resolution(node) else 10
            for node in self._racecontext.interface.nodes]

    def _eq_node_channels(self):
        """The channel label each node is tuned to, one per node."""
        freqs = json.loads(self._racecontext.race.profile.frequencies)
        bands, chans = freqs.get('b') or [], freqs.get('c') or []
        out = []
        for idx in range(self._racecontext.race.num_nodes):
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
            if label not in seen:
                seen.append(label)
                steps.append(('low', label))
                steps.append(('high', label))
        return steps

    def _eq_scale(self, node_index):
        """What a node's corrected reading can reach."""
        node = self._racecontext.interface.nodes[node_index]
        return EQ_FULL_SCALE_WIDE if node_full_resolution(node) else EQ_FULL_SCALE_BYTE

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

        if not captured and any(self._eq_stored('eq_pivots', 0)):
            # already calibrated - do not arm the first step, so a stray click
            #  cannot start overwriting a good calibration
            return {'state': 'applied' if self._eq_resolution_matches() else 'incompatible', 'level': None, 'channel': None,
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
    def eq_wizard_capture(self):
        """Capture the next step: clear the extremes, settle, then read."""
        state = self.eq_wizard_state()
        if state['state'] != 'capturing' or getattr(self, '_eq_busy', False):
            return False

        self._eq_busy = True
        try:
            self._racecontext.rhui.emit_eq_wizard_state()
            self.eq_reset_extremums()
            gevent.sleep(EQ_SETTLE_SECONDS)

            level = state['level']
            key = level if state['channel'] is None \
                else '{0}:{1}'.format(level, state['channel'])
            nodes = self._racecontext.interface.nodes
            vals = []
            for idx in range(self._racecontext.race.num_nodes):
                node = nodes[idx]
                v = node.node_nadir_rssi if level == 'noise' else node.node_peak_rssi
                vals.append(int(v) if v and v < node.max_rssi_value else None)

            if level == 'noise' and any(v is None for v in vals):
                msg = 'Noise capture failed: no reading on node {0}'.format(
                    vals.index(None) + 1)
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False

            self._eq_captured = getattr(self, '_eq_captured', {})
            self._eq_captured[key] = vals
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
        num = self._racecontext.race.num_nodes
        self._eq_store([0] * num, [0] * num, [256] * num, [0] * num, [256] * num)
        for idx in range(num):
            self._racecontext.interface.set_equalisation(idx, 0, 0, 256, 0, 256)
        self.eq_reset_extremums()
        self._racecontext.rhui.emit_eq_wizard_state()
        logger.info('Equalisation cleared')
        return True

    def _eq_store(self, pivots, offset_ups, slope_ups, offset_los, slope_los):
        profile = self._racecontext.race.profile
        self._racecontext.race.profile = self._racecontext.rhdata.alter_profile({
            'profile_id': profile.id,
            'eq_pivots': {"v": pivots, 'adc_bits': [12 if node_full_resolution(node) else 10
                         for node in self._racecontext.interface.nodes]},
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
        levels = []
        for idx in range(num):
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
            # Legacy readings are about eight times smaller than 12-bit raw.
            min_gap = max(1, EQ_MIN_LEVEL_FRACTION * self._eq_scale(idx))
            if (hi - lo) < min_gap or (lo - fl) < min_gap:
                msg = ('Node {0} levels are too close together '
                       '(noise={1}, low={2}, high={3})').format(idx + 1, fl, lo, hi)
                logger.warning(msg)
                self._racecontext.rhui.emit_priority_message(msg)
                return False
            levels.append((fl, lo, hi))

        destination = self._eq_destination([(lo - fl, hi - lo) for fl, lo, hi in levels])
        logger.info('Equalisation destination from captured spans: %s', destination)

        pivots, offset_ups, slope_ups, offset_los, slope_los = [], [], [], [], []
        for idx in range(num):
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

        self._eq_store(pivots, offset_ups, slope_ups, offset_los, slope_los)
        for idx in range(num):
            self._racecontext.interface.set_equalisation(
                idx, pivots[idx], offset_ups[idx], slope_ups[idx],
                offset_los[idx], slope_los[idx])

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

    def eq_reset_extremums(self):
        """Restart peak/nadir tracking on every node.

        The crossing is ended first: a pass peak only updates while a node is
        crossing, so a node still in a crossing re-fills it from the live
        signal the moment after the reset, and then freezes there once the
        crossing ends.
        """
        for idx in range(self._racecontext.race.num_nodes):
            self._racecontext.interface.force_end_crossing(idx)
        for idx in range(self._racecontext.race.num_nodes):
            self._racecontext.interface.reset_node_extremums(idx)

    def hardware_set_all_equalisation(self):
        """Re-send the stored calibration; nodes keep nothing across a power cycle."""
        pivots = self._eq_stored('eq_pivots', 0)
        if not self._eq_resolution_matches():
            pivots = [0] * len(pivots)
        offset_ups = self._eq_stored('eq_offset_ups', 0)
        slope_ups = self._eq_stored('eq_slope_ups', 256)
        offset_los = self._eq_stored('eq_offset_los', 0)
        slope_los = self._eq_stored('eq_slope_los', 256)
        for idx in range(self._racecontext.race.num_nodes):
            self._racecontext.interface.set_equalisation(
                idx, pivots[idx], offset_ups[idx], slope_ups[idx],
                offset_los[idx], slope_los[idx])
        # Let the median filter discard samples from before the coefficients changed.
        gevent.sleep(0.5)
        self.eq_reset_extremums()

    def current_adc_bits(self):
        """The width the nodes are sampling at right now."""
        nodes = self._racecontext.interface.nodes
        if not nodes:
            return None
        return 12 if any(node_full_resolution(node) for node in nodes) else 10

    def rescale_thresholds_for_resolution(self, to_full):
        """Move stored EnterAt/ExitAt onto the new ADC scale.

        The low path clamps the 10-bit reading and halves it, so a reading is
        exactly eight times larger at 12 bits than at 8. Thresholds are
        compared straight against that reading and carry no other scaling, so
        the same factor of eight converts them. Going up is lossless; coming
        back down discards the low three bits, which is the resolution the
        operator asked to give up.

        Equalisation is deliberately left alone - see
        eq_rescale_is_lossy() for why it is re-run rather than converted.
        """
        profile = self._racecontext.race.profile
        node_count = self._racecontext.race.num_nodes

        def convert(raw):
            vals = json.loads(raw)["v"] if raw else []
            out = []
            for idx in range(node_count):
                v = vals[idx] if idx < len(vals) else None
                if not v:
                    out.append(v)
                elif to_full:
                    out.append(int(v) * 8)
                else:
                    out.append(max(1, int(v) // 8))
            return out

        enter_ats = convert(getattr(profile, 'enter_ats', None))
        exit_ats = convert(getattr(profile, 'exit_ats', None))
        bits = 12 if to_full else 10
        self._racecontext.rhdata.alter_profile({
            'profile_id': profile.id,
            'enter_ats': {"v": enter_ats, 'adc_bits': bits},
            'exit_ats': {"v": exit_ats, 'adc_bits': bits},
        })
        self._racecontext.race.profile = self._racecontext.rhdata.get_profile(profile.id)
        self.hardware_set_all_enter_ats(enter_ats)
        self.hardware_set_all_exit_ats(exit_ats)
        logger.info("Rescaled EnterAt/ExitAt for %s-bit sampling: enter=%s",
                    bits, enter_ats)
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

    def _race_matches_resolution(self, race):
        """True if this saved race was timed at the width now in use.

        Races saved before the width became switchable carry no tag; they are
        8-bit, because that is all the firmware of the time could produce.
        """
        current = self.current_adc_bits()
        if current is None:
            return True
        tagged = self._racecontext.rhdata.get_savedrace_attribute_value(
            race, 'adc_bits', None)
        stored = int(tagged) if tagged else 10
        return stored == current

    def find_best_calibration_values(self, node, seat_index):
        ''' Search race history for best tuning values '''

        # get commonly used values
        heat = self._racecontext.rhdata.get_heat(self._racecontext.race.current_heat)
        pilot = self._racecontext.rhdata.get_pilot_from_heatNode(self._racecontext.race.current_heat, seat_index)
        current_class = heat.class_id
        races = self._racecontext.rhdata.get_savedRaceMetas()
        races.sort(key=lambda x: x.id, reverse=True)
        # Drop races timed at the other ADC width; their thresholds are eight
        #  times off and would put every node permanently in or out of crossing.
        usable_race_ids = set()
        skipped = 0
        for race in list(races):
            if self._race_matches_resolution(race):
                usable_race_ids.add(race.id)
            else:
                races.remove(race)
                skipped += 1
        if skipped:
            logger.debug('Ignoring %d saved race(s) recorded at a different ADC width', skipped)
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
    