using System.Runtime.InteropServices;
namespace SDRSharp.T98;

internal sealed record SecretResult(long Session,int? Series,int? Key,string Status,bool Cached=false);
internal sealed record SearchRequest(long Session,long First,byte[] Payload,byte[] Llr);
internal sealed unsafe class SecretWorker : IDisposable {
    [StructLayout(LayoutKind.Sequential)]
    private struct ResultNative {
        public long Session;
        public ulong Next;
        public int Series,Key;
        public double Score,Margin;
    }
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern IntPtr t98_secret_create();
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern void t98_secret_destroy(IntPtr h);
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern IntPtr t98_secret_error();
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern int t98_secret_set_cache(IntPtr h,int* pairs,int count);
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern int t98_secret_cache_hit(IntPtr h);
    [DllImport("t98_native",CallingConvention=CallingConvention.Cdecl)] private static extern int t98_secret_process(IntPtr h,long session,ulong first,byte* llr,int count,out ResultNative result);
    private readonly object gate=new();
    private readonly AutoResetEvent wake=new(false);
    private readonly Thread thread;
    private readonly string? cachePath;
    private SearchRequest? pending;
    private bool stopping;
    public SecretResult Result=new(-1,null,null,"準備中");
    public static string FormatCode(int series,int key)=>series==0?$"{key:00000}":$"{series:00}-{key:00000}";
    public SecretWorker(string? cachePath=null) {
        this.cachePath=cachePath;
        thread=new(Run) {IsBackground=true,Name="T98 native privacy",Priority=ThreadPriority.BelowNormal};thread.Start();
    }
    public void Submit(SearchRequest request) {lock(gate) {if(stopping)return;pending=request;}wake.Set();}
    private void Run() {
        IntPtr engine=IntPtr.Zero;
        try {
            engine=t98_secret_create();
            if(engine==IntPtr.Zero) throw new InvalidOperationException(Marshal.PtrToStringUTF8(t98_secret_error()));
            var cache=new SecretCache(cachePath);int[] pairs=cache.Pairs();
            fixed(int* data=pairs)
                if(t98_secret_set_cache(engine,data,pairs.Length/2)!=0)
                    throw new InvalidOperationException(Marshal.PtrToStringUTF8(t98_secret_error()));
            Volatile.Write(ref Result,new(-1,null,null,"待機中"));
            while(true) {
                SearchRequest? request;
                lock(gate) {if(stopping) break;request=pending;pending=null;}
                if(request==null) {wake.WaitOne(100);continue;}
                ResultNative answer;
                fixed(byte* data=request.Llr)
                    if(t98_secret_process(engine,request.Session,checked((ulong)request.First),data,request.Llr.Length/288,out answer)!=0)
                        throw new InvalidOperationException(Marshal.PtrToStringUTF8(t98_secret_error()));
                if(answer.Key>0)cache.Remember(answer.Series,answer.Key);
                string status=answer.Key<0?"自動解析中":answer.Key==0?"平文":FormatCode(answer.Series,answer.Key);
                if(cache.Warning!=null)status+="\n"+cache.Warning;
                Volatile.Write(ref Result,new(answer.Session,answer.Series<0?null:answer.Series,answer.Key<0?null:answer.Key,status,t98_secret_cache_hit(engine)!=0));
            }
        } catch(Exception e) {Volatile.Write(ref Result,new(-1,null,null,"秘話エンジン: "+e.Message));}
        finally {if(engine!=IntPtr.Zero)t98_secret_destroy(engine);}
    }
    public void Dispose() {lock(gate)stopping=true;wake.Set();thread.Join();wake.Dispose();}
}
