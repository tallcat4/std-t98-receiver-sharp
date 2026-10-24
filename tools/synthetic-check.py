"""Offline 4FSK integration fixture, not an RF transmitter or sensitivity model."""
import ctypes as C
import json
from pathlib import Path
import sys
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
class Frame(C.Structure):
    _fields_ = [('symbol', C.c_uint64)] + [(n, C.c_int32) for n in ('traffic','crc','user','maker','call','mode')] + [('csm', C.c_char*16),('payload', C.c_uint8*36),('llr', C.c_int8*288)]
class Stats(C.Structure):
    _fields_ = [(n,C.c_uint64) for n in ('samples','frames','crc')] + [(n,C.c_double) for n in ('power','frequency')] + [('locked',C.c_int32),('active',C.c_int32)]

def bits(value, count):
    return [int(v) for v in f'{value:0{count}b}']

def convolution(data):
    result, state = [], 0
    for bit in data:
        register = bit*16+state
        result.extend([(register&0x13).bit_count()%2, (register&0x1d).bit_count()%2])
        state = bit*8+(state>>1)
    return result

def crc(data, size, taps):
    state = [1]*size
    for bit in data:
        fb = bit ^ state[-1]
        state = [fb] + [v ^ (fb if i in taps else 0) for i,v in enumerate(state[:-1],1)]
    return state[::-1]

def frame(traffic, payload, call=0):
    rich = [int(traffic),0,0,0,0,0,0,int(traffic)]
    message = [0]*8 + bits(call,2) + bits(123,9) + bits(42,7)
    encoded = convolution(message + crc(message,6,{1,2,5}) + [0]*4)
    sacch = np.array([v for i,v in enumerate(encoded) if i%12 not in (5,11)]).reshape(5,12).T.flatten().tolist()
    if traffic:
        tch = np.unpackbits(np.frombuffer(payload, np.uint8)).tolist()
    else:
        message = bits(0x123456789,36) + [0]*44
        encoded = convolution(message + crc(message,12,{1,2,3,11}) + [0]*4)
        pich = np.array([v for i,v in enumerate(encoded) if i%4 != 1]).reshape(9,16).T.flatten().tolist()
        tch = pich+[0]*144
    data = [v for bit in rich for v in (bit,bit)] + sacch+tch
    symbols = [-3,1,-3,3,-3,-3,3,3,-1,3] + [({(0,0):1,(0,1):3,(1,0):-1,(1,1):-3})[tuple(data[i:i+2])] for i in range(0,len(data),2)]
    state = [0,1,1,1,0,0,1,0,0]
    for i in range(10,192):
        symbols[i] *= -1 if state[-1] else 1
        state = [state[4]^state[8]]+state[:-1]
    return symbols

def signal(rate, offset=0):
    payload=bytes((i*17+3)%256 for i in range(36))
    symbols = np.array([1,-1]*120 + sum([frame(False,payload) for _ in range(8)],[]) + sum([frame(True,payload) for _ in range(28)],[]) + [1,-1]*120)
    n = int(len(symbols)*rate/2400)
    levels=symbols[np.minimum((np.arange(n)*2400/rate).astype(int),len(symbols)-1)]
    iq=np.exp(1j*np.cumsum((levels*315+offset)*2*np.pi/rate)).astype(np.complex64)*.7
    return iq,payload

def library():
    lib=C.CDLL(str(ROOT/'build/native/t98_native.dll'))
    lib.t98_create.argtypes=[C.c_double];lib.t98_create.restype=C.c_void_p
    lib.t98_destroy.argtypes=[C.c_void_p]
    lib.t98_process.argtypes=[C.c_void_p,C.POINTER(C.c_float),C.c_int]
    lib.t98_read.argtypes=[C.c_void_p,C.POINTER(Frame)]
    lib.t98_status.argtypes=[C.c_void_p,C.POINTER(Stats),C.POINTER(C.c_float),C.c_int]
    return lib

def receive(lib,iq,rate,chunk):
    handle=lib.t98_create(rate)
    assert handle
    frames=[]
    try:
        for p in range(0,len(iq),chunk):
            block=np.ascontiguousarray(iq[p:p+chunk])
            assert lib.t98_process(handle,block.ctypes.data_as(C.POINTER(C.c_float)),len(block))==0
            event=Frame()
            while lib.t98_read(handle,C.byref(event))>0:
                frames.append((event.symbol,event.traffic,event.crc,event.user,event.maker,bytes(event.csm),bytes(event.payload)))
        stats=Stats();eye=(C.c_float*2048)()
        count=lib.t98_status(handle,C.byref(stats),eye,2048)
        assert count>0 and all(np.isfinite(eye[:count]))
        return frames,stats
    finally: lib.t98_destroy(handle)

def main():
    lib=library();report=[]
    for rate in (24000,48000,96000,192000):
        iq,payload=signal(rate)
        a,stats=receive(lib,iq,rate,4096)
        b,_=receive(lib,iq,rate,337)
        traffic=[f for f in a if f[1] and f[2] and f[3:5]==(123,42) and f[6]==payload]
        pich=[f for f in a if not f[1] and f[2] and f[5]==b'123456789']
        assert len(traffic)>=20 and len(pich)>=4,(rate,len(traffic),len(pich),len(a))
        # AFC is block-adaptive; symbol positions may differ slightly. Contents must agree.
        assert [f[1:] for f in a]==[f[1:] for f in b], f'Chunk dependence at {rate}'
        report.append(dict(rate=rate,traffic=len(traffic),pich=len(pich),eye=True))
        if rate==48000:
            iq.tofile(ROOT/'build/synthetic.cf32')
    print(json.dumps(report,indent=2))
    (ROOT/'build/synthetic-results.json').write_text(json.dumps(report,indent=2))

if __name__=='__main__': main()
