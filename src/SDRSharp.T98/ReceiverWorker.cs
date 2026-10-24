using System.Buffers;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json;
namespace SDRSharp.T98;

internal sealed record Snapshot(ReceiverStats Stats,float[] Eye,string Status,string Privacy,string Csm,
    int? User,int? Maker,int? Call,int? Mode,long Drops,ulong AudioDrops);
internal sealed record EyeBatch(long Epoch,long Timestamp,float[] Traces);
internal sealed class ReceiverWorker : IDisposable {
    private sealed record Block(float[] Data,int Count,double Rate,long Epoch);
    private readonly BlockingCollection<Block> queue=new(512);
    private readonly Thread thread;
    private readonly SecretWorker secret=new();
    private Settings settings=new();
    private double rate;
    private long epoch,drops,queuedSamples,session;
    private int enabled,sourceRunning,disposed;
    public readonly ConcurrentQueue<EyeBatch> EyeBatches=new();
    public long Epoch=>Interlocked.Read(ref epoch);
    public Snapshot Snapshot=new(default,[],"停止中","待機中","—",null,null,null,null,0,0);
    public bool Enabled {get=>Volatile.Read(ref enabled)!=0;set {if(Interlocked.Exchange(ref enabled,value?1:0)!=(value?1:0)) SignalChanged();}}
    public bool SourceRunning {get=>Volatile.Read(ref sourceRunning)!=0;set {if(Interlocked.Exchange(ref sourceRunning,value?1:0)!=(value?1:0)) SignalChanged();}}
    public double SampleRate {set {if(Volatile.Read(ref rate)!=value) {Volatile.Write(ref rate,value);SignalChanged();}}}
    public void Configure(Settings next) => Volatile.Write(ref settings,next);
    public void SignalChanged() => Interlocked.Increment(ref epoch);
    public ReceiverWorker() {thread=new(Run) {IsBackground=true,Name="T98 selected-channel receiver"};thread.Start();}
    public void Push(ReadOnlySpan<float> iq) {
        if(!Enabled || !SourceRunning || Volatile.Read(ref disposed)!=0 || iq.Length==0) return;
        double r=Volatile.Read(ref rate);long generation=Interlocked.Read(ref epoch);
        if(!double.IsFinite(r) || r<12000 || r>3200000) {
            Volatile.Write(ref Snapshot,Snapshot with {Status="IQレートが範囲外です (12 kHz～3.2 MHz)"});return;
        }
        // Split large host blocks to bound processing and native output queues.
        for(int offset=0;offset<iq.Length;offset+=8192) {
            int count=Math.Min(8192,iq.Length-offset);
            if(Interlocked.Add(ref queuedSamples,count/2)>r*.75) {
                Interlocked.Add(ref queuedSamples,-count/2);Interlocked.Add(ref drops,(iq.Length-offset)/2);SignalChanged();return;
            }
            var data=ArrayPool<float>.Shared.Rent(count);iq.Slice(offset,count).CopyTo(data);
            try {if(queue.TryAdd(new(data,count,r,generation))) continue;} catch(InvalidOperationException) {}
            ArrayPool<float>.Shared.Return(data);Interlocked.Add(ref queuedSamples,-count/2);Interlocked.Add(ref drops,count/2);SignalChanged();return;
        }
    }
    private void Run() {
        NativeReceiver? receiver=null;WaveOutput? audio=null;Recording? recording=null;
        var window=new Queue<Frame>();long burstCount=0,currentEpoch=-1;ulong? lastSymbol=null;
        bool? privateCall=null;int? appliedKey=null;int device=int.MinValue;
        string csm="—",recordingFolder="";int? user=null,maker=null,call=null,mode=null;long nextUi=0,nextEye=0;float[] latestEye=[];
        void ResetCall() {session++;window.Clear();burstCount=0;lastSymbol=null;privateCall=null;appliedKey=null;receiver?.ResetVoice();audio?.Clear();}
        void Suspend() {receiver?.Dispose();receiver=null;audio?.Dispose();audio=null;recording?.Dispose();recording=null;ResetCall();latestEye=[];while(EyeBatches.TryDequeue(out _)){} }
        try {
            while(!queue.IsCompleted) {
                if(!Enabled || !SourceRunning) {if(receiver!=null || recording!=null) Suspend();}
                Settings options=Volatile.Read(ref settings);
                if((!options.Record || options.Folder!=recordingFolder) && recording!=null) {recording.Dispose();recording=null;}
                if(!queue.TryTake(out var block,100)) continue;
                Interlocked.Add(ref queuedSamples,-block.Count/2);
                try {
                    if(!Enabled || !SourceRunning || block.Epoch!=Interlocked.Read(ref epoch)) continue;
                    if(receiver==null || currentEpoch!=block.Epoch) {
                        Suspend();receiver=new(block.Rate);currentEpoch=block.Epoch;csm="—";user=maker=call=mode=null;
                    }
                    if(!options.Playback || (audio!=null && device!=options.Device)) {audio?.Dispose();audio=null;}
                    if(options.Playback && audio==null) {audio=new(options.Device);device=options.Device;}
                    if(options.Record && recording==null) {recording=new(options.Folder);recordingFolder=options.Folder;}
                    receiver.Push(block.Data.AsSpan(0,block.Count));
                    if(!Enabled || !SourceRunning || block.Epoch!=Interlocked.Read(ref epoch)) continue;
                    Frame? frame;
                    while((frame=receiver.Read())!=null) {
                        recording?.Frame(frame);
                        if(!frame.Traffic) {ResetCall();if(frame.Crc && frame.Csm.Length>0) csm=frame.Csm;continue;}
                        if(lastSymbol.HasValue && frame.Symbol!=lastSymbol.Value+192) ResetCall();
                        lastSymbol=frame.Symbol;
                        if(frame.Crc) {
                            bool next=frame.Call==1;
                            if(privateCall.HasValue && privateCall.Value!=next) {ResetCall();lastSymbol=frame.Symbol;}
                            privateCall=next;user=frame.User;maker=frame.Maker;call=frame.Call;mode=frame.Mode;
                        }
                        // Unknown/bad control CRC never silently selects a plaintext decoder.
                        bool search=privateCall!=false;
                        if(search) {
                            window.Enqueue(frame);burstCount++;while(window.Count>64) window.Dequeue();
                            secret.Submit(new(session,burstCount-window.Count,window.SelectMany(f=>f.Payload).ToArray(),window.SelectMany(f=>f.Llr).ToArray()));
                        }
                        var answer=Volatile.Read(ref secret.Result);
                        int? key=!search?0:answer.Session==session?answer.Key:null;
                        int series=!search?0:answer.Series ?? 0;
                        if(appliedKey!=key) {receiver.ResetVoice();audio?.Clear();appliedKey=key;}
                        // Do not play scrambled speech while inference is pending.
                        var pcm=key.HasValue?receiver.Decode(frame,series,key.Value):new float[640];
                        recording?.Audio(pcm);
                        recording?.Privacy(session,series,key);
                        if(audio!=null) for(int f=0;f<4;f++) {
                            float[] part=pcm.AsSpan(f*160,160).ToArray();
                            for(int i=0;i<part.Length;i++) part[i]*=options.Volume/100f;
                            audio.Write(part);
                        }
                    }
                    var stats=receiver.Stats();
                    if(stats.Locked==0 && lastSymbol.HasValue) {ResetCall();user=maker=call=mode=null;csm="—";}
                    long now=Stopwatch.GetTimestamp();
                    if(now>=nextEye) {
                        latestEye=receiver.DrainEye();
                        if(latestEye.Length>0) {
                            EyeBatches.Enqueue(new(currentEpoch,now,latestEye));
                            while(EyeBatches.Count>8) EyeBatches.TryDequeue(out _);
                        }
                        nextEye=now+Stopwatch.Frequency/60;
                    }
                    if(now>=nextUi) {
                        var answer=Volatile.Read(ref secret.Result);
                        string privacy=privateCall==false?"平文":answer.Session==session?answer.Status:
                            answer.Session<0?answer.Status:privateCall==true?"秘話を自動解析中":"判定待ち";
                        Volatile.Write(ref Snapshot,new(stats,latestEye,stats.Locked!=0?"受信中・同期":"信号を探索中",privacy,csm,user,maker,call,mode,Interlocked.Read(ref drops),audio?.DroppedBlocks??0));
                        nextUi=now+Stopwatch.Frequency/5;
                    }
                } catch(Exception e) {Volatile.Write(ref Snapshot,Snapshot with {Status=e.Message});Enabled=false;Suspend();}
                finally {ArrayPool<float>.Shared.Return(block.Data);}
            }
        } catch(Exception e) {Volatile.Write(ref Snapshot,Snapshot with {Status=e.Message});Enabled=false;}
        finally {Suspend();}
    }
    public void Dispose() {
        if(Interlocked.Exchange(ref disposed,1)!=0) return;
        Enabled=false;queue.CompleteAdding();thread.Join();queue.Dispose();secret.Dispose();
    }
}

