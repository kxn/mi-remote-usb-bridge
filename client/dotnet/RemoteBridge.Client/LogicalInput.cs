using System.Text.Json;
using System.Threading.Channels;

namespace RemoteBridge.Client;

public sealed record InputSource(string Receiver,uint Session,uint Peer,uint Connection,string Model,uint CatalogRevision);
public sealed record LogicalKeyEvent(InputSource Source,string Kind,ushort Key=0,ulong CapturedUs=0,uint Sequence=0,
    bool Synthetic=false,ushort[]? PressedKeys=null,string Reason="");
public sealed record InputEvent(Frame? Raw=null,LogicalKeyEvent? Key=null);

public static class InputProfiles
{
    private static readonly JsonDocument Data=JsonDocument.Parse(typeof(InputProfiles).Assembly.GetManifestResourceStream("RemoteBridge.InputProfiles.json")!);
    public static JsonElement? Model(string model)=>Data.RootElement.GetProperty("models").TryGetProperty(model,out var item)?item.Clone():null;
    // Presentation metadata only; a physical button does not imply BLE support.
    public static JsonElement? Layout(string model)
    {
        var item=Model(model);if(item is null)return null;
        string? id=item.Value.GetProperty("layout_id").GetString();
        return id is null?null:Data.RootElement.GetProperty("layouts").GetProperty(id).Clone();
    }
}

public sealed class LogicalKeys
{
    private readonly Action<LogicalKeyEvent> _emit;
    private readonly object _gate=new();
    private InputSource? _source;
    private KeyDefinition[] _catalog=[];
    private ulong _bits;
    private uint? _sequence;
    public LogicalKeys(Action<LogicalKeyEvent> emit)=>_emit=emit;
    public InputSource? Source {get {lock(_gate)return _source;}}
    public KeyDefinition[] Catalog {get {lock(_gate)return (KeyDefinition[])_catalog.Clone();}}
    public bool HasKey(RemoteKey key)=>HasKey((ushort)key);
    public bool HasKey(ushort key){lock(_gate)return _catalog.Any(k=>k.Id==key);}
    private ushort[] Held()=>_catalog.Where(k=>(_bits&(1UL<<k.Slot))!=0).Select(k=>k.Id).Order().ToArray();
    public ushort[] PressedKeys {get {lock(_gate)return Held();}}
    public void Reset(string reason="source_lost")
    {
        lock(_gate)
        {
            var source=_source;var held=Held();_source=null;_catalog=[];_bits=0;_sequence=null;
            if(source is null)return;
            foreach(ushort key in held)_emit(new(source,"up",key,Synthetic:true,Reason:reason));
            _emit(new(source,"reset",Synthetic:true,Reason:reason));
        }
    }
    public void Install(InputSource source,IEnumerable<KeyDefinition> keys,int count)
    {
        var catalog=keys.ToArray();
        if(source.Session==0||source.Connection==0||source.Peer==0||count is <0 or >64||catalog.Length!=count||
           !catalog.Select(k=>(int)k.Slot).SequenceEqual(Enumerable.Range(0,count))||
           catalog.Any(k=>k.Id==0)||catalog.Select(k=>k.Id).Distinct().Count()!=count)
            throw new InvalidDataException("Invalid logical key catalog");
        lock(_gate){Reset("catalog_changed");_source=source;_catalog=catalog;}
    }
    public void Feed(KeysState state,ulong capturedUs=0)
    {
        lock(_gate)
        {
            if(_source is null)return;
            if(state.Kind>2||state.Reason>3||(state.Kind==2&&state.Bits!=0))throw new InvalidDataException("Key state values");
            if(_catalog.Length<64&&(state.Bits>>_catalog.Length)!=0)throw new InvalidDataException("Unknown key slot");
            if(_sequence is not null&&(state.Sequence<_sequence||state.Sequence==_sequence&&state.Kind!=2))return;
            var before=Held();_bits=state.Bits;_sequence=state.Sequence;var after=Held();
            if(state.Kind==0){_emit(new(_source,"snapshot",CapturedUs:capturedUs,Sequence:state.Sequence,Synthetic:true,PressedKeys:after));return;}
            foreach(ushort key in before.Except(after))_emit(new(_source,"up",key,capturedUs,state.Sequence,state.Kind==2,Reason:state.Reason.ToString()));
            foreach(ushort key in after.Except(before))_emit(new(_source,"down",key,capturedUs,state.Sequence));
            if(state.Kind==2)_emit(new(_source,"reset",CapturedUs:capturedUs,Sequence:state.Sequence,Synthetic:true,Reason:state.Reason.ToString()));
        }
    }
}

