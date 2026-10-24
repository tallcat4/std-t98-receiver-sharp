using System.Runtime.InteropServices;
namespace SDRSharp.T98;

internal sealed class WaveOutput : IDisposable {
    [StructLayout(LayoutKind.Sequential, Pack = 2)]
    private struct Format {
        public ushort Tag, Channels;
        public uint Rate, BytesPerSecond;
        public ushort Align, Bits, Extra;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct Header {
        public IntPtr Data;
        public uint Length, Recorded;
        public UIntPtr User;
        public uint Flags, Loops;
        public IntPtr Next;
        public UIntPtr Reserved;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct Capabilities {
        public ushort Manufacturer, Product;
        public uint Version;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string Name;
        public uint Formats;
        public ushort Channels, Reserved;
        public uint Support;
    }
    [DllImport("winmm.dll")]
    private static extern uint waveOutOpen(out IntPtr handle, uint device, ref Format format, IntPtr callback,
                                           IntPtr instance, uint flags);
    [DllImport("winmm.dll")]
    private static extern uint waveOutPrepareHeader(IntPtr handle, IntPtr header, uint size);
    [DllImport("winmm.dll")]
    private static extern uint waveOutUnprepareHeader(IntPtr handle, IntPtr header, uint size);
    [DllImport("winmm.dll")]
    private static extern uint waveOutWrite(IntPtr handle, IntPtr header, uint size);
    [DllImport("winmm.dll")]
    private static extern uint waveOutReset(IntPtr handle);
    [DllImport("winmm.dll")]
    private static extern uint waveOutClose(IntPtr handle);
    [DllImport("winmm.dll")]
    private static extern uint waveOutGetNumDevs();
    [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
    private static extern uint waveOutGetDevCapsW(UIntPtr device, out Capabilities caps, uint size);
    private IntPtr handle;
    private readonly short[] pcm = new short[160];
    private readonly List<(IntPtr Header, IntPtr Data)> buffers = [];
    private static readonly uint HeaderSize = (uint)Marshal.SizeOf<Header>();
    public ulong DroppedBlocks { get; private set; }
    public static string[] Devices() {
        var names = new List<string> { "システムの既定" };
        for (uint i = 0; i < waveOutGetNumDevs(); i++) {
            var result = waveOutGetDevCapsW((UIntPtr)i, out var c, (uint)Marshal.SizeOf<Capabilities>());
            names.Add(result == 0 ? c.Name : $"Device {i}");
        }
        return names.ToArray();
    }
    private static void Check(uint code) {
        if (code != 0)
            throw new IOException($"Windows audio error {code}.");
    }
    public WaveOutput(int device) {
        Format format =
            new() { Tag = 1, Channels = 1, Rate = 8000, BytesPerSecond = 16000, Align = 2, Bits = 16 };
        Check(waveOutOpen(out handle, device < 0 ? uint.MaxValue : checked((uint)device), ref format,
                          IntPtr.Zero, IntPtr.Zero, 0));
        try {
            for (int i = 0; i < 48; i++) {
                IntPtr data = Marshal.AllocHGlobal(320), header = Marshal.AllocHGlobal((int)HeaderSize);
                Marshal.StructureToPtr(new Header { Data = data, Length = 320 }, header, false);
                buffers.Add((header, data));
                Check(waveOutPrepareHeader(handle, header, HeaderSize));
            }
        } catch {
            Dispose();
            throw;
        }
    }
    public void Write(float[] samples) {
        if (samples.Length != 160)
            throw new ArgumentException("Expected 20 ms audio block.");
        foreach (var b in buffers) {
            var header = Marshal.PtrToStructure<Header>(b.Header);
            if ((header.Flags & 16) != 0)
                continue; // WHDR_INQUEUE
            for (int i = 0; i < 160; i++)
                pcm[i] = (short)Math.Clamp((int)Math.Round(samples[i] * 32767), -32768, 32767);
            Marshal.Copy(pcm, 0, b.Data, 160);
            Check(waveOutWrite(handle, b.Header, HeaderSize));
            return;
        }
        DroppedBlocks++;
    }
    public void Clear() {
        if (handle != IntPtr.Zero)
            Check(waveOutReset(handle));
    }
    public void Dispose() {
        if (handle == IntPtr.Zero)
            return;
        waveOutReset(handle);
        foreach (var b in buffers) {
            waveOutUnprepareHeader(handle, b.Header, HeaderSize);
            Marshal.FreeHGlobal(b.Header);
            Marshal.FreeHGlobal(b.Data);
        }
        buffers.Clear();
        waveOutClose(handle);
        handle = IntPtr.Zero;
    }
}
