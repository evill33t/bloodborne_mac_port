// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

// A read-only plan for the three verified Burg/Parish encounters. This is deliberately
// separate from target AI, rewards, flags and event emission: a source opcode is
// not assumed to have the same contract in Bloodborne.
internal static class BossEncounterSource {
    internal const string SourceMap="m10_01_00_00";
    internal record Asset(string Path,string SHA256);
    internal record SourceEvent(string Path,long ID,string SHA256,int Instructions,int Parameters);
    internal record Actor(string Name,string Model,int EntityID,int NPCParamID,int ThinkParamID,int InitAnimID,
        Vector3 Position,Vector3 Rotation,Vector3 Scale,string CollisionName) {
        public IReadOnlyList<int> DisplayMasks {get;init;}=Array.Empty<int>();
    }
    // Dimensions use (width,height,depth), matching MSB Box fields rather than
    // the order in which the fields happen to be serialized.
    internal record Region(string Name,int EntityID,Vector3 Position,Vector3 Rotation,Vector3 Dimensions) {
        public string Shape {get;init;}="Box";
        public float Radius {get;init;}
    }
    internal record Fog(string Name,string Model,int EntityID,Vector3 Position,Vector3 Rotation,Vector3 Scale,
        string CollisionName,int SFXEntityID,int SourceEffectID,Vector3 SFXPosition,Vector3 SFXRotation,
        Vector3 BoxHalfExtents,Vector3 BoxTranslation,float ConvexRadius,Region? Entry,Region? RotateTarget) {
        // Authored rigid-body translation, applied after the child translation
        // and before the object MSB transform (rotation is identity here).
        public Vector3 BoxBodyTranslation {get;init;}=new(.021964788f,0,-.013287157f);
    }
    internal record Motion(string Clip,string SHA256,float Duration,Vector3 LocalDisplacement,Vector3 LandingPosition) {
        public float YawDeltaRadians {get;init;}
        public float LandingYawDegrees {get;init;}
    }
    internal record Intro(int IdleAnimation,int EnterAnimation,Region Trigger,Region TowerTop,Region TowerBottom,
        int JumpDownMessage,int JumpUpMessage) {
        public Motion? RootMotion {get;init;}
    }
    internal record LotOutcome(int Category,int ItemID,int Count,int Weight);
    internal record ItemLot(int ID,int CollectionFlag,IReadOnlyList<LotOutcome> Outcomes);
    internal record Reward(int GameAreaParamID,int SoloSouls,int MultiplayerSouls,int SourceNPCSouls,
        ItemLot? Immediate,IReadOnlyList<ItemLot> Delayed,float DelaySeconds,IReadOnlyList<ItemLot> RandomDrops);
    internal record Encounter(string Key,string Name,Actor Boss,IReadOnlyList<Actor> Adds,IReadOnlyList<Actor> SuppressedDuplicates,
        Fog EntryFog,IReadOnlyList<Fog> ExitFogs,Region StartRegion,Region ArenaRegion,Intro? Entrance,
        int DefeatFlag,int EnteredFlag,int ActiveFlag,int ProgressionFlag,int MusicEntityID,int NameID,
        Reward Rewards,IReadOnlyList<long> EventIDs) {
        public Actor? SecondaryBoss {get;init;}
        public float? SecondaryActivationRatio {get;init;}
        public Region? SecondaryStartRegion {get;init;}
        public int? SecondaryIdleAnimation {get;init;}
        public int? SecondaryEnterAnimation {get;init;}
        public Motion? SecondaryMotion {get;init;}
        public Actor? CutsceneActor {get;init;}
        public int? CutsceneID {get;init;}
        public Region? CutsceneTrigger {get;init;}
        public IReadOnlyList<Actor> DetachedTailActors {get;init;}=Array.Empty<Actor>();
        public ItemLot? TailCutReward {get;init;}
        public IReadOnlyDictionary<int,IReadOnlyList<ItemLot>> IndividualDeathDrops {get;init;}=
            new System.Collections.ObjectModel.ReadOnlyDictionary<int,IReadOnlyList<ItemLot>>(new Dictionary<int,IReadOnlyList<ItemLot>>());
        public IReadOnlyDictionary<int,IReadOnlyList<int>> ActorDisplayMasks {get;init;}=
            new System.Collections.ObjectModel.ReadOnlyDictionary<int,IReadOnlyList<int>>(new Dictionary<int,IReadOnlyList<int>>());
    }
    internal record Plan(string Profile,string Map,IReadOnlyList<Encounter> Encounters,IReadOnlyList<Asset> Assets,
        IReadOnlyList<SourceEvent> SourceEvents,IReadOnlyList<string> Limitations);

