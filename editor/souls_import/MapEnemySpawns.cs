// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

// An explicit ordinary-hollow adapter. Bosses, quest actors, phantoms and source
// event-controlled populations require separate adapters, never a blanket spawn.
internal static class MapEnemySpawns {
    internal record Spawn(string SourceName,string SourceModel,int SourceNPCParamID,int SourceThinkParamID,int SourceEntityID,
        string TargetModel,int EntityID,int NPCParamID,int ThinkParamID,Vector3 Position,int[] DisplayMasks,double DirectWeightShare,float ProportionScale,IReadOnlyList<BoneMatch> Bones,bool HasSourceTorch);
    internal record Omitted(string SourceName,string SourceModel,int SourceNPCParamID,int SourceInitAnimID,string Reason);
    internal record Result(IReadOnlyDictionary<string,byte[]> Files,IReadOnlyList<Spawn> Spawns,IReadOnlyList<Omitted> Skipped,IReadOnlyList<string> Limitations);
    static readonly HashSet<int> SupportedParams=[250000,250001,250050,254000,254001,254002,254010,254011,254012,
        255000,255001,255002,255010,255040,255041,255042,256000,256002];
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static byte[] WithoutDonorTorch(byte[] bytes) {
        var bank=BND4.Read(bytes);var removed=new List<long>();
        foreach(var file in bank.Files.Where(f=>TAE.Is(f.Bytes.ToArray()))) {
            var tae=TAE.Read(file.Bytes);
            if(tae.BigEndian||tae.ID!=202610)throw new InvalidDataException("Unsupported hollow donor animation layout");
            foreach(var animation in tae.Animations)animation.Events.RemoveAll(e=> {
                if(e.Type!=100)return false;
                var p=e.GetParameterBytes(false);
                if(p.Length<4||BitConverter.ToInt32(p,0)!=610250)return false;
                // Native c2610 torch flame, dummy 40, slot 23. Other weapon,
                // footstep, blood and combat events remain untouched.
                if(p.Length!=16||BitConverter.ToInt32(p,4)!=40||BitConverter.ToInt32(p,8)!=23||BitConverter.ToInt32(p,12)!=0)
                    throw new InvalidDataException("Unsupported hollow donor torch effect");
                removed.Add(animation.ID);return true;
            });
            file.Bytes=tae.Write();
        }
        if(!removed.Order().SequenceEqual(new long[]{1000000,1007000,1007001,1007014}))
            throw new InvalidDataException("Native hollow donor torch events changed");
        return bank.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
    internal static Result Bake(Func<string,byte[]?> sourceRead,MSB1 source,string sourceMap,string definitions,string nativeRoot,
        MSBB destination,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        byte[] gameParamBytes,string? nativeMap=null,Func<string,byte[]?>? readNative=null) {
        var selected=source.Parts.Enemies.Where(p=>SupportedParams.Contains(p.NPCParamID)&&p.ModelName=="c"+(p.NPCParamID/100)&&p.TalkID<=0&&p.CharaInitID<0&&p.InitAnimID==-1).ToArray();
        var names=selected.Select(p=>p.Name).ToHashSet(StringComparer.Ordinal);
        var skipped=source.Parts.Enemies.Where(p=>!names.Contains(p.Name)).Select(p=>new Omitted(p.Name,p.ModelName,p.NPCParamID,p.InitAnimID,SupportedParams.Contains(p.NPCParamID)&&p.InitAnimID!=-1?"Unsupported initial pose/ambush animation; not instantiated.":"No ordinary-hollow adapter for this actor; source conditions or interactions are not converted.")).ToArray();
        static bool Finite(Vector3 p)=>float.IsFinite(p.X)&&float.IsFinite(p.Y)&&float.IsFinite(p.Z);
        if(!Finite(offset)||chunks.Count!=slots.Count||slots.Count==0)throw new InvalidDataException("Invalid enemy collision/coordinate bindings");
        foreach(var p in selected)if(!Finite(p.Position)||!Finite(p.Rotation)||!Finite(p.Scale)||Math.Min(p.Scale.X,Math.Min(p.Scale.Y,p.Scale.Z))<=0||Math.Max(p.Scale.X,Math.Max(p.Scale.Y,p.Scale.Z))>10)throw new InvalidDataException("Invalid source enemy transform");
        var files=new Dictionary<string,byte[]>();var reports=new List<Spawn>();
        if(selected.Length==0)return new(files,reports,skipped,[]);
        var sourceParams=BND3.Read(sourceRead("param/GameParam/GameParam.parambnd.dcx")??throw new FileNotFoundException("Source NPC parameters unavailable"));
        var sourceNpcs=PARAM.Read(sourceParams.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);
        sourceNpcs.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,"NpcParam.xml")));
        var donorMap=MSBB.Read(File.ReadAllBytes(Path.Combine(nativeRoot,"map/MapStudio/m22_00_00_00.msb.dcx")));
        // c2630's reference arms have substantially different lengths. Use the
        // symmetric Hemwick humanoid rig for ordinary human-sized hollows.
        const string template="c2610";const int templateParam=261010;
        var donor=donorMap.Parts.Enemies.First(p=>p.ModelName==template&&p.NPCParamID==templateParam&&p.ThinkParamID==templateParam);
        byte[] Native(string path)=>readNative?.Invoke(path)??File.ReadAllBytes(Path.Combine(nativeRoot,path));
        var nativeCharacter=Native("chr/"+template+".chrbnd.dcx");var nativeAnimation=Native("chr/"+template+".anibnd.dcx");var nativeScript=Native("action/script/"+template+".hks");
        var noTorchAnimation=WithoutDonorTorch(nativeAnimation);
        var game=BND4.Read(gameParamBytes);var npcFile=game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var npcs=MapCharacters.NativeRows(npcFile.Bytes,false);
        var paramDonor=npcs.Rows.Single(r=>r.ID==templateParam);var used=npcs.Rows.Select(r=>r.ID).ToHashSet();
        int nextModel=9600,nextEntity=sourceMap=="m10_02_00_00"?2419800:2409000;
        var entities=destination.Parts.GetEntries().Select(p=>p.EntityID).Concat(destination.Events.GetEntries().Select(e=>e.EntityID)).Concat(destination.Regions.GetEntries().Select(r=>r.EntityID)).ToHashSet();
        foreach(var group in selected.GroupBy(p=>(p.ModelName,p.NPCParamID))) {
            while(used.Contains(nextModel*100)||destination.Models.Enemies.Any(m=>m.Name=="c"+nextModel)||File.Exists(Path.Combine(nativeRoot,"chr/c"+nextModel+".chrbnd.dcx")))nextModel++;
            if(nextModel>=9800)throw new InvalidDataException("Imported enemy namespace exhausted");
            int id=nextModel++,paramID=id*100;string target="c"+id;used.Add(paramID);
            var row=sourceNpcs.Rows.Single(r=>r.ID==group.Key.NPCParamID);
            var masks=Enumerable.Range(0,16).Where(i=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="modelDispMask"+i).Value)!=0).ToHashSet();
            var appearance=MapCharacters.Authored(sourceRead,group.Key.ModelName);
            // c2500 mask 1 is the authored torch loadout (NpcParam 250001).
            // Armoured c2540/c2550/c2560 variants carry other equipment.
            bool hasTorch=group.Key.ModelName=="c2500"&&masks.Contains(1);
            var bake=MapCharacters.Package(appearance.Model,appearance.Textures,"DS1R",template,target,nativeCharacter,hasTorch?nativeAnimation:noTorchAnimation,nativeScript,files,masks);
            var payload=MapCharacters.Payload(paramDonor);foreach(int i in new[]{0x146,0x147,0x14A,0x14B})payload[i]=255;
            MapCharacters.CloneRow(npcs,paramDonor,paramID,"Imported hollow "+group.Key,payload);
            var model=(MSBB.Model.Enemy)donorMap.Models.Enemies.Single(m=>m.Name==template).DeepCopy();model.Name=target;model.SibPath=model.SibPath.Replace(template,target,StringComparison.OrdinalIgnoreCase);destination.Models.Enemies.Add(model);
            foreach(var sourceActor in group) {
                while(entities.Contains(nextEntity))nextEntity++;int entity=nextEntity++;entities.Add(entity);
                int collision=Enumerable.Range(0,chunks.Count).FirstOrDefault(i=>chunks[i].SourceName==sourceActor.CollisionName,0);
                var actor=(MSBB.Part.Enemy)donor.DeepCopy();actor.Name=target+"_"+reports.Count.ToString("D4");actor.ModelName=target;
                actor.EntityID=entity;actor.NPCParamID=paramID;actor.ThinkParamID=templateParam;actor.TalkID=-1;actor.CharaInitID=-1;
                actor.Position=sourceActor.Position+offset;actor.Rotation=sourceActor.Rotation;actor.Scale=sourceActor.Scale*bake.ProportionScale;actor.CollisionName=slots[collision].Name;
                actor.InitAnimID=-1;actor.DamageAnimID=-1;actor.PatrolInfoName=null!;actor.PatrolType=0;actor.FallbackPlatoonID=0;Array.Clear(actor.MovePointNames);
                Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.DispGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);
                destination.Parts.Enemies.Add(actor);reports.Add(new(sourceActor.Name,sourceActor.ModelName,sourceActor.NPCParamID,sourceActor.ThinkParamID,sourceActor.EntityID,target,entity,paramID,templateParam,actor.Position,masks.Order().ToArray(),bake.DirectWeightShare,bake.ProportionScale,bake.Bones,hasTorch));
            }
        }
        npcFile.Bytes=MapCharacters.WriteRows(npcs);files.Add("param/gameparam/gameparam.parambnd.dcx",game.Write(DCX.Type.DCX_DFLT_10000_44_9));
        string aiPath="script/"+(nativeMap??(sourceMap=="m10_02_00_00"?"m24_01_00_00":"m24_00_00_00"))+".luabnd.dcx";
        files.Add(aiPath,MapNativeAi.Merge(Native(aiPath),Native("script/m22_00_00_00.luabnd.dcx")));
        return new(files,reports,skipped,["Source hollow appearance, display masks and placements use native Hemwick humanoid animation, attack/hitbox, AI and health. Segment fitting preserves joint continuity and compensates skin thickness for the actor's height scale. The item adapter replaces native donor loot where enabled. Source combat and weapon behavior are not reproduced.",
            "Native donor torch-flame events are retained only for the source c2500 torch loadout. Other donor animation events and native combat/status parameters remain unchanged. Source torch placement, weapon movesets and fire damage are not converted.",
            "Only explicitly supported ordinary hollow parameter rows are instantiated. Source quest actors, bosses, multiplayer phantoms and other unsupported actors remain disabled.",
            "Source navigation, patrols and event activation are not converted. Hanging/special initial-pose actors are skipped until their animation/activation adapter exists. Chasing across the imported map requires gameplay validation; native navigation is not source navigation."]);
    }
}
