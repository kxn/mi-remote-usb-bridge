using System.Buffers.Binary;
using System.Text;
using System.Text.Json;

namespace RemoteBridge.Client;

public sealed record Frame(byte Kind, uint Session, uint Sequence, uint Request, ushort Opcode,
    ushort Status, uint Connection, byte[] Payload);

public static class Wire
{
    public static uint Crc(ReadOnlySpan<byte> bytes)
    {
        uint crc = uint.MaxValue;
        foreach (var b in bytes) { crc ^= b; for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0x82F63B78u : 0); }
        return ~crc;
    }
    public static byte[] Encode(Frame f)
    {
        if (f.Payload.Length > 512 || f.Sequence == 0) throw new InvalidDataException("Frame bounds");
        byte[] raw = new byte[36 + f.Payload.Length];
        raw[0] = 0x52; raw[1] = 0x42; raw[2] = 3; raw[4] = f.Kind;
        BinaryPrimitives.WriteUInt16LittleEndian(raw.AsSpan(6), 32);
        Put32(raw, 8, f.Session); Put32(raw, 12, f.Sequence); Put32(raw, 16, f.Request);
        BinaryPrimitives.WriteUInt16LittleEndian(raw.AsSpan(20), f.Opcode);
        BinaryPrimitives.WriteUInt16LittleEndian(raw.AsSpan(22), f.Status);
        BinaryPrimitives.WriteUInt16LittleEndian(raw.AsSpan(24), (ushort)f.Payload.Length);
        Put32(raw, 28, f.Connection); f.Payload.CopyTo(raw, 32); Put32(raw, raw.Length - 4, Crc(raw.AsSpan(0, raw.Length - 4)));
        var result = new List<byte> { 0 }; int codeAt = 0; byte code = 1;
        foreach (byte b in raw)
        {
            if (b == 0) { result[codeAt] = code; codeAt = result.Count; result.Add(0); code = 1; }
            else { result.Add(b); if (++code == 255) { result[codeAt] = code; codeAt = result.Count; result.Add(0); code = 1; } }
        }
        result[codeAt] = code; result.Add(0); return result.ToArray();
    }
    public static Frame Decode(ReadOnlySpan<byte> encoded)
    {
        if (encoded.Length is < 1 or > 551) throw new InvalidDataException("COBS length");
        var raw = new List<byte>();
        for (int pos = 0; pos < encoded.Length;)
        {
            byte code = encoded[pos++];
            if (code == 0 || pos + code - 1 > encoded.Length) throw new InvalidDataException("COBS record");
            for (int i = 1; i < code; i++) raw.Add(encoded[pos++]);
            if (code < 255 && pos < encoded.Length) raw.Add(0);
        }
        byte[] b = raw.ToArray();
        if (b.Length is < 36 or > 548 || b[0] != 0x52 || b[1] != 0x42 || b[2] != 3 || b[3] != 0 || b[5] != 0 || U16(b,6) != 32 || U16(b,26) != 0 || U16(b,24) != b.Length-36 || U32(b,12) == 0 || Crc(b.AsSpan(0,b.Length-4)) != U32(b,b.Length-4))
            throw new InvalidDataException("RBP header/CRC");
        return new(b[4],U32(b,8),U32(b,12),U32(b,16),U16(b,20),U16(b,22),U32(b,28),b[32..^4]);
    }
    public static ushort U16(ReadOnlySpan<byte> b,int at=0)=>BinaryPrimitives.ReadUInt16LittleEndian(b[at..]);
    public static uint U32(ReadOnlySpan<byte> b,int at=0)=>BinaryPrimitives.ReadUInt32LittleEndian(b[at..]);
    public static ulong U64(ReadOnlySpan<byte> b,int at=0)=>BinaryPrimitives.ReadUInt64LittleEndian(b[at..]);
    public static void Put32(byte[] b,int at,uint v)=>BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(at),v);
}

