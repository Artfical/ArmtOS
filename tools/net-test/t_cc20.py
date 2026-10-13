# Checks our ChaCha20-Poly1305 against python-cryptography and the RFC 8439 vector.
import subprocess, sys
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
out = subprocess.run(['/tmp/t_cc20'], capture_output=True, text=True).stdout.splitlines()
bad = 0; n = 0
RFC_TAG = '1ae10b594f09e26a7e902ecbd0600691'
for line in out:
    f = line.split()
    if f[0] == 'rfc_tag':
        if f[1] != RFC_TAG: print('RFC 8439 tag mismatch:', f[1]); bad += 1
    elif f[0] == 'case':
        k, nonce, aad, pt, ct, tag = f[1:]
        k = bytes.fromhex(k); nonce = bytes.fromhex(nonce)
        aad = b'' if aad == '-' else bytes.fromhex(aad)
        pt = b'' if pt == '-' else bytes.fromhex(pt)
        ct = b'' if ct == '-' else bytes.fromhex(ct)
        if ChaCha20Poly1305(k).encrypt(nonce, pt, aad) != ct + bytes.fromhex(tag):
            print('MISMATCH len(pt)=%d len(aad)=%d' % (len(pt), len(aad))); bad += 1
        n += 1
    elif 'FAILED' in line or 'ACCEPTED' in line:
        print(line); bad += 1
print('ChaCha20-Poly1305: %d cases + RFC vector, %s' % (n, 'FAILED' if bad else 'ok'))
sys.exit(1 if bad else 0)
