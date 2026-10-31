// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

// Foreign urn visuals on a native Bloodborne destructible object rig. The native
// convex bodies, masses, break impulse and sound remain an explicit approximation.
internal static class MapBreakables {
    internal const string TemplateModel="o240035";
    static readonly HashSet<string> Urns=["o1495","o1496","o1497"];
    static readonly HashSet<string> Burg=["o1000","o1001","o1048","o1122","o1129","o1130","o1132","o1133","o1134","o1135","o1140","o1145","o1146","o1150","o1152","o1154","o1155","o1156","o1160","o1162","o1163","o1172","o1175","o1176","o1230","o1231","o1233","o1240","o1241","o1242","o1243","o1251","o1252","o1270","o1321","o1323"];
    // Families were checked against the source/native mesh projections and
    // authored diffuse identities. Bounds scoring operates WITHIN a family:
    // similar boxes do not make a barrel a suitable chair/table collider.
    internal static (string Family,string[] Donors) Family(string source)=>source switch {
        "o1000" or "o1001" or "o1160" or "o1162" or "o1163" or "o1243" or "o1270"=>("table",["o220506","o240023","o240024","o240025","o240026"]),
        "o1175" or "o1176" or "o1240"=>("bench",["o240020","o240022"]),
        "o1242"=>("chair",["o220507","o322020","o322030"]),
        "o1150" or "o1152" or "o1154" or "o1156"=>("barrel",["o240028","o270568","o270569","o270540"]),
        "o1130" or "o1132" or "o1133" or "o1134"=>("wooden-cargo",["o240037","o270571"]),
        "o1135" or "o1140" or "o1251"=>("pail-basket",["o240085"]),
        "o1048"=>("wheel",["o240029"]),
        "o1129" or "o1241" or "o1321"=>("wooden-planks",["o270571"]),
        "o1230" or "o1231" or "o1233"=>("stone-pillar",["o330010"]),
        "o1122"=>("wagon",[]),"o1172"=>("bookcase",[]),"o1145" or "o1146"=>("fabric-sack",[]),
        _=>("unsupported",[])
    };
    internal static IReadOnlySet<string> CandidateModels(string profile,string map)=>profile=="DS1R"&&map=="m10_01_00_00"?Burg:profile is "DS1" or "DS1R"?Urns:new HashSet<string>();
    internal record Placement(string Name,string Model,Vector3 Position,Vector3 Rotation,Vector3 Scale);
    internal record Fragment(int Source,int Target,float Distance);
    internal record ModelReport(string Source,string Target,int SourceFragments,int NativeFragments,
        Vector3 PhysicsScaleMultiplier,IReadOnlyList<Fragment> Fragments,string Donor=TemplateModel,string PhysicsLimit="Native donor convex bodies, masses, break impulse and sound approximate source physics; authored intact and broken visual rest geometry is preserved.",string Family="urn");
    internal record Omitted(string SourceName,string Model,string Reason);
    internal record Export(IReadOnlyDictionary<string,byte[]> Files,IReadOnlySet<string> ConsumedNames,
        IReadOnlyList<MSBB.Model.Object> Models,IReadOnlyList<MSBB.Part.Object> Objects,IReadOnlyList<ModelReport> Reports,
        IReadOnlyDictionary<string,string> SourceActors,IReadOnlyList<Omitted> Skipped,IReadOnlyList<string> Limits);
    sealed record Rig(string Model,byte[] Bytes,PARAM.Row Row,byte[] Payload,FLVER2 Intact,FLVER2 Broken,Dictionary<int,List<Vector3>> Fragments);

