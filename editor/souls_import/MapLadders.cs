// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.RegularExpressions;

// The source visual remains a map piece. An owned native invisible ladder adds
// the five authored interaction endpoints and the engine's climbing controller.
internal static class MapLadders {
    const string Params="param/gameparam/gameparam.parambnd.dcx",Donor="o256000";
    internal static readonly IReadOnlyList<int> ReservedFlags=Enumerable.Range(52100900,26).ToArray();
    static readonly HashSet<int> FlagSet=ReservedFlags.ToHashSet();
    internal record Marker(short ReferenceID,Vector3 Position,Vector3 Forward,Vector3 Upward);
    internal record Binding(string SourceName,string SourceModel,int SourceEntity,string Model,string ObjectName,int Entity,
        int SourceStartFlag,int SourceStopFlag,int StartFlag,int StopFlag,string Collision,Vector3 Position,Vector3 Rotation,
        IReadOnlyList<Marker> Markers,float Height,float DescendPromptOffset=0);
    internal record Omitted(string SourceName,string Model,int Entity,string Reason);
    internal record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,IReadOnlyList<Binding> Bindings,
        IReadOnlySet<string> ConsumedNames,IReadOnlyList<Omitted> Skipped,IReadOnlyList<string> Limits,IReadOnlyDictionary<string,string> NativeHashes);
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] bytes)=>Convert.ToHexStringLower(SHA256.HashData(bytes));
    static bool Finite(Vector3 v)=>float.IsFinite(v.X)&&float.IsFinite(v.Y)&&float.IsFinite(v.Z);
    static byte[] Pack(params int[] values)=>values.SelectMany(BitConverter.GetBytes).ToArray();
    static readonly string[] Limits=[
        "Source static ladder meshes and map H/L collision are retained. Owned native dummy-only objects add climbing; no replacement stairs or unrelated ladder geometry are emitted.",
        "The o1208 kick-down shortcut is not registered or unlocked: its source object animation, action and persistent gate need a separate adapter.",
        "The checked Taurus o1209 upper prompt marker194 is lowered10cm: the native5010 cylinder otherwise starts6.3cm above the accessible platform. Motion markers191/192 and the14-rung native traversal count are retained; this is a source-specific contact adjustment.",
        "Source object HKX rail collision, AI ladder navigation and multiplayer ladder synchronization are not converted. The native invisible ladder donor intentionally has no HKX body."
    ];
    static PARAM Rows(Memory<byte> bytes,bool native) {
        var p=PARAM.Read(bytes);int size=native?32:16;
        if(p.ParamType!="OBJECT_PARAM_ST"||p.DetectedSize!=size||p.ParamdefDataVersion!=1||p.BigEndian||native&&p.Format2D!=PARAM.FormatFlags1.LongDataOffset)
            throw new InvalidDataException("Unsupported ladder ObjectParam schema");
        var def=new PARAMDEF{ParamType=p.ParamType,DataVersion=1,Unicode=native};
        def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=size});p.ApplyParamdef(def);return p;
    }
    static byte[] Payload(PARAM.Row r)=>(byte[])r.Cells.Single().Value;
    static void CheckDescendPrompt(Memory<byte> bytes) {
        var p=PARAM.Read(bytes);
        if(p.ParamType!="ACTIONBUTTON_PARAM_ST"||p.DetectedSize!=72||p.ParamdefDataVersion!=1||p.BigEndian||p.Format2D!=PARAM.FormatFlags1.LongDataOffset)
            throw new InvalidDataException("Unsupported native ladder ActionButtonParam schema");
        var def=new PARAMDEF{ParamType=p.ParamType,DataVersion=1,Unicode=true};
        def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=72});p.ApplyParamdef(def);
        var row=p.Rows.SingleOrDefault(r=>r.ID==5010)??throw new InvalidDataException("Native ladder descend prompt5010 missing");var b=Payload(row);
        // Retail Regist_LadderAction registers5010 for descent. Its cylinder
        // tests the same physics position exposed by the native observer:
        // marker194.Y-1 < player.Y < marker194.Y+1, distanceXZ <1.7.
        if(Hash(b)!="402df470d9f2fba837c129b410b685a06cff8db08c13a0264dfe307bcbeafaa6"||
            b[0]!=0||BitConverter.ToInt32(b,4)!=194||BitConverter.ToInt32(b,8)!=-1||BitConverter.ToSingle(b,12)!=1.7f||
            BitConverter.ToInt32(b,16)!=90||BitConverter.ToSingle(b,28)!=2||BitConverter.ToSingle(b,32)!=-1||
            b[36]!=0||BitConverter.ToInt32(b,40)!=90||BitConverter.ToInt32(b,52)!=-1||BitConverter.ToInt32(b,56)!=-1)
            throw new InvalidDataException("Native ladder descend prompt5010 contract changed");
    }
    static int Rungs(FLVER2 visual)=>(int)(2*MathF.Abs(visual.Dummies.Single(d=>d.ReferenceID==194).Position.Y-
        visual.Dummies.Single(d=>d.ReferenceID==193).Position.Y)-3);
    static string Fingerprint(EMEVD.Event e) {
        using var stream=new MemoryStream();using var w=new BinaryWriter(stream);
        w.Write((uint)e.RestBehavior);w.Write(e.Instructions.Count);
        foreach(var i in e.Instructions){w.Write(i.Bank);w.Write(i.ID);w.Write(i.Layer is {} l?(long)l:-1L);w.Write(i.ArgData.Length);w.Write(i.ArgData);}
        return Hash(stream.ToArray());
    }
    static void Scan(ReadOnlySpan<byte> bytes,string owner) {
        for(int at=0;at+4<=bytes.Length;++at)if(FlagSet.Contains(System.Buffers.Binary.BinaryPrimitives.ReadInt32LittleEndian(bytes[at..])))
            throw new InvalidDataException("Owned ladder flag is occupied: "+owner);
    }
    internal static void VerifyReservations(string nativeRoot) {
        foreach(var f in BND4.Read(Path.Combine(nativeRoot,Params)).Files)Scan(f.Bytes.Span,f.Name);
        foreach(string folder in new[]{"event","script/talk"})foreach(string path in Directory.EnumerateFiles(Path.Combine(nativeRoot,folder),"*.dcx",SearchOption.AllDirectories)) {
            var bytes=File.ReadAllBytes(path);
            if(BND4.Is(bytes))foreach(var f in BND4.Read(bytes).Files)Scan(DCX.Is(f.Bytes.ToArray())?DCX.Decompress(f.Bytes).Span:f.Bytes.Span,path+":"+f.Name);
            else Scan(DCX.Is(bytes)?DCX.Decompress(bytes).Span:bytes,path);
        }
    }

    internal static Result Apply(Func<string,byte[]?> sourceRead,Func<string,byte[]?> nativeRead,Func<string,byte[]?> composedRead,
        MSB1 source,MSBB destination,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,
        IReadOnlyList<MSBB.Part.Collision> slots,byte[] scriptBytes,string sourceMap,string targetMap) {
        if(sourceMap is not("m10_01_00_00" or "m10_02_00_00"))throw new NotSupportedException("Ladders currently support DS1R Firelink/Burg");
        if(!Regex.IsMatch(targetMap,@"^m\d{2}_\d{2}_00_00$")||!Finite(offset))throw new InvalidDataException("Invalid ladder destination");
        var files=new Dictionary<string,byte[]>();var hashes=new Dictionary<string,string>();var consumed=new HashSet<string>(StringComparer.Ordinal);
        var bindings=new List<Binding>();var skipped=new List<Omitted>();
        byte[] Source(string path)=>sourceRead(path)??throw new FileNotFoundException("Source ladder dependency missing",path);
        byte[] Native(string path){var b=nativeRead(path)??throw new FileNotFoundException("Native ladder dependency missing",path);hashes.TryAdd(path,Hash(b));return b;}
        var sourceRows=Rows(BND3.Read(Source("param/GameParam/GameParam.parambnd.dcx")).Files.Single(f=>Leaf(f.Name)=="ObjectParam.param").Bytes,false);
        var ladderModels=sourceRows.Rows.Where(r=>(Payload(r)[9]&0x80)!=0).Select(r=>r.ID).ToHashSet();
        var candidates=source.Parts.Objects.Where(p=>p.ModelName!=null&&int.TryParse(p.ModelName[1..],out int id)&&ladderModels.Contains(id)).ToArray();
        if(candidates.Length==0)return new(files,scriptBytes.ToArray(),bindings,consumed,skipped,[],hashes);
        if(sourceMap!="m10_01_00_00"||candidates.Length>26||chunks.Count!=slots.Count||slots.Count==0||
            slots.Any(s=>!destination.Parts.Collisions.Any(c=>c.Name==s.Name)))throw new InvalidDataException("Invalid ladder source/collision bindings");
        var sourceScript=EMEVD.Read(Source("event/"+sourceMap+".emevd.dcx"));
        var registration=sourceScript.Events.Single(e=>e.ID==11010008);
        if(sourceScript.Format!=EMEVD.Game.DarkSouls1||registration.Parameters.Count!=0||
            Fingerprint(registration)!="d53bf3644c639bcabbba712a024a157f4709375790c1434579ae3ec7d923d728"||
            !sourceScript.Events.Single(e=>e.ID==0).Instructions.Any(i=>i.Bank==2000&&i.ID==0&&i.Layer==null&&i.ArgData.AsSpan().SequenceEqual(Pack(0,11010008,0))))
            throw new InvalidDataException("Source unconditional ladder registration changed");
        var nativeMap=MSBB.Read(Native("map/MapStudio/m25_00_00_00.msb.dcx"));
        var actorTemplate=nativeMap.Parts.Objects.Single(p=>p.EntityID==2501502&&p.ModelName==Donor);
        var nativeScript=EMEVD.Read(Native("event/m25_00_00_00.emevd.dcx"));
        if(!nativeScript.Events.Single(e=>e.ID==0).Instructions.Any(i=>i.Bank==2009&&i.ID==0&&i.Layer==null&&i.ArgData.AsSpan().SequenceEqual(Pack(12500992,12500993,2501502))))
            throw new InvalidDataException("Native ladder registration contract changed");
        var donorBytes=Native("obj/"+Donor+".objbnd.dcx");var donorBundle=BND4.Read(donorBytes);
        if(donorBundle.Files.Count!=1||Leaf(donorBundle.Files[0].Name)!=Donor+".flver")throw new InvalidDataException("Native invisible ladder asset changed");
        var donorVisual=FLVER2.Read(donorBundle.Files[0].Bytes);
        if(donorVisual.Meshes.Count!=0||donorVisual.Nodes.Count!=1||donorVisual.Nodes[0].ParentIndex!=-1||donorVisual.Dummies.Count!=5||
            !donorVisual.Dummies.Select(d=>(int)d.ReferenceID).Order().SequenceEqual(Enumerable.Range(191,5)))
            throw new InvalidDataException("Unsupported native invisible ladder topology");
        var nativeGame=BND4.Read(Native(Params));CheckDescendPrompt(nativeGame.Files.Single(f=>Leaf(f.Name)=="ActionButtonParam.param").Bytes);
        var game=BND4.Read(composedRead(Params)??Native(Params));foreach(var f in game.Files)Scan(f.Bytes.Span,f.Name);
        CheckDescendPrompt(game.Files.Single(f=>Leaf(f.Name)=="ActionButtonParam.param").Bytes);
        var paramFile=game.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param");var rows=Rows(paramFile.Bytes,true);var donorRow=rows.Rows.Single(r=>r.ID==256000);
        if((Payload(donorRow)[9]&0x80)==0||(Payload(donorRow)[9]&0x30)!=0)throw new InvalidDataException("Native dummy ladder collision/type flags changed");
        var script=EMEVD.Read(scriptBytes);
        if(script.Format!=EMEVD.Game.Bloodborne)throw new InvalidDataException("Ladders require a Bloodborne target script");
        Scan(DCX.Is(scriptBytes)?DCX.Decompress(scriptBytes).Span:scriptBytes,"composed target script");
        var ctor=script.Events.Single(e=>e.ID==0);
        var entities=destination.Parts.GetEntries().Select(p=>p.EntityID).Concat(destination.Events.GetEntries().Select(e=>e.EntityID))
            .Concat(destination.Regions.GetEntries().Select(r=>r.EntityID)).ToHashSet();
        var pendingActors=new List<MSBB.Part.Object>();var pendingModels=new List<MSBB.Model.Object>();
        int code=int.Parse(targetMap.AsSpan(1,2))*10+int.Parse(targetMap.AsSpan(4,2));
        if(!Matrix4x4.Invert(donorVisual.Nodes[0].ComputeLocalTransform(),out var donorInverse))throw new InvalidDataException("Singular native ladder dummy parent");
        foreach(var part in candidates.OrderBy(p=>p.EntityID)) {
            var commands=registration.Instructions.Where(i=>i.Bank==2009&&i.ID==0&&i.ArgData.Length==12&&BitConverter.ToInt32(i.ArgData,8)==part.EntityID).ToArray();
            if(commands.Length==0){skipped.Add(new(part.Name,part.ModelName,part.EntityID,"Source ladder is not unconditionally registered. The o1208 sliding/kick-down event11010100 and its persistent unlock/action/object animation are not converted; source static visual retained, native climbing remains unregistered."));continue;}
            if(commands.Length!=1||part.EntityID is <1011140 or >1011152||part.InitAnimID!=-1||part.Scale!=Vector3.One||
                !Finite(part.Position)||!Finite(part.Rotation)||Math.Abs(part.Rotation.X)>.001f||Math.Abs(part.Rotation.Z)>.001f)
                throw new InvalidDataException("Unsupported source fixed ladder placement: "+part.Name);
            int sourceStart=BitConverter.ToInt32(commands[0].ArgData),sourceStop=BitConverter.ToInt32(commands[0].ArgData,4);
            if(sourceStart!=11010010+(part.EntityID-1011140)*2||sourceStop!=sourceStart+1)throw new InvalidDataException("Source ladder flag identity changed");
            int ordinal=part.EntityID-1011140,modelID=695000+ordinal,entity=code*10000+1140+ordinal;
            string model="o"+modelID,name=model+"_0000",path="obj/"+model+".objbnd.dcx";
            if(!entities.Add(entity)||rows.Rows.Any(r=>r.ID==modelID)||destination.Models.Objects.Any(m=>m.Name==model)||
                destination.Parts.GetEntries().Any(p=>p.Name==name)||nativeRead(path)!=null||composedRead(path)!=null)
                throw new InvalidDataException("Owned ladder model/entity reservation is occupied: "+part.Name);
            byte[] sourceBundleBytes=Source("obj/"+part.ModelName+".objbnd.dcx");var sourceBundle=BND3.Read(sourceBundleBytes);
            if(sourceBundle.Files.Any(f=>f.Name.EndsWith(".anibnd",StringComparison.OrdinalIgnoreCase)))throw new InvalidDataException("Animated source ladder requires its state adapter");
            var sourceVisual=FLVER2.Read(sourceBundle.Files.Single(f=>Leaf(f.Name)==part.ModelName+".flver").Bytes);
            if(sourceVisual.Dummies.Count!=5||!sourceVisual.Dummies.Select(d=>(int)d.ReferenceID).Order().SequenceEqual(Enumerable.Range(191,5)))
                throw new InvalidDataException("Unsupported source ladder endpoint set");
            var output=FLVER2.Read(donorBundle.Files[0].Bytes);var markers=new List<Marker>();
            foreach(var dummy in output.Dummies) {
                var authored=sourceVisual.Dummies.Single(d=>d.ReferenceID==dummy.ReferenceID);
                if(authored.AttachBoneIndex!=-1||authored.FollowAttachBone||!authored.UseUpwardVector||!Finite(authored.Position)||
                    !Finite(authored.Forward)||!Finite(authored.Upward)||authored.Forward.LengthSquared()<1e-8f||authored.Upward.LengthSquared()<1e-8f)
                    throw new InvalidDataException("Unsupported source ladder endpoint binding");
                var world=authored.ParentBoneIndex<0?Matrix4x4.Identity:World(sourceVisual.Nodes,authored.ParentBoneIndex);
                var transform=world*donorInverse;
                dummy.Position=Vector3.Transform(authored.Position,transform);
                dummy.Forward=Vector3.Normalize(Vector3.TransformNormal(authored.Forward,transform))*dummy.Forward.Length();
                dummy.Upward=Vector3.Normalize(Vector3.TransformNormal(authored.Upward,transform))*dummy.Upward.Length();
                dummy.ParentBoneIndex=0;dummy.AttachBoneIndex=-1;
                if(Math.Abs(Vector3.Dot(Vector3.Normalize(dummy.Forward),Vector3.Normalize(dummy.Upward)))>.001f)
                    throw new InvalidDataException("Nonorthogonal source ladder endpoint");
                markers.Add(new(dummy.ReferenceID,dummy.Position,dummy.Forward,dummy.Upward));
            }
            float promptOffset=0;
            if(part.EntityID==1011141||part.ModelName=="o1209") {
                var authored=sourceVisual.Dummies.Single(d=>d.ReferenceID==194);
                if(part.EntityID!=1011141||part.ModelName!="o1209"||part.Name!="o1209"||
                    part.Position!=new Vector3(49.57464f,15.869997f,-116.99638f)||part.Rotation!=new Vector3(0,78.861725f,0)||
                    Hash(sourceBundleBytes)!="af0153a72b881d33848200e93513798f66c27409d72761256efc1b2d40d929e4"||
                    Vector3.Distance(authored.Position,new Vector3(2.3841855e-7f,8.89821f,-.48285735f))>1e-6f||Rungs(output)!=14)
                    throw new InvalidDataException("Taurus ladder upper-prompt source contract changed");
                // Only194 supplies the prompt volume. Native descent aligns to
                //192; retaining it preserves the authored upper traversal pose.
                var dummy=output.Dummies.Single(d=>d.ReferenceID==194);var position=dummy.Position;position.Y-=.1f;dummy.Position=position;
                if(Rungs(output)!=14)throw new InvalidDataException("Taurus ladder upper-prompt adjustment changed native rung count");
                int marker=markers.FindIndex(m=>m.ReferenceID==194);markers[marker]=markers[marker] with{Position=position};promptOffset=-.1f;
            }
            float height=markers.Single(m=>m.ReferenceID==192).Position.Y-markers.Single(m=>m.ReferenceID==191).Position.Y;
            if(!float.IsFinite(height)||height<1||height>100)throw new InvalidDataException("Source ladder height outside supported range");
            var bundle=BND4.Read(donorBytes);bundle.Files[0].Bytes=output.Write();bundle.Files[0].Name=bundle.Files[0].Name.Replace(Donor,model,StringComparison.Ordinal);
            files.Add(path,bundle.Write(DCX.Type.DCX_DFLT_10000_44_9));
            var row=new PARAM.Row(donorRow){ID=modelID,Name="Imported DS1R ladder "+part.Name};row.Cells.Single().Value=Payload(donorRow).ToArray();rows.Rows.Add(row);
            var modelRecord=(MSBB.Model.Object)nativeMap.Models.Objects.Single(m=>m.Name==Donor).DeepCopy();modelRecord.Name=model;
            modelRecord.SibPath=@"N:\SPRJ\data\Model\obj\"+model+@"\sib\"+model+".sib";pendingModels.Add(modelRecord);
            int slot=MapCollisionBindings.ObjectSlot(chunks,part.CollisionName,part.Position+offset);
            var actor=(MSBB.Part.Object)actorTemplate.DeepCopy();actor.Name=name;actor.ModelName=model;actor.InstanceID=0;actor.EntityID=entity;
            actor.Position=part.Position+offset;actor.Rotation=part.Rotation;actor.Scale=Vector3.One;actor.CollisionName=slots[slot].Name;
            actor.SibPath=@"N:\SPRJ\data\Model\map\"+targetMap+@"\layout\o_layout.SIB";
            Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);pendingActors.Add(actor);
            int start=52100900+ordinal*2,stop=start+1;ctor.Instructions.Add(new(2009,0,Pack(start,stop,entity)));
            bindings.Add(new(part.Name,part.ModelName,part.EntityID,model,name,entity,sourceStart,sourceStop,start,stop,actor.CollisionName,actor.Position,actor.Rotation,markers,height,promptOffset));
        }
        var nullNames=rows.Rows.Select((r,i)=>(r.Name,i)).Where(p=>p.Name==null).Select(p=>p.i).ToArray();var encoded=rows.Write();
        foreach(int index in nullNames)Array.Clear(encoded,64+index*24+16,8);paramFile.Bytes=encoded;
        files.Add(Params,game.Write(DCX.Type.DCX_DFLT_10000_44_9));var scriptOutput=script.Write(DCX.Type.DCX_DFLT_10000_44_9);
        var trial=MSBB.Read(destination.Write());Append(trial);_=MSBB.Read(trial.Write());Append(destination);
        return new(files,scriptOutput,bindings,consumed,skipped,Limits,hashes);
        void Append(MSBB msb){msb.Models.Objects.AddRange(pendingModels);msb.Parts.Objects.AddRange(pendingActors);}
    }
    static Matrix4x4 World(IReadOnlyList<FLVER.Node> nodes,int index) {
        if(index<0||index>=nodes.Count)throw new InvalidDataException("Invalid source ladder dummy parent");
        var seen=new HashSet<int>();var world=Matrix4x4.Identity;
        while(index>=0){if(index>=nodes.Count||!seen.Add(index))throw new InvalidDataException("Invalid ladder node hierarchy");var n=nodes[index];
            if(!Finite(n.Scale)||!Finite(n.Rotation)||!Finite(n.Translation)||n.Scale!=Vector3.One)throw new InvalidDataException("Unsupported ladder node transform");
            world*=n.ComputeLocalTransform();index=n.ParentIndex;}
        return world;
    }
}
