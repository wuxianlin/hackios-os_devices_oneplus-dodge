#!/usr/bin/env python3
"""hackios.py — boot HakiOS on the OnePlus 13 from Linux or Windows.

The xl flow of the HakiOS repository (its README documents every part), in one
cross-platform script for the release zip: boot / ramdisk / run / screenshot /
log / stop / reboot / state.

  python hackios.py boot              fastboot-boot the UEFI+loader (boot.img)
  python hackios.py ramdisk SYS.apfs  upload the system image ONCE per power-on
  python hackios.py run               boot iOS, wait for the session's end
  python hackios.py screenshot [F]    what is on the phone's screen, as PNG
  python hackios.py log               the log of the run in progress
  python hackios.py stop              end the run now
  python hackios.py reboot            back to Android (or into fastboot: --bl)
  python hackios.py state             where the phone is

Environment for `run`: SECS=900 (the session's length), CMDLINE="-v" (extra
kernel arguments; device-args beside this script is merged in automatically),
RAMDISK=root.apfs ("resident" uses only the big image already uploaded), STAY=1 (leave the phone in the
loader after the session instead of rebooting to Android — xl's own default).

Needs python3 (nothing else) and fastboot/adb from Android's platform-tools in
PATH or in a platform-tools/ folder beside this script.
"""
import os, struct, subprocess, sys, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))

def find_tool(name):
    for d in (os.path.join(HERE, 'platform-tools'), ''):
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return os.path.abspath(p)
    return name  # from PATH

FASTBOOT, ADB = find_tool('fastboot'), find_tool('adb')

