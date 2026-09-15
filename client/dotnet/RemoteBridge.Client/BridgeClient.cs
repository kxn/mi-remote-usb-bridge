using System.Collections.Concurrent;
using System.IO.Ports;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Threading.Channels;

namespace RemoteBridge.Client;

public sealed class RpcException(ushort status,string message,bool uncertain=false):Exception($"RBP 0x{status:X4}: {message}")
{ public ushort Status {get;}=status; public bool Uncertain {get;}=uncertain; }

public interface IBridgeTransport:IDisposable
{ int Read(byte[] buffer); void Write(byte[] bytes); }

public sealed class SerialTransport:IBridgeTransport
{
    private readonly SerialPort _port;
    public SerialTransport(string name)
    {
        _port=new(name,115200,Parity.None,8,StopBits.One){ReadTimeout=50,WriteTimeout=1000,Handshake=Handshake.None,DtrEnable=true,ReadBufferSize=65536};
        try{_port.Open();_port.DiscardInBuffer();}catch{_port.Dispose();throw;}
    }
    public int Read(byte[] b)=>_port.Read(b,0,b.Length);
    public void Write(byte[] b)=>_port.Write(b,0,b.Length);
    public void Dispose()=>_port.Dispose();
}

public sealed class TcpTransport:IBridgeTransport
{
    private readonly TcpClient _client;
    public TcpTransport(string host,int port){_client=new();_client.Connect(host,port);_client.NoDelay=true;_client.GetStream().WriteTimeout=1000;}
    public int Read(byte[] b){if(!_client.Client.Poll(50_000,SelectMode.SelectRead))throw new TimeoutException();return _client.GetStream().Read(b);}
    public void Write(byte[] b)=>_client.GetStream().Write(b);
    public void Dispose()=>_client.Dispose();
}

/// <summary>One reader, serialized writes, at most 3 ordinary RPCs + 1 heartbeat.
/// Events are bounded and never silently discarded. Consumers must not do ASR on the I/O reader.</summary>
public sealed class BridgeClient:IAsyncDisposable
{
    private sealed record Pending(ushort Opcode,uint Connection,TaskCompletionSource<Frame> Reply);
    private readonly IBridgeTransport _transport;
    private readonly CancellationTokenSource _stop=new();
    private readonly SemaphoreSlim _slots=new(3,3);
    private readonly object _sendGate=new();
    private readonly ConcurrentDictionary<uint,Pending> _pending=new();
    private readonly Channel<Frame> _events=Channel.CreateBounded<Frame>(new BoundedChannelOptions(512){SingleWriter=true,SingleReader=true,FullMode=BoundedChannelFullMode.Wait});
    private readonly TaskCompletionSource<Exception?> _ended=new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly byte[] _nonce=RandomNumberGenerator.GetBytes(16);
    private volatile uint _session,_connection;
    private uint _tx,_rx,_request;
    private Task _reader=Task.CompletedTask,_heartbeat=Task.CompletedTask;
    private int _failed,_disposed;
    public string Firmware {get;private set;}="";
    public uint ConnectionId=>_connection;
    public bool IsOpen=>_session!=0&&!_stop.IsCancellationRequested;
    public ChannelReader<Frame> Events=>_events.Reader;
    public Task<Exception?> Completion=>_ended.Task;
    private BridgeClient(IBridgeTransport transport)=>_transport=transport;

