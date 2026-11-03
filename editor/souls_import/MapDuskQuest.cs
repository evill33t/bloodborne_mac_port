// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class MapDuskQuest {
    internal const string Model="c8910";
    internal const int Entity=4046050,Npc=891000,GoldenEntity=4048200;
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,string SourceName,int Entity,int Npc,
        IReadOnlyList<MapDuskAnimations.Motion> Motions,MapDuskDialogue.Result Dialogue,MapDuskEvents.Result Events,IReadOnlyList<string> Limitations);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Dusk rescue: "+why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,
        MSB1 sourceMap,MSBB target,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        string definitions,string nativeRoot,string targetMap,byte[] script,int goldenSocket=601) {
        Need(targetMap=="m40_04_00_00"&&slots.Count==chunks.Count&&slots.Count>0,"destination/collision bindings");
        var from=sourceMap.Parts.Enemies.Single(p=>p.EntityID==6050);var golem=sourceMap.Parts.Enemies.Single(p=>p.EntityID==1200200);
        Need(from.ModelName=="c0000"&&from.CharaInitID==6050&&from.NPCParamID==6050&&from.TalkID==120605&&from.Scale==Vector3.One,"source rescue actor changed");
        Need(golem.ModelName=="c2711"&&golem.NPCParamID==271100&&golem.ThinkParamID==271100&&golem.Position==from.Position,"source golden golem identity changed");
        var golden=target.Parts.Enemies.SingleOrDefault(p=>p.EntityID==GoldenEntity);
        Need(golden is {ModelName:"c8832",NPCParamID:883200,ThinkParamID:883200},"a working owned source golden-golem actor is required before adding the rescue quest");
        var physical=BND4.Read(composed("chr/c8832.chrbnd.dcx")??throw new InvalidDataException("Golden-golem package unavailable"));
        Need(FLVER2.Read(physical.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)).Bytes).Dummies.Count(d=>d.ReferenceID==goldenSocket)==1,"owned source golem101 rescue socket is unavailable");
        Need(!target.Models.Enemies.Any(m=>m.Name==Model)&&!target.Parts.GetEntries().Any(p=>p.Name=="souls_dusk_rescue"||p.EntityID==Entity)&&
            !target.Regions.GetEntries().Any(p=>p.EntityID==Entity)&&!target.Events.GetEntries().Any(p=>p.EntityID==Entity),"owned Dusk map identity occupied");
        Need(composed("chr/"+Model+".chrbnd.dcx")==null&&composed("chr/"+Model+".anibnd.dcx")==null,"owned Dusk archive occupied");
        var appearance=MapDuskAppearance.Load(source,definitions);var files=new Dictionary<string,byte[]>();
        var skin=MapCharacters.Package(appearance.Model.Write(),appearance.Textures,"DS1R","c8030",Model,native("chr/c8030.chrbnd.dcx"),native("chr/c8030.anibnd.dcx"),native("action/script/c8030.hks"),files,preserveSourceDimensions:true);
        var motions=MapDuskAnimations.Apply(source,native,skin,files);
        byte[] Composed(string path)=>composed(path)??native(path);
        var game=BND4.Read(Composed("param/gameparam/gameparam.parambnd.dcx"));var npcFile=game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var rows=MapCharacters.NativeRows(npcFile.Bytes,false);var donor=rows.Rows.Single(r=>r.ID==803000);var data=MapCharacters.Payload(donor);
        BitConverter.GetBytes(-1).CopyTo(data,4);data[0x149]&=0xdf;foreach(int index in new[]{0x146,0x147,0x14a,0x14b})data[index]=255;
        MapCharacters.CloneRow(rows,donor,Npc,"Imported Dusk rescue6050",data);npcFile.Bytes=MapCharacters.WriteRows(rows);files.Add("param/gameparam/gameparam.parambnd.dcx",game.Write(game.Compression));
        var dialogue=MapDuskDialogue.Export(source,native,composed,nativeRoot,definitions,files);
        var events=MapDuskEvents.Apply(script,source("event/m12_00_00_00.emevd.dcx")??throw new FileNotFoundException("Source Dusk event script unavailable"),Entity,GoldenEntity,goldenSocket,motions);
        var nativeMap=MSBB.Read(native("map/MapStudio/m21_00_00_00.msb.dcx"));var template=nativeMap.Parts.Enemies.First(p=>p.ModelName=="c8030");
        var model=(MSBB.Model.Enemy)nativeMap.Models.Enemies.Single(m=>m.Name=="c8030").DeepCopy();model.Name=Model;model.SibPath=model.SibPath.Replace("c8030",Model,StringComparison.OrdinalIgnoreCase);
        var actor=(MSBB.Part.Enemy)template.DeepCopy();actor.Name="souls_dusk_rescue";actor.ModelName=Model;actor.EntityID=Entity;actor.NPCParamID=Npc;actor.ThinkParamID=-1;actor.CharaInitID=-1;actor.TalkID=MapDuskDialogue.Talk;
        actor.Position=from.Position+offset;actor.Rotation=from.Rotation;actor.Scale=from.Scale*skin.ProportionScale;
        actor.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,from.CollisionName,actor.Position)].Name;actor.InitAnimID=-1;actor.DamageAnimID=-1;actor.PatrolInfoName=null!;actor.PatrolType=0;actor.FallbackPlatoonID=0;Array.Clear(actor.MovePointNames);
        Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.DispGroups,0u);Array.Fill(actor.BackreadGroups,uint.MaxValue);
        // All resource and source-contract checks completed before map mutation.
        target.Models.Enemies.Add(model);target.Parts.Enemies.Add(actor);
        return new(files,events.Script,from.Name,Entity,Npc,motions,dialogue,events,[
            "Dusk6050 uses her original Antiquated set, real FaceGen6050, source neck and crown hair with source body customization; native friendly hitboxes, cloth and some face shaders remain approximations.",
            "Only the first rescue encounter is provided. Source spell teaching, later summon services, gesture, DLC and full hostility reactions are not yet exposed; later quest states must not be advertised as complete.",
            "Source named action aliases are resolved for7875/7876/7877/8289. Native source-pose, voice, interaction and save/reload behavior still require in-game validation."
        ]);
    }
}
