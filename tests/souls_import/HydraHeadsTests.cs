// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;

static class HydraHeadsTests {
    static void Check(bool value,string why){if(!value)throw new Exception(why);}
    static void Reject(Action action,string why){try{action();}catch(InvalidDataException){return;}throw new Exception("Accepted "+why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] value)=>Convert.ToHexStringLower(SHA256.HashData(value));
    static float F(HollowDeathRig.Pack p,int at)=>BitConverter.ToSingle(p.Bytes,p.Data+at);
    static short S(HollowDeathRig.Pack p,int at)=>BitConverter.ToInt16(p.Bytes,p.Data+at);
    static Vector3 V(HollowDeathRig.Pack p,int at)=>new(F(p,at),F(p,at+4),F(p,at+8));
    static Quaternion Q(HollowDeathRig.Pack p,int at)=>new(F(p,at),F(p,at+4),F(p,at+8),F(p,at+12));
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{Vector3.Zero,Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Max(v=>Vector3.Distance(Vector3.Transform(v,a),Vector3.Transform(v,b)));
    static Matrix4x4 Frame(HollowDeathRig.Pack p,int at)=>new(F(p,at),F(p,at+4),F(p,at+8),0,F(p,at+16),F(p,at+20),F(p,at+24),0,F(p,at+32),F(p,at+36),F(p,at+40),0,F(p,at+48),F(p,at+52),F(p,at+56),1);
    static Matrix4x4 Sample(Spline.TransformTrack t,float frame) {
        Vector3 Vector(bool scale) {
            var types=scale?t.Mask.ScaleTypes:t.Mask.PositionTypes;var spline=scale?t.SplineScale:t.SplinePosition;var result=scale?Vector3.One:Vector3.Zero;var stat=scale?t.StaticScale:t.StaticPosition;
            for(int i=0;i<3;i++){if(spline!=null)result[i]=(i==0?spline.GetValueX(frame):i==1?spline.GetValueY(frame):spline.GetValueZ(frame))??result[i];else if(types.Contains((Spline.FlagOffset)(1<<i)))result[i]=stat[i];}return result;
        }
        var q=t.HasSplineRotation?t.SplineRotation.GetValue(frame):t.HasStaticRotation?t.StaticRotation:Quaternion.Identity;
        return Matrix4x4.CreateScale(Vector(true))*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(q))*Matrix4x4.CreateTranslation(Vector(false));
    }
    static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1],output=args[2];Directory.CreateDirectory(output);var hashes=new Dictionary<string,string>();
        byte[] Read(string root,string rel){string path=Path.Combine(root,rel);var bytes=File.ReadAllBytes(path);hashes.TryAdd(path,Hash(bytes));return bytes;}
        byte[]? Source(string rel)=>File.Exists(Path.Combine(sourceRoot,rel))?Read(sourceRoot,rel):null;
        byte[] Native(string rel)=>Read(nativeRoot,rel);
        var result=HydraHeads.Package(Source,Native);var source=HydraHeadsSource.Load(Source);
        Check(result.Files.Keys.Order().SequenceEqual(new[]{"action/script/c8831.hks","chr/c8831.anibnd.dcx","chr/c8831.chrbnd.dcx"}),"Owned file membership differs");
        var bank=BND4.Read(result.Files["chr/c8831.chrbnd.dcx"]);var anim=BND4.Read(result.Files["chr/c8831.anibnd.dcx"]);
        var nativeBank=BND4.Read(Native("chr/c2730.chrbnd.dcx"));var nativeAnim=BND4.Read(Native("chr/c2730.anibnd.dcx"));
        Check(result.Files["action/script/c8831.hks"].SequenceEqual(Native("action/script/c2730.hks")),"Passive graph script changed");
        var model=FLVER2.Read(bank.Files.Single(f=>f.ID==200).Bytes);var rig=NativeAnimation.Skeleton(anim.Files.Single(f=>f.ID==1000000).Bytes.ToArray());
        Check(rig.Names.Length==124&&rig.Names.Skip(110).SequenceEqual(source.Animation.Names.Select(n=>"DS1_"+n)),"Complete source animation closure lost");
        var world=SkinRetarget.World(model.Nodes);int weighted=0;var animatedNames=rig.Names.ToHashSet();
        foreach(var mesh in model.Meshes)foreach(var v in mesh.Vertices)for(int i=0;i<4;i++)if(v.BoneWeights[i]>0) {
            int bone=SkinRetarget.Bone(mesh,v.BoneIndices[i]);string name=model.Nodes[bone].Name;
            Check(name.StartsWith("DS1_",StringComparison.Ordinal)&&animatedNames.Contains(name),"Weighted source bone absent from animation rig");
            Check(Error(world[bone],rig.World[Array.IndexOf(rig.Names,name)])<.001f,"Weighted skin/animation bind mismatch");weighted++;
        }
        Check(result.Skin.Vertices==3161&&result.Skin.DirectWeightShare>.999999&&weighted>3000,"Source head mesh or source weights lost");
        var textures=TPF.Read(bank.Files.Single(f=>f.ID==100).Bytes);Check(textures.Textures.Count>=6&&result.Report.InputSHA256.Keys.Contains("DS1R/chr/c3530.chrbnd.dcx"),"Shared c3530 texture owner unresolved");
        Check(bank.Files.All(f=>!Leaf(f.Name).EndsWith("_c.hkx")&&!Leaf(f.Name).EndsWith(".clm2")),"Donor cloth retained");

        var physicsBytes=bank.Files.Single(f=>f.ID==300).Bytes.ToArray();var p=new HollowDeathRig.Pack(physicsBytes);int system=p.One("hknpRagdollData");
        var nativePhysics=nativeBank.Files.Single(f=>f.ID==300).Bytes.ToArray();var nativeClasses=new HollowDeathRig.Pack(nativePhysics).Objects.Select(o=>o.Name).ToHashSet(StringComparer.Ordinal);
        Check(p.Objects.Count(o=>o.Name=="hkaSkeletonMapper")==2&&p.Objects.All(o=>o.Name!="Mapper"),"Both native mapper classes must be serialized");
        foreach(string name in p.Objects.Select(o=>o.Name).Distinct()) {
            var type=typeof(HKX2.hkaSkeletonMapper).Assembly.GetType("HKX2."+name);
            Check(type!=null&&typeof(HKX2.IHavokObject).IsAssignableFrom(type),"Unregistered virtual native class: "+name);
            Check(HydraHeadsPhysics.ClassSignature(physicsBytes,name)==((HKX2.IHavokObject)Activator.CreateInstance(type!)!).Signature,"Registered native signature changed: "+name);
            if(nativeClasses.Contains(name))Check(HydraHeadsPhysics.ClassSignature(physicsBytes,name)==HydraHeadsPhysics.ClassSignature(nativePhysics,name),"Shipped donor class signature changed: "+name);
        }
        var boxPrecedent=BND4.Read(Native("obj/o241013.objbnd.dcx")).Files.Single(f=>f.ID==300).Bytes.ToArray();
        Check(HydraHeadsPhysics.ClassSignature(physicsBytes,"hknpConvexPolytopeShape")==HydraHeadsPhysics.ClassSignature(boxPrecedent,"hknpConvexPolytopeShape"),"Shipped native box class signature changed");
        var bodies=p.Array(system+64,96,32);var joints=p.Array(system+80,24,32);var motions=p.Array(system+48,112,32);var props=p.Array(system+32,64,32);
        Check(bodies.Count==13&&joints.Count==12&&motions.Count==13&&props.Count==13&&p.Objects.Count(o=>o.Name=="hknpCapsuleShape")==12&&p.Objects.Count(o=>o.Name=="hknpConvexPolytopeShape")==1,"Donor physics retained or source shapes lost");
        var worlds=new Matrix4x4[13];float maxShape=0,maxPivot=0;
        for(int i=0;i<13;i++) {
            var b=source.Bodies[i];int at=bodies.At+i*96,ma=motions.At+i*112,pr=props.At+i*64;int shape=p.Pointer(at);
            worlds[i]=Matrix4x4.CreateFromQuaternion(Q(p,at+64))*Matrix4x4.CreateTranslation(V(p,at+48));
            Check(p.Text(at+32)==b.Name&&Error(worlds[i],b.Rest)<.0001f&&p.Int(at+12)==i&&S(p,at+18)==i&&unchecked((uint)p.Int(at+20))==b.CollisionFilter,"Physical source body ownership/rest/filter changed");
            Check(F(p,ma+4)==b.InertiaMassInverse.W&&Vector3.Distance(V(p,ma+32),new(b.InertiaMassInverse.X,b.InertiaMassInverse.Y,b.InertiaMassInverse.Z))<.000001f,"Source mass/inertia changed");
            Check(F(p,pr+8)==b.Gravity&&F(p,pr+12)==b.TimeFactor&&F(p,pr+24)==b.LinearDamping&&F(p,pr+28)==b.AngularDamping,"Source physics coefficients changed");
            Check(F(p,shape+20)==b.Shape.Radius,"Source shape radius lost");
            if(b.Shape.Type=="hkpCapsuleShape") {
                float error=Math.Max(Vector3.Distance(V(p,shape+64),b.Shape.A+b.Shape.Translation),Vector3.Distance(V(p,shape+80),b.Shape.B+b.Shape.Translation));maxShape=Math.Max(maxShape,error);Check(error<.000001f,"Source capsule endpoints changed");
            } else {
                int va=shape+40+BitConverter.ToUInt16(p.Bytes,p.Data+shape+42);var points=Enumerable.Range(0,8).Select(n=>V(p,va+n*16)).ToArray();var lo=points.Aggregate(Vector3.Min);var hi=points.Aggregate(Vector3.Max);
                float error=Math.Max(Vector3.Distance((lo+hi)/2,b.Shape.Translation),Vector3.Distance((hi-lo)/2,b.Shape.Half));maxShape=Math.Max(maxShape,error);Check(error<.00001f,"Source translated head box changed");
            }
        }
        for(int i=0;i<12;i++) {
            var c=source.Constraints[i];int at=joints.At+i*24,data=p.Pointer(at);Check(p.Objects.Single(o=>o.At==data).Name==c.Type&&p.Int(at+8)==c.A&&p.Int(at+12)==c.B,"Source joint topology changed");
            Check(Error(Frame(p,data+48),c.FrameA)<.000001f&&Error(Frame(p,data+112),c.FrameB)<.000001f,"Source joint frames changed");
            for(int n=0;n<3;n++){int la=data+304+n*32;Check(S(p,la)==(n==0?15:16)&&F(p,la+8)==c.Limits[n].Min&&F(p,la+12)==c.Limits[n].Max&&F(p,la+16)==c.Limits[n].Tau,"Source joint angular limits changed");}
            float pivot=Vector3.Distance((Frame(p,data+48)*worlds[c.A]).Translation,(Frame(p,data+112)*worlds[c.B]).Translation);maxPivot=Math.Max(maxPivot,pivot);Check(pivot<.001f,"Source constraint pivots diverged");
        }
        foreach(var mapper in p.Objects.Where(o=>o.Name=="hkaSkeletonMapper")) {
            var bones=p.Array(p.Pointer(mapper.At+16)+40,16,128);bool forward=bones.Count==124;var sm=source.Mappings.Single(m=>m.A.Names.Length==(forward?14:13));
            var simple=p.Array(mapper.At+80,64,32);var chains=p.Array(mapper.At+96,112,2);var unmapped=p.Array(mapper.At+112,2,128);
            Check(simple.Count==12&&chains.Count==1&&unmapped.Count==(forward?0:111)&&p.Bytes[p.Data+mapper.At+176]==1,"Source chained mapper coverage lost");
            for(int n=0;n<12;n++){int at=simple.At+n*64;var m=sm.Simple[n];Check(S(p,at)==m.A+(forward?110:0)&&S(p,at+2)==m.B+(forward?0:110)&&p.Raw(at+16,48).SequenceEqual(m.Transform),"Mapper source simple transform changed");}
            var chain=sm.Chains.Single();Check(S(p,chains.At)==chain.StartA+(forward?110:0)&&S(p,chains.At+2)==chain.EndA+(forward?110:0)&&S(p,chains.At+4)==chain.StartB+(forward?0:110)&&S(p,chains.At+6)==chain.EndB+(forward?0:110)&&p.Raw(chains.At+16,96).SequenceEqual(chain.Transforms),"Source chain transforms/indices changed");
            Check(p.Raw(mapper.At+128,48).SequenceEqual(sm.ExtractedMotion),"Source extracted identity SIMD lanes changed");
            if(!forward)Check(Enumerable.Range(0,110).All(n=>S(p,unmapped.At+n*2)==n),"Controller-prefix bones were physically driven");
        }
        var pwvRaw=bank.Files.Single(f=>Leaf(f.Name)=="c8831.hkxpwv").Bytes.ToArray();var pwv=HKXPWV.Read(pwvRaw);
        var originalPwv=nativeBank.Files.Single(f=>Leaf(f.Name)=="c2730.hkxpwv").Bytes.ToArray();
        Check(pwv.BoneMapEntries.Count==13&&pwv.AnimBoneEntries.Count==124&&pwv.RagdollBoneEntries.Count==13,"Typed native PWV population differs");
        Check(pwv.BoneMapEntries.All(e=>e.UnknownValueA==-1&&e.UnknownValueB==0)&&pwv.RagdollBoneEntries.All(e=>e.RagdollParamID==-1&&e.DamageAnimID==-1&&e.NPCPartGroupIndex==0&&e.UnknownBB==-1),"Head acquired donor part/damage parameters");
        Check(pwvRaw.AsSpan(32+13*8,110*8).SequenceEqual(originalPwv.AsSpan(32+60*8,110*8)),"Native SkeletonParam prefix changed");

        var tae=TAE.Read(anim.Files.Single(f=>f.ID==3000000).Bytes);Check(tae.Animations.Where(a=>a.ID%1000000!=7005).All(a=>a.Events.Count==0),"Donor attack/FX events remain");
        var motion=SourceAnimation.Load(Source,"c3531",8100);float maxPose=0,maxController=0;int poseCount=0;
        foreach(var fall in result.Report.FallingMotions) {
            var raw=anim.Files.Single(f=>f.ID==1000000000+fall.NativeAnimation).Bytes.ToArray();var pack=new NativeAnimation.Pack(raw);int at=pack.Object("hkaSplineCompressedAnimation");
            Check(pack.Int(at+20)==124&&pack.Int(at+56)==151&&Math.Abs(pack.Float(at+16)-5)<.0001f,"Source falling clip rescaled to donor timing");
            var data=pack.Array(at+152,1,32*1024*1024);var tracks=Spline.ReadSplineCompressedAnimByteBlock(false,raw.AsSpan(pack.Data+data.At,data.Count).ToArray(),124,pack.Int(at+60));Check(tracks.Count==1,"Unexpected falling animation block count");
            foreach(int frame in new[]{0,15,35,45,70,100,150}) {
                var local=tracks[0].Select(t=>Sample(t,frame)).ToArray();var emitted=NativeAnimation.Worlds(local,rig.Parents);var expected=NativeAnimation.Worlds(motion.Sample(frame*motion.FrameDuration),motion.Bones.Select(b=>b.Parent).ToArray());
                for(int n=0;n<14;n++){float e=Error(emitted[110+n],expected[n]);maxPose=Math.Max(maxPose,e);Check(e<.03f,"Source falling pose differs");}
                for(int n=0;n<110;n++){float e=Error(local[n],rig.Local[n]);maxController=Math.Max(maxController,e);Check(e<.003f,"Donor controller motion retained");}poseCount++;
            }
            var ev=tae.Animations.Single(a=>a.ID==fall.NativeAnimation).Events;Check(ev.Count==3&&ev.Single(e=>e.Type==0).GetParameterBytes(false)[0]==51,"Unverified source effects/actions forwarded");
            var fades=ev.Where(e=>e.Type==193).ToArray();Check(fades.Length==2&&fades.All(e=>e.Group?.GroupType==192)&&Math.Abs(fades[0].StartTime-1.5f)<.001f&&Math.Abs(fades[0].EndTime-7f/3)<.001f&&fades[1].EndTime==5,"Source opacity timing/native group changed");
            Check(fades[0].GetParameterBytes(false).SequenceEqual(new byte[]{0,0,128,63,0,0,0,0,0,0,0,0,0,0,0,0})&&fades[1].GetParameterBytes(false).All(b=>b==0),"Native opacity payload changed");
        }
        Reject(()=>HydraHeads.Package(Source,Native,"c8832"),"unallocated model");
        var corrupt=source.PhysicsSHA256.Length>0?BND3.Read(Source("chr/c3531.chrbnd.dcx")!).Files.Single(f=>Leaf(f.Name)=="c3531.hkx").Bytes.ToArray():[];corrupt[^1]^=1;Reject(()=>new HydraHeadsSource(corrupt),"source physics drift");
        foreach(var (path,hash) in hashes)Check(Hash(File.ReadAllBytes(path))==hash,"Original input changed: "+path);
        foreach(var (path,bytes) in result.Files){var file=Path.Combine(output,"package",path);Directory.CreateDirectory(Path.GetDirectoryName(file)!);File.WriteAllBytes(file,bytes);}
        File.WriteAllText(Path.Combine(output,"validation.json"),JsonSerializer.Serialize(new{status="PASS",weighted,maxShape,maxPivot,maxPose,maxController,poseCount,result.Report,inputs=hashes},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"PASS Hydra detached heads:3161vertices/14sourcebones;13sourcebodies/12joints/bothchainedmappers;151frames/5seconds/{poseCount}poses;nativeopacityfade;{hashes.Count}originalarchivesunchanged. Native gameplay pending.");
    }
}