public sealed class Tlv
{
    private readonly Dictionary<byte,(byte Type,byte[] Data)> _items = new();
    public bool Has(byte tag)=>_items.ContainsKey(tag);
    public Tlv Add(byte tag,byte type,byte[] bytes) { if(!_items.TryAdd(tag,(type,bytes)))throw new InvalidDataException("Duplicate TLV");return this; }
    public Tlv U8(byte tag,byte v)=>Add(tag,1,[v]);
    public Tlv U16(byte tag,ushort v){byte[] b=new byte[2];BinaryPrimitives.WriteUInt16LittleEndian(b,v);return Add(tag,2,b);}
    public Tlv U32(byte tag,uint v){byte[] b=new byte[4];Wire.Put32(b,0,v);return Add(tag,3,b);}
    public Tlv Bool(byte tag,bool v)=>Add(tag,5,[(byte)(v?1:0)]);
    public Tlv Blob(byte tag,byte[] v)=>Add(tag,7,v);
    public byte[] Bytes(byte tag,byte expected=7) { if(!_items.TryGetValue(tag,out var v)||v.Type!=expected)throw new InvalidDataException($"Missing/type TLV {tag}");return v.Data; }
    public byte Byte(byte tag)=>Bytes(tag,1)[0];
    public ushort Short(byte tag)=>Wire.U16(Bytes(tag,2));
    public uint Int(byte tag)=>Wire.U32(Bytes(tag,3));
    public ulong Long(byte tag)=>Wire.U64(Bytes(tag,4));
    public bool Flag(byte tag)=>Bytes(tag,5)[0]!=0;
    public string Text(byte tag)=>new UTF8Encoding(false,true).GetString(Bytes(tag,6));
    public byte[] Encode()
    {
        using var ms=new MemoryStream();using var writer=new BinaryWriter(ms);
        foreach(var (tag,v) in _items){writer.Write(tag);writer.Write(v.Type);writer.Write((ushort)v.Data.Length);writer.Write(v.Data);}
        if(ms.Length>512)throw new InvalidDataException("TLV payload too large");return ms.ToArray();
    }
    public static Tlv Parse(byte[] data,JsonElement? fields=null)
    {
        var t=new Tlv();var seen=new HashSet<byte>();
        for(int p=0;p<data.Length;)
        {
            if(data.Length-p<4)throw new InvalidDataException("TLV header");
            byte tag=data[p],type=data[p+1];int n=Wire.U16(data,p+2);p+=4;
            if(p+n>data.Length||!seen.Add(tag))throw new InvalidDataException("TLV bounds/duplicate");
            byte[] v=data[p..(p+n)];p+=n;
            if(fields is {} f){if(!f.TryGetProperty(tag.ToString(),out var def))continue;if(def.GetProperty("type").GetByte()!=type)throw new InvalidDataException("TLV schema type");}
            bool valid=type switch {1=>n==1,2=>n==2,3=>n==4,4=>n==8,5=>n==1&&v[0]<=1,6=>n<=96&&!v.Contains((byte)0),7=>n<=480,_=>fields is null};
            if(!valid)throw new InvalidDataException("TLV value");
            if(type==6)_=new UTF8Encoding(false,true).GetString(v);
            t.Add(tag,type,v);
        }
        if(fields is {} schema)foreach(var field in schema.EnumerateObject())if(field.Value.GetProperty("required").GetBoolean()&&!t.Has(byte.Parse(field.Name)))throw new InvalidDataException("Missing required TLV "+field.Name);
        return t;
    }
}

public static class Schema
{
    private static readonly JsonDocument Document=JsonDocument.Parse(typeof(Schema).Assembly.GetManifestResourceStream("RemoteBridge.Client.schema.json")!);
    public static Tlv Validate(Frame f)
    {
        var root=Document.RootElement;
        if(f.Status>=2)return Tlv.Parse(f.Payload,root.GetProperty("error_fields"));
        foreach(var m in root.GetProperty("messages").EnumerateArray())if(m.GetProperty("opcode").GetUInt16()==f.Opcode)
        {
            if(f.Kind==2){if(f.Status!=m.GetProperty("success_status").GetUInt16())throw new InvalidDataException("Response status");if(f.Opcode==0x201)return new Tlv();return Tlv.Parse(f.Payload,m.GetProperty("response"));}
            if(f.Opcode is 0x280 or 0x381)return new Tlv();
            return Tlv.Parse(f.Payload,m.GetProperty("fields"));
        }
        throw new InvalidDataException($"Unknown opcode 0x{f.Opcode:X4}");
    }
}
