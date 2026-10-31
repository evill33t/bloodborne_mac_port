// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

// Explicit hollow, cemetery skeleton and Parish boar/Titanite adapters. Other
// bosses, quest actors and event-controlled populations require separate adapters.
internal static class MapEnemySpawns {
    internal record Spawn(string SourceName,string SourceModel,int SourceNPCParamID,int SourceThinkParamID,int SourceEntityID,
        string TargetModel,int EntityID,int NPCParamID,int ThinkParamID,Vector3 Position,int[] DisplayMasks,double DirectWeightShare,float ProportionScale,IReadOnlyList<BoneMatch> Bones,bool HasSourceTorch,int SourceInitAnimID,string? NativeAdapter=null,SourceClothPose.Report? SourceClothPose=null,SourceEquipmentMounts.Report? SourceEquipmentMounts=null);
    internal record Omitted(string SourceName,string SourceModel,int SourceNPCParamID,int SourceInitAnimID,string Reason);
    internal record Result(IReadOnlyDictionary<string,byte[]> Files,IReadOnlyList<Spawn> Spawns,IReadOnlyList<Omitted> Skipped,IReadOnlyList<string> Limitations);
    static readonly HashSet<int> SupportedParams=[250000,250001,250050,254000,254001,254002,254010,254011,254012,
        255000,255001,255002,255010,255040,255041,255042,256000,256002];
    static bool CemeterySkeleton(MSB1.Part.Enemy p,string map)=>map=="m10_02_00_00"&&p.InitAnimID==9001&&
        (p.ModelName,p.NPCParamID,p.ThinkParamID) is ("c2900",290000,290003) or ("c2900",290002,290004) or ("c2910",291000,291002);
    static string OmissionReason(MSB1.Part.Enemy p,string map) {
        if(map=="m10_01_00_00"&&p.NPCParamID is 250053 or 254013 or 254014 or 255043 or 255044 or 256004&&p.EntityID is >=1010900 and <=1010912)
            return "Conditional Gravelord black phantom: source event11015070 disables this actor before its multiplayer activation guards. Source phantom state is not imported.";
        if(map=="m10_01_00_00"&&p.Name=="c2500_0051"&&p.ModelName=="c2500"&&p.NPCParamID==250052&&p.ThinkParamID==250020)
            return "Source bow archer: its separate bow rig, ranged AI and projectile attacks require a ranged adapter; the native melee wrapper is not used.";
        return SupportedParams.Contains(p.NPCParamID)&&p.InitAnimID!=-1?"Unsupported initial pose/ambush animation; not instantiated.":"No enemy adapter for this actor; source conditions or interactions are not converted.";
    }
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
        byte[] gameParamBytes,string? nativeMap=null,Func<string,byte[]?>? readNative=null,int? ordinaryFirstEntity=null) {
        if(ordinaryFirstEntity is <10000 or >int.MaxValue-1000)throw new ArgumentOutOfRangeException(nameof(ordinaryFirstEntity));
        var selected=source.Parts.Enemies.Where(p=>p.TalkID<=0&&p.CharaInitID<0&&
            (SupportedParams.Contains(p.NPCParamID)&&p.ModelName=="c"+(p.NPCParamID/100)&&p.InitAnimID==-1||CemeterySkeleton(p,sourceMap)||NonHumanoidEnemies.Select(p,sourceMap)!=null)).ToArray();
        var names=selected.Select(p=>p.Name).ToHashSet(StringComparer.Ordinal);
        var skipped=source.Parts.Enemies.Where(p=>!names.Contains(p.Name)).Select(p=>new Omitted(p.Name,p.ModelName,p.NPCParamID,p.InitAnimID,OmissionReason(p,sourceMap))).ToArray();
        static bool Finite(Vector3 p)=>float.IsFinite(p.X)&&float.IsFinite(p.Y)&&float.IsFinite(p.Z);
        if(!Finite(offset)||chunks.Count!=slots.Count||slots.Count==0)throw new InvalidDataException("Invalid enemy collision/coordinate bindings");
        foreach(var p in selected)if(!Finite(p.Position)||!Finite(p.Rotation)||!Finite(p.Scale)||Math.Min(p.Scale.X,Math.Min(p.Scale.Y,p.Scale.Z))<=0||Math.Max(p.Scale.X,Math.Max(p.Scale.Y,p.Scale.Z))>10)throw new InvalidDataException("Invalid source enemy transform");
        var files=new Dictionary<string,byte[]>();var reports=new List<Spawn>();var clothCache=new Dictionary<string,SourceClothPose.Result>(StringComparer.Ordinal);
        var equipmentCache=new Dictionary<(string Model,string Masks),SourceEquipmentMounts.Result?>();
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
        int nextModel=9600,nextSkeletonModel=9700,nextSpecialModel=8800,nextEntity=ordinaryFirstEntity??(sourceMap=="m10_02_00_00"?2419800:2409000),nextSkeletonEntity=2419900;
        int firstEntity=nextEntity,nextSpecialEntity=checked(firstEntity+900);
        var entities=destination.Parts.GetEntries().Select(p=>p.EntityID).Concat(destination.Events.GetEntries().Select(e=>e.EntityID)).Concat(destination.Regions.GetEntries().Select(r=>r.EntityID)).ToHashSet();
        foreach(var group in selected.GroupBy(p=>(p.ModelName,p.NPCParamID))) {
            bool skeleton=CemeterySkeleton(group.First(),sourceMap);var special=NonHumanoidEnemies.Select(group.First(),sourceMap);
            int next=special!=null?nextSpecialModel:skeleton?nextSkeletonModel:nextModel,limit=special!=null?8900:skeleton?9800:9700;
            while(next<limit&&(used.Contains(next*100)||destination.Models.Enemies.Any(m=>m.Name=="c"+next)||File.Exists(Path.Combine(nativeRoot,"chr/c"+next+".chrbnd.dcx"))||special!=null&&(Directory.EnumerateFiles(Path.Combine(nativeRoot,"chr"),"c"+next+".*").Any()||File.Exists(Path.Combine(nativeRoot,"action/script/c"+next+".hks"))||Directory.EnumerateFiles(Path.Combine(nativeRoot,"sound"),"sprj_c"+next+".*").Any()||Directory.EnumerateFiles(Path.Combine(nativeRoot,"sound"),"sprj_c"+next+"_*.fsb").Any())))next++;
            if(next>=limit)throw new InvalidDataException("Imported enemy namespace exhausted");
            int id=next,paramID=id*100;if(special!=null)nextSpecialModel=next+1;else if(skeleton)nextSkeletonModel=next+1;else nextModel=next+1;
            string target="c"+id;used.Add(paramID);
            var row=sourceNpcs.Rows.Single(r=>r.ID==group.Key.NPCParamID);
            var masks=Enumerable.Range(0,16).Where(i=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="modelDispMask"+i).Value)!=0).ToHashSet();
            var appearance=MapCharacters.Authored(sourceRead,group.Key.ModelName);SourceClothPose.Report? clothReport=null;SourceClothPose.Result? clothResult=null;
            if(SourceClothPose.ReadPhysics(sourceRead,"DS1R",group.Key.ModelName) is {} clothPhysics) {
                if(!clothCache.TryGetValue(group.Key.ModelName,out var cloth))clothCache.Add(group.Key.ModelName,cloth=SourceClothPose.Bake(appearance.Model,clothPhysics));
                appearance.Model=cloth.Model;clothReport=cloth.Report;clothResult=cloth;
            }
            SourceEquipmentMounts.Result? equipment=null;
            if(!skeleton&&special==null&&group.Key.ModelName is "c2500" or "c2540" or "c2550" or "c2560") {
                var key=(group.Key.ModelName,string.Join(',',masks.Order()));
                if(!equipmentCache.TryGetValue(key,out equipment))equipmentCache.Add(key,equipment=SourceEquipmentMounts.Load(sourceRead,"DS1R",group.Key.ModelName,appearance.Model,masks));
            }
            // c2500 mask 1 is the authored torch loadout (NpcParam 250001).
            // Armoured c2540/c2550/c2560 variants carry other equipment.
            bool hasTorch=group.Key.ModelName=="c2500"&&masks.Contains(1);
            var bake=special!=null?NonHumanoidEnemies.Package(sourceRead,Native,special,target,masks,files):MapCharacters.Package(appearance.Model,appearance.Textures,"DS1R",template,target,nativeCharacter,hasTorch?nativeAnimation:noTorchAnimation,nativeScript,files,masks,allowLargeHumanoid:skeleton&&group.Key.ModelName=="c2910",preserveEnemyProportions:!skeleton,staticClothAnchors:clothResult?.Anchors,equipmentMounts:equipment?.Frames,equipmentGrip:equipment?.Grip);
            var actorMap=special!=null?MSBB.Read(Native("map/MapStudio/"+special.Map+".msb.dcx")):donorMap;
            var actorDonor=special!=null?actorMap.Parts.Enemies.First(p=>p.ModelName==special.Native):donor;
            var rowDonor=special!=null?npcs.Rows.Single(r=>r.ID==special.Npc):paramDonor;
            string actorTemplate=special?.Native??template;int actorThink=special?.Think??templateParam;
            if(skeleton)MapSkeletonAnimations.Apply(sourceRead,group.Key.ModelName,target,bake,files);
            var payload=MapCharacters.Payload(rowDonor);if(special!=null)payload=NonHumanoidEnemies.Capsule(payload,row,definitions);foreach(int i in new[]{0x146,0x147,0x14A,0x14B})payload[i]=255;
            MapCharacters.CloneRow(npcs,rowDonor,paramID,(special!=null?"Imported Parish enemy ":skeleton?"Imported cemetery skeleton ":"Imported hollow ")+group.Key,payload);
            var model=(MSBB.Model.Enemy)actorMap.Models.Enemies.Single(m=>m.Name==actorTemplate).DeepCopy();model.Name=target;model.SibPath=model.SibPath.Replace(actorTemplate,target,StringComparison.OrdinalIgnoreCase);destination.Models.Enemies.Add(model);
            foreach(var sourceActor in group) {
                int entity=special!=null?nextSpecialEntity:skeleton?nextSkeletonEntity:nextEntity;while(entities.Contains(entity))entity++;
                if(!skeleton&&ordinaryFirstEntity is {} first&&entity>=first+1000)throw new InvalidDataException("Imported ordinary enemy entity namespace exhausted");
                if(special!=null){if(entity>=firstEntity+1000)throw new InvalidDataException("Imported Parish enemy entity namespace exhausted");nextSpecialEntity=entity+1;}else if(skeleton){if(entity>=2420000)throw new InvalidDataException("Imported skeleton entity namespace exhausted");nextSkeletonEntity=entity+1;}else nextEntity=entity+1;
                entities.Add(entity);
                int collision=Enumerable.Range(0,chunks.Count).FirstOrDefault(i=>chunks[i].SourceName==sourceActor.CollisionName,0);
                var actor=(MSBB.Part.Enemy)actorDonor.DeepCopy();actor.Name=target+"_"+reports.Count.ToString("D4");actor.ModelName=target;
                actor.EntityID=entity;actor.NPCParamID=paramID;actor.ThinkParamID=actorThink;actor.TalkID=-1;actor.CharaInitID=-1;
                actor.Position=sourceActor.Position+offset;actor.Rotation=sourceActor.Rotation;actor.Scale=sourceActor.Scale*bake.ProportionScale;actor.CollisionName=slots[collision].Name;
                actor.InitAnimID=-1;actor.DamageAnimID=-1;actor.PatrolInfoName=null!;actor.PatrolType=0;actor.FallbackPlatoonID=0;Array.Clear(actor.MovePointNames);
                Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.DispGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);
                destination.Parts.Enemies.Add(actor);reports.Add(new(sourceActor.Name,sourceActor.ModelName,sourceActor.NPCParamID,sourceActor.ThinkParamID,sourceActor.EntityID,target,entity,paramID,actorThink,actor.Position,masks.Order().ToArray(),bake.DirectWeightShare,bake.ProportionScale,bake.Bones,hasTorch,sourceActor.InitAnimID,special?.Native,clothReport,equipment?.Report));
            }
        }
        npcFile.Bytes=MapCharacters.WriteRows(npcs);files.Add("param/gameparam/gameparam.parambnd.dcx",game.Write(DCX.Type.DCX_DFLT_10000_44_9));
        string aiPath="script/"+(nativeMap??(sourceMap=="m10_02_00_00"?"m24_01_00_00":"m24_00_00_00"))+".luabnd.dcx";
        var ai=MapNativeAi.Merge(Native(aiPath),Native("script/m22_00_00_00.luabnd.dcx"));
        foreach(var profile in selected.Select(p=>NonHumanoidEnemies.Select(p,sourceMap)).Where(p=>p!=null).Distinct())
            ai=MapNativeAi.Merge(ai,Native("script/"+profile!.Ai+".luabnd.dcx"),int.Parse(profile.Native[1..]));
        files.Add(aiPath,ai);
        return new(files,reports,skipped,["Ordinary hollows retain source segment lengths in both their bind rig and every native animation's positional channels, without relying on MSB enemy scale. Native animation rotations, timing, events, extracted motion, attack/hitbox, AI and health remain in use. Native combat reach and physics are approximations; source combat and weapon behavior are not reproduced. The item adapter replaces native donor loot where enabled.",
            "Native donor torch-flame events are retained only for the source c2500 torch loadout. Hollow donor animation events and native combat/status parameters remain unchanged. Skeleton attack superarmor is aligned with the source basic attacks. Source torch placement, weapon movesets and fire damage are not converted.",
            "Firelink's identified c2900/c2910 cemetery skeletons retain source appearance, equipment masks, placement and stature. Source 9001/9061 bone-pile awakening and living heavy-hit 2014/2020/2040 motion are retargeted onto native playback slots, with source wake groups and protection windows. Combat AI, attacks, hitboxes, health, death and effects remain native; necromancer resurrection, original movesets and giant attack reach are not converted. Other special-pose enemies remain disabled.",
            "The Parish armored boar and Titanite Demon retain their complete source skin, segment lengths and source animation on appended rig tracks; native body tracks, locomotion, attack timing, hitboxes, AI and HP remain in use. The cloned native character capsule uses the matching source hitHeight/hitRadius/hitYOffset fields. Extra native states reuse explicitly mapped source actions. Their source armor, lightning/projectiles, unique death persistence and original combat behavior are not converted. Native physical/attack anchors can differ from the visible source body and require gameplay validation.",
            "The Balder Knight cape uses an offline gravity pose calculated from its source capsule bodies, masses, inertia, constraints and contacts. This is a static appearance pose that follows the animated body; ongoing native cloth simulation, wind and cloth/body collisions are not reproduced.",
            "Supported hollow weapons and shields use their authored source idle attachment frames on the native anatomical hands and forearms. Other source animation-local equipment movement and visibility channels are not retargeted; combat poses and weapon behavior remain native approximations.",
            "Only explicitly supported ordinary hollow, cemetery skeleton and Parish boar/Titanite parameter rows are instantiated. Source quest actors, bosses, multiplayer phantoms and other unsupported actors remain disabled.",
            "This enemy adapter does not convert source patrols or event activation. Consult the navigation report for static source navigation coverage. Hanging/special initial-pose actors are skipped until their animation/activation adapter exists. Chasing across the imported map requires gameplay validation."]);
    }
}
