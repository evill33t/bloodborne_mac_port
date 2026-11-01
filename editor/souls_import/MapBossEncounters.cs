// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

// Offline, source-specific encounter state machines on the native event VM.
// Completion flags are persistent; admission/phase flags reset on every load.
internal static class MapBossEncounters {
    internal record Encounter(string Key,int Boss,int? Secondary,IReadOnlyList<int> Adds,int DefeatedFlag,int EnteredFlag,int ActiveFlag,int PhaseFlag,int RewardFlag,int StartRegion,int ArenaRegion,int NameID,int EventID,IReadOnlyList<BossFogWalls.Binding> Walls) {
        public int? IntroRegion {get;init;}
    }
    internal record Result(byte[] Script,IReadOnlyDictionary<string,byte[]> Files,IReadOnlyList<Encounter> Encounters,IReadOnlyList<string> Limits);
    const int FirstEvent=12415840,FirstFlag=52100940;
    internal static readonly int[] ReservedFlags=Enumerable.Range(FirstFlag,12).ToArray();
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Require(bool b,string why){if(!b)throw new InvalidDataException("Imported boss encounter: "+why);}
    internal static byte[] Args(params object[] values) {
        using var s=new MemoryStream();using var w=new BinaryWriter(s);
        foreach(var v in values) {
            int size=v is byte or sbyte?1:v is short or ushort?2:4;while(s.Position%size!=0)w.Write((byte)0);
            switch(v){case byte n:w.Write(n);break;case sbyte n:w.Write(n);break;case short n:w.Write(n);break;case ushort n:w.Write(n);break;case int n:w.Write(n);break;case uint n:w.Write(n);break;case float n:w.Write(n);break;default:throw new ArgumentException("Unsupported boss event argument");}
        }
        while(s.Position%4!=0)w.Write((byte)0);return s.ToArray();
    }
    internal static EMEVD.Instruction Ins(int bank,int id,params object[] values)=>new(bank,id,Args(values));
    sealed class Code {
        internal readonly List<EMEVD.Instruction> Instructions=[];
        readonly Dictionary<string,int> labels=new();readonly List<(int At,string Label,int? Flag,bool On)> branches=[];
        internal void Add(int bank,int id,params object[] values)=>Instructions.Add(Ins(bank,id,values));
        internal void Label(string label)=>labels.Add(label,Instructions.Count);
        internal void Jump(string label,int? flag=null,bool on=true){branches.Add((Instructions.Count,label,flag,on));Instructions.Add(Ins(1000,3));}
        internal EMEVD.Event Event(int id) {
            foreach(var b in branches){int n=labels[b.Label]-b.At-1;Require(n is >=0 and <=255,"event branch out of bounds");Instructions[b.At]=b.Flag is {} flag?Ins(1003,1,(byte)n,(byte)(b.On?1:0),(byte)0,flag):Ins(1000,3,(byte)n);}
            var e=new EMEVD.Event(id,EMEVD.Event.RestBehaviorType.Restart);e.Instructions.AddRange(Instructions);return e;
        }
    }
    static void AI(Code c,int id,bool on)=>c.Add(2004,1,id,(byte)(on?1:0));
    static void Enabled(Code c,int id,bool on)=>c.Add(2004,5,id,(byte)(on?1:0));
    static void Invincible(Code c,int id,bool on)=>c.Add(2004,15,id,(byte)(on?1:0));
    static void Flag(Code c,int id,bool on)=>c.Add(2003,2,id,(byte)(on?1:0));
    static void Bar(Code c,int actor,int name,short slot,bool on)=>c.Add(2003,11,(sbyte)(on?1:0),actor,slot,name);
    static void Walls(Code c,IEnumerable<BossFogWalls.Binding> walls,bool on) {
        foreach(var w in walls){c.Add(2005,3,w.ObjectEntity,(byte)(on?1:0));if(on)c.Add(2006,2,w.SfxEntity);else c.Add(2006,1,w.SfxEntity,(byte)1);}
    }
    static void Done(Code c)=>c.Add(1000,4,(byte)0);
    static void Inside(Code c,sbyte condition,int region,bool inside=true)=>c.Add(3,2,condition,(byte)(inside?1:0),10000,region);
    static void Wait(Code c,sbyte group)=>c.Add(0,0,(sbyte)0,(byte)1,group);
    static void Prefix(EMEVD.Event e,IReadOnlyList<EMEVD.Instruction> code){foreach(var p in e.Parameters)p.InstructionIndex+=code.Count;e.Instructions.InsertRange(0,code);}
    internal static void VerifyReservations(string root) {
        var flags=ReservedFlags.ToHashSet();var events=Enumerable.Range(FirstEvent,30).ToHashSet();
        void Scan(ReadOnlySpan<byte> bytes,string owner){for(int n=0;n+4<=bytes.Length;n++){int v=BitConverter.ToInt32(bytes.Slice(n,4));Require(!flags.Contains(v),"native flag reservation occupied "+v+" in "+owner);}}
        foreach(var f in BND4.Read(Path.Combine(root,"param/gameparam/gameparam.parambnd.dcx")).Files)Scan(f.Bytes.Span,f.Name);
        foreach(string path in Directory.EnumerateFiles(Path.Combine(root,"event"),"*.dcx")) {
            var bytes=File.ReadAllBytes(path);var raw=DCX.Is(bytes)?DCX.Decompress(bytes).ToArray():bytes;
            if(EMEVD.Is(raw)){var script=EMEVD.Read(raw);Require(!script.Events.Any(e=>events.Contains(checked((int)e.ID))),"native event reservation occupied in "+path);}
            Scan(raw,path);
        }
        foreach(string path in Directory.EnumerateFiles(Path.Combine(root,"script/talk"),"*.dcx"))foreach(var f in BND4.Read(path).Files)Scan(f.Bytes.Span,path+":"+f.Name);
    }
    internal static Result Apply(BossEncounterSource.Plan plan,MSBB destination,IReadOnlyList<MapEnemySpawns.Spawn> spawns,
        Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        Func<string,byte[]?> sourceRead,Func<string,byte[]?> nativeRead,Func<string,byte[]?> composedRead,
        byte[] scriptBytes,string targetMap,string nativeRoot,string definitions,bool rewards) {
        Require(plan.Profile=="DS1R"&&plan.Map=="m10_01_00_00"&&plan.Encounters.Count==3,"unsupported source plan");
        VerifyReservations(nativeRoot);
        var script=EMEVD.Read(scriptBytes);Require(script.Format==EMEVD.Game.Bloodborne,"wrong target VM");
        Require(!script.Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+30),"encounter events already present");
        var files=new Dictionary<string,byte[]>(StringComparer.Ordinal);byte[] Read(string p)=>files.GetValueOrDefault(p)??composedRead(p)??throw new FileNotFoundException("Boss dependency unavailable",p);
        var requests=new List<BossFogWalls.Wall>();var bindings=new List<Encounter>();int nextRegion=4032800,nextWall=0;
        var used=destination.Parts.GetEntries().Select(p=>p.EntityID).Concat(destination.Events.GetEntries().Select(p=>p.EntityID)).Concat(destination.Regions.GetEntries().Select(p=>p.EntityID)).ToHashSet();
        int Region(BossEncounterSource.Region from,string name) {
            int id=nextRegion++;Require(used.Add(id),"region namespace occupied");
            MSB.Shape shape=from.Shape switch {"Box"=>new MSB.Shape.Box{Width=from.Dimensions.X,Height=from.Dimensions.Y,Depth=from.Dimensions.Z},"Cylinder"=>new MSB.Shape.Cylinder{Radius=from.Radius,Height=from.Dimensions.Y},_=>throw new NotSupportedException("Boss region shape: "+from.Shape)};
            destination.Regions.Regions.Add(new(){Name="souls_boss_"+name,EntityID=id,Position=from.Position+offset,Rotation=from.Rotation,Shape=shape});return id;
        }
        MSBB.Part.Enemy Actor(BossEncounterSource.Actor from) {
            var report=spawns.Single(s=>s.SourceEntityID==from.EntityID);return destination.Parts.Enemies.Single(p=>p.EntityID==report.EntityID);
        }
        var pending=new List<(BossEncounterSource.Encounter Source,MSBB.Part.Enemy Boss,MSBB.Part.Enemy? Secondary,int[] Adds,int Start,int Arena,int Outside,int Inside,int Nest,int? SecondaryNest,int? Intro)>();
        foreach(var e in plan.Encounters) {
            var boss=Actor(e.Boss);var secondary=e.SecondaryBoss is {} s?Actor(s):null;
            // Taurus waits on his authored tower. Only his explicit entrance
            // clips carry translated source root motion and movement flags.
            if(e.Entrance is {} entrance) {
                Require(e.Key=="taurus"&&entrance.RootMotion!=null,"unsupported boss entrance");
                boss.Position=e.Boss.Position+offset;boss.InitAnimID=BossEnemyCharacters.TaurusWaitingAnimation;
            }
            if(secondary!=null&&e.SecondaryMotion is {} secondMotion){secondary.Position=secondMotion.LandingPosition+offset;secondary.Rotation=new(0,secondMotion.LandingYawDegrees,0);}
            foreach(var actor in new[]{boss,secondary}.Where(a=>a!=null))actor!.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,null,actor.Position)].Name;
            int start=Region(e.StartRegion,e.Key+"_start"),arena=Region(e.ArenaRegion,e.Key+"_arena");
            Require(e.EntryFog.Entry!=null&&e.EntryFog.RotateTarget!=null,"missing source fog interaction regions");
            int outside=Region(e.EntryFog.Entry!,e.Key+"_outside"),inside=Region(e.EntryFog.RotateTarget!,e.Key+"_inside");
            foreach(var wall in new[]{e.EntryFog}.Concat(e.ExitFogs)) {
                bool reverse=wall.Name==e.EntryFog.Name&&Vector3.Transform(e.EntryFog.Entry!.Position-wall.Position,Matrix4x4.CreateRotationY(-wall.Rotation.Y*MathF.PI/180)).Z<0;
                int index=nextWall++;requests.Add(new(wall.Name,wall.Position+offset,wall.Rotation,wall.Scale,4031800+index,4033800+index,
                    slots[MapCollisionBindings.ObjectSlot(chunks,wall.CollisionName,wall.Position+offset)].Name,wall.BoxHalfExtents,wall.BoxTranslation+wall.BoxBodyTranslation,wall.ConvexRadius,reverse));
            }
            // Arena boxes describe admission/battle bounds, not a safe home
            // coordinate: their origins can lie below the actual roof/bridge.
            int nest=Region(new(e.Key+"_nest",-1,e.Entrance?.RootMotion?.LandingPosition??boss.Position-offset,boss.Rotation,new(.1f,1,.1f)),e.Key+"_nest");
            int? secondaryNest=secondary==null?null:Region(new(e.Key+"_second_nest",-1,secondary.Position-offset,secondary.Rotation,new(.1f,1,.1f)),e.Key+"_second_nest");
            int? intro=e.Entrance==null?null:Region(e.Entrance.Trigger,e.Key+"_intro");
            pending.Add((e,boss,secondary,e.Adds.Select(a=>Actor(a).EntityID).ToArray(),start,arena,outside,inside,nest,secondaryNest,intro));
        }
        var fog=BossFogWalls.Apply(nativeRead,p=>files.GetValueOrDefault(p)??composedRead(p),destination,requests,targetMap);foreach(var pair in fog.Files)files.Add(pair.Key,pair.Value);
        var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);script.Events.Add(pre);}var ctor=script.Events.Single(e=>e.ID==0);var starts=new List<EMEVD.Instruction>();
        for(int index=0;index<pending.Count;index++) {
            var p=pending[index];var e=p.Source;int defeated=MapQuestState.Flag(e.DefeatFlag),entered=FirstFlag+index*4,active=entered+1,phase=entered+2,id=FirstEvent+index*10,name=900400+index;
            var walls=fog.Bindings.Where(w=>new[]{e.EntryFog.Name}.Concat(e.ExitFogs.Select(f=>f.Name)).Contains(w.SourceName)).ToArray();
            bindings.Add(new(e.Key,p.Boss.EntityID,p.Secondary?.EntityID,p.Adds,defeated,entered,active,phase,entered+3,p.Start,p.Arena,name,id,walls){IntroRegion=p.Intro});
            var reset=new Code();Flag(reset,entered,false);Flag(reset,active,false);Flag(reset,phase,false);Bar(reset,p.Boss.EntityID,name,0,false);if(p.Secondary!=null)Bar(reset,p.Secondary.EntityID,name,1,false);Prefix(pre,reset.Instructions);
            void Start(Code c,int eventID){script.Events.Add(c.Event(eventID));starts.Add(Ins(2000,0,0,(uint)eventID,0u));}
            var c=new Code();c.Jump("defeated",defeated);AI(c,p.Boss.EntityID,false);Invincible(c,p.Boss.EntityID,true);Enabled(c,p.Boss.EntityID,true);
            if(p.Intro!=null)c.Add(2003,18,p.Boss.EntityID,BossEnemyCharacters.TaurusWaitingAnimation,(byte)1,(byte)0,(byte)0);
            foreach(int add in p.Adds){AI(c,add,false);Invincible(c,add,true);Enabled(c,add,true);}if(p.Secondary!=null){AI(c,p.Secondary.EntityID,false);Enabled(c,p.Secondary.EntityID,false);}
            // Admission is earned by the action/rotation controller reaching
            // the source inside region. Merely appearing inside is insufficient.
            Walls(c,walls,true);c.Add(3,0,(sbyte)0,(byte)1,(byte)0,entered);
            if(p.Intro is {} introRegion) {
                Invincible(c,p.Boss.EntityID,false);Inside(c,-1,introRegion);c.Add(4,1,(sbyte)-1,p.Boss.EntityID,10000);Wait(c,-1);
                Flag(c,phase,true);c.Add(2003,18,p.Boss.EntityID,BossEnemyCharacters.TaurusJumpAnimation,(byte)0,(byte)0,(byte)0);
                // The donor's special state can replay before its completion
                // notification. End this explicit entrance on the checked
                // source clip boundary, then release the native controller.
                c.Add(1001,0,e.Entrance!.RootMotion!.Duration);c.Add(2004,43,p.Boss.EntityID,(byte)0);
            }
            Flag(c,active,true);if(e.Key=="capra")Flag(c,MapQuestState.Flag(e.ProgressionFlag),true);Invincible(c,p.Boss.EntityID,false);AI(c,p.Boss.EntityID,true);Bar(c,p.Boss.EntityID,name,0,true);
            foreach(int add in p.Adds){Invincible(c,add,false);AI(c,add,true);}c.Add(2004,13,p.Boss.EntityID,p.Nest);c.Add(2004,20,p.Boss.EntityID);Done(c);
            c.Label("defeated");Enabled(c,p.Boss.EntityID,false);if(p.Secondary!=null)Enabled(c,p.Secondary.EntityID,false);foreach(int add in p.Adds)Enabled(c,add,false);Walls(c,walls,false);Done(c);Start(c,id);
            var death=new Code();death.Add(1003,2,(byte)0,(byte)1,(byte)0,defeated);death.Add(3,0,(sbyte)1,(byte)1,(byte)0,active);death.Add(4,0,(sbyte)1,p.Boss.EntityID,(byte)1);if(p.Secondary!=null)death.Add(4,0,(sbyte)1,p.Secondary.EntityID,(byte)1);Wait(death,1);
            Flag(death,defeated,true);Flag(death,active,false);Flag(death,entered,false);Bar(death,p.Boss.EntityID,name,0,false);if(p.Secondary!=null)Bar(death,p.Secondary.EntityID,name,1,false);Walls(death,walls,false);foreach(int add in p.Adds)Enabled(death,add,false);
            death.Add(2003,12,p.Boss.EntityID);Done(death);Start(death,id+1);
            if(rewards) {
                // This controller also runs on reload after an interrupted clear.
                // Its completion bit is persistent and is never reset above.
                var reward=new Code();reward.Add(1003,2,(byte)0,(byte)1,(byte)0,entered+3);reward.Add(3,0,(sbyte)0,(byte)1,(byte)0,defeated);
                if(e.Rewards.Immediate is {} immediate)reward.Add(2003,4,checked(90000000+immediate.ID));
                if(e.Rewards.Delayed.Count>0){reward.Add(1001,0,e.Rewards.DelaySeconds);foreach(var lot in e.Rewards.Delayed)reward.Add(2003,4,checked(90000000+lot.ID));}
                Flag(reward,entered+3,true);Done(reward);Start(reward,id+4);
            }
            var traverse=new Code();traverse.Add(1003,2,(byte)0,(byte)1,(byte)0,defeated);Inside(traverse,1,p.Outside);traverse.Add(3,24,(sbyte)1,BossFogWalls.ActionButtonID,walls.Single(w=>w.SourceName==e.EntryFog.Name).ObjectEntity);Wait(traverse,1);
            traverse.Add(2004,14,10000,p.Inside,101130,(byte)0);Inside(traverse,-1,p.Start);traverse.Add(1,0,(sbyte)-1,2f);Wait(traverse,-1);Inside(traverse,2,p.Start);traverse.Add(1000,2,(byte)1,(byte)0,(sbyte)2);Flag(traverse,entered,true);traverse.Add(1000,4,(byte)1);Start(traverse,id+2);
            if(p.Secondary!=null) {
                var second=new Code();second.Add(1003,2,(byte)0,(byte)1,(byte)0,defeated);second.Add(3,0,(sbyte)1,(byte)1,(byte)0,active);second.Add(4,2,(sbyte)1,p.Boss.EntityID,(sbyte)5,e.SecondaryActivationRatio??throw new InvalidDataException("Missing phase threshold"));Wait(second,1);
                Enabled(second,p.Secondary.EntityID,true);AI(second,p.Secondary.EntityID,true);Invincible(second,p.Secondary.EntityID,false);second.Add(2004,13,p.Secondary.EntityID,p.SecondaryNest!.Value);second.Add(2004,20,p.Secondary.EntityID);Bar(second,p.Secondary.EntityID,name,1,true);Flag(second,phase,true);Done(second);Start(second,id+3);
            }
        }
        Prefix(ctor,starts);files["param/gameparam/gameparam.parambnd.dcx"]=Parameters(Read("param/gameparam/gameparam.parambnd.dcx"),pending.Select((p,i)=>(p.Source,p.Boss.EntityID,p.Secondary?.EntityID,Name:900400+i)).ToArray(),definitions);
        Names(plan,sourceRead,p=>files.GetValueOrDefault(p)??composedRead(p),nativeRoot,files);
        var bytes=script.Write(DCX.Type.DCX_DFLT_10000_44_9);_=MSBB.Read(destination.Write());_=EMEVD.Read(bytes);
        return new(bytes,files,bindings,[..fog.Limits,"Offline native encounter controllers, health bars, source base HP and boss-clear souls. Native damage, movement, hitboxes, combat actions and status effects remain donor approximations.","Taurus waits at his source tower until fog admission and the source bridge/damage trigger, plays the explicit source-motion entrance, then releases combat. Source intro cutscenes remain unconverted. The second Gargoyle uses its checked source landing position and begins at the authored60% threshold; both must die. Tail cutting, source fire projectiles, multiplayer and boss music remain unconverted.","Fog admission uses the native action/rotation animation gated by the source outside region. Failure to reach the inside region does not set admission. Per-load transient flags reset while defeat flags persist; gameplay validation is required.","Reward goods use the existing Humanity→Insight consumable policy. The Key to Depths gains an inventory identity; the separate keyed door interaction is not converted. Equipment drops remain excluded. Ringing the upper bell remains separate from defeating Gargoyles."]);
    }
    static byte[] Parameters(byte[] bytes,IReadOnlyList<(BossEncounterSource.Encounter Source,int EntityID,int? Secondary,int Name)> encounters,string definitions) {
        var binder=BND4.Read(bytes);string defs=Path.GetFullPath(Path.Combine(definitions,"../../BB/Defs"));
        var file=binder.Files.Single(f=>Leaf(f.Name)=="GameAreaParam.param");var areas=PARAM.Read(file.Bytes);areas.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(defs,"GameAreaParam.xml")));
        var areaOpaque=ImportParam.Read(file.Bytes,"GAME_AREA_PARAM_ST",32,1);
        var npcFile=binder.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var npcs=PARAM.Read(npcFile.Bytes);npcs.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(defs,"NpcParam.xml")));
        var npcOpaque=ImportParam.Read(npcFile.Bytes,"NPC_PARAM_ST",388,5);var changedNpcs=new HashSet<int>();
        foreach(var e in encounters) {
            Require(!areas.Rows.Any(r=>r.ID==e.EntityID),"boss-clear reward row occupied");var row=new PARAM.Row(areas.Rows.First()){ID=e.EntityID,Name="Imported boss clear "+e.Source.Key};
            row.Cells.Single(c=>c.Def.InternalName=="bonusSoul_single").Value=checked((uint)e.Source.Rewards.SoloSouls);row.Cells.Single(c=>c.Def.InternalName=="bonusSoul_multi").Value=checked((uint)e.Source.Rewards.MultiplayerSouls);row.Cells.Single(c=>c.Def.InternalName=="humanityPointCountFlagIdTop").Value=-1;areas.Rows.Add(row);
            foreach(int entity in new[]{e.EntityID,e.Secondary??-1}.Where(x=>x>=0)) {
                // Model/entity roles have fixed append-only reservations.
                int param=(entity%1000) switch {800=>895000,801=>895100,804=>895300,805=>895400,_=>throw new InvalidDataException("Unexpected boss actor role")};
                var npc=npcs.Rows.Single(r=>r.ID==param);npc.Cells.Single(c=>c.Def.InternalName=="getSoul").Value=0u;npc.Cells.Single(c=>c.Def.InternalName=="nameId").Value=e.Name;changedNpcs.Add(param);
            }
        }
        var encodedAreas=ImportParam.Read(areas.Write(),"GAME_AREA_PARAM_ST",32,1);
        foreach(var e in encounters)ImportParam.Append(areaOpaque,areaOpaque.Rows.First(),e.EntityID,"Imported boss clear "+e.Source.Key,ImportParam.Payload(encodedAreas.Rows.Single(r=>r.ID==e.EntityID)));
        var encodedNpcs=ImportParam.Read(npcs.Write(),"NPC_PARAM_ST",388,5);
        foreach(int id in changedNpcs)npcOpaque.Rows.Single(r=>r.ID==id).Cells.Single().Value=ImportParam.Payload(encodedNpcs.Rows.Single(r=>r.ID==id));
        file.Bytes=ImportParam.Write(areaOpaque);npcFile.Bytes=ImportParam.Write(npcOpaque);return BossEnemyCharacters.NormalizeHealth(binder.Write(DCX.Type.DCX_DFLT_10000_44_9),definitions);
    }
    static void Names(BossEncounterSource.Plan plan,Func<string,byte[]?> source,Func<string,byte[]?> composed,string root,Dictionary<string,byte[]> files) {
        string? Name(string language,int id){var b=source("msg/"+language+"/item.msgbnd.dcx");if(b==null)return null;return FMG.Read(BND3.Read(b).Files.Single(f=>f.ID==18).Bytes).Entries.SingleOrDefault(e=>e.ID==id)?.Text;}
        foreach(string directory in Directory.EnumerateDirectories(Path.Combine(root,"msg"))) {
            string language=Path.GetFileName(directory),path="msg/"+language+"/item.msgbnd.dcx";var input=composed(path);if(input==null)continue;var binder=BND4.Read(input);var file=binder.Files.Single(f=>f.ID==18);var fmg=FMG.Read(file.Bytes);
            for(int i=0;i<plan.Encounters.Count;i++){var e=plan.Encounters[i];int id=900400+i;Require(!fmg.Entries.Any(n=>n.ID==id),"boss name namespace occupied");string text=Name(MapAreaName.SourceLanguage(language,"DS1R")??"ENGLISH",e.NameID)??Name("ENGLISH",e.NameID)??throw new InvalidDataException("Missing source boss name");fmg.Entries.Add(new(fmg,id,text));}
            file.Bytes=fmg.Write();files[path]=binder.Write(binder.Compression);
        }
    }
}
