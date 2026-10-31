// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

// Additional destinations use their own lamp state in an existing native flag
// group. They do not borrow Central Yharnam's unlocks, checkpoint, or requests.
internal static class MapTravel {
    internal const int HeadstoneEntity=2100980,HeadstoneStone=2101980,HeadstoneTalk=210980,HeadstoneAvailable=52100599;
    // 500 is a retail ItemLot flag;600..899 belong to imported pickups. This
    // manifest owns exactly two20-bit native lamp records and nine requests/
    // access bits. Never renumber these once a copied mod save has used them.
    internal static readonly IReadOnlyList<int> ReservedFlags=Enumerable.Range(52100520,40)
        .Concat(Enumerable.Range(52100580,6)).Concat(new[]{52100590,52100591,HeadstoneAvailable}).ToArray();
    internal sealed record Destination(string SourceMap,string TargetMap,string Name,Vector3 Spawn,Vector3 LampPosition,
        Vector3 LampRotation,string? CollisionName=null,string? SourceBonfirePart=null,string? ReplacedMapPiece=null,string? SourceBonfireKind=null);
    internal sealed record Binding(string SourceMap,string TargetMap,string Name,int Controller,int Lamp,int Respawn,
        int Chair,int RegistrationBase,int ActiveFlag,int RegistrationRequest,int ReturnRequest,int ArrivalRequest,
        int DisabledFlag,int MenuText,Vector3 Spawn,Vector3 LampPosition,string Collision,string? SourceBonfirePart,string? ReplacedMapPiece,string? SourceBonfireKind);
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,IReadOnlyList<Binding> Destinations,
        IReadOnlyDictionary<string,string> NativeSources,IReadOnlyList<string> Limitations);
    static string Leaf(string path)=>Path.GetFileName(path.Replace('\\','/'));
    static byte[] Pack(params int[] args)=>args.SelectMany(BitConverter.GetBytes).ToArray();
    static EMEVD.Event AddRoot(EMEVD script,long id){var e=new EMEVD.Event(id);script.Events.Add(e);return e;}
    // Separate method keeps the missing-root path explicit without replacing a
    // caller's composed constructor/preconstructor or their event parameters.
    static EMEVD.Event EnsureRoot(EMEVD script,long id)=>script.Events.SingleOrDefault(e=>e.ID==id)??AddRoot(script,id);
    static void Start(EMEVD.Event ctor,int slot,int id,params int[] args)=>ctor.Instructions.Add(new(2000,0,Pack([slot,id,..args])));
    static void Flag(EMEVD.Event ctor,int flag,bool value)=>ctor.Instructions.Add(new(2003,2,Pack(flag,value?1:0)));
    // Native controllers use their authored backread groups; all bits set can
    // make the talk environment report an otherwise loaded actor as disabled.
    static void Groups(MSBB.Part part){Array.Fill(part.DrawGroups,uint.MaxValue);Array.Fill(part.DispGroups,uint.MaxValue);}
    static void Free(MSBB map,params int[] ids){var used=map.Parts.GetEntries().Select(p=>p.EntityID).Concat(map.Events.GetEntries().Select(e=>e.EntityID)).Concat(map.Regions.GetEntries().Select(r=>r.EntityID)).ToHashSet();if(ids.Any(used.Contains))throw new InvalidDataException("Destination travel entity reservation is occupied");}
    static byte[] WriteRows(PARAM rows){var nulls=rows.Rows.Select((r,i)=>(r.Name,i)).Where(x=>x.Name==null).Select(x=>x.i).ToArray();var bytes=rows.Write();foreach(int index in nulls)Array.Clear(bytes,64+index*24+16,8);return bytes;}
    static void ReservationScan(ReadOnlySpan<byte> bytes,string owner) {
        var reserved=ReservedFlags.ToHashSet();for(int at=0;at+4<=bytes.Length;++at){int value=System.Buffers.Binary.BinaryPrimitives.ReadInt32LittleEndian(bytes[at..]);if(reserved.Contains(value))throw new InvalidDataException("Destination travel flag is occupied: "+value+" ("+owner+")");}
    }
    internal static void VerifyReservations(string nativeRoot) {
        foreach(var file in BND4.Read(Path.Combine(nativeRoot,"param/gameparam/gameparam.parambnd.dcx")).Files)ReservationScan(file.Bytes.Span,file.Name);
        foreach(string folder in new[]{"event","script/talk"})foreach(string path in Directory.EnumerateFiles(Path.Combine(nativeRoot,folder),"*.dcx",SearchOption.AllDirectories)) {
            var bytes=File.ReadAllBytes(path);string owner=Path.GetRelativePath(nativeRoot,path);
            if(BND4.Is(bytes))foreach(var file in BND4.Read(bytes).Files)ReservationScan(file.Bytes.Span,owner+":"+file.Name);
            else ReservationScan(DCX.Is(bytes)?DCX.Decompress(bytes).Span:bytes,owner);
        }
        var owned=new HashSet<int>{HeadstoneEntity,HeadstoneStone,4020950,4021950,4022950,4030950,4031950,4032950};
        foreach(string path in Directory.EnumerateFiles(Path.Combine(nativeRoot,"map/MapStudio"),"*.msb*",SearchOption.AllDirectories)) {
            var raw=File.ReadAllBytes(path);var bytes=DCX.Is(raw)?DCX.Decompress(raw).ToArray():raw;bool possible=false;
            for(int at=0;at+4<=bytes.Length;at+=4)if(owned.Contains(System.Buffers.Binary.BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(at,4)))){possible=true;break;}
            if(!possible)continue;var map=MSBB.Read(bytes);var ids=map.Parts.GetEntries().Select(p=>p.EntityID).Concat(map.Events.GetEntries().Select(e=>e.EntityID)).Concat(map.Regions.GetEntries().Select(r=>r.EntityID));
            foreach(int id in ids)if(owned.Contains(id))throw new InvalidDataException("Destination travel entity is occupied by native data: "+id+" ("+path+")");
        }
    }
    static void CommonLink(EMEVD script,EMEVD donor) {
        byte[] PathAt(EMEVD e,long offset){if(offset<0||offset>=e.StringData.Length||offset%2!=0)throw new InvalidDataException("Invalid native linked event path");int end=(int)offset;while(end+1<e.StringData.Length&&(e.StringData[end]!=0||e.StringData[end+1]!=0))end+=2;if(end+1>=e.StringData.Length)throw new InvalidDataException("Unterminated linked event path");return e.StringData[(int)offset..(end+2)];}
        bool Common(byte[] bytes)=>System.Text.Encoding.Unicode.GetString(bytes).Replace('\\','/').EndsWith("/common.emevd\0",StringComparison.OrdinalIgnoreCase);
        if(script.LinkedFileOffsets.Any(o=>Common(PathAt(script,o))))return;
        var common=donor.LinkedFileOffsets.Select(o=>PathAt(donor,o)).Single(Common);script.LinkedFileOffsets.Add(script.StringData.Length);script.StringData=[..script.StringData,..common];
    }

    internal static Result Compose(Func<string,byte[]?> readNative,Func<string,byte[]?> readComposed,string definitions,
        IReadOnlyList<Destination> destinations,IReadOnlyList<string>? languages=null) {
        if(destinations.Count is <1 or >2||destinations[0].SourceMap!="m10_02_00_00"||
            destinations.Count==2&&destinations[1].SourceMap!="m10_01_00_00")
            throw new NotSupportedException("Additional travel currently supports Firelink followed optionally by Undead Burg");
        if(destinations.Select(d=>d.TargetMap).Distinct().Count()!=destinations.Count)throw new InvalidDataException("Duplicate destination map");
        var files=new Dictionary<string,byte[]>(StringComparer.Ordinal);var sources=new Dictionary<string,string>(StringComparer.Ordinal);
        byte[] Native(string path){var bytes=readNative(path)??throw new FileNotFoundException("Native travel dependency unavailable",path);sources[path]=Convert.ToHexStringLower(SHA256.HashData(bytes));return bytes;}
        byte[] Merged(string path)=>readComposed(path)??Native(path);
        var nativeMap=MSBB.Read(Native("map/MapStudio/m24_01_00_00.msb.dcx"));
        var nativeDream=MSBB.Read(Native("map/MapStudio/m21_00_00_00.msb.dcx"));
        var common=EMEVD.Read(Native("event/common.emevd.dcx"));
        foreach(int id in new[]{7000,7100,7200,7300})if(common.Events.Count(e=>e.ID==id)!=1)throw new InvalidDataException("Native common lamp event unavailable: "+id);
        var nativeScript=EMEVD.Read(Native("event/m24_01_00_00.emevd.dcx"));
        void Contract(int id,params int[] args){if(!nativeScript.Events.Single(e=>e.ID==0).Instructions.Any(i=>i.Bank==2000&&i.ID==0&&i.ArgData.AsSpan().SequenceEqual(Pack([15,id,..args]))))throw new InvalidDataException("Native lamp constructor contract changed: "+id);}
        Contract(7000,2410950,2411950,9401,12417800);Contract(7100,72410200,2411950);Contract(7200,72410100,2411950,2102950);Contract(7300,72102410,2411950);
        var lampTemplate=nativeMap.Parts.Objects.Single(p=>p.EntityID==2411950);
        var actorTemplate=nativeMap.Parts.Enemies.Single(p=>p.EntityID==2410950);
        var spawnTemplate=nativeMap.Events.SpawnPoints.Single(e=>e.EntityID==2412950);
        var spawnRegionTemplate=nativeMap.Regions.GetEntries().Single(r=>r.Name==spawnTemplate.SpawnPointName);
        if(spawnRegionTemplate.Shape is not MSB.Shape.Point||new[]{spawnTemplate.UnkE0C,spawnTemplate.UnkE0D,spawnTemplate.UnkE0E,spawnTemplate.UnkE0F}.Any(v=>v!=1))throw new InvalidDataException("Native field-lamp spawn template contract changed");
        if(lampTemplate.ModelName!="o009900"||actorTemplate.ModelName!="c9010"||actorTemplate.TalkID!=241000)throw new InvalidDataException("Unexpected native lamp pair");
        var talkTemplate=BND4.Read(Native("script/talk/m24_01_00_00.talkesdbnd.dcx"));
        var lampTalkFile=talkTemplate.Files.Single(f=>Leaf(f.Name)=="t241000.esd");
        var game=BND4.Read(Merged("param/gameparam/gameparam.parambnd.dcx"));
        foreach(var file in game.Files)ReservationScan(file.Bytes.Span,"composed:"+file.Name);
        var returnFile=game.Files.Single(f=>Leaf(f.Name)=="ReturnPointParam.param");
        var returns=PARAM.Read(returnFile.Bytes);returns.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,"ReturnPointParam.xml")));
        if(returns.BigEndian||returns.DetectedSize!=32||returns.Format2D!=PARAM.FormatFlags1.LongDataOffset)throw new InvalidDataException("Unexpected native ReturnPointParam format");
        var donorReturn=returns.Rows.Single(r=>r.ID==2412950);
        if(Convert.ToInt32(donorReturn["warpChairNo"].Value)!=18||Convert.ToInt32(donorReturn["isRegistDeadReturn"].Value)!=1)throw new InvalidDataException("Native ReturnPoint checkpoint contract changed");
        var bindings=new List<Binding>();
        for(int index=0;index<destinations.Count;++index) {
            var d=destinations[index];
            if(d.TargetMap!=(index==0?"m40_02_00_00":"m40_03_00_00")||new[]{d.Spawn,d.LampPosition,d.LampRotation}.Any(v=>!float.IsFinite(v.X)||!float.IsFinite(v.Y)||!float.IsFinite(v.Z)))throw new ArgumentException("Invalid additional destination map or coordinates");
            int area=int.Parse(d.TargetMap.AsSpan(1,2)),block=int.Parse(d.TargetMap.AsSpan(4,2)),code=area*10+block;
            int entity=code*10000,registration=52100520+index*20,chair=55+index,respawn=entity+2950;
            if(returns.Rows.Any(r=>r.ID==respawn||Convert.ToInt32(r["warpChairNo"].Value)==chair))throw new InvalidDataException("Destination ReturnPoint or chair reservation is occupied");
            var mapPath=$"map/MapStudio/{d.TargetMap}.msb.dcx";var map=MSBB.Read(readComposed(mapPath)??throw new FileNotFoundException("Staged destination MSB unavailable",mapPath));
            Free(map,entity+950,entity+1950,respawn);
            var collision=d.CollisionName==null?map.Parts.Collisions.FirstOrDefault(c=>!c.DisableStart):map.Parts.Collisions.SingleOrDefault(c=>c.Name==d.CollisionName);
            if(collision==null)throw new InvalidDataException("Destination lamp has no active collision binding");
            var binding=new Binding(d.SourceMap,d.TargetMap,d.Name,entity+950,entity+1950,respawn,chair,registration,registration+10,
                52100580+index,52100582+index,52100584+index,52100590+index,70900000+index,d.Spawn,d.LampPosition,collision.Name,d.SourceBonfirePart,d.ReplacedMapPiece,d.SourceBonfireKind);bindings.Add(binding);
            if(d.ReplacedMapPiece!=null&&map.Parts.MapPieces.RemoveAll(p=>p.Name==d.ReplacedMapPiece)!=1)throw new InvalidDataException("Selected source bonfire visual mapping is invalid");
            if(map.Parts.GetEntries().Any(p=>p.Name=="c9010_0090"||p.Name=="o009900_0090")||map.Regions.GetEntries().Any(r=>r.Name=="souls_lamp_arrival"))throw new InvalidDataException("Destination travel names are occupied");
            if(!map.Models.Objects.Any(m=>m.Name=="o009900"))map.Models.Objects.Add((MSBB.Model.Object)nativeMap.Models.Objects.Single(m=>m.Name=="o009900").DeepCopy());
            if(!map.Models.Enemies.Any(m=>m.Name=="c9010"))map.Models.Enemies.Add((MSBB.Model.Enemy)nativeMap.Models.Enemies.Single(m=>m.Name=="c9010").DeepCopy());
            var lamp=(MSBB.Part.Object)lampTemplate.DeepCopy();lamp.Name="o009900_0090";lamp.InstanceID=90;lamp.EntityID=binding.Lamp;lamp.Position=d.LampPosition;lamp.Rotation=d.LampRotation;lamp.CollisionName=collision.Name;Groups(lamp);
            var actor=(MSBB.Part.Enemy)actorTemplate.DeepCopy();actor.Name="c9010_0090";actor.InstanceID=90;actor.EntityID=binding.Controller;actor.Position=d.LampPosition;actor.Rotation=d.LampRotation;actor.CollisionName=collision.Name;actor.TalkID=code*1000+950;Groups(actor);
            map.Parts.Objects.Add(lamp);map.Parts.Enemies.Add(actor);
            var arrival=spawnRegionTemplate.DeepCopy();arrival.Name="souls_lamp_arrival";arrival.Position=d.Spawn;arrival.Rotation=d.LampRotation;map.Regions.Regions.Add(arrival);
            // Preserve the native field-lamp event's opaque activation bytes
            // and base fields; a default event's four zero bytes differ.
            var spawn=(MSBB.Event.SpawnPoint)spawnTemplate.DeepCopy();spawn.Name="souls_lamp_spawn";spawn.EventID=map.Events.GetEntries().Select(e=>e.EventID).DefaultIfEmpty(-1).Max()+1;spawn.EntityID=respawn;spawn.SpawnPointName=arrival.Name;map.Events.SpawnPoints.Add(spawn);
            files[mapPath]=map.Write(DCX.Type.DCX_DFLT_10000_44_9);
            var row=new PARAM.Row(donorReturn){ID=respawn,Name=d.Name+" lamp"};row["areaNo"].Value=(short)area;row["blockNo"].Value=(short)block;row["returnPointEntityId"].Value=respawn;row["warpChairNo"].Value=(sbyte)chair;returns.Rows.Add(row);
            var talkPath=$"script/talk/{d.TargetMap}.talkesdbnd.dcx";
            var talk=readComposed(talkPath) is {} talkBytes?BND4.Read(talkBytes):BND4.Read(talkTemplate.Write());
            // No donor map conversations are installed when the destination did
            // not already own a talk binder.
            if(readComposed(talkPath)==null)talk.Files.Clear();
            if(talk.Files.Any(f=>Leaf(f.Name)==$"t{actor.TalkID}.esd"))throw new InvalidDataException("Destination lamp talk ID is occupied");
            var esd=BuildLamp(lampTalkFile.Bytes.ToArray(),binding);esd.Name=$"t{actor.TalkID}";
            talk.Files.Add(new BinderFile(lampTalkFile.Flags,talk.Files.Select(f=>f.ID).DefaultIfEmpty(-1).Max()+1,$@"N:\SPRJ\data\INTERROOT_ps4\script\talk\{d.TargetMap}\t{actor.TalkID}.esd",esd.Write()));files[talkPath]=talk.Write(DCX.Type.DCX_DFLT_10000_44_9);
            var scriptPath=$"event/{d.TargetMap}.emevd.dcx";var script=EMEVD.Read(readComposed(scriptPath)??throw new FileNotFoundException("Staged destination EMEVD unavailable",scriptPath));ReservationScan(script.Write(),"composed:"+scriptPath);
            CommonLink(script,nativeScript);var ctor=EnsureRoot(script,0);EnsureRoot(script,50);Flag(ctor,HeadstoneAvailable,true);
            foreach(int eventID in new[]{7000,7100,7200,7300})if(ctor.Instructions.Any(i=>i.Bank==2000&&i.ID==0&&i.ArgData.Length>=8&&BitConverter.ToInt32(i.ArgData,4)==eventID&&BitConverter.ToInt32(i.ArgData,0)==90))throw new InvalidDataException("Destination travel event slot is occupied");
            Start(ctor,90,7000,binding.Controller,binding.Lamp,HeadstoneAvailable,registration);Start(ctor,90,7100,binding.RegistrationRequest,binding.Lamp);Start(ctor,90,7200,binding.ReturnRequest,binding.Lamp,2102950);Start(ctor,90,7300,binding.ArrivalRequest,binding.Lamp);
            files[scriptPath]=script.Write(DCX.Type.DCX_DFLT_10000_44_9);
        }
        // A new map's load list must also declare its connected scenes. The
        // area-wide list alone is not consulted when this block is entered.
        foreach(var destination in destinations) {
            string path=$"map/{destination.TargetMap}/{destination.TargetMap}.arealoadlist";
            var list=readComposed(path);
            foreach(var connected in destinations)list=MapDestination.AppendLoadList(list,connected.TargetMap);
            files[path]=list!;
        }
        // The native loader canonicalizes talk resources to the area's block0,
        // even while map EMEVD and AI banks use their individual block IDs.
        // Keep the per-map outputs intact and publish the complete shared bank.
        const string sharedTalkMap="m40_00_00_00";
        string sharedTalkPath=$"script/talk/{sharedTalkMap}.talkesdbnd.dcx";
        byte[]? sharedTalk=readComposed(sharedTalkPath);
        if(sharedTalk==null&&readNative(sharedTalkPath) is {} nativeShared){sharedTalk=nativeShared;sources[sharedTalkPath]=Convert.ToHexStringLower(SHA256.HashData(nativeShared));}
        var shared=sharedTalk==null?BND4.Read(talkTemplate.Write()):BND4.Read(sharedTalk);
        if(sharedTalk==null)shared.Files.Clear();
        MergeAreaTalk(shared,destinations.Select(d=>BND4.Read(files[$"script/talk/{d.TargetMap}.talkesdbnd.dcx"])),sharedTalkMap);
        files[sharedTalkPath]=shared.Write(DCX.Type.DCX_DFLT_10000_44_9);
        returns.Rows.Sort((a,b)=>a.ID.CompareTo(b.ID));returnFile.Bytes=WriteRows(returns);files["param/gameparam/gameparam.parambnd.dcx"]=game.Write(DCX.Type.DCX_DFLT_10000_44_9);
        var dreamPath="map/MapStudio/m21_00_00_00.msb.dcx";var dream=MSBB.Read(Merged(dreamPath));Free(dream,HeadstoneEntity,HeadstoneStone);
        if(dream.Parts.GetEntries().Any(p=>p.Name=="c9020_0080"))throw new InvalidDataException("Additional headstone actor name is occupied");
        if(!dream.Models.Enemies.Any(m=>m.Name=="c9020"))dream.Models.Enemies.Add((MSBB.Model.Enemy)nativeDream.Models.Enemies.Single(m=>m.Name=="c9020").DeepCopy());
        if(!dream.Models.MapPieces.Any(m=>m.Name=="m000010"))dream.Models.MapPieces.Add((MSBB.Model.MapPiece)nativeDream.Models.MapPieces.Single(m=>m.Name=="m000010").DeepCopy());
        var headstoneDonor=nativeDream.Parts.Enemies.Single(p=>p.EntityID==2100950);
        var headstone=(MSBB.Part.Enemy)headstoneDonor.DeepCopy();headstone.Name="c9020_0080";headstone.InstanceID=80;headstone.EntityID=HeadstoneEntity;headstone.TalkID=HeadstoneTalk;headstone.Position=new(-13.8f,-7f,-25f);Groups(headstone);dream.Parts.Enemies.Add(headstone);
        var stone=(MSBB.Part.MapPiece)nativeDream.Parts.MapPieces.Single(p=>p.Name=="m000010"&&p.ModelName=="m000010").DeepCopy();
        if(dream.Parts.GetEntries().Any(p=>p.Name=="m000010_0080"||p.ModelName=="m000010"&&p.InstanceID==80))throw new InvalidDataException("Additional headstone stone name or instance is occupied");
        // The retail headstone is scenery, not an OBJ. Share its existing model
        // and map texture references. Turn the visual around its model center,
        // retaining its world position; keep the messenger's proven heading.
        var stoneModel=FLVER2.Read(Native("map/m21_00_00_00/m21_00_00_00_000010.flver.dcx"));
        if(stoneModel.Meshes.Sum(m=>m.Vertices.Count)!=8724)throw new InvalidDataException("Native physical headstone template changed");
        var stoneMin=stoneModel.Meshes.SelectMany(m=>m.Vertices).Select(v=>v.Position).Aggregate(Vector3.Min);var stoneMax=stoneModel.Meshes.SelectMany(m=>m.Vertices).Select(v=>v.Position).Aggregate(Vector3.Max);
        if(!float.IsFinite(stoneMin.X+stoneMin.Y+stoneMin.Z+stoneMax.X+stoneMax.Y+stoneMax.Z)||Vector3.Distance(stoneMin,stoneModel.Header.BoundingBoxMin)>.001f||Vector3.Distance(stoneMax,stoneModel.Header.BoundingBoxMax)>.001f)throw new InvalidDataException("Native physical headstone bounds changed");
        var stoneCenter=(stoneMin+stoneMax)*.5f;stoneCenter.Y=0;
        Matrix4x4 StoneRotation(Vector3 rotation)=>Matrix4x4.CreateRotationX(rotation.X*MathF.PI/180)*Matrix4x4.CreateRotationZ(rotation.Z*MathF.PI/180)*Matrix4x4.CreateRotationY(rotation.Y*MathF.PI/180);
        var oldStoneOffset=Vector3.Transform(stoneCenter*stone.Scale,StoneRotation(stone.Rotation));
        stone.Rotation=new(stone.Rotation.X,(stone.Rotation.Y+180)%360,stone.Rotation.Z);var newStoneOffset=Vector3.Transform(stoneCenter*stone.Scale,StoneRotation(stone.Rotation));
        stone.Name="m000010_0080";stone.InstanceID=80;stone.EntityID=HeadstoneStone;stone.Position+=headstone.Position-headstoneDonor.Position+new Vector3(oldStoneOffset.X-newStoneOffset.X,0,oldStoneOffset.Z-newStoneOffset.Z);Groups(stone);dream.Parts.MapPieces.Add(stone);
        files[dreamPath]=dream.Write(DCX.Type.DCX_DFLT_10000_44_9);
        var dreamScriptPath="event/m21_00_00_00.emevd.dcx";var dreamScript=EMEVD.Read(Merged(dreamScriptPath));ReservationScan(dreamScript.Write(),"composed:"+dreamScriptPath);var dreamCtor=EnsureRoot(dreamScript,0);var pre=EnsureRoot(dreamScript,50);
        var warp=dreamScript.Events.Single(e=>e.ID==12107000);if(warp.Instructions.Count!=5||warp.Instructions[4].Bank!=2003||warp.Instructions[4].ID!=49||warp.Parameters.Count!=3)throw new InvalidDataException("Native headstone warp contract changed");
        Flag(dreamCtor,HeadstoneAvailable,true);dreamCtor.Instructions.Add(new(2003,18,Pack(HeadstoneEntity,7001,1)));
        foreach(var setup in pre.Instructions.Where(i=>i.ArgData.Length>=4&&BitConverter.ToInt32(i.ArgData,0)==2100950).ToArray()){var data=setup.ArgData.ToArray();BitConverter.GetBytes(HeadstoneEntity).CopyTo(data,0);pre.Instructions.Add(new(setup.Bank,setup.ID,data));}
        foreach(var b in bindings){int slot=80+bindings.IndexOf(b);if(dreamCtor.Instructions.Any(i=>i.Bank==2000&&i.ID==0&&i.ArgData.Length>=8&&BitConverter.ToInt32(i.ArgData,0)==slot&&BitConverter.ToInt32(i.ArgData,4)==12107000))throw new InvalidDataException("Additional headstone warp slot is occupied");Flag(dreamCtor,b.ArrivalRequest,false);Start(dreamCtor,slot,12107000,b.ArrivalRequest,HeadstoneEntity,b.Respawn);}
        files[dreamScriptPath]=dreamScript.Write(DCX.Type.DCX_DFLT_10000_44_9);
        var dreamTalkPath="script/talk/m21_00_00_00.talkesdbnd.dcx";var dreamTalk=BND4.Read(Merged(dreamTalkPath));var donorTalk=dreamTalk.Files.Single(f=>Leaf(f.Name)=="t210000.esd");
        if(dreamTalk.Files.Any(f=>Leaf(f.Name)==$"t{HeadstoneTalk}.esd"))throw new InvalidDataException("Imported Dream headstone talk ID is occupied");
        var headstoneEsd=BuildHeadstone(donorTalk.Bytes.ToArray(),bindings);headstoneEsd.Name=$"t{HeadstoneTalk}";
        dreamTalk.Files.Add(new BinderFile(donorTalk.Flags,dreamTalk.Files.Max(f=>f.ID)+1,$@"N:\SPRJ\data\INTERROOT_ps4\script\talk\m21_00_00_00\t{HeadstoneTalk}.esd",headstoneEsd.Write()));files[dreamTalkPath]=dreamTalk.Write(DCX.Type.DCX_DFLT_10000_44_9);
        int locales=0;foreach(string language in languages??["as","dandk","deude","enggb","engus","eu","finfi","frafr","itait","japanese","jp","jpnjp","korkr","na","nldnl","norno","polpl","porbr","porpt","rusru","spaar","spaes","swese","turtr","uk","zhocn","zhotw"]) {
            if(!Regex.IsMatch(language,@"\A[A-Za-z0-9_]+\z"))throw new ArgumentException("Invalid native menu language");string path=$"msg/{language}/menu.msgbnd.dcx";var original=readNative(path);if(original==null)continue;var binder=BND4.Read(readComposed(path)??Native(path));
            var candidates=binder.Files.Where(f=>Leaf(f.Name).EndsWith(".fmg",StringComparison.OrdinalIgnoreCase)).Select(f=>(File:f,Text:FMG.Read(f.Bytes))).Where(f=>f.Text.Entries.Any(e=>e.ID==70002410)).ToArray();if(candidates.Length!=1)throw new InvalidDataException("Native warp menu text contract changed: "+language);var text=candidates[0].Text;
            foreach(var b in bindings){if(text.Entries.Any(e=>e.ID==b.MenuText))throw new InvalidDataException("Destination menu text reservation is occupied");text.Entries.Add(new(text,b.MenuText,b.Name));}candidates[0].File.Bytes=text.Write();files[path]=binder.Write(binder.Compression);++locales;
        }
        if(locales==0)throw new InvalidDataException("Native warp menu text is unavailable");
        return new(files,bindings,sources,["Additional destination interactions, death checkpoint persistence and save/reload require native gameplay validation.","Native lamp actor/object and common events are reused; source bonfire rest/kindling/Estus behavior is not transplanted. Only the two explicitly selected source bonfires become lamps; other source bonfires remain scenery.","The additional Dream headstone clones native physical scenery m000010 and its c9020 messenger controller. Its visual has no separately cloned solid collision body; existing native Dream collision and all original headstones are preserved."]);
    }

    internal static ESD BuildLamp(byte[] original,Binding b) {
        var esd=ESD.Read(original);
        var replacements=new Dictionary<int,int>{[10007810]=b.ActiveFlag-2410000,[70000000]=b.DisabledFlag-2410,[70000100]=b.ReturnRequest-2410000,[70000200]=b.RegistrationRequest-2410000};
        var changed=Rewrite(esd,replacements);if(replacements.Keys.Any(k=>!changed.Contains(k)))throw new InvalidDataException("Native lamp ESD flag expressions changed");return esd;
    }
    internal static ESD BuildHeadstone(byte[] original,IReadOnlyList<Binding> bindings) {
        var esd=ESD.Read(original);if(!esd.StateGroups.ContainsKey(2147483641)||!esd.StateGroups.ContainsKey(2147483644))throw new InvalidDataException("Native headstone graph changed");
        var nativeMenu=esd.StateGroups[2147483641].GetValueOrDefault(2);
        if(nativeMenu?.WhileCommands.Count!=1||nativeMenu.WhileCommands[0].CommandBank!=1||nativeMenu.WhileCommands[0].CommandID!=68||nativeMenu.WhileCommands[0].Arguments.Count!=1||!nativeMenu.WhileCommands[0].Arguments[0].SequenceEqual((byte[])[0x81,..BitConverter.GetBytes(.33),0xa1]))throw new InvalidDataException("Native headstone menu lifetime contract changed");
        ESD.CommandCall MenuLifetime(){var call=nativeMenu.WhileCommands[0];return new(call.CommandBank,call.CommandID,call.Arguments.Select(a=>a.ToArray()).ToArray());}
        // Retain the native multiplayer/death/menu guards, with the broad
        // native Talk contact shape proven at this additional open-path stone.
        var required=new Dictionary<int,int>{[12105030]=HeadstoneAvailable,[6110]=6000,[6130]=6000};
        var changed=Rewrite(esd,required);if(required.Keys.Any(k=>!changed.Contains(k)))throw new InvalidDataException("Native headstone access contract changed");
        var states=new Dictionary<long,ESD.State>();ESD.State State(long id){var s=new ESD.State();states.Add(id,s);return s;}
        void Go(ESD.State s,long target,byte[] expr)=>s.Conditions.Add(new(target,End(expr)));
        Go(State(0),1,Value(1));var menu=State(1);menu.EntryCommands.Add(Command(20));
        for(int i=0;i<bindings.Count;++i)menu.EntryCommands.Add(Command(19,i+1,bindings[i].MenuText,i==0?-1:bindings[i].ActiveFlag));Go(menu,2,Value(1));
        // Warp-menu initialization is asynchronous. Keep native talk ownership
        // alive, and observe the menu open before accepting its closed state.
        var opening=State(2);opening.EntryCommands.Add(Command(41));opening.WhileCommands.Add(MenuLifetime());Go(opening,3,Binary(0x95,Call(59,Value(100),Value(0)),Value(1)));
        var select=State(3);select.WhileCommands.Add(MenuLifetime());
        for(int i=0;i<bindings.Count;++i){Go(select,10+i,Binary(0x95,Call(23),Value(i+1)));var chosen=State(10+i);chosen.EntryCommands.Add(Command(11,bindings[i].ArrivalRequest,1));Go(chosen,4,Value(1));}
        Go(select,4,Binary(0x99,Binary(0x95,Call(23),Value(0)),Binary(0x95,Call(59,Value(100),Value(0)),Value(0))));
        State(4).Conditions.Add(new(){Evaluator=End(Value(1)),TargetState=null,PassCommands=[new(7,-1,End(Value(0)))]});esd.StateGroups[2147483641]=states;return esd;
    }
    static byte[] Value(int v)=>v is >=-64 and <=63?[(byte)(v+64)]:[0x82,..BitConverter.GetBytes(v)];
    static byte[] End(byte[] x)=>[..x,0xa1];
    static byte[] Call(int id,params byte[][] args)=>[..Value(id),..args.SelectMany(a=>a),(byte)(0x84+args.Length),0xa6];
    static byte[] Binary(byte op,byte[] x,byte[] y)=>[..x,..y,op];
    static ESD.CommandCall Command(int id,params int[] args)=>new(1,id,args.Select(x=>End(Value(x))).ToArray());
    internal static void MergeAreaTalk(BND4 destination,IEnumerable<BND4> maps,string areaMap) {
        if(areaMap!="m40_00_00_00")throw new ArgumentException("Unsupported shared talk area");
        var names=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach(var file in destination.Files)if(!names.Add(Leaf(file.Name)))throw new InvalidDataException("Shared talk binder has duplicate native names");
        if(destination.Files.Select(f=>f.ID).Distinct().Count()!=destination.Files.Count)throw new InvalidDataException("Shared talk binder has duplicate native IDs");
        var additions=new List<BinderFile>();int next=destination.Files.Select(f=>f.ID).DefaultIfEmpty(-1).Max();
        foreach(var map in maps)foreach(var file in map.Files) {
            string leaf=Leaf(file.Name);
            if(!leaf.EndsWith(".esd",StringComparison.OrdinalIgnoreCase)||!names.Add(leaf))throw new InvalidDataException("Duplicate or unsupported additional talk member: "+leaf);
            additions.Add(new BinderFile(file.Flags,checked(++next),$@"N:\SPRJ\data\INTERROOT_ps4\script\talk\{areaMap}\{leaf}",file.Bytes.ToArray()));
        }
        destination.Files.AddRange(additions);
    }
    static HashSet<int> Rewrite(ESD esd,IReadOnlyDictionary<int,int> replacements) {
        var changed=new HashSet<int>();
        void Expr(byte[] bytes){for(int i=0;i<bytes.Length;){byte op=bytes[i++];if(op is 0x80 or 0x81 or 0x82){int count=op==0x81?8:4;if(i+count>bytes.Length)throw new InvalidDataException("Truncated native ESD expression");if(op==0x82&&replacements.TryGetValue(BitConverter.ToInt32(bytes,i),out int replace)){changed.Add(BitConverter.ToInt32(bytes,i));BitConverter.GetBytes(replace).CopyTo(bytes,i);}i+=count;}else if(op==0xa5){while(i+1<bytes.Length&&(bytes[i]!=0||bytes[i+1]!=0))i+=2;if(i+1>=bytes.Length)throw new InvalidDataException("Unterminated native ESD string");i+=2;}}}
        void Commands(IEnumerable<ESD.CommandCall> calls){foreach(var c in calls)foreach(var arg in c.Arguments)Expr(arg);}
        void Conditions(IEnumerable<ESD.Condition> conditions){foreach(var c in conditions){Expr(c.Evaluator);Commands(c.PassCommands);Conditions(c.Subconditions);}}
        foreach(var s in esd.StateGroups.Values.SelectMany(g=>g.Values)){Commands(s.EntryCommands);Commands(s.WhileCommands);Commands(s.ExitCommands);Conditions(s.Conditions);}return changed;
    }
}

