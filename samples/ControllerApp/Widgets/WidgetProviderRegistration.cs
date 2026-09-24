using Microsoft.Windows.Widgets.Providers;
using System.Runtime.InteropServices;
using WinRT;

namespace MatterControllerApp.Widgets;

internal sealed class WidgetProviderRegistration : IDisposable
{
    private const uint LocalServer = 0x4;
    private const uint MultipleUse = 0x1;
    private readonly uint cookie;

    private WidgetProviderRegistration(uint cookie)
    {
        this.cookie = cookie;
    }

    public static WidgetProviderRegistration Register()
    {
        Guid classId = typeof(MatterWidgetProvider).GUID;
        int result = CoRegisterClassObject(
            classId,
            new WidgetProviderFactory<MatterWidgetProvider>(),
            LocalServer,
            MultipleUse,
            out uint cookie);
        Marshal.ThrowExceptionForHR(result);
        return new WidgetProviderRegistration(cookie);
    }

    public void Dispose()
    {
        int result = CoRevokeClassObject(cookie);
        if (result < 0)
        {
            Marshal.ThrowExceptionForHR(result);
        }
    }

    [DllImport("ole32.dll")]
    private static extern int CoRegisterClassObject(
        [MarshalAs(UnmanagedType.LPStruct)] Guid classId,
        [MarshalAs(UnmanagedType.IUnknown)] object classFactory,
        uint classContext,
        uint flags,
        out uint register);

    [DllImport("ole32.dll")]
    private static extern int CoRevokeClassObject(uint register);

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("00000001-0000-0000-C000-000000000046")]
    private interface IClassFactory
    {
        [PreserveSig]
        int CreateInstance(nint outer, ref Guid interfaceId, out nint instance);

        [PreserveSig]
        int LockServer([MarshalAs(UnmanagedType.Bool)] bool lockServer);
    }

    private sealed class WidgetProviderFactory<T> : IClassFactory
        where T : IWidgetProvider, new()
    {
        public int CreateInstance(nint outer, ref Guid interfaceId, out nint instance)
        {
            instance = nint.Zero;
            if (outer != nint.Zero)
            {
                return unchecked((int)0x80040110);
            }
            if (interfaceId != typeof(T).GUID &&
                interfaceId != new Guid("00000000-0000-0000-C000-000000000046"))
            {
                return unchecked((int)0x80004002);
            }

            instance = MarshalInspectable<IWidgetProvider>.FromManaged(new T());
            return 0;
        }

        public int LockServer(bool lockServer) => 0;
    }
}
