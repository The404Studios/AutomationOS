#!/usr/bin/env python3
# Pure-Python P-384 ECDSA reference. Generates many (pub, hash, r, s, expect)
# tuples -- both VALID signatures and a battery of forged/edge cases -- as a
# C array, so the real C verifier can be differentially tested far beyond the
# single RFC6979 vector.
import hashlib, os, random

p  = 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff
a  = p - 3
b  = 0xb3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef
n  = 0xffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973
Gx = 0xaa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7
Gy = 0x3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f
G  = (Gx, Gy)

def inv(x, m): return pow(x, -1, m)

def add(P, Q):
    if P is None: return Q
    if Q is None: return P
    x1,y1=P; x2,y2=Q
    if x1==x2 and (y1+y2)%p==0: return None
    if P==Q:
        l=(3*x1*x1+a)*inv(2*y1,p)%p
    else:
        l=(y2-y1)*inv(x2-x1,p)%p
    x3=(l*l-x1-x2)%p
    y3=(l*(x1-x3)-y1)%p
    return (x3,y3)

def mul(k, P):
    R=None; Q=P
    while k:
        if k&1: R=add(R,Q)
        Q=add(Q,Q); k>>=1
    return R

def bits2int(h):
    # leftmost 384 bits of hash; SHA-384 is exactly 384 bits.
    x=int.from_bytes(h,'big')
    return x

def sign(d, h, k):
    e=bits2int(h)%n
    R=mul(k,G); r=R[0]%n
    s=inv(k,n)*(e+r*d)%n
    return r,s

def on_curve(P):
    if P is None: return False
    x,y=P
    return (y*y-(x*x*x+a*x+b))%p==0

def h48(P): return '%096x'%(P%p) if False else '%096x'%P

def b48hex(x): return '%096x'%(x % (1<<384))

vectors=[]  # (label, pubX, pubY, hashhex(48B), r, s, expect)

random.seed(0xC0FFEE)

def add_vec(label, Q, hbytes, r, s, expect):
    Qx,Qy = Q
    vectors.append((label, b48hex(Qx), b48hex(Qy), hbytes.hex(), b48hex(r), b48hex(s), 1 if expect else 0))

# ---- VALID signatures (random keys/messages/nonces) ----
for i in range(40):
    d = random.randrange(1, n)
    Q = mul(d, G)
    msg = os.urandom(random.randint(0,80))
    h = hashlib.sha384(msg).digest()
    k = random.randrange(1, n)
    r,s = sign(d,h,k)
    if r==0 or s==0:
        continue
    add_vec("valid", Q, h, r, s, True)

# ---- VALID with a 0x04 (uncompressed) -- already default. Tamper each into a forgery. ----
base=[v for v in vectors if v[0]=="valid"][:15]

# ---- Forgeries: tamper r ----
for v in base[:8]:
    _,Qx,Qy,hh,rh,sh,_=v
    r=int(rh,16); s=int(sh,16)
    h=bytes.fromhex(hh)
    add_vec("tamper_r", (int(Qx,16),int(Qy,16)), h, (r^1)%n if (r^1)!=0 else 2, s, False)

# ---- Forgeries: tamper s ----
for v in base[:8]:
    _,Qx,Qy,hh,rh,sh,_=v
    r=int(rh,16); s=int(sh,16)
    h=bytes.fromhex(hh)
    add_vec("tamper_s", (int(Qx,16),int(Qy,16)), h, r, (s^1)%n if (s^1)!=0 else 2, False)

# ---- Forgeries: wrong hash ----
for v in base[:8]:
    _,Qx,Qy,hh,rh,sh,_=v
    r=int(rh,16); s=int(sh,16)
    h=bytearray(bytes.fromhex(hh)); h[0]^=0x80
    add_vec("wrong_hash", (int(Qx,16),int(Qy,16)), bytes(h), r, s, False)

