# Builds ECDSA/P-256 cases with python-cryptography and checks our verifier agrees:
# real signatures must verify, tampered ones must not.
import subprocess, sys, os, hashlib
from cryptography.hazmat.primitives.asymmetric import ec, utils
from cryptography.hazmat.primitives import hashes, serialization
args = []
HASHES = [('sha256', hashes.SHA256(), 32), ('sha384', hashes.SHA384(), 48), ('sha512', hashes.SHA512(), 64)]
CURVES = [('2', ec.SECP256R1, 32), ('3', ec.SECP384R1, 48)]
for trial in range(24):
    name, halg, hlen = HASHES[trial % 3]
    cid, curve, clen = CURVES[(trial // 3) % 2]
    key = ec.generate_private_key(curve())
    pub = key.public_key().public_bytes(serialization.Encoding.X962,
                                        serialization.PublicFormat.UncompressedPoint)
    msg = os.urandom(40 + trial)
    sig = key.sign(msg, ec.ECDSA(halg))
    r, s = utils.decode_dss_signature(sig)
    digest = hashlib.new(name, msg).digest()
    rb, sb = r.to_bytes(clen, 'big').hex(), s.to_bytes(clen, 'big').hex()
    args += [cid, pub.hex(), rb, sb, digest.hex(), '1']
    # same signature, one bit flipped in the digest -> must be refused
    d2 = bytearray(digest); d2[0] ^= 1
    args += [cid, pub.hex(), rb, sb, bytes(d2).hex(), '0']
    # r and s swapped -> must be refused
    args += [cid, pub.hex(), sb, rb, digest.hex(), '0']
    # a different key's public point -> must be refused
    other = ec.generate_private_key(curve()).public_key().public_bytes(
        serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    args += [cid, other.hex(), rb, sb, digest.hex(), '0']
r = subprocess.run(['/tmp/t_p256'] + args, capture_output=True, text=True)
print(r.stdout.strip() or r.stderr.strip()[-2000:])
sys.exit(r.returncode)
