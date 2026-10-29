// HarmonyRecomp Setup - the installation steps, independent of the window.
//
// Layout of an installation:
//   <InstallDir>\harmonyrecomp.exe, *.dll, harmonyrecomp.toml   the game
//   <InstallDir>\assets\                                       extracted game data
//   <InstallDir>\dlc\                                          DLC packages (optional)
//   <InstallDir>\setup.log                                     log of the last setup run
//   <InstallDir>\.setup\                                       downloads, tools, build
//                                                              (removed afterwards unless kept)

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace HarmonyRecompSetup
{
    public sealed class SetupOptions
    {
        public string InstallDir;
        public string GameFile;
        public string DlcDir;          // may be null
        public bool KeepBuildFiles;
        public bool DesktopShortcut;
        public bool StartMenuShortcut;
    }

    public sealed class SetupCancelled : Exception
    {
        public SetupCancelled() : base("Setup was cancelled.") { }
    }

    /// <summary>Progress of the whole setup in 0..1 plus a status line.</summary>
    public delegate void ProgressHandler(double fraction, string status);

    /// <summary>Asked when the Microsoft C++ components must be downloaded.
    /// Returns true if the user accepts Microsoft's license (URL given).</summary>
    public delegate bool LicenseHandler(string licenseUrl);

    public sealed class Installer
    {
        public const string PackageAsset = "harmonyrecomp-package.zip";
        public const string ToolchainAsset = "harmonyrecomp-toolchain.zip";
        public const uint TitleId = 0x58410A7A;
        public const uint ArcadeContentType = 0x000D0000;

        // Weights of the stages in the overall progress bar (sum = 1).
        const double WDownload = 0.20, WUnpack = 0.05, WMsvc = 0.10, WExtract = 0.05,
                     WCodegen = 0.10, WCompile = 0.45, WInstall = 0.05;

        readonly SetupOptions options;
        readonly ProgressHandler progress;
        readonly Action<string> log;
        readonly LicenseHandler askLicense;
        readonly CancellationToken cancel;
        StreamWriter logFile;
        Process running;
        readonly object runningLock = new object();

        public Installer(SetupOptions options, ProgressHandler progress, Action<string> log,
                         LicenseHandler askLicense, CancellationToken cancel)
        {
            this.options = options;
            this.progress = progress;
            this.log = log;
            this.askLicense = askLicense;
            this.cancel = cancel;
        }

        public static string ReleaseTag { get { return Metadata("ReleaseTag") ?? "dev"; } }
        public static string RepoSlug { get { return Metadata("RepoSlug") ?? "Wrexxial/HarmonyRecomp"; } }

        static string Metadata(string key)
        {
            foreach (AssemblyMetadataAttribute a in Assembly.GetExecutingAssembly()
                         .GetCustomAttributes(typeof(AssemblyMetadataAttribute), false))
            {
                if (a.Key == key) return a.Value;
            }
            return null;
        }

        string SetupDir { get { return Path.Combine(options.InstallDir, ".setup"); } }
        string ExePath { get { return Path.Combine(options.InstallDir, "harmonyrecomp.exe"); } }

        // ---- Checks used by the window before starting ----------------------

        /// <summary>Null if the file is the XBLA package of the game, else the reason.</summary>
        public static string CheckGameFile(string path)
        {
            try
            {
                byte[] header = new byte[0x400];
                using (var f = File.OpenRead(path))
                {
                    if (f.Read(header, 0, header.Length) != header.Length) return "The file is too small.";
                }
                string magic = Encoding.ASCII.GetString(header, 0, 4);
                if (magic != "LIVE" && magic != "PIRS")
                    return "This is not an Xbox 360 package (it should start with LIVE).";
                uint contentType = ReadBe32(header, 0x344);
                uint titleId = ReadBe32(header, 0x360);
                if (titleId != TitleId)
                    return string.Format("This package belongs to another game (title ID {0:X8}).", titleId);
                if (contentType != ArcadeContentType)
                    return "This is a package of the game, but not the game itself (maybe a DLC). " +
                           "Use the file from the folder 58410A7A\\000D0000.";
                return null;
            }
            catch (Exception e)
            {
                return "The file cannot be read: " + e.Message;
            }
        }

        static uint ReadBe32(byte[] b, int o)
        {
            return ((uint)b[o] << 24) | ((uint)b[o + 1] << 16) | ((uint)b[o + 2] << 8) | b[o + 3];
        }

        public static int DefaultJobs()
        {
            int cores = Math.Max(1, Environment.ProcessorCount);
            double gb = new Microsoft.VisualBasic.Devices.ComputerInfo().TotalPhysicalMemory / (1024.0 * 1024 * 1024);
            // Compiling the generated code takes up to ~2 GB per job.
            int byMemory = Math.Max(1, (int)(gb / 2.0));
            return Math.Max(1, Math.Min(cores, byMemory));
        }

        // ---- The setup ---------------------------------------------------------

        public void Run()
        {
            Directory.CreateDirectory(options.InstallDir);
            using (logFile = new StreamWriter(Path.Combine(options.InstallDir, "setup.log"), false, new UTF8Encoding(false)))
            {
                logFile.AutoFlush = true;
                Log("HarmonyRecomp Setup " + ReleaseTag + " (" + RepoSlug + ")");
                Log("Install folder: " + options.InstallDir);
                Log("Game file: " + options.GameFile);
                if (!string.IsNullOrEmpty(options.DlcDir)) Log("DLC folder: " + options.DlcDir);

                Directory.CreateDirectory(SetupDir);
                string downloads = Path.Combine(SetupDir, "downloads");
                Directory.CreateDirectory(downloads);

                // 1. Downloads (or local files next to the setup, for offline installs)
                string package = Obtain(PackageAsset, downloads, 0.0, WDownload * 0.4);
                string toolchain = Obtain(ToolchainAsset, downloads, WDownload * 0.4, WDownload * 0.6);

                // 2. Unpack
                double at = WDownload;
                string packageDir = Path.Combine(SetupDir, "package");
                string toolchainDir = Path.Combine(SetupDir, "toolchain");
                Report(at, "Unpacking the build tools");
                Unzip(package, packageDir);
                Report(at + WUnpack * 0.3, "Unpacking the build tools");
                Unzip(toolchain, toolchainDir);
                at += WUnpack;

                string python = Path.Combine(packageDir, "python", "python.exe");
                string src = Path.Combine(packageDir, "src");
                string sdk = Path.Combine(packageDir, "sdk");
                foreach (string required in new[] { python, Path.Combine(src, "scripts", "build.py"), sdk })
                {
                    if (!File.Exists(required) && !Directory.Exists(required))
                        throw new Exception("The release package is incomplete (" + required + " missing).");
                }

                var env = new Dictionary<string, string>();
                var path = new List<string>
                {
                    Path.Combine(toolchainDir, "llvm", "bin"),
                    Path.Combine(toolchainDir, "cmake", "bin"),
                    Path.Combine(toolchainDir, "ninja"),
                    Path.Combine(packageDir, "python"),
                };
                path.Add(Environment.GetEnvironmentVariable("PATH") ?? "");
                env["PATH"] = string.Join(";", path);
                env["PYTHONUNBUFFERED"] = "1";
                env["PYTHONUTF8"] = "1";

                // 3. Microsoft C++ components (only if Visual Studio is not installed)
                Report(at, "Checking for the Microsoft C++ build components");
                bool haveVs = RunTool(python, new[] { Path.Combine(src, "scripts", "portable_msvc.py"), "--detect" },
                                      src, env, null) == 0
                              && Environment.GetEnvironmentVariable("HARMONYRECOMP_PORTABLE_MSVC") != "1";
                if (!haveVs)
                {
                    string msvcDir = Path.Combine(SetupDir, "msvc");
                    string envJson = Path.Combine(msvcDir, "env.json");
                    if (!File.Exists(envJson))
                    {
                        string url = CaptureTool(python, new[] { Path.Combine(src, "scripts", "portable_msvc.py"), "--license" }, src, env).Trim();
                        if (!askLicense(url)) throw new SetupCancelled();
                        double start = at;
                        int code = RunTool(python, new[] { Path.Combine(src, "scripts", "portable_msvc.py"), "--accept-license", msvcDir },
                                           src, env, line => DownloadLine(line, start, WMsvc, "Downloading Microsoft C++ build components"));
                        if (code != 0) throw new Exception("Downloading the Microsoft C++ build components failed (see the log).");
                    }
                    var paths = ReadEnvJson(envJson);
                    env["INCLUDE"] = paths["INCLUDE"];
                    env["LIB"] = paths["LIB"];
                }
                at += WMsvc;

                // 4. Build: extract, codegen, compile, install (scripts/build.py)
                var args = new List<string>
                {
                    Path.Combine(src, "scripts", "build.py"),
                    "--game", options.GameFile,
                    "--sdk-dir", sdk,
                    "--install-to", options.InstallDir,
                    "--jobs", DefaultJobs().ToString(),
                };
                if (!string.IsNullOrEmpty(options.DlcDir))
                {
                    args.Add("--dlc-dir");
                    args.Add(options.DlcDir);
                }
                // Without Visual Studio the PC may lack the Visual C++ runtime:
                // the game gets its own copy next to the exe.
                if (!haveVs) args.Add("--app-local-runtime");
                double buildStart = at;
                int result = RunTool(python, args.ToArray(), src, env, line => BuildLine(line, buildStart));
                if (result != 0)
                    throw new Exception("Building the game failed (exit code " + result + "). The log has the details.");
                if (!File.Exists(ExePath)) throw new Exception("The build finished, but harmonyrecomp.exe is missing.");

                // 5. Shortcuts, cleanup
                Report(0.98, "Creating shortcuts");
                if (options.DesktopShortcut)
                    CreateShortcut(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory));
                if (options.StartMenuShortcut)
                    CreateShortcut(Environment.GetFolderPath(Environment.SpecialFolder.Programs));
                if (!options.KeepBuildFiles)
                {
                    Report(0.99, "Removing the build files");
                    TryDelete(SetupDir);
                }
                Report(1.0, "Done");
                Log("Setup finished: " + ExePath);
            }
        }

        // ---- Stage helpers -------------------------------------------------------

        string currentStage = "";
        double stageStart;

        void BuildLine(string line, double start)
        {
            // build.py prints "##stage <name>"; ninja prints "[done/total] ..."
            if (line.StartsWith("##stage "))
            {
                currentStage = line.Substring(8).Trim();
                switch (currentStage)
                {
                    case "extract": stageStart = start; Notify(stageStart, "Extracting the game files"); break;
                    case "codegen": stageStart = start + WExtract; Notify(stageStart, "Translating the game code (codegen)"); break;
                    case "compile": stageStart = start + WExtract + WCodegen; Notify(stageStart, "Compiling the game"); break;
                    case "install": stageStart = start + WExtract + WCodegen + WCompile; Notify(stageStart, "Installing"); break;
                }
                return;
            }
            if (currentStage == "compile")
            {
                Match m = Regex.Match(line, @"^\[(\d+)/(\d+)\]");
                if (m.Success)
                {
                    double done = double.Parse(m.Groups[1].Value), total = double.Parse(m.Groups[2].Value);
                    if (total > 0)
                        Notify(stageStart + WCompile * done / total,
                               string.Format("Compiling the game ({0} of {1} files)", m.Groups[1].Value, m.Groups[2].Value));
                }
            }
            else if (currentStage == "codegen")
            {
                Match m = Regex.Match(line, @"phase\s+(\S+): (\w+)");
                if (m.Success) Notify(-1, "Translating the game code: " + m.Groups[1].Value + " (" + m.Groups[2].Value + ")");
            }
        }

        void DownloadLine(string line, double start, double weight, string status)
        {
            // portable_msvc.py prints "##download <name> <done> <total>" per file
            Match m = Regex.Match(line, @"^##download (\S+) (\d+) (\d+)$");
            if (!m.Success) { Log(line); return; }
            double done = double.Parse(m.Groups[2].Value), total = double.Parse(m.Groups[3].Value);
            Notify(start + weight * 0.9 * (total > 0 ? done / total : 0), status + " (" + m.Groups[1].Value + ")");
        }

        string Obtain(string asset, string downloads, double start, double weight)
        {
            // An asset next to the setup program is used as is (offline install).
            string local = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, asset);
            if (File.Exists(local))
            {
                Log("Using " + local);
                return local;
            }
            string target = Path.Combine(downloads, asset);
            string url = "https://github.com/" + RepoSlug + "/releases/download/" + ReleaseTag + "/" + asset;
            if (File.Exists(target) && File.Exists(target + ".ok"))
            {
                Log("Already downloaded: " + asset);
                return target;
            }
            Log("Downloading " + url);
            for (int attempt = 1; ; attempt++)
            {
                try
                {
                    Download(url, target, start, weight, asset);
                    File.WriteAllText(target + ".ok", "");
                    return target;
                }
                catch (SetupCancelled) { throw; }
                catch (Exception e)
                {
                    if (attempt >= 3) throw new Exception("Download of " + asset + " failed: " + e.Message);
                    Log("Download failed (" + e.Message + "), retrying");
                    Thread.Sleep(3000);
                }
            }
        }

        void Download(string url, string target, double start, double weight, string label)
        {
            var request = (HttpWebRequest)WebRequest.Create(url);
            request.UserAgent = "HarmonyRecomp-Setup";
            request.AllowAutoRedirect = true;
            request.Timeout = 60000;
            request.ReadWriteTimeout = 120000;
            using (var response = (HttpWebResponse)request.GetResponse())
            using (var input = response.GetResponseStream())
            using (var output = File.Create(target + ".part"))
            {
                long total = response.ContentLength;
                long done = 0;
                byte[] buffer = new byte[1 << 20];
                int read;
                var timer = Stopwatch.StartNew();
                while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
                {
                    if (cancel.IsCancellationRequested) throw new SetupCancelled();
                    output.Write(buffer, 0, read);
                    done += read;
                    if (timer.ElapsedMilliseconds > 200)
                    {
                        timer.Restart();
                        double f = total > 0 ? (double)done / total : 0;
                        Report(start + weight * f, string.Format("Downloading {0} ({1} of {2} MB)", label,
                                                                 done >> 20, total > 0 ? (total >> 20).ToString() : "?"));
                    }
                }
            }
            if (File.Exists(target)) File.Delete(target);
            File.Move(target + ".part", target);
        }

        void Unzip(string zip, string dir)
        {
            if (cancel.IsCancellationRequested) throw new SetupCancelled();
            string marker = Path.Combine(dir, ".unpacked");
            string stamp = new FileInfo(zip).Length + ":" + File.GetLastWriteTimeUtc(zip).Ticks;
            if (File.Exists(marker) && File.ReadAllText(marker) == stamp) return;
            TryDelete(dir);
            Log("Unpacking " + Path.GetFileName(zip));
            ZipFile.ExtractToDirectory(zip, dir);
            File.WriteAllText(marker, stamp);
        }

        static Dictionary<string, string> ReadEnvJson(string path)
        {
            // Tiny reader for the flat {"KEY": "value"} file portable_msvc.py writes.
            var result = new Dictionary<string, string>();
            foreach (Match m in Regex.Matches(File.ReadAllText(path), "\"(\\w+)\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\""))
                result[m.Groups[1].Value] = Regex.Unescape(m.Groups[2].Value);
            if (!result.ContainsKey("INCLUDE") || !result.ContainsKey("LIB"))
                throw new Exception("Microsoft C++ components are incomplete; delete " + Path.GetDirectoryName(path) + " and run setup again.");
            return result;
        }

        // ---- Processes -------------------------------------------------------------

        int RunTool(string exe, string[] args, string cwd, Dictionary<string, string> env, Action<string> onLine)
        {
            var psi = new ProcessStartInfo(exe, JoinArgs(args))
            {
                WorkingDirectory = cwd,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                RedirectStandardInput = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            };
            foreach (var kv in env) psi.EnvironmentVariables[kv.Key] = kv.Value;
            Log("> " + Path.GetFileName(exe) + " " + psi.Arguments);
            using (var p = new Process { StartInfo = psi })
            {
                DataReceivedEventHandler handler = (s, e) =>
                {
                    if (e.Data == null) return;
                    Log(e.Data);
                    if (onLine != null) onLine(e.Data);
                };
                p.OutputDataReceived += handler;
                p.ErrorDataReceived += handler;
                p.Start();
                p.StandardInput.Close();  // tools must never wait for input
                lock (runningLock) running = p;
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                while (!p.WaitForExit(250))
                {
                    if (cancel.IsCancellationRequested)
                    {
                        KillTree(p);
                        throw new SetupCancelled();
                    }
                }
                p.WaitForExit();  // flush the asynchronous output
                lock (runningLock) running = null;
                return p.ExitCode;
            }
        }

        string CaptureTool(string exe, string[] args, string cwd, Dictionary<string, string> env)
        {
            var output = new StringBuilder();
            int code = RunTool(exe, args, cwd, env, line => output.AppendLine(line));
            if (code != 0) throw new Exception("Could not reach Microsoft's download server (see the log).");
            return output.ToString();
        }

        static void KillTree(Process p)
        {
            try
            {
                using (var kill = Process.Start(new ProcessStartInfo("taskkill", "/T /F /PID " + p.Id)
                       { CreateNoWindow = true, UseShellExecute = false }))
                {
                    kill.WaitForExit(10000);
                }
            }
            catch { }
        }

        static string JoinArgs(IEnumerable<string> args)
        {
            return string.Join(" ", args.Select(Quote));
        }

        // Windows command line quoting (CommandLineToArgvW rules).
        static string Quote(string arg)
        {
            if (arg.Length > 0 && arg.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return arg;
            var sb = new StringBuilder("\"");
            int backslashes = 0;
            foreach (char c in arg)
            {
                if (c == '\\') { backslashes++; continue; }
                if (c == '"') { sb.Append('\\', backslashes * 2 + 1); sb.Append('"'); }
                else { sb.Append('\\', backslashes); sb.Append(c); }
                backslashes = 0;
            }
            sb.Append('\\', backslashes * 2);
            sb.Append('"');
            return sb.ToString();
        }

        // ---- Misc ----------------------------------------------------------------------

        void CreateShortcut(string folder)
        {
            try
            {
                Type shellType = Type.GetTypeFromProgID("WScript.Shell");
                dynamic shell = Activator.CreateInstance(shellType);
                dynamic link = shell.CreateShortcut(Path.Combine(folder, "Castlevania Harmony of Despair (HarmonyRecomp).lnk"));
                link.TargetPath = ExePath;
                link.WorkingDirectory = options.InstallDir;
                link.Description = "Castlevania: Harmony of Despair (HarmonyRecomp)";
                link.IconLocation = ExePath + ",0";
                link.Save();
            }
            catch (Exception e)
            {
                Log("Could not create a shortcut in " + folder + ": " + e.Message);
            }
        }

        static void TryDelete(string dir)
        {
            for (int attempt = 0; attempt < 3 && Directory.Exists(dir); attempt++)
            {
                try { Directory.Delete(dir, true); }
                catch { Thread.Sleep(1000); }
            }
        }

        // On the setup thread: also the place where cancelling takes effect.
        void Report(double fraction, string status)
        {
            if (cancel.IsCancellationRequested) throw new SetupCancelled();
            progress(fraction, status);
        }

        // From tool output callbacks (thread pool): must never throw.
        // A negative fraction keeps the bar where it is.
        void Notify(double fraction, string status)
        {
            try { progress(fraction, status); } catch { }
        }

        void Log(string line)
        {
            if (logFile != null)
            {
                lock (logFile) logFile.WriteLine(line);
            }
            log(line);
        }

        public void Cancel()
        {
            lock (runningLock)
            {
                if (running != null && !running.HasExited) KillTree(running);
            }
        }
    }
}
