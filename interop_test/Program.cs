// InteropTest — P/Invoke validation for arcnet.dll
//
// Checks:
//   1. Marshal.SizeOf(ArcPacket) == C sizeof(arc_packet_t)  (expected 516 bytes)
//   2. arc_result_str(ARC_OK)    == "ARC_OK"
//   3. arc_list_devices()        — shows found paths
//   4. arc_open + arc_close      — opens first device if any (non-fatal if none)

using System.Runtime.InteropServices;
using WetrechArcnetManager;

Console.WriteLine("=== ArcnetInterop P/Invoke test ===");
Console.WriteLine();

// ── 1. Struct size ────────────────────────────────────────────────────────────
// C layout: src(1) + dst(1) + pad(2) + len(4) + data[508] = 516 bytes
int sz = Marshal.SizeOf<ArcPacket>();
bool sizeOk = sz == 516;
Console.WriteLine($"[1] Marshal.SizeOf<ArcPacket>() = {sz}  (expected 516) -> {(sizeOk ? "OK" : "MISMATCH -- check padding!")}");

// ── 2. arc_result_str ─────────────────────────────────────────────────────────
try
{
    string okStr  = ArcnetDevice.ResultString(ArcResult.Ok);
    string errStr = ArcnetDevice.ResultString(ArcResult.ErrOpen);
    Console.WriteLine($"[2] arc_result_str(ARC_OK)      = \"{okStr}\"  -> {(okStr == "ARC_OK"      ? "OK" : "UNEXPECTED")}");
    Console.WriteLine($"    arc_result_str(ARC_ERR_OPEN) = \"{errStr}\"  -> {(errStr == "ARC_ERR_OPEN" ? "OK" : "UNEXPECTED")}");
}
catch (Exception ex)
{
    Console.WriteLine($"[2] FAIL -- {ex.GetType().Name}: {ex.Message}");
}

// ── 3. arc_list_devices ───────────────────────────────────────────────────────
string[] devices;
try
{
    devices = ArcnetDevice.ListDevices(8);
    Console.WriteLine($"[3] arc_list_devices() found {devices.Length} device(s):");
    foreach (var d in devices)
        Console.WriteLine($"    {d}");
    if (devices.Length == 0)
        Console.WriteLine("    (none -- plug a USB22-485 to test arc_open)");
}
catch (Exception ex)
{
    Console.WriteLine($"[3] FAIL -- {ex.GetType().Name}: {ex.Message}");
    devices = Array.Empty<string>();
}

// ── 4. arc_open / arc_close ───────────────────────────────────────────────────
if (devices.Length > 0)
{
    Console.WriteLine($"[4] Opening first device: {devices[0]}");
    try
    {
        using var dev = new ArcnetDevice(devices[0]);
        Console.WriteLine("    arc_open  -> OK (context non-null)");

        var r = dev.Init();
        Console.WriteLine($"    arc_init  -> {ArcnetDevice.ResultString(r)}");

        // arc_close is called by Dispose()
        Console.WriteLine("    arc_close -> (via Dispose)");
    }
    catch (ArcnetException ex)
    {
        Console.WriteLine($"    arc_open  -> FAIL: {ex.Message}");
    }
    catch (Exception ex)
    {
        Console.WriteLine($"[4] FAIL -- {ex.GetType().Name}: {ex.Message}");
    }
}
else
{
    Console.WriteLine("[4] Skipped (no devices found)");
}

Console.WriteLine();
Console.WriteLine("=== Done ===");
