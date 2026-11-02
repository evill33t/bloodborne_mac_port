// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;

static class SourceCorpseVariantsTests {
    static void Check(bool v,string message){if(!v)throw new Exception(message);}
    static void Reject(Action action,string message){try{action();}catch(InvalidDataException){return;}catch(NotSupportedException){return;}throw new Exception(message);}
    static string Leaf(string s)=>s.Replace('\\','/').Split('/').Last();
    static Matrix4x4[] Worlds(IReadOnlyList<Matrix4x4> local,IReadOnlyList<int> parent) {
        var result=new Matrix4x4[local.Count];for(int i=0;i<result.Length;i++){var m=local[i];int p=parent[i],steps=0;while(p>=0){Check(++steps<=local.Count,"Independent hierarchy is cyclic");m*=local[p];p=parent[p];}result[i]=m;}return result;
    }
    // Read raw TAG metadata, but use Smithbox's independent spline decoder for
    // the actual sampled tracks. Restore authored reference values for absent
    // channels: the generic decoder has no skeleton/reference-pose argument.
    static Dictionary<string,Matrix4x4> Poses(byte[] skeletonBytes,byte[] clipBytes) {
        var sk=new SourceCollision.Tag(skeletonBytes);var cl=new SourceCollision.Tag(clipBytes);var obj=sk.Items.Single(i=>sk.Name(i.Type)=="hkaSkeleton");var names=sk.Array(obj,"bones");var refs=sk.Array(obj,"referencePose");var parents=sk.Array(obj,"parentIndices");
        var pose=new Matrix4x4[names.Count];var parent=new int[names.Count];var boneNames=new string[names.Count];var pos=new Vector3[names.Count];var scale=new Vector3[names.Count];var rot=new Quaternion[names.Count];
        for(int i=0;i<names.Count;i++){var n=sk.Element(names,i);var text=sk.Reference(sk.Nested(n.Type,n.At,"name").At);boneNames[i]=Encoding.UTF8.GetString(Enumerable.Range(text.At,text.Count-1).Select(sk.Byte).ToArray());int at=sk.Element(refs,i).At;pos[i]=sk.Vector(at);rot[i]=new(sk.Vector(at+16),BitConverter.Int32BitsToSingle(sk.Int(at+28)));scale[i]=sk.Vector(at+32);parent[i]=unchecked((short)sk.UShort(sk.Element(parents,i).At));}
        var anim=cl.Items.Single(i=>cl.Name(i.Type)=="hkaSplineCompressedAnimation");var binding=cl.Items.Single(i=>cl.Name(i.Type)=="hkaAnimationBinding");var map=cl.Array(binding,"transformTrackToBoneIndices");var data=cl.Array(anim,"data");var raw=Enumerable.Range(data.At,data.Count).Select(cl.Byte).ToArray();
        Check(cl.Int(cl.Nested(anim.Type,anim.At,"numFrames").At)==2,"Installed selected corpse clip is no longer static");var decoded=Spline.ReadSplineCompressedAnimByteBlock(false,raw,map.Count,1).Single();
        for(int t=0;t<decoded.Length;t++) {int bone=unchecked((short)cl.UShort(cl.Element(map,t).At));var track=decoded[t];Check(!track.HasSplinePosition&&!track.HasSplineRotation&&!track.HasSplineScale,"Selected corpse clip gained animated channels");for(int axis=0;axis<3;axis++){if((raw[t*4+1]&(1<<axis))!=0)pos[bone][axis]=track.StaticPosition[axis];if((raw[t*4+3]&(1<<axis))!=0)scale[bone][axis]=track.StaticScale[axis];}if(raw[t*4+2]!=0)rot[bone]=track.StaticRotation;}
        for(int i=0;i<pose.Length;i++)pose[i]=Matrix4x4.CreateScale(scale[i])*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(rot[i]))*Matrix4x4.CreateTranslation(pos[i]);var world=Worlds(pose,parent);return boneNames.Select((n,i)=>(n,i)).ToDictionary(p=>p.n,p=>world[p.i],StringComparer.Ordinal);
    }
    internal static void Run(string root) {
        var hashes=new Dictionary<string,byte[]>();byte[] Read(string p){var path=Path.Combine(root,p);var b=File.ReadAllBytes(path);hashes.TryAdd(path,SHA256.HashData(b));return b;}
        var placements=new List<SourceCorpsePose.Placement>();var plans=new List<SourceCorpsePose.Plan>();
        foreach(var (map,count) in new[]{("m10_02_00_00",16),("m10_01_00_00",28),("m12_00_00_00",15),("m13_00_00_00",15)}) {
            var scene=MSB1.Read(Read("map/MapStudio/"+map+".msb"));var before=scene.Write();var plan=SourceCorpsePose.Resolve("DS1R",map,scene,Read);plans.Add(plan);placements.AddRange(plan.Placements);
            Check(plan.Placements.Count==count&&plan.Placements.Select(p=>p.Name).Distinct().Count()==count,"Placed corpse inventory changed");
            Check(plan.Placements.All(p=>scene.Parts.Objects.Any(o=>o.Name==p.Name&&o.ModelName==p.Model))&&plan.Placements.All(p=>!scene.Parts.DummyObjects.Any(o=>o.Name==p.Name)),"Dummy corpse or substituted model was emitted");
            Check(before.SequenceEqual(scene.Write()),"Pose planning mutated source placements/groups");
            if(map=="m10_01_00_00") {
                Check(plan.Placements.Count(p=>p.Model=="o0502")==4&&plan.Placements.Count(p=>p.Model=="o0504")==2,"Armored/clothed source variants were replaced by o0500");
                Check(plan.Placements.Where(p=>p.InitAnimation==-1).Select(p=>p.Name).Order().SequenceEqual(new[]{"o0500_0004","o0500_0027","o0500_0034"})&&plan.Placements.Where(p=>p.InitAnimation==-1).All(p=>p.Animation==100&&p.SourceEvent==11010400),"Source constructor's held-corpse pose100 was not resolved");
                var ev=EMEVD.Read(Read("event/"+map+".emevd.dcx"));var initial=ev.Events.Single(e=>e.ID==11010400).Instructions[5];initial.ArgData[4]=10;var bad=ev.Write();Reject(()=>SourceCorpsePose.Resolve("DS1R",map,scene,p=>p.StartsWith("event/")?bad:Read(p)),"Changed source event was guessed as held pose100");
            }
            var unsupported=MSB1.Read(before);unsupported.Parts.Objects.First(o=>o.ModelName=="o0500").InitAnimID=99;Reject(()=>SourceCorpsePose.Resolve("DS1R",map,unsupported,Read),"Unknown corpse pose silently fell back to bind pose");
        }
        int vertices=0,triangles=0;float maximum=0;var reports=new List<object>();
        foreach(var group in placements.GroupBy(p=>(p.Model,p.Animation))) {
            var bnd=BND3.Read(Read("obj/"+group.Key.Model+".objbnd.dcx"));var source=FLVER2.Read(bnd.Files.Single(f=>Leaf(f.Name)==group.Key.Model+".flver").Bytes);var bank=BND3.Read(bnd.Files.Single(f=>Leaf(f.Name)==group.Key.Model+".anibnd").Bytes);
            var posed=SourceCorpsePose.Load(Read,"DS1R",group.Key.Model,group.Key.Animation);var output=FLVER2.Read(posed.Write());var worlds=Poses(bank.Files.Single(f=>Leaf(f.Name).Equals("skeleton.hkx",StringComparison.OrdinalIgnoreCase)).Bytes.ToArray(),bank.Files.Single(f=>Leaf(f.Name)==$"a00_{group.Key.Animation:D4}.hkx").Bytes.ToArray());
            int expectedVertices=group.Key.Model switch{"o0500"=>2144,"o0502"=>2278,"o0504"=>2469,_=>throw new Exception()};Check(output.Meshes.Sum(m=>m.Vertices.Count)==expectedVertices&&output.Meshes.Count==source.Meshes.Count,"Source corpse variant geometry lost");
            Check(output.Materials.Count==source.Materials.Count&&output.Materials.Zip(source.Materials).All(m=>m.First.Name==m.Second.Name&&m.First.MTD==m.Second.MTD&&m.First.Textures.Select(t=>t.Path).SequenceEqual(m.Second.Textures.Select(t=>t.Path))),"Source corpse materials/textures were substituted");
            var bind=Worlds(source.Nodes.Select(n=>n.ComputeLocalTransform()).ToArray(),source.Nodes.Select(n=>(int)n.ParentIndex).ToArray());
            for(int m=0;m<source.Meshes.Count;m++) {
                var original=source.Meshes[m];var actual=output.Meshes[m];Check(original.FaceSets.Count==actual.FaceSets.Count&&original.FaceSets.Zip(actual.FaceSets).All(f=>f.First.Flags==f.Second.Flags&&f.First.CullBackfaces==f.Second.CullBackfaces&&f.First.Indices.SequenceEqual(f.Second.Indices)),"Corpse topology/LOD changed");
                for(int v=0;v<original.Vertices.Count;v++) {var input=original.Vertices[v];var position=Vector3.Zero;var normal=Vector3.Zero;for(int k=0;k<4;k++)if(input.BoneWeights[k]>0){int bone=original.BoneIndices[input.BoneIndices[k]];Check(Matrix4x4.Invert(bind[bone],out var inverse),"Singular independent corpse bind");var delta=inverse*worlds[source.Nodes[bone].Name];position+=Vector3.Transform(input.Position,delta)*input.BoneWeights[k];Check(Matrix4x4.Invert(delta,out var ni),"Singular independent corpse normal");normal+=Vector3.TransformNormal(input.Normal,Matrix4x4.Transpose(ni))*input.BoneWeights[k];}
                    float error=Vector3.Distance(position,actual.Vertices[v].Position);maximum=Math.Max(maximum,error);Check(error<.0001f&&Vector3.Distance(Vector3.Normalize(normal),posed.Meshes[m].Vertices[v].Normal)<.0001f&&Vector3.Distance(actual.Vertices[v].Normal,posed.Meshes[m].Vertices[v].Normal)<.014f, $"Independent source pose/normal differs from serialized corpse {group.Key} mesh{m} vertex{v}: position{error},normal{Vector3.Distance(Vector3.Normalize(normal),actual.Vertices[v].Normal)} expected{position} actual{actual.Vertices[v].Position}");Check(input.UVs.SequenceEqual(actual.Vertices[v].UVs)&&input.Colors.SequenceEqual(actual.Vertices[v].Colors),"Corpse UV/color changed");vertices++;}
                triangles+=actual.FaceSets.Where(f=>f.Flags==0).Sum(f=>f.Triangulate(true).Count/3);
            }
            reports.Add(new{model=group.Key.Model,animation=group.Key.Animation,placements=group.Count(),vertices=expectedVertices,boundsMin=output.Header.BoundingBoxMin,boundsMax=output.Header.BoundingBoxMax});
        }
        Check(reports.Count==20&&placements.Count==74,"Installed corpse coverage incomplete");
        Check(SourceCorpsePose.Resolve("DS1","m10_01_00_00",null,_=>throw new Exception("Unexpected source read")).Placements.Count==0,"Unsupported profile was changed");
        foreach(var (path,sha) in hashes)Check(SHA256.HashData(File.ReadAllBytes(path)).SequenceEqual(sha),"Original source asset changed: "+path);
        string destination=Path.Combine(Environment.CurrentDirectory,"out/map-editor/fidelity-followup/corpses");Directory.CreateDirectory(destination);File.WriteAllText(Path.Combine(destination,"regression.json"),JsonSerializer.Serialize(new{status="PASS",placements=placements.Count,models=3,variants=reports.Count,vertices,triangles,maximumPoseError=maximum,originalAssets=hashes.Count,plans,reports},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"PASS corpses:74 placements/3 actual source models/20 static model-pose variants; {vertices} independently sampled vertices, max error{maximum*1000:F4}mm; original source assets unchanged");
    }
}
