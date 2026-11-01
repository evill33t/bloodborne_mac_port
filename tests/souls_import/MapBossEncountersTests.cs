// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

internal static class MapBossEncountersTests {
    static void Check(bool ok,string why){if(!ok)throw new InvalidDataException(why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static bool Same(EMEVD.Instruction a,EMEVD.Instruction b)=>a.Bank==b.Bank&&a.ID==b.ID&&a.Layer==b.Layer&&a.ArgData.SequenceEqual(b.ArgData);
    sealed class Actor {internal bool Enabled=true,AI=true,Invincible,Dead;internal float HP=1;}
    sealed class World {
        internal readonly Dictionary<int,bool> Flags=[];
        internal readonly Dictionary<int,Actor> Actors=[];
        internal readonly Dictionary<int,bool> Objects=[],Effects=[];
        internal readonly Dictionary<(int Actor,short Slot),bool> Bars=[];
        internal readonly HashSet<int> Inside=[],Writes=[];
        internal readonly HashSet<(int Param,int Entity)> Actions=[];
        internal readonly List<int> Awards=[],Clears=[];
        internal readonly List<(int Actor,int Region,int Animation)> Rotations=[];
        internal readonly List<(int Actor,int Animation,bool Loop)> Animations=[];
        internal readonly HashSet<(int Actor,int Animation)> CompletedAnimations=[];
        internal readonly HashSet<int> Damaged=[];
        internal readonly List<int> AnimationResets=[];
        internal float Time;
        internal Actor A(int id){if(!Actors.TryGetValue(id,out var a))Actors.Add(id,a=new());return a;}
    }
    // A small interpreter of the emitted native opcode subset, with live AND/OR
    // conditions and explicit time. It tests observable state, not code order.
    sealed class Machine(IEnumerable<EMEVD.Instruction> input,World world) {
        readonly EMEVD.Instruction[] code=input.ToArray();
        readonly Dictionary<sbyte,List<Func<bool>>> groups=[];
        readonly Dictionary<int,float> delays=[];
        readonly HashSet<int> animationRequests=[];
        int pc;bool complete;
        bool Flag(int id)=>world.Flags.GetValueOrDefault(id);
        bool Group(sbyte id)=>groups.TryGetValue(id,out var g)&&(id<0?g.Any(f=>f()):g.All(f=>f()));
        bool Condition(sbyte group,Func<bool> test) {
            if(group==0)return test();
            if(!groups.TryGetValue(group,out var members))groups.Add(group,members=[]);members.Add(test);return true;
        }
        void Restart(){pc=0;groups.Clear();delays.Clear();}
        bool End(byte kind){if(kind==0){complete=true;return true;}Check(kind==1,"unknown native end mode");Restart();return false;}
        internal bool Run() {
            if(complete)return true;
            for(int steps=0;pc<code.Length;steps++) {
                Check(steps<10000,"native event did not reach a wait/end");var i=code[pc];var b=i.ArgData;
                int I(int at=0)=>BitConverter.ToInt32(b,at);float F(int at)=>BitConverter.ToSingle(b,at);sbyte S(int at=0)=>unchecked((sbyte)b[at]);
                switch((i.Bank,i.ID)) {
                    case (1003,1):Check(b[2]==0,"relative flag in emitted event");if(Flag(I(4))==(b[1]!=0))pc+=b[0];break;
                    case (1003,2):Check(b[2]==0,"relative flag in end guard");if(Flag(I(4))==(b[1]!=0)){if(End(b[0]))return true;continue;}break;
                    case (1000,3):pc+=b[0];break;
                    case (1000,2):if(Group(S(2))==(b[1]!=0)){if(End(b[0]))return true;continue;}break;
                    case (1000,4):if(End(b[0]))return true;continue;
                    case (2003,2):world.Flags[I()]=b[4]!=0;world.Writes.Add(I());break;
                    case (2003,11):world.Bars[(I(4),BitConverter.ToInt16(b,8))]=b[0]!=0;break;
                    case (2003,12):world.Clears.Add(I());break;
                    case (2003,4):world.Awards.Add(I());break;
                    case (2003,18):
                        if(animationRequests.Add(pc))world.Animations.Add((I(),I(4),b[8]!=0));
                        if(b[9]!=0&&!world.CompletedAnimations.Contains((I(),I(4))))return false;
                        break;
                    case (2004,1):world.A(I()).AI=b[4]!=0;break;
                    case (2004,5):world.A(I()).Enabled=b[4]!=0;break;
                    case (2004,15):world.A(I()).Invincible=b[4]!=0;break;
                    case (2004,43):world.AnimationResets.Add(I());break;
                    case (2004,13):case (2004,20):break;
                    case (2004,14):world.Rotations.Add((I(),I(4),I(8)));break;
                    case (2005,3):world.Objects[I()]=b[4]!=0;break;
                    case (2006,1):world.Effects[I()]=false;break;
                    case (2006,2):world.Effects[I()]=true;break;
                    case (3,0): {int id=I(4);bool on=b[1]!=0;Check(b[2]==0,"relative flag condition");if(!Condition(S(),()=>Flag(id)==on))return false;break;}
                    case (3,2): {int id=I(8);bool inside=b[1]!=0;Check(I(4)==10000,"nonplayer admission condition");if(!Condition(S(),()=>world.Inside.Contains(id)==inside))return false;break;}
                    case (3,24): {int param=I(4),id=I(8);if(!Condition(S(),()=>world.Actions.Contains((param,id))))return false;break;}
                    case (4,0): {int id=I(4);bool dead=b[8]!=0;if(!Condition(S(),()=>world.A(id).Dead==dead))return false;break;}
                    case (4,1): {int id=I(4);Check(I(8)==10000,"unexpected entrance attacker");if(!Condition(S(),()=>world.Damaged.Contains(id)))return false;break;}
                    case (4,2): {int id=I(4),compare=S(8);float hp=F(12);Check(compare==5,"unexpected source HP comparison");if(!Condition(S(),()=>world.A(id).HP<=hp))return false;break;}
                    case (1,0): {float targetTime=world.Time+F(4);if(!Condition(S(),()=>world.Time>=targetTime))return false;break;}
                    case (0,0):Check(b[0]==0,"unexpected native main group assignment");if(Group(S(2))!=(b[1]!=0))return false;break;
                    case (1001,0):if(!delays.TryGetValue(pc,out var until))delays.Add(pc,until=world.Time+F(0));if(world.Time<until)return false;break;
                    default:throw new InvalidDataException("Unimplemented native test opcode "+i.Bank+":"+i.ID);
                }
                pc++;
            }
            complete=true;return true;
        }
    }
    static Dictionary<(int,int),int[]> NativeSchema(byte[] bytes) {
        int word=bytes[5]==0?4:8;
        long N(int at)=>word==8?BitConverter.ToInt64(bytes,at):BitConverter.ToInt32(bytes,at);
        var header=Enumerable.Range(0,19).Select(i=>N(16+i*word)).ToArray();var result=new Dictionary<(int,int),int[]>();
        for(int c=0;c<header[0];c++) {
            int at=checked((int)header[1]+c*(word*4+(word==4?4:0)));int bank=checked((int)N(at)),count=checked((int)N(at+word));long offset=N(at+word*2);
            for(int n=0;n<count;n++) {
                int ins=checked((int)(header[3]+offset+n*(word*4+(word==4?4:0))));int id=checked((int)N(ins)),argc=checked((int)N(ins+word));long args=N(ins+word*2);
                result.Add((bank,id),Enumerable.Range(0,argc).Select(k=>BitConverter.ToInt32(bytes,checked((int)(header[5]+args+k*(word==8?64:36)+word)))).ToArray());
            }
        }
        return result;
    }
    static PARAM Opaque(Memory<byte> bytes) {
        var p=PARAM.Read(bytes);var d=new PARAMDEF{ParamType=p.ParamType,DataVersion=p.ParamdefDataVersion,Unicode=true};
        d.Fields.Add(new PARAMDEF.Field(d,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=checked((int)p.DetectedSize)});p.ApplyParamdef(d);return p;
    }
    static void Main(string[] args) {
        Check(args.Length==4,"Pass source root, native dvdroot, pre-boss connected mod, DS1R definitions");
        string sourceRoot=args[0],nativeRoot=args[1],mod=args[2],defs=args[3];
        if(Directory.Exists(Path.Combine(mod,"dvdroot_ps4")))mod=Path.Combine(mod,"dvdroot_ps4");
        var inputs=new Dictionary<string,string>();
        byte[]? ReadAt(string root,string path){string full=Path.Combine(root,path);if(!File.Exists(full))return null;var b=File.ReadAllBytes(full);inputs.TryAdd(full,Convert.ToHexStringLower(SHA256.HashData(b)));return b;}
        byte[]? Source(string path)=>ReadAt(sourceRoot,path);byte[]? Native(string path)=>ReadAt(nativeRoot,path);
        var staged=new Dictionary<string,byte[]>(StringComparer.Ordinal);
        byte[]? Composed(string path)=>staged.GetValueOrDefault(path)??ReadAt(mod,path)??Native(path);
        var plan=BossEncounterSource.Load(Source,"DS1R",BossEncounterSource.SourceMap,defs);
        const string mapPath="map/MapStudio/m40_03_00_00.msb.dcx",paramPath="param/gameparam/gameparam.parambnd.dcx",eventPath="event/m40_03_00_00.emevd.dcx";
        var map=MSBB.Read(Composed(mapPath)??throw new FileNotFoundException("Pre-boss connected map required"));
        Check(map.Parts.Enemies.All(a=>a.EntityID is <4039800 or >4039805),"Pass a pre-boss baseline: test owns the new encounter fixtures");
        var offset=new Vector3(-143.8f,20.07f,164.63f);var source=MSB1.Read(Source("map/MapStudio/m10_01_00_00.msb")!);
        var parameters=BND4.Read(Composed(paramPath)!);var npcFile=parameters.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var rows=MapCharacters.NativeRows(npcFile.Bytes,false);
        var sourceParameters=BND3.Read(Source("param/GameParam/GameParam.parambnd.dcx")!);var sourceNpcs=PARAM.Read(sourceParameters.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);sourceNpcs.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(defs,"NpcParam.xml")));
        var spawns=new List<MapEnemySpawns.Spawn>();
        var roles=new[]{(1010700,800,8950),(1010750,801,8951),(1010500,802,8952),(1010501,803,8952),(1010800,804,8953),(1010801,805,8954)};
        foreach(var (id,role,modelID) in roles) {
            var from=source.Parts.Enemies.Single(a=>a.EntityID==id);var profile=BossEnemyCharacters.Select(from,BossEncounterSource.SourceMap)??throw new InvalidDataException("Missing explicit source role");
            var donorMap=MSBB.Read(Native("map/MapStudio/"+profile.Map+".msb.dcx")!);var donor=donorMap.Parts.Enemies.First(a=>a.ModelName==profile.Native);
            string model="c"+modelID;int param=modelID*100,entity=4039000+role;
            if(!rows.Rows.Any(r=>r.ID==param)){var row=rows.Rows.Single(r=>r.ID==profile.Npc);MapCharacters.CloneRow(rows,row,param,"Imported boss encounter actor "+model,BossEnemyCharacters.Parameters(MapCharacters.Payload(row),sourceNpcs.Rows.Single(r=>r.ID==from.NPCParamID),defs));}
            if(!map.Models.Enemies.Any(m=>m.Name==model)){var m=(MSBB.Model.Enemy)donorMap.Models.Enemies.Single(m=>m.Name==profile.Native).DeepCopy();m.Name=model;map.Models.Enemies.Add(m);}
            var a=(MSBB.Part.Enemy)donor.DeepCopy();a.Name=model+"_fixture_"+role;a.ModelName=model;a.EntityID=entity;a.NPCParamID=param;a.Position=from.Position+offset;a.Rotation=from.Rotation;a.Scale=Vector3.One;a.CollisionName=map.Parts.Collisions[0].Name;
            a.PatrolInfoName=null!;a.PatrolType=0;a.FallbackPlatoonID=0;a.CharaInitID=-1;Array.Clear(a.MovePointNames);map.Parts.Enemies.Add(a);
            spawns.Add(new(from.Name,from.ModelName,from.NPCParamID,from.ThinkParamID,id,model,entity,param,profile.Think,a.Position,[],1,1,[],false,from.InitAnimID));
        }
        npcFile.Bytes=MapCharacters.WriteRows(rows);staged[paramPath]=parameters.Write(DCX.Type.DCX_DFLT_10000_44_9);
        byte[] beforeParameters=staged[paramPath];var beforeMap=MSBB.Read(map.Write());
        var chunks=new[]{new MapCollisionBindings.Chunk("fixture_floor",new[]{new MapCollision.Triangle(new(-1000,0,-1000),new(1000,0,-1000),new(0,0,1000))},[])};
        var slots=new[]{map.Parts.Collisions[0]};
        var original=EMEVD.Read(Composed(eventPath)!);var ctor=original.Events.Single(e=>e.ID==0);if(ctor.Instructions.Count>0)ctor.Parameters.Add(new(0,0,0,4));
        var pre=original.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);original.Events.Add(pre);}if(pre.Instructions.Count==0)pre.Instructions.Add(new(2003,2,59999999,(byte)0));pre.Parameters.Add(new(0,0,0,4));
        var sentinel=new EMEVD.Event(19999990);sentinel.Instructions.Add(new(1001,0,1f));original.Events.Add(sentinel);
        var result=MapBossEncounters.Apply(plan,map,spawns,offset,chunks,slots,Source,Native,Composed,original.Write(),"m40_03_00_00",nativeRoot,defs,true);
        var script=EMEVD.Read(result.Script);int firstEvent=result.Encounters.Min(e=>e.EventID);var generated=script.Events.Where(e=>e.ID>=firstEvent&&e.ID<firstEvent+30).ToArray();
        Check(result.Encounters.Count==3&&result.Encounters.Select(e=>e.Boss).SequenceEqual(new[]{4039800,4039801,4039804})&&result.Encounters.Single(e=>e.Key=="gargoyles").Secondary==4039805,"append-only source roles");
        Check(generated.Length==13&&result.Encounters.SelectMany(e=>new[]{e.EnteredFlag,e.ActiveFlag,e.PhaseFlag,e.RewardFlag}).Distinct().Count()==12,"owned events/flags are distinct");
        int preShift=script.Events.Single(e=>e.ID==50).Instructions.Count-pre.Instructions.Count,ctorShift=script.Events.Single(e=>e.ID==0).Instructions.Count-ctor.Instructions.Count;
        foreach(var e in original.Events) {
            var after=script.Events.Single(a=>a.ID==e.ID);int shift=e.ID==0?ctorShift:e.ID==50?preShift:0;
            Check(after.Instructions.Skip(shift).Zip(e.Instructions).All(p=>Same(p.First,p.Second))&&after.Instructions.Count==e.Instructions.Count+shift&&after.RestBehavior==e.RestBehavior,"existing native event changed "+e.ID);
            Check(after.Parameters.Count==e.Parameters.Count&&after.Parameters.Zip(e.Parameters).All(p=>p.First.InstructionIndex==p.Second.InstructionIndex+shift&&p.First.TargetStartByte==p.Second.TargetStartByte&&p.First.SourceStartByte==p.Second.SourceStartByte&&p.First.ByteCount==p.Second.ByteCount&&p.First.UnkID==p.Second.UnkID),"native parameter patch moved incorrectly");
        }
        Check(script.StringData.SequenceEqual(original.StringData)&&script.LinkedFileOffsets.SequenceEqual(original.LinkedFileOffsets),"native event links changed");
        var schemaBytes=Native("event/common.emedf.dcx")??throw new FileNotFoundException("Native instruction metadata unavailable");
        var nativeSchema=NativeSchema(DCX.Is(schemaBytes)?DCX.Decompress(schemaBytes).ToArray():schemaBytes);
        var prefix=script.Events.Single(e=>e.ID==50).Instructions.Take(preShift).ToArray();
        foreach(var i in generated.SelectMany(e=>e.Instructions).Concat(prefix)) {
            Check(nativeSchema.TryGetValue((i.Bank,i.ID),out var schema),"opcode absent from shipped native metadata");int bytes=0;
            foreach(int type in schema!){int size=type is 0 or 3?1:type is 1 or 4?2:4;bytes=(bytes+size-1)/size*size;bytes+=size;}bytes=(bytes+3)/4*4;
            Check(i.ArgData.Length==bytes,"argument layout disagrees with shipped native metadata: "+i.Bank+":"+i.ID);
        }
        World Fresh(Dictionary<int,bool>? retained=null) {
            var w=new World();if(retained!=null)foreach(var p in retained)w.Flags.Add(p.Key,p.Value);w.Flags[MapQuestState.Flag(11010700)]=false;w.Flags[12345678]=true;
            new Machine(prefix,w).Run();return w;
        }
        Dictionary<long,Machine> Machines(World w)=>generated.ToDictionary(e=>e.ID,e=>new Machine(e.Instructions,w));
        void Tick(Dictionary<long,Machine> machines){foreach(var m in machines.Values)m.Run();}
        var world=Fresh();var machines=Machines(world);Tick(machines);
        foreach(var e in result.Encounters) {
            Check(world.A(e.Boss) is {Enabled:true,AI:false,Invincible:true}&&!world.Flags.GetValueOrDefault(e.ActiveFlag),"fresh boss starts protected/inactive");
            Check(e.Walls.All(w=>world.Objects.GetValueOrDefault(w.ObjectEntity)&&world.Effects.GetValueOrDefault(w.SfxEntity)),"fresh encounter lacks solid fog/effect");
            foreach(int add in e.Adds)Check(world.A(add) is {Enabled:true,AI:false,Invincible:true},"fresh Capra add starts active");
        }
        var taurus=result.Encounters.Single(e=>e.Key=="taurus");var entry=plan.Encounters.Single(e=>e.Key=="taurus").EntryFog;
        Check(taurus.IntroRegion!=null&&world.Animations.Contains((taurus.Boss,7000,true)),"Taurus does not wait in the source entrance pose");
        world.Inside.Add(taurus.StartRegion);machines[taurus.EventID].Run();Check(!world.Flags.GetValueOrDefault(taurus.EnteredFlag)&&!world.A(taurus.Boss).AI,"appearing inside bypassed fog admission");world.Inside.Remove(taurus.StartRegion);
        int outside=map.Regions.GetEntries().Single(r=>r.Name=="souls_boss_taurus_outside").EntityID,inside=map.Regions.GetEntries().Single(r=>r.Name=="souls_boss_taurus_inside").EntityID;
        var action=(BossFogWalls.ActionButtonID,taurus.Walls.Single(w=>w.SourceName==entry.Name).ObjectEntity);
        world.Inside.Add(outside);world.Actions.Add(action);machines[taurus.EventID+2].Run();Check(world.Rotations.Last().Region==inside&&!world.Flags.GetValueOrDefault(taurus.EnteredFlag),"fog action admitted before reaching inside");
        world.Actions.Clear();world.Time+=3;machines[taurus.EventID+2].Run();Check(!world.Flags.GetValueOrDefault(taurus.EnteredFlag)&&!world.Flags.GetValueOrDefault(taurus.ActiveFlag),"failed fog traversal activated boss");
        world.Actions.Add(action);machines[taurus.EventID+2].Run();world.Actions.Clear();world.Inside.Remove(outside);world.Inside.Add(taurus.StartRegion);machines[taurus.EventID+2].Run();Check(world.Flags.GetValueOrDefault(taurus.EnteredFlag)&&!world.Flags.GetValueOrDefault(taurus.ActiveFlag),"successful admission state");
        machines[taurus.EventID].Run();Check(!world.A(taurus.Boss).AI&&!world.Bars[(taurus.Boss,0)]&&!world.Flags.GetValueOrDefault(taurus.ActiveFlag),"fog entry skipped the delayed source entrance");
        world.Inside.Add(taurus.IntroRegion!.Value);machines[taurus.EventID].Run();Check(world.Animations.Contains((taurus.Boss,7010,false))&&world.Flags.GetValueOrDefault(taurus.PhaseFlag)&&!world.A(taurus.Boss).AI&&!world.Flags.GetValueOrDefault(taurus.ActiveFlag),"jump did not start in the source trigger or released AI before landing");
        world.Time+=4.15f;machines[taurus.EventID].Run();Check(!world.A(taurus.Boss).AI&&world.AnimationResets.Count==0,"source jump was interrupted before its clip boundary");
        world.Time+=.02f;machines[taurus.EventID].Run();Check(world.A(taurus.Boss) is {AI:true,Invincible:false}&&world.Bars[(taurus.Boss,0)]&&world.Flags.GetValueOrDefault(taurus.ActiveFlag)&&world.AnimationResets.SequenceEqual(new[]{taurus.Boss}),"completed jump did not release the donor animation/controller");
        var attacked=Fresh();var attackedMachines=Machines(attacked);Tick(attackedMachines);attacked.Flags[taurus.EnteredFlag]=true;attacked.Damaged.Add(taurus.Boss);attackedMachines[taurus.EventID].Run();Check(attacked.Animations.Contains((taurus.Boss,7010,false))&&!attacked.A(taurus.Boss).AI,"source damage entrance trigger was lost");
        var retry=Fresh(new(world.Flags));var retryMachines=Machines(retry);Tick(retryMachines);Check(!retry.Flags.GetValueOrDefault(taurus.EnteredFlag)&&!retry.Flags.GetValueOrDefault(taurus.ActiveFlag)&&retry.A(taurus.Boss).Invincible,"load/retry retained transient admission");
        var capra=result.Encounters.Single(e=>e.Key=="capra");var cw=Fresh();var cm=Machines(cw);Tick(cm);cw.Inside.Add(capra.StartRegion);cm[capra.EventID].Run();Check(!cw.A(capra.Boss).AI,"Capra bypassed fog admission");cw.Flags[capra.EnteredFlag]=true;cm[capra.EventID].Run();
        Check(cw.A(capra.Boss) is {AI:true,Invincible:false}&&capra.Adds.All(a=>cw.A(a) is {AI:true,Invincible:false})&&cw.Flags.GetValueOrDefault(MapQuestState.Flag(11010597)),"Capra entry did not release both dogs and preserve source dialogue progression");
        cw.A(capra.Boss).Dead=true;cm[capra.EventID+1].Run();Check(cw.Flags.GetValueOrDefault(capra.DefeatedFlag)&&capra.Adds.All(a=>!cw.A(a).Enabled),"Capra clear left encounter dogs active");
        cm[capra.EventID+4].Run();Check(cw.Awards.SequenceEqual(new[]{90002510})&&!cw.Flags.GetValueOrDefault(capra.RewardFlag),"Capra key was not immediate/separate from delayed consumables");
        cw.Time=5.01f;cm[capra.EventID+4].Run();Check(cw.Awards.SequenceEqual(new[]{90002510,90009000,90009030})&&cw.Flags.GetValueOrDefault(capra.RewardFlag),"Capra delayed rewards did not finish");
        var gargoyle=result.Encounters.Single(e=>e.Key=="gargoyles");var g=Fresh();var gm=Machines(g);Tick(gm);int second=gargoyle.Secondary!.Value;
        Check(!g.A(second).Enabled&&!g.A(second).AI,"second Gargoyle active before phase");g.Inside.Add(gargoyle.StartRegion);g.Flags[gargoyle.EnteredFlag]=true;gm[gargoyle.EventID].Run();g.A(gargoyle.Boss).HP=.60001f;gm[gargoyle.EventID+3].Run();Check(!g.A(second).Enabled,"phase begins above source60%threshold");
        g.A(gargoyle.Boss).HP=.6f;gm[gargoyle.EventID+3].Run();Check(g.A(second) is {Enabled:true,AI:true,Invincible:false}&&g.Flags.GetValueOrDefault(gargoyle.PhaseFlag)&&g.Bars[(second,1)],"second Gargoyle does not activate at exact60%");
        g.A(gargoyle.Boss).Dead=true;g.A(gargoyle.Boss).HP=0;gm[gargoyle.EventID+1].Run();Check(!g.Flags.GetValueOrDefault(gargoyle.DefeatedFlag),"one Gargoyle death cleared the encounter");
        g.A(second).Dead=true;gm[gargoyle.EventID+1].Run();Check(g.Flags.GetValueOrDefault(gargoyle.DefeatedFlag)&&g.Clears.SequenceEqual(new[]{gargoyle.Boss})&&gargoyle.Walls.All(w=>!g.Objects[w.ObjectEntity]&&!g.Effects[w.SfxEntity]),"both deaths did not clear/remove fog exactly once");
        gm[gargoyle.EventID+4].Run();Check(!g.Flags.GetValueOrDefault(gargoyle.RewardFlag)&&g.Awards.Count==0,"delayed reward was not delayed");
        // Discard running machines before the5s wait, just like a map unload.
        var resumed=Fresh(new(g.Flags));var resumedMachines=Machines(resumed);Tick(resumedMachines);Check(!resumed.A(gargoyle.Boss).Enabled&&!resumed.A(second).Enabled,"defeated load respawned Gargoyles");
        resumed.Time=5.01f;Tick(resumedMachines);Check(resumed.Flags.GetValueOrDefault(gargoyle.RewardFlag)&&resumed.Awards.SequenceEqual(new[]{90009020}),"interrupted clear lost delayed reward");
        var settled=Fresh(new(resumed.Flags));Tick(Machines(settled));Check(settled.Awards.Count==0&&settled.Flags.GetValueOrDefault(gargoyle.RewardFlag),"completed clear reward repeated on reload");
        Check(!g.Flags[MapQuestState.Flag(11010700)]&&g.Flags[12345678]&&!resumed.Flags[MapQuestState.Flag(11010700)]&&resumed.Flags[12345678],"boss controller changed unrelated bell/native state");
        var allowed=result.Encounters.SelectMany(e=>new[]{e.DefeatedFlag,e.EnteredFlag,e.ActiveFlag,e.PhaseFlag,e.RewardFlag}).Append(MapQuestState.Flag(11010597)).ToHashSet();
        Check(new[]{world,retry,cw,g,resumed,settled}.All(w=>w.Writes.IsSubsetOf(allowed)),"event wrote an unowned native/quest flag");
        foreach(var e in result.Encounters)foreach(var wall in e.Walls)Check(map.Parts.Objects.Single(o=>o.EntityID==wall.ObjectEntity).CollisionName==slots[0].Name,"fog references an unavailable collision slot");
        foreach(var e in plan.Encounters) {
            var r=map.Regions.GetEntries().Single(r=>r.Name=="souls_boss_"+e.Key+"_start");Check(r.Position==e.StartRegion.Position+offset&&r.Rotation==e.StartRegion.Rotation,"source start region transform changed");
            var actor=map.Parts.Enemies.Single(a=>a.EntityID==spawns.Single(s=>s.SourceEntityID==e.Boss.EntityID).EntityID);Check(actor.Position==e.Boss.Position+offset,"boss does not use the checked source starting placement");
            if(e.Entrance is {} entrance) {
                Check(actor.InitAnimID==7000,"Taurus initial pose does not wait on the tower");
                var trigger=map.Regions.GetEntries().Single(r=>r.Name=="souls_boss_taurus_intro");Check(trigger.Position==entrance.Trigger.Position+offset&&trigger.Rotation==entrance.Trigger.Rotation,"source Taurus entrance trigger moved");
                var nest=map.Regions.GetEntries().Single(r=>r.Name=="souls_boss_taurus_nest");Check(nest.Position==entrance.RootMotion!.LandingPosition+offset,"Taurus combat home is on the intro tower");
            }
        }
        var afterParameters=BND4.Read(result.Files[paramPath]);var beforeBinder=BND4.Read(beforeParameters);int originalRows=0;
        foreach(var file in beforeBinder.Files) {
            var after=afterParameters.Files.Single(f=>f.ID==file.ID);string name=Leaf(file.Name);
            if(name is not("NpcParam.param" or "GameAreaParam.param" or "ObjectParam.param" or "SpEffectParam.param")){Check(after.Bytes.Span.SequenceEqual(file.Bytes.Span),"unrelated parameter member changed: "+name);continue;}
            var oldRows=Opaque(file.Bytes);var newRows=Opaque(after.Bytes);var changed=name=="NpcParam.param"?new HashSet<int>{895000,895100,895200,895300,895400}:[];
            var oldIDs=oldRows.Rows.Select(r=>r.ID).ToHashSet();var originalSequence=newRows.Rows.Where(r=>oldIDs.Contains(r.ID)).ToArray();
            Check(oldRows.Rows.Select(r=>r.ID).SequenceEqual(originalSequence.Select(r=>r.ID)),"original parameter row order/duplicate population changed");
            foreach(var (row,index) in oldRows.Rows.Select((r,i)=>(r,i))){var actual=originalSequence[index];if(changed.Contains((int)row.ID))continue;Check(actual.Name==row.Name&&ImportParam.Payload(actual).SequenceEqual(ImportParam.Payload(row)),"existing parameter row changed: "+name+":"+row.ID);originalRows++;}
        }
        var areaFile=afterParameters.Files.Single(f=>Leaf(f.Name)=="GameAreaParam.param");var areas=PARAM.Read(areaFile.Bytes);areas.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(defs,"../../BB/Defs/GameAreaParam.xml"))));
        foreach(var e in result.Encounters){var row=areas.Rows.Single(r=>r.ID==e.Boss);var expected=plan.Encounters.Single(p=>p.Key==e.Key);Check(Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="bonusSoul_single").Value)==expected.Rewards.SoloSouls,"source clear soul reward mismatch");}
        var npcDefinition=PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(defs,"../../BB/Defs/NpcParam.xml")));
        var oldTyped=PARAM.Read(beforeBinder.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);oldTyped.ApplyParamdef(npcDefinition);
        var newTyped=PARAM.Read(afterParameters.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);newTyped.ApplyParamdef(npcDefinition);
        bool EqualCell(object a,object b)=>a is byte[] aa&&b is byte[] bb?aa.SequenceEqual(bb):Equals(a,b);
        foreach(var e in result.Encounters)foreach(int actor in new[]{e.Boss,e.Secondary??-1}.Where(id=>id>=0)) {
            int id=map.Parts.Enemies.Single(a=>a.EntityID==actor).NPCParamID;var old=oldTyped.Rows.Single(r=>r.ID==id);var current=newTyped.Rows.Single(r=>r.ID==id);
            Check(Convert.ToInt32(current.Cells.Single(c=>c.Def.InternalName=="getSoul").Value)==0&&Convert.ToInt32(current.Cells.Single(c=>c.Def.InternalName=="nameId").Value)==e.NameID,"owned boss soul/name fields mismatch");
            Check(old.Cells.Zip(current.Cells).Where(pair=>pair.First.Def.InternalName is not("getSoul" or "nameId" or "spEffectID6" or "GameClearSpEffectID")).All(pair=>EqualCell(pair.First.Value,pair.Second.Value)),"owned boss changed unrelated capsule/HP/behavior/effects/masks");
            Check(Convert.ToInt32(current.Cells.Single(c=>c.Def.InternalName=="spEffectID6").Value) is >=895710 and <=895715&&Convert.ToInt32(current.Cells.Single(c=>c.Def.InternalName=="GameClearSpEffectID").Value) is >=895710 and <=895715,"owned encounter still uses donor HP multipliers");
        }
        int names=0;foreach(var (path,bytes) in result.Files.Where(p=>p.Key.StartsWith("msg/"))) {
            var before=BND4.Read(Composed(path)!);var after=BND4.Read(bytes);
            foreach(var f in before.Files){var a=after.Files.Single(v=>v.ID==f.ID);if(f.ID!=18){Check(a.Bytes.Span.SequenceEqual(f.Bytes.Span),"unrelated message table changed");continue;}var old=FMG.Read(f.Bytes);var current=FMG.Read(a.Bytes);foreach(var text in old.Entries)Check(current.Entries.Single(e=>e.ID==text.ID).Text==text.Text,"existing localized boss/NPC name changed");Check(current.Entries.Count==old.Entries.Count+3,"localized name count");names++;}
        }
        // Imported friendly models from the old connected package must retain
        // their global identities while the five boss roles use a new range.
        foreach(int id in new[]{890000,890100,890200})Check(Opaque(afterParameters.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes).Rows.Any(r=>r.ID==id),"existing friendly NPC parameter identity missing");
        Check(beforeMap.Parts.Enemies.Where(a=>a.EntityID<4039800||a.EntityID>4039805).All(a=>map.Parts.Enemies.Any(v=>v.EntityID==a.EntityID&&v.ModelName==a.ModelName&&v.NPCParamID==a.NPCParamID)),"existing actor identity changed");
        foreach(var (path,hash) in inputs)Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)))==hash,"input asset changed: "+path);
        Directory.CreateDirectory("out/map-editor/boss-encounters/events");File.WriteAllText("out/map-editor/boss-encounters/events/tests.json",JsonSerializer.Serialize(new{Encounters=result.Encounters,GeneratedEvents=generated.Length,OriginalRowsPreserved=originalRows,Languages=names,InputsPreserved=inputs.Count,Limit="Static native-VM and installed-asset fixture; no native gameplay proof."},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"PASS:3encounters,13nativeevents,phase/retry/fog/reward state checks,{originalRows}existingrows,{names}languages,{inputs.Count}inputassets preserved");
    }
}
