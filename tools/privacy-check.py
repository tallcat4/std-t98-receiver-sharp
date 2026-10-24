"""Check native FEC -> automatic series/key -> descrambled PCM and session boundaries."""
import base64
import ctypes as C
import importlib.util
import json
from pathlib import Path
import re
import sys
import time
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests/reference'))
from std_t98_cracker.sequence import THUMBDV_MAP, load_sequence
from std_t98_cracker.fec import FecDecoder

class NativeResult(C.Structure):
    _fields_=[('session',C.c_int64),('next',C.c_uint64),('series',C.c_int32),('key',C.c_int32),('score',C.c_double),('margin',C.c_double)]

class NativeEngine:
    def __init__(self):
        self.lib=C.CDLL(str(ROOT/'build/native/t98_native.dll'))
        self.lib.t98_secret_create.restype=C.c_void_p
        self.lib.t98_secret_destroy.argtypes=[C.c_void_p]
        self.lib.t98_secret_process.argtypes=[C.c_void_p,C.c_int64,C.c_uint64,C.c_void_p,C.c_int,C.POINTER(NativeResult)]
        self.lib.t98_secret_set_cache.argtypes=[C.c_void_p,C.c_void_p,C.c_int]
        self.lib.t98_secret_cache_hit.argtypes=[C.c_void_p]
        self.handle=self.lib.t98_secret_create();assert self.handle
    def send(self,session,first,llr):
        buffer=C.create_string_buffer(llr);out=NativeResult()
        assert self.lib.t98_secret_process(self.handle,session,first,buffer,len(llr)//288,C.byref(out))==0
        return dict(series=out.series if out.series>=0 else None,key=out.key if out.key>=0 else None,bursts=out.next,score=out.score,margin=out.margin,cached=bool(self.lib.t98_secret_cache_hit(self.handle)))
    def cache(self,codes):
        pairs=(C.c_int32*(2*len(codes)))(*(v for pair in codes for v in pair))
        assert self.lib.t98_secret_set_cache(self.handle,pairs,len(codes))==0
    def close(self): self.lib.t98_secret_destroy(self.handle)

def encoder():
    source=(ROOT/'third_party/ambe2/src/blip25.c').read_text()
    annex=[tuple(map(int,m)) for m in re.findall(r'\{(\d+), (\d+), (\d+), (\d+)\}',source.split('ANNEX_S[36][4] = {')[1].split('};')[0])]
    table=(ROOT/'third_party/ambe2/src/b25_golay.c').read_text().split('b25_golay23_gen[12] = {')[1].split('};')[0]
    generators=[int(x,16) for x in re.findall(r'0x[0-9a-f]+',table)]
    def golay(value):
        cw=0
        for i,g in enumerate(generators):
            if value&(1<<(11-i)): cw^=g
        return cw
    def encode(thumb):
        natural=np.zeros(49,np.uint8);natural[THUMBDV_MAP]=thumb
        values=[]
        for start,width in ((0,12),(12,12),(24,11),(35,14)):
            values.append(int(''.join(map(str,natural[start:start+width])),2))
        c0=golay(values[0]);c0=(c0<<1)|(c0.bit_count()%2)
        pr=16*values[0];mask=0
        for k in range(23):
            pr=(173*pr+13849)&65535;mask|=(pr>>15)<<(22-k)
        code=[c0,golay(values[1])^mask,values[2],values[3]]
        bits=[v for a,b,c,d in annex for v in ((code[a]>>b)&1,(code[c]>>d)&1)]
        return np.packbits(bits).tobytes()
    return encode

def main():
    encode=encoder()
    plain=np.array([np.unpackbits(np.frombuffer(bytes.fromhex(h),np.uint8))[:49] for h in
        ('deba0301a6e600','9f9ac301a64500','de9cc909b6e600','df9ac201a64c00')])
    dll=ROOT/'build/native/ambe2.dll';fec=FecDecoder(dll)
    engine=NativeEngine()
    report=[]
    try:
        for token,(series,key) in enumerate([(s,32767 if s==15 else 12345) for s in range(16)],1):
            encrypted=plain^load_sequence(series=series).thumbdv(key)
            payload=b''.join(map(encode,encrypted))
            llr=((np.unpackbits(np.frombuffer(payload,np.uint8)).astype(np.int16)*2-1)*100).astype(np.int8).tobytes()
            decoded,valid,_=fec.fec(np.frombuffer(payload,np.uint8).reshape(1,4,9),np.frombuffer(llr,np.int8).reshape(1,4,72))
            np.testing.assert_array_equal(decoded[0],encrypted);assert valid.all()
            t0=time.perf_counter()
            def send(first,count):
                return engine.send(token,first,llr*count)
            result=send(0,20)
            assert (result['series'],result['key'])==(series,key),result
            # Overlap must not duplicate samples. A gap starts fresh evidence.
            result=send(15,6);assert result['bursts']==21
            result=send(40,1);assert result['key'] is None
            report.append(dict(series=series,key=key,seconds=round(time.perf_counter()-t0,3),reset=True))
            if series==10:
                (ROOT/'build/cache-check.llr').write_bytes(llr*10)
                spec=importlib.util.spec_from_file_location('synthetic',ROOT/'tools/synthetic-check.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
                symbols=np.array([1,-1]*120+sum([m.frame(False,payload) for _ in range(5)],[])+sum([m.frame(True,payload,call=1) for _ in range(80)],[]))
                levels=np.repeat(symbols,20)
                iq=(.7*np.exp(1j*np.cumsum(levels*315*2*np.pi/48000))).astype(np.complex64)
                iq.tofile(ROOT/'build/secret.cf32')
                # Compare native output bit-for-bit against the clear-input path.
                lib=m.library();lib.t98_voice.argtypes=[C.c_void_p,C.c_void_p,C.c_int,C.c_int,C.c_void_p]
                a,b=lib.t98_create(48000),lib.t98_create(48000)
                try:
                    clear=b''.join(map(encode,plain))
                    clear_llr=((np.unpackbits(np.frombuffer(clear,np.uint8)).astype(np.int16)*2-1)*100).astype(np.int8)
                    encrypted_llr=np.frombuffer(llr,np.int8)
                    left=np.empty(640,np.int16);right=np.empty(640,np.int16)
                    for _ in range(5):
                        assert lib.t98_voice(a,encrypted_llr.ctypes.data,series,key,left.ctypes.data)==0
                        assert lib.t98_voice(b,clear_llr.ctypes.data,0,0,right.ctypes.data)==0
                        np.testing.assert_array_equal(left,right)
                finally: lib.t98_destroy(a);lib.t98_destroy(b)
        # Fresh contexts simulate persisted cache load, including more than five
        # candidates (all must be considered, not the solver's top-five limit).
        def soft(series,key):
            payload=b''.join(map(encode,plain^load_sequence(series=series).thumbdv(key)))
            return ((np.unpackbits(np.frombuffer(payload,np.uint8)).astype(np.int16)*2-1)*100).astype(np.int8).tobytes()
        cache_engine=NativeEngine()
        try:
            cache_engine.cache([(s,12345) for s in range(16)]+[(10,12345)])
            for series in range(16):
                llr=soft(series,12345);token=100+series
                r=cache_engine.send(token,0,llr*9);assert r['key'] is None and not r['cached'],r
                t0=time.perf_counter();r=cache_engine.send(token,9,llr)
                assert (r['series'],r['key'],r['cached'])==(series,12345,True),r
                report.append(dict(cache_series=series,seconds=round(time.perf_counter()-t0,3)))
                # A cache hit remains latched for this call, without model work.
                r=cache_engine.send(token,10,bytes(288*5));assert r['key']==12345 and r['cached'] and r['bursts']==15,r
                r=cache_engine.send(token,20,llr);assert r['key'] is None and not r['cached'],r
            llr=soft(10,23456)
            r=cache_engine.send(200,0,llr*20)
            assert (r['series'],r['key'],r['cached'])==(10,23456,False),r
            # Newly discovered code joins the in-memory cache immediately.
            r=cache_engine.send(201,0,llr*10)
            assert (r['key'],r['cached'])==(23456,True),r
            r=cache_engine.send(202,0,soft(0,0)*10)
            assert r['key']==0,r
        finally: cache_engine.close()
        print(json.dumps(report,indent=2));(ROOT/'build/privacy-results.json').write_text(json.dumps(report,indent=2))
    finally:
        engine.close()

if __name__=='__main__': main()
