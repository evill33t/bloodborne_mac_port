// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

// First-visit cinematics and the persistent bridge occupant. This does not run
// DS1 Lua on the native VM or enable the unrelated donor's melee controller.
internal static class MapHellkite {
    internal const string Model="c8960";
    internal const int Npc=896000,BridgeEntity=4039970,FlybyEntity=4039971;
    internal const int FlybySeen=52100172,BridgeSeen=52100980,Defeated=52100981,FlybyRunning=52100982,BridgeReady=52100983;
    internal const int FirstEvent=12415780;
    internal static readonly int[] ReservedFlags=[BridgeSeen,Defeated,FlybyRunning,BridgeReady];
    internal sealed record CameraCue(float SourceStart,float Start,float End,int Rumble,int Dummy,float BlurStart,float BlurEnd);
    internal sealed record Clip(int Action,int SourceAction,float Duration,int Frames,int SourceFrames,string SHA256,Vector4 End) {
        public IReadOnlyList<CameraCue> Camera {get;init;}=[];
    }
    internal sealed record Report(int BridgeActor,int FlybyActor,int FlybyFlag,int BridgeFlag,int DefeatFlag,int FlybyTrigger,int BridgeTrigger,int BridgeExclusion,int Perch,
        IReadOnlyList<Clip> Clips,IReadOnlyDictionary<long,string> SourceEvents,IReadOnlyList<string> Limits);
    internal sealed record Result(byte[] Script,IReadOnlyDictionary<string,byte[]> Files,IReadOnlyList<MapEnemySpawns.Spawn> Spawns,Report Details);
    static readonly NonHumanoidEnemies.Profile Profile=new("c3430","c2730",273100,273100,"m24_00_00_00","m24_00_00_00",110,159);
    static string Leaf(string value)=>value.Replace('\\','/').Split('/').Last();
    static void Require(bool value,string reason)=>HellkiteSource.Require(value,reason);
    internal static void VerifyReservations(string root) {
        void Scan(ReadOnlySpan<byte> bytes,string owner) {
            for(int i=0;i+4<=bytes.Length;i++)Require(!ReservedFlags.Contains(BitConverter.ToInt32(bytes[i..])),"native flag reservation occupied: "+owner);
        }
        foreach(var f in BND4.Read(Path.Combine(root,"param/gameparam/gameparam.parambnd.dcx")).Files)Scan(f.Bytes.Span,f.Name);
        foreach(string folder in new[]{"event","script/talk"})foreach(string path in Directory.EnumerateFiles(Path.Combine(root,folder),"*.dcx",SearchOption.AllDirectories)) {
            var bytes=File.ReadAllBytes(path);
            if(BND4.Is(bytes))foreach(var f in BND4.Read(bytes).Files)Scan(f.Bytes.Span,path+":"+f.Name);
            else {
                var raw=DCX.Is(bytes)?DCX.Decompress(bytes).ToArray():bytes;Scan(raw,path);
                if(EMEVD.Is(raw))Require(!EMEVD.Read(raw).Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+3),"native Hellkite event reservation occupied");
            }
        }
    }
    static IReadOnlyList<Clip> Playback(Func<string,byte[]?> source,Dictionary<string,byte[]> files,HellkiteSource.Plan plan,byte[] nativeTemplate) {
        string path="chr/"+Model+".anibnd.dcx";var bank=BND4.Read(files[path]);
        var rig=NativeAnimation.Skeleton(bank.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),allowMirroredScale:true);
        Require(rig.Names.Length==242&&rig.Names.Skip(110).All(n=>n.StartsWith("DS1_",StringComparison.Ordinal)),"not the checked 110+132 bone rig");
        var original=BND3.Read(source("chr/c3430.anibnd.dcx")!);var sourceTae=TAE.Read(original.Files.Single(f=>Leaf(f.Name).EndsWith(".tae",StringComparison.OrdinalIgnoreCase)).Bytes);
        var taeFile=bank.Files.Single(f=>f.ID==3000000);var tae=TAE.Read(taeFile.Bytes);var reports=new List<Clip>();
        // This wrapper supplies a native animation graph, not native combat.
        // Drop its unrelated mesh-visibility, death, attack and effect events,
        // including fallback actions selected by the graph with AI disabled.
        foreach(var animation in tae.Animations){animation.Events.Clear();animation.EventGroups.Clear();}
        // c2730's HKS can select either animation bank for an event action.
        // Adapt both, including both TAE entries; otherwise the alternate bank
        // retains native root motion and native death-state side effects.
        foreach(int action in bank.Files.Where(f=>f.ID>=1000000000&&new[]{0,7005,7006,7012}.Contains((f.ID-1000000000)%1000000)).Select(f=>f.ID-1000000000).ToArray()) {
            int slot=action%1000000,from=slot==0?7006:slot;
            var clip=SourceAnimation.Load(source,"c3430",from);var motion=plan.Motions[from];
            var file=bank.Files.Single(f=>f.ID==1000000000+action);var pack=new NativeAnimation.Pack(file.Bytes.ToArray());
            int count=pack.Int(pack.Object("hkaSplineCompressedAnimation")+56);
            var roots=from==7006?Enumerable.Repeat(Vector4.Zero,motion.Samples.Length).ToArray():motion.Samples;
            // Keep the retail container, native physical channels and appended
            // source pose splines. Only timing and extracted movement change.
            file.Bytes=BossEntranceMotion.RetimeFlight(file.Bytes.ToArray(),nativeTemplate,clip.Duration,roots,242);
            var target=tae.Animations.Single(a=>a.ID==action);var sourceEvents=sourceTae.Animations.Single(a=>a.ID==from).Events;var flags=sourceEvents.Where(e=>e.Type==0).ToArray();
            // The donor marks these event actions as one-shots; the perch is
            // a loop in both the event instruction and the action metadata.
            Require(target.MiniHeader is TAE.Animation.AnimMiniHeader.Standard,"unsupported native action header");
            ((TAE.Animation.AnimMiniHeader.Standard)target.MiniHeader).IsLoopByDefault=from==7006;
            target.Events.Clear();target.EventGroups.Clear();var group=new TAE.EventGroup(0);target.EventGroups.Add(group);
            foreach(var flag in flags) {
                var data=flag.GetParameterBytes(false);Require(data.Length==12&&BitConverter.ToInt32(data,4)==0&&BitConverter.ToInt32(data,8)==-1,"source flight movement-state layout changed");
                // Only movement states with matching verified native semantics;
                // source attack, sound, SFX and message events need adapters.
                int id=BitConverter.ToInt32(data);Require(id is 7 or 8 or 19 or 27 or 39 or 82,"unknown source flight movement state");
                if(id==82)continue;
                target.Events.Add(new TAE.Event(Math.Min(flag.StartTime,clip.Duration),Math.Min(flag.EndTime,clip.Duration),0,0,[..data,0,0,0,0],false){Group=group});
            }
            var cameras=new List<CameraCue>();var cameraGroup=new TAE.EventGroup(144);var blurGroup=new TAE.EventGroup(195);
            foreach(var camera in sourceEvents.Where(e=>e.Type==144)) {
                var data=camera.GetParameterBytes(false);Require(data.Length==12&&BitConverter.ToInt16(data)==101&&BitConverter.ToUInt16(data,2) is 3 or 4 or 7,"source camera cue changed");
                // Source TAE tails can outlive the sampled motion. Keep the
                // final landing impulse on the last held pose before perch.
                float start=Math.Min(camera.StartTime,clip.Duration-.1f),end=Math.Min(camera.EndTime,clip.Duration);
                Require(start>=0&&end>start,"invalid source camera window");
                float blurStart=Math.Max(0,start-.1f),blurEnd=Math.Min(clip.Duration,end+.35f);
                target.Events.Add(new(start,end,144,0,[..data,0,0,0,0],false){Group=cameraGroup});
                // Native feedback blur supplies a short cinematic emphasis;
                // source camera timing/falloff supplies the actual shake.
                target.Events.Add(new(blurStart,blurEnd,195,0,new byte[16],false){Group=blurGroup});
                cameras.Add(new(camera.StartTime,start,end,101,BitConverter.ToUInt16(data,2),blurStart,blurEnd));
            }
            if(cameras.Count>0){target.EventGroups.Add(cameraGroup);target.EventGroups.Add(blurGroup);}
            reports.Add(new(action,from,clip.Duration,count,clip.FrameCount,motion.SHA256,roots[^1]){Camera=cameras});
        }
        taeFile.Bytes=tae.Write();files[path]=bank.Write(DCX.Type.DCX_DFLT_10000_44_9);return reports;
    }
    static void Materials(Func<string,byte[]?> read,SkinBake skin) {
        var source=FLVER2.Read(MapCharacters.Authored(read,"c3430").Model);var definitions=new Dictionary<string,MTD>(StringComparer.OrdinalIgnoreCase);
        foreach(string path in new[]{"mtd/Mtd.mtdbnd.dcx","mtd/MtdPatch.mtdbnd.dcx"})foreach(var f in BND3.Read(read(path)??throw new FileNotFoundException("Hellkite material source missing",path)).Files)
            if(source.Materials.Any(m=>Leaf(m.MTD).Equals(Leaf(f.Name),StringComparison.OrdinalIgnoreCase)))definitions[Leaf(f.Name)]=MTD.Read(f.Bytes);
        Require(source.Meshes.Count==skin.Model.Materials.Count,"unexpected Hellkite display-mask/material selection");
        for(int i=0;i<source.Meshes.Count;i++) {
            var material=source.Materials[source.Meshes[i].MaterialIndex];var definition=definitions[Leaf(material.MTD)];
            int blend=Convert.ToInt32(definition.Params.Single(p=>p.Name=="g_BlendMode").Value);Require(blend is 0 or 1,"unsupported source dragon blend mode");
            if(blend==1){var target=skin.Model.Materials[i];if(!target.MTD.EndsWith("_e.mtd",StringComparison.Ordinal))target.MTD=target.MTD[..^4]+"_e.mtd";}
        }
    }
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]?> read,MSBB destination,Vector3 offset,
        IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,byte[] parameters,byte[] events,string nativeRoot,string definitions) {
        var plan=HellkiteSource.Load(source);VerifyReservations(nativeRoot);
        Require(MapQuestState.Flag(11010790)==FlybySeen,"existing flyby quest identity changed");
        Require(!destination.Models.Enemies.Any(m=>m.Name==Model)&&!destination.Parts.Enemies.Any(a=>a.EntityID is BridgeEntity or FlybyEntity),"actor reservation occupied");
        foreach(string folder in new[]{"chr","action/script","sound"})Require(!Directory.EnumerateFiles(Path.Combine(nativeRoot,folder),"*"+Model+"*").Any(),"native model namespace occupied");
        byte[] Native(string path)=>read(path)??throw new FileNotFoundException("Native Hellkite dependency unavailable",path);
        var game=BND4.Read(parameters);var npcFile=game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var npcs=MapCharacters.NativeRows(npcFile.Bytes,false);Require(!npcs.Rows.Any(r=>r.ID==Npc),"NPC reservation occupied");
        var sourceNpcs=PARAM.Read(BND3.Read(source("param/GameParam/GameParam.parambnd.dcx")!).Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);sourceNpcs.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,"NpcParam.xml")));
        var row=sourceNpcs.Rows.Single(r=>r.ID==343000);var masks=Enumerable.Range(0,16).Where(i=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="modelDispMask"+i).Value)!=0).ToHashSet();
        var files=new Dictionary<string,byte[]>();
        var sourceRumble=BND3.Read(source("other/default.rumblebnd")??throw new FileNotFoundException("Source camera bank missing"));
        var nativeRumble=BND4.Read(Native("other/default.rumblebnd.dcx"));
        Require(sourceRumble.Files.Single(f=>f.ID==101).Bytes.Span.SequenceEqual(nativeRumble.Files.Single(f=>f.ID==101).Bytes.Span),"source/native camera101 curves differ");
        var skin=NonHumanoidEnemies.Package(source,Native,Profile,Model,masks,files,action=>(action%1000000) switch{7005=>7005,7012=>7012,2200=>2200,2250=>2250,_=>7006},bake=>Materials(source,bake),
            (bake,rig)=>{
                var original=MapCharacters.Authored(source,"c3430").Model;var geometry=FLVER2.Read(original);
                foreach(short id in new short[]{3,4,7}) {
                    var socket=geometry.Dummies.Single(d=>d.ReferenceID==id);
                    bake.Model.Dummies.RemoveAll(d=>d.ReferenceID==id);
                    bake.Model.Dummies.Add(HollowSourceSkin.RebindSocket(socket,original,rig,id));
                }
            });
        var nativeAnimation=BND4.Read(Native("chr/c2730.anibnd.dcx"));
        var clips=Playback(source,files,plan,nativeAnimation.Files.Single(f=>f.ID==1000007012).Bytes.ToArray());
        var donor=npcs.Rows.Single(r=>r.ID==Profile.Npc);var payload=NonHumanoidEnemies.Capsule(MapCharacters.Payload(donor),row,definitions);
        BitConverter.GetBytes(Convert.ToUInt32(row.Cells.Single(c=>c.Def.InternalName=="hp").Value)).CopyTo(payload,32);
        BitConverter.GetBytes(Convert.ToUInt32(row.Cells.Single(c=>c.Def.InternalName=="getSoul").Value)).CopyTo(payload,40);
        foreach(int i in new[]{0x146,0x147,0x14A,0x14B})payload[i]=255;
        // No donor-specific drops, and the flyby actor is invincible. The item
        // stage appends the ordinary shared vial lot to this owned NPC row.
        for(int i=44;i<=68;i+=4)BitConverter.GetBytes(-1).CopyTo(payload,i);
        MapCharacters.CloneRow(npcs,donor,Npc,"Imported Hellkite c3430",payload);npcFile.Bytes=MapCharacters.WriteRows(npcs);
        files["param/gameparam/gameparam.parambnd.dcx"]=game.Write(DCX.Type.DCX_DFLT_10000_44_9);
        var donorMap=MSBB.Read(Native("map/MapStudio/"+Profile.Map+".msb.dcx"));var actorTemplate=donorMap.Parts.Enemies.First(p=>p.ModelName==Profile.Native);
        var model=(MSBB.Model.Enemy)donorMap.Models.Enemies.Single(m=>m.Name==Profile.Native).DeepCopy();model.Name=Model;model.SibPath=model.SibPath.Replace(Profile.Native,Model,StringComparison.OrdinalIgnoreCase);destination.Models.Enemies.Add(model);
        var spawns=new List<MapEnemySpawns.Spawn>();
        foreach(var (from,id) in new[]{(plan.Bridge,BridgeEntity),(plan.Flyby,FlybyEntity)}) {
            var a=(MSBB.Part.Enemy)actorTemplate.DeepCopy();a.Name="souls_hellkite_"+(id==BridgeEntity?"bridge":"flyby");a.ModelName=Model;a.EntityID=id;a.NPCParamID=Npc;a.ThinkParamID=Profile.Think;a.CharaInitID=-1;a.TalkID=-1;
            a.InitAnimID=7006;a.DamageAnimID=-1;a.Position=from.Position+offset;a.Rotation=from.Rotation;a.Scale=Vector3.One;
            a.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,from.CollisionName,a.Position)].Name;
            a.PatrolInfoName=null!;a.PatrolType=0;a.FallbackPlatoonID=0;Array.Clear(a.MovePointNames);Array.Fill(a.DrawGroups,uint.MaxValue);Array.Fill(a.DispGroups,uint.MaxValue);Array.Copy(actorTemplate.BackreadGroups,a.BackreadGroups,a.BackreadGroups.Length);destination.Parts.Enemies.Add(a);
            spawns.Add(new(from.Name,from.ModelName,from.NPCParamID,from.ThinkParamID,from.EntityID,Model,id,Npc,Profile.Think,a.Position,masks.Order().ToArray(),skin.DirectWeightShare,1,skin.Bones,false,from.InitAnimID,Profile.Native));
        }
        int next=4032870;
        int Region(MSB1.Region from,string label) {
            int id=next++;Require(!destination.Regions.GetEntries().Any(r=>r.EntityID==id),"region reservation occupied");
            destination.Regions.Regions.Add(new(){EntityID=id,Name="souls_hellkite_"+label,Position=from.Position+offset,Rotation=from.Rotation,Shape=from.Shape.DeepCopy()});return id;
        }
        int flybyTrigger=Region(plan.FlybyTrigger,"flyby_trigger"),flybyStart=Region(plan.FlybyStart,"flyby_start"),bridgeTrigger=Region(plan.BridgeTrigger,"bridge_trigger"),bridgeExclusion=Region(plan.BridgeExclusion,"bridge_exclusion"),bridgeStart=Region(plan.BridgeStart,"bridge_start"),perch=Region(plan.Perch,"perch");
        var script=EMEVD.Read(events);Require(script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+3),"wrong VM or event reservation occupied");
        var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);script.Events.Add(pre);}var ctor=script.Events.Single(e=>e.ID==0);
        EMEVD.Instruction I(int bank,int id,params object[] args)=>MapBossEncounters.Ins(bank,id,args);
        var init=new List<EMEVD.Instruction>{I(2003,2,FlybyRunning,(byte)0),I(2003,2,BridgeReady,(byte)0)};
        foreach(int actor in new[]{BridgeEntity,FlybyEntity})init.AddRange([I(2004,5,actor,(byte)0),I(2004,1,actor,(byte)0),I(2004,10,actor,(byte)0),I(2004,31,actor,(byte)1),I(2004,15,actor,(byte)1)]);
        Prefix(pre,init);
        var fly=new EMEVD.Event(FirstEvent,EMEVD.Event.RestBehaviorType.Restart);
        fly.Instructions.AddRange([I(1003,2,(byte)0,(byte)1,(byte)0,FlybySeen),I(1003,2,(byte)0,(byte)1,(byte)0,Defeated),I(3,2,(sbyte)0,(byte)1,10000,flybyTrigger),I(2003,2,FlybySeen,(byte)1),I(2003,2,FlybyRunning,(byte)1)]);
        Flight(fly,FlybyEntity,flybyStart,7012,plan.Motions[7012].Duration);fly.Instructions.AddRange([I(2004,5,FlybyEntity,(byte)0),I(2004,35,FlybyEntity,(byte)0),I(2003,2,FlybyRunning,(byte)0)]);
        var bridge=new EMEVD.Event(FirstEvent+1,EMEVD.Event.RestBehaviorType.Restart);bridge.Instructions.Add(I(1003,2,(byte)0,(byte)1,(byte)0,Defeated));
        var arrival=new List<EMEVD.Instruction>{I(3,0,(sbyte)1,(byte)1,(byte)0,FlybySeen),I(3,0,(sbyte)1,(byte)0,(byte)0,FlybyRunning),I(3,2,(sbyte)1,(byte)1,10000,bridgeTrigger),I(3,2,(sbyte)1,(byte)0,10000,bridgeExclusion),I(0,0,(sbyte)0,(byte)1,(sbyte)1),I(2003,2,BridgeSeen,(byte)1)};
        var motionEvent=new EMEVD.Event(0);Flight(motionEvent,BridgeEntity,bridgeStart,7005,plan.Motions[7005].Duration);arrival.AddRange(motionEvent.Instructions);
        bridge.Instructions.Add(I(1003,1,checked((byte)arrival.Count),(byte)1,(byte)0,BridgeSeen));bridge.Instructions.AddRange(arrival);
        bridge.Instructions.AddRange([I(2004,5,BridgeEntity,(byte)1),I(2004,35,BridgeEntity,(byte)1),I(2004,41,BridgeEntity,(byte)1,perch,-1),I(2004,9,BridgeEntity,7006,-1,-1,-1,-1),I(2003,18,BridgeEntity,7006,(byte)1,(byte)0,(byte)0),I(2004,15,BridgeEntity,(byte)0),I(2003,2,BridgeReady,(byte)1)]);
        var death=new EMEVD.Event(FirstEvent+2,EMEVD.Event.RestBehaviorType.Restart);
        death.Instructions.AddRange([I(1003,2,(byte)0,(byte)1,(byte)0,Defeated),I(3,0,(sbyte)1,(byte)1,(byte)0,BridgeReady),I(4,0,(sbyte)1,BridgeEntity,(byte)1),I(0,0,(sbyte)0,(byte)1,(sbyte)1),I(2003,2,Defeated,(byte)1),I(2003,2,BridgeReady,(byte)0)]);
        foreach(var e in new[]{fly,bridge,death})script.Events.Add(e);Prefix(ctor,new[]{fly,bridge,death}.Select(e=>I(2000,0,0,(uint)e.ID,0u)).ToArray());
        void Flight(EMEVD.Event e,int actor,int start,int animation,float duration) {
            e.Instructions.AddRange([I(2004,35,actor,(byte)1),I(2004,34,actor,(byte)1,(sbyte)0),I(2004,5,actor,(byte)1),I(2004,41,actor,(byte)1,start,-1),I(2003,18,actor,animation,(byte)0,(byte)0,(byte)0),I(1001,0,duration)]);
        }
        var limits=new[]{"The first Burg c3430 flyby and subsequent bridge approach use the checked source regions,9.3333s/13.1667s flight poses and extracted trajectories. The bridge actor restores to its authored perch on later visits; seen/defeat flags persist while in-flight flags reset on reload.",
            "Source visible animation uses132 bones appended to a110-bone native c2730 wrapper, within the256-node skin limit. Retail native physical channels/container metadata are preserved while source flight poses, duration and extracted trajectories are adapted. Native gravity/map collision are disabled for these cinematic actors, matching the source flight/perch state.",
            "This stage converts the flyby, bridge approach and perched waiting appearance. Dragon fire breath, bridge sweeping damage, ground combat, tail severing, source voices/SFX and source object destruction are not converted. The donor's melee AI is deliberately disabled. Native damage/capsule/death/loot behavior remains approximate; source base HP/reward values retain native scaling."};
        var result=script.Write(DCX.Type.DCX_DFLT_10000_44_9);_=EMEVD.Read(result);_=MSBB.Read(destination.Write());
        return new(result,files,spawns,new(BridgeEntity,FlybyEntity,FlybySeen,BridgeSeen,Defeated,flybyTrigger,bridgeTrigger,bridgeExclusion,perch,clips,plan.SourceEvents,limits));
    }
    static void Prefix(EMEVD.Event e,IReadOnlyList<EMEVD.Instruction> prefix){foreach(var p in e.Parameters)p.InstructionIndex+=prefix.Count;e.Instructions.InsertRange(0,prefix);}
}
