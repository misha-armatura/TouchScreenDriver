using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Diagnostics;

namespace TouchScreenLib
{
    public class TouchEventArgs : EventArgs
    {
        public int X { get; set; }
        public int Y { get; set; }
        public bool IsDown { get; set; }
    }

    internal static class TouchReaderNative
    {
        private const string LibraryName = "libtouch_reader.so";

        [DllImport(LibraryName)]
        public static extern IntPtr touch_reader_create();

        [DllImport(LibraryName)]
        public static extern void touch_reader_destroy(IntPtr handle);

        [DllImport(LibraryName)]
        public static extern int touch_reader_open(IntPtr handle, string device_path);

        [DllImport(LibraryName)]
        public static extern void touch_reader_close(IntPtr handle);

        [DllImport(LibraryName)]
        public static extern int touch_reader_get_next_event(IntPtr handle, out int event_type, out int touch_count, out int x, out int y, out int value);

        [DllImport(LibraryName)]
        public static extern void touch_reader_set_calibration(IntPtr handle, int min_x, int max_x, int min_y, int max_y, int screen_width, int screen_height);

        [DllImport(LibraryName)]
        public static extern void touch_reader_set_calibration_margin(IntPtr handle, double margin_percent);

        [DllImport(LibraryName)]
        public static extern void touch_reader_set_affine_calibration(IntPtr handle, double[] matrix, int screen_width, int screen_height);

        [DllImport(LibraryName)]
        public static extern int touch_reader_run_calibration(IntPtr handle, int screen_width, int screen_height);

        [DllImport(LibraryName)]
        public static extern int touch_reader_load_calibration(IntPtr handle, string filename);

        [DllImport(LibraryName)]
        public static extern int touch_reader_save_calibration(IntPtr handle, string filename);
    }

    public class TouchReader : IDisposable
    {
        private IntPtr _handle;
        private bool _disposed;
        private string _selectedDevice = string.Empty;

        public string SelectedDevice => _selectedDevice;

        public TouchReader()
        {
            _handle = TouchReaderNative.touch_reader_create();
        }

        public bool Open(string devicePath)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(TouchReader));
            if (TouchReaderNative.touch_reader_open(_handle, devicePath) == 0)
            {
                _selectedDevice = devicePath;
                return true;
            }
            return false;
        }

        public void Close()
        {
            if (_disposed) return;
            TouchReaderNative.touch_reader_close(_handle);
            _selectedDevice = string.Empty;
        }

        public bool ReadEvent(out int x, out int y, out bool isDown)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(TouchReader));
            int eventType, touchCount, value;
            int result = TouchReaderNative.touch_reader_get_next_event(_handle, out eventType, out touchCount, out x, out y, out value);
            isDown = eventType == 0; // TouchDown event
            return result > 0;
        }

        public void SetCalibration(int minX, int maxX, int minY, int maxY, int screenWidth, int screenHeight)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(TouchReader));
            
            Console.WriteLine($"Using screen resolution: {screenWidth}x{screenHeight}");
            Console.WriteLine($"Device coordinate range: {maxX}x{maxY}");
            
            // Apply calibration directly
            TouchReaderNative.touch_reader_set_calibration(_handle, minX, maxX, minY, maxY, screenWidth, screenHeight);
        }

        public void SetCalibrationMargin(double marginPercent)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(TouchReader));
            TouchReaderNative.touch_reader_set_calibration_margin(_handle, marginPercent);
        }

        public void SetAffineCalibration(double[] matrix, int screenWidth, int screenHeight)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(TouchReader));
            if (matrix.Length != 6) throw new ArgumentException("Affine matrix must contain 6 elements.");
            TouchReaderNative.touch_reader_set_affine_calibration(_handle, matrix, screenWidth, screenHeight);
        }

        public bool RunCalibration(int screenWidth, int screenHeight)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(TouchReader));
            return TouchReaderNative.touch_reader_run_calibration(_handle, screenWidth, screenHeight) == 0;
        }

        public bool LoadCalibration(string filename)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(TouchReader));
            return TouchReaderNative.touch_reader_load_calibration(_handle, filename) == 0;
        }

        public bool SaveCalibration(string filename)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(TouchReader));
            return TouchReaderNative.touch_reader_save_calibration(_handle, filename) == 0;
        }

        public void Dispose()
        {
            if (!_disposed)
            {
                Close();
                TouchReaderNative.touch_reader_destroy(_handle);
                _handle = IntPtr.Zero;
                _disposed = true;
            }
            GC.SuppressFinalize(this);
        }

        ~TouchReader()
        {
            Dispose();
        }
    }

    public class TouchReaderAsync : IDisposable
    {
        private readonly TouchReader _touchReader;
        private CancellationTokenSource? _cts;
        private Task? _readTask;
        private bool _disposed;

        public event EventHandler<TouchEventArgs>? TouchEvent;
        public event EventHandler<Exception>? Error;

        public string SelectedDevice => _touchReader.SelectedDevice;

        public TouchReaderAsync()
        {
            _touchReader = new TouchReader();
        }

        public bool Open(string devicePath)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(TouchReaderAsync));
            return _touchReader.Open(devicePath);
        }

        public void Close()
        {
            if (_disposed) return;
            StopReading();
            _touchReader.Close();
        }

        public void SetCalibration(int minX, int maxX, int minY, int maxY, int screenWidth, int screenHeight)
        {
            _touchReader.SetCalibration(minX, maxX, minY, maxY, screenWidth, screenHeight);
        }

        public void SetCalibrationMargin(double marginPercent)
        {
            _touchReader.SetCalibrationMargin(marginPercent);
        }

        public void SetAffineCalibration(double[] matrix, int screenWidth, int screenHeight)
        {
            _touchReader.SetAffineCalibration(matrix, screenWidth, screenHeight);
        }

        public bool RunCalibration(int screenWidth, int screenHeight)
        {
            return _touchReader.RunCalibration(screenWidth, screenHeight);
        }

        public bool LoadCalibration(string filename)
        {
            return _touchReader.LoadCalibration(filename);
        }

        public bool SaveCalibration(string filename)
        {
            return _touchReader.SaveCalibration(filename);
        }

        public void StartReading(CancellationToken cancellationToken = default)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(TouchReaderAsync));
            if (_readTask != null) return;

            _cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            _readTask = Task.Run(ReadLoop, _cts.Token);
        }

        public void StopReading()
        {
            _cts?.Cancel();
            _readTask?.Wait();
            _readTask = null;
            _cts?.Dispose();
            _cts = null;
        }

        private void ReadLoop()
        {
            try
            {
                while (!_cts?.Token.IsCancellationRequested ?? false)
                {
                    if (_touchReader.ReadEvent(out int x, out int y, out bool isDown))
                    {
                        TouchEvent?.Invoke(this, new TouchEventArgs { X = x, Y = y, IsDown = isDown });
                    }
                    else
                    {
                        Thread.Sleep(1); // Prevent busy waiting
                    }
                }
            }
            catch (Exception ex)
            {
                Error?.Invoke(this, ex);
            }
        }

        public void Dispose()
        {
            if (!_disposed)
            {
                StopReading();
                _touchReader.Dispose();
                _disposed = true;
            }
            GC.SuppressFinalize(this);
        }

        ~TouchReaderAsync()
        {
            Dispose();
        }
    }
}
