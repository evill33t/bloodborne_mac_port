// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

internal static class CatacombsCreaturesSource {
    internal sealed record Cohort(int Slot,MSB1.Part.Enemy Caster,IReadOnlyList<MSB1.Part.Enemy> Skeletons);
    internal sealed record Wake(int Slot,int Entity,float Radius);
    internal sealed record Plan(IReadOnlyList<Cohort> Cohorts,IReadOnlyList<Wake> Wakes,IReadOnlyList<MSB1.Part.Enemy> Ordinary,IReadOnlyList<MSB1.Part.Enemy> Excluded,PARAM Npcs,PARAM Thinks);
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs population: "+why);}
    internal static Plan Load(Func<string,byte[]?> read,MSB1 map,string definitions) {
        var script=EMEVD.Read(read("event/m13_00_00_00.emevd.dcx")??throw new FileNotFoundException("Source Catacombs events unavailable"));
        var constructor=script.Events.Single(e=>e.ID==0);
        static bool Call(EMEVD.Instruction i,int id)=>i.Bank==2000&&i.ID==0&&i.ArgData.Length>=8&&BitConverter.ToInt32(i.ArgData,4)==id;
        Need(constructor.Instructions.Count(i=>Call(i,11300800))==1,"missing source population constructor");
        var population=script.Events.Single(e=>e.ID==11300800);var actors=map.Parts.Enemies.Where(p=>p.EntityID>=0).ToDictionary(p=>p.EntityID);
        var links=population.Instructions.Where(i=>Call(i,11305100)).Select(i=> {
            Need(i.ArgData.Length==16,"invalid source resurrection link");return (Slot:BitConverter.ToInt32(i.ArgData),Caster:BitConverter.ToInt32(i.ArgData,8),Skeleton:BitConverter.ToInt32(i.ArgData,12));
        }).ToArray();
        Need(links.Length==49&&links.Select(p=>p.Skeleton).Distinct().Count()==49,"source linked skeleton population changed");
        var sourceDeath=population.Instructions.Where(i=>Call(i,11300801)).ToArray();Need(sourceDeath.Length==6,"source necromancer population changed");
        var cohorts=sourceDeath.Select(i=> {
            Need(i.ArgData.Length==12,"invalid source caster death arguments");int slot=BitConverter.ToInt32(i.ArgData),id=BitConverter.ToInt32(i.ArgData,8);
            Need(slot is >=0 and <6&&actors.TryGetValue(id,out _),"invalid caster identity");var caster=actors[id];
            Need(caster.ModelName=="c2650"&&caster.NPCParamID==265000&&caster.ThinkParamID==265000&&caster.CharaInitID==-1&&caster.TalkID<=0&&caster.InitAnimID==-1,"source caster contract changed");
            var skeletons=links.Where(l=>l.Caster==id).Select(l=>actors[l.Skeleton]).ToArray();
            Need(skeletons.Length==new[]{14,8,7,6,7,7}[slot]&&skeletons.All(p=>p.ModelName=="c2900"&&p.NPCParamID is 290020 or 290021 or 290022&&p.ThinkParamID is >=290000 and <=290006&&p.CharaInitID==-1&&p.TalkID<=0&&p.InitAnimID is -1 or 9001),"source skeleton cohort contract changed");
            return new Cohort(slot,caster,skeletons);
        }).OrderBy(c=>c.Slot).ToArray();Need(cohorts.Select(c=>c.Slot).SequenceEqual(Enumerable.Range(0,6)),"source caster slots changed");
        var wakes=population.Instructions.Where(i=>Call(i,11305070)).Select(i=> {
            Need(i.ArgData.Length==16,"invalid source awakening call");int slot=BitConverter.ToInt32(i.ArgData),entity=BitConverter.ToInt32(i.ArgData,8);float radius=BitConverter.ToSingle(i.ArgData,12);
            Need(links.Any(l=>l.Skeleton==entity)&&actors[entity].InitAnimID==9001&&float.IsFinite(radius)&&radius>0&&radius<=30,"invalid source awakening binding");return new Wake(slot,entity,radius);
        }).ToArray();Need(wakes.Length==17&&wakes.Select(w=>w.Entity).Distinct().Count()==17&&links.Select(l=>actors[l.Skeleton]).Count(p=>p.InitAnimID==9001)==17,"source awakening population changed");
        var selected=cohorts.SelectMany(c=>c.Skeletons.Append(c.Caster)).Select(p=>p.Name).ToHashSet(StringComparer.Ordinal);
        var giant=map.Parts.Enemies.Single(p=>p.EntityID==1300050);Need(giant.ModelName=="c2910"&&giant.NPCParamID==291002&&giant.ThinkParamID==291002&&giant.InitAnimID==-1,"source giant skeleton changed");selected.Add(giant.Name);
        var wheels=map.Parts.Enemies.Where(p=>p.ModelName=="c2930"&&p.NPCParamID==293000&&p.ThinkParamID==293000&&p.EntityID==-1&&p.InitAnimID==-1&&p.CharaInitID==-1&&p.TalkID<=0).ToArray();Need(wheels.Length==7,"source ordinary wheel skeleton population changed");foreach(var p in wheels)selected.Add(p.Name);
        var original=BND3.Read(read("param/GameParam/GameParam.parambnd.dcx")!);PARAM Table(string name){var p=PARAM.Read(original.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);p.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,name+".xml")));return p;}
        var npcs=Table("NpcParam");var thinks=Table("NpcThinkParam");
        foreach(var p in cohorts.SelectMany(c=>c.Skeletons))Need(Convert.ToInt32(npcs.Rows.Single(r=>r.ID==p.NPCParamID)["spEffectID5"].Value)==5451,"source skeleton immortality effect changed");
        return new(cohorts,wakes,map.Parts.Enemies.Where(p=>selected.Contains(p.Name)).ToArray(),map.Parts.Enemies.Where(p=>!selected.Contains(p.Name)).ToArray(),npcs,thinks);
    }
}
