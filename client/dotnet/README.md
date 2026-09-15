# RBP/3 .NET SDK

Reusable .NET 9 client with no WPF, Python or Windows Bluetooth dependency. MIT; System.IO.Ports is the only NuGet runtime dependency. This new source targets firmware 0.6.5 but is not included in the previously published 0.6.5 release manifest.

Build: `dotnet build client/dotnet/RemoteBridge.Client -c Release`. Add a project reference to RemoteBridge.Client.csproj.

Open with `await BridgeClient.OpenAsync(new SerialTransport(port), token)` and dispose with `await using`. Only one app may own a COM port. Consume `client.Events.ReadAllAsync(token)` promptly; `client.Completion` reports session loss. A dedicated reader and heartbeat operate independently of the event consumer.

`RequestAsync(opcode, Tlv, token)` sends one request and never retries side effects. Three ordinary RPC slots leave an independent heartbeat slot. Timeout means an operation may have an unknown outcome. Pairing requires observing OPERATION or GET_OPERATION completion after acceptance.

`Schema.Validate(frame)` checks typed payloads against the embedded schema. DeviceState, KeysState and Entries expose state, keys, candidates and raw faults. Consult [wire protocol](../../docs/wire-protocol.md) for commands and lifecycle.

For voice, construct AudioDecoder from START's AudioFormat, call SetFormat for FORMAT, Feed for DATA, End for END, and CheckTimeout periodically. Feed returns PCM16LE only after a complete encoded unit. Current support is IMA ADPCM revision 1, 8/16 kHz mono. Negotiate only supported codecs. Discard failed decoders. Event queue overflow explicitly closes the session.

The host owns device selection, reconnect policy, key injection and ASR. The SDK does not replay pairing or auto-select remotes. MiRemote adds catalog/snapshot-aware input and a bounded voice queue.

## Tests

Set RBP_SIM_BINARY to the built C sim_bridge executable, then run `dotnet test client/dotnet/RemoteBridge.Tests -c Release`.

Tests cover wire/CRC goldens, TLV validation, fragmented ADPCM with reference PCM and invalid sequences, and C simulator handshake, idle heartbeat, discovery without auto-pair, manual pair and voice. Tests use local TCP ports 45971/45972 and temporary storage. They do not replace hardware acceptance.
