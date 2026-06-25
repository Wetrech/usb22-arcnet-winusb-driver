// ArcnetInterop.cs
//
// P/Invoke binding layer for arcnet.dll (MSVC cdecl, x64 Windows).
// Layout:
//   1. Enums          — ArcResult, ArcLogLevel          (mirror arcnet.h)
//   2. Struct         — ArcPacket                       (provisional; not yet in arcnet.h)
//   3. Delegates      — ArcLogCallbackNative,
//                       ArcRecvCallbackNative           (provisional)
//   4. ArcnetNative   — raw DllImport declarations      (internal)
//   5. ArcnetDevice   — managed wrapper, IDisposable    (public)
//   6. ArcnetException
//
// Marshaling notes
// ────────────────
// arc_ctx_t*         : opaque pointer → IntPtr
// C bool (_Bool)     : 1-byte in MSVC x64 → [MarshalAs(UnmanagedType.I1)]
//                      (P/Invoke default BOOL is 4 bytes — wrong for C bool)
// const char* return : pointer to a static DLL string → IntPtr +
//                      Marshal.PtrToStringAnsi; MUST NOT be freed.
// char paths[][256]  : 2-D array is not directly expressible in P/Invoke.
//                      Solution: allocate a flat byte buffer of
//                      maxDevices*256 bytes via Marshal.AllocHGlobal, pass as
//                      IntPtr, then walk it in strides of 256 with IntPtr.Add.
//                      See ArcnetDevice.ListDevices.
// Delegates          : Always store as a field on ArcnetDevice; if the GC
//                      collects a delegate while the native DLL still holds
//                      the function pointer the process will crash.

using System.Runtime.InteropServices;

namespace WetrechArcnetManager;

// ─────────────────────────────────────────────────────────────────────────────
// 1. Enums
// ─────────────────────────────────────────────────────────────────────────────

public enum ArcResult : int
{
    Ok            =  0,
    NoPacket      =  1,
    NotAcked      =  2,
    ErrOpen       = -1,
    ErrIo         = -2,
    ErrTimeout    = -3,
    ErrParam      = -4,
    ErrEcho       = -5,
    ErrDeviceGone = -6,
    ErrNetBusy    = -7,
}

public enum ArcLogLevel : int
{
    None  = 0,
    Error = 1,
    Info  = 2,
    Debug = 3,
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Struct (provisional — not yet in arcnet.h)
// ─────────────────────────────────────────────────────────────────────────────

// Provisional layout for the planned arc_packet_t C struct:
//   uint8_t src;        offset  0
//   uint8_t dst;        offset  1
//   uint8_t _pad[2];    offset  2  (natural int-alignment padding)
//   int32_t len;        offset  4
//   uint8_t data[508];  offset  8
//   total: 516 bytes
//
// Update when the C header is finalised.
[StructLayout(LayoutKind.Sequential)]
public struct ArcPacket
{
    public byte Src;
    public byte Dst;
    private byte _pad0;           // explicit padding keeps layout obvious
    private byte _pad1;
    public int   Len;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 508)]
    public byte[] Data;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Unmanaged callback delegates
// ─────────────────────────────────────────────────────────────────────────────

// void (*arc_log_fn)(arc_log_level_t level, const char *msg, void *user)
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void ArcLogCallbackNative(ArcLogLevel level, IntPtr msg, IntPtr user);

// void (*arc_recv_fn)(const arc_packet_t *pkt, void *user)
// No ctx parameter — the C typedef is 2-arg. Copy pkt immediately; pointer is transient.
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void ArcRecvCallbackNative(IntPtr packetPtr, IntPtr user);

// ─────────────────────────────────────────────────────────────────────────────
// 4. Raw P/Invoke declarations
// ─────────────────────────────────────────────────────────────────────────────

internal static class ArcnetNative
{
    private const string Dll = "arcnet.dll";

    // Returns a pointer to a static string inside the DLL — do NOT free.
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr arc_result_str(ArcResult r);

    // char paths[][256] — see marshaling note at top of file.
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int arc_list_devices(IntPtr paths, int maxDevices);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr arc_open(
        [MarshalAs(UnmanagedType.LPStr)] string? devicePath,
        [MarshalAs(UnmanagedType.I1)]    bool    verbose);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void arc_close(IntPtr ctx);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void arc_set_log_level(IntPtr ctx, ArcLogLevel level);

