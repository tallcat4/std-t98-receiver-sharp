using System.Text;
using System.Buffers.Binary;
namespace SDRSharp.T98;

public sealed class WaveWriter : IDisposable {
    private readonly BinaryWriter writer;
    private readonly int rate;
    private long samples;
    private bool disposed;
    public WaveWriter(string path, int sampleRate = 8000) {
        rate = sampleRate;
        writer = new BinaryWriter(new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read));
        Header();
    }
    private void Header() {
        writer.BaseStream.Position = 0;
        writer.Write(Encoding.ASCII.GetBytes("RIFF"));
        writer.Write(checked((uint)(36 + samples * 2)));
        writer.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
        writer.Write(16u);
        writer.Write((ushort)1);
        writer.Write((ushort)1);
        writer.Write(rate);
        writer.Write(rate * 2);
        writer.Write((ushort)2);
        writer.Write((ushort)16);
        writer.Write(Encoding.ASCII.GetBytes("data"));
        writer.Write(checked((uint)(samples * 2)));
    }
    public void Write(ReadOnlySpan<float> audio) {
        ObjectDisposedException.ThrowIf(disposed, this);
        if ((samples + audio.Length) * 2 > uint.MaxValue - 36)
            throw new IOException("WAV reached its 4 GiB limit. Start a new recording.");
        Span<byte> block = stackalloc byte[1024];
        for (int start = 0; start < audio.Length;) {
            int count = Math.Min(512, audio.Length - start);
            for (int i = 0; i < count; i++) {
                short pcm = (short)Math.Clamp((int)Math.Round(audio[start + i] * 32767), -32768, 32767);
                BinaryPrimitives.WriteInt16LittleEndian(block.Slice(2 * i, 2), pcm);
            }
            writer.Write(block[..(2 * count)]);
            start += count;
        }
        samples += audio.Length;
    }
    public void Dispose() {
        if (disposed)
            return;
        disposed = true;
        try {
            Header();
        } finally {
            writer.Dispose();
        }
    }
}
