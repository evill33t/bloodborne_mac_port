// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

internal static class NativeEventReservations {
    internal static void Verify(string nativeRoot,long first,int count) {
        if(first<0||count is <=0 or >10000)throw new ArgumentOutOfRangeException(nameof(count));
        foreach(string file in Directory.EnumerateFiles(Path.Combine(nativeRoot,"event"),"*.emevd.dcx"))
            if(EMEVD.Read(File.ReadAllBytes(file)).Events.Any(e=>e.ID>=first&&e.ID<first+count))
                throw new InvalidDataException("Imported event reservation overlaps native events: "+Path.GetFileName(file));
    }
}
