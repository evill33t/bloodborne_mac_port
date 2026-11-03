// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class MapGoldenGolem {
    internal const int Entity=4048200;
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,MapEnemySpawns.Spawn Spawn,object Report,string SourceName);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Dusk golden golem: "+why);}
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,
        MSB1 sourceMap,MSBB target,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        string definitions,string targetMap,byte[] scriptBytes) {
        Need(targetMap=="m40_04_00_00"&&slots.Count==chunks.Count&&slots.Count>0,"destination/collision binding");
        var actor=sourceMap.Parts.Enemies.Single(p=>p.EntityID==1200200);
        Need(actor.ModelName=="c2711"&&actor.NPCParamID==271100&&actor.ThinkParamID==271100&&actor.CharaInitID==-1&&actor.Scale==Vector3.One,"actual golden placement changed");
        Need(!target.Parts.GetEntries().Any(p=>p.EntityID==Entity)&&!target.Events.GetEntries().Any(p=>p.EntityID==Entity)&&!target.Regions.GetEntries().Any(p=>p.EntityID==Entity),"actor reservation occupied");
        Need(!target.Models.Enemies.Any(m=>m.Name==GoldenGolemCharacters.Model),"model reservation occupied");
        byte[] Composed(string path)=>composed(path)??native(path);
        foreach(string path in new[]{"chr/c8832.chrbnd.dcx","chr/c8832.anibnd.dcx","action/script/c8832.hks"})Need(composed(path)==null,"owned character path occupied");
        var body=GoldenGolemCharacters.Bake(source,native,Composed,definitions);var files=body.Files.ToDictionary(p=>p.Key,p=>p.Value);
        var visual=FLVER2.Read(BND4.Read(files["chr/c8832.chrbnd.dcx"]).Files.Single(f=>f.ID==200).Bytes);
        Need(visual.Dummies.Count(d=>d.ReferenceID==601)==1,"Dusk requires unique source101→601 rescue socket");
        string ai="script/"+targetMap+".luabnd.dcx";var aiBytes=GoldenGolemAi.Build(Composed(ai),source,native);files.Add(ai,aiBytes);
        foreach(var pair in MapNativeAi.Fallbacks(aiBytes,composed))files.Add(pair.Key,pair.Value);
        // MapHydra normally composed this same complete bank first. Compose is
        // also safe when the golden package is independently fixture-exported.
        string effects="sfx/frpg_sfxbnd_m40.ffxbnd.dcx";var fluid=HydraEffects.Compose(Composed(effects),native(HydraEffects.NativePath));files.Add(effects,fluid.Bytes);
        var donor=MSBB.Read(native("map/MapStudio/m24_00_00_00.msb.dcx"));var template=donor.Parts.Enemies.First(p=>p.ModelName=="c2730");
        var model=(MSBB.Model.Enemy)donor.Models.Enemies.Single(m=>m.Name=="c2730").DeepCopy();model.Name=GoldenGolemCharacters.Model;model.SibPath=model.SibPath.Replace("c2730",model.Name,StringComparison.OrdinalIgnoreCase);
        var placed=(MSBB.Part.Enemy)template.DeepCopy();placed.Name="souls_dusk_golden_golem";placed.ModelName=model.Name;placed.EntityID=Entity;
        placed.NPCParamID=GoldenGolemParameters.Npc;placed.ThinkParamID=GoldenGolemParameters.Think;placed.CharaInitID=-1;placed.TalkID=-1;placed.InitAnimID=-1;placed.DamageAnimID=-1;
        placed.Position=actor.Position+offset;placed.Rotation=actor.Rotation;placed.Scale=actor.Scale;
        placed.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,actor.CollisionName,placed.Position)].Name;placed.PatrolInfoName=null!;placed.PatrolType=0;placed.FallbackPlatoonID=0;Array.Clear(placed.MovePointNames);
        Array.Fill(placed.DrawGroups,uint.MaxValue);Array.Fill(placed.DispGroups,0u);Array.Fill(placed.BackreadGroups,uint.MaxValue);
        target.Models.Enemies.Add(model);target.Parts.Enemies.Add(placed);
        var masks=Enumerable.Range(0,32).ToArray();var spawn=new MapEnemySpawns.Spawn(actor.Name,actor.ModelName,actor.NPCParamID,actor.ThinkParamID,actor.EntityID,
            model.Name,Entity,GoldenGolemParameters.Npc,GoldenGolemParameters.Think,placed.Position,masks,body.Skin.DirectWeightShare,1,body.Skin.Bones,false,actor.InitAnimID,"c2730");
        // MapDuskEvents12416020 is the sole owner of reload-gated appearance,
        // actual body-death11200800 and nonrespawn. It must run after this Apply.
        return new(files,scriptBytes,spawn,new{body.PhysicsReport,body.AnimationReport,body.ParameterReport,body.Limitations,fluid.SharedVariants,
            PresenceOwner="MapDuskEvents12416020",SourceHydraDead=11200801,SourceGoldenDead=11200800,RescueSocket=601},actor.Name);
    }
}