    const string MapScript="event/m10_01_00_00.emevd.dcx",CommonScript="event/common.emevd.dcx";
    // Fingerprints cover instruction bank/id, exact argument bytes, layers,
    // event rest behavior and every parameter patch. They are intentionally
    // narrower than the whole source EMEVD, allowing unrelated source events.
    static readonly IReadOnlyDictionary<long,string> Contracts=new Dictionary<long,string> {
        [11015380]="8be3d79cfb0510543d345b463d5e2f85704b939a60b886e179c78d23eb3854f5",
        [11015381]="ba7fbf1c53c3789666948165538fb6407391b54a844dbdf79b1341b43b2b343b",
        [11015383]="d707c82fd421a86d244f13cddb6f63e436d0bfdc04e355015e1e773a29c2ff92",
        [11015382]="2cbe843772fea31440053fdd4a5bf47dcc1d40165259b6179c979509d60ba89d",
        [11015384]="059e09dad717b92af3fc688c2c58a123b97548c0e8cbbc0e4129874346847b82",
        [11015385]="bf9ce673ebabbb4d978825c2ed583e1e8e09f1f4ca496c62a69bfb7b2025050f",
        [11015386]="49d6dab108d37b61cd031fe02bea004ecd1945f3ff44ff38b26731d342669434",
        [11010901]="2a824a45a709f99940ace9b88b2507751ac4189b7b708aad9aa63187a921c478",
        [11015370]="722a0acea8c521d8c092d42e32874f668053a94cda63865569c2526557bf9bd3",
        [11015371]="1975857ddb3ad327692c456d351eb14400ea2a93ade67c984520b82a353bc5b5",
        [11015373]="e9dae2eeff6e873c83ccb219a149061ba32e198abbfebc9e9e012d82bba41c68",
        [11015372]="b7fe8aa6abeb0514a71afea49dfea0730e9f85ba61fada0d8f7ccf7a8c742671",
        [11015374]="c4719ab1cb56af668cd07f7890c4540de52df2e19c1b9efcd477007b9bb91c28",
        [11015375]="8821e411f3ee19c28954cbd6001a6bec91919e203529211ccb561204d5d10ab2",
        [11010902]="982cf0b9a709fc03465dfe28b86690c5419404e96f9d296e0a3cce8da174bb05",
        [11015843]="4461048bb0f1fa90d9c2f04af57748953c899e3dd7e54ce3bec9394e17c795d2",
        [11015846]="05e6891d8a7a5e6c08f397dd34ed14d5dffc2ecacbc00a9b54892b7039a18105",
        [11010000]="1d7dcc2622f9a8d71ddb952517b5ff5d838546d5350134f86fd47f2b0f0c9c9c",
        [11015390]="8961774684e3396a6f268404f0facb8da589b2bcadcfa109cad68071e1168ef1",
        [11015391]="7bd58ed9fd4aaca291f04144a166341ad009da09cb725ad2671297a16eb7be19",
        [11015393]="b7d50e9e211d5eada6e8d30d8a9207915e4b202e5fee1e9aebc8a7c1b93a6d72",
        [11015392]="1661f0127d045093a0a7d6135839b22569dc560ff48b014d7f5be35754afd9ac",
        [11010001]="6bc428266de41438025df4aa369c01fa69b3c453b9be549fe9acd411b974d34e",
        [11015394]="276ea575da0a1e56b5549249b6db03735a482000a4d00f0c2a9d37c3b12ad21a",
        [11015395]="c9d76efa948fcfbbf5746bb3a6387d9abca0d7f2b6d0138a62b110ba59f1ce1a",
        [11015396]="3f798c113e94e12a1ed66289c26696ca2174b77a5f26b9ab39c7cb53cd1bc426",
        [11015397]="1a62551bb206429bc30e2458e5a9b50757179c1834e30048d98e1711813d38fa",
        [11015398]="9effab867b39dd783b0e156896d8413e9fc83fab75c9eec3382103418ed42897",
        [11010750]="a379ec607ff4beeea9947066e4fec0eb8cc2ef1480244e5d824e2f5145f8c22b"
    };
    static void Require(bool value,string detail){if(!value)throw new InvalidDataException("Burg boss source contract: "+detail);}
    static string Hash(byte[] bytes)=>Convert.ToHexStringLower(SHA256.HashData(bytes));
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static bool Near(Vector3 a,Vector3 b)=>Vector3.DistanceSquared(a,b)<1e-10f;

