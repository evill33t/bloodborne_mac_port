// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

internal static class CatacombsSpecialSource {
    internal sealed record Family(string Source,int Npc,int Think,int Model,int Goal,MSB1.Part.Enemy[] Actors);
    internal sealed record Death(int Slot,int SourceActor,int SourceFlag,int Lot);
    internal sealed record Plan(Family[] Families,Death[] Deaths,PARAM Npcs,PARAM Thinks,EMEVD Events);
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs specials source: "+why);}
    internal static Plan Load(Func<string,byte[]?> read,MSB1 map,string definitions) {
        var game=BND3.Read(read("param/GameParam/GameParam.parambnd.dcx")!);PARAM Table(string n){var p=PARAM.Read(game.Files.Single(f=>Leaf(f.Name)==n+".param").Bytes);p.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,n+".xml")));return p;}
        var npcs=Table("NpcParam");var thinks=Table("NpcThinkParam");var input=EMEVD.Read(read("event/m13_00_00_00.emevd.dcx")!);var ctor=input.Events.Single(e=>e.ID==0);
        var specifications=new[]{("c3501",350101,350101,8846,12),("c2300",230000,230000,8847,1),("c2794",279020,279002,8848,1),("c3300",330030,330000,8849,2)};
        var families=specifications.Select(s=>{var actors=map.Parts.Enemies.Where(p=>p.ModelName==s.Item1&&p.NPCParamID==s.Item2&&p.ThinkParamID==s.Item3&&p.CharaInitID<0&&p.TalkID<=0&&p.InitAnimID==-1).ToArray();Need(actors.Length==s.Item5,"ordinary source population "+s.Item1);var brain=thinks.Rows.Single(r=>r.ID==s.Item3);return new Family(s.Item1,s.Item2,s.Item3,s.Item4,Convert.ToInt32(brain["battleGoalID"].Value),actors);}).ToArray();
        var calls=ctor.Instructions.Where(i=>i.Bank==2000&&i.ID==0&&i.ArgData.Length==16&&BitConverter.ToInt32(i.ArgData,4)==11300850&&BitConverter.ToInt32(i.ArgData) is >=6 and <=9).ToArray();Need(calls.Length==4,"source unique special defeat population");
        var deaths=calls.Select(i=>{int slot=BitConverter.ToInt32(i.ArgData),actor=BitConverter.ToInt32(i.ArgData,8),lot=BitConverter.ToInt32(i.ArgData,12);Need(actor==new[]{1300500,1300501,1300300,1300400}[slot-6]&&lot==new[]{33003000,33003000,0,27902000}[slot-6],"source special defeat contract");return new Death(slot,actor,11300850+slot,lot);}).OrderBy(d=>d.Slot).ToArray();
        var population=input.Events.Single(e=>e.ID==11300800);var wisps=population.Instructions.Where(i=>i.Bank==2000&&i.ID==0&&i.ArgData.Length==12&&BitConverter.ToInt32(i.ArgData,4)==11305210).ToArray();Need(wisps.Length==12,"source wisp constructor contract "+wisps.Length);
        Need(map.Parts.DummyEnemies.Count(p=>p.ModelName=="c3501"&&p.NPCParamID==350101)==2,"source dummy wisp population changed");
        return new(families,deaths,npcs,thinks,input);
    }
}
