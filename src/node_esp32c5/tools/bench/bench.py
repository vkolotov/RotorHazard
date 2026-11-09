"""C5 sensor + C5 simulator bench session (both ports held open)."""
import re, sys, time, serial
SEN = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_38:44:BE:0E:28:F0-if00'
SIM = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_38:44:BE:0E:2A:14-if00'
RB = [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]

def port(p):
    s = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=2)
    s.dtr = False; s.rts = False; s.port = p; s.open()
    return s

class Dev:
    def __init__(self, p, bang):
        self.s = port(p); self.bang = bang
    def boot(self):
        time.sleep(2.5); self.s.reset_input_buffer()
    def cmd(self, c, wait=10):
        self.s.write(((('!' if self.bang else '') + c) + '\n').encode())
        out, end = [], time.monotonic() + wait
        buf = b''
        while time.monotonic() < end:
            buf += self.s.read(512)
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                t = line.decode(errors='replace').strip()
                if t == 'ok' or t.startswith('err'):
                    if t.startswith('err'): out.append(t)
                    return out
                out.append(t)
        return out

def scan(sen, cycles=300):
    rows = {}
    for l in sen.cmd('scan %d' % cycles, wait=30):
        m = re.match(r'(\d+) MHz .*mean (-?[\d.]+) max (-?[\d.]+) wide (-?[\d.]+) clip (\d+)', l)
        if m:
            rows[int(m.group(1))] = (float(m.group(2)), float(m.group(3)), float(m.group(4)), int(m.group(5)))
    return rows

def setup():
    sen, sim = Dev(SEN, True), Dev(SIM, False)
    sen.boot(); sim.boot()
    for i, f in enumerate(RB):
        sen.cmd('freq %d %d' % (i, f))
    return sen, sim
