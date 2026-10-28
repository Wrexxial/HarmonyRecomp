// HarmonyRecomp Setup - the window.

using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

namespace HarmonyRecompSetup
{
    public sealed class MainForm : Form
    {
        const string RegistryKey = @"Software\HarmonyRecomp";
        const long RecommendedFreeBytes = 10L << 30;  // tools + build + game data

        // Options page
        readonly Panel optionsPage = new Panel { Dock = DockStyle.Fill };
        readonly TextBox gameBox = new TextBox();
        readonly Label gameCheck = new Label { AutoSize = true };
        readonly TextBox dlcBox = new TextBox();
        readonly TextBox dirBox = new TextBox();
        readonly CheckBox desktopBox = new CheckBox { Text = "Create a desktop shortcut", Checked = true, AutoSize = true };
        readonly CheckBox startMenuBox = new CheckBox { Text = "Create a Start menu entry", Checked = true, AutoSize = true };
        readonly CheckBox keepBox = new CheckBox { Text = "Keep the build tools (faster updates, needs several GB)", AutoSize = true };
        readonly Button installButton = new Button { Text = "Install", AutoSize = true };

        // Progress page
        readonly Panel progressPage = new Panel { Dock = DockStyle.Fill, Visible = false };
        readonly Label statusLabel = new Label { AutoSize = false, Height = 40 };
        readonly ProgressBar bar = new ProgressBar { Minimum = 0, Maximum = 1000, Height = 24 };
        readonly Label elapsedLabel = new Label { AutoSize = true, ForeColor = SystemColors.GrayText };
        readonly CheckBox detailsBox = new CheckBox { Text = "Show details", AutoSize = true };
        readonly TextBox logBox = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical,
                                                Font = new Font(FontFamily.GenericMonospace, 8.25f), Visible = false,
                                                WordWrap = false };
        readonly Button cancelButton = new Button { Text = "Cancel", AutoSize = true };

        // Result page
        readonly Panel donePage = new Panel { Dock = DockStyle.Fill, Visible = false };
        readonly Label doneTitle = new Label { AutoSize = true, Font = new Font(SystemFonts.MessageBoxFont.FontFamily, 13f, FontStyle.Bold) };
        readonly Label doneText = new Label { AutoSize = false };
        readonly Button playButton = new Button { Text = "Play", AutoSize = true };
        readonly Button folderButton = new Button { Text = "Open folder", AutoSize = true };
        readonly Button logButton = new Button { Text = "Open log", AutoSize = true };
        readonly Button backButton = new Button { Text = "Back", AutoSize = true };
        readonly Button closeButton = new Button { Text = "Close", AutoSize = true };

        readonly ConcurrentQueue<string> pendingLog = new ConcurrentQueue<string>();
        readonly System.Windows.Forms.Timer uiTimer = new System.Windows.Forms.Timer { Interval = 250 };
        CancellationTokenSource cancelSource;
        Installer installer;
        Thread worker;
        Stopwatch elapsed;
        double lastFraction;
        string lastStatus = "";
        readonly object statusLock = new object();

        public MainForm()
        {
            Text = "HarmonyRecomp Setup";
            AutoScaleMode = AutoScaleMode.Dpi;
            Font = SystemFonts.MessageBoxFont;
            ClientSize = new Size(640, 460);
            MinimumSize = new Size(560, 420);
            StartPosition = FormStartPosition.CenterScreen;
            Padding = new Padding(16);

            BuildOptionsPage();
            BuildProgressPage();
            BuildDonePage();
            Controls.Add(optionsPage);
            Controls.Add(progressPage);
            Controls.Add(donePage);

            uiTimer.Tick += (s, e) => FlushUi();
            FormClosing += OnClosing;
            LoadSettings();
            UpdateGameCheck();
        }

        // ---- Layout -----------------------------------------------------------------

        static Label Heading(string text)
        {
            return new Label { Text = text, AutoSize = true, Font = new Font(SystemFonts.MessageBoxFont.FontFamily, 13f, FontStyle.Bold),
                               Margin = new Padding(0, 0, 0, 8) };
        }

