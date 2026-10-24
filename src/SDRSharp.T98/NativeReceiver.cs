using System.Runtime.InteropServices;
namespace SDRSharp.T98;

[StructLayout(LayoutKind.Sequential)]
internal struct ReceiverStats {
    public ulong Samples, Frames, CrcOk;
    public double Power, FrequencyError;
    public int Locked, Active;
}
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeFrame {
    public ulong Symbol;
    public int Traffic, Crc, User, Maker, Call, Mode;
    public fixed byte Csm[16];
    public fixed byte Payload[36];
    public fixed byte Llr[288];
    public Frame Copy() {
        fixed (byte* c=Csm, p=Payload, l=Llr)
            return new(Symbol,Traffic!=0,Crc!=0,User,Maker,Call,Mode,
                Marshal.PtrToStringAnsi((IntPtr)c) ?? "",new ReadOnlySpan<byte>(p,36).ToArray(),new ReadOnlySpan<byte>(l,288).ToArray());
    }
}
internal sealed record Frame(ulong Symbol,bool Traffic,bool Crc,int User,int Maker,int Call,int Mode,string Csm,byte[] Payload,byte[] Llr);
internal sealed unsafe class NativeReceiver : IDisposable {
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern IntPtr t98_create(double rate);
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern void t98_destroy(IntPtr h);
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern int t98_process(IntPtr h,float* iq,int count);
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern int t98_read(IntPtr h,out NativeFrame frame);
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern int t98_status(IntPtr h,out ReceiverStats stats,float* eye,int count);
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern IntPtr t98_error();
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern int t98_eye_read(IntPtr h,float* eye,int count);
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern int t98_voice(IntPtr h,byte* llr,int series,int key,short* pcm);
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern void t98_voice_reset(IntPtr h);
    private IntPtr handle;
    public NativeReceiver(double rate) { handle=t98_create(rate); if(handle==IntPtr.Zero) throw new InvalidOperationException(Marshal.PtrToStringUTF8(t98_error())); }
    public void Push(ReadOnlySpan<float> iq) {
        fixed(float* p=iq) if(t98_process(handle,p,iq.Length/2)!=0) throw new InvalidOperationException(Marshal.PtrToStringUTF8(t98_error()));
    }
    public Frame? Read() => t98_read(handle,out var frame)>0 ? frame.Copy() : null;
    public void ResetVoice() => t98_voice_reset(handle);
    public float[] Decode(Frame frame,int series,int key) {
        var pcm=new short[640];
        fixed(byte* p=frame.Llr) fixed(short* o=pcm)
            if(t98_voice(handle,p,series,key,o)!=0) throw new InvalidOperationException("音声復号に失敗しました");
        return Array.ConvertAll(pcm,x=>x/32768f);
    }
    public (ReceiverStats Stats,float[] Eye) Snapshot() {
        var eye=new float[32*64]; int n;
        ReceiverStats stats;
        fixed(float* p=eye) n=t98_status(handle,out stats,p,eye.Length);
        return (stats,eye[..Math.Max(0,n)]);
    }
    public ReceiverStats Stats() {t98_status(handle,out var stats,null,0);return stats;}
    public float[] DrainEye() {
        var eye=new float[256*64];int count;
        fixed(float* data=eye) count=t98_eye_read(handle,data,eye.Length);
        return count>0?eye[..count]:[];
    }
    public void Dispose() { if(handle!=IntPtr.Zero) t98_destroy(handle);handle=IntPtr.Zero; }
}