    // fn = null restores the stderr default.
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void arc_set_log_callback(
        IntPtr ctx, ArcLogCallbackNative? fn, IntPtr user);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void arc_set_verbose(
        IntPtr ctx,
        [MarshalAs(UnmanagedType.I1)] bool enable);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ArcResult arc_init(
        IntPtr ctx,
        byte   nodeId,
        byte   timeout,
        byte   clockPrescaler,
        [MarshalAs(UnmanagedType.I1)] bool recvBroadcasts);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ArcResult arc_register(
        IntPtr ctx,
        [MarshalAs(UnmanagedType.I1)] bool bWrite,
        byte   reg,
        ref byte value);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ArcResult arc_transmit(
        IntPtr        ctx,
        byte          destNode,
        [In] byte[]   data,
        int           len,
        [MarshalAs(UnmanagedType.I1)] bool waitAck);

    // data[] must be ≥ 253 bytes; len is in/out (capacity in, bytes received out).
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ArcResult arc_receive(
        IntPtr         ctx,
        out byte       src,
        out byte       dst,
        [Out] byte[]   data,
        ref int        len);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ArcResult arc_read_event(
        IntPtr        ctx,
        [Out] byte[]  buf,
        int           bufsize,
        uint          timeoutMs,
        out int       outLen);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ArcResult arc_reopen(IntPtr ctx);

    // ── Provisional async API ─────────────────────────────────────────────────
    // These entry points are not yet exported by arcnet.dll (not in arcnet.h).
    // Calling them will throw EntryPointNotFoundException until the DLL is
    // updated with background-listen support.

    // Starts an internal background receive thread feeding a packet queue.
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ArcResult arc_listen_start(IntPtr ctx);

    // Stops the background thread; blocks until the thread exits.
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ArcResult arc_listen_stop(IntPtr ctx);

    // Pops one packet from the internal queue. Returns NoPacket when empty.
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ArcResult arc_poll_packet(IntPtr ctx, out ArcPacket packet);

    // Number of packets currently in the internal receive queue.
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int arc_pending_count(IntPtr ctx);

    // fn = null clears the callback. fn fires on the DLL's background thread.
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ArcResult arc_set_recv_callback(
        IntPtr ctx, ArcRecvCallbackNative? fn, IntPtr user);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Managed wrapper
// ─────────────────────────────────────────────────────────────────────────────

public sealed class ArcnetDevice : IDisposable
{
    private IntPtr _ctx;
    private bool   _disposed;

    // Stored as fields so the GC never collects them while the DLL holds a
    // function pointer to the underlying native thunk.
    private ArcLogCallbackNative?  _logCbRef;
    private ArcRecvCallbackNative? _recvCbRef;

    // ── Static helpers ────────────────────────────────────────────────────────

    // Enumerates all detected CC ARCNET devices. Returns WinUSB interface paths.
    public static string[] ListDevices(int maxDevices = 8)
    {
        const int stride = 256;
        IntPtr buf = Marshal.AllocHGlobal(maxDevices * stride);
        try
        {
            int n = ArcnetNative.arc_list_devices(buf, maxDevices);
            var paths = new string[n];
            for (int i = 0; i < n; i++)
                paths[i] = Marshal.PtrToStringAnsi(IntPtr.Add(buf, i * stride)) ?? "";
            return paths;
        }
        finally { Marshal.FreeHGlobal(buf); }
    }

    // Safe: falls back to enum name if DLL is not loaded.
    public static string ResultString(ArcResult r)
    {
        try   { return Marshal.PtrToStringAnsi(ArcnetNative.arc_result_str(r)) ?? r.ToString(); }
        catch { return r.ToString(); }
    }

    // ── Construction / teardown ───────────────────────────────────────────────

    // devicePath: WinUSB interface path from ListDevices(), or null to auto-select
    //             the first CC ARCNET device.
    // Throws ArcnetException(ErrOpen) on failure.
    public ArcnetDevice(string? devicePath = null, bool verbose = false)
    {
        _ctx = ArcnetNative.arc_open(devicePath, verbose);
        if (_ctx == IntPtr.Zero)
            throw new ArcnetException(ArcResult.ErrOpen,
                $"arc_open: device not found ({devicePath ?? "auto"})");
    }

