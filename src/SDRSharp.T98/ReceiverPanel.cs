namespace SDRSharp.T98;

internal sealed class ReceiverPanel : UserControl {
    private static readonly Color Background=Color.FromArgb(17,25,36),CardColor=Color.FromArgb(25,37,51),
        TextColor=Color.FromArgb(229,239,246),Muted=Color.FromArgb(145,164,181),Accent=Color.FromArgb(71,216,179);
    private readonly Button start=new() {Text="受信開始",Name="StartReception",Height=40,Dock=DockStyle.Fill};
    private readonly Label frequency=new() {Text="SDR# 選局チャネル",AutoSize=true};
    private readonly Label status=new() {Text="●  停止中",AutoSize=true};
    private readonly Label privacy=new() {Text="待機中",AutoSize=true};
    private readonly Label csm=new() {Text="—",AutoSize=true};
    private readonly Label user=new() {Text="—",AutoSize=true};
    private readonly Label maker=new() {Text="—",AutoSize=true};
    private readonly Label detail=new() {Text="呼種別 —  ·  モード —",AutoSize=true};
    private readonly Label quality=new() {AutoSize=true};
    private readonly EyeView eye=new() {Dock=DockStyle.Fill};
    private readonly ComboBox device=new() {DropDownStyle=ComboBoxStyle.DropDownList,Dock=DockStyle.Fill,FlatStyle=FlatStyle.Flat};
    private readonly CheckBox playback=new() {Text="音声再生",Checked=true,AutoSize=true};
    private readonly TrackBar volume=new() {Minimum=0,Maximum=100,Value=80,TickStyle=TickStyle.None,AutoSize=false,Height=28,Dock=DockStyle.Fill};
    private readonly Label volumeValue=new() {Text="80%",AutoSize=true,Anchor=AnchorStyles.Right};
    private readonly CheckBox record=new() {Text="録音する",AutoSize=true};
    private readonly Button folder=new() {Text="保存先…",AutoSize=true,Anchor=AnchorStyles.Right,FlatStyle=FlatStyle.Flat};
    private readonly Label folderLabel=new() {Text="保存先未選択",AutoEllipsis=true,Height=19,Dock=DockStyle.Fill};
    private readonly ToolTip tips=new();
    private string destination="";
    private bool active,applying;
    public event Action<bool>? ActiveChanged;
    public event Action? SettingsChanged;
    public ReceiverPanel() {
        Font=new Font("Yu Gothic UI",9);BackColor=Background;ForeColor=TextColor;
        AutoScroll=true;MinimumSize=new(260,480);Padding=new(10);
        var layout=Column();layout.Dock=DockStyle.Top;layout.Padding=Padding.Empty;
        var heading=Column();heading.BackColor=Background;
        Add(heading,new Label {Text="STD-T98",AutoSize=true,Font=new Font(Font.FontFamily,10,FontStyle.Bold),ForeColor=Muted});
        frequency.Font=new Font("Segoe UI",16,FontStyle.Bold);Add(heading,frequency);Add(layout,heading);
        StyleButton(start,true);Add(layout,start);
        var reception=Card();status.ForeColor=Muted;status.Font=new Font(Font.FontFamily,9,FontStyle.Bold);Add(reception,status);
        var secretRow=Row();Add(secretRow,Caption("秘話"));privacy.Font=new Font("Consolas",13,FontStyle.Bold);privacy.ForeColor=Accent;Add(secretRow,privacy);Add(reception,secretRow);
        Add(layout,reception);
        Add(layout,eye);
        var information=Card();
        var codes=Column();codes.Name="ReceiverCodes";codes.Margin=new(0,0,0,6);
        var csmRow=CodeRow(("CSM",csm));csmRow.ColumnStyles[0]=new(SizeType.Absolute,90);
        Add(codes,csmRow);Add(codes,CodeRow(("UC",user),("メーカー",maker)));
        Add(information,codes);
        detail.ForeColor=Muted;Add(information,detail);quality.ForeColor=Muted;Add(information,quality);Add(layout,information);
        var audio=Card();Add(audio,Caption("音声出力"));device.BackColor=Color.FromArgb(35,50,67);device.ForeColor=TextColor;Add(audio,device);
        var audioRow=Row();Add(audioRow,playback);Add(audioRow,volumeValue);Add(audio,audioRow);volume.BackColor=CardColor;Add(audio,volume);Add(layout,audio);
        var recording=Card();var recordingRow=Row();Add(recordingRow,record);StyleButton(folder,false);Add(recordingRow,folder);Add(recording,recordingRow);
        folderLabel.ForeColor=Muted;Add(recording,folderLabel);Add(layout,recording);
        Controls.Add(layout);
        device.Items.AddRange(WaveOutput.Devices());device.SelectedIndex=0;
        start.Click+=(_,_)=>{
            active=!active;UpdateButton();
            try {ActiveChanged?.Invoke(active);} catch(Exception e) {active=false;UpdateButton();ShowError(e.Message);}
        };
        device.SelectedIndexChanged+=Changed;playback.CheckedChanged+=Changed;
        volume.ValueChanged+=(_,_)=>{volumeValue.Text=$"{volume.Value}%";Changed(null,EventArgs.Empty);};
        record.CheckedChanged+=(_,_)=>{
            if(!applying&&record.Checked&&string.IsNullOrEmpty(destination)&&!SelectFolder()) {record.Checked=false;return;}
            Changed(null,EventArgs.Empty);
        };
        folder.Click+=(_,_)=>{if(SelectFolder())Changed(null,EventArgs.Empty);};
        tips.SetToolTip(record,"WAV、受信バースト、軟判定、受信情報を保存します");
    }
    private static TableLayoutPanel Column()=>new() {AutoSize=true,AutoSizeMode=AutoSizeMode.GrowAndShrink,ColumnCount=1,Dock=DockStyle.Fill,Margin=Padding.Empty};
    private static TableLayoutPanel Card() {
        var card=Column();card.BackColor=CardColor;card.Padding=new(10,7,10,7);card.Margin=new(0,0,0,8);return card;
    }
    private static TableLayoutPanel Row() {
        var row=new TableLayoutPanel {AutoSize=true,ColumnCount=2,RowCount=1,Dock=DockStyle.Fill,Margin=Padding.Empty};
        row.ColumnStyles.Add(new(SizeType.Percent,50));row.ColumnStyles.Add(new(SizeType.Percent,50));return row;
    }
    private static Label Caption(string value)=>new() {Text=value,AutoSize=true,ForeColor=Muted,Anchor=AnchorStyles.Left};
    private static TableLayoutPanel CodeRow(params (string Caption,Label Value)[] fields) {
        var row=new TableLayoutPanel {AutoSize=true,ColumnCount=fields.Length*2,RowCount=1,Dock=DockStyle.Fill,Margin=Padding.Empty,CellBorderStyle=TableLayoutPanelCellBorderStyle.Single};
        int column=0;
        foreach(var (caption,value) in fields) {
            row.ColumnStyles.Add(new(SizeType.AutoSize));row.ColumnStyles.Add(new(SizeType.Percent,100f/fields.Length));
            var label=Caption(caption);label.Margin=new(8,8,8,8);
            value.Font=new Font("Consolas",12,FontStyle.Bold);value.Anchor=AnchorStyles.Left|AnchorStyles.Right;value.TextAlign=ContentAlignment.MiddleRight;value.Margin=new(8,8,8,8);
            row.Controls.Add(label,column++,0);row.Controls.Add(value,column++,0);
        }
        return row;
    }
    private static void Add(TableLayoutPanel parent,Control control) {
        if(control is not TableLayoutPanel)control.Margin=new Padding(0,3,0,3);
        if(parent.ColumnCount==1) {control.Anchor=AnchorStyles.Left|AnchorStyles.Right;parent.RowCount++;}
        parent.Controls.Add(control);
    }
    private static void StyleButton(Button button,bool primary) {
        button.FlatStyle=FlatStyle.Flat;button.FlatAppearance.BorderSize=primary?0:1;
        button.FlatAppearance.BorderColor=Color.FromArgb(65,87,106);
        button.BackColor=primary?Accent:CardColor;button.ForeColor=primary?Background:TextColor;
        button.Font=new Font("Yu Gothic UI",primary?10:9,primary?FontStyle.Bold:FontStyle.Regular);
        button.Cursor=Cursors.Hand;
    }
    private void UpdateButton() {start.Text=active?"受信停止":"受信開始";start.BackColor=active?Color.FromArgb(39,65,75):Accent;start.ForeColor=active?TextColor:Background;}
    private void Changed(object? sender,EventArgs e) {if(!applying)SettingsChanged?.Invoke();}
    private bool SelectFolder() {
        using var dialog=new FolderBrowserDialog {Description="録音先フォルダー",SelectedPath=destination,UseDescriptionForTitle=true};
        if(dialog.ShowDialog(this)!=DialogResult.OK)return false;
        destination=dialog.SelectedPath;ShowFolder();return true;
    }
    private void ShowFolder() {folderLabel.Text=string.IsNullOrEmpty(destination)?"保存先未選択":destination;tips.SetToolTip(folderLabel,destination);}
    public void Apply(Settings settings) {
        applying=true;try {
            device.SelectedIndex=Math.Clamp(settings.Device+1,0,device.Items.Count-1);playback.Checked=settings.Playback;
            volume.Value=Math.Clamp(settings.Volume,0,100);record.Checked=settings.Record;destination=settings.Folder;ShowFolder();
        } finally {applying=false;}
    }
    public Settings Read()=>new(device.SelectedIndex-1,playback.Checked,volume.Value,record.Checked,destination);
    public void SetFrequency(long value)=>frequency.Text=$"{value/1e6:F5} MHz";
    public void ShowError(string text) {status.Text=text;status.ForeColor=Color.Salmon;}
    public void RefreshEye(IEnumerable<EyeBatch> batches,long epoch,bool running)=>eye.Advance(batches,epoch,running);
    public void RefreshState(Snapshot s,bool running,bool source) {
        active=running;UpdateButton();
        status.Text="●  "+(running&&!source?"SDR#の受信開始を待機中":s.Status);
        status.ForeColor=running&&source&&s.Stats.Locked!=0?Accent:Muted;
        privacy.Text=s.Privacy;csm.Text=s.Csm;user.Text=s.User?.ToString()??"—";maker.Text=s.Maker?.ToString()??"—";
        detail.Text=$"呼種別 {s.Call?.ToString()??"—"}  ·  モード {s.Mode?.ToString()??"—"}";
        quality.Text=$"CRC {s.Stats.CrcOk}/{s.Stats.Frames}  ·  {s.Stats.Power:F1} dB  ·  {s.Stats.FrequencyError:+0;-0;0} Hz\n欠落  IQ {s.Drops} / 音声 {s.AudioDrops}";
    }
    protected override void Dispose(bool disposing) {if(disposing)tips.Dispose();base.Dispose(disposing);}
}