internal sealed class Recording : IDisposable {
    private readonly BinaryWriter bursts,llr;
    private readonly StreamWriter events;
    private readonly WaveWriter wave;
    private (long,int,int?)? lastPrivacy;
    public Recording(string folder) {
        if(string.IsNullOrWhiteSpace(folder)) throw new IOException("録音先フォルダーを選んでください");
        Directory.CreateDirectory(folder);
        string stem=Path.Combine(folder,$"T98-{DateTime.Now:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}");
        // Every stream uses CreateNew; no existing capture can be overwritten.
        bursts=new(new FileStream(stem+".burst",FileMode.CreateNew));
        try {
            llr=new(new FileStream(stem+".llr",FileMode.CreateNew));
            events=new(new FileStream(stem+".jsonl",FileMode.CreateNew));wave=new(stem+".wav");
        } catch {bursts.Dispose();llr?.Dispose();events?.Dispose();throw;}
    }
    public void Frame(Frame f) {
        events.WriteLine(JsonSerializer.Serialize(new {utc=DateTimeOffset.UtcNow,f.Symbol,f.Traffic,f.Crc,f.User,f.Maker,f.Call,f.Mode,f.Csm}));
        if(!f.Traffic) return;
        bursts.Write((byte)0xff);for(int i=0;i<4;i++) {bursts.Write((byte)0x48);bursts.Write(f.Payload,i*9,9);}llr.Write(f.Llr);
    }
    public void Audio(float[] pcm)=>wave.Write(pcm);
    public void Privacy(long session,int series,int? key) {
        var value=(session,series,key);
        if(lastPrivacy==value) return;
        lastPrivacy=value;events.WriteLine(JsonSerializer.Serialize(new {utc=DateTimeOffset.UtcNow,session,series,key}));
    }
    public void Dispose() {try {wave.Dispose();} finally {bursts.Dispose();llr.Dispose();events.Dispose();}}
}