    public void Close()
    {
        if (_ctx != IntPtr.Zero)
        {
            ArcnetNative.arc_close(_ctx);
            _ctx = IntPtr.Zero;
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        Close();
        _logCbRef  = null;
        _recvCbRef = null;
        _disposed  = true;
    }

    // ── Core API ──────────────────────────────────────────────────────────────

    public ArcResult Init(byte nodeId         = 1,
                          byte timeout        = 0x18,
                          byte clockPrescaler = 0x00,
                          bool recvBroadcasts = true)
    {
        ThrowIfDisposed();
        return ArcnetNative.arc_init(_ctx, nodeId, timeout, clockPrescaler, recvBroadcasts);
    }

    public ArcResult ReadRegister(byte reg, out byte value)
    {
        ThrowIfDisposed();
        value = 0;
        return ArcnetNative.arc_register(_ctx, bWrite: false, reg, ref value);
    }

    public ArcResult WriteRegister(byte reg, byte value)
    {
        ThrowIfDisposed();
        return ArcnetNative.arc_register(_ctx, bWrite: true, reg, ref value);
    }

    // len = -1 means use data.Length.
    public ArcResult Transmit(byte destNode, byte[] data, int len = -1, bool waitAck = true)
    {
        ThrowIfDisposed();
        if (data is null || data.Length == 0) return ArcResult.ErrParam;
        return ArcnetNative.arc_transmit(_ctx, destNode, data, len < 0 ? data.Length : len, waitAck);
    }

    // data[] must be ≥ 253 bytes. Returns NoPacket when nothing is waiting.
    public ArcResult Receive(out byte src, out byte dst, byte[] data, out int len)
    {
        ThrowIfDisposed();
        len = data.Length;
        return ArcnetNative.arc_receive(_ctx, out src, out dst, data, ref len);
    }

    public ArcResult ReadEvent(byte[] buf, uint timeoutMs, out int len)
    {
        ThrowIfDisposed();
        return ArcnetNative.arc_read_event(_ctx, buf, buf.Length, timeoutMs, out len);
    }

    public ArcResult Reopen()
    {
        ThrowIfDisposed();
        return ArcnetNative.arc_reopen(_ctx);
    }

    // ── Logging ───────────────────────────────────────────────────────────────

    public void SetLogLevel(ArcLogLevel level)
    {
        ThrowIfDisposed();
        ArcnetNative.arc_set_log_level(_ctx, level);
    }

    // callback = null restores stderr output.
    public void SetLogCallback(Action<ArcLogLevel, string>? callback)
    {
        ThrowIfDisposed();
        if (callback is null)
        {
            _logCbRef = null;
            ArcnetNative.arc_set_log_callback(_ctx, null, IntPtr.Zero);
            return;
        }
        _logCbRef = (level, msgPtr, _) =>
            callback(level, Marshal.PtrToStringAnsi(msgPtr) ?? "");
        ArcnetNative.arc_set_log_callback(_ctx, _logCbRef, IntPtr.Zero);
    }

    // ── Provisional async API ─────────────────────────────────────────────────
    // Will throw EntryPointNotFoundException until arcnet.dll exports these.

    public ArcResult StartListen()  { ThrowIfDisposed(); return ArcnetNative.arc_listen_start(_ctx); }
    public ArcResult StopListen()   { ThrowIfDisposed(); return ArcnetNative.arc_listen_stop(_ctx); }
    public int       PendingCount() { ThrowIfDisposed(); return ArcnetNative.arc_pending_count(_ctx); }

    public ArcResult PollPacket(out ArcPacket packet)
    {
        ThrowIfDisposed();
        return ArcnetNative.arc_poll_packet(_ctx, out packet);
    }

    // callback fires on the DLL's background thread — marshal to the UI thread
    // with Dispatcher.InvokeAsync if updating WPF controls.
    public void SetRecvCallback(Action<ArcPacket>? callback)
    {
        ThrowIfDisposed();
        if (callback is null)
        {
            _recvCbRef = null;
            ArcnetNative.arc_set_recv_callback(_ctx, null, IntPtr.Zero);
            return;
        }
        _recvCbRef = (pktPtr, _) =>
            callback(Marshal.PtrToStructure<ArcPacket>(pktPtr));
        ArcnetNative.arc_set_recv_callback(_ctx, _recvCbRef, IntPtr.Zero);
    }

    // ─────────────────────────────────────────────────────────────────────────

    private void ThrowIfDisposed()
    {
        if (_disposed || _ctx == IntPtr.Zero)
            throw new ObjectDisposedException(nameof(ArcnetDevice));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Exception
// ─────────────────────────────────────────────────────────────────────────────

public sealed class ArcnetException : Exception
{
    public ArcResult Result { get; }

    public ArcnetException(ArcResult result, string? message = null)
        : base(message ?? result.ToString())
    {
        Result = result;
    }
}
