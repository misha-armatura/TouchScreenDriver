using System;
using System.Threading;
using System.Threading.Tasks;
using TouchScreenLib;
using System.IO;
using System.Reflection;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text.RegularExpressions;

class TouchScreenExample
{
    private static int _screenWidth = 1920;
    private static int _screenHeight = 1080;
    private static int _deviceMaxX = 4096;
    private static int _deviceMaxY = 4096;
    private static int _monitorX = 0;
    private static int _monitorY = 0;

    static async Task Main(string[] args)
    {
        try
        {
            string devicePath = string.Empty;
            int deviceId = -1;
            bool calibrate = false;
            bool loadCalibration = false;
            int monitorIndex = -1;

            // Parse command line arguments
            for (int i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--help":
                    case "-h":
                        ShowHelp();
                        return;

                    case "--device":
                    case "-d":
                        if (i + 1 < args.Length)
                        {
                            devicePath = args[++i];
                        }
                        break;

                    case "--device-id":
                        if (i + 1 < args.Length && int.TryParse(args[++i], out int id))
                        {
                            deviceId = id;
                        }
                        break;

                    case "--calibrate":
                    case "-c":
                        calibrate = true;
                        break;

                    case "--load":
                    case "-l":
                        loadCalibration = true;
                        break;

                    case "--resolution":
                    case "-r":
                        if (i + 1 < args.Length)
                        {
                            var res = args[++i].Split('x');
                            if (res.Length == 2 && 
                                int.TryParse(res[0], out int w) && 
                                int.TryParse(res[1], out int h))
                            {
                                _screenWidth = w;
                                _screenHeight = h;
                            }
                        }
                        break;

                    case "--monitor":
                    case "-m":
                        if (i + 1 < args.Length && int.TryParse(args[++i], out int mon))
                        {
                            monitorIndex = mon;
                        }
                        break;
                }
            }

            // Create cancellation handling
            var exitEvent = new TaskCompletionSource<bool>();
            var cts = new CancellationTokenSource();
            bool isExiting = false;

            // Handle Ctrl+C
            Console.CancelKeyPress += (s, e) => {
                e.Cancel = true;
                if (!isExiting)
                {
                    isExiting = true;
                    exitEvent.TrySetResult(true);
                    Console.WriteLine("Shutting down...");
                }
            };

            // Create touch reader
            using var touchReader = new TouchReaderAsync();

            // Set up event handlers
            touchReader.TouchEvent += (s, e) =>
            {
                Console.WriteLine($"Touch event: X={e.X}, Y={e.Y}, IsDown={e.IsDown}");
            };

            touchReader.Error += (s, e) =>
            {
                Console.WriteLine($"Error: {e.Message}");
                if (!isExiting)
                {
                    isExiting = true;
                    exitEvent.TrySetResult(true);
                }
            };

            // Try to open device
            bool deviceOpened = false;
            if (!string.IsNullOrEmpty(devicePath))
            {
                deviceOpened = touchReader.Open(devicePath);
            }
            else if (deviceId >= 0)
            {
                // Get device path from xinput
                var deviceInfo = GetDeviceInfo(deviceId);
                if (!string.IsNullOrEmpty(deviceInfo.Path))
                {
                    devicePath = deviceInfo.Path;
                    _deviceMaxX = deviceInfo.MaxX;
                    _deviceMaxY = deviceInfo.MaxY;
                    deviceOpened = touchReader.Open(devicePath);
                }
            }

            if (!deviceOpened)
            {
                Console.WriteLine("Failed to open touch device.");
                return;
            }

            Console.WriteLine($"Touch screen found: {touchReader.SelectedDevice}");

            // Get monitor info
            var monitors = MonitorHelper.GetMonitors();
            if (monitorIndex >= 0 && monitorIndex < monitors.Count)
            {
                var monitor = monitors[monitorIndex];
                // Always use monitor's actual resolution
                _screenWidth = monitor.Width;
                _screenHeight = monitor.Height;
                _monitorX = monitor.X;
                _monitorY = monitor.Y;
                Console.WriteLine($"Using monitor {monitorIndex}: {_screenWidth}x{_screenHeight} at ({_monitorX},{_monitorY})");
            }
            else if (monitorIndex == -1)
            {
                // Full desktop mode - calculate total width and max height
                _screenWidth = monitors.Sum(m => m.Width);
                _screenHeight = monitors.Max(m => m.Height);
                _monitorX = 0;
                _monitorY = 0;
                Console.WriteLine($"Using full desktop: {_screenWidth}x{_screenHeight}");
            }

            // Handle calibration
            string calibrationFile = Path.Combine(Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location) ?? ".", 
                monitorIndex >= 0 ? $"touch_calibration_mon{monitorIndex}.ini" : "touch_calibration.ini");

