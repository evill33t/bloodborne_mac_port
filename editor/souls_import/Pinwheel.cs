// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
internal static class Pinwheel {
    internal const int Body=4058800,LogicRegion=4058850,FogObject=4058890,FogSfx=4058891;
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,IReadOnlyList<MapEnemySpawns.Spawn> Spawns,IReadOnlyList<int> RewardSourceLots,object Report,IReadOnlyList<string> Limits,BossFogWalls.Binding Fog);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Pinwheel encounter: "+why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    internal static IReadOnlySet<string> ConsumedSourceObjects(string sourceMap,MSB1 map) {
        if(sourceMap!="m13_00_00_00")return new HashSet<string>();var fog=map.Parts.Objects.Single(p=>p.EntityID==1301990);Need(fog.ModelName=="o3954","source fog model changed");return new HashSet<string>(StringComparer.Ordinal){fog.Name};
    }
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,
        MSB1 from,MSBB target,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        string definitions,string targetMap,byte[] script,string nativeRoot) {
        Need(targetMap=="m40_05_00_00"&&chunks.Count==slots.Count&&slots.Count>0,"destination/collision contract");
        var commonBytes=source("event/common.emevd.dcx")??throw new FileNotFoundException("Source Pinwheel common reward event unavailable");
        Need(Convert.ToHexStringLower(SHA256.HashData(commonBytes))=="84758a6888d5ece12abed648b7390ff5427f9766ae66337c52df81d43347faa0","source common reward resource changed");
        var common=EMEVD.Read(commonBytes);
        Need(common.Events.Single(e=>e.ID==0).Instructions.Any(i=>i.Bank==2000&&i.ID==0&&i.Layer==null&&i.ArgData.AsSpan().SequenceEqual(PinwheelEvents.Args(7,970,6,2550,9000,9030))),"source Pinwheel common970 reward call changed");
        var actors=from.Parts.Enemies.Where(p=>p.ModelName=="c3320").OrderBy(p=>p.EntityID).ToArray();
        Need(actors.Length==15&&actors.Select(p=>p.EntityID).SequenceEqual(Enumerable.Range(1300800,15))&&actors.All(p=>p.NPCParamID==332000+(p.EntityID==1300800?0:1)&&p.ThinkParamID==p.NPCParamID&&p.Scale==Vector3.One),"source actor population/profile");
        Need(!target.Models.Enemies.Any(m=>m.Name==PinwheelCharacters.Model),"owned model occupied");
        var used=target.Parts.GetEntries().Select(p=>p.EntityID).Concat(target.Events.GetEntries().Select(p=>p.EntityID)).Concat(target.Regions.GetEntries().Select(p=>p.EntityID)).ToHashSet();
        Need(Enumerable.Range(Body,15).All(i=>!used.Contains(i)),"owned actors occupied");
        var files=new Dictionary<string,byte[]>();byte[] Read(string p)=>files.GetValueOrDefault(p)??composed(p)??native(p);
        foreach(string p in new[]{"chr/c8880.chrbnd.dcx","chr/c8880.anibnd.dcx","action/script/c8880.hks"})Need(composed(p)==null,"owned character path occupied");
        var character=PinwheelCharacters.Bake(source,native,Read,definitions);foreach(var p in character.Files)files.Add(p.Key,p.Value);
        foreach(var p in PinwheelParameters.Names(source,Read,nativeRoot))files.Add(p.Key,p.Value);
        var donor=MSBB.Read(native("map/MapStudio/m24_00_00_00.msb.dcx"));var template=donor.Parts.Enemies.First(p=>p.ModelName=="c2730");
        var model=(MSBB.Model.Enemy)donor.Models.Enemies.Single(m=>m.Name=="c2730").DeepCopy();model.Name=PinwheelCharacters.Model;model.SibPath=model.SibPath.Replace("c2730",model.Name,StringComparison.OrdinalIgnoreCase);target.Models.Enemies.Add(model);
        var spawns=new List<MapEnemySpawns.Spawn>();foreach(var actor in actors) {
            int index=actor.EntityID-1300800;var p=(MSBB.Part.Enemy)template.DeepCopy();p.Name="souls_pinwheel_"+index.ToString("D2");p.ModelName=model.Name;p.EntityID=Body+index;p.NPCParamID=888000+(index==0?0:1);p.ThinkParamID=p.NPCParamID;p.CharaInitID=-1;p.TalkID=-1;p.InitAnimID=-1;p.DamageAnimID=-1;
            p.Position=actor.Position+offset;p.Rotation=actor.Rotation;p.Scale=actor.Scale;p.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,actor.CollisionName,p.Position)].Name;
            p.PatrolInfoName=null!;p.PatrolType=0;p.FallbackPlatoonID=0;Array.Clear(p.MovePointNames);Array.Fill(p.DrawGroups,uint.MaxValue);Array.Fill(p.DispGroups,0u);Array.Fill(p.BackreadGroups,uint.MaxValue);target.Parts.Enemies.Add(p);
            spawns.Add(new(actor.Name,actor.ModelName,actor.NPCParamID,actor.ThinkParamID,actor.EntityID,model.Name,p.EntityID,p.NPCParamID,p.ThinkParamID,p.Position,Enumerable.Range(0,32).ToArray(),character.Skin.DirectWeightShare,1,character.Skin.Bones,false,actor.InitAnimID,"c2730 / complete source113-bone Pinwheel"));
        }
        var regions=new Dictionary<int,int>();var sourceRegions=from.Regions.GetEntries().Where(r=>r.EntityID==1302500||r.EntityID is >=1302600 and <=1302612||r.EntityID is >=1302996 and <=1302999).OrderBy(r=>r.EntityID).ToArray();Need(sourceRegions.Length==18,"source encounter regions");
        foreach(var r in sourceRegions){int id=r.EntityID==1302500?LogicRegion:r.EntityID is >=1302600 and <=1302612?4058851+r.EntityID-1302600:4058864+r.EntityID-1302996;Need(used.Add(id)&&r.Shape is MSB.Shape.Box,"region reservation/shape");var box=(MSB.Shape.Box)r.Shape;
            target.Regions.Regions.Add(new(){Name="souls_pinwheel_region_"+r.EntityID,EntityID=id,Position=r.Position+offset,Rotation=r.Rotation,Shape=new MSB.Shape.Box{Width=box.Width,Height=box.Height,Depth=box.Depth}});regions.Add(r.EntityID,id);}
        var fog=from.Parts.Objects.Single(p=>p.EntityID==1301990);Need(fog.ModelName=="o3954"&&fog.Scale==Vector3.One,"source fog identity");
        var fogBank=BND3.Read(source("obj/o3954.objbnd.dcx")??throw new FileNotFoundException("Source Pinwheel fog"));var fogPhysics=fogBank.Files.Single(f=>Leaf(f.Name)=="o3954.hkx").Bytes.ToArray();var tag=new SourceCollision.Tag(fogPhysics);var shape=tag.Items.Single(i=>i.Count==1&&tag.Name(i.Type)=="hkpBoxShape");var half=tag.Vector(tag.Nested(shape.Type,shape.At,"halfExtents").At);float radius=BitConverter.ToSingle(fogPhysics,tag.Nested(shape.Type,shape.At,"radius").At);
        Vector3 center=Vector3.Zero;var translated=tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type)=="hkpConvexTranslateShape").ToArray();Need(translated.Length<=1,"nested fog translation");if(translated.Length==1)center+=tag.Vector(tag.Nested(translated[0].Type,translated[0].At,"translation").At);
        var rigid=tag.Items.Single(i=>i.Count==1&&tag.Name(i.Type)=="hkpRigidBody");int transform=tag.Nested(rigid.Type,rigid.At,"motion","motionState","transform").At;
        Need(Vector3.Distance(tag.Vector(transform),Vector3.UnitX)<.00001f&&Vector3.Distance(tag.Vector(transform+16),Vector3.UnitY)<.00001f&&Vector3.Distance(tag.Vector(transform+32),Vector3.UnitZ)<.00001f&&!tag.Items.Any(i=>i.Count==1&&tag.Name(i.Type)=="hkpConvexTransformShape"),"rotated/nested source fog body needs a dedicated conversion");center+=tag.Vector(transform+48);
        var entry=sourceRegions.Single(r=>r.EntityID==1302998);bool reverse=Vector3.Transform(entry.Position-fog.Position,Matrix4x4.CreateRotationY(-fog.Rotation.Y*MathF.PI/180)).Z<0;
        byte[]? OptionalNative(string p){try{return native(p);}catch(FileNotFoundException){return null;}}
        var wall=BossFogWalls.Apply(OptionalNative,p=>files.GetValueOrDefault(p)??composed(p),target,[new(fog.Name,fog.Position+offset,fog.Rotation,fog.Scale,FogObject,FogSfx,slots[MapCollisionBindings.ObjectSlot(chunks,fog.CollisionName,fog.Position+offset)].Name,half,center,radius,reverse)],targetMap);
        foreach(var p in wall.Files)files[p.Key]=p.Value;
        string ai="script/"+targetMap+".luabnd.dcx";files[ai]=PinwheelAi.Build(Read(ai),source,native);foreach(var p in MapNativeAi.Fallbacks(files[ai],composed))files.TryAdd(p.Key,p.Value);
        string fx="sfx/frpg_sfxbnd_m40.ffxbnd.dcx";var effects=CatacombsCreaturesEffects.Compose(Read(fx),native(CatacombsCreaturesEffects.NativePath));files[fx]=effects.Bytes;
        var encounter=PinwheelEvents.Apply(script,source("event/m13_00_00_00.emevd.dcx")!,regions,wall.Bindings.Single());
        return new(files,encounter,spawns,[2550,9000,9030],new{character.PhysicsReport,character.AnimationReport,character.ParameterReport,Effects=effects.SharedVariants,Regions=regions},[
            "One source boss and fourteen stateful illusions use complete source poses, physics bodies, AI and attack rows. Source event requests drive paired summons, teleportation, helper disappearance and the <=30% phase.",
            "The source entry regions gate native fog traversal. Original cutscene130000, boss music1303800 and original fire/teleport FXR1 are unconverted; native fog and priest fire are appearance adapters. BB still supplies controller locomotion/action routing and shader/solver policy.",
            "Defeat, intro and reward completion persist in owned native storage. Common970 awards source quest lot2550 (Rite of Kindling) and delayed consumable lots9000/9030; the armor-only NPC mask lot33200000 is deliberately excluded. MapItems adds a 10% Blood Vial roll to the real boss; the fourteen reusable illusions receive no loot. Kindling is an inventory identity, without a native bonfire-kindling mechanic. Post-defeat ladder1301143 is supplied by MapCatacombsLadders. Native gameplay validation remains required."],wall.Bindings.Single());
    }
    internal static byte[] Rewards(byte[] script,IReadOnlyDictionary<int,int> nativeLots)=>PinwheelEvents.Rewards(script,nativeLots);
}