    static string Leaf(string name)=>name.Replace('\\','/').Split('/').Last();
    static Memory<byte> Raw(Memory<byte> bytes)=>DCX.Is(bytes.ToArray())?DCX.Decompress(bytes):bytes;
    static List<BinderFile> SourceBinder(string path) {
        var raw=Raw(File.ReadAllBytes(path));
        return BND3.Is(raw.ToArray())?BND3.Read(raw).Files:BND4.Is(raw.ToArray())?BND4.Read(raw).Files:
            throw new InvalidDataException("Unsupported source object binder");
    }
    static FLVER2 Visual(IReadOnlyList<BinderFile> files,string name)=>FLVER2.Read(files.Single(f=>Leaf(f.Name)==name+".flver").Bytes);
    static FLVER2 Clone(FLVER2 value)=>FLVER2.Read(value.Write());
    static bool Finite(Vector3 p)=>float.IsFinite(p.X)&&float.IsFinite(p.Y)&&float.IsFinite(p.Z);

    // A byte-array schema preserves every existing field and padding bit, without
    // making the optional converter depend on an installed PARAMDEF at runtime.
    internal static PARAM ObjectRows(Memory<byte> bytes,bool native) {
        var rows=PARAM.Read(bytes);int size=native?32:16;const short version=1;
        if(rows.ParamType!="OBJECT_PARAM_ST"||rows.ParamdefDataVersion!=version||rows.DetectedSize!=size||rows.BigEndian||
            native&&rows.Format2D!=PARAM.FormatFlags1.LongDataOffset)
            throw new InvalidDataException("Unsupported "+(native?"Bloodborne":"DS1")+" ObjectParam layout");
        var def=new PARAMDEF {ParamType=rows.ParamType,DataVersion=version,Unicode=native};
        def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=size});
        rows.ApplyParamdef(def);return rows;
    }
    internal static byte[] Payload(PARAM.Row row)=>((byte[])row.Cells.Single().Value).ToArray();

    internal static Export BakeFirelink(string sourceRoot,string profile,string nativeDvdRoot,MSBB destination,
        IEnumerable<Placement> placements,Vector3 offset,FLVER2 surface,
        Func<IFlverMaterial,FLVER2.Material> materialFactory,byte[]? gameParamOverride=null,MSBB.Part.GparamConfigStruct? defaultLightingGparam=null,string? sourceMap=null) {
        if(profile is not("DS1" or "DS1R"))throw new NotSupportedException("Destructible visual import currently supports DS1 Firelink urns");
        bool burg=profile=="DS1R"&&sourceMap=="m10_01_00_00";
        var candidates=placements.Where(p=>(burg?Burg:Urns).Contains(p.Model)).ToArray();var skipped=new List<Omitted>();
        var limits=new[]{"Native collision/rigid bodies and break impulses are fitted same-family donor approximations; source intact/broken mesh geometry and per-fragment visual rest positions are retained.","Event-addressed source props are kept as static scenery. Their scripted explosions, traps, support-breaking logic and separate native prop collider are not emitted; source map collision remains unchanged.","Wagons, bookcases and fabric sacks lack a verified same-family native one-body donor with enough rigid fragments; their static source visuals remain, without a separate imported native object collider."};
        var selected=candidates;
        if(burg) {
            var original=MSB1.Read(Path.Combine(sourceRoot,"map/MapStudio/"+sourceMap+".msb")).Parts.Objects.ToDictionary(p=>p.Name,StringComparer.Ordinal);
            foreach(var p in candidates)if(!original.TryGetValue(p.Name,out var authored)||authored.ModelName!=p.Model||authored.Position!=p.Position||authored.Rotation!=p.Rotation||authored.Scale!=p.Scale)throw new InvalidDataException("Breakable placement differs from source MSB: "+p.Name);
            selected=candidates.Where(p=>{
                var a=original[p.Name];if(a.EntityID<0&&a.InitAnimID==-1)return true;
                skipped.Add(new(p.Name,p.Model,$"Source event-addressed or animated prop (entity {a.EntityID}, animation {a.InitAnimID}); source script/initial state and its separate object collision require a dedicated adapter. Static source visual retained."));return false;
            }).ToArray();
        }
        var files=new Dictionary<string,byte[]>();var names=new HashSet<string>(StringComparer.Ordinal);
        var models=new List<MSBB.Model.Object>();var objects=new List<MSBB.Part.Object>();var reports=new List<ModelReport>();
        var sourceActors=new Dictionary<string,string>(StringComparer.Ordinal);
        if(selected.Length==0)return new(files,names,models,objects,reports,sourceActors,skipped,limits);
        if(selected.Length>1000||selected.Select(p=>p.Name).Distinct().Count()!=selected.Length||!Finite(offset))
            throw new InvalidDataException("Invalid destructible placement set");
        foreach(var p in selected)if(!Finite(p.Position)||!Finite(p.Rotation)||!Finite(p.Scale)||
            Math.Min(p.Scale.X,Math.Min(p.Scale.Y,p.Scale.Z))<=0||Math.Max(p.Scale.X,Math.Max(p.Scale.Y,p.Scale.Z))>10)
            throw new InvalidDataException("Invalid destructible transform");
        var sourceParamFiles=SourceBinder(Path.Combine(sourceRoot,"param/GameParam/GameParam.parambnd.dcx"));
        var sourceRows=ObjectRows(sourceParamFiles.Single(f=>Leaf(f.Name)=="ObjectParam.param").Bytes,false);
        var paramPath=Path.Combine(nativeDvdRoot,"param/gameparam/gameparam.parambnd.dcx");
        var gameParams=BND4.Read(gameParamOverride??File.ReadAllBytes(paramPath));
        var paramFile=gameParams.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param");var nativeRows=ObjectRows(paramFile.Bytes,true);
        var rigs=new Dictionary<string,Rig>(StringComparer.Ordinal);
        Rig NativeRig(string model) {
            if(rigs.TryGetValue(model,out var cached))return cached;
            var row=nativeRows.Rows.Single(r=>r.ID==int.Parse(model[1..]));var payload=Payload(row);
            if(BitConverter.ToInt16(payload)!=1||(payload[9]&4)!=0||(payload[9]&16)==0||(payload[10]&2)!=0)throw new InvalidDataException("Native donor is not a hittable one-hit physics breakable: "+model);
            var bytes=File.ReadAllBytes(Path.Combine(nativeDvdRoot,"obj",model+".objbnd.dcx"));var bundle=BND4.Read(bytes);
            var hkxFiles=bundle.Files.Where(f=>f.Name.EndsWith(".hkx",StringComparison.OrdinalIgnoreCase)).ToArray();if(hkxFiles.Length<2)throw new InvalidDataException("Native breakable lacks intact/broken physics: "+model);
            foreach(var f in hkxFiles){var hkx=Raw(f.Bytes);if(hkx.Length<64||BitConverter.ToUInt32(hkx.Span)!=0x57e0e057||hkx.Span[16]!=8)throw new InvalidDataException("Native object physics packfile is invalid");}
            var intact=Visual(bundle.Files,model);var broken=Visual(bundle.Files,model+"_1");var fragments=Groups(broken,false);
            if(fragments.Count<1||fragments.Keys.Any(k=>k>255)||Groups(intact,false).Count!=1)throw new InvalidDataException("Native breakable has unsupported rigid topology: "+model);
            var rig=new Rig(model,bytes,row,payload,intact,broken,fragments);rigs.Add(model,rig);return rig;
        }
        if(!burg)_=NativeRig(TemplateModel);
        var usedIds=destination.Models.Objects.Select(m=>m.Name).ToHashSet(StringComparer.Ordinal);
        var usedRows=nativeRows.Rows.Select(r=>r.ID).ToHashSet();var partNames=destination.Parts.GetEntries().Select(p=>p.Name).ToHashSet(StringComparer.Ordinal);
        var actorTemplate=destination.Parts.Objects.FirstOrDefault(p=>p.ModelName=="o240028")??
            destination.Parts.Objects.FirstOrDefault(p=>p.ModelName==TemplateModel)??throw new InvalidDataException("Destination lacks a native destructible actor template");
        int next=690100;
        foreach(var group in selected.GroupBy(p=>p.Model,StringComparer.Ordinal)) {
            var sourceRow=sourceRows.Rows.Single(r=>r.ID==int.Parse(group.Key[1..]));var sourcePayload=Payload(sourceRow);
            if(BitConverter.ToInt16(sourcePayload)!=1||(sourcePayload[9]&4)!=0)
                throw new InvalidDataException("Source urn does not use the supported physics break mode");
            while(usedRows.Contains(next)||usedIds.Contains("o"+next.ToString("D6"))||File.Exists(Path.Combine(nativeDvdRoot,"obj","o"+next.ToString("D6")+".objbnd.dcx"))) {
                if(++next>999999)throw new InvalidDataException("No free object model ID");
            }
            int id=next++;string target="o"+id.ToString("D6");usedIds.Add(target);usedRows.Add(id);
            var sourceFiles=SourceBinder(Path.Combine(sourceRoot,"obj",group.Key+".objbnd.dcx"));
            var intact=Visual(sourceFiles,group.Key);var broken=Visual(sourceFiles,group.Key+"_1");
            var sourceGroups=Groups(broken,true);
            var sourceBounds=Bounds((burg?Groups(intact,true):sourceGroups).Values.SelectMany(p=>p));
            Rig donor;
            if(!burg)donor=NativeRig(TemplateModel);
            else {
                // Retail one-body furniture/cargo/barrel rigs avoid adding
                // invisible independent intact bodies. All broken source
                // fragments receive distinct native bodies (up to88 here).
                string[] donors=Family(group.Key).Donors;
                var size=sourceBounds.Max-sourceBounds.Min;
                float Score(Rig r) {var b=Bounds(Groups(r.Intact,false).Values.SelectMany(p=>p));var fit=(b.Max-b.Min)/size;if(!Finite(fit)||Math.Min(fit.X,Math.Min(fit.Y,fit.Z))<.1f||Math.Max(fit.X,Math.Max(fit.Y,fit.Z))>10)return float.PositiveInfinity;var log=new Vector3(MathF.Log(fit.X),MathF.Log(fit.Y),MathF.Log(fit.Z));float mean=(log.X+log.Y+log.Z)/3;var origin=((sourceBounds.Min+sourceBounds.Max)*fit-(b.Min+b.Max))/(b.Max-b.Min);return(log-new Vector3(mean)).LengthSquared()+.1f*log.LengthSquared()+origin.LengthSquared();}
                var fits=donors.Select(NativeRig).Where(r=>r.Fragments.Count>=sourceGroups.Count).Select(r=>(rig:r,score:Score(r))).Where(r=>float.IsFinite(r.score)).OrderBy(r=>r.score).ThenBy(r=>r.rig.Model,StringComparer.Ordinal).ToArray();
                if(fits.Length==0){foreach(var part in group)skipped.Add(new(part.Name,part.Model,$"No verified native same-family ({Family(part.Model).Family}) one-body breakable fits the source bounds/fragment count; static source visual retained and no separate native prop collider emitted."));continue;}
                donor=fits[0].rig;
            }
            var targetGroups=donor.Fragments;var intactRig=donor.Intact;var brokenRig=donor.Broken;
            if(sourceGroups.Count>targetGroups.Count)throw new InvalidDataException("Native donor has fewer fragments than the source");
            var targetBounds=Bounds((burg?Groups(intactRig,false):targetGroups).Values.SelectMany(p=>p));
            var fit=(targetBounds.Max-targetBounds.Min)/(sourceBounds.Max-sourceBounds.Min);
            if(!Finite(fit)||fit.X<.1f||fit.Y<.1f||fit.Z<.1f||fit.X>10||fit.Y>10||fit.Z>10)
                throw new InvalidDataException("Incompatible source/native urn bounds");
            var fragments=Match(sourceGroups,targetGroups);
            var mapping=fragments.ToDictionary(f=>f.Source,f=>f.Target);
            int intactBody=Groups(intactRig,false).Keys.Single();var intactMapping=Groups(intact,true).Keys.ToDictionary(k=>k,_=>intactBody);
            var outIntact=RetargetVisual(intact,intactRig,surface,profile,fit,intactMapping,materialFactory);
            var outBroken=RetargetVisual(broken,brokenRig,surface,profile,fit,mapping,materialFactory);
            var bundle=BND4.Read(donor.Bytes);
            foreach(var file in bundle.Files) {
                var leaf=Leaf(file.Name);
                if(leaf==donor.Model+".flver")file.Bytes=outIntact.Write();
                else if(leaf==donor.Model+"_1.flver")file.Bytes=outBroken.Write();
                // Preserve all HKX bytes and rig/body names. Matching FLVER nodes
                // retain those exact names; the enclosing object ID is independent.
                file.Name=file.Name.Replace(donor.Model,target,StringComparison.Ordinal);
            }
            files.Add("obj/"+target+".objbnd.dcx",bundle.Write(DCX.Type.DCX_DFLT_10000_44_9));
            var row=new PARAM.Row(donor.Row){ID=id,Name="Imported "+profile+" "+group.Key};
            var payload=donor.Payload.ToArray();
            if(burg){sourcePayload.AsSpan(0,4).CopyTo(payload);payload[9]=sourcePayload[9];}
            row.Cells.Single().Value=payload;nativeRows.Rows.Add(row);
            models.Add(new MSBB.Model.Object(target){SibPath=@"N:\SPRJ\data\Model\obj\"+target+@"\sib\"+target+".sib"});
            int instance=0;foreach(var sourcePart in group) {
                var actor=(MSBB.Part.Object)actorTemplate.DeepCopy();actor.ModelName=target;
                do {actor.Name=target+"_"+(instance++).ToString("D4");}while(!partNames.Add(actor.Name));
                actor.Position=sourcePart.Position+offset;actor.Rotation=sourcePart.Rotation;actor.Scale=sourcePart.Scale/fit;
                actor.EntityID=-1;actor.InstanceID=instance-1;actor.CollisionName=null;
                Array.Fill(actor.AnimIDs,(short)-1);Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);
                actor.GparamConfigStruct=(defaultLightingGparam??destination.Parts.MapPieces.First(p=>p.GparamConfig.LightSetID==0).GparamConfig).DeepCopy();
                objects.Add(actor);names.Add(sourcePart.Name);sourceActors.Add(sourcePart.Name,actor.Name);
            }
            reports.Add(new(group.Key,target,sourceGroups.Count,targetGroups.Count,Vector3.One/fit,fragments,donor.Model,Family:burg?Family(group.Key).Family:"urn"));
        }
        // SoulsFormats serializes null row names as empty strings. Keep the
        // original null pointers as well as every row's order and opaque bytes.
        var nullNames=nativeRows.Rows.Select((row,index)=>(row.Name,index)).Where(p=>p.Name==null).Select(p=>p.index).ToArray();
        var paramBytes=nativeRows.Write();foreach(int index in nullNames)Array.Clear(paramBytes,64+index*24+16,8);
        paramFile.Bytes=paramBytes;files.Add("param/gameparam/gameparam.parambnd.dcx",gameParams.Write(DCX.Type.DCX_DFLT_10000_44_9));
        return new(files,names,models,objects,reports,sourceActors,skipped,limits);
    }

    static int Bone(FLVER2.Mesh mesh,FLVER.Vertex v,bool source) {
        if(mesh.UseBoneWeights)throw new InvalidDataException("Animated/skinned breakable meshes are unsupported");
        int local=source?v.BoneIndices[0]:v.NormalW;
        if(mesh.BoneIndices.Count==0)return local;
        if(local<0||local>=mesh.BoneIndices.Count)throw new InvalidDataException("Invalid urn rigid palette index");
        return mesh.BoneIndices[local];
    }
    static Dictionary<int,List<Vector3>> Groups(FLVER2 model,bool source) {
        if(model.Meshes.Sum(m=>(long)m.Vertices.Count)>1000000)throw new InvalidDataException("Urn geometry exceeds budget");
        var worlds=SkinRetarget.World(model.Nodes);var result=new Dictionary<int,List<Vector3>>();
        foreach(var mesh in model.Meshes)foreach(var v in mesh.Vertices) {
            int bone=Bone(mesh,v,source);if(bone<0||bone>=worlds.Length)throw new InvalidDataException("Invalid urn global bone");
            var p=Vector3.Transform(v.Position,worlds[bone]);if(!Finite(p))throw new InvalidDataException("Nonfinite urn vertex");
            if(!result.ContainsKey(bone))result[bone]=[];result[bone].Add(p);
        }
        return result;
    }
    static (Vector3 Min,Vector3 Max) Bounds(IEnumerable<Vector3> points) {
        var values=points.ToArray();if(values.Length==0)throw new InvalidDataException("Empty urn geometry");
        return(values.Aggregate(Vector3.Min),values.Aggregate(Vector3.Max));
    }

    // Rectangular Hungarian assignment gives each source piece its own native
    // rigid body. Matching uses normalized rest-pose centroids, not node names.
    static List<Fragment> Match(Dictionary<int,List<Vector3>> source,Dictionary<int,List<Vector3>> target) {
        var s=source.Keys.Order().ToArray();var t=target.Keys.Order().ToArray();int n=s.Length,m=t.Length;
        var sb=Bounds(source.Values.SelectMany(p=>p));var tb=Bounds(target.Values.SelectMany(p=>p));
        Vector3 Center(List<Vector3> ps){var b=Bounds(ps);return(b.Min+b.Max)/2;}
        Vector3 S(int k)=>(Center(source[k])-(sb.Min+sb.Max)/2)/(sb.Max-sb.Min);
        Vector3 T(int k)=>(Center(target[k])-(tb.Min+tb.Max)/2)/(tb.Max-tb.Min);
        var cost=new double[n+1,m+1];for(int i=1;i<=n;++i)for(int j=1;j<=m;++j)cost[i,j]=Vector3.DistanceSquared(S(s[i-1]),T(t[j-1]));
        var u=new double[n+1];var v=new double[m+1];var p=new int[m+1];var way=new int[m+1];
        for(int i=1;i<=n;++i) {
            p[0]=i;int j0=0;var min=Enumerable.Repeat(double.PositiveInfinity,m+1).ToArray();var used=new bool[m+1];
            do {used[j0]=true;int i0=p[j0],j1=0;double delta=double.PositiveInfinity;
                for(int j=1;j<=m;++j)if(!used[j]) {double c=cost[i0,j]-u[i0]-v[j];if(c<min[j]){min[j]=c;way[j]=j0;}if(min[j]<delta){delta=min[j];j1=j;}}
                if(!double.IsFinite(delta))throw new InvalidDataException("No finite native fragment assignment");
                for(int j=0;j<=m;++j)if(used[j]){u[p[j]]+=delta;v[j]-=delta;}else min[j]-=delta;j0=j1;
            }while(p[j0]!=0);
            do {int j1=way[j0];p[j0]=p[j1];j0=j1;}while(j0!=0);
        }
        var result=new List<Fragment>();for(int j=1;j<=m;++j)if(p[j]!=0)result.Add(new(s[p[j]-1],t[j-1],(float)Math.Sqrt(cost[p[j],j])));
        return result.OrderBy(f=>f.Source).ToList();
    }

    internal static FLVER2 RetargetVisual(FLVER2 source,FLVER2 rig,FLVER2 surface,string profile,Vector3 fit,
        IReadOnlyDictionary<int,int> mapping,Func<IFlverMaterial,FLVER2.Material> materialFactory) {
        if(!Finite(fit)||fit.X<=0||fit.Y<=0||fit.Z<=0)throw new InvalidDataException("Invalid urn visual fit");
        var result=Clone(rig);var shader=Clone(surface);result.BufferLayouts=shader.BufferLayouts;result.GXLists=shader.GXLists;
        result.Meshes.Clear();result.Materials.Clear();var template=shader.Meshes.First(m=>m.MaterialIndex==0);
        var worlds=SkinRetarget.World(rig.Nodes);var geometry=new StaticMapGeometry(source,profile);
        var localBounds=new Dictionary<int,List<Vector3>>();var allWorld=new List<Vector3>();
        foreach(var mesh in source.Meshes) {
            var material=materialFactory(source.Materials[mesh.MaterialIndex]);material.GXIndex=shader.Materials[0].GXIndex;
            var output=new FLVER2.Mesh{UseBoneWeights=false,NodeIndex=0,MaterialIndex=result.Materials.Count};
            foreach(var buffer in template.VertexBuffers)output.VertexBuffers.Add(new FLVER2.VertexBuffer(buffer.LayoutIndex){BufferIndex=buffer.BufferIndex});
            var meshWorld=new List<Vector3>();
            foreach(var input in mesh.Vertices) {
                int original=Bone(mesh,input,true);
                if(!mapping.TryGetValue(original,out var target)||target<0||target>=worlds.Length)
                    throw new InvalidDataException("Incomplete urn fragment mapping");
                if(!Matrix4x4.Invert(worlds[target],out var inverse))throw new InvalidDataException("Singular native urn node");
                var matrix=Matrix4x4.CreateScale(fit)*inverse;
                if(!Matrix4x4.Invert(matrix,out var inverseMatrix))throw new InvalidDataException("Singular urn retarget transform");
                var vertex=geometry.Bake(mesh,input);var world=vertex.Position*fit;
                vertex.Position=Vector3.Transform(vertex.Position,matrix);vertex.Normal=Vector3.Normalize(Vector3.TransformNormal(vertex.Normal,Matrix4x4.Transpose(inverseMatrix)));
                for(int i=0;i<vertex.Tangents.Count;++i) {
                    var tangent=vertex.Tangents[i];var direction=Vector3.TransformNormal(new(tangent.X,tangent.Y,tangent.Z),matrix);
                    direction-=vertex.Normal*Vector3.Dot(direction,vertex.Normal);
                    if(direction.LengthSquared()<1e-12f)throw new InvalidDataException("Invalid urn tangent");
                    vertex.Tangents[i]=new(Vector3.Normalize(direction),tangent.W);
                }
                vertex.NormalW=target;output.Vertices.Add(vertex);allWorld.Add(world);meshWorld.Add(world);
                if(!localBounds.ContainsKey(target))localBounds[target]=[];localBounds[target].Add(vertex.Position);
            }
            foreach(var face in StaticMapGeometry.Faces(source,mesh)) {
                if(face.Indices.Count%3!=0||face.Indices.Any(i=>i<0||i>=mesh.Vertices.Count))throw new InvalidDataException("Invalid urn triangle");
                for(int i=0;i<face.Indices.Count;i+=3) {
                    int a=Bone(mesh,mesh.Vertices[face.Indices[i]],true),b=Bone(mesh,mesh.Vertices[face.Indices[i+1]],true),c=Bone(mesh,mesh.Vertices[face.Indices[i+2]],true);
                    if(a!=b||a!=c)throw new InvalidDataException("Urn triangle spans separate rigid fragments");
                }
                foreach(var lod in new[]{FLVER2.FaceSet.FSFlags.None,FLVER2.FaceSet.FSFlags.LodLevel1,FLVER2.FaceSet.FSFlags.LodLevel2})
                    output.FaceSets.Add(new(lod,false,face.Cull,face.Unknown,new List<int>(face.Indices)));
            }
            if(output.Vertices.Count==0||output.FaceSets.Count==0)continue;
            StaticMapUV.Apply(output,material,profile);
            var bounds=Bounds(meshWorld);output.BoundingBox=new(){Min=bounds.Min,Max=bounds.Max};result.Materials.Add(material);result.Meshes.Add(output);
        }
        var total=Bounds(allWorld);result.Header.BoundingBoxMin=total.Min;result.Header.BoundingBoxMax=total.Max;
        foreach(var (node,points) in localBounds) {var bounds=Bounds(points);result.Nodes[node].BoundingBoxMin=bounds.Min;result.Nodes[node].BoundingBoxMax=bounds.Max;}
        return result;
    }
}
