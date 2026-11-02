// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

// Explicit ordinary and encounter adapters. Quest actors and unsupported
// event-controlled populations stay omitted until their behavior is adapted.
internal static class MapEnemySpawns {
    internal record Spawn(string SourceName,string SourceModel,int SourceNPCParamID,int SourceThinkParamID,int SourceEntityID,
        string TargetModel,int EntityID,int NPCParamID,int ThinkParamID,Vector3 Position,int[] DisplayMasks,double DirectWeightShare,float ProportionScale,IReadOnlyList<BoneMatch> Bones,bool HasSourceTorch,int SourceInitAnimID,string? NativeAdapter=null,SourceClothPose.Report? SourceClothPose=null,SourceEquipmentMounts.Report? SourceEquipmentMounts=null,HollowCombatCharacters.Report? HollowCombat=null);
    internal record Omitted(string SourceName,string SourceModel,int SourceNPCParamID,int SourceInitAnimID,string Reason);
    internal record Result(IReadOnlyDictionary<string,byte[]> Files,IReadOnlyList<Spawn> Spawns,IReadOnlyList<Omitted> Skipped,IReadOnlyList<string> Limitations,IReadOnlyList<HollowProjectiles.Binding>? Projectiles=null);
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
        NonHumanoidEnemies.Profile? Special(MSB1.Part.Enemy p)=>FoliageCombat.Select(p,sourceMap)?FoliageCombat.Profile:NonHumanoidEnemies.Select(p,sourceMap)??BossEnemyCharacters.Select(p,sourceMap);
        bool Encounter(MSB1.Part.Enemy p)=>BossEnemyCharacters.Select(p,sourceMap)!=null;
        var selected=source.Parts.Enemies.Where(p=>p.TalkID<=0&&p.CharaInitID<0&&
            (SupportedParams.Contains(p.NPCParamID)&&p.ModelName=="c"+(p.NPCParamID/100)&&p.InitAnimID==-1||CemeterySkeleton(p,sourceMap)||Special(p)!=null)).ToArray();
        var names=selected.Select(p=>p.Name).ToHashSet(StringComparer.Ordinal);
        var skipped=source.Parts.Enemies.Where(p=>!names.Contains(p.Name)).Select(p=>new Omitted(p.Name,p.ModelName,p.NPCParamID,p.InitAnimID,OmissionReason(p,sourceMap))).ToArray();
        static bool Finite(Vector3 p)=>float.IsFinite(p.X)&&float.IsFinite(p.Y)&&float.IsFinite(p.Z);
        if(!Finite(offset)||chunks.Count!=slots.Count||slots.Count==0)throw new InvalidDataException("Invalid enemy collision/coordinate bindings");
        foreach(var p in selected)if(!Finite(p.Position)||!Finite(p.Rotation)||!Finite(p.Scale)||Math.Min(p.Scale.X,Math.Min(p.Scale.Y,p.Scale.Z))<=0||Math.Max(p.Scale.X,Math.Max(p.Scale.Y,p.Scale.Z))>10)throw new InvalidDataException("Invalid source enemy transform");
        var files=new Dictionary<string,byte[]>();var reports=new List<Spawn>();var clothCache=new Dictionary<string,SourceClothPose.Result>(StringComparer.Ordinal);
        var projectileBindings=new List<HollowProjectiles.Binding>();
        if(selected.Length==0)return new(files,reports,skipped,[]);
        var sourceParams=BND3.Read(sourceRead("param/GameParam/GameParam.parambnd.dcx")??throw new FileNotFoundException("Source NPC parameters unavailable"));
        var sourceNpcs=PARAM.Read(sourceParams.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);
        sourceNpcs.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,"NpcParam.xml")));
        var donorMap=MSBB.Read(File.ReadAllBytes(Path.Combine(nativeRoot,"map/MapStudio/m22_00_00_00.msb.dcx")));
        // Cemetery skeletons keep their existing wrapper. Ordinary hollows
        // use separate role controllers and an independent authored visible rig.
        const string template="c2610";const int templateParam=261010;
        var donor=donorMap.Parts.Enemies.First(p=>p.ModelName==template&&p.NPCParamID==templateParam&&p.ThinkParamID==templateParam);
        byte[] Native(string path)=>readNative?.Invoke(path)??File.ReadAllBytes(Path.Combine(nativeRoot,path));
        var nativeCharacter=Native("chr/"+template+".chrbnd.dcx");var nativeAnimation=Native("chr/"+template+".anibnd.dcx");var nativeScript=Native("action/script/"+template+".hks");
        var noTorchAnimation=WithoutDonorTorch(nativeAnimation);
        var hollowMap=MSBB.Read(Native("map/MapStudio/"+HollowCombatProfiles.NativeMap+".msb.dcx"));
        var game=BND4.Read(gameParamBytes);var npcFile=game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var npcs=MapCharacters.NativeRows(npcFile.Bytes,false);
        var paramDonor=npcs.Rows.Single(r=>r.ID==templateParam);var used=npcs.Rows.Select(r=>r.ID).ToHashSet();
        int nextModel=9600,nextSkeletonModel=9700,nextSpecialModel=8800,nextEntity=ordinaryFirstEntity??(sourceMap=="m10_02_00_00"?2419800:2409000),nextSkeletonEntity=2419900;
        int firstEntity=nextEntity,nextSpecialEntity=checked(firstEntity+900);
        int firstRatEntity=sourceMap=="m10_02_00_00"?4029950:checked(firstEntity+950),nextRatEntity=firstRatEntity;
        var entities=destination.Parts.GetEntries().Select(p=>p.EntityID).Concat(destination.Events.GetEntries().Select(e=>e.EntityID)).Concat(destination.Regions.GetEntries().Select(r=>r.EntityID)).ToHashSet();
        foreach(var group in selected.GroupBy(p=>(p.ModelName,p.NPCParamID))) {
            bool skeleton=CemeterySkeleton(group.First(),sourceMap);var special=Special(group.First());bool encounter=Encounter(group.First()),rat=special==NonHumanoidEnemies.Rat,foliage=special==FoliageCombat.Profile;
            int encounterModel=group.Key.NPCParamID switch {225000=>8950,224000=>8951,334003=>8952,535000=>8953,535001=>8954,_=>0};
            int ratModel=sourceMap=="m10_02_00_00"?8820:8821;
            int foliageModel=group.Key.NPCParamID==233000?8825:8826;
            int next=foliage?foliageModel:rat?ratModel:encounter?encounterModel:special!=null?nextSpecialModel:skeleton?nextSkeletonModel:nextModel,limit=foliage?foliageModel+1:rat?ratModel+1:encounter?encounterModel+1:special!=null?8900:skeleton?9800:9700;
            while(next<limit&&(used.Contains(next*100)||destination.Models.Enemies.Any(m=>m.Name=="c"+next)||File.Exists(Path.Combine(nativeRoot,"chr/c"+next+".chrbnd.dcx"))||special!=null&&(Directory.EnumerateFiles(Path.Combine(nativeRoot,"chr"),"c"+next+".*").Any()||File.Exists(Path.Combine(nativeRoot,"action/script/c"+next+".hks"))||Directory.EnumerateFiles(Path.Combine(nativeRoot,"sound"),"sprj_c"+next+".*").Any()||Directory.EnumerateFiles(Path.Combine(nativeRoot,"sound"),"sprj_c"+next+"_*.fsb").Any())))next++;
            if(next>=limit)throw new InvalidDataException("Imported enemy namespace exhausted");
            int id=next,paramID=id*100;if(!encounter&&!rat&&special!=null)nextSpecialModel=next+1;else if(special==null&&skeleton)nextSkeletonModel=next+1;else if(special==null)nextModel=next+1;
            string target="c"+id;used.Add(paramID);
            var row=sourceNpcs.Rows.Single(r=>r.ID==group.Key.NPCParamID);
            var masks=Enumerable.Range(0,16).Where(i=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="modelDispMask"+i).Value)!=0).ToHashSet();
            var ordinary=!skeleton&&special==null;
            var profiles=new Dictionary<string,HollowCombatProfiles.Profile>(StringComparer.Ordinal);
            if(ordinary)foreach(var actor in group)profiles.Add(actor.Name,HollowCombatProfiles.Select(actor,masks)??
                throw new InvalidDataException("Source hollow has no explicit combat role: "+actor.Name));
            // A source crossbow cohort can mix mobile and fixed brains while
            // retaining one visible model and stable model/entity allocation.
            var profile=ordinary?profiles.Values.FirstOrDefault(p=>p.Role==HollowCombatProfiles.Kind.Crossbow)??profiles.Values.First():null;
            if(ordinary&&profiles.Values.Any(p=>p.SourceModel!=profile!.SourceModel||p.MotionBank!=profile.MotionBank||
                p.Role!=profile.Role&&!(p.Ranged&&profile.Ranged)))throw new InvalidDataException("Incompatible source hollow roles share a model cohort");
            var appearance=MapCharacters.Authored(sourceRead,group.Key.ModelName);SourceClothPose.Report? clothReport=null;SourceClothPose.Result? clothResult=null;
            if(special==null&&SourceClothPose.ReadPhysics(sourceRead,"DS1R",group.Key.ModelName) is {} clothPhysics) {
                if(!clothCache.TryGetValue(group.Key.ModelName,out var cloth))clothCache.Add(group.Key.ModelName,cloth=SourceClothPose.Bake(appearance.Model,clothPhysics));
                clothReport=cloth.Report;clothResult=cloth;
            }
            // c2500 mask 1 is the authored torch loadout (NpcParam 250001).
            // Armoured c2540/c2550/c2560 variants carry other equipment.
            bool hasTorch=group.Key.ModelName=="c2500"&&masks.Contains(1);
            var hollow=ordinary?HollowCombatCharacters.Package(sourceRead,Native,profile!,target,appearance.Model,appearance.Textures,masks,clothResult,files):null;
            var bake=hollow?.Skin??(foliage?FoliageCombat.Package(sourceRead,Native,target,masks,files):encounter?BossEnemyCharacters.Package(sourceRead,Native,special!,target,masks,files):special!=null?NonHumanoidEnemies.Package(sourceRead,Native,special,target,masks,files):
                MapCharacters.Package(appearance.Model,appearance.Textures,"DS1R",template,target,nativeCharacter,noTorchAnimation,nativeScript,files,masks,allowLargeHumanoid:skeleton&&group.Key.ModelName=="c2910"));
            var actorMap=ordinary?hollowMap:special!=null?MSBB.Read(Native("map/MapStudio/"+special.Map+".msb.dcx")):donorMap;
            var actorDonor=ordinary?actorMap.Parts.Enemies.First(p=>p.ModelName==HollowCombatProfiles.NativeModel):special!=null?actorMap.Parts.Enemies.First(p=>p.ModelName==special.Native&&(!foliage||p.NPCParamID==special.Npc&&p.ThinkParamID==special.Think)):donor;
            var rowDonor=special!=null?npcs.Rows.Single(r=>r.ID==special.Npc):paramDonor;
            string actorTemplate=ordinary?HollowCombatProfiles.NativeModel:special?.Native??template;int actorThink=foliage?SourceEnemyAi.FoliageGoal:special?.Think??templateParam;
            if(skeleton)MapSkeletonAnimations.Apply(sourceRead,group.Key.ModelName,target,bake,files);
            var roleParams=new Dictionary<int,int>();
            if(ordinary) {
                foreach(var role in profiles.Values.DistinctBy(p=>p.NativeNpc)) {
                    int owned=checked(paramID+roleParams.Count);
                    if(owned!=paramID&&!used.Add(owned))throw new InvalidDataException("Imported hollow role parameter reservation is occupied");
                    var nativeRow=npcs.Rows.Single(r=>r.ID==role.NativeNpc);
                    var payload=HollowCombatProfiles.Parameters(role,MapCharacters.Payload(nativeRow),row,definitions);
                    foreach(int i in new[]{0x146,0x147,0x14A,0x14B})payload[i]=255;
                    MapCharacters.CloneRow(npcs,nativeRow,owned,"Imported hollow "+group.Key+" / "+role.Role,payload);
                    roleParams.Add(role.NativeNpc,owned);
                }
                if(profile!.Ranged)projectileBindings.Add(new(roleParams.Values.ToArray(),id*10,paramID));
            } else {
                var payload=MapCharacters.Payload(rowDonor);if(encounter)payload=BossEnemyCharacters.Parameters(payload,row,definitions);else if(special!=null)payload=NonHumanoidEnemies.Capsule(payload,row,definitions);foreach(int i in new[]{0x146,0x147,0x14A,0x14B})payload[i]=255;
                MapCharacters.CloneRow(npcs,rowDonor,paramID,(rat?"Imported rat ":encounter?"Imported boss encounter actor ":special!=null?"Imported Parish enemy ":"Imported cemetery skeleton ")+group.Key,payload);
            }
            var model=(MSBB.Model.Enemy)actorMap.Models.Enemies.Single(m=>m.Name==actorTemplate).DeepCopy();model.Name=target;model.SibPath=model.SibPath.Replace(actorTemplate,target,StringComparison.OrdinalIgnoreCase);destination.Models.Enemies.Add(model);
            foreach(var sourceActor in group) {
                int bossOffset=sourceActor.EntityID switch {1010700=>800,1010750=>801,1010500=>802,1010501=>803,1010800=>804,1010801=>805,_=>-1};
                int entity=rat?nextRatEntity:encounter?checked(firstEntity+bossOffset):special!=null?nextSpecialEntity:skeleton?nextSkeletonEntity:nextEntity;if(encounter&&(bossOffset<0||entities.Contains(entity)))throw new InvalidDataException("Imported encounter actor reservation is occupied");
                if(!encounter)while(entities.Contains(entity))entity++;
                if(special==null&&!skeleton&&ordinaryFirstEntity is {} first&&entity>=first+800)throw new InvalidDataException("Imported ordinary enemy entity namespace exhausted");
                if(rat){if(entity>=firstRatEntity+10)throw new InvalidDataException("Imported rat entity namespace exhausted");nextRatEntity=entity+1;}
                else if(!encounter&&special!=null){if(entity>=firstEntity+950)throw new InvalidDataException("Imported Parish enemy entity namespace exhausted");nextSpecialEntity=entity+1;}else if(special==null&&skeleton){if(entity>=2420000)throw new InvalidDataException("Imported skeleton entity namespace exhausted");nextSkeletonEntity=entity+1;}else if(special==null)nextEntity=entity+1;
                entities.Add(entity);
                int collision=encounter||rat||foliage?MapCollisionBindings.ObjectSlot(chunks,sourceActor.CollisionName,sourceActor.Position+offset):Enumerable.Range(0,chunks.Count).FirstOrDefault(i=>chunks[i].SourceName==sourceActor.CollisionName,0);
                var actor=(MSBB.Part.Enemy)actorDonor.DeepCopy();actor.Name=target+"_"+reports.Count.ToString("D4");actor.ModelName=target;
                var actorRole=ordinary?profiles[sourceActor.Name]:null;
                actor.EntityID=entity;actor.NPCParamID=actorRole==null?paramID:roleParams[actorRole.NativeNpc];actor.ThinkParamID=actorRole?.NativeThink??actorThink;actor.TalkID=-1;actor.CharaInitID=-1;
                actor.Position=sourceActor.Position+offset;actor.Rotation=sourceActor.Rotation;actor.Scale=sourceActor.Scale*bake.ProportionScale;actor.CollisionName=slots[collision].Name;
                actor.InitAnimID=-1;actor.DamageAnimID=-1;actor.PatrolInfoName=null!;actor.PatrolType=0;actor.FallbackPlatoonID=0;Array.Clear(actor.MovePointNames);
                Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.DispGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);
                if(rat)Array.Copy(actorDonor.BackreadGroups,actor.BackreadGroups,actor.BackreadGroups.Length);
                destination.Parts.Enemies.Add(actor);reports.Add(new(sourceActor.Name,sourceActor.ModelName,sourceActor.NPCParamID,sourceActor.ThinkParamID,sourceActor.EntityID,target,entity,actor.NPCParamID,actor.ThinkParamID,actor.Position,masks.Order().ToArray(),bake.DirectWeightShare,bake.ProportionScale,bake.Bones,hasTorch,sourceActor.InitAnimID,special?.Native,clothReport,null,hollow?.Report));
            }
        }
        npcFile.Bytes=MapCharacters.WriteRows(npcs);
        var combatParameters=HollowProjectiles.Apply(game.Write(DCX.Type.DCX_DFLT_10000_44_9),projectileBindings);
        var foliageOwners=reports.Where(p=>p.SourceModel=="c2330"&&p.NativeAdapter=="c2050").Select(p=>(Model:int.Parse(p.TargetModel[1..]),SourceNpc:p.SourceNPCParamID)).Distinct().ToArray();
        if(foliageOwners.Length>0)combatParameters=FoliageCombat.Parameters(combatParameters,sourceRead,definitions,foliageOwners);
        files.Add("param/gameparam/gameparam.parambnd.dcx",HollowCombatProfiles.Viscerals(combatParameters,
            reports.Where(p=>p.HollowCombat!=null).Select(p=>int.Parse(p.TargetModel[1..])).Distinct().ToArray()));
        string aiPath="script/"+(nativeMap??(sourceMap=="m10_02_00_00"?"m24_01_00_00":"m24_00_00_00"))+".luabnd.dcx";
        var ai=MapNativeAi.Merge(Native(aiPath),Native("script/m22_00_00_00.luabnd.dcx"));
        var specialProfiles=selected.Select(Special).Where(p=>p!=null).Distinct().ToArray();
        bool hasHollows=reports.Any(p=>p.HollowCombat!=null);
        if(specialProfiles.Length>0||hasHollows)ai=MapNativeAi.Merge(ai,Native("script/aicommon.luabnd.dcx"),requiredGoals:[10000]);
        if(hasHollows)ai=MapNativeAi.Merge(ai,Native("script/"+HollowCombatProfiles.NativeAi+".luabnd.dcx"),2630);
        foreach(var profile in specialProfiles)
            ai=MapNativeAi.Merge(ai,Native("script/"+profile!.Ai+".luabnd.dcx"),int.Parse(profile.Native[1..]));
        if(foliageOwners.Length>0)ai=SourceEnemyAi.Foliage(ai,sourceRead,Native);
        files.Add(aiPath,ai);
        foreach(var (path,bytes) in MapNativeAi.Fallbacks(ai,path=>readNative?.Invoke(path)??
            (File.Exists(Path.Combine(nativeRoot,path))?File.ReadAllBytes(Path.Combine(nativeRoot,path)):null)))files.Add(path,bytes);
        return new(files,reports,skipped,[..BossEnemyCharacters.Limitations,..HollowCombatProfiles.Limitations,..(foliageOwners.Length>0?FoliageCombat.Limitations:Array.Empty<string>()),..(projectileBindings.Count>0?HollowProjectiles.Limitations:Array.Empty<string>()),"Ordinary hollow source bodies and equipment keep an independent animated rig. Explicit native weapon-role controllers and source hit/shot windows replace the shared torch-melee wrapper. Native physical attack anchors, locomotion, clip timing, defense and health remain approximations. The item adapter replaces native donor loot where enabled.",
            "Cemetery skeletons retain their prior native wrapper and filtered torch effects. Skeleton attack superarmor is aligned with source basic attacks. Other hollow roles no longer inherit the c2610 torch attack bank.",
            "Firelink's identified c2900/c2910 cemetery skeletons retain source appearance, equipment masks, placement and stature. Source 9001/9061 bone-pile awakening and living heavy-hit 2014/2020/2040 motion are retargeted onto native playback slots, with source wake groups and protection windows. Combat AI, attacks, hitboxes, health, death and effects remain native; necromancer resurrection, original movesets and giant attack reach are not converted. Other special-pose enemies remain disabled.",
            "The Parish armored boar and Titanite Demon retain their complete source skin, segment lengths and source animation on appended rig tracks; native body tracks, locomotion, attack timing, hitboxes, AI and HP remain in use. The cloned native character capsule uses the matching source hitHeight/hitRadius/hitYOffset fields. Extra native states reuse explicitly mapped source actions. Their source armor, lightning/projectiles, unique death persistence and original combat behavior are not converted. Native physical/attack anchors can differ from the visible source body and require gameplay validation.",
            "The Balder Knight cape uses an offline gravity pose calculated from its source capsule bodies, masses, inertia, constraints and contacts. This is a static appearance pose that follows the animated body; ongoing native cloth simulation, wind and cloth/body collisions are not reproduced.",
            "The four c1201 rats retain source geometry, stature, capsule and authored motion on native c1100 rat playback. Native combat decisions, timing, damage, poison, HP and physical/ragdoll anchors remain approximations; uncommon native states reuse explicit source actions. Rat model/entity reservations do not shift existing actors. Ungrouped source rats keep unfiltered display groups and native rat backread settings.",
            "Supported hollow weapons, hands and shields retain their authored animated source hierarchy and visibility channels. Ranged projectiles originate at the source muzzle frame. Native combat decisions and physical hitboxes remain approximations.",
            "Only explicit ordinary, cemetery skeleton, rat, Parish and Burg boss encounter parameter rows are instantiated. Source quest actors, other bosses, multiplayer phantoms and unsupported actors remain disabled.",
            "This enemy adapter does not convert source patrols or event activation. Consult the navigation report for static source navigation coverage. Hanging/special initial-pose actors are skipped until their animation/activation adapter exists. Chasing across the imported map requires gameplay validation."],projectileBindings);
    }
}