            if (calibrate)
            {
                Console.WriteLine("\n===== CALIBRATION MODE =====\n");
                Console.WriteLine($"Screen resolution: {_screenWidth}x{_screenHeight}");
                Console.WriteLine("Please touch each corner when prompted.");

                if (touchReader.RunCalibration(_screenWidth, _screenHeight))
                {
                    Console.WriteLine("Calibration completed successfully!");
                    // Save calibration and exit
                    touchReader.SaveCalibration(calibrationFile);
                    return;
                }
                else
                {
                    Console.WriteLine("Calibration failed.");
                    return;
                }
            }
            else if (loadCalibration && File.Exists(calibrationFile))
            {
                Console.WriteLine($"Loading calibration from {calibrationFile}");
                if (touchReader.LoadCalibration(calibrationFile))
                {
                    Console.WriteLine("Calibration loaded successfully!");
                }
                else
                {
                    Console.WriteLine("Failed to load calibration, using default values.");
                    touchReader.SetCalibration(0, _deviceMaxX, 0, _deviceMaxY, _screenWidth, _screenHeight);
                }
            }
            else
            {
                Console.WriteLine("Using default calibration values.");
                touchReader.SetCalibration(0, _deviceMaxX, 0, _deviceMaxY, _screenWidth, _screenHeight);
            }

            // Start reading events
            touchReader.StartReading(cts.Token);

            // Wait for application exit
            try
            {
                await exitEvent.Task.WaitAsync(cts.Token);
            }
            catch (OperationCanceledException)
            {
                // Clean exit
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"Fatal error: {ex.Message}");
            Environment.Exit(1);
        }
    }

    private static void ShowHelp()
    {
        Console.WriteLine("Usage: TouchScreenExample [options]");
        Console.WriteLine("Options:");
        Console.WriteLine("  -h, --help            Display this help message");
        Console.WriteLine("  -c, --calibrate       Run calibration procedure");
        Console.WriteLine("  -l, --load            Load calibration from INI file");
        Console.WriteLine("  -d, --device <path>   Specify touch device path");
        Console.WriteLine("  --device-id <id>      Use specific xinput device ID");
        Console.WriteLine("  -r, --resolution <w>x<h>  Specify screen resolution");
        Console.WriteLine("  -m, --monitor <index> Select monitor (-1=full desktop, 0 to N-1 for specific monitor)");
    }

    private static DeviceInfo GetDeviceInfo(int deviceId)
    {
        var info = new DeviceInfo();
        
        // Get device path from xinput
        var deviceNodeOutput = RunCommand($"xinput list-props {deviceId} | grep 'Device Node'");
        var match = Regex.Match(deviceNodeOutput, "\"(/dev/input/event\\d+)\"");
        if (match.Success)
        {
            info.Path = match.Groups[1].Value;
            
            // Get Wacom tablet area if available
            var areaOutput = RunCommand($"xinput list-props {deviceId} | grep 'Wacom Tablet Area'");
            var areaMatch = Regex.Match(areaOutput, @"(\d+), (\d+), (\d+), (\d+)");
            if (areaMatch.Success)
            {
                info.MaxX = int.Parse(areaMatch.Groups[3].Value);
                info.MaxY = int.Parse(areaMatch.Groups[4].Value);
            }
        }
        
        return info;
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

    private class DeviceInfo
    {
        public string Path { get; set; } = string.Empty;
        public int MaxX { get; set; } = 4096;
        public int MaxY { get; set; } = 4096;
    }
}
