using System.Buffers.Binary;

namespace RemoteBridge.Client;

public sealed record AudioFormat(uint Stream,uint Epoch,uint Codec,ushort Revision,uint Rate,byte Channels,
    byte[] Config,uint MaxUnit,ulong FirstSample,ulong CapturedUs,uint FirstUnit)
{
    public static AudioFormat Parse(Tlv t)=>new(t.Int(1),t.Int(2),t.Int(3),t.Short(4),t.Int(5),t.Byte(6),t.Bytes(7),t.Int(8),t.Long(9),t.Long(10),t.Int(11));
}
public sealed record AudioEnd(uint Stream,byte Reason,ulong EncodedBytes,ulong EndedUs,uint Units,ulong Samples,uint Epoch)
{
    public static AudioEnd Parse(Tlv t)=>new(t.Int(1),t.Byte(2),t.Long(3),t.Long(4),t.Int(5),t.Long(6),t.Int(7));
    public bool Complete=>Reason is 0 or 7 or 8;
}

/// <summary>Validated, bounded IMA profile 1/1 decoder. New instance per START.
/// FORMAT only changes decoder seed at an exact coding-unit boundary.</summary>
public sealed class AudioDecoder
{
    private static readonly int[] Steps=[7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
        73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,
        963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,
        6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767];
    private static readonly int[] Index=[-1,-1,-1,-1,2,4,6,8];
    private uint _frame,_unit=1,_epoch,_offset,_size,_unitSamples;
    private ulong _samples,_bytes;
    private byte[] _buffer=[];
    private int _predictor,_index;
    private long _started,_progress;
    private bool _failed;
    public AudioFormat Format {get;private set;}=null!;
    public AudioDecoder(AudioFormat format)=>SetFormat(format);
    private void Ready(){if(_failed)throw new InvalidDataException("Audio decoder already ended/failed");}
    public void SetFormat(AudioFormat f)
    {
        Ready();
        try
        {
            if(f.Stream==0||f.Epoch!=_epoch+1||f.FirstUnit!=_unit||f.FirstSample!=_samples||_offset!=0||
               (Format is not null&&f.Stream!=Format.Stream)||f.Codec!=1||f.Revision!=1||f.Rate is not (8000 or 16000)||
               f.Channels!=1||f.MaxUnit is 0 or >65536||f.Config.Length!=4||f.Config[2]>88||f.Config[3]!=0)
                throw new InvalidDataException("Unsupported audio format or invalid boundary");
            Format=f;_epoch=f.Epoch;_predictor=BinaryPrimitives.ReadInt16LittleEndian(f.Config);_index=f.Config[2];
            _buffer=new byte[f.MaxUnit];
        }
        catch{_failed=true;throw;}
    }
    public void CheckTimeout()
    {
        Ready();long now=Environment.TickCount64;
        if(_offset>0&&(now-_progress>=2000||now-_started>=10000)){_failed=true;throw new InvalidDataException("Incomplete coding unit timed out");}
    }
    public byte[] Feed(byte[] data)
    {
        Ready();
        try
        {
            CheckTimeout();
            if(data.Length is <41 or >512)throw new InvalidDataException("Audio payload length");
            uint stream=Wire.U32(data),frame=Wire.U32(data,4),epoch=Wire.U32(data,8),unit=Wire.U32(data,12),size=Wire.U32(data,16),offset=Wire.U32(data,20),samples=Wire.U32(data,32);
            int n=data.Length-40;
            if(Wire.U32(data,36)!=0||stream!=Format.Stream||frame!=_frame+1||epoch!=_epoch||unit!=_unit||offset!=_offset||
                size==0||size>Format.MaxUnit||Wire.U64(data,24)!=_samples||(ulong)offset+(uint)n>size||
                (samples!=uint.MaxValue&&samples!=size*2)||(_offset!=0&&(size!=_size||samples!=_unitSamples)))
                throw new InvalidDataException("Audio continuity/sample count");
            if(_offset==0){_started=Environment.TickCount64;_size=size;_unitSamples=samples;}
            _progress=Environment.TickCount64;Array.Copy(data,40,_buffer,(int)_offset,n);
            _offset+=(uint)n;_frame=frame;_bytes+=(uint)n;
            if(_offset<size)return [];
            byte[] pcm=new byte[size*4];int p=0;
            for(int i=0;i<size;i++)for(int half=0;half<2;half++)
            {
                int nibble=half==0?_buffer[i]>>4:_buffer[i]&15;
                int step=Steps[_index];int delta=(step>>3)+((nibble&4)!=0?step:0)+((nibble&2)!=0?step>>1:0)+((nibble&1)!=0?step>>2:0);
                _predictor=Math.Clamp(_predictor+((nibble&8)!=0?-delta:delta),-32768,32767);_index=Math.Clamp(_index+Index[nibble&7],0,88);
                BinaryPrimitives.WriteInt16LittleEndian(pcm.AsSpan(p),(short)_predictor);p+=2;
            }
            _samples=_samples==ulong.MaxValue||samples==uint.MaxValue?ulong.MaxValue:checked(_samples+samples);
            _unit=checked(_unit+1);_offset=0;return pcm;
        }
        catch{_failed=true;_buffer=[];throw;}
    }
    public void End(AudioEnd end)
    {
        Ready();
        try
        {
            if(end.Stream!=Format.Stream||end.Reason>9||end.EncodedBytes!=_bytes||end.Units!=_unit-1||end.Samples!=_samples||end.Epoch!=_epoch||end.Complete&&_offset!=0)
                throw new InvalidDataException("Audio END integrity");
        }
        finally{_failed=true;_buffer=[];}
    }
}