    public static async Task<BridgeClient> OpenAsync(IBridgeTransport transport,CancellationToken ct=default)
    {
        var client=new BridgeClient(transport);
        try
        {
            client._reader=Task.Factory.StartNew(client.ReadLoop,CancellationToken.None,TaskCreationOptions.LongRunning,TaskScheduler.Default);
            var response=await client.SendAsync(1,new Tlv().Blob(1,client._nonce).Encode(),false,TimeSpan.FromSeconds(3),ct);
            client.Firmware=Schema.Validate(response).Text(3);
            client._heartbeat=client.HeartbeatAsync();return client;
        }
        catch{await client.DisposeAsync();throw;}
    }
    public async Task<Frame> RequestAsync(ushort op,Tlv? args=null,CancellationToken ct=default)
    {
        if(op is 1 or 2)throw new ArgumentException("HELLO/PING are owned by the session");
        return await SendAsync(op,args?.Encode()??[],true,TimeSpan.FromSeconds(2),ct);
    }
    private async Task<Frame> SendAsync(ushort op,byte[] payload,bool ordinary,TimeSpan timeout,CancellationToken ct)
    {
        using var linked=CancellationTokenSource.CreateLinkedTokenSource(ct,_stop.Token);
        if(ordinary)await _slots.WaitAsync(linked.Token);
        uint id=0;
        try
        {
            TaskCompletionSource<Frame> reply=new(TaskCreationOptions.RunContinuationsAsynchronously);
            lock(_sendGate)
            {
                linked.Token.ThrowIfCancellationRequested();
                if(op!=1&&!IsOpen)throw new IOException("USB session not ready");
                if(_tx>=uint.MaxValue-1||_request>=uint.MaxValue-1)throw new IOException("Session sequence exhausted");
                id=++_request;uint conn=op is >=0x200 and <=0x303?_connection:0;
                _pending[id]=new(op,conn,reply);
                try{_transport.Write(Wire.Encode(new(1,_session,++_tx,id,op,0,conn,payload)));}
                catch(Exception ex){Fail(new IOException("Transport write failed",ex));throw;}
            }
            Frame f=await reply.Task.WaitAsync(timeout,linked.Token);
            if(f.Status>=2){var e=Schema.Validate(f);throw new RpcException(f.Status,e.Has(1)?e.Text(1):"operation failed",e.Has(2)&&e.Flag(2));}
            return f;
        }
        catch(TimeoutException){throw new TimeoutException($"RBP opcode 0x{op:X4} timed out; operation outcome may be unknown (not replayed)");}
        catch(IOException ex){Fail(ex);throw;}
        finally{if(id!=0)_pending.TryRemove(id,out _);if(ordinary)_slots.Release();}
    }
    private async Task HeartbeatAsync()
    {
        try
        {
            while(!_stop.IsCancellationRequested)
            {
                await Task.Delay(1000,_stop.Token);
                uint cookie=BitConverter.ToUInt32(RandomNumberGenerator.GetBytes(4));
                var f=await SendAsync(2,new Tlv().U32(1,cookie).Encode(),false,TimeSpan.FromSeconds(4),_stop.Token);
                if(Schema.Validate(f).Int(1)!=cookie)throw new InvalidDataException("Heartbeat cookie mismatch");
            }
        }
        catch(OperationCanceledException) when(_stop.IsCancellationRequested){}
        catch(Exception ex){Fail(ex);}
    }
    private void ReadLoop()
    {
        byte[] buffer=new byte[1024];var encoded=new List<byte>();long started=0;
        try
        {
            while(!_stop.IsCancellationRequested)
            {
                int count=0;
                try{count=_transport.Read(buffer);if(count==0)throw new EndOfStreamException("USB transport closed");}
                catch(TimeoutException){}
                if(encoded.Count>0&&Environment.TickCount64-started>=1000){encoded.Clear();if(_session!=0)throw new InvalidDataException("Partial RBP frame timed out");}
                for(int i=0;i<count;i++)
                {
                    byte b=buffer[i];
                    if(b!=0){if(encoded.Count==0)started=Environment.TickCount64;if(encoded.Count>=551)throw new InvalidDataException("RBP frame overflow");encoded.Add(b);continue;}
                    if(encoded.Count==0)continue;
                    Frame f;
                    try{f=Wire.Decode(encoded.ToArray());}catch(InvalidDataException) when(_session==0){encoded.Clear();continue;}
                    encoded.Clear();Receive(f);
                }
            }
        }
        catch(Exception ex) when(_stop.IsCancellationRequested){_ = ex;}
        catch(Exception ex){Fail(ex);}
    }
    private void Receive(Frame f)
    {
        if(_session==0)
        {
            if(f.Kind!=2||f.Opcode!=1||f.Request!=1||f.Sequence!=1||f.Connection!=0||f.Status!=0||f.Session==0)return;
            var t=Schema.Validate(f);
            if(!t.Bytes(1).SequenceEqual(_nonce)||t.Bytes(2).Length!=16||t.Byte(4) is <1 or >64||t.Short(5)!=512)return;
            _session=f.Session;_rx=1;
        }
        else
        {
            if(f.Session!=_session)return;
            if(_rx==uint.MaxValue||f.Sequence!=_rx+1)throw new InvalidDataException("RBP receive sequence gap");
            _rx=f.Sequence;
        }
        if(f.Kind==2)
        {
            if(!_pending.TryGetValue(f.Request,out var pending))return;
            if(f.Opcode!=pending.Opcode||f.Connection!=pending.Connection||f.Status>17)throw new InvalidDataException("RPC correlation mismatch");
            var t=Schema.Validate(f);
            if(f.Opcode==3&&f.Status==0)_connection=t.Int(1);
            pending.Reply.TrySetResult(f);return;
        }
        if(f.Kind!=(f.Opcode==0x381?4:3)||f.Request!=0||f.Status!=0)throw new InvalidDataException("Event header");
        var fields=Schema.Validate(f);
        if(f.Opcode==0x183)_connection=fields.Int(1);
        if(f.Opcode is 0x280 or 0x380 or 0x381 or 0x382 or 0x383)
            if(_connection==0||f.Connection!=_connection)throw new InvalidDataException("Stale connection event");
        if(!_events.Writer.TryWrite(f))throw new IOException("Host event queue overrun; session stopped, no silent loss");
    }
    private void Fail(Exception? ex)
    {
        if(Interlocked.Exchange(ref _failed,1)!=0)return;
        _session=0;_connection=0;_stop.Cancel();
        foreach(var item in _pending.Values)item.Reply.TrySetException(ex??new IOException("Session closed"));
        _events.Writer.TryComplete(ex);_ended.TrySetResult(ex);
    }
    public async ValueTask DisposeAsync()
    {
        if(Interlocked.Exchange(ref _disposed,1)!=0)return;
        Fail(null);_transport.Dispose();
        await Task.WhenAll(_reader,_heartbeat).ConfigureAwait(false);
        // Pending callers may still be unwinding their finally blocks; do not dispose their semaphore.
    }
}
