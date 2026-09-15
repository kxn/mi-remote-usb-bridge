using System.Text;

namespace RemoteBridge.Client;

public sealed record DeviceState(uint Connection,uint Peer,byte State,string Model,string Name,uint CatalogRevision,
    byte KeyCount,byte VoiceState,uint SampleRate,byte Battery,bool VoiceEnabled,uint Stream,string Message,byte Interaction,uint CaptureLimitMs)
{
    public bool Ready=>State==5;
    public static DeviceState Parse(Tlv t)
    {
        var d=new DeviceState(t.Int(1),t.Int(2),t.Byte(3),t.Text(4),t.Text(5),t.Int(6),t.Byte(7),t.Byte(8),t.Int(9),t.Byte(10),t.Flag(12),t.Int(14),t.Has(17)?t.Text(17):"",t.Byte(18),t.Int(19));
        if(d.State>7||d.KeyCount>64||d.VoiceState>4||d.Interaction>3||d.Battery>100&&d.Battery!=255||t.Byte(11)>2||d.SampleRate>384000)throw new InvalidDataException("Device state values");
        return d;
    }
}
public sealed record KeyDefinition(byte Slot,ushort Id,string Name);
public sealed record Candidate(uint Search,uint Id,byte Support,byte Signal,string Name)
{ public override string ToString()=>$"{Name} · 信号 {Signal} · #{Id}"; }
public sealed record KeysState(uint Sequence,ulong Bits,byte Kind,byte Reason)
{
    public static KeysState Parse(byte[] b)
    {
        if(b.Length!=24||Wire.U16(b,22)!=0||b[20]>2||b[21]>3||b[20]==2&&Wire.U64(b,12)!=0)throw new InvalidDataException("Keys state");
        return new(Wire.U32(b),Wire.U64(b,12),b[20],b[21]);
    }
}
public static class Entries
{
    public static List<KeyDefinition> Keys(byte[] b)
    {
        if(b.Length==0||b[0]>64)throw new InvalidDataException("Key count");
        var result=new List<KeyDefinition>();int p=1;
        for(int i=0;i<b[0];i++)
        {
            if(p+4>b.Length)throw new InvalidDataException("Key entry");
            byte slot=b[p];ushort id=Wire.U16(b,p+1);int n=b[p+3];p+=4;
            if(slot>=64||id==0||n>48||p+n>b.Length)throw new InvalidDataException("Key values");
            result.Add(new(slot,id,new UTF8Encoding(false,true).GetString(b,p,n)));p+=n;
        }
        if(p!=b.Length)throw new InvalidDataException("Key trailing bytes");return result;
    }
    public static List<Candidate> Candidates(uint search,byte[] b)
    {
        if(b.Length==0||b[0]>8)throw new InvalidDataException("Candidate count");
        var result=new List<Candidate>();int p=1;
        for(int i=0;i<b[0];i++)
        {
            if(p+7>b.Length)throw new InvalidDataException("Candidate entry");
            uint id=Wire.U32(b,p);byte support=b[p+4],signal=b[p+5];int n=b[p+6];p+=7;
            if(id==0||support>2||n>48||p+n>b.Length)throw new InvalidDataException("Candidate values");
            result.Add(new(search,id,support,signal,new UTF8Encoding(false,true).GetString(b,p,n)));p+=n;
        }
        if(p!=b.Length)throw new InvalidDataException("Candidate trailing bytes");return result;
    }
    public static IEnumerable<string> Faults(Tlv stats)
    {
        if(!stats.Has(7))yield break;
        byte[] b=stats.Bytes(7);
        if(b.Length>96||b.Length%24!=0)throw new InvalidDataException("Fault records length");
        for(int p=0;p<b.Length;p+=24)
        {
            if(Wire.U32(b,p)==0||Wire.U32(b,p+16)==0)throw new InvalidDataException("Fault counters");
            yield return $"seq={Wire.U32(b,p)} source={Wire.U16(b,p+4)} stage=0x{Wire.U16(b,p+6):X4} code=0x{Wire.U32(b,p+8):X8} context=0x{Wire.U32(b,p+12):X8} count={Wire.U32(b,p+16)} board_ms={Wire.U32(b,p+20)}";
        }
    }
}
