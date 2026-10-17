# Updating the server and restarting it

The RotorHazard server restarts itself in process. On a timer where `sudo`
wants a password over ssh and `systemctl` refuses non-interactively, this is
the only restart route that works unattended - and it is the one to prefer
anyway, because it keeps the node link open.

## Updating the code

The timer is a git clone. Pull, do not copy files in:

```bash
cd ~/RotorHazard
git fetch origin
git reset --hard origin/<branch>
git status --porcelain     # expect empty
```

Copying individual files over ssh leaves the working tree dirty against the
commit it claims to be on, so the next pull conflicts and nobody can tell
what is actually deployed. If a network outage forces a direct copy, undo it
with `git reset --hard` once connectivity returns.

`git status --porcelain` printing nothing is the check that the deploy is
what the commit says it is.

## Restarting

### Over socket.io

```python
import base64, socketio
AUTH = base64.b64encode(b'admin:password').decode()
sio = socketio.Client()
sio.connect('http://127.0.0.1:5000', headers={'Authorization': 'Basic ' + AUTH})
sio.emit('restart_server')          # no arguments
```

`on_restart_server()` takes no parameters, so passing a payload raises a
`TypeError` that the wrapper swallows - the call appears to succeed and
nothing restarts. Emit it bare.

The process re-executes itself, so the serial port is reopened and the nodes
re-enumerate.

### From the UI

Settings has a restart control, and a banner appears when a change needs one.

### Why not systemctl

On this timer `sudo -n` fails and `systemctl restart` over non-interactive
ssh returns *Interactive authentication required*. The unit is `Restart=no`,
so killing the process leaves the server **down**, not restarted. If a
systemd restart is genuinely needed, it has to be run by hand:

```bash
ssh -t rotorhazard@rotorhazard 'sudo systemctl restart rotorhazard'
```

## Confirming the restart

The server writes a new log file per start. Wait for it to answer, then read
the newest log:

```bash
until curl -s -o /dev/null -m 2 http://127.0.0.1:5000/; do sleep 3; done
L=$(ls -t ~/rh-data/logs/*.log | head -1)
grep -iE 'Serial multi-node found' "$L" | tail -1
grep -cE '\[ERROR\]|Traceback' "$L"
```

Three things to check:

- the enumeration line: node count, API level, firmware timestamp
- `node_api_match` true in the server-info line
- zero errors

Reading a log chosen before the restart is a trap - `ls -t | head -1` picks
the previous file if the new one has not been created yet. Wait for the
server to answer first, then list.

## Restarts are not always needed

A setting that is pushed to the nodes when it changes does not need one. The
RSSI resolution setting was listed in `Config._restart_required_keys` from
when it was only read at startup; it is applied immediately now, and the
entry was removed. If a setting raises the restart banner but takes effect
without one, check that list.

## Reboot

A full reboot takes about 40 seconds, but the host can be unreachable for
several minutes afterwards while the network comes back. Do not diagnose a
boot failure early.

Journald is volatile on this image, so kernel messages do not survive a
reboot. The server's own logs under `~/rh-data/logs/` do persist.
