"""FP32 equivalence against the original PyTorch checkpoint; Python is test-only."""
import ctypes as C
import json
from pathlib import Path
import sys
import time
import numpy as np
import torch

root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tests/reference'))
from std_t98_cracker.ai import _load_checkpoint

torch.set_num_threads(1)
torch.backends.mkldnn.enabled=False
model,_=_load_checkpoint(root/'models/ambe2_hybrid.safetensors',torch.device('cpu'))
lib=C.CDLL(str(root/'build/native/t98_native.dll'))
lib.t98_secret_create.restype=C.c_void_p
lib.t98_secret_destroy.argtypes=[C.c_void_p]
lib.t98_hybrid_logits.argtypes=[C.c_void_p,C.c_void_p,C.c_int,C.c_void_p]
handle=lib.t98_secret_create();assert handle
try:
    rng=np.random.default_rng(98)
    inputs=np.vstack([rng.integers(0,2,(128,980)),np.zeros((1,980)),np.ones((1,980)),np.tile([0,1],(1,490))]).astype(np.float32)
    t0=time.perf_counter();actual=np.empty((len(inputs),2),np.float32)
    assert lib.t98_hybrid_logits(handle,inputs.ctypes.data,len(inputs),actual.ctypes.data)==0
    elapsed=time.perf_counter()-t0
    with torch.inference_mode(): expected=model(torch.from_numpy(inputs)).numpy()
    delta=float(np.max(np.abs(expected-actual)))
    assert np.isfinite(actual).all()
    np.testing.assert_allclose(actual,expected,rtol=2e-5,atol=2e-4)
    np.testing.assert_array_equal(actual.argmax(1),expected.argmax(1))
    report=dict(inputs=len(inputs),max_absolute_logit_error=delta,native_ms_per_block=elapsed*1000/len(inputs),classifications_match=True)
    print(json.dumps(report,indent=2));(root/'build/native-model-results.json').write_text(json.dumps(report,indent=2))
finally: lib.t98_secret_destroy(handle)
