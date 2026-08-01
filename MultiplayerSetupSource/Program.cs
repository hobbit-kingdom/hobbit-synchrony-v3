using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

// FakeHobbitsPatcher
//
// Distributes FAKE_HOBBITS.EXPORT + BILBOFAKE.NPCGEOM into every level folder
// under .\Levels\, registers the "FAKE_HOBBITS" layer in ALLUSEDLAYERS.TXT and
// INITIALOBJECTLAYERS.TXT (bumping the [ Layers : N ] counter), then removes the
// two source files from the root.

internal static class Program
{
    private const string ExportFile = "FAKE_HOBBITS.EXPORT";
    private const string GeomFile = "BILBOFAKE.NPCGEOM";
    private const string LevelsDir = "Levels";
    private const string LayerName = "FAKE_HOBBITS";

    private static readonly string[] LayerFiles =
    {
        "ALLUSEDLAYERS.TXT",
        "INITIALOBJECTLAYERS.TXT"
    };

    // [ Layers : 19 ]
    private static readonly Regex HeaderRe =
        new Regex(@"\[\s*Layers\s*:\s*(\d+)\s*\]", RegexOptions.IgnoreCase);

    // A layer entry:    "CH00_GLOBALS"
    private static readonly Regex QuotedRe =
        new Regex("^[ \t]*\"[^\"]*\"[ \t]*\r?$");

    // ISO-8859-1 round-trips every byte, so untouched bytes stay byte-identical.
    private static readonly Encoding Latin1 = Encoding.GetEncoding(28591);

    private static bool _dryRun;
    private static bool _keepSources;
    private static int _errors;

    private static int Main(string[] args)
    {
        // A fresh console means nobody is there to read the output afterwards.
        bool ownConsole = AtTopOfConsole();
        string root = null;

        foreach (string arg in args)
        {
            switch (arg.ToLowerInvariant())
            {
                case "-n":
                case "--dry-run":
                    _dryRun = true;
                    break;
                case "-k":
                case "--keep":
                    _keepSources = true;
                    break;
                case "-h":
                case "-?":
                case "--help":
                    Usage();
                    return 0;
                default:
                    if (arg.StartsWith("-"))
                    {
                        Console.Error.WriteLine("Unknown option: " + arg);
                        Usage();
                        return 2;
                    }
                    root = arg;
                    break;
            }
        }

        root = root == null ? ResolveRoot() : Path.GetFullPath(root);

        int exitCode = Run(root);

        if (ownConsole)
        {
            try
            {
                Console.WriteLine();
                Console.WriteLine("Press any key to close...");
                Console.ReadKey(true);
            }
            catch (InvalidOperationException)
            {
                // No interactive stdin - nothing to wait for.
            }
        }

        return exitCode;
    }

    // With no folder argument, look in the working directory and then around the
    // exe itself, so dropping the tool in a subfolder and double-clicking works.
    private static string ResolveRoot()
    {
        string cwd = Directory.GetCurrentDirectory();
        var candidates = new List<string> { cwd };

        string exeDir = Path.GetDirectoryName(
            System.Reflection.Assembly.GetExecutingAssembly().Location);

        for (string d = exeDir; d != null; d = Path.GetDirectoryName(d))
            candidates.Add(d);

        foreach (string c in candidates)
            if (Directory.Exists(Path.Combine(c, LevelsDir)) &&
                File.Exists(Path.Combine(c, ExportFile)) &&
                File.Exists(Path.Combine(c, GeomFile)))
                return Path.GetFullPath(c);

        return cwd;
    }

    private static void Usage()
    {
        Console.WriteLine("MultiplayerSetup [options] [root-folder]");
        Console.WriteLine();
        Console.WriteLine("  root-folder   folder holding " + ExportFile + ", " + GeomFile);
        Console.WriteLine("                and " + LevelsDir + "\\ (default: current directory)");
        Console.WriteLine();
        Console.WriteLine("  -n, --dry-run  report what would change, write nothing");
        Console.WriteLine("  -k, --keep     do not delete the source files afterwards");
        Console.WriteLine("  -h, --help     this text");
    }

