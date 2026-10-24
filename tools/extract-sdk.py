"""Extract reference assemblies from a local SDR# .NET bundle (never redistributed)."""
import argparse
import io
from pathlib import Path
import struct
import zlib

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('executable',type=Path)
    parser.add_argument('--output',type=Path,default=Path('build/sdk'))
    args=parser.parse_args()
    data=args.executable.read_bytes()
    signature=bytes.fromhex('8b1202b96a612038727b930214d7a03213f5b9e6efae3318ee3b2dce24b36aae')
    position=data.find(signature)
    if position<8:raise ValueError('Not a .NET single-file bundle')
    offset=struct.unpack_from('<q',data,position-8)[0]
    if not 0<offset<len(data):raise ValueError('Invalid manifest offset')
    stream=io.BytesIO(data);stream.seek(offset)
    def read(fmt):return struct.unpack(fmt,stream.read(struct.calcsize(fmt)))
    def string():
        length=0
        for shift in range(0,35,7):
            b=read('<B')[0];length|=(b&127)<<shift
            if b<128:return stream.read(length).decode('utf-8')
        raise ValueError('Invalid string length')
    major,minor,count=read('<IIi');string()
    if major not in (2,6) or not 0<count<10000:raise ValueError('Unsupported bundle version')
    read('<qqqqQ')
    needed={'SDRSharp.Common.dll','SDRSharp.Radio.dll','SDRSharp.PanView.dll'}
    extracted=set();args.output.mkdir(parents=True,exist_ok=True)
    for _ in range(count):
        start,size=read('<qq');compressed=read('<q')[0] if major>=6 else 0
        read('<B');name=string()
        if name not in needed:continue
        length=compressed or size
        if min(start,size,length)<0 or start+length>len(data):raise ValueError('Invalid assembly extent')
        content=data[start:start+length]
        if compressed:content=zlib.decompress(content,-15)
        if len(content)!=size:raise ValueError('Assembly size mismatch')
        (args.output/name).write_bytes(content);extracted.add(name)
    if extracted!=needed:raise ValueError('Required SDR# interfaces were not found')
    print('Prepared SDR# build references.')
if __name__=='__main__':main()
