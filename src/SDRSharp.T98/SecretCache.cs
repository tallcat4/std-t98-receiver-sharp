using System.Globalization;
using System.Text.Json;
namespace SDRSharp.T98;

internal readonly record struct SecretCode(int Series,int Key);
internal sealed class SecretCache {
    private sealed record Document(int Version,string[] Codes);
    private readonly string path;
    private readonly HashSet<SecretCode> codes=[];
    private bool readable=true;
    public string? Warning {get;private set;}
    internal static string DefaultPath=>Path.Combine(Path.GetDirectoryName(typeof(SecretCache).Assembly.Location)!,"secret-cache.json");
    public SecretCache(string? path=null) {this.path=path??DefaultPath;Load();}
    public int[] Pairs()=>codes.OrderBy(c=>c.Series).ThenBy(c=>c.Key).SelectMany(c=>new[]{c.Series,c.Key}).ToArray();
    private static bool TryParse(string? value,out SecretCode code) {
        code=default;if(value==null)return false;
        int series=0;string keyText=value;
        if(value.Length==8 && value[2]=='-') {
            if(!int.TryParse(value.AsSpan(0,2),NumberStyles.None,CultureInfo.InvariantCulture,out series)||series<1||series>15)return false;
            keyText=value[3..];
        } else if(value.Length!=5)return false;
        if(!int.TryParse(keyText,NumberStyles.None,CultureInfo.InvariantCulture,out int key)||key<1||key>32767)return false;
        code=new(series,key);return true;
    }
    private void Load() {
        try {
            if(!File.Exists(path))return;
            if(new FileInfo(path).Length>8*1024*1024)throw new IOException("キャッシュが大きすぎます");
            var data=JsonSerializer.Deserialize<Document>(File.ReadAllText(path));
            if(data?.Version!=1 || data.Codes==null)throw new JsonException("未対応の形式です");
            foreach(string value in data.Codes)if(TryParse(value,out var code))codes.Add(code);
        } catch(Exception e) when(e is IOException or UnauthorizedAccessException or JsonException) {
            readable=false;Warning="キャッシュ読込不可: "+e.Message;
        }
    }
    public void Remember(int series,int key) {
        if(series<0||series>15||key<1||key>32767||!codes.Add(new(series,key))||!readable)return;
        string temporary=path+"."+Guid.NewGuid().ToString("N")+".tmp";
        try {
            var data=new Document(1,codes.OrderBy(c=>c.Series).ThenBy(c=>c.Key).Select(c=>SecretWorker.FormatCode(c.Series,c.Key)).ToArray());
            using(var stream=new FileStream(temporary,FileMode.CreateNew,FileAccess.Write,FileShare.None)) {
                JsonSerializer.Serialize(stream,data,new JsonSerializerOptions {WriteIndented=true});stream.Flush(true);
            }
            File.Move(temporary,path,true);Warning=null;
        } catch(Exception e) when(e is IOException or UnauthorizedAccessException) {Warning="キャッシュ保存不可: "+e.Message;}
        finally {try {if(File.Exists(temporary))File.Delete(temporary);}catch(IOException){}catch(UnauthorizedAccessException){}}
    }
}