internal static partial class Program {
    internal static int FinalizeDestination(Dictionary<string,string> values,Profile profile,string root) {
        if(profile.Id!="DS1R")throw new NotSupportedException("Additional destinations currently support DS1 Remastered Firelink/Burg");
        string Get(string key)=>values.TryGetValue(key,out var value)?value:throw new ArgumentException("Missing "+key);
        var output=Canonical(Get("--output"));var nativeInput=Canonical(Get("--native-root"));var native=nativeInput;if(Directory.Exists(Path.Combine(native,"dvdroot_ps4")))native=Path.Combine(native,"dvdroot_ps4");
        foreach(var source in new[]{Canonical(root),nativeInput,native})if(output.Equals(source,StringComparison.OrdinalIgnoreCase)||output.StartsWith(source.TrimEnd(Path.DirectorySeparatorChar)+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new IOException("Destination output must be outside source/native assets");
        var smithbox=values.GetValueOrDefault("--smithbox")??Environment.GetEnvironmentVariable("SMITHBOX_DIR");if(smithbox==null&&File.Exists(Path.Combine(AppContext.BaseDirectory,"smithbox-path.txt")))smithbox=File.ReadAllText(Path.Combine(AppContext.BaseDirectory,"smithbox-path.txt")).Trim();if(smithbox==null)throw new DirectoryNotFoundException("Set --smithbox or SMITHBOX_DIR");
        string SafeOutput(string relative){if(Path.IsPathRooted(relative)||relative.Replace('\\','/').Split('/').Any(p=>p==".."))throw new IOException("Invalid staged destination path");var path=Canonical(Path.Combine(output,relative));if(!path.StartsWith(output+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new IOException("Staged destination path escapes output");return path;}
        string Safe(string relative)=>SafeOutput(Path.Combine("dvdroot_ps4",relative));
        byte[]? ReadNative(string relative){var path=Path.Combine(native,relative);return File.Exists(path)?File.ReadAllBytes(path):null;}
        byte[]? ReadStaged(string relative){var path=Safe(relative);return File.Exists(path)?File.ReadAllBytes(path):null;}
        var reportPath=SafeOutput("conversion.json");using var report=JsonDocument.Parse(File.ReadAllBytes(reportPath));var reports=new List<JsonElement>{report.RootElement};
        var children=new List<JsonDocument>();try {
            if(report.RootElement.TryGetProperty("connectedMaps",out var connected))foreach(var entry in connected.EnumerateArray()) {
                string? child=entry.TryGetProperty("conversion",out var named)?named.GetString():entry.TryGetProperty("report",out var p)?p.GetString():null;
                if(child==null)throw new InvalidDataException("Connected map report path unavailable");var doc=JsonDocument.Parse(File.ReadAllBytes(SafeOutput(child)));children.Add(doc);reports.Add(doc.RootElement);
            }
            using var source=new StaticMapSource(root,profile.Id,"m10_02_00_00");var destinations=new List<MapTravel.Destination>();
            Vector3 Vector(JsonElement element)=>new(element.GetProperty("X").GetSingle(),element.GetProperty("Y").GetSingle(),element.GetProperty("Z").GetSingle());
            foreach(var entry in reports.OrderBy(e=>e.GetProperty("map").GetString()=="m10_02_00_00"?0:1)) {
                var sourceMap=entry.GetProperty("map").GetString()!;var target=entry.GetProperty("target").GetString()!;var bytes=source.ReadAsset($"map/MapStudio/{sourceMap}.msb")??source.ReadAsset($"map/MapStudio/{sourceMap}.msb.dcx")??throw new FileNotFoundException("Source bonfire MSB unavailable");var msb=MSB1.Read(bytes);int bonfireID=sourceMap=="m10_02_00_00"?1021960:sourceMap=="m10_01_00_00"?1011960:throw new NotSupportedException("Unsupported additional destination source map");
                // The initial Burg bonfire is authored as a DummyObject in
                // this source MSB. It is an explicit checkpoint selection,
                // not a promise to translate that object's source event state.
                var bonfire=msb.Parts.GetEntries().OfType<MSB1.Part.ObjectBase>().Single(p=>p.EntityID==bonfireID);var offset=Vector(entry.GetProperty("offset"));string? collision=null;
                if(entry.TryGetProperty("collisionEnvironment",out var bindings)&&bindings.ValueKind==JsonValueKind.Array){var match=bindings.EnumerateArray().FirstOrDefault(b=>b.GetProperty("source").GetString()==bonfire.CollisionName);if(match.ValueKind==JsonValueKind.Object&&match.TryGetProperty("target",out var found))collision=found.GetString();}
                string? visual=null;if(entry.TryGetProperty("placementResults",out var placements)) {
                    var matches=placements.EnumerateArray().Where(p=>p.GetProperty("sourceName").GetString()==bonfire.Name).ToArray();
                    if(matches.Length>1)throw new InvalidDataException("Duplicate source bonfire render placement: "+bonfire.Name);
                    // A valid source object can be an interaction anchor whose
                    // model emitted no regular render placement. Its authored
                    // transform still selects the native lamp's position.
                    if(matches.Length==1&&matches[0].TryGetProperty("targetKind",out var kind)&&kind.GetString()=="MapPiece")visual=matches[0].GetProperty("targetName").GetString();
                }
                if(entry.GetProperty("source").GetString()!=profile.Id||destinations.Count>0&&offset!=Vector(reports[0].GetProperty("offset")))throw new InvalidDataException("Additional destination source profiles/transforms must agree");
                var lamp=bonfire.Position+offset;var arrival=lamp+Vector3.UnitY+Vector3.Transform(new Vector3(0,0,2),Matrix4x4.CreateRotationY(bonfire.Rotation.Y*MathF.PI/180));
                destinations.Add(new(sourceMap,target,sourceMap=="m10_02_00_00"?"Firelink Shrine":"Undead Burg",arrival,lamp,bonfire.Rotation,collision,bonfire.Name,visual,bonfire.GetType().Name));
            }
            MapTravel.VerifyReservations(native);
            var result=MapTravel.Compose(ReadNative,ReadStaged,Path.Combine(smithbox,"src/Smithbox.Data/Assets/PARAM/BB/Defs"),destinations,Directory.EnumerateDirectories(Path.Combine(native,"msg")).Select(Path.GetFileName).Select(n=>n!).ToArray());
            // Validate every composed artifact before the caller's staging tree
            // is changed; failures leave all original staged files untouched.
            var writes=result.Files.Select(p=>(Path:Safe(p.Key),p.Value)).ToArray();var travelReport=SafeOutput("destination-travel.json");
            foreach(var write in writes){Directory.CreateDirectory(Path.GetDirectoryName(write.Path)!);File.WriteAllBytes(write.Path,write.Value);}
            var text=JsonSerializer.Serialize(new{schema="bbmap-destination-travel-1",result.Destinations,result.NativeSources,result.Limitations},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true});File.WriteAllText(travelReport,text);Console.WriteLine(text);return 0;
        }finally{foreach(var child in children)child.Dispose();}
    }
}