    private static int Run(string root)
    {
        Console.WriteLine("Root   : " + root);
        if (_dryRun)
            Console.WriteLine("Mode   : DRY RUN (nothing will be written)");
        Console.WriteLine();

        string exportSrc = Path.Combine(root, ExportFile);
        string geomSrc = Path.Combine(root, GeomFile);
        string levels = Path.Combine(root, LevelsDir);

        if (!Directory.Exists(levels))
            return Fail("No '" + LevelsDir + "' folder found in " + root);

        if (!File.Exists(exportSrc))
            return Fail("Missing source file: " + exportSrc);

        if (!File.Exists(geomSrc))
            return Fail("Missing source file: " + geomSrc);

        string[] levelDirs = Directory.GetDirectories(levels);
        Array.Sort(levelDirs, StringComparer.OrdinalIgnoreCase);

        if (levelDirs.Length == 0)
            return Fail("No level folders inside " + levels);

        int copied = 0;
        int patched = 0;

        foreach (string dir in levelDirs)
        {
            Console.WriteLine("[" + Path.GetFileName(dir) + "]");

            copied += CopyAsset(exportSrc, Path.Combine(dir, ExportFile));
            copied += CopyAsset(geomSrc, Path.Combine(dir, GeomFile));

            foreach (string name in LayerFiles)
                patched += PatchLayerFile(Path.Combine(dir, name));

            Console.WriteLine();
        }

        Console.WriteLine("Levels   : " + levelDirs.Length);
        Console.WriteLine("Copied   : " + copied + " file(s)");
        Console.WriteLine("Patched  : " + patched + " layer file(s)");

        if (_errors > 0)
        {
            Console.WriteLine("Errors   : " + _errors);
            Console.WriteLine();
            Console.WriteLine("Source files kept because of the errors above.");
            return 1;
        }

        if (_keepSources)
            Console.WriteLine("Sources  : kept (--keep)");
        else
        {
            DeleteSource(exportSrc);
            DeleteSource(geomSrc);
        }

        Console.WriteLine();
        Console.WriteLine(_dryRun ? "Dry run complete." : "Done.");
        return _errors > 0 ? 1 : 0;
    }

    private static int CopyAsset(string src, string dst)
    {
        try
        {
            if (File.Exists(dst) && SameContent(src, dst))
            {
                Console.WriteLine("  = " + Path.GetFileName(dst) + " (identical, skipped)");
                return 0;
            }

            string verb = File.Exists(dst) ? "overwrite" : "copy";

            if (!_dryRun)
                File.Copy(src, dst, true);

            Console.WriteLine("  + " + Path.GetFileName(dst) + " (" + verb + ")");
            return 1;
        }
        catch (Exception ex)
        {
            return Problem("  ! " + Path.GetFileName(dst) + ": " + ex.Message);
        }
    }

    private static bool SameContent(string a, string b)
    {
        var fa = new FileInfo(a);
        var fb = new FileInfo(b);

        if (fa.Length != fb.Length)
            return false;

        using (FileStream sa = File.OpenRead(a))
        using (FileStream sb = File.OpenRead(b))
        {
            var ba = new byte[64 * 1024];
            var bb = new byte[64 * 1024];

            while (true)
            {
                int na = ReadBlock(sa, ba);
                int nb = ReadBlock(sb, bb);

                if (na != nb)
                    return false;

                if (na == 0)
                    return true;

                for (int i = 0; i < na; i++)
                    if (ba[i] != bb[i])
                        return false;
            }
        }
    }

    private static int ReadBlock(Stream s, byte[] buf)
    {
        int total = 0;

        while (total < buf.Length)
        {
            int n = s.Read(buf, total, buf.Length - total);

            if (n == 0)
                break;

            total += n;
        }

        return total;
    }

