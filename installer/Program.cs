// HarmonyRecomp Setup - entry point.

using System;
using System.Net;
using System.Windows.Forms;

namespace HarmonyRecompSetup
{
    static class Program
    {
        [STAThread]
        static int Main(string[] args)
        {
            // GitHub only accepts TLS 1.2 and newer.
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;

            // Console self-test for the release workflow: checks a game file
            // header without opening the window ("--check-game <file>").
            if (args.Length == 2 && args[0] == "--check-game")
            {
                string problem = Installer.CheckGameFile(args[1]);
                Console.WriteLine(problem ?? "ok");
                return problem == null ? 0 : 1;
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
            return 0;
        }
    }
}
