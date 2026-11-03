// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
internal static class CatacombsSpecialParameters {
    internal static CatacombsCreaturesParameters.Result Build(byte[] game,Func<string,byte[]?> source,string definitions,CatacombsSpecialSource.Plan plan,IReadOnlyDictionary<int,CatacombsSpecialCharacters.Result> characters) {
        var owners=plan.Families.Select(f=>{var c=characters[f.Model];return new CatacombsCreaturesParameters.Owner(f.Model,f.Npc,[f.Think],c.Character,c.Profile.Npc,c.Profile.Think,f.Source=="c2794"?[5402]:null,true);}).ToArray();
        return CatacombsCreaturesParameters.Build(game,source,definitions,owners,CatacombsSpecialAi.Goals,884600,886910,false,CatacombsSpecialEffects.Projectiles);
    }
}