    private static int PatchLayerFile(string path)
    {
        string shortName = Path.GetFileName(path);

        if (!File.Exists(path))
        {
            Console.WriteLine("  . " + shortName + " (not present, skipped)");
            return 0;
        }

        try
        {
            string text = File.ReadAllText(path, Latin1);
            bool crlf = text.IndexOf("\r\n", StringComparison.Ordinal) >= 0;

            // Keeps every original line ending intact: '\r' rides along on the
            // end of each element and is restored by the final Join.
            List<string> lines = new List<string>(text.Split('\n'));

            int headerIndex = -1;
            int declared = -1;
            int lastQuoted = -1;
            int firstQuoted = -1;
            int quotedCount = 0;

            for (int i = 0; i < lines.Count; i++)
            {
                string line = lines[i];

                if (headerIndex < 0)
                {
                    Match m = HeaderRe.Match(line);

                    if (m.Success)
                    {
                        headerIndex = i;
                        declared = int.Parse(m.Groups[1].Value);
                        continue;
                    }
                }

                if (QuotedRe.IsMatch(line))
                {
                    if (firstQuoted < 0)
                        firstQuoted = i;

                    lastQuoted = i;
                    quotedCount++;

                    if (Unquote(line).Equals(LayerName, StringComparison.OrdinalIgnoreCase))
                    {
                        Console.WriteLine("  = " + shortName + " (already has \"" + LayerName + "\")");
                        return 0;
                    }
                }
            }

            if (lastQuoted < 0)
                return Problem("  ! " + shortName + ": no layer entries found, left untouched");

            string entry = FormatEntry(lines[firstQuoted], crlf);
            lines.Insert(lastQuoted + 1, entry);

            string counterNote;

            if (headerIndex < 0)
                counterNote = ", no [ Layers : N ] header found";
            else
            {
                if (declared != quotedCount)
                    Console.WriteLine("  ~ " + shortName + ": header said " + declared +
                                      " but found " + quotedCount + " entries; counting from the header");

                lines[headerIndex] = ReplaceCount(lines[headerIndex], declared + 1);
                counterNote = ", " + declared + " -> " + (declared + 1);
            }

            if (!_dryRun)
                File.WriteAllText(path, string.Join("\n", lines.ToArray()), Latin1);

            Console.WriteLine("  + " + shortName + " (added \"" + LayerName + "\"" + counterNote + ")");
            return 1;
        }
        catch (Exception ex)
        {
            return Problem("  ! " + shortName + ": " + ex.Message);
        }
    }

    private static string Unquote(string line)
    {
        int a = line.IndexOf('"');
        int b = line.LastIndexOf('"');
        return b > a ? line.Substring(a + 1, b - a - 1) : string.Empty;
    }

    // Mirrors the column layout of the entries already in the file.
    private static string FormatEntry(string sample, bool crlf)
    {
        string body = sample.TrimEnd('\r');
        int indent = 0;

        while (indent < body.Length && (body[indent] == ' ' || body[indent] == '\t'))
            indent++;

        int width = body.Length - indent;
        string quoted = "\"" + LayerName + "\"";

        if (width > quoted.Length)
            quoted = quoted.PadRight(width);

        return body.Substring(0, indent) + quoted + (crlf ? "\r" : string.Empty);
    }

    private static string ReplaceCount(string headerLine, int count)
    {
        return HeaderRe.Replace(headerLine, delegate(Match m)
        {
            Group g = m.Groups[1];
            int offset = g.Index - m.Index;
            return m.Value.Substring(0, offset) + count +
                   m.Value.Substring(offset + g.Length);
        }, 1);
    }

    private static void DeleteSource(string path)
    {
        try
        {
            if (!_dryRun)
                File.Delete(path);

            Console.WriteLine("Sources  : deleted " + Path.GetFileName(path));
        }
        catch (Exception ex)
        {
            Problem("Sources  ! could not delete " + Path.GetFileName(path) + ": " + ex.Message);
        }
    }

    private static int Fail(string message)
    {
        Console.Error.WriteLine("ERROR: " + message);
        _errors++;
        return 1;
    }

    private static int Problem(string message)
    {
        Console.Error.WriteLine(message);
        _errors++;
        return 0;
    }

    private static bool AtTopOfConsole()
    {
        try
        {
            return Console.CursorLeft == 0 && Console.CursorTop == 0;
        }
        catch
        {
            return false;
        }
    }
}
