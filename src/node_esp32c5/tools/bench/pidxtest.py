import sys, time, serial
sys.path.insert(0, 'tools'); import rh_watch
SIM = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_38:44:BE:0E:2A:14-if00'
ser = serial.Serial(port=None, baudrate=115200, timeout=0.2); ser.dtr = False; ser.rts = False
ser.port = SIM; ser.open(); time.sleep(1.5); ser.reset_input_buffer()
def cmd(c):
    ser.write((c + '\n').encode()); time.sleep(0.3)
    return ser.read(400).decode(errors='replace').strip().replace('\n', ' | ')
def r4():
    s = rh_watch.watch('http://akltimer2', 2.5); v = s[3]; return sum(v) / len(v)
print('base', round(r4(), 1))
print(cmd('level 5769 0'))
PREFIX = sys.argv[1] + ' '
for k in sys.argv[2:]:
    rep = cmd(PREFIX + k)
    print(PREFIX, k, '->', rep, ' R4 %.1f' % r4(), flush=True)
cmd('pidx -1'); cmd('off'); ser.close()
