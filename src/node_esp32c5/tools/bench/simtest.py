import sys, time, serial
sys.path.insert(0, 'tools')
import rh_watch
SIM = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_38:44:BE:0E:2A:14-if00'
names = ['R1','R2','R3','R4','R5','R6','R7','R8']
ser = serial.Serial(port=None, baudrate=115200, timeout=0.2); ser.dtr = False; ser.rts = False
ser.port = SIM; ser.open(); time.sleep(1.5); ser.reset_input_buffer()
def cmd(c):
    ser.write((c + '\n').encode()); time.sleep(0.3)
    return ser.read(400).decode(errors='replace').strip().replace('\n', ' | ')
def show(label, secs=3):
    s = rh_watch.watch('http://akltimer2', secs)
    print('%-22s %s' % (label, '  '.join('%s %5.1f' % (names[i], sum(v)/len(v)) for i, v in sorted(s.items()))), flush=True)
print('status:', cmd('status'))
show('off')
for args in sys.argv[1:]:
    print('cmd:', args, '->', cmd(args))
    show(args)
    cmd('off')
show('off again')
ser.close()
