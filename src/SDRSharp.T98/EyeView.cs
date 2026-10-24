using System.Diagnostics;
using System.Drawing.Drawing2D;
namespace SDRSharp.T98;

internal sealed class EyeView : Control {
    private sealed record Trace(float[] Values,long Timestamp);
    private readonly Queue<Trace> traces=new();
    private readonly PointF[] points=new PointF[64];
    private long epoch=-1;
    private const double range=9;
    internal const double WindowSeconds=.3;
    internal double DisplayRange=>range;
    internal int TraceCount=>traces.Count;
    internal long PaintedFrames {get;private set;}
    public EyeView() {
        SetStyle(ControlStyles.AllPaintingInWmPaint|ControlStyles.UserPaint|ControlStyles.OptimizedDoubleBuffer|ControlStyles.ResizeRedraw,true);
        Height=210;MinimumSize=new(180,180);BackColor=Color.Black;ForeColor=Color.FromArgb(201,217,228);
    }
    public void Advance(IEnumerable<EyeBatch> batches,long generation,bool running) {
        long now=Stopwatch.GetTimestamp();
        if(epoch!=generation) {traces.Clear();epoch=generation;}
        foreach(var batch in batches) {
            if(batch.Epoch!=epoch) continue;
            int count=batch.Traces.Length/64;
            for(int i=0;i<count;i++) {
                long time=batch.Timestamp-(long)((count-i-1)*Stopwatch.Frequency/600.0);
                if((now-time)/(double)Stopwatch.Frequency>WindowSeconds) continue;
                float[] values=batch.Traces.AsSpan(i*64,64).ToArray();
                if(values.Any(v=>!float.IsFinite(v))) continue;
                traces.Enqueue(new(values,time));
            }
        }
        while(traces.TryPeek(out var trace) && (now-trace.Timestamp)/(double)Stopwatch.Frequency>WindowSeconds) traces.Dequeue();
        while(traces.Count>200) traces.Dequeue();
        // Fixed ±9 viewport, with decision-level labels only at ±1 and ±3.
        // Preserve sample amplitudes; drawing outside the viewport is clipped.
        Invalidate();
    }
    protected override void OnPaint(PaintEventArgs e) {
        base.OnPaint(e);
        long now=Stopwatch.GetTimestamp();PaintedFrames++;
        var g=e.Graphics;g.SmoothingMode=SmoothingMode.AntiAlias;
        var area=new RectangleF(32,29,Math.Max(1,Width-46),Math.Max(1,Height-39));
        float Y(double value)=>(float)(area.Top+area.Height*(range-value)/(range*2));
        using var subtle=new SolidBrush(Color.FromArgb(130,153,171));
        using var text=new SolidBrush(ForeColor);
        g.DrawString("アイパターン",Font,text,10,7);
        using var grid=new Pen(Color.FromArgb(41,59,75));
        using var zero=new Pen(Color.FromArgb(28,43,57));
        g.DrawLine(zero,area.Left,Y(0),area.Right,Y(0));
        foreach(int level in new[]{-3,-1,1,3}) {
            g.DrawLine(grid,area.Left,Y(level),area.Right,Y(level));
            g.DrawString(level>0?$"+{level}":level.ToString(),Font,subtle,5,Y(level)-8);
        }
        using var decision=new Pen(Color.FromArgb(110,151,158,115)) {DashStyle=DashStyle.Dash};
        foreach(float x in new[]{.25f,.75f})g.DrawLine(decision,area.Left+area.Width*x,area.Top,area.Left+area.Width*x,area.Bottom);
        var saved=g.Save();g.SetClip(area);
        foreach(var trace in traces) {
            double age=(now-trace.Timestamp)/(double)Stopwatch.Frequency;
            int alpha=Math.Clamp((int)(255*Math.Pow(Math.Max(0,1-age/WindowSeconds),2)),1,255);
            using var ink=new Pen(Color.FromArgb(alpha,255,230,0),1.2f);
            for(int i=0;i<64;i++)points[i]=new(area.Left+area.Width*i/63,Y(trace.Values[i]));
            g.DrawLines(ink,points);
        }
        g.Restore(saved);
    }
}
