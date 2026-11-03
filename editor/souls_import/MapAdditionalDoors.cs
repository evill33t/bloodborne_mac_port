// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

// Authored Griggs rescue door. Its completion flag is shared intentionally
// with the independently imported source NPC transition1110→1111.
internal static class MapAdditionalDoors {
    const string Params="param/gameparam/gameparam.parambnd.dcx",Donor="o241013",NativeMap="m24_02_00_00";
    internal const int Model=696001,ActionParam=696002,Entity=4031702,ActionEntity=4031752,FirstEvent=12416700;
    internal const int SourceCompletion=11010181,OpenFlag=52411165,EligibilityFlag=52411166;
    internal static readonly IReadOnlyList<int> ReservedFlags=[OpenFlag,EligibilityFlag];
    internal static readonly IReadOnlyList<string> RequiredModels=["o1317"];
    internal sealed record Binding(string SourceName,string SourceModel,int SourceEntity,int SourceAction,int SourceCompletionFlag,int SourceMsbOpenFlag,
        string ObjectName,int Entity,int ActionEntity,int ActionParam,int OpenFlag,int EligibilityFlag,int SourceKey,int NativeKey,
        string Collision,Vector3 Position,Vector3 Rotation,float Duration,int Frames,DoorPhysics.Box Box);
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,IReadOnlyList<Binding> Bindings,
        IReadOnlySet<string> ConsumedNames,IReadOnlyList<string> Limits,IReadOnlyDictionary<string,string> NativeHashes);
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static string Hash(byte[] b)=>Convert.ToHexStringLower(SHA256.HashData(b));
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Additional doors: "+why);}
    internal static IReadOnlySet<string> SourceNames(string map,MSB1 source) {
        if(map!="m10_01_00_00")return new HashSet<string>(StringComparer.Ordinal);
        var parts=source.Parts.Objects.Where(p=>p.Name=="o1317_04").ToArray();Need(parts.Length==1&&parts[0].ModelName=="o1317"&&parts[0].EntityID==1011317,"Griggs door source placement");
        return new HashSet<string>(["o1317_04"],StringComparer.Ordinal);
    }
    static byte[] Args(params object[] args){using var s=new MemoryStream();using var w=new BinaryWriter(s);foreach(var a in args){int width=a is byte or sbyte?1:a is short or ushort?2:4;while(s.Position%width!=0)w.Write((byte)0);switch(a){case byte n:w.Write(n);break;case sbyte n:w.Write(n);break;case short n:w.Write(n);break;case int n:w.Write(n);break;case float n:w.Write(n);break;default:throw new InvalidDataException("Door event argument");}}while(s.Position%4!=0)w.Write((byte)0);return s.ToArray();}
    static EMEVD.Instruction I(int bank,int id,params object[] args)=>new(bank,id,Args(args));
    static PARAM Raw(Memory<byte> b,string type,int size,short version){var p=PARAM.Read(b);Need(p.ParamType==type&&p.DetectedSize==size&&p.ParamdefDataVersion==version&&!p.BigEndian&&p.Format2D==PARAM.FormatFlags1.LongDataOffset,"native parameter ABI "+type);var def=new PARAMDEF{ParamType=type,DataVersion=version,Unicode=true};def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=size});p.ApplyParamdef(def);return p;}
    static byte[] Payload(PARAM.Row row)=>((byte[])row.Cells.Single().Value).ToArray();
    static void Add(PARAM p,PARAM.Row donor,int id,string label,byte[] bytes){Need(!p.Rows.Any(r=>r.ID==id),"owned parameter row occupied "+id);var row=new PARAM.Row(donor){ID=id,Name=label};row.Cells.Single().Value=bytes;int at=p.Rows.FindIndex(r=>r.ID>id);if(at<0)p.Rows.Add(row);else p.Rows.Insert(at,row);}
    static byte[] Write(PARAM p){var nulls=p.Rows.Select((r,i)=>(r.Name,i)).Where(x=>x.Name==null).Select(x=>x.i).ToArray();var b=p.Write();foreach(int i in nulls)Array.Clear(b,64+i*24+16,8);return b;}
    static void Scan(ReadOnlySpan<byte> bytes,bool sharedOpen,string owner){for(int at=0;at+4<=bytes.Length;at++){int value=BitConverter.ToInt32(bytes[at..]);Need(value!=EligibilityFlag&&(sharedOpen||value!=OpenFlag),"persistent flag reservation occupied in "+owner);}}
    internal static void VerifyReservations(string nativeRoot) {
        foreach(var file in BND4.Read(Path.Combine(nativeRoot,Params)).Files)Scan(file.Bytes.Span,false,file.Name);
        foreach(var path in Directory.EnumerateFiles(Path.Combine(nativeRoot,"event"),"*.dcx")){var b=File.ReadAllBytes(path);var raw=DCX.Is(b)?DCX.Decompress(b).ToArray():b;Scan(raw,false,path);if(EMEVD.Is(raw))Need(!EMEVD.Read(raw).Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+10),"native event reservation occupied");}
        foreach(var path in Directory.EnumerateFiles(Path.Combine(nativeRoot,"script/talk"),"*.dcx"))foreach(var f in BND4.Read(path).Files)Scan(f.Bytes.Span,false,path+":"+f.Name);
    }
    internal static Result Apply(Func<string,byte[]?> sourceRead,Func<string,byte[]?> nativeRead,Func<string,byte[]?> composedRead,
        MSB1 source,MSBB destination,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,
        IReadOnlyList<MSBB.Part.Collision> slots,byte[] scriptBytes,string sourceMap,string targetMap,
        FLVER2 surface,Func<IFlverMaterial,FLVER2.Material> materialFactory,string sourceDefinitions,MSBB.Part.GparamConfigStruct? defaultLightingGparam=null) {
        var consumed=SourceNames(sourceMap,source);var files=new Dictionary<string,byte[]>();var hashes=new Dictionary<string,string>();
        if(consumed.Count==0)return new(files,scriptBytes.ToArray(),[],consumed,[],hashes);
        Need(targetMap=="m40_03_00_00"&&chunks.Count==slots.Count&&slots.Count>0&&float.IsFinite(offset.LengthSquared()),"unsupported target/collision binding");
        byte[] Source(string p)=>sourceRead(p)??throw new FileNotFoundException("Source Griggs door asset unavailable",p);
        byte[] Native(string p){var b=nativeRead(p)??throw new FileNotFoundException("Native door carrier unavailable",p);hashes.TryAdd(p,Hash(b));return b;}
        var part=source.Parts.Objects.Single(p=>p.Name=="o1317_04");var action=source.Events.ObjActs.Single(a=>a.ObjActPartName==part.Name);
        Need(part.InitAnimID==-1&&part.Scale==Vector3.One&&action.ObjActEntityID==SourceCompletion&&action.ObjActParamID==-1&&action.ObjActState==1&&action.EventFlagID==61010517,"source door/ObjAct state");
        var authoredScript=EMEVD.Read(Source("event/m10_01_00_00.emevd.dcx"));Need(authoredScript.Format==EMEVD.Game.DarkSouls1,"source event VM");var constructor=authoredScript.Events.Single(e=>e.ID==0);
        Need(constructor.Instructions.Count(i=>i.Bank==2000&&i.ID==0&&i.ArgData.SequenceEqual(Args(1,11010180,SourceCompletion,10010881,1011317)))==1,"source Griggs event binding");
        var controller=authoredScript.Events.Single(e=>e.ID==11010180);Need(controller.Instructions.Select(i=>(i.Bank,i.ID)).SequenceEqual(new[]{(1003,2),(5,2),(1003,6),(2007,1)})&&controller.Parameters.Count==3,"source successful-action event controller");
        Need(controller.Parameters.Any(p=>p.InstructionIndex==1&&p.TargetStartByte==4&&p.SourceStartByte==0&&p.ByteCount==4)&&controller.Parameters.Any(p=>p.InstructionIndex==3&&p.TargetStartByte==0&&p.SourceStartByte==4&&p.ByteCount==4)&&controller.Parameters.Any(p=>p.InstructionIndex==3&&p.TargetStartByte==8&&p.SourceStartByte==8&&p.ByteCount==4),"source action/dialog parameter mapping");
        var sourceGame=BND3.Read(Source("param/GameParam/GameParam.parambnd.dcx"));var sourceActions=PARAM.Read(sourceGame.Files.Single(f=>Leaf(f.Name)=="ObjActParam.param").Bytes);sourceActions.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(sourceDefinitions,"ObjActParam.xml")));var sourceAction=sourceActions.Rows.Single(r=>r.ID==1317);
        int S(string field)=>Convert.ToInt32(sourceAction[field].Value);
        Need(S("spQualifiedPassEventFlag")==SourceCompletion&&S("spQualifiedId")==2021&&S("spQualifiedType")==1&&S("spQualifiedId2")==0&&S("spQualifiedType2")==0&&S("objDummyId")==100&&S("objAnimId")==0&&S("playerAnimId")==7110&&S("eventKickTiming")==1,"Residence-only key/action contract (Master Key is not accepted)");
        var sourceGoods=PARAM.Read(sourceGame.Files.Single(f=>Leaf(f.Name)=="EquipParamGoods.param").Bytes);sourceGoods.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(sourceDefinitions,"EquipParamGoods.xml")));var key=sourceGoods.Rows.Single(r=>r.ID==2021);Need(Convert.ToInt32(key["goodsType"].Value)==1&&Convert.ToInt32(key["isEquip"].Value)==0&&Convert.ToInt32(key["refId"].Value)==-1&&Convert.ToInt32(key["isConsume"].Value)==1,"source Residence Key category/unused-item payload");
        var script=EMEVD.Read(scriptBytes);Need(script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+10),"owned event namespace occupied");Scan(DCX.Is(scriptBytes)?DCX.Decompress(scriptBytes).Span:scriptBytes,true,"composed events");
        var game=BND4.Read(composedRead(Params)??Native(Params));var originalGame=BND4.Read(Native(Params));var objectFile=game.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param");var objects=Raw(objectFile.Bytes,"OBJECT_PARAM_ST",32,1);var actionFile=game.Files.Single(f=>Leaf(f.Name)=="ObjActParam.param");var actions=Raw(actionFile.Bytes,"OBJ_ACT_PARAM_ST",48,2);
        Need(!PARAM.Read(game.Files.Single(f=>Leaf(f.Name)=="EquipParamGoods.param").Bytes).Rows.Any(r=>r.ID==65535),"key sentinel is a real native item");
        foreach(var file in game.Files)Scan(file.Bytes.Span,true,"composed "+file.Name);
        Need(!objects.Rows.Any(r=>r.ID==Model)&&!actions.Rows.Any(r=>r.ID==ActionParam)&&!destination.Models.Objects.Any(m=>m.Name=="o"+Model)&&nativeRead("obj/o"+Model+".objbnd.dcx")==null&&composedRead("obj/o"+Model+".objbnd.dcx")==null,"owned model/parameter asset occupied");
        var entities=destination.Parts.GetEntries().Select(p=>p.EntityID).Concat(destination.Regions.GetEntries().Select(r=>r.EntityID)).Concat(destination.Events.GetEntries().Select(e=>e.EntityID)).Concat(destination.Events.ObjActs.Select(e=>e.ObjActEntityID)).ToHashSet();Need(entities.Add(Entity)&&entities.Add(ActionEntity),"owned actor/action entity occupied");
        var nativeMap=MSBB.Read(Native("map/MapStudio/"+NativeMap+".msb.dcx"));var donorActor=nativeMap.Parts.Objects.Single(p=>p.Name=="o241013_0000");var donorAction=nativeMap.Events.ObjActs.Single(a=>a.ObjActPartName==donorActor.Name);Need(donorActor.EntityID==2421220&&donorAction.ObjActParamID==2420010&&donorAction.ObjActState==1,"native door ObjAct carrier");
        var conversion=MapAdditionalDoorsAssets.Convert(Source("obj/o1317.objbnd.dcx"),Native("obj/"+Donor+".objbnd.dcx"),surface,materialFactory);files.Add("obj/o"+Model+".objbnd.dcx",conversion.Bundle);
        var actor=(MSBB.Part.Object)donorActor.DeepCopy();actor.Name="o"+Model+"_0000";actor.ModelName="o"+Model;actor.EntityID=Entity;actor.InstanceID=destination.Parts.Objects.Select(p=>p.InstanceID).DefaultIfEmpty(-1).Max()+1;actor.Position=part.Position+offset;actor.Rotation=part.Rotation;actor.Scale=part.Scale;actor.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,part.CollisionName,actor.Position)].Name;actor.SibPath=@"N:\SPRJ\data\Model\map\"+targetMap+@"\layout\o_layout.SIB";
        Need(!destination.Parts.GetEntries().Any(p=>p.Name==actor.Name),"owned part name occupied");actor.GparamConfigStruct=(defaultLightingGparam??destination.Parts.MapPieces.FirstOrDefault(p=>p.GparamConfig.LightSetID==0)?.GparamConfig??new MSBB.Part.GparamConfigStruct()).DeepCopy();Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);Array.Fill(actor.AnimIDs,(short)-1);
        var objact=(MSBB.Event.ObjAct)donorAction.DeepCopy();objact.Name="souls_griggs_rescue_door_action";Need(!destination.Events.GetEntries().Any(e=>e.Name==objact.Name),"owned event name occupied");objact.EventID=destination.Events.GetEntries().Select(e=>e.EventID).DefaultIfEmpty(-1).Max()+1;objact.PartName=actor.CollisionName;objact.ObjActPartName=actor.Name;objact.ObjActEntityID=ActionEntity;objact.ObjActParamID=ActionParam;objact.EventFlagID=OpenFlag;objact.EntityID=-1;
        var nativeActions=Raw(originalGame.Files.Single(f=>Leaf(f.Name)=="ObjActParam.param").Bytes,"OBJ_ACT_PARAM_ST",48,2);var payload=Payload(nativeActions.Rows.Single(r=>r.ID==2420010));Need(BitConverter.ToInt32(payload,12)==101100&&payload[26]==100&&BitConverter.ToInt32(payload,28)==1&&payload[37]==1&&BitConverter.ToInt32(payload,40)==7000,"native player/object action handshake");
        BitConverter.GetBytes(10010163).CopyTo(payload,4);BitConverter.GetBytes(EligibilityFlag).CopyTo(payload,8);BitConverter.GetBytes(ushort.MaxValue).CopyTo(payload,22);Array.Clear(payload,24,2);payload[33]=1;payload[34]=0;
        Add(actions,actions.Rows.Single(r=>r.ID==2420010),ActionParam,"Imported DS1R Griggs Residence Key door",payload);Add(objects,objects.Rows.Single(r=>r.ID==241013),Model,"Imported DS1R Griggs animated rescue door",Payload(objects.Rows.Single(r=>r.ID==241013)));
        var eligibility=new EMEVD.Event(FirstEvent,EMEVD.Event.RestBehaviorType.Restart);eligibility.Instructions.AddRange([I(1003,2,(byte)0,(byte)1,(byte)0,EligibilityFlag),I(3,4,(sbyte)-1,(byte)3,MapItemPolicy.QuestID(2021),(byte)1),I(3,0,(sbyte)-1,(byte)1,(byte)0,OpenFlag),I(0,0,(sbyte)0,(byte)1,(sbyte)-1),I(2003,2,EligibilityFlag,(byte)1),I(1000,4,(byte)0)]);
        var state=new EMEVD.Event(FirstEvent+1,EMEVD.Event.RestBehaviorType.Restart);state.Instructions.AddRange([I(1003,1,(byte)3,(byte)0,(byte)0,OpenFlag),I(2005,7,Entity,1),I(2005,6,Entity,-1,(byte)0),I(1000,4,(byte)0),I(5,2,(sbyte)0,ActionEntity),I(2003,2,OpenFlag,(byte)1),I(2005,6,Entity,-1,(byte)0),I(1000,4,(byte)0)]);
        script.Events.Add(eligibility);script.Events.Add(state);script.Events.Single(e=>e.ID==0).Instructions.AddRange([I(2000,0,0,FirstEvent,0),I(2000,0,0,FirstEvent+1,0)]);
        objectFile.Bytes=Write(objects);actionFile.Bytes=Write(actions);files.Add(Params,game.Write(DCX.Type.DCX_DFLT_10000_44_9));var model=(MSBB.Model.Object)nativeMap.Models.Objects.Single(m=>m.Name==Donor).DeepCopy();model.Name="o"+Model;model.SibPath=@"N:\SPRJ\data\Model\obj\o"+Model+@"\sib\o"+Model+".sib";
        void Append(MSBB map){map.Models.Objects.Add(model);map.Parts.Objects.Add(actor);map.Events.ObjActs.Add(objact);}var trial=MSBB.Read(destination.Write());Append(trial);_=MSBB.Read(trial.Write());Append(destination);
        var binding=new Binding(part.Name,part.ModelName,part.EntityID,action.ObjActEntityID,SourceCompletion,action.EventFlagID,actor.Name,Entity,ActionEntity,ActionParam,OpenFlag,EligibilityFlag,2021,MapItemPolicy.QuestID(2021),actor.CollisionName,actor.Position,actor.Rotation,conversion.Duration,conversion.Frames,conversion.Box);
        return new(files,script.Write(DCX.Type.DCX_DFLT_10000_44_9),[binding],consumed,[
            "Griggs' actual Residence-only lock, source swing, hinge and collision box are retained. Master Key is intentionally not accepted, matching source ObjAct1317.",
            "The native o241013 object-action/player alignment, sound cues, collision material and solver metadata remain Bloodborne's. Source key-used dialog is not transplanted; native Open/locked prompts remain.",
            "Imported Residence Key902021 must be supplied by the existing item/merchant exporter, whose key rows set isConsume0. The eligibility bridge is monotonic and never grants NPC quest states directly. Source goods2021 itself has isConsume1 but isEquip0/refId-1; that source item bit is not evidence of door-use consumption.",
            "Persistent open flag52411165 represents successful source action/event11010181 and source MSB door-open state61010517. Griggs rescue progression remains owned by the NPC quest adapter.",
            "Source marker100 is retained for the front action. Opposite-side alignment, player timing and dynamic collision require live gameplay validation. Other keyed doors and gates remain separate mechanisms."
        ],hashes);
    }
}
