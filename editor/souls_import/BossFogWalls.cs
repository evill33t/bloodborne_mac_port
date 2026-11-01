// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.RegularExpressions;

// Physical apertures come from the source boxes. Particle appearance and the
// traversal prompt use an unchanged, installed Bloodborne boss-fog donor.
internal static class BossFogWalls {
    internal const string DonorModel="o240308";
    internal const int EffectID=824102,ActionButtonID=2410041;
    const string DonorMap="map/MapStudio/m24_01_00_01.msb.dcx",Params="param/gameparam/gameparam.parambnd.dcx";
    const string BoxHash="5cab9799d50f5c0c1a43639b2af7cd2c8481b71747e300c62f399cbd3808c54f";
    internal record Wall(string SourceName,Vector3 Position,Vector3 Rotation,Vector3 Scale,int ObjectEntity,int SfxEntity,
        string CollisionName,Vector3 HalfExtents,Vector3 Center,float Radius=.025f,bool ReverseInteraction=false);
    internal record Binding(string SourceName,string ObjectName,string Model,int ObjectEntity,int SfxEntity,
        string SfxEventName,string SfxRegionName,int EffectID,int ActionButtonID,Vector3 HalfExtents,Vector3 Center,float Radius,bool ReverseInteraction=false);
    internal record Result(IReadOnlyDictionary<string,byte[]> Files,IReadOnlyList<Binding> Bindings,IReadOnlyList<string> Limits,
        IReadOnlyDictionary<string,string> NativeHashes);
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] bytes)=>Convert.ToHexStringLower(SHA256.HashData(bytes));
    static bool Finite(Vector3 v)=>float.IsFinite(v.X)&&float.IsFinite(v.Y)&&float.IsFinite(v.Z);
    static readonly string[] Limits=[
        "Source box vertices, child/body translation and convex radius are preserved; static native fog body metadata is retained.",
        "Fog particle appearance and dimensions remain the native Gascoigne effect. Source FXR effects, visual scaling and multiplayer fog behavior are not converted.",
        "Native action-button width is 3.2m; traversal must additionally require the source entrance region. Encounter lifecycle/traversal EMEVD is supplied by the encounter adapter."
    ];

    internal static Result Apply(Func<string,byte[]?> readNative,Func<string,byte[]?> readComposed,MSBB destination,
        IReadOnlyList<Wall> walls,string targetMap="m40_03_00_00") {
        if(!Regex.IsMatch(targetMap,@"^m\d{2}_\d{2}_\d{2}_\d{2}$"))throw new InvalidDataException("Invalid fog destination map");
        if(walls.Count==0)return new(new Dictionary<string,byte[]>(),[],[],new Dictionary<string,string>());
        if(walls.Count>20||walls.Select(w=>w.SourceName).Distinct(StringComparer.Ordinal).Count()!=walls.Count)
            throw new InvalidDataException("Invalid fog wall set");
        var entities=destination.Parts.GetEntries().Select(p=>p.EntityID).Concat(destination.Events.GetEntries().Select(e=>e.EntityID))
            .Concat(destination.Regions.GetEntries().Select(r=>r.EntityID)).Where(id=>id>=0).ToHashSet();
        foreach(var wall in walls) {
            if(string.IsNullOrWhiteSpace(wall.SourceName)||!Finite(wall.Position)||!Finite(wall.Rotation)||!Finite(wall.Scale)||
                wall.Scale!=Vector3.One||!Finite(wall.HalfExtents)||!Finite(wall.Center)||
                wall.HalfExtents.X<=0||wall.HalfExtents.Y<=0||wall.HalfExtents.Z<=0||wall.HalfExtents.Length()>20||
                !float.IsFinite(wall.Radius)||wall.Radius<0||wall.Radius>.2f||wall.ObjectEntity<0||wall.SfxEntity<0||
                !entities.Add(wall.ObjectEntity)||!entities.Add(wall.SfxEntity)||
                !destination.Parts.Collisions.Any(p=>p.Name==wall.CollisionName))
                throw new InvalidDataException("Unsupported fog transform, entity reservation or collision: "+wall.SourceName);
        }
        var hashes=new Dictionary<string,string>(StringComparer.Ordinal);
        byte[] Native(string path) {var bytes=readNative(path)??throw new FileNotFoundException("Missing native fog resource",path);hashes.TryAdd(path,Hash(bytes));return bytes;}
        var donor=MSBB.Read(Native(DonorMap));
        var actor=donor.Parts.Objects.Single(p=>p.ModelName==DonorModel&&p.EntityID==2411810);
        var model=donor.Models.Objects.Single(m=>m.Name==DonorModel);
        var effect=donor.Events.SFX.Single(e=>e.EntityID==2413810&&e.EffectID==EffectID);
        var region=donor.Regions.GetEntries().Single(r=>r.Name==effect.RegionName);
        if(region.Shape is not MSB.Shape.Point)throw new InvalidDataException("Unsupported native fog SFX transform");
        byte[] objectBytes=Native("obj/"+DonorModel+".objbnd.dcx");var donorBundle=BND4.Read(objectBytes);
        var physics=donorBundle.Files.Single(f=>Leaf(f.Name)==DonorModel+".hkx");
        var visual=FLVER2.Read(donorBundle.Files.Single(f=>Leaf(f.Name)==DonorModel+".flver").Bytes);
        if(visual.Meshes.Count!=0||visual.Dummies.All(d=>d.ReferenceID!=191))throw new InvalidDataException("Native fog object/prompt donor changed");
        var gameParams=BND4.Read(readComposed(Params)??Native(Params));
        var objectParam=gameParams.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param");var rows=ObjectRows(objectParam.Bytes);
        if(!PARAM.Read(gameParams.Files.Single(f=>Leaf(f.Name)=="ActionButtonParam.param").Bytes).Rows.Any(r=>r.ID==ActionButtonID))
            throw new InvalidDataException("Native boss-fog action-button row is absent");
        var donorRow=rows.Rows.Single(r=>r.ID==240308);
        string bankPath="sfx/frpg_sfxbnd_"+targetMap[..3]+".ffxbnd.dcx";
        byte[]? existing=readComposed(bankPath)??readNative(bankPath);
        var bank=existing==null?BND4.Read(Native("sfx/frpg_sfxbnd_m24.ffxbnd.dcx")):BND4.Read(existing);
        Merge(bank,BND4.Read(Native("sfx/frpg_sfxbnd_m24_01.ffxbnd.dcx")));
        if(!bank.Files.Any(f=>Leaf(f.Name)=="f000824102.fxr"))throw new InvalidDataException("Fog SFX closure lacks the native effect");
        var files=new Dictionary<string,byte[]>(StringComparer.Ordinal);
        var bindings=new List<Binding>();var pendingModels=new List<MSBB.Model.Object>();var pendingActors=new List<MSBB.Part.Object>();
        var pendingEffects=new List<MSBB.Event.SFX>();var pendingRegions=new List<MSBB.Region>();
        var names=destination.Parts.GetEntries().Select(p=>p.Name).Concat(destination.Events.GetEntries().Select(e=>e.Name))
            .Concat(destination.Regions.GetEntries().Select(r=>r.Name)).ToHashSet(StringComparer.Ordinal);
        var used=rows.Rows.Select(r=>r.ID).ToHashSet();int next=694000;
        int eventID=checked(destination.Events.GetEntries().Select(e=>e.EventID).DefaultIfEmpty(-1).Max()+1);
        int instance=checked(destination.Parts.Objects.Select(p=>p.InstanceID).DefaultIfEmpty(-1).Max()+1);
        foreach(var wall in walls) {
            string name;
            while(true) {
                if(next>694999)throw new InvalidDataException("Owned fog model range exhausted");
                name="o"+next;string path="obj/"+name+".objbnd.dcx";
                if(!used.Contains(next)&&!destination.Models.Objects.Any(m=>m.Name==name)&&readNative(path)==null&&readComposed(path)==null)break;
                ++next;
            }
            int id=next++;used.Add(id);var bundle=BND4.Read(objectBytes);
            bundle.Files.Single(f=>Leaf(f.Name)==DonorModel+".hkx").Bytes=FitBox(physics.Bytes.ToArray(),wall.HalfExtents,wall.Center,wall.Radius);
            if(wall.ReverseInteraction) {
                // Retail Gascoigne admits from local +Z. Source entrance boxes
                // can lie on either side of their wall, independently of its yaw.
                // Turn only the action-button marker; retain its position, the
                // physical shape, other dummies and the particle transform.
                var file=bundle.Files.Single(f=>Leaf(f.Name)==DonorModel+".flver");var flver=FLVER2.Read(file.Bytes);
                var marker=flver.Dummies.Single(d=>d.ReferenceID==191);
                marker.Forward=new(-marker.Forward.X,marker.Forward.Y,-marker.Forward.Z);file.Bytes=flver.Write();
            }
            foreach(var f in bundle.Files)f.Name=f.Name.Replace(DonorModel,name,StringComparison.Ordinal);
            files.Add("obj/"+name+".objbnd.dcx",bundle.Write(DCX.Type.DCX_DFLT_10000_44_9));
            var row=new PARAM.Row(donorRow){ID=id,Name="Imported source fog "+wall.SourceName};
            row.Cells.Single().Value=((byte[])donorRow.Cells.Single().Value).ToArray();rows.Rows.Add(row);
            var outputModel=(MSBB.Model.Object)model.DeepCopy();outputModel.Name=name;
            outputModel.SibPath=@"N:\SPRJ\data\Model\obj\"+name+@"\sib\"+name+".sib";pendingModels.Add(outputModel);
            var output=(MSBB.Part.Object)actor.DeepCopy();output.ModelName=name;output.Name=name+"_0000";
            if(!names.Add(output.Name))throw new InvalidDataException("Owned fog actor name occupied");
            output.EntityID=wall.ObjectEntity;output.InstanceID=instance++;output.Position=wall.Position;output.Rotation=wall.Rotation;output.Scale=wall.Scale;
            output.CollisionName=wall.CollisionName;Array.Fill(output.AnimIDs,(short)-1);Array.Fill(output.DrawGroups,uint.MaxValue);
            Array.Fill(output.BackreadGroups,uint.MaxValue);pendingActors.Add(output);
            string sfxName=wall.SourceName+"_import_fog_sfx",regionName=wall.SourceName+"_import_fog_origin";
            if(!names.Add(sfxName)||!names.Add(regionName))throw new InvalidDataException("Owned fog SFX name occupied");
            var origin=region.DeepCopy();origin.Name=regionName;origin.Position=wall.Position;origin.Rotation=wall.Rotation;origin.EntityID=-1;pendingRegions.Add(origin);
            var sfx=(MSBB.Event.SFX)effect.DeepCopy();sfx.Name=sfxName;sfx.EventID=eventID++;sfx.EntityID=wall.SfxEntity;
            sfx.PartName=wall.CollisionName;sfx.RegionName=regionName;pendingEffects.Add(sfx);
            bindings.Add(new(wall.SourceName,output.Name,name,wall.ObjectEntity,wall.SfxEntity,sfxName,regionName,EffectID,ActionButtonID,wall.HalfExtents,wall.Center,wall.Radius,wall.ReverseInteraction));
        }
        // Preserve native null-name pointers as well as row order and payloads.
        var nullNames=rows.Rows.Select((r,i)=>(r.Name,i)).Where(p=>p.Name==null).Select(p=>p.i).ToArray();var paramBytes=rows.Write();
        foreach(int index in nullNames)Array.Clear(paramBytes,64+index*24+16,8);objectParam.Bytes=paramBytes;
        files.Add(Params,gameParams.Write(DCX.Type.DCX_DFLT_10000_44_9));files.Add(bankPath,bank.Write(DCX.Type.DCX_DFLT_10000_44_9));
        // Validate reference closure before committing any mutation to the caller.
        var trial=MSBB.Read(destination.Write());Append(trial);_=MSBB.Read(trial.Write());Append(destination);
        return new(files,bindings,Limits,hashes);
        void Append(MSBB map) {map.Models.Objects.AddRange(pendingModels);map.Parts.Objects.AddRange(pendingActors);map.Events.SFX.AddRange(pendingEffects);map.Regions.Regions.AddRange(pendingRegions);}
    }

    internal static PARAM ObjectRows(Memory<byte> bytes) {
        var rows=PARAM.Read(bytes);
        if(rows.ParamType!="OBJECT_PARAM_ST"||rows.ParamdefDataVersion!=1||rows.DetectedSize!=32||rows.BigEndian||rows.Format2D!=PARAM.FormatFlags1.LongDataOffset)
            throw new InvalidDataException("Unsupported Bloodborne ObjectParam layout");
        var def=new PARAMDEF{ParamType=rows.ParamType,DataVersion=1,Unicode=true};
        def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=32});rows.ApplyParamdef(def);return rows;
    }

    static int Category(BinderFile f)=>Leaf(f.Name).Split('.').Last().ToLowerInvariant() switch {
        "fxr"=>0,"tpf"=>100000,"flver"=>200000,_=>throw new InvalidDataException("Unsupported native SFX resource category")
    };
    static void Merge(BND4 target,BND4 source) {
        if(target.Files.Select(f=>f.ID).Distinct().Count()!=target.Files.Count)throw new InvalidDataException("Duplicate existing SFX binder ID");
        var leaves=target.Files.ToDictionary(f=>Leaf(f.Name),StringComparer.OrdinalIgnoreCase);var ids=target.Files.Select(f=>f.ID).ToHashSet();
        foreach(var f in source.Files) {
            if(leaves.TryGetValue(Leaf(f.Name),out var prior)) {
                if(!prior.Bytes.Span.SequenceEqual(f.Bytes.Span))throw new InvalidDataException("Conflicting existing fog SFX resource: "+Leaf(f.Name));continue;
            }
            int id=Category(f);while(ids.Contains(id))++id;if(id>=Category(f)+100000)throw new InvalidDataException("SFX resource category exhausted");
            ids.Add(id);f.ID=id;target.Files.Add(f);leaves.Add(Leaf(f.Name),f);
        }
        target.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));
    }

    // Exact BB box donor: DS3/HKX2 XML puts vertices at +48, whereas this BB
    // hknpConvexPolytopeShape has hkRelArrays at +40/+44/+48/+52. Re-serializing
    // with the DS3 class would drop the inline arrays. Patch only XYZ/planes/radius;
    // retain vertex W metadata, topology, fixups, static body and class signatures.
    internal static byte[] FitBox(byte[] bytes,Vector3 half,Vector3 center,float radius) {
        if(Hash(bytes)!=BoxHash||!Finite(half)||!Finite(center)||half.X<=0||half.Y<=0||half.Z<=0||half.Length()>20||
            !float.IsFinite(radius)||radius<0||radius>.2f)throw new InvalidDataException("Unsupported native fog convex-box template");
        var result=bytes.ToArray();const int data=512,shape=544,vertices=data+shape+64,planes=data+shape+192;
        var positions=Enumerable.Range(0,8).Select(i=>new Vector3(BitConverter.ToSingle(bytes,vertices+i*16),BitConverter.ToSingle(bytes,vertices+i*16+4),BitConverter.ToSingle(bytes,vertices+i*16+8))).ToArray();
        var lo=positions.Aggregate(Vector3.Min);var hi=positions.Aggregate(Vector3.Max);var oldCenter=(lo+hi)/2;
        void Float(int at,float f)=>BitConverter.GetBytes(f).CopyTo(result,at);
        for(int i=0;i<8;++i) {
            var p=positions[i];p=new(center.X+(p.X<oldCenter.X?-half.X:half.X),center.Y+(p.Y<oldCenter.Y?-half.Y:half.Y),center.Z+(p.Z<oldCenter.Z?-half.Z:half.Z));
            Float(vertices+i*16,p.X);Float(vertices+i*16+4,p.Y);Float(vertices+i*16+8,p.Z);
        }
        for(int i=0;i<6;++i) {
            var n=new Vector3(BitConverter.ToSingle(bytes,planes+i*16),BitConverter.ToSingle(bytes,planes+i*16+4),BitConverter.ToSingle(bytes,planes+i*16+8));
            Float(planes+i*16+12,-Vector3.Dot(n,center)-Math.Abs(n.X)*half.X-Math.Abs(n.Y)*half.Y-Math.Abs(n.Z)*half.Z);
        }
        Float(data+shape+20,radius);return result;
    }
}
