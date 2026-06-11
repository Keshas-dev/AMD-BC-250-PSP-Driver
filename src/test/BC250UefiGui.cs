using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace BC250UefiGui
{
    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }

    internal class UefiNative
    {
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        public static extern uint GetFirmwareEnvironmentVariableW(
            string lpName, string lpGuid, IntPtr pBuffer, uint nSize);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        public static extern bool SetFirmwareEnvironmentVariableW(
            string lpName, string lpGuid, IntPtr pBuffer, uint nSize);

        // Attempt to enable SeSystemEnvironmentPrivilege
        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool OpenProcessToken(IntPtr h, uint acc, out IntPtr tok);
        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool AdjustTokenPrivileges(IntPtr tok, bool dis, ref TokPriv priv, uint len, IntPtr prev, IntPtr rel);
        [DllImport("advapi32.dll", CharSet = CharSet.Auto)]
        static extern bool LookupPrivilegeValue(string host, string name, out long luid);

        struct TokPriv
        {
            public uint PrivCount;
            public long Luid;
            public uint Attr;
        }

        const uint SE_PRIVILEGE_ENABLED = 0x2;
        const uint TOKEN_QUERY = 0x8;
        const uint TOKEN_ADJUST_PRIVILEGES = 0x20;

        public static int LastReadError = 0;

        public static bool EnableSystemPrivilege()
        {
            try
            {
                IntPtr htok;
                if (!OpenProcessToken(new IntPtr(-1), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, out htok))
                    return false;
                long luid;
                LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out luid);
                var tp = new TokPriv { PrivCount = 1, Luid = luid, Attr = SE_PRIVILEGE_ENABLED };
                bool ok = AdjustTokenPrivileges(htok, false, ref tp, (uint)Marshal.SizeOf(tp), IntPtr.Zero, IntPtr.Zero);
                return ok;
            }
            catch { return false; }
        }

        public static byte[] ReadVariable(string name, Guid guid)
        {
            uint size = 65536;
            IntPtr buf = Marshal.AllocHGlobal((int)size);
            try
            {
                EnableSystemPrivilege();
                uint ret = GetFirmwareEnvironmentVariableW(name, "{" + guid.ToString("D").ToUpper() + "}", buf, size);
                if (ret == 0)
                {
                    LastReadError = Marshal.GetLastWin32Error();
                    return null;
                }
                byte[] result = new byte[ret];
                Marshal.Copy(buf, result, 0, (int)ret);
                return result;
            }
            finally { Marshal.FreeHGlobal(buf); }
        }

        public static int WriteVariable(string name, Guid guid, byte[] data)
        {
            IntPtr buf = Marshal.AllocHGlobal(data.Length);
            try
            {
                EnableSystemPrivilege();
                Marshal.Copy(data, 0, buf, data.Length);
                bool ok = SetFirmwareEnvironmentVariableW(name, "{" + guid.ToString("D").ToUpper() + "}", buf, (uint)data.Length);
                if (!ok) return Marshal.GetLastWin32Error();
                return 0;
            }
            finally { Marshal.FreeHGlobal(buf); }
        }
    }

    public class UefiVariable
    {
        public string Name { get; set; }
        public Guid Guid { get; set; }
        public byte[] Data { get; set; }
        public int Size { get { return Data != null ? Data.Length : 0; } }
        public string GuidStr { get { return Guid.ToString("D").ToUpper(); } }
        public bool IsReadable { get { return Data != null; } }
        public string ErrorMsg { get; set; }

        public UefiVariable(string name, Guid guid)
        {
            Name = name;
            Guid = guid;
        }

        public override string ToString()
        {
            return string.Format("{0} [{1}] {2}B", Name, GuidStr, Size);
        }
    }

    public class MainForm : Form
    {
        private ListView _varList;
        private TextBox _hexBox;
        private TextBox _statusBar;
        private Button _btnRefresh;
        private Button _btnApply;
        private Button _btnFindIo;
        private Button _btnCopy;
        private ComboBox _modeCombo;
        private List<UefiVariable> _variables;
        private bool _modified;
        private byte[] _originalData;
        private ToolTip _tooltip;

        // Setup variable: GUID from SCT IFR
        private static readonly Guid SetupGuid = new Guid("{EC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9}");
        private static readonly Guid CommonGuid = new Guid("{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}");
        private static readonly Guid CpuFeaturesGuid = new Guid("{C0B4FB05-15E5-4588-9FE9-B3D39C067715}");
        private static readonly Guid AmiTseGuid = new Guid("{B4909CF3-7B93-4751-9BD8-5BA8220B9BB2}");

        private static readonly string[] VarNames = { "Setup", "AmdSetup", "CbsSetup", "SaSetup", "PchSetup", "CpuSetup" };
        private static readonly Guid[] VarGuids = { SetupGuid, CommonGuid, CpuFeaturesGuid, AmiTseGuid };

        public MainForm()
        {
            Text = "BC-250 UEFI Setup Editor";
            Size = new Size(1200, 750);
            MinimumSize = new Size(800, 500);
            _variables = new List<UefiVariable>();
            InitializeComponents();
        }

        private void InitializeComponents()
        {
            var mainPanel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 3, Padding = new Padding(5) };
            mainPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 35));
            mainPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 65));
            mainPanel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            mainPanel.RowStyles.Add(new RowStyle(SizeType.Percent, 90));
            mainPanel.RowStyles.Add(new RowStyle(SizeType.AutoSize));

            // Top toolbar
            var toolPanel = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight, Height = 36 };
            _btnRefresh = new Button { Text = "Read Variables", Width = 120, Height = 28 };
            _btnRefresh.Click += BtnRefresh_Click;
            _btnApply = new Button { Text = "Apply Changes", Width = 110, Height = 28, Enabled = false };
            _btnApply.Click += BtnApply_Click;
            _btnFindIo = new Button { Text = "Find IOMMU", Width = 100, Height = 28, Enabled = false };
            _btnFindIo.Click += BtnFindIo_Click;
            _btnCopy = new Button { Text = "Copy Hex", Width = 80, Height = 28 };
            _btnCopy.Click += BtnCopy_Click;
            _modeCombo = new ComboBox { Width = 150, Height = 28, DropDownStyle = ComboBoxStyle.DropDownList };
            _modeCombo.Items.AddRange(new[] { "Hex View", "ASCII View", "Offset Search" });
            _modeCombo.SelectedIndex = 0;
            _modeCombo.SelectedIndexChanged += ModeCombo_Changed;

            toolPanel.Controls.AddRange(new Control[] { _btnRefresh, _btnApply, _btnFindIo, _btnCopy, _modeCombo });
            mainPanel.Controls.Add(toolPanel, 0, 0);
            mainPanel.SetColumnSpan(toolPanel, 2);

            // Left: Variable list
            _varList = new ListView { Dock = DockStyle.Fill, View = View.Details, FullRowSelect = true, HideSelection = false };
            _varList.Columns.Add("Variable", 180);
            _varList.Columns.Add("Size", 50);
            _varList.Columns.Add("Status", 100);
            _varList.SelectedIndexChanged += VarList_Selected;
            _varList.DoubleClick += VarList_DoubleClick;
            _varList.ContextMenuStrip = CreateContextMenu();
            mainPanel.Controls.Add(_varList, 0, 1);

            // Right: Hex editor
            var rightPanel = new SplitContainer { Dock = DockStyle.Fill, Orientation = Orientation.Horizontal };
            _hexBox = new TextBox
            {
                Dock = DockStyle.Fill,
                Multiline = true,
                Font = new Font("Consolas", 9),
                ScrollBars = ScrollBars.Both,
                WordWrap = false,
                ReadOnly = false,
                BackColor = Color.White,
                ForeColor = Color.Black,
                Text = "Select a variable to view/edit"
            };
            _hexBox.TextChanged += HexBox_TextChanged;
            _hexBox.KeyDown += HexBox_KeyDown;
            rightPanel.Panel1.Controls.Add(_hexBox);

            var infoPanel = new Panel { Dock = DockStyle.Fill };
            _statusBar = new TextBox
            {
                Dock = DockStyle.Fill,
                Multiline = true,
                Font = new Font("Consolas", 8),
                ScrollBars = ScrollBars.Vertical,
                ReadOnly = true,
                BackColor = Color.FromArgb(240, 240, 240),
                Text = "Ready. Click 'Read Variables' to start."
            };
            infoPanel.Controls.Add(_statusBar);
            rightPanel.Panel2.Controls.Add(infoPanel);
            mainPanel.Controls.Add(rightPanel, 1, 1);

            // Status bar
            var bottomBar = new StatusStrip();
            var statusLabel = new ToolStripStatusLabel("BC-250 UEFI Editor v1.0 | Run as Admin");
            bottomBar.Items.Add(statusLabel);
            mainPanel.Controls.Add(bottomBar, 0, 2);
            mainPanel.SetColumnSpan(bottomBar, 2);

            Controls.Add(mainPanel);

            _tooltip = new ToolTip();
            _tooltip.SetToolTip(_btnFindIo, "Search for IOMMU/OS Selection byte patterns");
        }

        private ContextMenuStrip CreateContextMenu()
        {
            var menu = new ContextMenuStrip();
            menu.Items.Add("Read", null, (s, e) => BtnRefresh_Click(null, null));
            menu.Items.Add("Copy Name+GUID", null, (s, e) => CopyVariableInfo());
            return menu;
        }

        private void BtnRefresh_Click(object sender, EventArgs e)
        {
            Cursor = Cursors.WaitCursor;
            _variables.Clear();
            _varList.Items.Clear();
            Log("Scanning UEFI variables...");

            foreach (var name in VarNames)
            {
                foreach (var guid in VarGuids)
                {
                    var v = new UefiVariable(name, guid);
                    v.Data = UefiNative.ReadVariable(name, guid);
                    if (v.IsReadable)
                    {
                        Log(string.Format("  FOUND: {0} {{{1}}} = {2}B", v.Name, v.GuidStr, v.Size));
                    }
                    else if (UefiNative.LastReadError != 0)
                    {
                        v.ErrorMsg = string.Format("Error {0}: {1}", UefiNative.LastReadError, GetErrorMessage(UefiNative.LastReadError));
                        Log(string.Format("  FAIL: {0} {{{1}}} - {2}", v.Name, v.GuidStr, v.ErrorMsg));
                    }
                    _variables.Add(v);
                }
            }

            // Add well-known variables
            _variables.Add(new UefiVariable("BootOrder", new Guid("{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}")));
            _variables.Add(new UefiVariable("Timeout", new Guid("{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}")));

            // Try to read them too
            foreach (var v in _variables)
            {
                if (!v.IsReadable && v.ErrorMsg == null)
                {
                    try { v.Data = UefiNative.ReadVariable(v.Name, v.Guid); }
                    catch { }
                }
            }

            RefreshVariableList();
            Cursor = Cursors.Default;
            Log("Scan complete.");
        }

        private void RefreshVariableList()
        {
            _varList.BeginUpdate();
            _varList.Items.Clear();
            foreach (var v in _variables)
            {
                var item = new ListViewItem(v.Name);
                item.SubItems.Add(v.IsReadable ? v.Size.ToString() : "-");
                item.SubItems.Add(v.IsReadable ? "OK" : (v.ErrorMsg ?? "Not found"));
                item.Tag = v;
                item.BackColor = v.IsReadable ? Color.White : Color.LightGray;
                _varList.Items.Add(item);
            }
            _varList.EndUpdate();
        }

        private void VarList_Selected(object sender, EventArgs e)
        {
            if (_varList.SelectedItems.Count == 0) return;
            var v = _varList.SelectedItems[0].Tag as UefiVariable;
            if (v != null) DisplayVariable(v);
        }

        private void VarList_DoubleClick(object sender, EventArgs e)
        {
            if (_varList.SelectedItems.Count == 0) return;
            var v = _varList.SelectedItems[0].Tag as UefiVariable;
            if (v != null && v.IsReadable) ShowVariableDetails(v);
        }

        private void DisplayVariable(UefiVariable v)
        {
            if (!v.IsReadable)
            {
                _hexBox.Text = string.Format("Variable not accessible.\nName: {0}\nGUID: {{{1}}}\nError: {2}",
                    v.Name, v.GuidStr, v.ErrorMsg ?? "Not present in NVRAM");
                _originalData = null;
                _btnApply.Enabled = false;
                _btnFindIo.Enabled = false;
                return;
            }

            _originalData = (byte[])v.Data.Clone();
            UpdateHexDisplay(v.Data);
            _btnApply.Enabled = true;
            _btnFindIo.Enabled = true;
        }

        private void UpdateHexDisplay(byte[] data)
        {
            if (data == null) { _hexBox.Text = ""; return; }

            var sb = new StringBuilder();
            sb.AppendFormat("Offset  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F  ASCII\r\n");
            sb.AppendFormat("------  ------------------------------------------------  ----------------\r\n");

            for (int i = 0; i < data.Length; i += 16)
            {
                sb.AppendFormat("{0:X6}: ", i);
                for (int j = 0; j < 16; j++)
                {
                    if (i + j < data.Length)
                        sb.AppendFormat("{0:X2} ", data[i + j]);
                    else
                        sb.Append("   ");
                }
                sb.Append(" ");
                for (int j = 0; j < 16 && i + j < data.Length; j++)
                {
                    char c = (char)data[i + j];
                    sb.Append(c >= 32 && c < 127 ? c : '.');
                }
                sb.AppendLine();
            }

            _hexBox.Text = sb.ToString();
            _hexBox.Select(0, 0);
        }

        private void ShowVariableDetails(UefiVariable v)
        {
            var msg = string.Format(
                "Variable: {0}\r\nGUID: {{{1}}}\r\nSize: {2} bytes\r\n\r\nRaw hex (first 64 bytes):\r\n{3}",
                v.Name, v.GuidStr, v.Size,
                BitConverter.ToString(v.Data, 0, Math.Min(64, v.Data.Length)).Replace("-", " "));
            MessageBox.Show(msg, "Variable Details", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        private void BtnApply_Click(object sender, EventArgs e)
        {
            if (_originalData == null || _varList.SelectedItems.Count == 0) return;
            var v = _varList.SelectedItems[0].Tag as UefiVariable;
            if (v == null || !v.IsReadable) return;

            var newData = ParseHexBox();
            if (newData == null) return;

            if (ArraysEqual(newData, _originalData))
            {
                MessageBox.Show("No changes detected.", "Apply", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            int changed = CountDifferences(newData, _originalData);
            var result = MessageBox.Show(
                string.Format("{0} byte(s) changed. Write to NVRAM?\r\n\r\nThis will require a reboot to take effect.",
                    changed),
                "Confirm Write", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);

            if (result == DialogResult.Yes)
            {
                Cursor = Cursors.WaitCursor;
                int err = UefiNative.WriteVariable(v.Name, v.Guid, newData);
                Cursor = Cursors.Default;

                if (err == 0)
                {
                    Log(string.Format("SUCCESS: {0} {{{1}}} updated ({2}B written)", v.Name, v.GuidStr, newData.Length));
                    MessageBox.Show("Variable written successfully.\nReboot required for changes to take effect.",
                        "Success", MessageBoxButtons.OK, MessageBoxIcon.Information);
                    v.Data = newData;
                    _originalData = (byte[])newData.Clone();
                    _modified = false;
                }
                else
                {
                    string errMsg = GetErrorMessage(err);
                    Log(string.Format("FAILED: {0} {{{1}}} error {2}: {3}", v.Name, v.GuidStr, err, errMsg));
                    MessageBox.Show(string.Format("Write failed.\nError {0}: {1}", err, errMsg),
                        "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            }
        }

        private byte[] ParseHexBox()
        {
            try
            {
                var lines = _hexBox.Text.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
                var bytes = new List<byte>();

                foreach (var line in lines)
                {
                    if (line.StartsWith("Offset") || line.StartsWith("------") || line.Trim() == "")
                        continue;

                    // Parse hex columns (after offset)
                    if (line.Length >= 58)
                    {
                        var hexPart = line.Substring(7, 48);
                        var hexChars = hexPart.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                        foreach (var h in hexChars)
                        {
                            byte b;
                            if (h.Length == 2 && byte.TryParse(h, System.Globalization.NumberStyles.HexNumber, null, out b))
                                bytes.Add(b);
                        }
                    }
                }

                return bytes.Count > 0 ? bytes.ToArray() : null;
            }
            catch
            {
                MessageBox.Show("Failed to parse hex data.", "Parse Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return null;
            }
        }

        private void BtnFindIo_Click(object sender, EventArgs e)
        {
            if (_originalData == null) return;

            var data = _originalData;
            var results = new StringBuilder();

            results.AppendLine("=== Pattern Search in Setup Variable ===");
            results.AppendLine(string.Format("Size: {0} bytes ({1:X}h)", data.Length, data.Length));

            // Search for known patterns
            results.AppendLine("\n--- Known byte patterns ---");

            // IOMMU: often at offset 0x18-0x30 area
            results.AppendLine("\nIOMMU candidates (searching for 00/01 values at plausible offsets):");
            for (int i = 0; i < Math.Min(100, data.Length); i++)
            {
                if (data[i] <= 2)
                    results.AppendLine(string.Format("  Offset 0x{0:X2} ({0}): value={1}", i, data[i]));
            }

            // OS Selection: often 00, 01, 02 
            results.AppendLine("\nOS Selection candidates (00=Default, 01=Windows, 02=Linux):");
            for (int i = 0; i < Math.Min(200, data.Length); i++)
            {
                if (data[i] <= 2 && (i == 0 || data[i - 1] <= 1))
                {
                    var ctx = "";
                    for (int j = Math.Max(0, i - 2); j <= Math.Min(data.Length - 1, i + 3); j++)
                        ctx += string.Format("{0:X2} ", data[j]);
                    results.AppendLine(string.Format("  Offset 0x{0:X2}: val={1} context: {2}", i, data[i], ctx));
                }
            }

            // Null-terminated strings
            results.AppendLine("\nInteresting strings found:");
            for (int i = 0; i < data.Length - 3; i++)
            {
                if (data[i] >= 0x20 && data[i] <= 0x7e)
                {
                    var str = new StringBuilder();
                    int j = i;
                    while (j < data.Length && data[j] >= 0x20 && data[j] <= 0x7e && str.Length < 30)
                        str.Append((char)data[j++]);
                    if (str.Length >= 4)
                    {
                        results.AppendLine(string.Format("  Offset 0x{0:X4}: \"{1}\"", i, str));
                        i = j;
                    }
                }
            }

            _statusBar.Text = results.ToString();
        }

        private static string GetErrorMessage(int err)
        {
            switch (err)
            {
                case 0: return "Success";
                case 5: return "ERROR_ACCESS_DENIED (Run as Admin)";
                case 13: return "ERROR_INVALID_DATA";
                case 87: return "ERROR_INVALID_PARAMETER";
                case 1314: return "ERROR_PRIVILEGE_NOT_HELD (requires SeSystemEnvironmentPrivilege)";
                case 1460: return "ERROR_TIMEOUT";
                case 0x3EB: return "ERROR_NO_MORE_ITEMS";
                case 0x3E6: return "ERROR_NO_SUCH_USER";
                default: return string.Format("Unknown error (0x{0:X})", err);
            }
        }

        private void ModeCombo_Changed(object sender, EventArgs e)
        {
            if (_originalData == null) return;
            UpdateHexDisplay(_originalData);
        }

        private void HexBox_TextChanged(object sender, EventArgs e)
        {
            _modified = true;
        }

        private void HexBox_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Control && e.KeyCode == Keys.C)
            {
                if (!string.IsNullOrEmpty(_hexBox.SelectedText))
                    Clipboard.SetText(_hexBox.SelectedText);
            }
        }

        private void BtnCopy_Click(object sender, EventArgs e)
        {
            if (!string.IsNullOrEmpty(_hexBox.Text) && _originalData != null)
            {
                Clipboard.SetText(_hexBox.Text);
                Log("Hex view copied to clipboard.");
            }
        }

        private void CopyVariableInfo()
        {
            if (_varList.SelectedItems.Count == 0) return;
            var v = _varList.SelectedItems[0].Tag as UefiVariable;
            if (v != null)
            {
                var info = string.Format("{0} {{{1}}}", v.Name, v.GuidStr);
                Clipboard.SetText(info);
                Log(string.Format("Copied: {0}", info));
            }
        }

        private void Log(string msg)
        {
            var timestamp = DateTime.Now.ToString("HH:mm:ss");
            _statusBar.AppendText(string.Format("[{0}] {1}\r\n", timestamp, msg));
            _statusBar.SelectionStart = _statusBar.Text.Length;
            _statusBar.ScrollToCaret();
        }

        private static bool ArraysEqual(byte[] a, byte[] b)
        {
            if (a.Length != b.Length) return false;
            for (int i = 0; i < a.Length; i++)
                if (a[i] != b[i]) return false;
            return true;
        }

        private static int CountDifferences(byte[] a, byte[] b)
        {
            int count = 0;
            int len = Math.Min(a.Length, b.Length);
            for (int i = 0; i < len; i++)
                if (a[i] != b[i]) count++;
            return count;
        }

        protected override void OnLoad(EventArgs e)
        {
            base.OnLoad(e);
            // Auto-scan on startup
            BtnRefresh_Click(null, null);
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            base.OnFormClosing(e);
            if (_modified && _originalData != null)
            {
                var result = MessageBox.Show("Unsaved changes will be lost. Exit anyway?",
                    "Unsaved Changes", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
                if (result == DialogResult.No) e.Cancel = true;
            }
        }
    }
}