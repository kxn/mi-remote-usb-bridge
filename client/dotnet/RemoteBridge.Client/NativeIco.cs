using System.Runtime.InteropServices;

namespace RemoteBridge.Client;

// Same C decoder as Python; applications still receive PCM16LE from AudioDecoder.
internal sealed class NativeIco : IDisposable
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr Create();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void Destroy(IntPtr state);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int DecodeFrame(IntPtr state,byte[] frame,int length,[Out] short[] pcm);
    private sealed class Api
    {
        public readonly Create New;
        public readonly Destroy Free;
        public readonly DecodeFrame Decode;
        public Api()
        {
            string name=OperatingSystem.IsWindows()?"ico.dll":OperatingSystem.IsMacOS()?"libico.dylib":"libico.so";
            string path=Environment.GetEnvironmentVariable("RBP_ICO_LIBRARY")??Path.Combine(AppContext.BaseDirectory,name);
            IntPtr lib=NativeLibrary.Load(path); // kept alive for all decoder instances
            New=Marshal.GetDelegateForFunctionPointer<Create>(NativeLibrary.GetExport(lib,"ico_create"));
            Free=Marshal.GetDelegateForFunctionPointer<Destroy>(NativeLibrary.GetExport(lib,"ico_destroy"));
            Decode=Marshal.GetDelegateForFunctionPointer<DecodeFrame>(NativeLibrary.GetExport(lib,"ico_decode"));
        }
    }
    private static readonly Lazy<Api> Functions=new(()=>new Api());
    private static readonly object Gate=new();
    private IntPtr _state;
    public static bool Available {get {try {_=Functions.Value;return true;}catch{return false;}}}
    public NativeIco()
    {
        lock(Gate)_state=Functions.Value.New();
        if(_state==IntPtr.Zero)throw new OutOfMemoryException("ICO decoder allocation");
    }
    public byte[] Decode(byte[] data)
    {
        if(data.Length!=40||_state==IntPtr.Zero)throw new InvalidDataException("ICO coding unit");
        short[] samples=new short[320];
        lock(Gate)if(Functions.Value.Decode(_state,data,40,samples)!=320)throw new InvalidDataException("ICO decode failed");
        byte[] pcm=new byte[640];
        for(int i=0;i<320;i++)System.Buffers.Binary.BinaryPrimitives.WriteInt16LittleEndian(pcm.AsSpan(i*2),samples[i]);
        return pcm;
    }
    public void Dispose(){lock(Gate){if(_state!=IntPtr.Zero){Functions.Value.Free(_state);_state=IntPtr.Zero;}}GC.SuppressFinalize(this);}
    ~NativeIco(){Dispose();}
}