def fb(*args, timeout=20):
    try:
        r = subprocess.run([FASTBOOT, *args], capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return ''

def adb(*args, timeout=10):
    try:
        r = subprocess.run([ADB, *args], capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return ''

def in_fastboot():
    return 'fastboot' in fb('devices', timeout=5)

def is_loader():
    return in_fastboot() and 'xnuloader' in fb('getvar', 'product', timeout=5)

def state():
    if is_loader(): return 'loader'
    if in_fastboot(): return 'fastboot'
    if adb('get-state', timeout=3).strip().endswith('device'): return 'android'
    return 'absent'

def wait_for(want, secs):
    for _ in range(0, secs, 2):
        if state() == want: return True
        time.sleep(2)
    return False

def die(msg):
    sys.exit(f'hackios: {msg}')

def enter():
    st = state()
    if st == 'android':
        adb('reboot', 'bootloader')
        wait_for('fastboot', 60) or die('phone did not reach fastboot')
    elif st == 'absent':
        print('waiting for the phone (Android or fastboot)...')
        if not wait_for('fastboot', 180) and state() != 'loader':
            die('phone not reachable')
        return enter()
    if is_loader(): return
    img = os.path.join(HERE, 'boot.img')
    os.path.isfile(img) or die('boot.img not next to this script')
    for _ in range(2):     # the first boot right after entering fastboot is sometimes not taken
        if 'OKAY' in fb('boot', img, timeout=60):
            time.sleep(6)
            if wait_for('loader', 30): return
    die('loader did not come up')

def ramdisk(path=None):
    path = path or os.path.join(HERE, 'system.apfs')
    os.path.isfile(path) or die(f'{path} not found')
    enter()
    'OKAY' in fb('oem', 'ramdisk') or die('the loader refused (no memory window?)')
    print(f'uploading {os.path.getsize(path) // 1048576} MB (a minute or two)...')
    t0 = time.time()
    'OKAY' in fb('stage', path, timeout=3600) or die('upload failed')
    print(f'uploaded in {max(1, int(time.time() - t0))} s -- stays in RAM across runs while powered')

def device_args():
    """the device's own kernel arguments (devices/oneplus-dodge/args in the
    repository; shipped beside this script with the comments stripped)"""
    p = os.path.join(HERE, 'device-args')
    if not os.path.isfile(p):
        return ''
    return ' '.join(l.split('#', 1)[0].strip() for l in open(p)).strip()

def run():
    enter()
    kern = os.path.join(HERE, 'kernel.bin')
    os.path.isfile(kern) or die('kernel.bin not next to this script')
    secs = os.environ.get('SECS', '900')
    args = os.environ.get('CMDLINE', '').strip()
    extra = device_args()
    if extra:
        args = (extra + ' ' + args).strip()
    # the run's RAM disk is the SMALL root image (~42 MB, /dev/md1, /redwood);
    # the big system image is already resident from `ramdisk` (/dev/md0)
    rd = os.environ.get('RAMDISK', 'root.apfs')
    payload = [sys.executable, os.path.join(HERE, 'mkpayload.py'), '-o',
               os.path.join(HERE, 'run-payload.bin'), f'kernel={kern}']
    if rd and rd.lower() not in ('none', 'resident'):
        rp = rd if os.path.isabs(rd) else os.path.join(HERE, rd)
        os.path.isfile(rp) or die(f'ramdisk {rp} not found (RAMDISK=resident to use only the resident system image)')
        payload.append(f'ramdisk={rp}')
    payload += ['--cmdline', f'loader_return_after={secs} epoch={int(time.time())} sb_delay=8 {args}']
    ver_p = os.path.join(HERE, 'kernel.ver')
    if os.path.isfile(ver_p):
        ver = open(ver_p).read().strip()
        if ver:
            payload += ['--kernel-version', ver]
    try:
        subprocess.run(payload, check=True, stdout=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        die('mkpayload failed -- is kernel.bin valid?')
    out = fb('stage', os.path.join(HERE, 'run-payload.bin'), timeout=300)
    if 'OKAY' not in out:
        die(f'stage failed: {out.strip()[:500]}')
    fb('oem', 'run')
    print(f'iOS starting ({secs} s session). log: python {os.path.basename(__file__)} log')
    deadline = time.time() + int(secs) + 120
    while time.time() < deadline:
        for line in fb('getvar', 'result', timeout=15).splitlines():
            if line.startswith('result:') and 'running' not in line:
                print(line.strip())
                log()
                if not os.environ.get('STAY'):
                    reboot()      # xl leaves the phone in the loader; back to Android by default here
                return
        time.sleep(2)
    print('no answer -- the kernel hung; the watchdog resets the phone')

def log():
    """the whole log in one transfer when that works, else in 16 KB parts
    (some UEFI/host pairs stop a big upload at 32767 bytes; a small one
    always goes through -- oem log-part N on the loader stages slice N)"""
    p = os.path.join(HERE, 'last-run.log')
    whole = b''
    fb('oem', 'log')
    fb('get_staged', p, timeout=60)
    if os.path.isfile(p):
        whole = open(p, 'rb').read().replace(b'\0', b'')
    parts, i = [], 0
    while True:
        out = fb('oem', f'log-part {i}', timeout=20)
        if 'OKAY' not in out:        # "past the end", or the loader is gone
            break
        pp = p + f'.part{i}'
        fb('get_staged', pp, timeout=60)
        if not os.path.isfile(pp):
            break
        raw = open(pp, 'rb').read()  # judged by its RAW length: the NULs (block
        os.unlink(pp)                # padding) are stripped after joining, or a full
        if not raw:                  # part with one in it would end the loop early
            break
        parts.append(raw)
        if len(raw) < 16384:         # the last part is short
            break
        i += 1
        if i > 512:                  # 8 MB of log is far past anything real
            break
    joined = b''.join(parts).replace(b'\0', b'')
    data = joined if len(joined) > len(whole) else whole
    if len(data) < 1024:
        # a USB re-enumeration mid-fetch delivers a stub: one clean retry
        time.sleep(2)
        fb('oem', 'log')
        fb('get_staged', p, timeout=60)
        if os.path.isfile(p):
            again = open(p, 'rb').read().replace(b'\0', b'')
            data = again if len(again) > len(data) else data
    if data:
        open(p, 'wb').write(data)
        print(f'log: {p} ({len(data)} bytes)')

def screenshot(out=None):
    enter()
    out = out or os.path.join(HERE, 'screen.png')
    geom = ''
    for line in fb('getvar', 'screen', timeout=15).splitlines():
        if line.startswith('screen:'):
            geom = line[7:].strip()
    if not geom or 'OKAY' not in fb('oem', 'screenshot', timeout=30):
        die('the loader has no display')
    raw = os.path.join(HERE, 'screen.raw')
    fb('get_staged', raw, timeout=60)
    os.path.isfile(raw) or die('no screenshot came back')
    w, h, stride = map(int, geom.split()[:3])
    data = open(raw, 'rb').read()
    rows = bytearray()
    for y in range(h):
        line = data[y * stride:y * stride + w * 4]
        rows.append(0)                                   # PNG filter: none
        rgb = bytearray(w * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = line[2::4], line[1::4], line[0::4]   # the panel is BGRX
        rows += rgb
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    with open(out, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' +
                chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b''))
    os.unlink(raw)
    print(f'{out} ({w}x{h})')

def reboot(to_bootloader=False):
    st = state()
    if st == 'android' and not to_bootloader:
        adb('reboot')                      # already Android, nothing to do
        print('rebooting Android')
        return
    if st == 'absent':
        die('phone not reachable')
    if st != 'loader':
        enter()                            # from fastboot, boot the loader first
    cmd = 'reboot-bootloader' if to_bootloader else 'reboot'
    out = fb(cmd, timeout=10)              # the connection drops: no OKAY to wait for
    time.sleep(4)
    # some UEFI ports do not carry the reset through (this phone's among them): the
    # command is answered and nothing happens. Say so instead of promising Android.
    if state() == 'loader':
        print('this port\'s UEFI does not reset the phone: hold power ~10 s to leave'
              + (' (to fastboot: keep holding, or "fastboot reboot" once in Android)' if not to_bootloader else ''))
        return
    print('back to ' + ('fastboot' if to_bootloader else 'Android') + ' (takes half a minute; '
          'if nothing happens: hold power ~10 s)')

if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd in ('boot', 'enter'):
        enter()
    elif cmd == 'ramdisk':
        ramdisk(sys.argv[2] if len(sys.argv) > 2 else None)
    elif cmd == 'run':
        run()
    elif cmd == 'screenshot':
        screenshot(sys.argv[2] if len(sys.argv) > 2 else None)
    elif cmd == 'log':
        log()
    elif cmd == 'stop':
        'OKAY' in fb('oem', 'stop', timeout=30) or print('stop sent')
    elif cmd == 'reboot':
        reboot('--bl' in sys.argv)
    elif cmd == 'state':
        print(state())
    else:
        print(__doc__)
