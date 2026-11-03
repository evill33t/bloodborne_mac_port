// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

// Verified Havel tower doors: authored source swing and convex leaf collision,
// with the shipped Bloodborne object-action/player-interaction controller.
internal static class MapDoors {
    const string Donor="o241013",Params="param/gameparam/gameparam.parambnd.dcx",NativeMap="m24_02_00_00";
    const int Model=696000,FirstEvent=12415940,Eligibility=52100970;
    internal static readonly IReadOnlyList<int> ReservedFlags=Enumerable.Range(Eligibility,10).ToArray();
    internal record Binding(string SourceName,int SourceEntity,int SourceAction,int SourceOpenFlag,string ObjectName,int Entity,
        int ActionEntity,int ActionParam,int OpenFlag,int EligibilityFlag,string Collision,Vector3 Position,Vector3 Rotation,
        float Duration,DoorPhysics.Box Box,IReadOnlyList<int> SourceKeys,IReadOnlyList<int> NativeKeys);
    internal record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,IReadOnlyList<Binding> Bindings,
        IReadOnlySet<string> ConsumedNames,IReadOnlyList<string> Limits,IReadOnlyDictionary<string,string> NativeHashes);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] b)=>Convert.ToHexStringLower(SHA256.HashData(b));
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Imported door: "+why);}
    static readonly string[] Limits=[
        "Source Havel door swing and convex leaf dimensions are retained. Native object-action/player alignment, collision material, inertia metadata and door sounds remain the Bloodborne single-leaf donor.",
        "The upper decorative arch retains its static source visual; its separate source rigid box is not emitted. Existing source structural map collision remains responsible for that overhead opening.",
        "The native donor's front action uses marker100. Opposite-side prompt/alignment requires gameplay verification before claiming source two-sided interaction.",
        "Only Havel's two Watchtower Basement Key/Master Key doors are adapted here; unrelated Burg/Parish gates, levers, locked rooms and lifts need their own source mechanism contracts."
    ];
    internal static IReadOnlySet<string> SourceNames(string sourceMap,MSB1 source) {
        if(sourceMap!="m10_01_00_00")return new HashSet<string>(StringComparer.Ordinal);
        var names=new HashSet<string>(StringComparer.Ordinal);
        foreach(var (name,entity) in new[]{("o1305_01",1011315),("o1305_02",1011316)}) {
            var parts=source.Parts.Objects.Where(p=>p.Name==name).ToArray();Need(parts.Length==1&&parts[0].ModelName=="o1305"&&parts[0].EntityID==entity,"source Havel door placement changed: "+name);names.Add(name);
        }
        return names;
    }
    static byte[] Args(params object[] values) {
        using var s=new MemoryStream();using var w=new BinaryWriter(s);
        foreach(var v in values){int size=v is byte or sbyte?1:v is short or ushort?2:4;while(s.Position%size!=0)w.Write((byte)0);switch(v){case byte n:w.Write(n);break;case sbyte n:w.Write(n);break;case short n:w.Write(n);break;case int n:w.Write(n);break;default:throw new InvalidDataException("Unsupported door event argument");}}while(s.Position%4!=0)w.Write((byte)0);return s.ToArray();
    }
    static EMEVD.Instruction Ins(int bank,int id,params object[] values)=>new(bank,id,Args(values));
    static string Fingerprint(EMEVD.Event e) {
        using var s=new MemoryStream();using var w=new BinaryWriter(s);w.Write(e.ID);w.Write((int)e.RestBehavior);w.Write(e.Instructions.Count);w.Write(e.Parameters.Count);
        foreach(var i in e.Instructions){w.Write(i.Bank);w.Write(i.ID);w.Write(i.Layer is {} l?(long)l:-1L);w.Write(i.ArgData.Length);w.Write(i.ArgData);}
        foreach(var p in e.Parameters){w.Write((long)p.InstructionIndex);w.Write((long)p.TargetStartByte);w.Write((long)p.SourceStartByte);w.Write((long)p.ByteCount);w.Write((long)p.UnkID);}return Hash(s.ToArray());
    }

    static void Scan(ReadOnlySpan<byte> b,string owner) {
        for(int at=0;at+4<=b.Length;at++){int n=BitConverter.ToInt32(b[at..]);Need(n<Eligibility||n>=Eligibility+10,"persistent flag reservation occupied in "+owner);}
    }
    internal static void VerifyReservations(string nativeRoot) {
        foreach(var f in BND4.Read(Path.Combine(nativeRoot,Params)).Files)Scan(f.Bytes.Span,f.Name);
        foreach(string path in Directory.EnumerateFiles(Path.Combine(nativeRoot,"event"),"*.dcx")){
            var b=File.ReadAllBytes(path);var raw=DCX.Is(b)?DCX.Decompress(b).ToArray():b;Scan(raw,path);
            if(EMEVD.Is(raw))Need(!EMEVD.Read(raw).Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+20),"native door event reservation occupied");
        }
        foreach(string path in Directory.EnumerateFiles(Path.Combine(nativeRoot,"script/talk"),"*.dcx"))foreach(var f in BND4.Read(path).Files)Scan(f.Bytes.Span,path+":"+f.Name);
    }
    static PARAM Rows(Memory<byte> bytes,string type,int size,short version) {
        var p=PARAM.Read(bytes);Need(p.ParamType==type&&p.DetectedSize==size&&p.ParamdefDataVersion==version&&!p.BigEndian&&p.Format2D==PARAM.FormatFlags1.LongDataOffset,"native parameter schema changed: "+type);
        var def=new PARAMDEF{ParamType=type,DataVersion=version,Unicode=true};def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=size});p.ApplyParamdef(def);return p;
    }
    static byte[] Payload(PARAM.Row r)=>((byte[])r.Cells.Single().Value).ToArray();
    static void AddRow(PARAM p,PARAM.Row donor,int id,string name,byte[] b){Need(p.Rows.All(r=>r.ID!=id),"owned parameter row occupied: "+id);var r=new PARAM.Row(donor){ID=id,Name=name};r.Cells.Single().Value=b;int at=p.Rows.FindIndex(r=>r.ID>id);if(at<0)p.Rows.Add(r);else p.Rows.Insert(at,r);}
    static byte[] WriteRows(PARAM p){var nulls=p.Rows.Select((r,i)=>(r.Name,i)).Where(x=>x.Name==null).Select(x=>x.i).ToArray();var b=p.Write();foreach(int i in nulls)Array.Clear(b,64+i*24+16,8);return b;}
    internal static Result Apply(Func<string,byte[]?> sourceRead,Func<string,byte[]?> nativeRead,Func<string,byte[]?> composedRead,
        MSB1 source,MSBB destination,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,
        IReadOnlyList<MSBB.Part.Collision> slots,byte[] scriptBytes,string sourceMap,string targetMap,
        FLVER2 surface,Func<IFlverMaterial,FLVER2.Material> materialFactory,MSBB.Part.GparamConfigStruct? defaultLightingGparam=null) {
        var consumed=SourceNames(sourceMap,source);var files=new Dictionary<string,byte[]>();var hashes=new Dictionary<string,string>();var bindings=new List<Binding>();
        if(consumed.Count==0)return new(files,scriptBytes.ToArray(),bindings,consumed,[],hashes);
        Need(sourceMap=="m10_01_00_00"&&targetMap=="m40_03_00_00"&&chunks.Count==slots.Count&&slots.Count>0&&float.IsFinite(offset.LengthSquared()),"unsupported destination/collision bindings");
        byte[] Source(string p)=>sourceRead(p)??throw new FileNotFoundException("Source door resource missing",p);
        byte[] Native(string p){var b=nativeRead(p)??throw new FileNotFoundException("Native door resource missing",p);hashes.TryAdd(p,Hash(b));return b;}
        var script=EMEVD.Read(scriptBytes);Need(script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+20),"target door event namespace occupied");Scan(DCX.Is(scriptBytes)?DCX.Decompress(scriptBytes).Span:scriptBytes,"composed event");
        var game=BND4.Read(composedRead(Params)??Native(Params));foreach(var f in game.Files)Scan(f.Bytes.Span,"composed "+f.Name);
        var originalGame=BND4.Read(Native(Params));
        var objectFile=game.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param");var objects=Rows(objectFile.Bytes,"OBJECT_PARAM_ST",32,1);
        var actionFile=game.Files.Single(f=>Leaf(f.Name)=="ObjActParam.param");var actions=Rows(actionFile.Bytes,"OBJ_ACT_PARAM_ST",48,2);
        Need(PARAM.Read(game.Files.Single(f=>Leaf(f.Name)=="EquipParamGoods.param").Bytes).Rows.All(r=>r.ID!=65535),"key eligibility sentinel is a real native good");
        Need(objects.Rows.All(r=>r.ID!=Model)&&destination.Models.Objects.All(m=>m.Name!="o"+Model)&&nativeRead("obj/o"+Model+".objbnd.dcx")==null&&composedRead("obj/o"+Model+".objbnd.dcx")==null,"owned animated door asset occupied");
        var sourceScript=EMEVD.Read(Source("event/m10_01_00_00.emevd.dcx"));var sourceCtor=sourceScript.Events.Single(e=>e.ID==0);
        Need(sourceScript.Format==EMEVD.Game.DarkSouls1&&sourceScript.Events.Count(e=>e.ID==11010190)==1&&Fingerprint(sourceScript.Events.Single(e=>e.ID==11010190))=="b0a8d3ded92add2b0405399f8b6de3738365a783d9413f779191ad5a97b333f6","source keyed-door event changed");
        var sourceBytes=Source("obj/o1305.objbnd.dcx");Need(Hash(sourceBytes)=="e8db7760a5277e35007193cd02ccd6895b9065785bf52d73e53aa3f9cb266479","source Havel door object changed");
        var sourceBank=BND3.Read(sourceBytes);var sourceVisual=FLVER2.Read(sourceBank.Files.Single(f=>f.ID==200).Bytes);
        var donorBytes=Native("obj/"+Donor+".objbnd.dcx");var donor=BND4.Read(donorBytes);var originalVisual=FLVER2.Read(donor.Files.Single(f=>f.ID==200).Bytes);
        Need(originalVisual.Nodes.Count==2&&originalVisual.Nodes[0].Name==Donor&&originalVisual.Nodes[1].Name==Donor+"_00"&&originalVisual.Meshes.All(m=>!m.UseBoneWeights),"native animated door visual rig changed");
        var visual=MapBreakables.RetargetVisual(sourceVisual,originalVisual,surface,"DS1R",Vector3.One,new Dictionary<int,int>{{1,1},{2,0}},materialFactory);
        foreach(var d in visual.Dummies){var from=sourceVisual.Dummies.Single(x=>x.ReferenceID==d.ReferenceID);Need(from.AttachBoneIndex==-1&&from.ParentBoneIndex is 0 or 1,"source door action marker contract changed");d.Position=from.Position;d.Forward=from.Forward;d.Upward=from.Upward;d.ParentBoneIndex=from.ParentBoneIndex;}
        var box=DoorPhysics.Source(sourceBank.Files.Single(f=>f.ID==300).Bytes.ToArray());
        var motion=DoorAnimations.Convert(sourceBank.Files.Single(f=>f.ID==400).Bytes.ToArray(),donor.Files.Single(f=>f.ID==400).Bytes.ToArray());
        donor.Files.Single(f=>f.ID==200).Bytes=visual.Write();donor.Files.Single(f=>f.ID==300).Bytes=DoorPhysics.Fit(donor.Files.Single(f=>f.ID==300).Bytes.ToArray(),box);donor.Files.Single(f=>f.ID==400).Bytes=motion.Bundle;
        foreach(var f in donor.Files)f.Name=f.Name.Replace(Donor,"o"+Model,StringComparison.Ordinal);files.Add("obj/o"+Model+".objbnd.dcx",donor.Write(DCX.Type.DCX_DFLT_10000_44_9));
        AddRow(objects,objects.Rows.Single(r=>r.ID==241013),Model,"Imported DS1R animated Havel door",Payload(objects.Rows.Single(r=>r.ID==241013)));
        var nativeMap=MSBB.Read(Native("map/MapStudio/"+NativeMap+".msb.dcx"));var donorActor=nativeMap.Parts.Objects.Single(p=>p.Name=="o241013_0000");var donorAction=nativeMap.Events.ObjActs.Single(e=>e.ObjActPartName==donorActor.Name);
        Need(donorActor.EntityID==2421220&&donorAction.ObjActParamID==2420010&&donorAction.ObjActState==1,"native object action/controller changed");
        var nativeActionRows=Rows(originalGame.Files.Single(f=>Leaf(f.Name)=="ObjActParam.param").Bytes,"OBJ_ACT_PARAM_ST",48,2);var actionDonor=nativeActionRows.Rows.Single(r=>r.ID==2420010);var actionPayload=Payload(actionDonor);
        Need(BitConverter.ToInt32(actionPayload,12)==101100&&actionPayload[26]==100&&BitConverter.ToInt32(actionPayload,28)==1&&actionPayload[37]==1&&BitConverter.ToInt32(actionPayload,40)==7000,"native door player/action animation handshake changed");
        var actorNames=destination.Parts.GetEntries().Select(p=>p.Name).ToHashSet(StringComparer.Ordinal);var eventNames=destination.Events.GetEntries().Select(p=>p.Name).ToHashSet(StringComparer.Ordinal);
        var entities=destination.Parts.GetEntries().Select(p=>p.EntityID).Concat(destination.Regions.GetEntries().Select(p=>p.EntityID)).Concat(destination.Events.GetEntries().Select(p=>p.EntityID)).Concat(destination.Events.ObjActs.Select(e=>e.ObjActEntityID)).ToHashSet();
        var pendingActors=new List<MSBB.Part.Object>();var pendingActions=new List<MSBB.Event.ObjAct>();int eventIndex=destination.Events.GetEntries().Select(e=>e.EventID).DefaultIfEmpty(-1).Max()+1;
        for(int ordinal=0;ordinal<2;ordinal++){
            string sourceName=ordinal==0?"o1305_01":"o1305_02";var part=source.Parts.Objects.Single(p=>p.Name==sourceName);int entity=4031700+ordinal,actionEntity=4031750+ordinal,openFlag=Eligibility+1+ordinal,actionID=Model+ordinal,id=FirstEvent+1+ordinal;
            Need(entities.Add(entity)&&entities.Add(actionEntity)&&actorNames.Add("o"+Model+"_"+ordinal.ToString("D4")),"owned door actor/action entity occupied");
            int sourceAction=11010191+ordinal,sourceOpen=61010504+ordinal;var authored=source.Events.ObjActs.Single(e=>e.ObjActPartName==sourceName);
            Need(authored.ObjActEntityID==sourceAction&&authored.EventFlagID==sourceOpen&&authored.ObjActState==1&&authored.ObjActParamID==(ordinal==0?-1:11305)&&part.InitAnimID==-1&&part.Scale==Vector3.One,"source keyed-door event/transform changed");
            Need(sourceCtor.Instructions.Count(i=>i.Bank==2000&&i.ID==0&&i.ArgData.AsSpan().SequenceEqual(Args(ordinal+1,11010190,sourceAction,10010878,1011315+ordinal,10010883,2019)))==1,"source keyed-door constructor changed");
            var actor=(MSBB.Part.Object)donorActor.DeepCopy();actor.Name="o"+Model+"_"+ordinal.ToString("D4");actor.ModelName="o"+Model;actor.EntityID=entity;actor.InstanceID=destination.Parts.Objects.Select(p=>p.InstanceID).DefaultIfEmpty(-1).Max()+1+ordinal;
            actor.Position=part.Position+offset;actor.Rotation=part.Rotation;actor.Scale=part.Scale;actor.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,part.CollisionName,actor.Position)].Name;actor.SibPath=@"N:\SPRJ\data\Model\map\"+targetMap+@"\layout\o_layout.SIB";
            actor.GparamConfigStruct=(defaultLightingGparam??destination.Parts.MapPieces.FirstOrDefault(p=>p.GparamConfig.LightSetID==0)?.GparamConfig??new MSBB.Part.GparamConfigStruct()).DeepCopy();Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);Array.Fill(actor.AnimIDs,(short)-1);pendingActors.Add(actor);
            var action=(MSBB.Event.ObjAct)donorAction.DeepCopy();action.Name="souls_havel_door_action_"+ordinal;Need(eventNames.Add(action.Name),"owned door action name occupied");action.EventID=eventIndex++;action.PartName=actor.CollisionName;action.ObjActPartName=actor.Name;action.ObjActEntityID=actionEntity;action.ObjActParamID=actionID;action.EventFlagID=openFlag;action.EntityID=-1;pendingActions.Add(action);
            var payload=actionPayload.ToArray();BitConverter.GetBytes(10010163).CopyTo(payload,4);BitConverter.GetBytes(Eligibility).CopyTo(payload,8);BitConverter.GetBytes(ushort.MaxValue).CopyTo(payload,22);Array.Clear(payload,24,2);payload[33]=1;payload[34]=0;AddRow(actions,actions.Rows.Single(r=>r.ID==2420010),actionID,"Imported DS1R Havel door "+sourceName,payload);
            var state=new EMEVD.Event(id,EMEVD.Event.RestBehaviorType.Restart);state.Instructions.AddRange([
                Ins(1003,1,(byte)3,(byte)0,(byte)0,openFlag), // unopened -> listener
                Ins(2005,7,entity,1),Ins(2005,6,entity,-1,(byte)0),Ins(1000,4,(byte)0),
                Ins(5,2,(sbyte)0,actionEntity),Ins(2003,2,openFlag,(byte)1),Ins(2005,6,entity,-1,(byte)0),Ins(1000,4,(byte)0)
            ]);script.Events.Add(state);script.Events.Single(e=>e.ID==0).Instructions.Add(Ins(2000,0,0,id,0));
            bindings.Add(new(sourceName,part.EntityID,sourceAction,sourceOpen,actor.Name,entity,actionEntity,actionID,openFlag,Eligibility,actor.CollisionName,actor.Position,actor.Rotation,motion.Duration,box,[2019,2100],[MapItemPolicy.QuestID(2019),MapItemPolicy.QuestID(2100)]));
        }
        var keys=new EMEVD.Event(FirstEvent,EMEVD.Event.RestBehaviorType.Restart);keys.Instructions.AddRange([
            Ins(1003,2,(byte)0,(byte)1,(byte)0,Eligibility),Ins(3,4,(sbyte)-1,(byte)3,MapItemPolicy.QuestID(2019),(byte)1),Ins(3,4,(sbyte)-1,(byte)3,MapItemPolicy.QuestID(2100),(byte)1),Ins(0,0,(sbyte)0,(byte)1,(sbyte)-1),Ins(2003,2,Eligibility,(byte)1),Ins(1000,4,(byte)0)
        ]);script.Events.Add(keys);script.Events.Single(e=>e.ID==0).Instructions.Add(Ins(2000,0,0,FirstEvent,0));
        objectFile.Bytes=WriteRows(objects);actionFile.Bytes=WriteRows(actions);files.Add(Params,game.Write(DCX.Type.DCX_DFLT_10000_44_9));
        var outputModel=(MSBB.Model.Object)nativeMap.Models.Objects.Single(m=>m.Name==Donor).DeepCopy();outputModel.Name="o"+Model;outputModel.SibPath=@"N:\SPRJ\data\Model\obj\o"+Model+@"\sib\o"+Model+".sib";
        void Append(MSBB m){m.Models.Objects.Add(outputModel);m.Parts.Objects.AddRange(pendingActors);m.Events.ObjActs.AddRange(pendingActions);}
        var trial=MSBB.Read(destination.Write());Append(trial);_=MSBB.Read(trial.Write());Append(destination);
        return new(files,script.Write(DCX.Type.DCX_DFLT_10000_44_9),bindings,consumed,Limits,hashes);
    }
}
