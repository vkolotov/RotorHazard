/* Run with: node --test src/tests/test_norm_wizard_ui.js */
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const {test} = require('node:test');
const vm = require('node:vm');
const path = require('node:path');

function wizard() {
    const template = readFileSync(path.join(__dirname, '../server/templates/settings.html'), 'utf8');
    const start = template.indexOf("/* Normalisation scope selection */");
    const end = template.indexOf("$('#norm_apply_thresholds').click", start);
    const script = template.slice(start, end).replace(/\{\{ __\('([^']*)'\) \}\}/g, '$1');
    const elements = new Map();
    function $(selector) {
        if (typeof selector !== 'string') return selector;
        const items = selector.split(',').map(id => {
            id = id.trim();
            if (!elements.has(id)) elements.set(id, {visible: false, props: {}, data: {}, attrs: {}, classes: new Set()});
            return elements.get(id);
        });
        const api = {
            hide() { return this.toggle(false); },
            show() { return this.toggle(true); },
            toggle(value) { items.forEach(el => el.visible = value); return api; },
            prop(key, value) { items.forEach(el => el.props[key] = value); return api; },
            text(value) { items.forEach(el => el.text = value); return api; },
            val(value) { items.forEach(el => el.value = value); return api; },
            attr(key, value) { items.forEach(el => el.attrs[key] = value); return api; },
            toggleClass(key, value) { items.forEach(el => value ? el.classes.add(key) : el.classes.delete(key)); return api; },
            click(fn) { items.forEach(el => el.click = fn); return api; },
            data(key, value) { items.forEach(el => el.data[key] = value); return api; },
            is() { return false; }
        };
        return api;
    }
    $.each = (values, fn) => Object.entries(values).forEach(([key, value]) => fn(key, value));
    let render;
    const emitted = [];
    vm.runInNewContext(script, {
        $, socket: {on(event, fn) { render = fn; }, emit(event, data) { emitted.push({event, data: JSON.parse(JSON.stringify(data))}); }}, window: {},
        setInterval: () => 1, clearInterval: () => {}
    });
    return {
        render(msg) { render({index: 0, total: 16, busy: false, sweeping: false, ...msg}); },
        emitted,
        click(id) {
            const element = elements.get('#' + id);
            if (!element.props.disabled) element.click.call($('#' + id));
        },
        element(id) { return elements.get('#' + id); },
        controls() {
            return [...elements.entries()].filter(([id, el]) =>
                el.visible && id !== '#norm_threshold_controls' && id !== '#norm_wizard_progress')
                .map(([id]) => id.slice(1)).sort();
        }
    };
}

test('scope selection and noise capture show only relevant controls', () => {
    const ui = wizard();
    ui.render({state: 'choosing'});
    assert.deepEqual(ui.controls(), ['norm_scope_selection', 'norm_scope_start']);
    ui.render({state: 'capturing', level: 'noise', channel: 'R1'});
    assert.deepEqual(ui.controls(), ['norm_capture_pass', 'norm_wizard_reset']);
});

test('completed noise sweep replaces Cancel with HIGH capture', () => {
    const ui = wizard();
    ui.render({state: 'capturing', level: 'noise', channel: 'R8', index: 7, sweeping: true, busy: true});
    assert.deepEqual(ui.controls(), ['norm_cancel_pass']);
    ui.render({state: 'capturing', level: 'high', channel: 'R1', index: 8});
    assert.deepEqual(ui.controls(), ['norm_vtx_switch', 'norm_wizard_action',
        'norm_wizard_back', 'norm_wizard_reset']);
    assert.equal(ui.element('norm_wizard_action').text, 'Capture: R1');
    assert.equal(ui.element('norm_wizard_action').props.disabled, false);
    assert.equal(ui.element('norm_wizard_action').data.act, 'capture');
});

test('cancelled or failed sweeps restore noise capture controls', () => {
    for (const index of [0, 1]) {
        const ui = wizard();
        ui.render({state: 'capturing', level: 'noise', sweeping: true, busy: true});
        ui.render({state: 'capturing', level: 'noise', index});
        assert.equal(ui.element('norm_cancel_pass').visible, false);
        assert.equal(ui.element('norm_capture_pass').visible, true);
        assert.equal(ui.element('norm_wizard_back').visible, index > 0);
    }
});

test('ready and applied states refresh fitted values and thresholds; reset clears them', () => {
    const ui = wizard();
    const fit = {captured: [{pivot: 130, offset: 20, scale: 320}],
        suggested_enter_at: 150, suggested_exit_at: 140, normalised_channels: ['R1', 'R2']};
    for (const state of ['ready', 'applied']) {
        ui.render({state, ...fit});
        assert.equal(ui.element('norm_pivot_0').text, 130);
        assert.equal(ui.element('norm_scale_x_0').text, '×1.25');
        assert.equal(ui.element('norm_suggest_enter_at').value, 150);
        assert.equal(ui.element('norm_apply_thresholds').props.disabled, false);
        assert.equal(ui.element('norm_threshold_controls').visible, true);
    }
    assert.equal(ui.element('norm_wizard_action').visible, false);
    assert.equal(ui.element('norm_wizard_progress').text, 'Normalised: R1, R2');
    assert.deepEqual(ui.controls(), ['norm_wizard_reset']);
    ui.render({state: 'choosing', captured: [{pivot: 0, offset: 0, scale: 256}]});
    assert.equal(ui.element('norm_pivot_0').text, '–');
    assert.equal(ui.element('norm_threshold_controls').visible, false);
    assert.equal(ui.element('norm_suggest_enter_at').value, '');
});


test('progress describes the current channel, level, step and VTX setup', () => {
    const ui = wizard();
    ui.render({state: 'capturing', level: 'noise', channel: 'R1'});
    assert.equal(ui.element('norm_capture_pass').text, 'Capture noise (VTX off)');
    assert.equal(ui.element('norm_wizard_progress').text, 'Capture noise (0/16) — VTX off');
    ui.render({state: 'capturing', level: 'noise', channel: 'R1', busy: true, sweeping: true});
    assert.equal(ui.element('norm_wizard_progress').text, 'Capturing: R1 noise (1/17) — VTX off');
    ui.render({state: 'capturing', level: 'noise', channel: 'R8', index: 7, busy: true, sweeping: true});
    assert.equal(ui.element('norm_wizard_progress').text, 'Capturing: R8 noise (8/17) — VTX off');
    ui.render({state: 'capturing', level: 'high', channel: 'R1', index: 8});
    assert.equal(ui.element('norm_wizard_progress').text, 'Capturing: R1 high (9/17) — VTX on, quad at gate');
    ui.render({state: 'capturing', level: 'high', channel: 'R1', index: 8, busy: true});
    assert.equal(ui.element('norm_wizard_progress').text, 'Capturing: R1 high (9/17) — VTX on, quad at gate');
    ui.render({state: 'ready', index: 16});
    assert.equal(ui.element('norm_wizard_progress').text, 'Apply normalisation (17/17)');
    ui.render({state: 'ready', index: 16, busy: true});
    assert.equal(ui.element('norm_wizard_progress').text, 'Applying normalisation (17/17)');
    ui.render({state: 'capturing', level: 'noise', channel: 'R1'});
    assert.equal(ui.element('norm_wizard_progress').text, 'Capture noise (0/16) — VTX off');
});


test('scope buttons toggle independently; only Start sends the combined selection', () => {
    const ui = wizard();
    ui.render({state: 'choosing'});
    assert.equal(ui.element('norm_scope_current').attrs['aria-pressed'], 'true');
    ui.click('norm_scope_current');
    assert.equal(ui.element('norm_scope_start').props.disabled, true);
    ui.click('norm_scope_start');
    assert.equal(ui.emitted.length, 0);
    ui.click('norm_scope_r');
    ui.click('norm_scope_l');
    ui.click('norm_scope_a');
    ui.click('norm_scope_a');
    assert.equal(ui.element('norm_scope_r').attrs['aria-pressed'], 'true');
    assert.equal(ui.element('norm_scope_l').classes.has('selected'), true);
    assert.equal(ui.element('norm_scope_a').attrs['aria-pressed'], 'false');
    assert.equal(ui.emitted.length, 0);
    ui.click('norm_scope_start');
    assert.deepEqual(ui.emitted, [{event: 'norm_start', data: {scope: ['R', 'L']}}]);
    assert.equal(ui.element('norm_scope_start').props.disabled, true);
    ui.render({state: 'capturing', level: 'noise', channel: 'R1', scope: ['R', 'L']});
    assert.equal(ui.element('norm_scope_selection').visible, false);
    ui.render({state: 'choosing'});
    assert.equal(ui.element('norm_scope_r').attrs['aria-pressed'], 'true');
    assert.equal(ui.element('norm_scope_l').attrs['aria-pressed'], 'true');
    assert.equal(ui.element('norm_scope_start').props.disabled, false);
});
