// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

static class HollowDeathRigTests {
    static void Check(bool value,string why){if(!value)throw new Exception(why);}
    static string Leaf(string name)=>name.Replace('\\','/').Split('/').Last();
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{a.M11-b.M11,a.M12-b.M12,a.M13-b.M13,a.M21-b.M21,a.M22-b.M22,a.M23-b.M23,a.M31-b.M31,a.M32-b.M32,a.M33-b.M33,a.M41-b.M41,a.M42-b.M42,a.M43-b.M43}.Max(Math.Abs);
    static Matrix4x4 Inverse(Matrix4x4 a){Check(Matrix4x4.Invert(a,out var result),"Singular test frame");return result;}
    static float F(HollowDeathRig.Pack p,int at)=>BitConverter.ToSingle(p.Bytes,p.Data+at);
    static Vector3 V(HollowDeathRig.Pack p,int at)=>new(F(p,at),F(p,at+4),F(p,at+8));
    static short S(HollowDeathRig.Pack p,int at)=>BitConverter.ToInt16(p.Bytes,p.Data+at);
    static Matrix4x4 Transform(HollowDeathRig.Pack p,int at) {
        // hkTransform's SIMD W lanes are padding, not homogeneous matrix data.
        return new(F(p,at),F(p,at+4),F(p,at+8),0,F(p,at+16),F(p,at+20),F(p,at+24),0,F(p,at+32),F(p,at+36),F(p,at+40),0,F(p,at+48),F(p,at+52),F(p,at+56),1);
    }
    static void Reject(Action action,string why){try{action();}catch(InvalidDataException){return;}throw new Exception("Accepted invalid "+why);}
    static object Validate(byte[] bytes,HollowDeathSource source,NativeRigAppend.Plan plan) {
        var p=new HollowDeathRig.Pack(bytes);int root=p.One("hkRootLevelContainer");var variants=p.Array(root,24,16);
        string[] labels=["Merged Animation Container","Physics Scene Data","Physics Ragdoll","SkeletonMapper","SkeletonMapper"];
        string[] classes=["hkaAnimationContainer","hknpPhysicsSceneData","hknpRagdollData","hkaSkeletonMapper","hkaSkeletonMapper"];
        Check(variants.Count==5,"Native root variant count changed");
        for(int i=0;i<5;i++)Check(p.Text(variants.At+i*24)==labels[i]&&p.Text(variants.At+i*24+8)==classes[i],"Native root contract changed");
        int system=p.One("hknpRagdollData"),scene=p.One("hknpPhysicsSceneData");var systems=p.Array(scene+16,8,4);
        Check(systems.Count==1&&p.Pointer(systems.At)==system&&p.Pointer(variants.At+2*24+16)==system,"Scene/direct ragdoll must share one object");
        var bodies=p.Array(system+64,96,32);var constraints=p.Array(system+80,24,32);var motion=p.Array(system+48,112,32);var map=p.Array(system+128,4,32);
        Check(bodies.Count==source.Bodies.Count&&constraints.Count==source.Constraints.Count&&motion.Count==bodies.Count&&map.Count==source.BoneToBody.Length,"Source physical topology changed");
        Check(Enumerable.Range(0,map.Count).Select(i=>p.Int(map.At+i*4)).SequenceEqual(source.BoneToBody),"Physical bone/body ownership changed");
        var worlds=new Matrix4x4[bodies.Count];int capsules=0,boxes=0;
        for(int i=0;i<bodies.Count;i++) {
            int at=bodies.At+i*96;var s=source.Bodies[i];var pos=V(p,at+48);var q=new Quaternion(F(p,at+64),F(p,at+68),F(p,at+72),F(p,at+76));worlds[i]=Matrix4x4.CreateFromQuaternion(q)*Matrix4x4.CreateTranslation(pos);
            Check(p.Text(at+32)==s.Name&&Error(worlds[i],s.Transform)<.00001f,"Authored body rest frame changed");
            Check(p.Int(at+12)==i&&S(p,at+18)==i,"Body motion/material reference changed");
            Check(Math.Abs(F(p,motion.At+i*112+4)-s.InertiaMass.W)<.000001f&&Vector3.Distance(V(p,motion.At+i*112+32),new(s.InertiaMass.X,s.InertiaMass.Y,s.InertiaMass.Z))<.000001f,"Authored mass/inertia changed");
            Check(Vector3.Distance(V(p,motion.At+i*112+48),new(s.Center.X,s.Center.Y,s.Center.Z))<.000001f,"Authored center of mass changed");
            int shape=p.Pointer(at);string cls=p.Objects.Single(o=>o.At==shape).Name;var expected=s.Shape;
            Check(Math.Abs(F(p,shape+20)-expected.Radius)<.000001f,"Source convex radius changed");
            int vertex=shape+40+BitConverter.ToUInt16(p.Bytes,p.Data+shape+42),plane=shape+44+BitConverter.ToUInt16(p.Bytes,p.Data+shape+46);
            var verts=Enumerable.Range(0,8).Select(n=>V(p,vertex+n*16)).ToArray();
            if(expected.Type=="hkpCapsuleShape") {capsules++;Check(cls=="hknpCapsuleShape"&&Vector3.Distance(V(p,shape+64),expected.A+expected.Translation)<.000001f&&Vector3.Distance(V(p,shape+80),expected.B+expected.Translation)<.000001f,"Source capsule dimensions changed");}
            else {boxes++;Check(cls=="hknpConvexPolytopeShape"&&Vector3.Distance(verts.Aggregate(Vector3.Min),expected.Translation-expected.Half)<.00001f&&Vector3.Distance(verts.Aggregate(Vector3.Max),expected.Translation+expected.Half)<.00001f,"Source translated box dimensions changed");}
            for(int f=0;f<6;f++){var normal=V(p,plane+f*16);float d=F(p,plane+f*16+12);Check(Math.Abs(normal.Length()-1)<.0001f&&verts.All(v=>Vector3.Dot(normal,v)+d<.0001f),"Invalid convex hull support plane");}
        }
        float worstPivot=0,worstMapper=0;int motors=p.Objects.Count(o=>o.Name=="hkpPositionConstraintMotor");Check(motors==1,"Constraint motor must remain shared");
        for(int i=0;i<constraints.Count;i++) {
            int at=constraints.At+i*24,data=p.Pointer(at);var s=source.Constraints[i];Check(p.Int(at+8)==s.A&&p.Int(at+12)==s.B&&p.Int(at+16)==0,"Joint topology/native flags changed");
            var raw=p.Raw(data,s.Bytes.Length);Check(raw.AsSpan(0,192).SequenceEqual(s.Bytes.AsSpan(0,192)),"Joint frames/stabilization changed");
            int[] offsets;
            if(s.Type=="hkpLimitedHingeConstraintData") {
                // These offsets come from the shipped native c2630 atoms,
                // independently of the source TAG0 metadata/adapter layout.
                int[] locations=[32,176,192,232,248,264,280];int[] types=[2,23,18,17,14,12,5];
                for(int k=0;k<locations.Length;k++)Check(S(p,data+locations[k])==types[k],"Native hinge atom stream invalid");
                Check(raw.AsSpan(232,16).SequenceEqual(s.Bytes.AsSpan(224,16))&&raw.AsSpan(252,12).SequenceEqual(s.Bytes.AsSpan(248,12))&&raw.AsSpan(264,32).SequenceEqual(s.Bytes.AsSpan(272,32))&&F(p,data+204)==BitConverter.ToSingle(s.Bytes,208),"Typed source hinge limits/motor/friction changed");offsets=s.MotorOffsets.Select(_=>208).ToArray();
            } else {
                foreach(int limitAt in new[]{304,336,368})Check(raw.AsSpan(limitAt,20).SequenceEqual(s.Bytes.AsSpan(limitAt,20)),"Source ragdoll angular limit changed");offsets=s.MotorOffsets;
            }
            Check(offsets.All(o=>p.Pointer(data+o)==p.One("hkpPositionConstraintMotor")),"Joint motor reference lost");
            var a=Transform(p,data+48)*worlds[s.A];var b=Transform(p,data+112)*worlds[s.B];float error=Vector3.Distance(a.Translation,b.Translation);worstPivot=Math.Max(worstPivot,error);Check(error<.00001f,"Source joint pivots no longer meet");
        }
        int simpleCount=0;
        foreach(var o in p.Objects.Where(o=>o.Name=="hkaSkeletonMapper")) {
            var a=p.Skeleton(p.Pointer(o.At+16));var b=p.Skeleton(p.Pointer(o.At+24));var aw=NativeAnimation.Worlds(a.Local,a.Parents.Select(i=>(int)i).ToArray());var bw=NativeAnimation.Worlds(b.Local,b.Parents.Select(i=>(int)i).ToArray());
            Check(a.Names.Length==plan.Count||b.Names.Length==plan.Count,"Mapper does not include extended visible skeleton");var simple=p.Array(o.At+80,64,256);var chains=p.Array(o.At+96,112,256);var unmapped=p.Array(o.At+112,2,256);
            Check(p.Bytes[p.Data+o.At+176]==1&&p.Int(o.At+180)==0,"Native mapper mode changed");
            for(int i=0;i<simple.Count;i++){int at=simple.At+i*64,aa=S(p,at),bb=S(p,at+2);Check(aa>=0&&aa<a.Names.Length&&bb>=0&&bb<b.Names.Length,"Invalid simple bone index");float error=Error(p.Qs(at+16)*aw[aa],bw[bb]);worstMapper=Math.Max(worstMapper,error);Check(error<.00001f,"Bidirectional ragdoll rest mapping changed");simpleCount++;}
            for(int i=0;i<chains.Count;i++){int at=chains.At+i*112;Check(S(p,at)>=0&&S(p,at)<a.Names.Length&&S(p,at+2)>=0&&S(p,at+2)<a.Names.Length&&S(p,at+4)>=0&&S(p,at+4)<b.Names.Length&&S(p,at+6)>=0&&S(p,at+6)<b.Names.Length,"Invalid chain bone index");}
            Check(Enumerable.Range(0,unmapped.Count).Select(i=>S(p,unmapped.At+i*2)).All(i=>i>=0&&i<b.Names.Length),"Invalid unmapped bone index");
        }
        return new{bodies=bodies.Count,constraints=constraints.Count,capsules,boxes,simpleCount,worstPivot,worstMapper};
    }
    static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1],output=args[2];Directory.CreateDirectory(output);var hashes=new Dictionary<string,string>();
        byte[] Read(string root,string rel){string path=Path.Combine(root,rel);var b=File.ReadAllBytes(path);hashes.TryAdd(path,Convert.ToHexStringLower(SHA256.HashData(b)));return b;}
        byte[]? Source(string rel)=>File.Exists(Path.Combine(sourceRoot,rel))?Read(sourceRoot,rel):null;
        var native=BND4.Read(Read(nativeRoot,"chr/c2630.chrbnd.dcx"));var anim=BND4.Read(Read(nativeRoot,"chr/c2630.anibnd.dcx"));var physics=native.Files.Single(f=>f.ID==300).Bytes.ToArray();var reports=new List<object>();
        foreach(string model in new[]{"c2500","c2540","c2550","c2560"}) {
            var authored=MapCharacters.Authored(Source,model);var reference=SourceAnimation.Load(Source,model,0);var plan=HollowSourceSkin.Rig(authored.Model,native.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray(),anim.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),reference);
            var raw=HollowDeathRig.ReadPhysics(Source,model);var source=new HollowDeathSource(raw,model);var result=HollowDeathRig.Convert(physics,raw,plan,model);reports.Add(new{model,checks=Validate(result.Physics,source,plan),result.Report});
            Check(raw.SequenceEqual(HollowDeathRig.ReadPhysics(Source,model)),"Source was mutated");File.WriteAllBytes(Path.Combine(output,model+"-native.hkx"),result.Physics);
            var broken=physics.ToArray();broken[16]=4;Reject(()=>HollowDeathRig.Convert(broken,raw,plan,model),"native pointer size");
            var tag=new SourceCollision.Tag(raw);var capsule=tag.Items.First(i=>i.Count==1&&tag.Name(i.Type)=="hkpCapsuleShape");int radius=tag.Nested(capsule.Type,capsule.At,"radius").At;var corrupt=raw.ToArray();BitConverter.GetBytes(float.NaN).CopyTo(corrupt,radius);Reject(()=>HollowDeathRig.Convert(physics,corrupt,plan,model),"source capsule radius");
            var hinge=source.Constraints.FirstOrDefault(c=>c.Type=="hkpLimitedHingeConstraintData");
            if(hinge!=null){var bad=hinge.Bytes.ToArray();BitConverter.GetBytes(.5f).CopyTo(bad,260);Reject(()=>HollowDeathRig.ConstraintData(hinge with{Bytes=bad}),"unsupported source hinge damping");}
            Console.WriteLine("PASS "+model+" source physics and both serialized mappers");
        }
        Reject(()=>HollowDeathRig.ReadPhysics(Source,"c2300"),"unverified character family");
        foreach(var(path,hash)in hashes)Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)))==hash,"Input changed: "+path);
        File.WriteAllText(Path.Combine(output,"regression.json"),JsonSerializer.Serialize(new{status="PASS",reports,hashes},new JsonSerializerOptions{WriteIndented=true}));Console.WriteLine("PASS four source ragdolls, joint/contact frames, convex hulls, native contracts and "+hashes.Count+" unchanged inputs; native gameplay pending");
    }
}
