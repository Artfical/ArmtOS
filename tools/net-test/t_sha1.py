import subprocess, sys, hashlib
bad = 0; n = 0
for line in subprocess.run(['/tmp/t_sha1'], capture_output=True, text=True).stdout.splitlines():
    if 'mismatch' in line: print(line); bad += 1; continue
    ln, dg = line.split()
    ln = int(ln)
    m = bytes((i * 11 + ln) & 0xff for i in range(ln))
    if hashlib.sha1(m).hexdigest() != dg: print('SHA-1 mismatch at', ln); bad += 1
    n += 1
print('SHA-1: %d lengths, %s' % (n, 'FAILED' if bad else 'ok'))
sys.exit(1 if bad else 0)
