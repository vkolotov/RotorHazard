#!/usr/bin/env python3
"""Re-check the undocumented PHY facts the C5 node firmware relies on, against
the libraries a build actually links, and that static data stays clear of the
capture bank. Run after `idf.py build` (or point it at any libphy.a):

    python tools/check_phy.py --objdump riscv32-esp-elf-objdump \\
        --libphy $IDF_PATH/components/esp_phy/lib/esp32c5/libphy.a \\
        [--librftest .../librftest.a] [--elf build/rhnode_esp32c5.elf]

Every check asserts an expected instruction or value; a missing function or
an unexpected pattern fails. A different ESP-IDF or PHY library version that
moved these fields would otherwise retune the radio wrongly without an error.
"""
import argparse
import re
import subprocess
import sys

BANK_START = 0x40820000
BANK_MARGIN = 0x800   # keep at least 2 KB clear below the bank


def disasm(objdump, lib):
    out = subprocess.run([objdump, '-d', '-r', lib], capture_output=True, text=True, check=True).stdout
    funcs, cur = {}, None
    for line in out.splitlines():
        m = re.match(r'^[0-9a-f]+ <([^>]+)>:$', line)
        if m and m.group(1).startswith('.L'):
            continue   # a local label inside the current function
        if m:
            cur = m.group(1)
            funcs[cur] = []
        elif cur and line.strip():
            funcs[cur].append(line)
    return funcs


def text(funcs, name):
    if name not in funcs:
        raise AssertionError('%s not found' % name)
    return '\n'.join(funcs[name])


def expect(cond, what, failures):
    print(('ok    ' if cond else 'FAIL  ') + what)
    if not cond:
        failures.append(what)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--objdump', required=True)
    ap.add_argument('--nm')
    ap.add_argument('--libphy', required=True)
    ap.add_argument('--librftest')
    ap.add_argument('--elf')
    a = ap.parse_args()
    failures = []
    phy = disasm(a.objdump, a.libphy)

    t = text(phy, 'phy_set_rfpll_freq')
    expect(re.search(r'sh\s+a1,714\(', t) is not None,
           'phy_set_rfpll_freq stores the MHz (a1) at phy_param+714', failures)
    expect(re.search(r'lbu\s+a0,42\(', t) is not None and 'phy_i2c_sdm_en' in t,
           'phy_set_rfpll_freq passes phy_param[42] (5 GHz flag) to phy_i2c_sdm_en', failures)
    expect(re.search(r'li\s+a0,300', t) is not None and 'ets_delay_us' in t,
           'phy_set_rfpll_freq ends with ets_delay_us(300) (the wait hop() leaves out)', failures)
    # the calls hop() makes, in its order (ESP-IDF 5.5.1's libphy for the C5)
    steps = ['phy_rfpll_set_freq', 'phy_write_rfpll_sdm', 'phy_set_freq_i2c',
             'phy_wait_rfpll_cal_end', 'phy_i2c_sdm_en']
    calls = re.findall(r'R_RISCV_CALL\s+(\w+)', t)
    expect([c for c in calls if c != 'ets_delay_us'] == steps,
           'phy_set_rfpll_freq calls exactly %s (found %s)' % (', '.join(steps), ', '.join(calls)), failures)

    t = text(phy, 'phy_set_rf_freq_offset')
    m = re.search(r'addi\s+sp,sp,-(\d+)', t)
    expect(m is not None and int(m.group(1)) <= 48 and re.search(r'addi\s+a3,sp,8', t) is not None,
           'phy_set_rf_freq_offset keeps the SDM state in <= 16 bytes at sp+8 (hop() allows 16)', failures)
    calls = re.findall(r'R_RISCV_CALL\s+(\w+)', t)
    expect(calls == ['phy_set_rfpll_freq', 'phy_ckgen_5g_cal', 'phy_freq_mem_change_5g'],
           'phy_set_rf_freq_offset: set_rfpll_freq, ckgen_5g_cal, freq_mem_change_5g (found %s)'
           % ', '.join(calls), failures)
    expect(re.search(r'addi\s+a0,sp,8\s*\n.*\n?.*phy_freq_mem_change_5g', t) is not None,
           'phy_freq_mem_change_5g gets the SDM state (sp+8)', failures)

    t = text(phy, 'phy_force_rx_gain')
    expect('lui\ta4,0x600a7' in t.replace('  ', '\t') or re.search(r'lui\s+a\d,0x600a7', t) is not None,
           'phy_force_rx_gain writes register block 0x600a7xxx', failures)
    expect(re.search(r'slli\s+a1,a1,0x18', t) is not None and re.search(r'slli\s+a0,a0,0x17', t) is not None,
           'phy_force_rx_gain: index -> bits 24-31, enable -> bit 23', failures)
    expect(re.search(r'\b44\(a\d\)', t) is not None, 'phy_force_rx_gain uses offset 0x2c (0x600A702C)', failures)

    chan = [n for n in ('phy_set_channel_rfpll_freq', 'phy_set_channel_rfpll_freq_new') if n in phy]
    callers = [n for n, body in phy.items() if n not in chan and
               any(re.search(r'R_RISCV_CALL\s+(%s)$' % '|'.join(chan), l) for l in body)] if chan else []
    found = False
    for n in callers:
        body = text(phy, n)
        if re.search(r'lbu\s+a1,49\(', body) and re.search(r'lh\s+a2,30\(', body):
            found = True
    expect(found, "Espressif's channel code passes phy_param[49] (xtal) and int16 phy_param[30] (trim)", failures)

    t = text(phy, 'phy_set_chanfreq')
    expect('phy_chip_set_chan' in t, 'phy_set_chanfreq calls phy_chip_set_chan (band setup)', failures)

    if a.librftest:
        rf = disasm(a.objdump, a.librftest)
        expect('adctrig' in rf, 'librftest.a defines adctrig', failures)
    else:
        print('skip  librftest.a not given (adctrig)')

    if a.elf:
        nm = a.nm or a.objdump.replace('objdump', 'nm')
        syms = {}
        for line in subprocess.run([nm, a.elf], capture_output=True, text=True, check=True).stdout.splitlines():
            parts = line.split()
            if len(parts) == 3:
                syms[parts[2]] = int(parts[0], 16)
        for s in ('_iram_end', '_data_end', '_bss_end'):
            if s in syms:
                expect(syms[s] <= BANK_START - BANK_MARGIN,
                       '%s = 0x%08x at least 2 KB below the capture bank (0x%08x)' % (s, syms[s], BANK_START), failures)
            else:
                print('skip  %s not in the ELF' % s)
        expect('adctrig' in syms and 'phy_force_rx_gain' in syms, 'adctrig and phy_force_rx_gain linked', failures)

    print('%d check(s) failed' % len(failures) if failures else 'all checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