        static Label Paragraph(string text)
        {
            return new Label { Text = text, AutoSize = true, MaximumSize = new Size(600, 0), Margin = new Padding(0, 0, 0, 10) };
        }

        static TableLayoutPanel Stack()
        {
            return new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, AutoScroll = true };
        }

        Control PathRow(TextBox box, string buttonText, EventHandler browse)
        {
            var row = new TableLayoutPanel { ColumnCount = 2, Dock = DockStyle.Top, AutoSize = true, Margin = new Padding(0, 0, 0, 4) };
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            row.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            box.Dock = DockStyle.Fill;
            var button = new Button { Text = buttonText, AutoSize = true };
            button.Click += browse;
            row.Controls.Add(box, 0, 0);
            row.Controls.Add(button, 1, 0);
            return row;
        }

        void BuildOptionsPage()
        {
            var stack = Stack();
            stack.Controls.Add(Heading("Castlevania: Harmony of Despair for PC"));
            stack.Controls.Add(Paragraph(
                "This setup builds the PC version of the game from your own copy of the Xbox 360 game. " +
                "Nothing of the game is downloaded: the build tools are downloaded once, and your PC translates " +
                "and compiles the game. This takes about 10 to 30 minutes, depending on your PC."));

            stack.Controls.Add(new Label { Text = "Your game file (the file from 58410A7A\\000D0000, it starts with LIVE):", AutoSize = true });
            stack.Controls.Add(PathRow(gameBox, "Browse...", (s, e) => BrowseGame()));
            gameBox.TextChanged += (s, e) => UpdateGameCheck();
            stack.Controls.Add(gameCheck);

            stack.Controls.Add(new Label { Text = "DLC and title update folder (optional, files from 58410A7A\\00000002 and 000B0000):", AutoSize = true, Margin = new Padding(0, 10, 0, 0) });
            stack.Controls.Add(PathRow(dlcBox, "Browse...", (s, e) => BrowseFolder(dlcBox, "Folder with your DLC packages and title update")));

            stack.Controls.Add(new Label { Text = "Install to:", AutoSize = true, Margin = new Padding(0, 10, 0, 0) });
            stack.Controls.Add(PathRow(dirBox, "Browse...", (s, e) => BrowseFolder(dirBox, "Install folder")));

            var checks = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, Margin = new Padding(0, 10, 0, 0) };
            checks.Controls.Add(desktopBox);
            checks.Controls.Add(startMenuBox);
            checks.Controls.Add(keepBox);
            stack.Controls.Add(checks);

            var buttons = new FlowLayoutPanel { FlowDirection = FlowDirection.RightToLeft, Dock = DockStyle.Bottom, AutoSize = true };
            var exit = new Button { Text = "Exit", AutoSize = true };
            exit.Click += (s, e) => Close();
            installButton.Click += (s, e) => StartSetup();
            buttons.Controls.Add(exit);
            buttons.Controls.Add(installButton);
            var version = new Label { Text = "Version " + Installer.ReleaseTag, AutoSize = true, ForeColor = SystemColors.GrayText,
                                      Anchor = AnchorStyles.Left, Margin = new Padding(0, 8, 0, 0) };
            buttons.Controls.Add(version);

            optionsPage.Controls.Add(stack);
            optionsPage.Controls.Add(buttons);
            AcceptButton = installButton;
        }

        void BuildProgressPage()
        {
            var stack = Stack();
            stack.RowStyles.Clear();
            stack.Controls.Add(Heading("Installing"));
            statusLabel.Dock = DockStyle.Top;
            bar.Dock = DockStyle.Top;
            stack.Controls.Add(statusLabel);
            stack.Controls.Add(bar);
            stack.Controls.Add(elapsedLabel);
            stack.Controls.Add(Paragraph("You can keep using your PC. The setup uses all CPU cores while compiling, " +
                                         "so other programs may be slower until it is finished."));
            stack.Controls.Add(detailsBox);
            logBox.Dock = DockStyle.Fill;
            logBox.Height = 200;
            stack.Controls.Add(logBox);
            detailsBox.CheckedChanged += (s, e) => logBox.Visible = detailsBox.Checked;

            var buttons = new FlowLayoutPanel { FlowDirection = FlowDirection.RightToLeft, Dock = DockStyle.Bottom, AutoSize = true };
            cancelButton.Click += (s, e) => RequestCancel();
            buttons.Controls.Add(cancelButton);
            progressPage.Controls.Add(stack);
            progressPage.Controls.Add(buttons);
        }

        void BuildDonePage()
        {
            var stack = Stack();
            doneText.Dock = DockStyle.Top;
            doneText.Height = 160;
            stack.Controls.Add(doneTitle);
            stack.Controls.Add(doneText);

            var buttons = new FlowLayoutPanel { FlowDirection = FlowDirection.RightToLeft, Dock = DockStyle.Bottom, AutoSize = true };
            closeButton.Click += (s, e) => Close();
            playButton.Click += (s, e) => { Launch(Path.Combine(dirBox.Text, "harmonyrecomp.exe"), dirBox.Text); Close(); };
            folderButton.Click += (s, e) => Launch("explorer.exe", null, dirBox.Text);
            logButton.Click += (s, e) => Launch("notepad.exe", null, Path.Combine(dirBox.Text, "setup.log"));
            backButton.Click += (s, e) => ShowPage(optionsPage);
            buttons.Controls.Add(closeButton);
            buttons.Controls.Add(playButton);
            buttons.Controls.Add(folderButton);
            buttons.Controls.Add(logButton);
            buttons.Controls.Add(backButton);
            donePage.Controls.Add(stack);
            donePage.Controls.Add(buttons);
        }

        void ShowPage(Panel page)
        {
            optionsPage.Visible = page == optionsPage;
            progressPage.Visible = page == progressPage;
            donePage.Visible = page == donePage;
            AcceptButton = page == optionsPage ? installButton : page == donePage ? playButton : null;
        }

        // ---- Options ---------------------------------------------------------------

        void BrowseGame()
        {
            using (var dialog = new OpenFileDialog { Title = "Select your game file", Filter = "All files (*.*)|*.*", CheckFileExists = true })
            {
                if (File.Exists(gameBox.Text)) dialog.InitialDirectory = Path.GetDirectoryName(gameBox.Text);
                if (dialog.ShowDialog(this) == DialogResult.OK) gameBox.Text = dialog.FileName;
            }
        }

        void BrowseFolder(TextBox box, string description)
        {
            using (var dialog = new FolderBrowserDialog { Description = description, ShowNewFolderButton = true })
            {
                if (Directory.Exists(box.Text)) dialog.SelectedPath = box.Text;
                if (dialog.ShowDialog(this) == DialogResult.OK) box.Text = dialog.SelectedPath;
            }
        }

        bool UpdateGameCheck()
        {
            string path = gameBox.Text.Trim().Trim('"');
            if (path.Length == 0)
            {
                gameCheck.Text = "Select the game file to continue.";
                gameCheck.ForeColor = SystemColors.GrayText;
                return false;
            }
            if (!File.Exists(path))
            {
                gameCheck.Text = "File not found.";
                gameCheck.ForeColor = Color.Firebrick;
                return false;
            }
            string problem = Installer.CheckGameFile(path);
            gameCheck.Text = problem ?? "✔ Castlevania: Harmony of Despair found.";
            gameCheck.ForeColor = problem == null ? Color.ForestGreen : Color.Firebrick;
            return problem == null;
        }

        void LoadSettings()
        {
            string defaultDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HarmonyRecomp");
            using (var key = Registry.CurrentUser.OpenSubKey(RegistryKey))
            {
                dirBox.Text = key == null ? defaultDir : (string)key.GetValue("InstallDir", defaultDir);
                gameBox.Text = key == null ? "" : (string)key.GetValue("GameFile", "");
                dlcBox.Text = key == null ? "" : (string)key.GetValue("DlcDir", "");
            }
            if (File.Exists(Path.Combine(dirBox.Text, "harmonyrecomp.exe"))) installButton.Text = "Update / Repair";
        }

        void SaveSettings(SetupOptions options)
        {
            try
            {
                using (var key = Registry.CurrentUser.CreateSubKey(RegistryKey))
                {
                    key.SetValue("InstallDir", options.InstallDir);
                    key.SetValue("GameFile", options.GameFile);
                    key.SetValue("DlcDir", options.DlcDir ?? "");
                    key.SetValue("Version", Installer.ReleaseTag);
                }
            }
            catch { }
        }

        // ---- Setup -------------------------------------------------------------------

        void StartSetup()
        {
            if (!UpdateGameCheck())
            {
                MessageBox.Show(this, gameCheck.Text, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            string dir = dirBox.Text.Trim().Trim('"');
            string dlc = dlcBox.Text.Trim().Trim('"');
            if (dir.Length == 0 || !Path.IsPathRooted(dir))
            {
                MessageBox.Show(this, "Please choose a full install folder path.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (dlc.Length > 0 && !Directory.Exists(dlc))
            {
                MessageBox.Show(this, "The DLC folder does not exist.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            try
            {
                var drive = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(dir)));
                if (drive.AvailableFreeSpace < RecommendedFreeBytes &&
                    MessageBox.Show(this, string.Format("Only {0:0.0} GB are free on {1}. The setup needs about 10 GB while " +
                                    "it runs (about 1 GB afterwards). Continue anyway?", drive.AvailableFreeSpace / 1073741824.0, drive.Name),
                                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                    return;
            }
            catch { }

            var options = new SetupOptions
            {
                InstallDir = Path.GetFullPath(dir),
                GameFile = Path.GetFullPath(gameBox.Text.Trim().Trim('"')),
                DlcDir = dlc.Length > 0 ? Path.GetFullPath(dlc) : null,
                KeepBuildFiles = keepBox.Checked,
                DesktopShortcut = desktopBox.Checked,
                StartMenuShortcut = startMenuBox.Checked,
            };
            SaveSettings(options);

            cancelSource = new CancellationTokenSource();
            installer = new Installer(options, OnProgress, line => pendingLog.Enqueue(line), AskLicense, cancelSource.Token);
            logBox.Clear();
            bar.Value = 0;
            lastFraction = 0;
            lastStatus = "Starting";
            elapsed = Stopwatch.StartNew();
            cancelButton.Enabled = true;
            ShowPage(progressPage);
            uiTimer.Start();

            worker = new Thread(() => RunSetup()) { IsBackground = true, Name = "setup" };
            worker.Start();
        }

        void RunSetup()
        {
            Exception error = null;
            try
            {
                installer.Run();
            }
            catch (Exception e)
            {
                error = e;
            }
            BeginInvoke((Action)(() => Finished(error)));
        }

        void Finished(Exception error)
        {
            uiTimer.Stop();
            FlushUi();
            playButton.Visible = error == null;
            backButton.Visible = error != null;
            folderButton.Visible = error == null;
            if (error == null)
            {
                doneTitle.Text = "Ready to play";
                doneText.Text = "Castlevania: Harmony of Despair is installed in\r\n" + dirBox.Text + "\r\n\r\n" +
                                "Start it with \"Play\" or the shortcut. F6 opens the graphics settings, F2 shows the FPS. " +
                                "For online co-op see the HarmonyRecomp page on GitHub.\r\n\r\n" +
                                "To update later, run the newest setup again: your settings and saves are kept.";
            }
            else if (error is SetupCancelled)
            {
                doneTitle.Text = "Setup cancelled";
                doneText.Text = "Nothing was changed on your game file. Downloaded files are kept, so running the setup " +
                                "again continues where it stopped.";
            }
            else
            {
                doneTitle.Text = "Setup failed";
                doneText.Text = error.Message + "\r\n\r\nThe log (\"Open log\") has the details. If you report the problem " +
                                "on GitHub, please attach it.";
            }
            ShowPage(donePage);
            installer = null;
        }

        void OnProgress(double fraction, string status)
        {
            lock (statusLock)
            {
                if (fraction >= 0) lastFraction = Math.Max(lastFraction, Math.Min(1.0, fraction));
                if (!string.IsNullOrEmpty(status)) lastStatus = status;
            }
        }

        bool AskLicense(string url)
        {
            bool accepted = false;
            Invoke((Action)(() =>
            {
                using (var dialog = new LicenseDialog(url)) accepted = dialog.ShowDialog(this) == DialogResult.OK;
            }));
            return accepted;
        }

        void FlushUi()
        {
            lock (statusLock)
            {
                bar.Value = (int)(lastFraction * 1000);
                statusLabel.Text = lastStatus;
            }
            if (elapsed != null)
                elapsedLabel.Text = string.Format("Elapsed: {0:%h}:{0:mm}:{0:ss}", elapsed.Elapsed);
            var sb = new StringBuilder();
            string line;
            int count = 0;
            while (count < 2000 && pendingLog.TryDequeue(out line))
            {
                sb.Append(line).Append("\r\n");
                count++;
            }
            if (sb.Length > 0)
            {
                if (logBox.TextLength > 400000) logBox.Text = logBox.Text.Substring(logBox.TextLength - 200000);
                logBox.AppendText(sb.ToString());
            }
        }

        void RequestCancel()
        {
            if (installer == null) return;
            if (MessageBox.Show(this, "Cancel the setup? Downloads are kept for the next attempt.", Text,
                                MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                return;
            cancelButton.Enabled = false;
            statusLabel.Text = "Cancelling...";
            cancelSource.Cancel();
            installer.Cancel();
        }

        void OnClosing(object sender, FormClosingEventArgs e)
        {
            if (installer != null && worker != null && worker.IsAlive)
            {
                e.Cancel = true;
                RequestCancel();
            }
        }

        static void Launch(string file, string workingDir, string arguments = null)
        {
            try
            {
                var psi = new ProcessStartInfo(file) { UseShellExecute = true };
                if (arguments != null) psi.Arguments = "\"" + arguments + "\"";
                if (workingDir != null) psi.WorkingDirectory = workingDir;
                Process.Start(psi);
            }
            catch (Exception e)
            {
                MessageBox.Show(e.Message, "HarmonyRecomp Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }
    }

    /// <summary>Asks to accept Microsoft's license for the C++ build components.</summary>
    sealed class LicenseDialog : Form
    {
        public LicenseDialog(string url)
        {
            Text = "Microsoft C++ build components";
            AutoScaleMode = AutoScaleMode.Dpi;
            Font = SystemFonts.MessageBoxFont;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(520, 230);
            Padding = new Padding(16);

            var text = new Label
            {
                Text = "Compiling the game needs Microsoft's C/C++ runtime headers and libraries and the Windows SDK. " +
                       "They are not installed on this PC (they come with Visual Studio). The setup can download just " +
                       "these parts (about 400 MB) directly from Microsoft.\r\n\r\n" +
                       "They are licensed by Microsoft under the Visual Studio Build Tools license:",
                AutoSize = true, MaximumSize = new Size(488, 0), Dock = DockStyle.Top,
            };
            var link = new LinkLabel { Text = url, AutoSize = true, Dock = DockStyle.Top, Padding = new Padding(0, 8, 0, 8) };
            link.LinkClicked += (s, e) => { try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); } catch { } };
            var buttons = new FlowLayoutPanel { FlowDirection = FlowDirection.RightToLeft, Dock = DockStyle.Bottom, AutoSize = true };
            var decline = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, AutoSize = true };
            var accept = new Button { Text = "Accept and download", DialogResult = DialogResult.OK, AutoSize = true };
            buttons.Controls.Add(decline);
            buttons.Controls.Add(accept);
            Controls.Add(link);
            Controls.Add(text);
            Controls.Add(buttons);
            AcceptButton = accept;
            CancelButton = decline;
        }
    }
}
