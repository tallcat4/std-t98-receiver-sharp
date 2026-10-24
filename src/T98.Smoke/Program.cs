using System.ComponentModel;
using System.Diagnostics;
using System.Reflection;
using System.Text.Json;
using SDRSharp.Common;
using SDRSharp.Radio;
using SDRSharp.T98;

public class HostProxy : DispatchProxy {
    public Dictionary<string,object?> Values=new() {{"Frequency",351200000L},{"CenterFrequency",351200000L},{"IsPlaying",false},{"AudioIsMuted",false}};
    public IIQProcessor? Processor;
    private PropertyChangedEventHandler? changed;
    public void Tune(long value) {Values["Frequency"]=value;changed?.Invoke(this,new("Frequency"));}
    protected override object? Invoke(MethodInfo? m,object?[]? args) {
        string n=m!.Name;
        if(n=="add_PropertyChanged") {changed+=(PropertyChangedEventHandler)args![0]!;return null;}
        if(n=="remove_PropertyChanged") {changed-=(PropertyChangedEventHandler)args![0]!;return null;}
        if(n=="RegisterStreamHook") {Processor=(IIQProcessor)args![0]!;Processor.SampleRate=48000;return null;}
        if(n=="UnregisterStreamHook") {Processor=null;return null;}
        if(n=="StartRadio") {Values["IsPlaying"]=true;changed?.Invoke(this,new("StartRadio"));return null;}
        if(n.StartsWith("set_")) {Values[n[4..]]=args![0];return null;}
        if(n.StartsWith("get_")&&Values.TryGetValue(n[4..],out var v)) return v;
        return m.ReturnType!=typeof(void)&&m.ReturnType.IsValueType?Activator.CreateInstance(m.ReturnType):null;
    }
}
internal static class Program {
    private static void Check(bool ok,string message) {if(!ok) throw new Exception(message);}
    private static IEnumerable<Control> Walk(Control c) {foreach(Control child in c.Controls) {yield return child;foreach(var sub in Walk(child)) yield return sub;}}
    [STAThread]
    private static unsafe int Main(string[] args) {
        try {
            string root=Path.GetFullPath(args[0]);string outdir=Path.Combine(root,"build","smoke-"+DateTime.Now.ToString("yyyyMMdd-HHmmss"));Directory.CreateDirectory(outdir);
            Environment.SetEnvironmentVariable("T98_SETTINGS_PATH",Path.Combine(outdir,"settings.json"));
            CheckCache(root,outdir);
            new Settings(Playback:false).Save();
            var host=DispatchProxy.Create<ISharpControl,HostProxy>();var proxy=(HostProxy)(object)host;
            var plugin=new T98Plugin();plugin.Initialize(host);
            Check(SecretWorker.FormatCode(0,12345)=="12345"&&SecretWorker.FormatCode(1,12345)=="01-12345"&&SecretWorker.FormatCode(10,12345)=="10-12345","Code format failed");
            using var form=new Form {Size=new(380,960),Text="STD-T98 Receiver — test host"};
            form.Controls.Add(plugin.Gui);plugin.Gui.Dock=DockStyle.Fill;form.Show();Application.DoEvents();
            var panel=(ReceiverPanel)plugin.Gui;
            panel.Apply(new Settings(Playback:false,Record:true,Folder:outdir));
            var worker=(ReceiverWorker)typeof(T98Plugin).GetField("worker",BindingFlags.NonPublic|BindingFlags.Instance)!.GetValue(plugin)!;
            worker.Configure(panel.Read());
            var button=Walk(panel).OfType<Button>().Single(x=>x.Name=="StartReception");button.PerformClick();
            Check(host.IsPlaying&&host.AudioIsMuted&&plugin.Enabled,"One-button start/mute failed");
            byte[] bytes=File.ReadAllBytes(Path.Combine(root,"build/synthetic.cf32"));float[] samples=new float[bytes.Length/4];Buffer.BlockCopy(bytes,0,samples,0,bytes.Length);
            for(int i=0;i<samples.Length;i+=960) {
                int count=Math.Min(960,samples.Length-i);
                fixed(float* p=&samples[i]) plugin.Process((Complex*)p,count/2);
                Thread.Sleep(10);Application.DoEvents();
            }
            Thread.Sleep(300);Application.DoEvents();
            var state=Volatile.Read(ref worker.Snapshot);
            Check(state.Stats.CrcOk>=30,"IQ -> frame CRC pipeline failed");
            Check(state.Csm=="123456789"&&state.User==123&&state.Maker==42,"Metadata display failed");
            Check(state.Eye.Length>0,"Eye diagram missing");
            Check(state.Privacy=="平文","Plaintext automatic selection failed");
            Check(state.Drops==0,"Unexpected IQ drops");
            using(var bitmap=new Bitmap(form.Width,form.Height)) {form.DrawToBitmap(bitmap,form.ClientRectangle);bitmap.Save(Path.Combine(outdir,"panel.png"));}
            button.PerformClick();Application.DoEvents();Thread.Sleep(200);
            Check(!plugin.Enabled&&!host.AudioIsMuted,"Stop/mute restore failed");
            var burst=Directory.GetFiles(outdir,"*.burst").Single();
            Check(new FileInfo(burst).Length==28*41,"Burst recording format/count mismatch");
            Check(new FileInfo(Path.ChangeExtension(burst,".llr")).Length==28*288,"LLR alignment failed");
            var wav=File.ReadAllBytes(Path.ChangeExtension(burst,".wav"));
            Check(wav.Length==44+28*1280&&BitConverter.ToUInt32(wav,40)==28*1280,"WAV finalization failed");
            // A new PTT must find its own key in native code while IQ keeps flowing.
            proxy.Tune(351206250);
            worker.Configure(new Settings(Playback:false));
            button.PerformClick();
            bytes=File.ReadAllBytes(Path.Combine(root,"build/secret.cf32"));samples=new float[bytes.Length/4];Buffer.BlockCopy(bytes,0,samples,0,bytes.Length);
            bool acquired=false;
            var eye=Walk(panel).OfType<EyeView>().Single();long paintedBefore=eye.PaintedFrames;
            var eyeWatch=Stopwatch.StartNew();
            for(int i=0;i<samples.Length;i+=960) {
                int count=Math.Min(960,samples.Length-i);
                fixed(float* p=&samples[i]) plugin.Process((Complex*)p,count/2);
                Thread.Sleep(10);Application.DoEvents();
                acquired|=Volatile.Read(ref worker.Snapshot).Privacy=="10-12345";
            }
            Check(acquired,"Live automatic privacy acquisition failed: "+worker.Snapshot.Privacy);
            Check(worker.Snapshot.Drops==0,"Privacy worker blocked IQ input");
            double fps=(eye.PaintedFrames-paintedBefore)/eyeWatch.Elapsed.TotalSeconds;
            Check(fps>=23,$"Eye refresh too slow: {fps:F1} fps");
            Check(eye.TraceCount>0&&eye.TraceCount<=200,"Rolling eye window is empty or unbounded");
            using(var bitmap=new Bitmap(form.Width,form.Height)) {form.DrawToBitmap(bitmap,form.ClientRectangle);bitmap.Save(Path.Combine(outdir,"secret-panel.png"));}
            proxy.Tune(351212500);
            for(int i=0;i<20;i++) {var zeros=new float[960];fixed(float* p=zeros) plugin.Process((Complex*)p,480);Thread.Sleep(10);Application.DoEvents();}
            Check(worker.Snapshot.Privacy!="10-12345","Stale key survived tuning");
            button.PerformClick();
            // Overshoot must not move the fixed display axes.
            float[] high=Enumerable.Range(0,64).Select(i=>(float)(9*Math.Sin(i*Math.PI/31.5))).ToArray();
            eye.Advance([new(999,Stopwatch.GetTimestamp(),high)],999,true);
            Check(eye.DisplayRange==9&&eye.BackColor==Color.Black,"Fixed eye range/background failed");
            using(var bitmap=new Bitmap(eye.Width,eye.Height)) {eye.DrawToBitmap(bitmap,eye.ClientRectangle);bitmap.Save(Path.Combine(outdir,"eye-overshoot.png"));}
            eye.Advance([],1000,false);Check(eye.TraceCount==0,"Retuning retained old eye traces");
            plugin.Close();Check(proxy.Processor==null,"Stream hook not removed");
            Console.WriteLine($"PASS: cache persistence/restart/failure handling, host lifecycle, IQ/CRC/CSM, recording, native 10-12345, stale-key rejection; rolling eye {fps:F1} fps, fixed +/-9 range. "+outdir);
            return 0;
        } catch(Exception e) {Console.Error.WriteLine(e);return 1;}
    }
    private static void CheckCache(string root,string outdir) {
        Check(SecretCache.DefaultPath==Path.Combine(Path.GetDirectoryName(typeof(T98Plugin).Assembly.Location)!,"secret-cache.json"),"Cache is not beside the plugin");
        string path=Path.Combine(outdir,"secret-cache.json");
        byte[] llr=File.ReadAllBytes(Path.Combine(root,"build/cache-check.llr"));
        SecretResult Search(SecretWorker worker,long session) {
            worker.Submit(new(session,0,[],llr));
            var watch=Stopwatch.StartNew();SecretResult result;
            do {Thread.Sleep(10);result=Volatile.Read(ref worker.Result);}while(result.Session!=session&&watch.ElapsedMilliseconds<5000);
            Check(result.Key==12345&&result.Series==10,"Worker cache test did not acquire key: "+result.Status);return result;
        }
        using(var first=new SecretWorker(path))Check(!Search(first,1).Cached,"Empty cache unexpectedly hit");
        Check(File.Exists(path),"Confirmed key was not persisted");
        Check(new SecretCache(path).Pairs().SequenceEqual(new[]{10,12345}),"Cache code roundtrip failed");
        using(var restarted=new SecretWorker(path))Check(Search(restarted,2).Cached,"Restart did not use Hybrid cache path");
        var cache=new SecretCache(path);cache.Remember(0,12345);cache.Remember(1,12345);cache.Remember(10,12345);cache.Remember(0,0);cache.Remember(16,12345);
        Check(new SecretCache(path).Pairs().SequenceEqual(new[]{0,12345,1,12345,10,12345}),"Cache deduplication/range/plaintext exclusion failed");
        string broken=Path.Combine(outdir,"corrupt-cache.json");File.WriteAllText(broken,"broken json");
        var corrupt=new SecretCache(broken);corrupt.Remember(10,12345);
        Check(corrupt.Warning!=null&&File.ReadAllText(broken)=="broken json","Corrupt cache was overwritten");
        using(var fallback=new SecretWorker(broken))Check(!Search(fallback,3).Cached,"Corrupt cache prevented fallback");
        string blocked=Path.Combine(outdir,"blocked-cache.json");Directory.CreateDirectory(blocked);
        using(var fallback=new SecretWorker(blocked)) {
            var result=Search(fallback,4);Check(result.Status.Contains("キャッシュ保存不可"),"Failed cache write was not reported");
        }
    }
}
