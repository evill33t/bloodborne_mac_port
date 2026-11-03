// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
internal static class CatacombsSpecialEffects {
    internal const int LightningFlight=650825,LightningHit=650835,WispExplosion=20200,WispGlow=610101;
    internal static readonly IReadOnlyDictionary<int,int> Projectiles=new Dictionary<int,int>{{12303,LightningFlight},{12304,LightningHit},{13509,WispExplosion}};
    internal static readonly IReadOnlyDictionary<int,int> Attached=new Dictionary<int,int>{{12305,LightningFlight},{12306,LightningHit},{13506,WispGlow},{13507,WispGlow}};
    internal sealed record Result(byte[] Bytes,IReadOnlyList<string> SharedVariants);
    internal static Result Compose(byte[] destination,Func<string,byte[]> native) {
        var lightning=NativeEffectClosure.Compose(destination,native("sfx/frpg_sfxbnd_m29c.ffxbnd.dcx"),[LightningFlight,LightningHit]);
        var glow=NativeEffectClosure.Compose(lightning.Bytes,native(CatacombsCreaturesEffects.NativePath),[WispGlow]);
        var explosion=NativeEffectClosure.Compose(glow.Bytes,native("sfx/frpg_sfxbnd_commoneffects.ffxbnd.dcx"),[WispExplosion]);
        return new(explosion.Bytes,lightning.SharedVariants.Concat(glow.SharedVariants).Concat(explosion.SharedVariants).Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.Ordinal).ToArray());
    }
}
