// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class MapHydra {
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,IReadOnlyList<MapEnemySpawns.Spawn> Spawns,
        IReadOnlyList<string> ConsumedNames,object Character,object Heads,HydraEvents.Result Events,IReadOnlyList<string> SharedEffectVariants);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Basin Hydra: "+why);}
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,
        MSB1 sourceMap,MSBB target,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        string definitions,string targetMap,byte[] scriptBytes,int rewardLot=-1) {
        Need(targetMap=="m40_04_00_00"&&slots.Count==chunks.Count&&slots.Count>0,"destination/collision bindings");
        var actor=sourceMap.Parts.Enemies.Single(p=>p.EntityID==1200010);
        Need(actor.ModelName=="c3530"&&actor.NPCParamID==353001&&actor.ThinkParamID==353001&&actor.CharaInitID==-1&&actor.Scale==Vector3.One,"source body placement changed");
        var helpers=Enumerable.Range(0,7).Select(i=>sourceMap.Parts.Enemies.Single(p=>p.EntityID==1200011+i)).ToArray();
        Need(helpers.All(p=>p.ModelName=="c3531"&&p.NPCParamID==353101&&p.ThinkParamID==353100&&p.Scale==Vector3.One),"source severed actors changed");
        byte[] Composed(string p)=>composed(p)??native(p);
        foreach(string model in new[]{HydraCharacters.Model,HydraHeads.Model})foreach(string path in new[]{"chr/"+model+".chrbnd.dcx","chr/"+model+".anibnd.dcx","action/script/"+model+".hks"})Need(composed(path)==null,"owned character path occupied: "+path);
        var body=HydraCharacters.Bake(source,native,Composed,definitions);var head=HydraHeads.Package(source,native);
        var files=body.Files.ToDictionary(p=>p.Key,p=>p.Value);foreach(var pair in head.Files)Need(files.TryAdd(pair.Key,pair.Value),"repeated character output");
        var donor=MSBB.Read(native("map/MapStudio/m24_00_00_00.msb.dcx"));var template=donor.Parts.Enemies.First(p=>p.ModelName=="c2730");
        var entities=target.Parts.GetEntries().Select(p=>p.EntityID).Concat(target.Events.GetEntries().Select(p=>p.EntityID)).Concat(target.Regions.GetEntries().Select(p=>p.EntityID)).ToHashSet();
        MSBB.Part.Enemy Place(MSB1.Part.Enemy from,string model,int id,int npc,int think,bool helper) {
            Need(entities.Add(id),"actor reservation occupied");
            Need(target.Models.Enemies.Count(m=>m.Name==model)<=(helper?1:0),"model reservation occupied");
            if(!target.Models.Enemies.Any(m=>m.Name==model)) {
                var m=(MSBB.Model.Enemy)donor.Models.Enemies.Single(m=>m.Name=="c2730").DeepCopy();m.Name=model;m.SibPath=m.SibPath.Replace("c2730",model,StringComparison.OrdinalIgnoreCase);target.Models.Enemies.Add(m);
            }
            var a=(MSBB.Part.Enemy)template.DeepCopy();a.Name="souls_hydra_"+(helper?"head_"+(id-HydraEvents.FirstHead):"body");a.ModelName=model;a.EntityID=id;
            a.NPCParamID=npc;a.ThinkParamID=think;a.CharaInitID=-1;a.TalkID=-1;a.InitAnimID=-1;a.DamageAnimID=-1;
            a.Position=from.Position+offset;a.Rotation=from.Rotation;a.Scale=from.Scale;a.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,actor.CollisionName,actor.Position+offset)].Name;
            a.PatrolInfoName=null!;a.PatrolType=0;a.FallbackPlatoonID=0;Array.Clear(a.MovePointNames);
            Array.Fill(a.DrawGroups,uint.MaxValue);Array.Fill(a.DispGroups,0u);Array.Fill(a.BackreadGroups,uint.MaxValue);target.Parts.Enemies.Add(a);return a;
        }
        var imported=Place(actor,HydraCharacters.Model,HydraEvents.Body,HydraParameters.Npc,HydraParameters.Think,false);
        for(int i=0;i<7;i++)Place(helpers[i],"c8831",HydraEvents.FirstHead+i,HydraHeads.Npc,HydraHeads.Think,true);
        string ai="script/"+targetMap+".luabnd.dcx";var aiBytes=HydraAi.Build(Composed(ai),source,native);files.Add(ai,aiBytes);
        foreach(var pair in MapNativeAi.Fallbacks(aiBytes,composed))files.Add(pair.Key,pair.Value);
        string effects="sfx/frpg_sfxbnd_m40.ffxbnd.dcx";var fluid=HydraEffects.Compose(Composed(effects),native(HydraEffects.NativePath));files.Add(effects,fluid.Bytes);
        var events=HydraEvents.Apply(scriptBytes,source("event/m12_00_00_00.emevd.dcx")??throw new FileNotFoundException("Source Hydra script unavailable"),HydraParameters.EffectIds,rewardLot);
        int[] masks=Enumerable.Range(0,7).ToArray();var spawn=new MapEnemySpawns.Spawn(actor.Name,actor.ModelName,actor.NPCParamID,actor.ThinkParamID,actor.EntityID,
            HydraCharacters.Model,HydraEvents.Body,HydraParameters.Npc,HydraParameters.Think,imported.Position,masks,body.Skin.DirectWeightShare,1,body.Skin.Bones,false,actor.InitAnimID,"c2730");
        return new(files,events.Script,[spawn],new[]{actor.Name}.Concat(helpers.Select(p=>p.Name)).ToArray(),new{body.PhysicsReport,body.PwvReport,body.AnimationReport,body.ParameterReport},head.Report,events,fluid.SharedVariants);
    }
}
