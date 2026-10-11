using System.Security.Cryptography;

namespace Simoder.DevBridge.Core;

/// <summary>@summary Reads bounded, fixed-path installation evidence without requiring an elevated broker.</summary>
public static class LoaderDiagnostics
{
    /// <summary>@summary Returns disk identities and historical logs, without claiming live hook state.</summary>
    public static IReadOnlyList<string> Read(string gameRoot)
    {
        var result = new List<string> { "Evidence scope: installed files and historical logs; live hook state unverified." };
        foreach (var relative in new[] { "simoder/sc13modloader.dll", "simoder/simoder.exe", "SimCity/SimCity.exe" })
        {
            try
            {
                var path = Path.Combine(gameRoot, relative);
                if (!File.Exists(path)) { result.Add($"{relative}: missing"); continue; }
                var info = new FileInfo(path);
                if (info.Length > 64 * 1024 * 1024) { result.Add($"{relative}: exceeds diagnostic size limit"); continue; }
                using var file = File.OpenRead(path);
                result.Add($"{relative}: sha256={Convert.ToHexString(SHA256.HashData(file))} bytes={info.Length}");
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
            { result.Add($"{relative}: read_failed:{exception.GetType().Name}"); }
        }
        try { result.AddRange(RuntimeEvidenceReader.ReadLogs(gameRoot, null, 80)); }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or ArgumentException)
        { result.Add($"loader_log: read_failed:{exception.GetType().Name}"); }
        return result;
    }
}
