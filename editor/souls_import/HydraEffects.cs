// SPDX-License-Identifier: GPL-3.0-or-later
// Source FXR1 cannot be passed to BB's FXR3 loader. Native fluid visuals retain
// their native model/texture/animation dependencies and source ballistics.
internal static class HydraEffects {
    internal const string NativePath="sfx/frpg_sfxbnd_m32.ffxbnd.dcx";
    internal static readonly int[] Required=[625306,625307];
    internal static NativeEffectClosure.Result Compose(byte[] destination,byte[] native)=>NativeEffectClosure.Compose(destination,native,Required);
    internal static byte[] Merge(byte[] destination,byte[] native)=>Compose(destination,native).Bytes;
}
