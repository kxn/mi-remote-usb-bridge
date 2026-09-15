using System.Diagnostics;
using System.Net.Sockets;
using System.Text.Json;
using RemoteBridge.Client;
using Xunit;

namespace RemoteBridge.Tests;

public class ContractTests
{
    private static JsonElement Vectors(string name)=>JsonDocument.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory,"vectors",name))).RootElement;
    [Fact] public void WireGoldenVectorsRoundtrip()
    {
        Assert.Equal(0xe3069283u,Wire.Crc("123456789"u8));
        foreach(var v in Vectors("vectors.json").EnumerateArray())
        {
            var encoded=Convert.FromHexString(v.GetProperty("wire_hex").GetString()!);
            Frame f=Wire.Decode(encoded.AsSpan(0,encoded.Length-1));
            Assert.Equal(Convert.FromHexString(v.GetProperty("payload_hex").GetString()!),f.Payload);
            Assert.Equal(encoded,Wire.Encode(f));
            encoded[^2]^=0x40;Assert.Throws<InvalidDataException>(()=>Wire.Decode(encoded.AsSpan(0,encoded.Length-1)));
        }
    }
    [Fact] public void AudioGoldenVectorsAndMalformedBoundary()
    {
        AudioDecoder? decoder=null;int bytes=0;
        foreach(var v in Vectors("vectors.json").EnumerateArray())
        {
            var wire=Convert.FromHexString(v.GetProperty("wire_hex").GetString()!);var f=Wire.Decode(wire.AsSpan(0,wire.Length-1));
            if(f.Opcode==0x380)decoder=new(AudioFormat.Parse(Schema.Validate(f)));
            if(f.Opcode==0x383)decoder!.SetFormat(AudioFormat.Parse(Schema.Validate(f)));
            if(f.Opcode==0x381)bytes+=decoder!.Feed(f.Payload).Length;
            if(f.Opcode==0x382)decoder!.End(AudioEnd.Parse(Schema.Validate(f)));
        }
        Assert.True(bytes>2400);
        foreach(var v in Vectors("codec-vectors.json").EnumerateArray())
        {
            var f=new AudioFormat(1,1,1,1,16000,1,Convert.FromHexString(v.GetProperty("config_hex").GetString()!),1024,0,0,1);
            var d=new AudioDecoder(f);var data=Convert.FromHexString(v.GetProperty("encoded_hex").GetString()!);
            byte[] payload=new byte[40+data.Length];Wire.Put32(payload,0,1);Wire.Put32(payload,4,1);Wire.Put32(payload,8,1);Wire.Put32(payload,12,1);Wire.Put32(payload,16,(uint)data.Length);Wire.Put32(payload,32,(uint)data.Length*2);data.CopyTo(payload,40);
            Assert.Equal(Convert.FromHexString(v.GetProperty("pcm_s16le_hex").GetString()!),d.Feed(payload));
            Assert.Throws<InvalidDataException>(()=>d.Feed(payload));
            Assert.Throws<InvalidDataException>(()=>d.SetFormat(f));
        }
    }
    [Fact] public void TlvRejectsDuplicateAndBadScalar()
    {
        Assert.Throws<InvalidDataException>(()=>Tlv.Parse([1,1,1,0,3,1,1,1,0,4]));
        Assert.Throws<InvalidDataException>(()=>Tlv.Parse([1,5,1,0,2]));
        Assert.Throws<InvalidDataException>(()=>Tlv.Parse([1,3,2,0,0,0]));
        Assert.Throws<InvalidDataException>(()=>KeysState.Parse(new byte[23]));
    }
    [Fact] public async Task SimulatorHandshakeManualPairVoiceAndHeartbeat()
    {
        string binary=Environment.GetEnvironmentVariable("RBP_SIM_BINARY")??throw new InvalidOperationException("Set RBP_SIM_BINARY to the built simulator");
        string directory=Path.Combine(Path.GetTempPath(),"rbp-dotnet-"+Guid.NewGuid());Directory.CreateDirectory(directory);
        using var sim=Process.Start(new ProcessStartInfo(binary,"--data-port 45971 --control-port 45972 --quiet"){WorkingDirectory=directory,UseShellExecute=false,CreateNoWindow=true})!;
        try
        {
            await Task.Delay(400);
            await using var c=await BridgeClient.OpenAsync(new TcpTransport("127.0.0.1",45971));
            using var timeout=new CancellationTokenSource(15000);
            // Drain events independently of requests, exactly as a UI would.
            var voice=new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);
            var consumer=Task.Run(async()=>
            {
                AudioDecoder? audio=null;int pcm=0;
                await foreach(var f in c.Events.ReadAllAsync(timeout.Token))
                {
                    var t=Schema.Validate(f);
                    if(f.Opcode==0x380)audio=new(AudioFormat.Parse(t));
                    if(f.Opcode==0x383)audio!.SetFormat(AudioFormat.Parse(t));
                    if(f.Opcode==0x381)pcm+=audio!.Feed(f.Payload).Length;
                    if(f.Opcode==0x382){audio!.End(AudioEnd.Parse(t));voice.TrySetResult(pcm);}
                }
            });
            Tlv peer=Schema.Validate(await c.RequestAsync(4));Assert.Equal(0u,peer.Int(1));
            uint search=Schema.Validate(await c.RequestAsync(0x100,new Tlv().U16(1,500))).Int(1);
            await Task.Delay(650);Tlv list=Schema.Validate(await c.RequestAsync(0x101,new Tlv().U32(1,search).U8(2,0)));
            var candidate=Assert.Single(Entries.Candidates(search,list.Bytes(2)));
            // No automatic pairing after discovery.
            Assert.Equal(0u,Schema.Validate(await c.RequestAsync(4)).Int(1));
            await c.RequestAsync(0x110,new Tlv().U32(1,search).U32(2,candidate.Id));
            DeviceState device;
            do{await Task.Delay(100);device=DeviceState.Parse(Schema.Validate(await c.RequestAsync(3)));}while(!device.Ready&&!timeout.IsCancellationRequested);
            Assert.True(device.Ready);Assert.Equal(device.Connection,c.ConnectionId);
            await c.RequestAsync(0x202,new Tlv().Bool(1,true));
            await c.RequestAsync(0x300,new Tlv().Bool(1,true).Blob(2,[1,0,0,0,1,0]).U32(3,65536));
            using var control=new TcpClient("127.0.0.1",45972);
            await control.GetStream().WriteAsync("mic_on\n"u8.ToArray());await Task.Delay(500);await control.GetStream().WriteAsync("mic_off\n"u8.ToArray());
            Assert.True(await voice.Task.WaitAsync(TimeSpan.FromSeconds(4))>0);
            await Task.Delay(5500);Assert.True(c.IsOpen);await c.RequestAsync(6);
            timeout.Cancel();try{await consumer;}catch(OperationCanceledException){}
        }
        finally{sim.Kill();await sim.WaitForExitAsync();Directory.Delete(directory,true);}
    }
}
