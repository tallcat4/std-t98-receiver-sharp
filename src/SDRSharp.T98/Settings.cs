using System.Text.Json;
namespace SDRSharp.T98;
internal sealed record Settings(int Device=-1,bool Playback=true,int Volume=80,bool Record=false,string Folder="") {
    private static string FilePath => Environment.GetEnvironmentVariable("T98_SETTINGS_PATH") ??
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"T98Receiver","settings.json");
    public static Settings Load() {
        try { var s=JsonSerializer.Deserialize<Settings>(File.ReadAllText(FilePath)) ?? new();return s with {Volume=Math.Clamp(s.Volume,0,100),Record=false}; }
        catch { return new(); }
    }
    public void Save() {
        Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
        File.WriteAllText(FilePath+".tmp",JsonSerializer.Serialize(this with {Record=false}));
        File.Move(FilePath+".tmp",FilePath,true);
    }
}
