// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class MapThorolundQuest {
    internal sealed record Pose(int Source,int Physical,string Archive,int Frames,float Duration,bool Approximation);
    internal sealed record Actor(string SourceName,int SourceEntity,int Entity,int Npc,string Model,int Talk,IReadOnlyList<NpcArmorPart> Appearance,NpcBodyShape.Report BodyShape,Pose Pose);
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,IReadOnlyList<Actor> Actors,IReadOnlyList<MapThorolundDialogue.Result> Dialogues,IReadOnlyList<string> Limitations);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Thorolund quest: "+why);}
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,
        MSB1 sourceMap,MSBB target,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        string definitions,string nativeRoot,string targetMap,byte[] script,int petrusEntity) {
        Need(targetMap=="m40_02_00_00"&&chunks.Count==slots.Count&&slots.Count>0,"map/collision contract");
        var existingPetrus=target.Parts.Enemies.Single(p=>p.EntityID==petrusEntity);Need(existingPetrus.ThinkParamID==-1&&existingPetrus.ModelName.StartsWith("c89",StringComparison.Ordinal),"expected existing friendly Petrus actor");
        var files=new Dictionary<string,byte[]>();byte[] Read(string path)=>files.TryGetValue(path,out var bytes)?bytes:composed(path)??native(path);
        var game=BND4.Read(Read("param/gameparam/gameparam.parambnd.dcx"));var npcFile=game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var rows=MapCharacters.NativeRows(npcFile.Bytes,false);var donor=rows.Rows.Single(r=>r.ID==803000);
        var nativeMap=MSBB.Read(native("map/MapStudio/m21_00_00_00.msb.dcx"));var template=nativeMap.Parts.Enemies.First(p=>p.ModelName=="c8030");var nativeModel=nativeMap.Models.Enemies.Single(m=>m.Name=="c8030");
        var actions=new SourcePlayerActions(source);var actors=new List<Actor>();
        int[] sources=[6070,6090,6100,6080],entities=[4026070,4026090,4026100,petrusEntity];
        for(int index=0;index<sources.Length;index++) {
            int id=sources[index],entity=entities[index],modelID=8916+index,param=modelID*100,talk=402981+index;string model="c"+modelID;
            var from=sourceMap.Parts.Enemies.Single(p=>p.EntityID==id);int sourceTalk=102000+id/10,pose=id==6070?7880:0;
            Need(from.ModelName=="c0000"&&from.CharaInitID==id&&from.NPCParamID==id&&from.ThinkParamID==id&&from.TalkID==sourceTalk&&from.InitAnimID==(id==6070?7880:-1)&&from.Scale==Vector3.One,"source actor identity changed: "+id);
            Need(target.Models.Enemies.All(m=>m.Name!=model)&&rows.Rows.All(r=>r.ID!=param),"owned character/parameter identity occupied");
            if(id!=6080)Need(target.Parts.GetEntries().All(p=>p.EntityID!=entity)&&target.Regions.GetEntries().All(p=>p.EntityID!=entity)&&target.Events.GetEntries().All(p=>p.EntityID!=entity),"owned party actor occupied");
            foreach(string path in new[]{"chr/"+model+".chrbnd.dcx","chr/"+model+".anibnd.dcx","action/script/"+model+".hks"})Need(composed(path)==null,"owned character resource occupied: "+path);
            var appearance=MapThorolundAppearance.Load(source,definitions,id);
            var skin=MapCharacters.Package(appearance.Model.Write(),appearance.Textures,"DS1R","c8030",model,native("chr/c8030.chrbnd.dcx"),native("chr/c8030.anibnd.dcx"),native("action/script/c8030.hks"),files,preserveSourceDimensions:true);
            var motion=actions.Resolve(pose);Need(motion.Clip.FrameCount<=256,"source pose exceeds single-block encoder");var bank=BND4.Read(files["chr/"+model+".anibnd.dcx"]);
            var encoded=NativeAnimation.Convert(motion.Clip,bank.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),skin.Bones,skin.ProportionScale,preserveSourceWorld:true);
            foreach(int slot in new[]{0,1000000,2000000})bank.Files.Single(f=>f.ID==1000000000+slot).Bytes=encoded;
            var taeFile=bank.Files.Single(f=>f.ID==3000000);var tae=TAE.Read(taeFile.Bytes);foreach(var a in tae.Animations){a.Events.Clear();a.EventGroups.Clear();}taeFile.Bytes=tae.Write();files["chr/"+model+".anibnd.dcx"]=bank.Write(bank.Compression);
            var payload=MapCharacters.Payload(donor);BitConverter.GetBytes(-1).CopyTo(payload,4);payload[0x149]&=0xdf;foreach(int at in new[]{0x146,0x147,0x14a,0x14b})payload[at]=255;MapCharacters.CloneRow(rows,donor,param,"Imported Thorolund Firelink"+id,payload);
            var modelEntry=(MSBB.Model.Enemy)nativeModel.DeepCopy();modelEntry.Name=model;modelEntry.SibPath=modelEntry.SibPath.Replace("c8030",model,StringComparison.OrdinalIgnoreCase);target.Models.Enemies.Add(modelEntry);
            var actor=id==6080?existingPetrus:(MSBB.Part.Enemy)template.DeepCopy();actor.Name="souls_thorolund_"+id;actor.ModelName=model;actor.EntityID=entity;actor.NPCParamID=param;actor.ThinkParamID=-1;actor.CharaInitID=-1;actor.TalkID=talk;
            actor.Position=from.Position+offset;actor.Rotation=from.Rotation;actor.Scale=from.Scale*skin.ProportionScale;actor.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,from.CollisionName,actor.Position)].Name;
            actor.InitAnimID=-1;actor.DamageAnimID=-1;actor.PatrolInfoName=null!;actor.PatrolType=0;actor.FallbackPlatoonID=0;Array.Clear(actor.MovePointNames);Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);Array.Clear(actor.DispGroups);if(id!=6080)target.Parts.Enemies.Add(actor);
            actors.Add(new(from.Name,id,entity,param,model,talk,appearance.Parts,appearance.BodyShape,new(pose,motion.MotionID,motion.Archive,motion.Clip.FrameCount,motion.Clip.Duration,pose==0)));
        }
        npcFile.Bytes=MapCharacters.WriteRows(rows);files["param/gameparam/gameparam.parambnd.dcx"]=game.Write(game.Compression);
        var dialogues=new List<MapThorolundDialogue.Result>();foreach(var actor in actors)dialogues.Add(MapThorolundDialogue.Export(source,native,composed,nativeRoot,definitions,files,actor.SourceEntity,actor.Model,actor.Talk,900099000+(actor.Npc/100-8916)*1000,249980000+(actor.Npc/100-8916)*100));
        var events=MapThorolundEvents.Apply(script,source("event/m10_02_00_00.emevd.dcx")??throw new FileNotFoundException("Source Firelink events unavailable"),petrusEntity);
        return new(files,events,actors,dialogues,[
            "Rhea, Vince and Nico are absent in fresh state and arrive only after Capra's source defeat11010902. Petrus's original party-farewell completion11020600 arms departure; the source event's first completion11020560 keeps them present until area reload. The party then leaves, Petrus is absent in1192 and returns in1193 on a subsequent reload. Pinwheel introduction11300005 is an alternative external departure dependency; this adapter never fabricates it.",
            "The three authored source placements, armor, FaceGen faces/neck and body customization are retained. Rhea uses her actual kneeling pose7880; Vince, Nico and Petrus use the source unarmed idle because their initial animation is equipment-driven (-1). Native friendly physics, omitted equipment, cloth, talk gestures, hostility and combat remain approximations/unported.",
            "Only original initial Firelink conversations, farewell and Petrus's initial return greeting/repeat are exposed. The existing bounded gift-free Petrus warm greeting remains three lines and writes no copper-coin reward flag. Miracle/covenant menus, local spell services, the global source visit counter, Tomb rescue, later Rhea locations, Petrus's betrayal and quest completion are not implemented. Per-visit source group6 talk-reset semantics are not yet reproduced; initial/mission/farewell and source persistent completions remain available.",
            "Party deaths persist and retain native corpses until reload. Existing Petrus availability remains owned by the shared Firelink adapter; no competing visibility loop or second Petrus actor is added. Fixture evidence is distinct from native gameplay validation."]);
    }
}
