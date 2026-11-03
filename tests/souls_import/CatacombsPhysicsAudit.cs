// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;

// Read-only forensic audit of an already staged actor. This does not export,
// replace physics, or infer runtime damage from overlap alone.
internal static class CatacombsPhysicsAudit {
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static string Hash(byte[] p)=>Convert.ToHexStringLower(SHA256.HashData(p));
    static float[] V(Vector3 p)=>[p.X,p.Y,p.Z];
    static void Need(bool b,string why){if(!b)throw new InvalidDataException(why);}
    static Matrix4x4 Pose(Spline.TransformTrack t,float frame) {
        var p=t.StaticPosition;var s=t.StaticScale;
        if(t.HasSplinePosition)p=new(t.SplinePosition.GetValueX(frame)??p.X,t.SplinePosition.GetValueY(frame)??p.Y,t.SplinePosition.GetValueZ(frame)??p.Z);
        if(t.HasSplineScale)s=new(t.SplineScale.GetValueX(frame)??s.X,t.SplineScale.GetValueY(frame)??s.Y,t.SplineScale.GetValueZ(frame)??s.Z);
        return Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(t.HasSplineRotation?t.SplineRotation.GetValue(frame):t.StaticRotation))*Matrix4x4.CreateTranslation(p);
    }
    static Matrix4x4[] Worlds(byte[] clip,NativeAnimation.Rig rig,float fraction) {
        var pack=new NativeAnimation.Pack(clip);int a=pack.Object("hkaSplineCompressedAnimation");var data=pack.Array(a+152,1,64*1024*1024);
        var blocks=Spline.ReadSplineCompressedAnimByteBlock(false,clip.AsSpan(pack.Data+data.At,data.Count).ToArray(),rig.Names.Length,pack.Int(a+60));
        float frame=(pack.Int(a+56)-1)*fraction;int block=Math.Min((int)frame/255,blocks.Count-1);var local=Enumerable.Range(0,rig.Names.Length).Select(i=>Pose(blocks[block][i],frame-block*255)).ToArray();
        return NativeAnimation.Worlds(local,rig.Parents);
    }
    static Vector3[] Skin(FLVER2 model,NativeAnimation.Rig rig,Matrix4x4[] world,IReadOnlySet<int>? includeMeshes=null) {
        var bind=SkinRetarget.World(model.Nodes);var skin=new Matrix4x4[model.Nodes.Count];
        for(int i=0;i<skin.Length;i++){int bone=Array.IndexOf(rig.Names,model.Nodes[i].Name);if(bone<0)continue;Need(Matrix4x4.Invert(bind[i],out var inverse),"singular skin bind");skin[i]=inverse*world[bone];}
        var points=new List<Vector3>();
        for(int meshIndex=0;meshIndex<model.Meshes.Count;meshIndex++){if(includeMeshes!=null&&!includeMeshes.Contains(meshIndex))continue;var mesh=model.Meshes[meshIndex];
            foreach(var vertex in mesh.Vertices){var p=Vector3.Zero;for(int i=0;i<4;i++)if(vertex.BoneWeights[i]>0)p+=Vector3.Transform(vertex.Position,skin[SkinRetarget.Bone(mesh,vertex.BoneIndices[i])])*vertex.BoneWeights[i];points.Add(p);}}
        Need(points.Count>0,"no posed body vertices");return points.ToArray();
    }
    sealed record Capsule(int Body,int RagdollBone,int AnimationBone,string RagdollName,string AnimationName,Vector3 A,Vector3 B,float Radius);
    static object Bounds(IEnumerable<Vector3> p){var a=p.ToArray();return new{Min=V(a.Aggregate(Vector3.Min)),Max=V(a.Aggregate(Vector3.Max))};}
    static float Distance(Vector3 p,Vector3 a,Vector3 b){var ab=b-a;float t=ab.LengthSquared()>0?Math.Clamp(Vector3.Dot(p-a,ab)/ab.LengthSquared(),0,1):0;return Vector3.Distance(p,a+t*ab);}
    static object SourceProfile(byte[] bytes) {
        var bank=BND3.Read(bytes);var member=bank.Files.Single(f=>f.ID==300);var physics=member.Bytes.ToArray();var tag=new SourceCollision.Tag(physics);
        var rag=tag.Items.Single(i=>i.Count==1&&tag.Name(i.Type)=="hkaRagdollInstance");var bodies=tag.PointerArray(rag,"rigidBodies");var constraints=tag.PointerArray(rag,"constraints");
        var sourceBones=tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type)=="hkaSkeleton").Select(i=>tag.Array(i,"bones").Count).ToArray();
        var shapes=bodies.Select(b=>{var s=tag.Reference(tag.Nested(b.Type,b.At,"collidable","shape").At);var chain=new List<string>();int depth=0;while(true){string type=tag.Name(s.Type);chain.Add(type);if(type!="hkpConvexTranslateShape")break;Need(++depth<=4,"source shape chain exceeds audit limit");s=tag.Reference(tag.Nested(s.Type,s.At,"childShape","childShape").At);}return chain.ToArray();}).ToArray();
        var jointTypes=constraints.Select(c=>tag.Name(tag.Reference(tag.Nested(c.Type,c.At,"data").At).Type)).ToArray();
        var mappings=tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type)=="hkaSkeletonMapper").Select(m=>{var item=new SourceCollision.Tag.Item(tag.Nested(m.Type,m.At,"mapping").Type,tag.Nested(m.Type,m.At,"mapping").At,1);return new{Simple=tag.Array(item,"simpleMappings").Count,Chain=tag.Array(item,"chainMappings").Count,Unmapped=tag.Array(item,"unmappedBones").Count};}).ToArray();
        return new{PhysicsHash=Hash(physics),SkeletonBones=sourceBones,Bodies=bodies.Count,Constraints=constraints.Count,Shapes=shapes,JointTypes=jointTypes,Mappings=mappings,MotorTypes=tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type).Contains("ConstraintMotor",StringComparison.Ordinal)).Select(i=>new{Type=tag.Name(i.Type),Bytes=tag.ByteSize(i.Type)}),Pwv=bank.Files.Where(f=>Leaf(f.Name).EndsWith(".hkxpwv",StringComparison.OrdinalIgnoreCase)).Select(f=>new{f.Name,Length=f.Bytes.Length,Hash=Hash(f.Bytes.ToArray()),Header=Convert.ToHexString(f.Bytes.ToArray().AsSpan(0,32))})};
    }
    static void Main(string[] args) {
        string candidate=args[0],native=args[1],source=args[2],output=args[3];Directory.CreateDirectory(output);
        var hashes=new Dictionary<string,string>();
        byte[] Read(string p){var b=File.ReadAllBytes(p);hashes[p]=Hash(b);return b;}
        byte[] Export(string p)=>Read(Path.Combine(candidate,"dvdroot_ps4",p));
        var cb=BND4.Read(Export("chr/c8840.chrbnd.dcx"));var ab=BND4.Read(Export("chr/c8840.anibnd.dcx"));
        var nc=BND4.Read(Read(Path.Combine(native,"chr/c2610.chrbnd.dcx")));var na=BND4.Read(Read(Path.Combine(native,"chr/c2610.anibnd.dcx")));
        var sc=BND3.Read(Read(Path.Combine(source,"chr/c2900.chrbnd.dcx")));
        File.WriteAllText(Path.Combine(output,"source-families.json"),JsonSerializer.Serialize(new[]{"c2900","c2910","c2650","c2930"}.ToDictionary(id=>id,id=>SourceProfile(Read(Path.Combine(source,"chr/"+id+".chrbnd.dcx")))),new JsonSerializerOptions{WriteIndented=true}));
        var sourceModel=FLVER2.Read(sc.Files.Single(f=>Leaf(f.Name)=="c2900.flver").Bytes);
        var selectedSourceMeshes=sourceModel.Meshes.Where(m=>{var match=Regex.Match(sourceModel.Materials[m.MaterialIndex].Name,@"#(\d+)#");return !match.Success||int.Parse(match.Groups[1].Value) is 0 or 1;}).ToArray();
        var bodyMeshes=selectedSourceMeshes.Select((m,i)=>(m,i)).Where(x=>!Regex.IsMatch(sourceModel.Materials[x.m.MaterialIndex].Name,@"#\d+#")).Select(x=>x.i).ToHashSet();
        var physics=cb.Files.Single(f=>f.ID==300).Bytes.ToArray();var donor=nc.Files.Single(f=>f.ID==300).Bytes.ToArray();var pack=new HollowDeathRig.Pack(physics);
        var model=FLVER2.Read(cb.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)).Bytes);var rig=NativeAnimation.Skeleton(ab.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),true);
        Need(selectedSourceMeshes.Length==model.Meshes.Count&&selectedSourceMeshes.Select(m=>m.Vertices.Count).SequenceEqual(model.Meshes.Select(m=>m.Vertices.Count)),"source/output mesh order changed");
        var nativeRig=NativeAnimation.Skeleton(na.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),true);var clip=ab.Files.Single(f=>f.ID==1000000000).Bytes.ToArray();var donorClip=na.Files.Single(f=>f.ID==1000000000).Bytes.ToArray();
        var skeletons=pack.Objects.Where(o=>o.Name=="hkaSkeleton").Select(o=>(o.At,Info:pack.Skeleton(o.At))).ToArray();
        var anim=skeletons.Single(s=>s.Info.Names.Length==nativeRig.Names.Length);var rag=skeletons.Single(s=>s.At!=anim.At);
        int system=pack.One("hknpRagdollData");var bodies=pack.Array(system+64,96);var boneToBody=pack.Array(system+128,4);var bodyBones=Enumerable.Range(0,boneToBody.Count).Select(i=>pack.Int(boneToBody.At+i*4)).ToArray();
        var forward=pack.Objects.Single(o=>o.Name=="hkaSkeletonMapper"&&pack.Pointer(o.At+16)==anim.At);var simple=pack.Array(forward.At+80,64);var chains=pack.Array(forward.At+96,112);
        var mappings=Enumerable.Range(0,simple.Count).Select(i=>{int at=simple.At+i*64;return new{A=BitConverter.ToInt16(physics,pack.Data+at),B=BitConverter.ToInt16(physics,pack.Data+at+2),Transform=pack.Qs(at+16)};}).ToArray();
        var capsules=new List<Capsule>();
        for(int i=0;i<bodies.Count;i++){
            int at=bodies.At+i*96,shape=pack.Pointer(at),bone=Array.IndexOf(bodyBones,i);Need(bone>=0,"body lacks named ragdoll bone");
            Need(pack.Objects.Any(o=>o.At==shape&&o.Name=="hknpCapsuleShape"),"noncapsule donor shape");var match=mappings.SingleOrDefault(m=>m.B==bone);Need(match!=null,"body requires unimplemented chain evaluation");
            Vector3 Point(int o)=>new(BitConverter.ToSingle(physics,pack.Data+shape+o),BitConverter.ToSingle(physics,pack.Data+shape+o+4),BitConverter.ToSingle(physics,pack.Data+shape+o+8));
            capsules.Add(new(i,bone,match!.A,rag.Info.Names[bone],anim.Info.Names[match.A],Point(64),Point(80),BitConverter.ToSingle(physics,pack.Data+shape+20)));
        }
        var frames=new List<object>();
        foreach(float f in new[]{0f,.25f,.5f,.75f,1f}){
            var world=Worlds(clip,rig,f);var nw=Worlds(donorClip,nativeRig,f);var points=Skin(model,rig,world,bodyMeshes);var placed=capsules.Select(c=>{var map=mappings.Single(m=>m.B==c.RagdollBone);var transform=map.Transform*world[c.AnimationBone];return c with{A=Vector3.Transform(c.A,transform),B=Vector3.Transform(c.B,transform)};}).ToArray();
            float maxPrefixError=Enumerable.Range(0,nativeRig.Names.Length).Max(i=>new[]{Vector3.Zero,Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Max(p=>Vector3.Distance(Vector3.Transform(p,world[i]),Vector3.Transform(p,nw[i]))));
            var distances=points.Select(p=>placed.Min(c=>Distance(p,c.A,c.B)-c.Radius)).ToArray();
            var comparisons=new[]{"Head","Neck","Spine","Spine1","Spine2","L Hand","R Hand","L Foot","R Foot"}.Where(n=>rig.Names.Contains(n)&&rig.Names.Contains("DS1_"+n)).Select(n=>new{Name=n,Native=V(world[Array.IndexOf(rig.Names,n)].Translation),Source=V(world[Array.IndexOf(rig.Names,"DS1_"+n)].Translation),Distance=Vector3.Distance(world[Array.IndexOf(rig.Names,n)].Translation,world[Array.IndexOf(rig.Names,"DS1_"+n)].Translation)}).ToArray();
            frames.Add(new{Fraction=f,BodyVertices=points.Length,BodyBounds=Bounds(points),CapsuleBounds=Bounds(placed.SelectMany(c=>new[]{c.A-new Vector3(c.Radius),c.A+new Vector3(c.Radius),c.B-new Vector3(c.Radius),c.B+new Vector3(c.Radius)})),MaxControllerPoseError=maxPrefixError,VerticesInsideCapsules=distances.Count(d=>d<=0),VerticesWithin10cm=distances.Count(d=>d<=.1),MaxVertexOutsideDistance=distances.Max(),BoneComparisons=comparisons,Capsules=placed.Select(c=>new{c.Body,c.RagdollBone,c.AnimationBone,c.RagdollName,c.AnimationName,A=V(c.A),B=V(c.B),c.Radius})});
        }
        var pwvFiles=cb.Files.Where(f=>Leaf(f.Name).EndsWith(".hkxpwv",StringComparison.OrdinalIgnoreCase)).Select(f=>{var p=HKXPWV.Read(f.Bytes);return new{f.ID,f.Name,Hash=Hash(f.Bytes.ToArray()),BoneMap=p.BoneMapEntries.Count,AnimationEntries=p.AnimBoneEntries.Count,RagdollEntries=p.RagdollBoneEntries.Count,RagdollRows=p.RagdollBoneEntries.Select(r=>new{r.RagdollParamID,r.DamageAnimID,r.NPCPartGroupIndex,r.Unknown,r.UnknownBB})};}).ToArray();
        var tagBytes=sc.Files.Single(f=>Leaf(f.Name)=="c2900.hkx").Bytes.ToArray();var tag=new SourceCollision.Tag(tagBytes);var sourceRig=tag.Items.Single(i=>i.Count==1&&tag.Name(i.Type)=="hkaRagdollInstance");var sourceBodies=tag.PointerArray(sourceRig,"rigidBodies");
        var sourceCapsules=sourceBodies.Select((b,i)=>{var shape=tag.Reference(tag.Nested(b.Type,b.At,"collidable","shape").At);string type=tag.Name(shape.Type);return new{Body=i,Type=type,Rest=tag.Transform(tag.Nested(b.Type,b.At,"motion","motionState").At),Radius=BitConverter.ToSingle(tagBytes,tag.Nested(shape.Type,shape.At,"radius").At)};}).ToArray();
        foreach(var(path,hash)in hashes)Need(Hash(File.ReadAllBytes(path))==hash,"input changed "+path);
        File.WriteAllText(Path.Combine(output,"physics.json"),JsonSerializer.Serialize(new{Hashes=hashes,PhysicsEqualsDonor=physics.SequenceEqual(donor),PlaybackBones=rig.Names.Length,NativePrefixBones=nativeRig.Names.Length,PlaybackNames=rig.Names,EmbeddedAnimationBones=anim.Info.Names.Length,EmbeddedAnimationNames=anim.Info.Names,EmbeddedRagdollBones=rag.Info.Names.Length,RagdollNames=rag.Info.Names,BodyCount=bodies.Count,SimpleMappings=simple.Count,ChainMappings=chains.Count,HasSourceMappedBones=anim.Info.Names.Any(n=>n.StartsWith("DS1_")),Meshes=model.Meshes.Select((m,i)=>new{Material=model.Materials[m.MaterialIndex].Name,SourceMaterial=sourceModel.Materials[selectedSourceMeshes[i].MaterialIndex].Name,Body=bodyMeshes.Contains(i),m.Vertices.Count}),PWV=pwvFiles,Frames=frames,SourceBodyCount=sourceBodies.Count,SourceBodies=sourceCapsules,RuntimeContactsTested=false},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"PASS read-only physics audit: {rig.Names.Length} playback / {anim.Info.Names.Length} embedded animation bones, {bodies.Count} capsules, native donor equal={physics.SequenceEqual(donor)}, {hashes.Count} inputs unchanged; runtime damage not inferred");
    }
}
