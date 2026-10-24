using System.ComponentModel;
using System.Diagnostics;
using SDRSharp.Common;
using SDRSharp.Radio;
namespace SDRSharp.T98;

public sealed unsafe class T98Plugin : ISharpPlugin,IIQProcessor {
    private ISharpControl? host;
    private ReceiverPanel? panel;
    private ReceiverWorker? worker;
    private System.Windows.Forms.Timer? timer;
    private bool changedMute,oldMute;
    private DetectorType oldDetector;
    private int oldBandwidth;
    public string DisplayName=>"STD-T98 Receiver";
    public UserControl Gui=>panel??throw new InvalidOperationException("Not initialized");
    public bool Enabled {get=>worker?.Enabled??false;set {if(worker!=null) worker.Enabled=value;}}
    public double SampleRate {set {if(worker!=null) worker.SampleRate=value;}}
    public void Initialize(ISharpControl control) {
        if(host!=null) throw new InvalidOperationException("Already initialized");
        host=control;panel=new();worker=new();
        panel.Apply(Settings.Load());panel.SetFrequency(control.Frequency);
        worker.Configure(panel.Read());worker.SourceRunning=control.IsPlaying;
        panel.SettingsChanged+=()=>{
            var settings=panel.Read();worker.Configure(settings);
            try {settings.Save();} catch(Exception e) {panel.ShowError(e.Message);}
        };
        panel.ActiveChanged+=SetActive;
        control.RegisterStreamHook(this,ProcessorType.DecimatedAndFilteredIQ);
        control.PropertyChanged+=HostChanged;
        // A 25 ms request avoids the common ~47 ms quantization of a 33 ms
        // WinForms timer on Windows, keeping rendering near 30 fps.
        timer=new() {Interval=25};
        long nextText=0;
        timer.Tick+=(_,_)=>{
            var state=Volatile.Read(ref worker.Snapshot);
            var batches=new List<EyeBatch>();while(worker.EyeBatches.TryDequeue(out var batch))batches.Add(batch);
            panel.RefreshEye(batches,worker.Epoch,worker.Enabled&&worker.SourceRunning&&state.Stats.Active!=0);
            long now=Stopwatch.GetTimestamp();
            if(now>=nextText) {
                if(!worker.Enabled && state.Status is "受信中・同期" or "信号を探索中") state=state with {Status="停止中"};
                panel.RefreshState(state,worker.Enabled,worker.SourceRunning);nextText=now+Stopwatch.Frequency/5;
            }
            if(changedMute&&!worker.Enabled) RestoreMute();
        };
        timer.Start();
    }
    public void Process(Complex* buffer,int length) {
        if(buffer!=null && length>0 && length<=8388608) worker?.Push(new ReadOnlySpan<float>((float*)buffer,length*2));
    }
    private void SetActive(bool active) {
        if(host==null || worker==null) return;
        if(active) {
            oldMute=host.AudioIsMuted;host.AudioIsMuted=true;changedMute=true;
            oldDetector=host.DetectorType;oldBandwidth=host.FilterBandwidth;
            host.DetectorType=DetectorType.NFM;host.FilterBandwidth=8000;
            worker.SourceRunning=host.IsPlaying;worker.Enabled=true;
            // Start the SDR# source from the same button, if supported by this host.
            try {if(!host.IsPlaying) host.StartRadio();}
            catch {worker.Enabled=false;RestoreMute();throw;}
        } else {worker.Enabled=false;RestoreMute();}
    }
    private void RestoreMute() {
        if(changedMute && host!=null) {
            if(host.AudioIsMuted) host.AudioIsMuted=oldMute;
            if(host.DetectorType==DetectorType.NFM && host.FilterBandwidth==8000) {host.DetectorType=oldDetector;host.FilterBandwidth=oldBandwidth;}
            changedMute=false;
        }
    }
    private void HostChanged(object? sender,PropertyChangedEventArgs e) {
        if(worker==null || host==null) return;
        if(e.PropertyName is "StartRadio" or "StopRadio" or "IsPlaying") worker.SourceRunning=host.IsPlaying;
        if(e.PropertyName is "Frequency" or "CenterFrequency" or "InputSampleRate" or "Source") {
            worker.SignalChanged();
            if(panel?.IsHandleCreated==true) try {panel.BeginInvoke((Action)(()=>{if(host!=null) panel?.SetFrequency(host.Frequency);}));} catch(InvalidOperationException) {}
        }
    }
    public void Close() {
        timer?.Stop();timer?.Dispose();timer=null;
        if(host!=null) {host.PropertyChanged-=HostChanged;host.UnregisterStreamHook(this);}
        worker?.Dispose();worker=null;RestoreMute();panel?.Dispose();panel=null;host=null;
    }
}
