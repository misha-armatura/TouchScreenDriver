using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace TouchScreenLib
{
    public class MonitorInfo
    {
        public int Index { get; set; }
        public string Name { get; set; } = string.Empty;
        public bool IsPrimary { get; set; }
        public int X { get; set; }
        public int Y { get; set; }
        public int Width { get; set; }
        public int Height { get; set; }
        public double ScaleX { get; set; } = 1.0;
        public double ScaleY { get; set; } = 1.0;
        public string Rotation { get; set; } = "normal";
        public string EdidHash { get; set; } = string.Empty;
    }

    public class DesktopLayout
    {
        public List<MonitorInfo> Monitors { get; } = new List<MonitorInfo>();
        public int OriginX { get; set; }
        public int OriginY { get; set; }
        public int Width { get; set; }
        public int Height { get; set; }
        public string Hash { get; set; } = string.Empty;
    }

    public static class MonitorHelper
    {
        public static List<MonitorInfo> GetMonitors()
        {
            return GetDesktopLayout().Monitors;
        }

        public static DesktopLayout GetDesktopLayout()
        {
            var layout = new DesktopLayout();
            var listOutput = RunCommand("xrandr --listmonitors");
            var lines = listOutput.Split('\n', StringSplitOptions.RemoveEmptyEntries);
            if (lines.Length <= 1)
            {
                return layout;
            }

            foreach (var line in lines.Skip(1))
            {
                var trimmed = line.Trim();
                if (trimmed.Length == 0)
                {
                    continue;
                }
                var tokens = trimmed.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (tokens.Length < 3)
                {
                    continue;
                }
                var monitor = new MonitorInfo();
                var indexToken = tokens[0].TrimEnd(':');
                if (!int.TryParse(indexToken, out var idx))
                {
                    idx = layout.Monitors.Count;
                }
                monitor.Index = idx;
                monitor.IsPrimary = trimmed.Contains("+*");

                string geometry = tokens.FirstOrDefault(t => t.Contains('x') && t.Contains('+')) ?? string.Empty;
                if (geometry.Length == 0)
                {
                    continue;
                }
                var regex = new Regex(@"(\d+)/\d+x(\d+)/\d+([+-]\d+)([+-]\d+)");
                var match = regex.Match(geometry);
                if (!match.Success)
                {
                    continue;
                }
                monitor.Width = int.Parse(match.Groups[1].Value, CultureInfo.InvariantCulture);
                monitor.Height = int.Parse(match.Groups[2].Value, CultureInfo.InvariantCulture);
                monitor.X = int.Parse(match.Groups[3].Value, CultureInfo.InvariantCulture);
                monitor.Y = int.Parse(match.Groups[4].Value, CultureInfo.InvariantCulture);
                monitor.Name = tokens[^1];
                layout.Monitors.Add(monitor);
            }

            if (!layout.Monitors.Any())
            {
                return layout;
            }

            // Parse verbose information for rotation, scale, EDID
            var verbose = RunCommand("xrandr --verbose");
            var verboseLines = verbose.Split('\n');
            MonitorInfo? current = null;
            for (var i = 0; i < verboseLines.Length; i++)
            {
                var line = verboseLines[i];
                if (line.Length == 0)
                {
                    continue;
                }
                if (!char.IsWhiteSpace(line[0]))
                {
                    var trimmed = line.Trim();
                    current = layout.Monitors.FirstOrDefault(m => trimmed.StartsWith(m.Name + " ", StringComparison.Ordinal));
                    if (current != null)
                    {
                        var open = trimmed.IndexOf('(');
                        var close = trimmed.IndexOf(')', open + 1);
                        if (open >= 0 && close > open)
                        {
                            var rotationToken = trimmed.Substring(open + 1, close - open - 1).Split(' ', StringSplitOptions.RemoveEmptyEntries).FirstOrDefault();
                            if (!string.IsNullOrEmpty(rotationToken))
                            {
                                current.Rotation = rotationToken.ToLowerInvariant();
                            }
                        }
                    }
                    continue;
                }
                if (current == null)
                {
                    continue;
                }
                var trimmedLine = line.Trim();
                if (trimmedLine.StartsWith("Scale:", StringComparison.OrdinalIgnoreCase))
                {
                    var parts = trimmedLine.Substring(6).Split('x', StringSplitOptions.RemoveEmptyEntries);
                    if (parts.Length == 2)
                    {
                        if (double.TryParse(parts[0], NumberStyles.Float, CultureInfo.InvariantCulture, out var sx))
                        {
                            current.ScaleX = sx;
                        }
                        if (double.TryParse(parts[1], NumberStyles.Float, CultureInfo.InvariantCulture, out var sy))
                        {
                            current.ScaleY = sy;
                        }
                    }
                }
                else if (trimmedLine == "EDID:")
                {
                    var edidBuilder = new StringBuilder();
                    int j = i + 1;
                    while (j < verboseLines.Length && verboseLines[j].StartsWith("\t", StringComparison.Ordinal))
                    {
                        edidBuilder.Append(verboseLines[j].Trim());
                        j++;
                    }
                    if (edidBuilder.Length > 0)
                    {
                        current.EdidHash = HashString(edidBuilder.ToString());
                    }
                }
            }

            var minX = layout.Monitors.Min(m => m.X);
            var minY = layout.Monitors.Min(m => m.Y);
            var maxX = layout.Monitors.Max(m => m.X + m.Width);
            var maxY = layout.Monitors.Max(m => m.Y + m.Height);
            layout.OriginX = minX;
            layout.OriginY = minY;
            layout.Width = maxX - minX;
            layout.Height = maxY - minY;

            var hashBuilder = new StringBuilder();
            hashBuilder.Append(layout.OriginX).Append(',').Append(layout.OriginY).Append(',').Append(layout.Width).Append(',').Append(layout.Height).Append(';');
            foreach (var monitor in layout.Monitors)
            {
                hashBuilder.Append(monitor.Name).Append('|')
                           .Append(monitor.X).Append('|').Append(monitor.Y).Append('|')
                           .Append(monitor.Width).Append('|').Append(monitor.Height).Append('|')
                           .Append(monitor.Rotation).Append('|')
                           .Append(monitor.ScaleX.ToString(CultureInfo.InvariantCulture)).Append('|')
                           .Append(monitor.ScaleY.ToString(CultureInfo.InvariantCulture)).Append('|')
                           .Append(monitor.EdidHash).Append(';');
            }
            layout.Hash = HashString(hashBuilder.ToString());
            return layout;
        }

        public static double[] ComputeCtm(DesktopLayout layout, MonitorInfo monitor)
        {
            double desktopWidth = layout.Width <= 0 ? 1.0 : layout.Width;
            double desktopHeight = layout.Height <= 0 ? 1.0 : layout.Height;
            double offsetX = monitor.X - layout.OriginX;
            double offsetY = monitor.Y - layout.OriginY;
            double width = monitor.Width;
            double height = monitor.Height;
            if (monitor.ScaleX > 0) width *= monitor.ScaleX;
            if (monitor.ScaleY > 0) height *= monitor.ScaleY;

            double[] matrix = new double[9];
            string rotation = monitor.Rotation.ToLowerInvariant();
            if (rotation == "normal" || rotation.Length == 0)
            {
                matrix[0] = width / desktopWidth;
                matrix[1] = 0.0;
                matrix[2] = offsetX / desktopWidth;
                matrix[3] = 0.0;
                matrix[4] = height / desktopHeight;
                matrix[5] = offsetY / desktopHeight;
            }
            else if (rotation == "inverted")
            {
                matrix[0] = -width / desktopWidth;
                matrix[1] = 0.0;
                matrix[2] = (offsetX + width) / desktopWidth;
                matrix[3] = 0.0;
                matrix[4] = -height / desktopHeight;
                matrix[5] = (offsetY + height) / desktopHeight;
            }
            else if (rotation == "left")
            {
                matrix[0] = 0.0;
                matrix[1] = height / desktopWidth;
                matrix[2] = offsetX / desktopWidth;
                matrix[3] = -width / desktopHeight;
                matrix[4] = 0.0;
                matrix[5] = (offsetY + width) / desktopHeight;
            }
            else if (rotation == "right")
            {
                matrix[0] = 0.0;
                matrix[1] = -height / desktopWidth;
                matrix[2] = (offsetX + height) / desktopWidth;
                matrix[3] = width / desktopHeight;
                matrix[4] = 0.0;
                matrix[5] = offsetY / desktopHeight;
            }
            else
            {
                matrix[0] = width / desktopWidth;
                matrix[1] = 0.0;
                matrix[2] = offsetX / desktopWidth;
                matrix[3] = 0.0;
                matrix[4] = height / desktopHeight;
                matrix[5] = offsetY / desktopHeight;
            }
            matrix[6] = 0.0;
            matrix[7] = 0.0;
            matrix[8] = 1.0;
            return matrix;
        }

        public static void ApplyCtm(IEnumerable<int> deviceIds, double[] matrix)
        {
            var builder = new StringBuilder();
            builder.AppendFormat(CultureInfo.InvariantCulture, "{0} {1} {2} {3} {4} {5} {6} {7} {8}",
                matrix[0], matrix[1], matrix[2],
                matrix[3], matrix[4], matrix[5],
                matrix[6], matrix[7], matrix[8]);
            foreach (var id in deviceIds)
            {
                RunCommand($"xinput set-prop {id} \"Coordinate Transformation Matrix\" {builder}");
            }
        }

        public static MonitorInfo? FindByName(DesktopLayout layout, string name)
        {
            var lowered = name.ToLowerInvariant();
            return layout.Monitors.FirstOrDefault(m => m.Name.ToLowerInvariant() == lowered);
        }

        public static MonitorInfo? FindByIndex(DesktopLayout layout, int index)
        {
            return layout.Monitors.FirstOrDefault(m => m.Index == index) ??
                   (index >= 0 && index < layout.Monitors.Count ? layout.Monitors[index] : null);
        }

        public static string FormatMatrix(double[] matrix)
        {
            return string.Join(' ', matrix.Select(m => m.ToString("0.######", CultureInfo.InvariantCulture)));
        }

        private static string RunCommand(string command)
        {
            using var process = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = "bash",
                    Arguments = $"-c \"{command}\"",
                    RedirectStandardOutput = true,
                    UseShellExecute = false,
                    CreateNoWindow = true
                }
            };
            process.Start();
            var output = process.StandardOutput.ReadToEnd();
            process.WaitForExit();
            return output;
        }

        private static string HashString(string value)
        {
            const ulong offset = 1469598103934665603;
            const ulong prime = 1099511628211;
            ulong hash = offset;
            foreach (var ch in value)
            {
                hash ^= ch;
                hash *= prime;
            }
            return hash.ToString("x");
        }
    }
}