# ---- Edge: r = n (out of range, must reject) ----
v=base[0]; Qx,Qy=int(v[1],16),int(v[2],16); h=bytes.fromhex(v[3]); s=int(v[5],16)
add_vec("r_eq_n", (Qx,Qy), h, n, s, False)
add_vec("s_eq_n", (Qx,Qy), h, int(v[4],16), n, False)
add_vec("r_eq_0", (Qx,Qy), h, 0, s, False)
add_vec("s_eq_0", (Qx,Qy), h, int(v[4],16), 0, False)

# ---- Edge: valid sig but s replaced with (n - s). ECDSA malleability: (r, n-s)
#      is ALSO a valid signature for the same message (since -R has same x). A
#      correct verifier ACCEPTS both. This tests the affine x-coordinate path. ----
for v in base[:10]:
    Qx,Qy=int(v[1],16),int(v[2],16); h=bytes.fromhex(v[3]); r=int(v[4],16); s=int(v[5],16)
    add_vec("malleable_neg_s", (Qx,Qy), h, r, (n-s)%n, True)

# ---- Edge: a valid signature whose verification R has x-coordinate >= n, so that
#      r = R.x mod n != R.x. We must search for such a case to exercise the final
#      "mod n" on v. With overwhelming prob random sigs already hit R.x < n, so we
#      specifically search for R.x in [n, p).  (Probability ~ (p-n)/p ~ 2^-194, so
#      we cannot find one by brute force.) Instead, we construct r = (x mod n)
#      where x>=n directly: take a known point with large x.  Skip if not found. ----

# ---- Edge: hash longer than 48 bytes (SHA-512 digest) -- only leftmost 48 used.
for i in range(6):
    d = random.randrange(1, n)
    Q = mul(d, G)
    msg = os.urandom(40)
    h64 = hashlib.sha512(msg).digest()        # 64 bytes
    htrunc = h64[:48]                          # what bits2int(leftmost 384) uses
    k = random.randrange(1, n)
    r,s = sign(d, htrunc, k)
    if r==0 or s==0: continue
    # Pass FULL 64-byte hash to verifier; it should use leftmost 48.
    Qx,Qy=Q
    vectors.append(("long_hash", b48hex(Qx), b48hex(Qy), h64.hex(), b48hex(r), b48hex(s), 1))

# ---- Edge: hash shorter than 48 bytes (SHA-256 digest, 32 bytes) ----
for i in range(6):
    d = random.randrange(1, n)
    Q = mul(d, G)
    msg = os.urandom(40)
    h32 = hashlib.sha256(msg).digest()         # 32 bytes
    e = int.from_bytes(h32,'big') % n          # bits2int of a 32-byte hash = whole
    k = random.randrange(1, n)
    R=mul(k,G); r=R[0]%n
    s=inv(k,n)*(e+r*d)%n
    if r==0 or s==0: continue
    Qx,Qy=Q
    vectors.append(("short_hash", b48hex(Qx), b48hex(Qy), h32.hex(), b48hex(r), b48hex(s), 1))

# ---- Edge: off-curve pubkey (valid sig math but point not on curve) -> reject ----
v=base[1]; Qx,Qy=int(v[1],16),int(v[2],16); h=bytes.fromhex(v[3]); r=int(v[4],16); s=int(v[5],16)
add_vec("offcurve", (Qx^1, Qy), h, r, s, False)

# ---- Edge: pubkey coordinate == p (>= p, must reject) ----
add_vec("pub_x_eq_p", (p, Qy), h, r, s, False)

# Emit a C header.
print("/* AUTO-GENERATED by p384_ref.py. Do not edit. */")
print("typedef struct { const char* label; const char* qx; const char* qy; const char* h; const char* r; const char* s; int hlen; int expect; } p384_tv;")
print("static const p384_tv P384_TV[] = {")
for label,qx,qy,h,r,s,exp in vectors:
    hlen = len(h)//2
    print('  {"%s","%s","%s","%s","%s","%s",%d,%d},'%(label,qx,qy,h,r,s,hlen,exp))
print("};")
print("static const int P384_TV_N = %d;"%len(vectors))
