import sys, time, threading, serial, socketio
SIM = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_38:44:BE:0E:2A:14-if00'
node = int(sys.argv[2]) if len(sys.argv) > 2 else 3
series = []
sio = socketio.Client(reconnection=False)
@sio.on('heartbeat')
def hb(d):
    series.append((time.monotonic(), d['current_rssi'][node]))
sio.connect('http://akltimer2', transports=['websocket', 'polling'], wait_timeout=10)
ser = serial.Serial(port=None, baudrate=115200, timeout=0.2); ser.dtr = False; ser.rts = False
ser.port = SIM; ser.open(); time.sleep(1.5); ser.reset_input_buffer()
t0 = time.monotonic()
ser.write((sys.argv[1] + '\n').encode())
reply = b''
while time.monotonic() - t0 < 8:
    reply += ser.read(200)
    if b'ok' in reply: break
time.sleep(1.0)
sio.disconnect(); ser.close()
print('sim:', reply.decode(errors='replace').strip().replace('\n', ' | '))
line = ''
for t, v in series:
    if t >= t0 - 1.0:
        line += '%+.1f:%d ' % (t - t0, v)
print(line)