    internal static string Fingerprint(EMEVD.Event e) {
        using var stream=new MemoryStream();using var w=new BinaryWriter(stream);
        w.Write(e.ID);w.Write((uint)e.RestBehavior);w.Write(e.Instructions.Count);w.Write(e.Parameters.Count);
        foreach(var i in e.Instructions){w.Write(i.Bank);w.Write(i.ID);w.Write(i.Layer is {} layer?(long)layer:-1L);w.Write(i.ArgData.Length);w.Write(i.ArgData);}
        foreach(var p in e.Parameters){w.Write(p.InstructionIndex);w.Write(p.TargetStartByte);w.Write(p.SourceStartByte);w.Write((long)p.ByteCount);w.Write((long)p.UnkID);}
        return Hash(stream.ToArray());
    }
    static EMEVD.Event Unique(EMEVD script,long id) {
        var matches=script.Events.Where(e=>e.ID==id).ToArray();Require(matches.Length==1,"missing or repeated event "+id);return matches[0];
    }
    static void Call(EMEVD.Event constructor,params int[] args) {
        var raw=args.SelectMany(BitConverter.GetBytes).ToArray();
        Require(constructor.Instructions.Count(i=>i.Bank==2000&&i.ID==0&&i.Layer==null&&i.ArgData.AsSpan().SequenceEqual(raw))==1,
            "missing or repeated constructor call "+args[1]+" slot "+args[0]);
    }

