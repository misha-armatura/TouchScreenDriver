using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;
using System.Diagnostics;
using System.Linq;

namespace TouchScreenLib
{
    public class Monitor
    {
        public string Name { get; set; } = string.Empty;
        public int Width { get; set; }
        public int Height { get; set; }
        public int X { get; set; }
        public int Y { get; set; }
        public bool IsPrimary { get; set; }
    }

    public static class MonitorHelper
    {
        public static List<Monitor> GetMonitors()
        {
            var monitors = new List<Monitor>();
            var output = RunCommand("xrandr --listmonitors");
            
            // Parse xrandr output
            var lines = output.Split('\n');
            foreach (var line in lines.Skip(1)) // Skip the first line (header)
            {
                if (line.Trim().Length > 0)
                {
                    var monitor = new Monitor();
                    var parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                    
                    // Get monitor name (last part)
                    monitor.Name = parts[parts.Length - 1];
                    monitor.IsPrimary = line.Contains("+*");
                    
                    // Get resolution and position from the geometry part (e.g., 1920/527x1080/296+3840+0)
                    var geom = parts[2];
                    var match = Regex.Match(geom, @"(\d+)/\d+x(\d+)/\d+\+(\d+)\+(\d+)");
                    if (match.Success)
                    {
                        monitor.Width = int.Parse(match.Groups[1].Value);
                        monitor.Height = int.Parse(match.Groups[2].Value);
                        monitor.X = int.Parse(match.Groups[3].Value);
                        monitor.Y = int.Parse(match.Groups[4].Value);
                        monitors.Add(monitor);
                    }
                }
            }
            
            return monitors;
        }

        public static Monitor? GetPrimaryMonitor()
        {
            return GetMonitors().FirstOrDefault(m => m.IsPrimary);
        }

        private static string RunCommand(string command)
        {
            var process = new Process
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
    }
}