/// <summary>Optional high-level event owner. Consume Events here instead of client.Events.
/// Catalog loading/subscription is automatic. Raw voice and management frames are forwarded.
/// On completion/error the application must release all input for this receiver.</summary>
public sealed class RemoteInputSession : IAsyncDisposable
{
    private readonly BridgeClient _client;
    private readonly string _receiver;
    private readonly CancellationTokenSource _stop=new();
    private readonly Channel<InputEvent> _events=Channel.CreateBounded<InputEvent>(512);
    private readonly Task _pump;
    public LogicalKeys Keys {get;}
    public ChannelReader<InputEvent> Events=>_events.Reader;
    public Task Completion=>_pump;
    public RemoteInputSession(BridgeClient client,string? receiverId=null)
    {
        _client=client;_receiver=receiverId??Guid.NewGuid().ToString("N");
        Keys=new(e=>Publish(new(Key:e)));_pump=RunAsync();
    }
    private void Publish(InputEvent e)
    {
        if(!_events.Writer.TryWrite(e))throw new IOException("Logical input queue overflow; release receiver state");
    }
    private async Task ConfigureAsync(DeviceState d,uint session)
    {
        var source=new InputSource(_receiver,session,d.Peer,d.Connection,d.Model,d.CatalogRevision);
        if(Keys.Source==source&&d.Ready)return;
        Keys.Reset("device_changed");
        if(!d.Ready||d.Connection==0||d.Connection!=_client.ConnectionId)return;
        List<KeyDefinition> keys=[];byte cursor=0;
        do
        {
            var response=await _client.RequestAsync(0x200,new Tlv().U8(1,cursor),_stop.Token);
            if(_client.ConnectionId!=d.Connection||response.Connection!=d.Connection)return;
            var t=Schema.Validate(response);
            if(t.Int(1)!=d.CatalogRevision)throw new InvalidDataException("Catalog revision changed");
            keys.AddRange(Entries.Keys(t.Bytes(3)));
            byte next=t.Byte(2);if(next!=255&&(next<=cursor||next>=64))throw new InvalidDataException("Catalog cursor");
            cursor=next;
        }while(cursor!=255);
        if(_client.ConnectionId!=d.Connection)return;
        Keys.Install(source,keys,d.KeyCount);
        await _client.RequestAsync(0x202,new Tlv().Bool(1,true),_stop.Token);
        var snapshot=await _client.RequestAsync(0x201,ct:_stop.Token);
        if(snapshot.Connection!=d.Connection||_client.ConnectionId!=d.Connection){Keys.Reset("device_changed");return;}
        Keys.Feed(KeysState.Parse(snapshot.Payload),Wire.U64(snapshot.Payload,4));
    }
    private async Task ConfigureCurrentAsync(DeviceState d,uint session)
    {
        try{await ConfigureAsync(d,session);}
        catch(RpcException) when(_client.ConnectionId!=d.Connection||!_client.IsOpen){Keys.Reset("device_changed");}
    }
    private async Task RunAsync()
    {
        Exception? error=null;
        try
        {
            var initial=await _client.RequestAsync(3,ct:_stop.Token);
            await ConfigureCurrentAsync(DeviceState.Parse(Schema.Validate(initial)),initial.Session);
            await foreach(var frame in _client.Events.ReadAllAsync(_stop.Token))
            {
                if(frame.Opcode==0x183)
                {
                    var d=DeviceState.Parse(Schema.Validate(frame));
                    if(d.Connection==_client.ConnectionId)await ConfigureCurrentAsync(d,frame.Session);
                }
                else if(frame.Opcode==0x280&&Keys.Source is { } source&&source.Connection==frame.Connection)
                    Keys.Feed(KeysState.Parse(frame.Payload),Wire.U64(frame.Payload,4));
                Publish(new(Raw:frame));
            }
        }
        catch(OperationCanceledException) when(_stop.IsCancellationRequested){}
        catch(Exception ex){error=ex;}
        finally
        {
            try{Keys.Reset("session_lost");}catch(Exception ex){error??=ex;}
            _events.Writer.TryComplete(error);
        }
    }
    public async ValueTask DisposeAsync(){_stop.Cancel();await _pump.ConfigureAwait(false);}
}
