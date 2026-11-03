// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

internal static class CatacombsCreaturesEffects {
    // Native c1010 priest's small fire magic. This is a native visual adapter;
    // DS1 FXR1 recipes cannot be loaded as native BB FXR3.
    internal const int FireFlight=610101,FireHit=610102;
    internal const string NativePath="sfx/frpg_sfxbnd_m29b.ffxbnd.dcx";
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    internal static NativeEffectClosure.Result Compose(byte[] destination,byte[] native)=>NativeEffectClosure.Compose(destination,native,[FireFlight,FireHit]);
    internal static byte[] Merge(byte[] destination,byte[] native)=>Compose(destination,native).Bytes;
}