    internal static Plan Load(Func<string,byte[]?> read,string profile,string map,string definitions) {
        if(profile!="DS1R"||map!=SourceMap)throw new NotSupportedException("Boss encounter source supports only verified DS1R m10_01_00_00");
        var assets=new List<Asset>();
        byte[] Read(string path) {
            var bytes=read(path)??throw new FileNotFoundException("Boss encounter source asset unavailable",path);
            Require(bytes.Length is >0 and <=64*1024*1024,"asset exceeds the read budget: "+path);assets.Add(new(path,Hash(bytes)));return bytes;
        }
        var msb=MSB1.Read(Read("map/MapStudio/"+map+".msb"));
        var script=EMEVD.Read(Read(MapScript));var common=EMEVD.Read(Read(CommonScript));
        Require(script.Format==EMEVD.Game.DarkSouls1&&common.Format==EMEVD.Game.DarkSouls1,"source EMEVD format changed");
        var sourceEvents=new List<SourceEvent>();
        foreach(var (id,expected) in Contracts) {
            var e=Unique(script,id);string actual=Fingerprint(e);Require(actual==expected,"event "+id+" instructions or parameters changed");
            sourceEvents.Add(new(MapScript,id,actual,e.Instructions.Count,e.Parameters.Count));
        }
        var rewards=Unique(common,970);string rewardHash=Fingerprint(rewards);
        Require(rewardHash=="8ff621939eba162f7c0e891a7f566c0dd5d31f74127d2c09cc3f2b0b2359b74b","common reward event970 changed");
        sourceEvents.Add(new(CommonScript,970,rewardHash,rewards.Instructions.Count,rewards.Parameters.Count));
        var ctor=Unique(script,0);var commonCtor=Unique(common,0);
        var bossBranches=new EMEVD.Event(0,ctor.RestBehavior);
        bossBranches.Instructions.AddRange(ctor.Instructions.Skip(69).Take(52).Concat(ctor.Instructions.Skip(177).Take(6)));
        Require(ctor.Parameters.Count==0&&Fingerprint(bossBranches)=="50b66bcfd12601444bceff487a1afc8db9d145d9693b8720728fbf80b75b4360","boss constructor state branches changed");
        foreach(int id in Contracts.Keys.Where(id=>id is not(11015843 or 11015846 or 11010750 or 11015397 or 11015398)).Select(id=>checked((int)id))) {
            int expected=id is 11010901 or 11010902 or 11015392?2:1;
            var raw=new[]{0,id,0}.SelectMany(BitConverter.GetBytes).ToArray();
            Require(ctor.Instructions.Count(i=>i.Bank==2000&&i.ID==0&&i.Layer==null&&i.ArgData.AsSpan().SequenceEqual(raw))==expected,"boss constructor changed: "+id);
        }
        Call(ctor,0,11015846,11010901,1011890,1011891);Call(ctor,0,11015843,11010901,1011890,1012898,1012897);
        Call(ctor,1,11015846,11010902,1011790,1011791);Call(ctor,1,11015843,11010902,1011790,1012888,1012887);
        Call(ctor,2,11015846,3,1011990,1011991);Call(ctor,2,11015843,3,1011990,1012998,1012997);
        Call(ctor,0,11010750,1010800,53500000);Call(ctor,1,11010750,1010801,53500100);
        Call(ctor,0,11015397,1010800,5350,5350,1010810);Call(ctor,0,11015398,1010801,1010811);
        Call(commonCtor,1,970,11010901,0,9000,9030);Call(commonCtor,2,970,11010902,2510,9000,9030);
        Call(commonCtor,3,970,3,0,9020,0);

        Actor ActorAt(string name,string model,int entity,int npc,int think,Vector3 position,Vector3 rotation,string collision) {
            var found=msb.Parts.Enemies.Where(p=>p.Name==name||p.EntityID==entity).ToArray();
            Require(found.Length==1,"missing or repeated actor "+name);var p=found[0];
            Require(p.Name==name&&p.ModelName==model&&p.EntityID==entity&&p.NPCParamID==npc&&p.ThinkParamID==think&&p.InitAnimID==-1&&
                p.CharaInitID==-1&&p.TalkID==0&&p.CollisionName==collision&&Near(p.Position,position)&&Near(p.Rotation,rotation)&&Near(p.Scale,Vector3.One),"actor identity or transform changed: "+name);
            return new(p.Name,p.ModelName,p.EntityID,p.NPCParamID,p.ThinkParamID,p.InitAnimID,p.Position,p.Rotation,p.Scale,p.CollisionName);
        }
        Region Box(int id,Vector3 position,Vector3 rotation,Vector3 dimensions) {
            var found=msb.Regions.GetEntries().Where(p=>p.EntityID==id).ToArray();Require(found.Length==1,"missing or repeated region "+id);var r=found[0];
            Require(r.Shape is MSB.Shape.Box b&&Near(new(b.Width,b.Height,b.Depth),dimensions)&&Near(r.Position,position)&&Near(r.Rotation,rotation),"region shape or transform changed: "+id);
            return new(r.Name,r.EntityID,r.Position,r.Rotation,dimensions);
        }
        Region Cylinder(int id,Vector3 position,Vector3 rotation,float radius,float height) {
            var found=msb.Regions.GetEntries().Where(p=>p.EntityID==id).ToArray();Require(found.Length==1,"missing or repeated region "+id);var r=found[0];
            Require(r.Shape is MSB.Shape.Cylinder c&&c.Radius==radius&&c.Height==height&&Near(r.Position,position)&&Near(r.Rotation,rotation),"cylinder region shape or transform changed: "+id);
            return new(r.Name,r.EntityID,r.Position,r.Rotation,new(radius*2,height,radius*2)){Shape="Cylinder",Radius=radius};
        }
        var taurus=ActorAt("c2250_0000","c2250",1010700,225000,225000,new(-6.7f,26.98f,-110.4f),new(0,-65,0),"h1117B1");
        var capra=ActorAt("c2240_0000","c2240",1010750,224000,224001,new(-81.43f,-43.57f,-1.49f),new(0,-35,0),"h1122B1");
        var dogs=new[]{ActorAt("c3340_0003","c3340",1010500,334003,334000,new(-77.653f,-43.569f,-1.393f),new(.169f,25,0),"h1122B1"),
            ActorAt("c3340_0004","c3340",1010501,334003,334000,new(-85.19f,-38.08f,-.87f),new(0,-35,0),"h1122B1")};
        var duplicateDogs=new[]{ActorAt("c3340_0007","c3340",1010510,334000,334000,dogs[0].Position,dogs[0].Rotation,"h1122B1"),
            ActorAt("c3340_0008","c3340",1010511,334000,334000,dogs[1].Position,dogs[1].Rotation,"h1122B1")};
        var gargoyle=ActorAt("c5350_0000","c5350",1010800,535000,535000,new(11,48.88f,105),Vector3.Zero,"h1034B1");
        var secondGargoyle=ActorAt("c5350_0001","c5350",1010801,535001,535001,new(15.04f,69.76f,132.1f),new(0,-45,0),"h1034B1");
        var cutsceneGargoyle=ActorAt("c5350_0002","c5350",1010802,535000,535000,new(6.14f,69.76f,132.40002f),new(0,45,0),"h1034B1");
        var tails=new[]{ActorAt("c5352_0000","c5352",1010810,535200,1,new(11.11f,52.88f,108.07f),Vector3.Zero,"h1034B1"),
            ActorAt("c5352_0001","c5352",1010811,535200,1,new(11.11f,52.88f,108.07f),Vector3.Zero,"h1034B1")};
        var taurusEntry=Box(1012898,new(52.41f,16.86f,-118.49f),new(0,-10,0),new(1.5f,1,1.5f));
        var taurusTarget=Box(1012897,new(46.910046f,16.86f,-119.28999f),Vector3.Zero,new(.1f,1,.1f));
        var taurusStart=Box(1012896,new(50.11f,16,-118.84f),new(0,-10,0),new(2,2,1.5f));
        var taurusArena=Box(1012890,new(30.341164f,8.699534f,-114.76858f),new(0,10,0),new(79,29,49));
        var taurusIntro=Box(1012701,new(22.8f,15.16f,-120.7f),new(0,7,0),new(2,4,7.5f));
        var taurusTop=Box(1012740,new(53.633f,23,-118.23f),new(0,79,0),new(2,10,2));
        var taurusBottom=Box(1012741,new(47.101f,15,-119.274f),new(0,-101,0),new(2,7,2));
        var capraEntry=Box(1012888,new(-72.25f,-44,-17.2f),new(0,150,0),new(1.7f,1,1));
        var capraTarget=Box(1012887,new(-73.3f,-43.67f,-15.25f),new(0,58,0),new(.1f,1,.1f));
        var capraStart=Box(1012886,new(-73.15f,-44,-15.5f),new(0,150,0),new(4,1,2));
        var capraArena=Box(1012880,new(-77.73f,-43.67f,-8.87f),new(0,58,0),new(33,12,13));
        var gargoyleEntry=Box(1012998,new(17.34f,47.7f,73.47f),Vector3.Zero,new(1.3f,.5f,1.3f));
        var gargoyleTarget=Cylinder(1012997,new(17.24f,47.82f,76.34f),Vector3.Zero,.1f,.5f);
        var gargoyleStart=Box(1012996,new(17.34f,46,75.47f),Vector3.Zero,new(1.5f,2.5f,2));
        var gargoyleArena=Box(1012990,new(9.41f,45,107.9f),Vector3.Zero,new(30,20,70));
        var gargoyleCutscene=Cylinder(1012999,new(11,45.16f,107),Vector3.Zero,20,10);
        var secondGargoyleStart=Box(1012650,secondGargoyle.Position,secondGargoyle.Rotation,new(.1f,1,.1f));

        Fog FogAt(string name,string model,int entity,int sfx,int effect,Vector3 position,Vector3 rotation,string collision,
            Vector3 sfxPosition,Vector3 sfxRotation,Vector3 half,Vector3 shift,string objectHash,Region? entry,Region? target,
            float convexRadius=.025f,Vector3? bodyTranslation=null) {
            var found=msb.Parts.Objects.Where(p=>p.Name==name||p.EntityID==entity).ToArray();Require(found.Length==1,"missing or repeated fog object "+name);var o=found[0];
            Require(o.Name==name&&o.ModelName==model&&o.EntityID==entity&&o.InitAnimID==-1&&o.CollisionName==collision&&Near(o.Position,position)&&Near(o.Rotation,rotation)&&Near(o.Scale,Vector3.One),"fog object identity or transform changed: "+name);
            var effects=msb.Events.SFX.Where(e=>e.EntityID==sfx).ToArray();Require(effects.Length==1&&effects[0].EffectID==effect,"fog SFX identity changed: "+sfx);
            var regions=msb.Regions.GetEntries().Where(r=>r.Name==effects[0].RegionName).ToArray();
            Require(regions.Length==1&&regions[0].Shape is MSB.Shape.Point&&Near(regions[0].Position,sfxPosition)&&Near(regions[0].Rotation,sfxRotation),"fog SFX transform changed: "+sfx);
            Require(Hash(Read("obj/"+model+".objbnd.dcx"))==objectHash,"fog HKX box/empty visual contract changed: "+model);
            return new(name,model,entity,position,rotation,Vector3.One,collision,sfx,effect,sfxPosition,sfxRotation,half,shift,convexRadius,entry,target){BoxBodyTranslation=bodyTranslation??new(.021964788f,0,-.013287157f)};
        }
        var front=FogAt("o1405_0000","o1405",1011890,1011891,81005,new(51.41f,16.86f,-118.69f),new(0,-100,0),"h1117B1",
            new(51.41f,16.86f,-118.69f),new(0,80,0),new(1.2782525f,1.975f,.025f),new(0,2,0),"c66870802d8c996603df70115c2f958d24157a869805e1848ed1fc35d35da64e",taurusEntry,taurusTarget);
        var rear=FogAt("o1406_0000","o1406",1011892,1011893,81006,new(-3.94f,17.26f,-111.91f),new(0,-60,0),"h1117B1",
            new(-3.94f,17.26f,-111.91f),new(0,120,0),new(1.2782525f,1.975f,.025f),new(0,2,0),"42827c16b286559b4daea32753c3c02d0fe9c02c305cdb16d5142aaab5070121",null,null);
        var lower=FogAt("o1413_0000","o1413",1011790,1011791,81013,new(-72.75f,-43.57f,-16.5f),new(0,150,0),"h1122B1",
            new(-72.55f,-43.57f,-16.800001f),new(0,150,0),new(1.725f,2.725f,.025f),new(0,2.75f,0),"cb284172c0b17297b9bdfa32ed7d414b03dc43fd3459b6a943a0fd5c25220317",capraEntry,capraTarget);
        var roofFront=FogAt("o1403_0000","o1403",1011990,1011991,81003,new(17.36f,47.83f,74.37f),Vector3.Zero,"h1032B1",
            new(17.36f,47.83f,74.37f),Vector3.Zero,new(.8032558f,1.2581758f,.02218546f),new(0,1.0027673f,-4.4703484e-8f),
            "e00a6f3e39de38cfbe747e2618bc619d05075607d1ead717a8f78bae5452d388",gargoyleEntry,gargoyleTarget,0,Vector3.Zero);
        var roofRear=FogAt("o1404_0000","o1404",1011992,1011993,81004,new(7.09f,49.15f,131),Vector3.Zero,"h1034B1",
            new(7.09f,49.15f,131),Vector3.Zero,new(.8032558f,1.6805587f,.022185478f),new(0,1.4251502f,-6.3329935e-8f),
            "589b6de0fc8f88d15572b425acdb82393389591060acff29e1794dd4858fb49d",null,null,0,Vector3.Zero);

        // These constants were independently decoded from the typed source
        // referenceFrameSamples arrays. Check the exact embedded clip, rather
        // than silently applying them to a different animation revision.
        Motion RootMotion(Actor actor,int clipID,string expected,float duration,Vector3 displacement,float yaw) {
            var bank=BND3.Read(Read("chr/"+actor.Model+".anibnd.dcx"));string leaf=$"a00_{clipID:D4}.hkx";
            var files=bank.Files.Where(f=>Leaf(f.Name).Equals(leaf,StringComparison.OrdinalIgnoreCase)).ToArray();
            Require(files.Length==1&&Hash(files[0].Bytes.ToArray())==expected,"extracted root-motion clip changed: "+actor.Model+"/"+leaf);
            var r=actor.Rotation*(MathF.PI/180);
            var transform=Matrix4x4.CreateScale(actor.Scale)*Matrix4x4.CreateRotationX(r.X)*Matrix4x4.CreateRotationZ(r.Z)*Matrix4x4.CreateRotationY(r.Y);
            return new(leaf,expected,duration,displacement,actor.Position+Vector3.Transform(displacement,transform)) {
                YawDeltaRadians=yaw,LandingYawDegrees=actor.Rotation.Y+yaw*(180/MathF.PI)
            };
        }
        var taurusMotion=RootMotion(taurus,9061,"77da90629ac5c1e6f32af2dab2d0401046a550233fc7f4fcfbd576b8b8f65d63",4.1666665f,new(.22899437f,-11.460223f,-9.9610405f),0);
        var gargoyleMotion=RootMotion(secondGargoyle,7001,"58a3ce0289f787714186ee9ead310c7b3ba7ead5a03c4b21bf79be1e691d9e24",5.0666666f,new(-8.875716f,-20.813282f,-2.669593f),.9331883f);

        var game=BND3.Read(Read("param/GameParam/GameParam.parambnd.dcx"));
        PARAM Table(string name) {
            var files=game.Files.Where(f=>Leaf(f.Name)==name+".param").ToArray();Require(files.Length==1,"missing or repeated parameter "+name);
            var p=PARAM.Read(files[0].Bytes);p.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,name+".xml")));return p;
        }
        var npcs=Table("NpcParam");var areas=Table("GameAreaParam");var lots=Table("ItemLotParam");
        PARAM.Row Row(PARAM p,int id){var rows=p.Rows.Where(r=>r.ID==id).ToArray();Require(rows.Length==1,"missing or repeated parameter row "+p.ParamType+":"+id);return rows[0];}
        static int Number(PARAM.Row row,string field)=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName==field).Value);
        ItemLot Lot(int id) {
            var row=Row(lots,id);var outcomes=new List<LotOutcome>();
            for(int i=1;i<=8;i++){string suffix=i.ToString("D2");int weight=Number(row,"lotItemBasePoint"+suffix);if(weight==0)continue;
                Require(Number(row,"getItemFlagId"+suffix)==0,"unsupported conditioned lot outcome: "+id);
                outcomes.Add(new(Number(row,"lotItemCategory"+suffix),Number(row,"lotItemId"+suffix),Number(row,"lotItemNum"+suffix),weight));}
            return new(id,Number(row,"getItemFlagId"),outcomes.AsReadOnly());
        }
        var key=Lot(2510);var humanity=Lot(9000);var bone=Lot(9030);var twinHumanity=Lot(9020);
        Require(key.CollectionFlag==50001510&&key.Outcomes.SequenceEqual(new[]{new LotOutcome(0x40000000,2014,1,100)}),"Capra key reward changed");
        Require(humanity.CollectionFlag==-1&&humanity.Outcomes.SequenceEqual(new[]{new LotOutcome(0x40000000,500,1,100)})&&
            bone.CollectionFlag==-1&&bone.Outcomes.SequenceEqual(new[]{new LotOutcome(0x40000000,330,1,100)}),"shared consumable reward changed");
        Require(twinHumanity.CollectionFlag==-1&&twinHumanity.Outcomes.SequenceEqual(new[]{new LotOutcome(0x40000000,501,1,100)}),"Gargoyle clear reward changed");
        Actor WithMasks(Actor actor) {
            var row=Row(npcs,actor.NPCParamID);
            return actor with {DisplayMasks=Array.AsReadOnly(Enumerable.Range(0,16).Where(i=>Number(row,"modelDispMask"+i)!=0).ToArray())};
        }
        taurus=WithMasks(taurus);capra=WithMasks(capra);gargoyle=WithMasks(gargoyle);secondGargoyle=WithMasks(secondGargoyle);cutsceneGargoyle=WithMasks(cutsceneGargoyle);
        dogs=dogs.Select(WithMasks).ToArray();duplicateDogs=duplicateDogs.Select(WithMasks).ToArray();tails=tails.Select(WithMasks).ToArray();
        Require(gargoyle.DisplayMasks.SequenceEqual(new[]{0,1})&&secondGargoyle.DisplayMasks.SequenceEqual(new[]{2}),"Gargoyle primary/secondary display masks changed");
        Reward RewardFor(Actor actor,int souls,int drop,ItemLot? immediate) {
            var npc=Row(npcs,actor.NPCParamID);var area=Row(areas,actor.EntityID);
            Require(Number(npc,"getSoul")==0&&Number(npc,"itemLotId_1")==drop&&Enumerable.Range(2,5).All(i=>Number(npc,"itemLotId_"+i)==-1),"boss NPC reward changed: "+actor.EntityID);
            Require(Number(area,"bonusSoul_single")==souls&&Number(area,"bonusSoul_multi")==souls,"boss clear soul reward changed: "+actor.EntityID);
            var random=Lot(drop);int weapon=drop==22500000?751000:352000;
            Require(random.CollectionFlag==-1&&random.Outcomes.SequenceEqual(new[]{new LotOutcome(-1,0,0,95),new LotOutcome(0,weapon,1,5)}),"boss random weapon drop changed: "+drop);
            return new(actor.EntityID,souls,souls,0,immediate,Array.AsReadOnly(new[]{humanity,bone}),5,Array.AsReadOnly(new[]{random}));
        }
        foreach(var actor in new[]{gargoyle,secondGargoyle}) {
            var row=Row(npcs,actor.NPCParamID);
            Require(Number(row,"hp")==833&&Number(row,"getSoul")==0&&Enumerable.Range(1,6).All(i=>Number(row,"itemLotId_"+i)==-1),"Gargoyle actor HP or NPC loot routing changed");
        }
        var roofArea=Row(areas,gargoyle.EntityID);
        Require(Number(roofArea,"bonusSoul_single")==10000&&Number(roofArea,"bonusSoul_multi")==10000,"Gargoyle clear souls changed");
        ItemLot Chance(int id,int category,int item,int collection) {
            var lot=Lot(id);Require(lot.CollectionFlag==collection&&lot.Outcomes.SequenceEqual(new[]{new LotOutcome(-1,0,0,97),new LotOutcome(category,item,1,3)}),"Gargoyle individual death lot changed: "+id);return lot;
        }
        var primaryDrops=Array.AsReadOnly(new[]{Chance(53500000,0,1103000,-1),Chance(53500001,0,1478000,-1),Chance(53500002,0x10000000,630000,-2)});
        var secondaryDrops=Array.AsReadOnly(new[]{Chance(53500100,0,1103000,-1),Chance(53500101,0x10000000,630000,-2)});
        var tailReward=Lot(53520000);Require(tailReward.CollectionFlag==51010970&&tailReward.Outcomes.SequenceEqual(new[]{new LotOutcome(0,705000,1,100)}),"Gargoyle cut-tail reward changed");
        var deathDrops=new System.Collections.ObjectModel.ReadOnlyDictionary<int,IReadOnlyList<ItemLot>>(new Dictionary<int,IReadOnlyList<ItemLot>> {
            [gargoyle.EntityID]=primaryDrops,[secondGargoyle.EntityID]=secondaryDrops
        });
        var roofMasks=new System.Collections.ObjectModel.ReadOnlyDictionary<int,IReadOnlyList<int>>(new Dictionary<int,IReadOnlyList<int>> {
            [gargoyle.EntityID]=gargoyle.DisplayMasks,[secondGargoyle.EntityID]=secondGargoyle.DisplayMasks,[cutsceneGargoyle.EntityID]=cutsceneGargoyle.DisplayMasks
        });
        var encounters=new[]{
            new Encounter("taurus","Taurus Demon",taurus,[],[],front,Array.AsReadOnly(new[]{rear}),taurusStart,taurusArena,
                new(9001,9061,taurusIntro,taurusTop,taurusBottom,700,710){RootMotion=taurusMotion},11010901,11015383,11015382,11010005,1013802,2250,
                RewardFor(taurus,3000,22500000,null),Array.AsReadOnly(new long[]{11015380,11015381,11015383,11015382,11015384,11015385,11015386,11010901})),
            new Encounter("capra","Capra Demon",capra,Array.AsReadOnly(dogs),Array.AsReadOnly(duplicateDogs),lower,[],capraStart,capraArena,
                null,11010902,11015373,11015372,11010597,1013803,2240,RewardFor(capra,6000,22400000,key),
                Array.AsReadOnly(new long[]{11015370,11015371,11015373,11015372,11015374,11015375,11010902})),
            new Encounter("gargoyles","Bell Gargoyles",gargoyle,[],[],roofFront,Array.AsReadOnly(new[]{roofRear}),gargoyleStart,gargoyleArena,
                null,3,11015393,11015392,11010000,1013800,5350,
                new(gargoyle.EntityID,10000,10000,0,null,Array.AsReadOnly(new[]{twinHumanity}),5,[]),
                Array.AsReadOnly(new long[]{11010000,11015390,11015391,11015393,11015392,11010001,11015394,11015395,11015396,11015397,11015398,11010750})) {
                SecondaryBoss=secondGargoyle,SecondaryActivationRatio=.6f,SecondaryStartRegion=secondGargoyleStart,SecondaryIdleAnimation=7000,SecondaryEnterAnimation=7001,
                SecondaryMotion=gargoyleMotion,CutsceneActor=cutsceneGargoyle,CutsceneID=100110,CutsceneTrigger=gargoyleCutscene,
                DetachedTailActors=Array.AsReadOnly(tails),TailCutReward=tailReward,IndividualDeathDrops=deathDrops,ActorDisplayMasks=roofMasks
            }
        };
        return new(profile,map,Array.AsReadOnly(encounters),assets.AsReadOnly(),sourceEvents.AsReadOnly(),Array.AsReadOnly(new[]{
            "This plan emits no native AI, animation, fog, reward or event code. Native behavior and multiplayer require separate adapters.",
            "Entry/active source event flags are distinct from persistent boss-defeat and progression flags. Target attempt state must be reset explicitly on retry; source event flags are not copied into native global flags.",
            "Taurus source idle9001/jump9061, damage-or-region trigger and message700/710 tower warps require matching source animation/TAE support. Do not substitute guessed jump timing or coordinates.",
            "Checked extracted root motion describes authored clip displacement and endpoint before runtime physics/AI correction; it does not claim native playback applies that motion. The source clip and landing coordinates are recorded for an explicit target adapter.",
            "Capra primary dogs1010500/1010501 are removed on defeated reload; source duplicate dogs1010510/1010511 are disabled and killed on the fresh branch. This is not a four-dog encounter.",
            "Gargoyle primary/secondary actors use source display masks[0,1]/[2]; the second starts invulnerable with AI off until the first reaches60%HP, then uses source7001 landing motion. Clear flag3 requires both combatants dead and is separate from ringing the upper bell11010700.",
            "Gargoyle intro cutscene100110, its placeholder1010802, detachable tails1010810/1010811, cut-tail part damage/masks and loot are dependencies, not extra ordinary enemies or automatically converted interactions. Individual death equipment lots are separate from the overall TwinHumanities clear reward.",
            "Common970 rewards are observed only when a defeat changes from OFF to ON during that source event run; an already-defeated reload does not award them. Crash-safe target reward collection is a separate target contract.",
            "Source multiplayer scaling, phantom entry, boss sound/music and broader key/door/quest routing are recorded dependencies, not claims of converted behavior."
        }));
    }
}
