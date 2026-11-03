// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

internal static class MapCatacombsBridges {
    const string Params="param/gameparam/gameparam.parambnd.dcx",Donor="o241013";
    internal const int FirstEvent=12416500,Lever=4056300,Bridge=4056301,LowerAction=4056350,RaiseAction=4056351,ApproachRegion=4056380,CrossRegion=4056381;
    internal sealed record Binding(string SourceName,string SourceModel,int SourceEntity,string Model,int Entity,string Collision,Vector3 Position,Vector3 Rotation,int Frames,float Duration);
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,IReadOnlyList<Binding> Bindings,IReadOnlySet<string> ConsumedNames,IReadOnlyList<string> Limitations);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs mechanism: "+why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static int F(int source)=>MapNpcQuestState.Flag(source);
    internal static void VerifyReservations(string nativeRoot) {
        foreach(string path in Directory.EnumerateFiles(Path.Combine(nativeRoot,"event"),"*.emevd.dcx"))
            Need(!EMEVD.Read(path).Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+100),"native Catacombs mechanism/quest event range occupied: "+path);
        var game=BND4.Read(Path.Combine(nativeRoot,Params));
        foreach(string name in new[]{"ObjectParam.param","ObjActParam.param"})Need(PARAM.Read(game.Files.Single(f=>Leaf(f.Name)==name).Bytes).Rows.All(r=>r.ID<696100||r.ID>=696200),"native Catacombs object/action namespace occupied: "+name);
        foreach(int model in Enumerable.Range(696100,100))Need(!File.Exists(Path.Combine(nativeRoot,"obj/o"+model+".objbnd.dcx")),"native Catacombs object archive occupied: "+model);
    }
    static EMEVD.Instruction I(int bank,int id,params object[] values) {
        using var s=new MemoryStream();using var w=new BinaryWriter(s);foreach(var v in values){int width=v is byte or sbyte?1:v is short or ushort?2:4;while(s.Position%width!=0)w.Write((byte)0);switch(v){case byte n:w.Write(n);break;case sbyte n:w.Write(n);break;case int n:w.Write(n);break;case float n:w.Write(n);break;default:throw new InvalidDataException("Mechanism argument type");}}while(s.Position%4!=0)w.Write((byte)0);return new(bank,id,s.ToArray());
    }
    internal static IReadOnlySet<string> SourceNames(string sourceMap,MSB1 source) {
        var names=new HashSet<string>(StringComparer.Ordinal);if(sourceMap!="m13_00_00_00")return names;
        foreach(var p in new[]{(Name:"o3011_0002",Model:"o3011",Entity:1301003,Init:-1),(Name:"o3001_0000",Model:"o3001",Entity:1301103,Init:2)}) {
            var part=source.Parts.Objects.Single(x=>x.Name==p.Name);Need(part.ModelName==p.Model&&part.EntityID==p.Entity&&part.InitAnimID==p.Init&&part.Scale==Vector3.One,"source bridge/lever identity changed");names.Add(p.Name);
        }return names;
    }
    internal static Result Apply(Func<string,byte[]?> sourceRead,Func<string,byte[]?> nativeRead,Func<string,byte[]?> composedRead,
        MSB1 source,MSBB destination,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        byte[] scriptBytes,string sourceMap,string targetMap,FLVER2 surface,Func<IFlverMaterial,FLVER2.Material> materialFactory,MSBB.Part.GparamConfigStruct? defaultLightingGparam=null) {
        var consumed=SourceNames(sourceMap,source);var files=new Dictionary<string,byte[]>();var bindings=new List<Binding>();
        if(consumed.Count==0)return new(files,scriptBytes.ToArray(),bindings,consumed,[]);
        Need(targetMap=="m40_05_00_00"&&slots.Count==chunks.Count&&slots.Count>0&&float.IsFinite(offset.LengthSquared()),"unsupported map/collision frame");
        byte[] Source(string p)=>sourceRead(p)??throw new FileNotFoundException("Source Catacombs mechanism unavailable",p);
        byte[] Native(string p)=>nativeRead(p)??throw new FileNotFoundException("Native mechanism carrier unavailable",p);
        var authoredEvents=Source("event/m13_00_00_00.emevd.dcx");Need(Convert.ToHexStringLower(SHA256.HashData(authoredEvents))=="691516e02235c8770629d032b6512e2ce1e14844487b198a1ee88756207431ba","audited source Catacombs event resource changed");
        var script=EMEVD.Read(scriptBytes);Need(script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+10),"owned bridge event namespace occupied");
        var game=BND4.Read(composedRead(Params)??Native(Params));var objectFile=game.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param");var actionFile=game.Files.Single(f=>Leaf(f.Name)=="ObjActParam.param");
        var objects=Rows(objectFile.Bytes,"OBJECT_PARAM_ST",32,1);var actions=Rows(actionFile.Bytes,"OBJ_ACT_PARAM_ST",48,2);
        Need(objects.Rows.All(r=>r.ID<696100||r.ID>=696200)&&actions.Rows.All(r=>r.ID<696100||r.ID>=696200),"reserved Catacombs object/action rows occupied");
        var donor=BND4.Read(Native("obj/"+Donor+".objbnd.dcx"));var rigVisual=FLVER2.Read(donor.Files.Single(f=>f.ID==200).Bytes);
        var map=MSBB.Read(Native("map/MapStudio/m24_02_00_00.msb.dcx"));var actorTemplate=map.Parts.Objects.Single(p=>p.Name=="o241013_0000");var actionTemplate=map.Events.ObjActs.Single(e=>e.ObjActPartName==actorTemplate.Name);
        var originalObject=objects.Rows.Single(r=>r.ID==241013);var actionDonor=actions.Rows.Single(r=>r.ID==2420000);var actionPayload=Payload(actionDonor);
        Need(BitConverter.ToInt32(actionPayload,12)==101150&&BitConverter.ToInt32(actionPayload,16)==7100&&actionPayload[26]==191&&BitConverter.ToInt32(actionPayload,28)==1&&BitConverter.ToInt32(actionPayload,40)==7100,"native lever/player interaction precedent changed");
        var entities=destination.Parts.GetEntries().Select(p=>p.EntityID).Concat(destination.Events.GetEntries().Select(p=>p.EntityID)).Concat(destination.Events.ObjActs.Select(p=>p.ObjActEntityID)).Concat(destination.Regions.GetEntries().Select(p=>p.EntityID)).ToHashSet();
        foreach(int n in new[]{Lever,Bridge,LowerAction,RaiseAction,ApproachRegion,CrossRegion})Need(entities.Add(n),"owned mechanism entity occupied: "+n);
        var models=new List<MSBB.Model.Object>();var actors=new List<MSBB.Part.Object>();var objacts=new List<MSBB.Event.ObjAct>();var regions=new List<MSBB.Region>();
        int eventIndex=destination.Events.GetEntries().Select(e=>e.EventID).DefaultIfEmpty(-1).Max()+1;
        foreach(var spec in new[]{(Source:"o3011_0002",Model:696100,Entity:Lever),(Source:"o3001_0000",Model:696101,Entity:Bridge)}) {
            var part=source.Parts.Objects.Single(p=>p.Name==spec.Source);string model="o"+spec.Model;
            Need(destination.Models.Objects.All(m=>m.Name!=model)&&nativeRead("obj/"+model+".objbnd.dcx")==null&&composedRead("obj/"+model+".objbnd.dcx")==null,"owned mechanism asset occupied");
            var bank=BND3.Read(Source("obj/"+part.ModelName+".objbnd.dcx"));var visual=FLVER2.Read(bank.Files.Single(f=>f.ID==200).Bytes);
            Need(visual.Nodes.Count==2&&visual.Nodes.Select(n=>n.Name).ToHashSet().SetEquals(new[]{part.ModelName,part.ModelName+"_00"}),"source rigid visual rig");
            var mapping=visual.Nodes.Select((n,i)=>(i,target:n.Name==part.ModelName?0:1)).ToDictionary(p=>p.i,p=>p.target);
            var converted=MapBreakables.RetargetVisual(visual,rigVisual,surface,"DS1R",Vector3.One,mapping,materialFactory);
            converted.Dummies.Clear();foreach(var d in visual.Dummies){Need(d.AttachBoneIndex==-1,"source mechanism dummy has unsupported attachment");var clone=new FLVER.Dummy(d);clone.ParentBoneIndex=d.ParentBoneIndex<0?(short)-1:checked((short)mapping[d.ParentBoneIndex]);converted.Dummies.Add(clone);}
            var motion=CatacombsBridgeAnimations.Convert(part.ModelName,bank.Files.Single(f=>f.ID==400).Bytes.ToArray(),donor.Files.Single(f=>f.ID==400).Bytes.ToArray());
            byte[] physics;
            if(part.ModelName=="o3001") {
                Need(motion.SourceLeafReference==Matrix4x4.Identity,"bridge collider/reference frame changed");physics=CatacombsBridgePhysics.Convert(donor.Files.Single(f=>f.ID==300).Bytes.ToArray(),CatacombsBridgePhysics.Source(bank.Files.Single(f=>f.ID==300).Bytes.ToArray()));
            } else {
                var raw=bank.Files.Single(f=>f.ID==300).Bytes.ToArray();Need(Convert.ToHexStringLower(SHA256.HashData(raw))=="48f9030a92e46ad2243ad814083086d8edd65804eb8e0586203c960648a068f0","source lever collision resource changed");
                var tag=new SourceCollision.Tag(raw);var body=tag.Items.Single(i=>i.Count==1&&tag.Name(i.Type)=="hkpRigidBody");var rest=tag.Transform(tag.Nested(body.Type,body.At,"motion","motionState","transform").At);
                Need(rest==motion.SourceLeafReference,"lever physical/animation reference disagreement");var translated=tag.Reference(tag.Nested(body.Type,body.At,"collidable","shape").At);Need(tag.Name(translated.Type)=="hkpConvexTranslateShape","lever translated leaf box");var box=tag.Reference(tag.Nested(translated.Type,translated.At,"childShape","childShape").At);Need(tag.Name(box.Type)=="hkpBoxShape","lever physical shape");
                var half=tag.Vector(tag.Nested(box.Type,box.At,"halfExtents").At);var center=Vector3.Transform(tag.Vector(tag.Nested(translated.Type,translated.At,"translation").At),rest);float radius=BitConverter.ToSingle(raw,tag.Nested(box.Type,box.At,"radius").At);
                physics=DoorPhysics.Fit(donor.Files.Single(f=>f.ID==300).Bytes.ToArray(),new(half,center,radius));
            }
            var output=BND4.Read(Native("obj/"+Donor+".objbnd.dcx"));output.Files.Single(f=>f.ID==200).Bytes=converted.Write();output.Files.Single(f=>f.ID==300).Bytes=physics;output.Files.Single(f=>f.ID==400).Bytes=motion.Bundle;
            foreach(var f in output.Files)f.Name=f.Name.Replace(Donor,model,StringComparison.Ordinal);files.Add("obj/"+model+".objbnd.dcx",output.Write(DCX.Type.DCX_DFLT_10000_44_9));
            Append(objects,originalObject,spec.Model,"Imported DS1R animated "+part.ModelName,Payload(originalObject));var modelRow=(MSBB.Model.Object)map.Models.Objects.Single(m=>m.Name==Donor).DeepCopy();modelRow.Name=model;modelRow.SibPath=@"N:\SPRJ\data\Model\obj\"+model+@"\sib\"+model+".sib";models.Add(modelRow);
            var actor=(MSBB.Part.Object)actorTemplate.DeepCopy();actor.Name=model+"_0000";Need(destination.Parts.GetEntries().All(p=>p.Name!=actor.Name),"owned mechanism part name occupied");actor.ModelName=model;actor.EntityID=spec.Entity;actor.InstanceID=destination.Parts.Objects.Select(p=>p.InstanceID).DefaultIfEmpty(-1).Max()+1+actors.Count;
            actor.Position=part.Position+offset;actor.Rotation=part.Rotation;actor.Scale=part.Scale;actor.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,null,actor.Position)].Name;actor.SibPath=@"N:\SPRJ\data\Model\map\"+targetMap+@"\layout\o_layout.SIB";Array.Fill(actor.AnimIDs,(short)-1);if(spec.Entity==Bridge)actor.AnimIDs[0]=2;
            actor.GparamConfigStruct=(defaultLightingGparam??destination.Parts.MapPieces.FirstOrDefault(p=>p.GparamConfig.LightSetID==0)?.GparamConfig??new()).DeepCopy();Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);actors.Add(actor);
            bindings.Add(new(part.Name,part.ModelName,part.EntityID,model,spec.Entity,actor.CollisionName,actor.Position,actor.Rotation,motion.Frames,motion.Duration));
        }
        var leverActor=actors.Single(p=>p.EntityID==Lever);
        foreach(var spec in new[]{(Id:696110,Entity:LowerAction,Marker:191,Animation:1),(Id:696111,Entity:RaiseAction,Marker:192,Animation:3)}) {
            var payload=actionPayload.ToArray();payload[26]=checked((byte)spec.Marker);BitConverter.GetBytes(spec.Animation).CopyTo(payload,28);Append(actions,actionDonor,spec.Id,"Imported Catacombs lever "+spec.Animation,payload);
            var action=(MSBB.Event.ObjAct)actionTemplate.DeepCopy();action.Name="souls_catacombs_bridge_action_"+spec.Animation;action.EventID=eventIndex++;action.EntityID=-1;action.PartName=leverActor.CollisionName;action.ObjActPartName=leverActor.Name;action.ObjActEntityID=spec.Entity;action.ObjActParamID=spec.Id;action.ObjActState=0;action.EventFlagID=0;objacts.Add(action);
        }
        foreach(var spec in new[]{(Source:1302000,Target:ApproachRegion),(Source:1302001,Target:CrossRegion)}) {
            var from=source.Regions.GetEntries().Single(p=>p.EntityID==spec.Source);Need(from.Shape is MSB.Shape.Box,"authored trap region type changed");
            var region=new MSBB.Region{Name="souls_catacombs_trap_"+spec.Source,EntityID=spec.Target,Position=from.Position+offset,Rotation=from.Rotation,Shape=from.Shape.DeepCopy()};regions.Add(region);
        }
        objectFile.Bytes=Write(objects);actionFile.Bytes=Write(actions);files.Add(Params,game.Write(game.Compression));var emitted=Events(scriptBytes,authoredEvents);
        void ApplyMap(MSBB target){target.Models.Objects.AddRange(models);target.Parts.Objects.AddRange(actors);target.Events.ObjActs.AddRange(objacts);target.Regions.Regions.AddRange(regions);}
        var trial=MSBB.Read(destination.Write());ApplyMap(trial);_=MSBB.Read(trial.Write());ApplyMap(destination);
        return new(files,emitted,bindings,consumed,["Patches' source rotating bridge retains its exact animated convex hull, and the lever retains its source animated box. Native keyframed collision policy/material/inertia and lever/player101150 alignment remain Bloodborne's.","Source11300403 persists bridge orientation; manual lever actions and source1302000/1302001 traps use the same live mechanism. Source navmesh1304003 toggling has no proved native navigation adapter; NPC pathfinding across the moving bridge is not claimed.","Only the bridge and lever beside Patches are adapted. Entrance/midway gates, the other rotating bridge, coffin travel and later Tomb mechanisms remain separate source contracts. Native interaction/contact/trap save-reload still require gameplay validation."]);
    }
    static PARAM Rows(Memory<byte> bytes,string type,int size,short version) {
        var p=PARAM.Read(bytes);Need(p.ParamType==type&&p.DetectedSize==size&&p.ParamdefDataVersion==version&&!p.BigEndian&&p.Format2D==PARAM.FormatFlags1.LongDataOffset,"native parameter ABI "+type);var def=new PARAMDEF{ParamType=type,DataVersion=version,Unicode=true};def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=size});p.ApplyParamdef(def);return p;
    }
    static byte[] Payload(PARAM.Row row)=>((byte[])row.Cells.Single().Value).ToArray();
    static void Append(PARAM p,PARAM.Row donor,int id,string name,byte[] data){Need(p.Rows.All(r=>r.ID!=id),"owned row occupied");var row=new PARAM.Row(donor){ID=id,Name=name};row.Cells.Single().Value=data;int at=p.Rows.FindIndex(r=>r.ID>id);if(at<0)p.Rows.Add(row);else p.Rows.Insert(at,row);}
    static byte[] Write(PARAM p){var nulls=p.Rows.Select((r,i)=>(r.Name,i)).Where(r=>r.Name==null).Select(r=>r.i).ToArray();var bytes=p.Write();foreach(int i in nulls)Array.Clear(bytes,64+i*24+16,8);return bytes;}
    internal static byte[] Events(byte[] native,byte[] source) {
        Need(Convert.ToHexStringLower(SHA256.HashData(source))=="691516e02235c8770629d032b6512e2ce1e14844487b198a1ee88756207431ba","source trap/lever events changed");var script=EMEVD.Read(native);Need(!script.Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+10),"owned event namespace occupied");var ctor=script.Events.Single(e=>e.ID==0);
        EMEVD.Event Event(int n){var e=new EMEVD.Event(FirstEvent+n,EMEVD.Event.RestBehaviorType.Restart);script.Events.Add(e);ctor.Instructions.Add(I(2000,0,0,checked((int)e.ID),0));return e;}
        // Persistent orientation: source false=vertical/spikes, true=walkway.
        var controller=Event(0);controller.Instructions.AddRange([
            I(2005,6,Lever,696110,(byte)0),I(2005,6,Lever,696111,(byte)0),
            I(1003,1,(byte)4,(byte)0,(byte)0,F(11300403)),I(2005,7,Bridge,0),I(2005,7,Lever,2),I(2005,6,Lever,696111,(byte)1),I(1000,3,(byte)3),
            I(2005,7,Bridge,2),I(2005,7,Lever,0),I(2005,6,Lever,696110,(byte)1),
            I(3,0,(sbyte)1,(byte)1,(byte)0,F(11305037)),I(3,0,(sbyte)2,(byte)1,(byte)0,F(11305038)),I(0,0,(sbyte)-1,(byte)1,(sbyte)1),I(0,0,(sbyte)-1,(byte)1,(sbyte)2),I(0,0,(sbyte)0,(byte)1,(sbyte)-1),
            I(2003,2,F(11305037),(byte)0),I(2003,2,F(11305038),(byte)0),I(2005,6,Lever,696110,(byte)0),I(2005,6,Lever,696111,(byte)0),
            I(1000,7,(byte)5,(byte)1,(sbyte)2),I(2003,2,F(11300403),(byte)1),I(2003,18,Bridge,3,(byte)0,(byte)0,(byte)0),I(1001,0,140/30f),I(2005,6,Lever,696111,(byte)1),I(1000,4,(byte)1),
            I(2003,2,F(11300403),(byte)0),I(2003,18,Bridge,1,(byte)0,(byte)0,(byte)0),I(1001,0,140/30f),I(2005,6,Lever,696110,(byte)1),I(1000,4,(byte)1)]);
        foreach(var spec in new[]{(Offset:1,Action:LowerAction,Request:11305037),(Offset:2,Action:RaiseAction,Request:11305038)}) {
            var e=Event(spec.Offset);e.Instructions.AddRange([I(5,2,(sbyte)0,spec.Action),I(2003,2,F(spec.Request),(byte)1),I(1000,4,(byte)1)]);
        }
        foreach(var spec in new[]{(Offset:3,Trigger:11300592,State:0,Region:ApproachRegion,Request:11305037,Animation:1),(Offset:4,Trigger:11300593,State:1,Region:CrossRegion,Request:11305038,Animation:3)}) {
            var e=Event(spec.Offset);e.Instructions.Add(I(1003,2,(byte)0,(byte)1,(byte)0,F(spec.Trigger)));
            e.Instructions.AddRange([I(3,0,(sbyte)1,(byte)1,(byte)0,F(1620)),I(3,0,(sbyte)1,checked((byte)spec.State),(byte)0,F(11300403)),I(3,2,(sbyte)1,(byte)1,10000,spec.Region)]);
            if(spec.Trigger==11300593)e.Instructions.Add(I(3,0,(sbyte)1,(byte)1,(byte)0,F(11300592)));
            e.Instructions.AddRange([I(0,0,(sbyte)0,(byte)1,(sbyte)1),I(2003,2,F(spec.Trigger),(byte)1),I(2003,18,Lever,spec.Animation,(byte)0,(byte)0,(byte)0),I(2003,2,F(spec.Request),(byte)1)]);
        }
        var progress=Event(5);progress.Instructions.AddRange([I(3,0,(sbyte)1,(byte)1,(byte)0,F(1620)),I(3,0,(sbyte)1,(byte)1,(byte)0,F(11300593))]);
        foreach(int n in new[]{1622,1625,1627,1628})progress.Instructions.Add(I(3,0,(sbyte)1,(byte)0,(byte)0,F(n)));
        progress.Instructions.Add(I(0,0,(sbyte)0,(byte)1,(sbyte)1));for(int n=1620;n<=1631;n++)progress.Instructions.Add(I(2003,2,F(n),(byte)0));progress.Instructions.Add(I(2003,2,F(1621),(byte)1));
        var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);script.Events.Add(pre);}var reset=new[]{I(2003,2,F(11305037),(byte)0),I(2003,2,F(11305038),(byte)0)};foreach(var p in pre.Parameters)p.InstructionIndex+=reset.Length;pre.Instructions.InsertRange(0,reset);
        return script.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
