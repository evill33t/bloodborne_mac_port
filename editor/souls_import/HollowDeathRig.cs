// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text;
using System.Security.Cryptography;
using System.Text.RegularExpressions;

// Owned hollow ragdolls use source dimensions and joints inside the shipped
// Bloodborne hknp scene contract. The native combat skeleton remains a prefix;
// both mapper directions explicitly include the independently animated skin.
internal static class HollowDeathRig {
    internal sealed record Report(string SourceModel,string SourceSHA256,string NativeSHA256,int Bodies,int Constraints,int AnimationBones,int SourceMappings,int NativeMappings,int Capsules,int Boxes,IReadOnlyList<string> Limitations);
    internal sealed record Result(byte[] Physics,Report Report);
    internal sealed class Pack {
        internal readonly byte[] Bytes;internal readonly int Data,Size;
        internal readonly Dictionary<int,int> Pointers=[];internal readonly List<(int At,string Name)> Objects=[];
        internal Pack(byte[] bytes) {
            Bytes=bytes;Need(bytes.Length>=272&&U(0)==0x57e0e057&&U(4)==0x10c0c010&&I(12)==11&&bytes[16]==8&&bytes[17]==1&&I(20)==3,"native pack header");
            int section=64+BitConverter.ToUInt16(bytes,62),names=I(section+20),header=section+128;Data=I(header+20);Size=I(header+24);int global=I(header+28),virtuals=I(header+32),end=I(header+36);
            Need(Data>=0&&Size>=0&&global>=Size&&virtuals>=global&&end>=virtuals&&Data<=bytes.Length-end,"native section bounds");
            for(int i=Size;i+8<=global;i+=8)if(TableInt(i)!=-1)Need(Pointers.TryAdd(TableInt(i),TableInt(i+4)),"duplicate local pointer");
            for(int i=global;i+12<=virtuals;i+=12)if(TableInt(i)!=-1){Need(TableInt(i+4)==2,"external physics pointer");Need(Pointers.TryAdd(TableInt(i),TableInt(i+8)),"duplicate global pointer");}
            foreach(var p in Pointers)Need(p.Key>=0&&p.Key<=Size-8&&p.Value>=0&&p.Value<Size,"invalid physics fixup");
            for(int i=virtuals;i+12<=end;i+=12)if(TableInt(i)!=-1){Need(TableInt(i+4)==0&&TableInt(i)>=0&&TableInt(i)<Size,"invalid virtual pointer");int at=names+TableInt(i+8);Need(at>=0&&at<bytes.Length,"class name pointer");int stop=System.Array.IndexOf(bytes,(byte)0,at);Need(stop>at&&stop-at<256,"class name length");Objects.Add((TableInt(i),Encoding.ASCII.GetString(bytes,at,stop-at)));}
        }
        int TableInt(int at){Need(at>=0&&Data+at<=Bytes.Length-4,"fixup table bounds");return I(Data+at);}
        uint U(int at)=>BitConverter.ToUInt32(Bytes,at);int I(int at)=>BitConverter.ToInt32(Bytes,at);
        internal int Int(int at){Need(at>=0&&at<=Size-4,"physics scalar bounds");return I(Data+at);}
        internal int Pointer(int at)=>Pointers.TryGetValue(at,out int value)?value:throw new InvalidDataException("Missing hollow physics pointer: "+at);
        internal int One(string name)=>Objects.Count(o=>o.Name==name)==1?Objects.Single(o=>o.Name==name).At:throw new InvalidDataException("Expected one native physics "+name);
        internal (int At,int Count) Array(int field,int stride,int max=4096){int count=Int(field+8);Need(count>=0&&count<=max,"physics array budget");int at=count==0?0:Pointer(field);Need((long)at+(long)count*stride<=Size,"physics array bounds");return(at,count);}
        internal byte[] Raw(int at,int size){Need(at>=0&&size>=0&&at<=Size-size,"physics raw bounds");return Bytes.AsSpan(Data+at,size).ToArray();}
        internal string? Text(int field) {
            if(!Pointers.TryGetValue(field,out int at))return null;int stop=System.Array.IndexOf(Bytes,(byte)0,Data+at);Need(stop>=Data+at&&stop-Data-at<256,"physics name length");return stop==Data+at?"":SourceAnimation.DecodeBoneName(Bytes.AsSpan(Data+at,stop-Data-at).ToArray());
        }
        internal Matrix4x4 Qs(int at)=>QsMatrix(Raw(at,48));
        internal HollowDeathSource.Skeleton Skeleton(int at) {
            var bones=Array(at+40,16,256);var parents=Array(at+24,2,256);var pose=Array(at+56,48,256);Need(bones.Count>0&&parents.Count==bones.Count&&pose.Count==bones.Count,"embedded skeleton arrays");
            return new(Text(at+16)??throw new InvalidDataException("Missing skeleton name"),Enumerable.Range(0,bones.Count).Select(i=>Text(bones.At+i*16)??throw new InvalidDataException("Missing bone name")).ToArray(),Enumerable.Range(0,bones.Count).Select(i=>BitConverter.ToInt16(Bytes,Data+parents.At+i*2)).ToArray(),Enumerable.Range(0,bones.Count).Select(i=>Bytes[Data+bones.At+i*16+8]!=0).ToArray(),Enumerable.Range(0,bones.Count).Select(i=>Qs(pose.At+i*48)).ToArray());
        }
    }
    internal static void Need(bool b,string why){if(!b)throw new InvalidDataException("Hollow ragdoll: "+why);}
    internal static Matrix4x4 QsMatrix(byte[] raw) {
        var t=new Vector3(BitConverter.ToSingle(raw,0),BitConverter.ToSingle(raw,4),BitConverter.ToSingle(raw,8));var q=new Quaternion(BitConverter.ToSingle(raw,16),BitConverter.ToSingle(raw,20),BitConverter.ToSingle(raw,24),BitConverter.ToSingle(raw,28));var s=new Vector3(BitConverter.ToSingle(raw,32),BitConverter.ToSingle(raw,36),BitConverter.ToSingle(raw,40));
        Need(float.IsFinite(t.LengthSquared())&&float.IsFinite(s.LengthSquared())&&Math.Abs(q.LengthSquared()-1)<.001f,"invalid mapper transform");return Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(q)*Matrix4x4.CreateTranslation(t);
    }
    internal static byte[] QsBytes(Matrix4x4 m) {
        Need(Matrix4x4.Decompose(m,out var s,out var q,out var t)&&float.IsFinite(t.LengthSquared())&&float.IsFinite(s.LengthSquared())&&Math.Abs(q.LengthSquared()-1)<.001f,"non-QS mapper transform");
        return new[]{t.X,t.Y,t.Z,0,q.X,q.Y,q.Z,q.W,s.X,s.Y,s.Z,0}.SelectMany(BitConverter.GetBytes).ToArray();
    }
    internal static byte[] ReadPhysics(Func<string,byte[]?> read,string model) {
        Need(model is "c2500" or "c2540" or "c2550" or "c2560","unsupported source profile");var raw=read("chr/"+model+".chrbnd.dcx")??throw new FileNotFoundException("Source hollow ragdoll unavailable");
        return BND3.Read(raw).Files.Single(f=>f.ID==300&&f.Name.Replace('\\','/').Split('/').Last().Equals(model+".hkx",StringComparison.OrdinalIgnoreCase)).Bytes.ToArray();
    }
    static Matrix4x4 Inverse(Matrix4x4 m){Need(Matrix4x4.Invert(m,out var inverse),"singular mapper reference");return inverse;}
    static string Joint(string name) {
        string value=new(name.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());value=Regex.Replace(value,@"\Aragdoll","");value=Regex.Replace(value,@"0+(?=\d$)","");return value.StartsWith("master",StringComparison.Ordinal)?"pelvis"+value[6..]:value;
    }
    static void F(byte[] bytes,int at,float value){Need(float.IsFinite(value),"nonfinite physics value");BitConverter.GetBytes(value).CopyTo(bytes,at);}
    static void Vector(byte[] bytes,int at,Vector4 v){F(bytes,at,v.X);F(bytes,at+4,v.Y);F(bytes,at+8,v.Z);F(bytes,at+12,v.W);}
    static void Half(byte[] bytes,int at,float value){Need(float.IsFinite(value)&&value>=0&&value<=1000,"invalid native half coefficient");BitConverter.GetBytes(unchecked((ushort)(BitConverter.SingleToUInt32Bits(value)>>16))).CopyTo(bytes,at);}
    // Havok 2015 and 2014 have equal-sized constraint classes but different
    // angular-motor and hinge-limit atom layouts. Translate typed fields;
    // never forward a 2015 pointer slot into a 2014 object.
    internal static (byte[] Bytes,int[] MotorOffsets) ConstraintData(HollowDeathSource.Constraint c) {
        var raw=c.Bytes.ToArray();
        if(c.Type=="hkpRagdollConstraintData") {
            Need(raw.Length==416,"source ragdoll atom size");Array.Clear(raw,196,12);
            foreach(int at in new[]{304,336,368}) {Need(BitConverter.ToUInt16(raw,at)==(at==304?15:16)&&BitConverter.ToSingle(raw,at+20)==1,"source angular damping/atom contract changed");Array.Clear(raw,at+20,12);}
            return(raw,c.MotorOffsets.ToArray());
        }
        Need(c.Type=="hkpLimitedHingeConstraintData"&&raw.Length==304&&BitConverter.ToUInt16(raw,192)==18&&BitConverter.ToUInt16(raw,224)==17&&BitConverter.ToUInt16(raw,240)==14&&BitConverter.ToUInt16(raw,272)==12&&BitConverter.ToUInt16(raw,288)==5,"source hinge atoms changed");
        Need(raw[244]==(raw[243]+1)%3&&BitConverter.ToSingle(raw,260)==1,"source hinge cosine-axis/damping policy changed");
        var result=new byte[304];raw.AsSpan(0,192).CopyTo(result);raw.AsSpan(192,4).CopyTo(result.AsSpan(192));
        raw.AsSpan(208,4).CopyTo(result.AsSpan(204)); // targetAngle
        raw.AsSpan(224,16).CopyTo(result.AsSpan(232)); // angular friction
        raw.AsSpan(240,4).CopyTo(result.AsSpan(248)); // limit type/enabled/axis
        raw.AsSpan(248,12).CopyTo(result.AsSpan(252)); // min/max/tau
        raw.AsSpan(272,16).CopyTo(result.AsSpan(264));raw.AsSpan(288,16).CopyTo(result.AsSpan(280));
        Need(c.MotorOffsets.All(i=>i==200),"source hinge motor location changed");return(result,c.MotorOffsets.Select(_=>208).ToArray());
    }
    internal static Result Convert(byte[] nativePhysics,byte[] sourcePhysics,NativeRigAppend.Plan plan,string sourceModel) {
        Need(plan.Original.Names.Length==84&&plan.Added.Count>0&&plan.Added.All(b=>b.Name.StartsWith("DS1_",StringComparison.Ordinal)),"owned ordinary hollow rig only");
        var source=new HollowDeathSource(sourcePhysics,sourceModel);var native=new Pack(nativePhysics);int system=native.One("hknpRagdollData");
        int nativeRoot=native.One("hkRootLevelContainer");var variants=native.Array(nativeRoot,24,8);
        string[] names=["Merged Animation Container","Physics Scene Data","Physics Ragdoll","SkeletonMapper","SkeletonMapper"];
        string[] classes=["hkaAnimationContainer","hknpPhysicsSceneData","hknpRagdollData","hkaSkeletonMapper","hkaSkeletonMapper"];
        Need(variants.Count==5&&Enumerable.Range(0,5).All(i=>native.Text(variants.At+i*24)==names[i]&&native.Text(variants.At+i*24+8)==classes[i]),"native root variants changed");
        int nativeScene=native.One("hknpPhysicsSceneData");var systems=native.Array(nativeScene+16,8,4);
        Need(systems.Count==1&&native.Pointer(systems.At)==system&&native.Pointer(variants.At+2*24+16)==system&&!native.Pointers.ContainsKey(nativeScene+32),"native shared scene/ragdoll contract changed");
        var nativeBones=native.Objects.Where(o=>o.Name=="hkaSkeleton").Select(o=>(o.At,Value:native.Skeleton(o.At))).ToArray();Need(nativeBones.Length==2,"native skeleton population");var nativeAnimation=nativeBones.Single(s=>s.Value.Names.Length==84);var nativeRag=nativeBones.Single(s=>s.Value.Names.Length==24);
        Need(nativeAnimation.Value.Names.SequenceEqual(plan.Original.Names),"ragdoll donor differs from animation rig");
        foreach(var o in native.Objects.Where(o=>o.Name is "hkpRagdollConstraintData" or "hkpLimitedHingeConstraintData")) {
            int[] offsets=o.Name=="hkpRagdollConstraintData"?[32,176,192,288,304,336,368,400]:[32,176,192,232,248,264,280];
            int[] atoms=o.Name=="hkpRagdollConstraintData"?[2,23,19,17,15,16,16,5]:[2,23,18,17,14,12,5];
            Need(Enumerable.Range(0,offsets.Length).All(i=>BitConverter.ToUInt16(native.Bytes,native.Data+o.At+offsets[i])==atoms[i]),"native constraint atom layout changed");
            int[] motors=o.Name=="hkpRagdollConstraintData"?[256,264,272]:[208];
            Need(motors.All(i=>!native.Pointers.TryGetValue(o.At+i,out int at)||at==native.One("hkpPositionConstraintMotor")),"native constraint motor layout changed");
        }
        var nativeBodies=native.Array(system+64,96,32);var nativeMaterials=native.Array(system+16,80,32);var nativeProperties=native.Array(system+32,64,32);var nativeMotions=native.Array(system+48,112,32);var nativeMap=native.Array(system+128,4,32);
        Need(nativeBodies.Count==19&&nativeMaterials.Count==19&&nativeProperties.Count==19&&nativeMotions.Count==19&&nativeMap.Count==24,"native dynamic ragdoll population");
        int[] originalMap=Enumerable.Range(0,nativeMap.Count).Select(i=>native.Int(nativeMap.At+i*4)).ToArray();
        var nativeByJoint=Enumerable.Range(0,19).ToDictionary(i=>Joint(nativeRag.Value.Names[Array.IndexOf(originalMap,i)]),i=>i,StringComparer.Ordinal);
        var shapes=native.Objects.Where(o=>o.Name=="hknpCapsuleShape").ToArray();Need(shapes.Length==19,"native capsule template count");
        var shapeTemplate=HollowDeathPacking.Shape.Template(native,shapes[0].At);
        var animInfo=NativeAnimation.Skeleton(plan.Skeleton);var animPack=new NativeAnimation.Pack(plan.Skeleton);var animBones=animPack.Array(animPack.Object("hkaSkeleton")+40,16,256);
        var full=new HollowDeathPacking.hkaSkeleton(new(animInfo.Name,animInfo.Names,animInfo.Parents.Select(p=>checked((short)p)).ToArray(),Enumerable.Range(0,animInfo.Names.Length).Select(i=>plan.Skeleton[animPack.Data+animBones.At+i*16+8]!=0).ToArray(),animInfo.Local));
        var rag=new HollowDeathPacking.hkaSkeleton(source.Ragdoll);var sourceIndices=full.Info.Names.Select((name,i)=>(name,i)).ToDictionary(p=>p.name,p=>p.i,StringComparer.Ordinal);
        var ragIndices=source.Ragdoll.Names.Select((name,i)=>(Name:Joint(name),i)).ToDictionary(p=>p.Name,p=>p.i,StringComparer.Ordinal);
        var forward=source.Mappings.Single(m=>m.B.Names.SequenceEqual(source.Ragdoll.Names));var backward=source.Mappings.Single(m=>m.A.Names.SequenceEqual(source.Ragdoll.Names));
        int Visible(string name)=>sourceIndices.TryGetValue("DS1_"+name,out int index)?index:-1;
        var fSimple=forward.Simple.Select(m=>{int a=Visible(forward.A.Names[m.A]);Need(a>=0,"source physics-driving bone omitted from visible closure: "+forward.A.Names[m.A]);return new HollowDeathPacking.Simple(checked((short)a),m.B,m.Transform);}).ToList();
        var fChain=forward.Chains.Select(c=>new HollowDeathPacking.Chain(checked((short)Visible(forward.A.Names[c.StartA])),checked((short)Visible(forward.A.Names[c.EndA])),c.StartB,c.EndB,c.Transforms)).ToList();Need(fChain.All(c=>c.StartA>=0&&c.EndA>=0),"source forward chain omitted");
        var bSimple=backward.Simple.Where(m=>Visible(backward.B.Names[m.B])>=0).Select(m=>new HollowDeathPacking.Simple(m.A,checked((short)Visible(backward.B.Names[m.B])),m.Transform)).ToList();
        var bChain=backward.Chains.Where(c=>Visible(backward.B.Names[c.StartB])>=0&&Visible(backward.B.Names[c.EndB])>=0).Select(c=>new HollowDeathPacking.Chain(c.StartA,c.EndA,checked((short)Visible(backward.B.Names[c.StartB])),checked((short)Visible(backward.B.Names[c.EndB])),c.Transforms)).ToList();
        var ragWorld=NativeAnimation.Worlds(source.Ragdoll.Local,source.Ragdoll.Parents.Select(p=>(int)p).ToArray());int nativeMappings=0;
        foreach(var item in native.Objects.Where(o=>o.Name=="hkaSkeletonMapper")) {
            if(native.Pointer(item.At+16)!=nativeRag.At)continue;var simple=native.Array(item.At+80,64,64);
            for(int i=0;i<simple.Count;i++) {
                int at=simple.At+i*64,a=BitConverter.ToInt16(native.Bytes,native.Data+at),b=BitConverter.ToInt16(native.Bytes,native.Data+at+2);
                if(!ragIndices.TryGetValue(Joint(nativeRag.Value.Names[a]),out int mappedRag))continue;
                bSimple.Add(new(checked((short)mappedRag),checked((short)b),QsBytes(plan.Original.World[b]*Inverse(ragWorld[mappedRag]))));nativeMappings++;
            }
        }
        Need(nativeMappings>=18&&bSimple.Count>=36&&bSimple.Select(m=>m.B).Distinct().Count()==bSimple.Count,"native/source back mapper collision");
        var motor=new HollowDeathPacking.hkpPositionConstraintMotor(source.Motor);var convertedShapes=new List<HKX2.hknpShape>();
        var materials=new List<HKX2.hknpMaterial>();var properties=new List<HKX2.hknpMotionProperties>();var motions=new List<HKX2.hknpMotionCinfo>();var bodies=new List<HKX2.hknpBodyCinfo>();
        for(int i=0;i<source.Bodies.Count;i++) {
            var body=source.Bodies[i];string joint=Joint(source.Ragdoll.Names[Array.IndexOf(source.BoneToBody,i)]);Need(nativeByJoint.TryGetValue(joint,out int donor),"unmatched source rigid body: "+joint);
            var shape=HollowDeathPacking.Shape.Convert(shapeTemplate,body.Shape);convertedShapes.Add(shape);
            var mat=native.Raw(nativeMaterials.At+donor*80,80);Half(mat,18,body.Friction);Half(mat,20,body.Friction);Half(mat,22,body.Restitution);Need(!native.Pointers.ContainsKey(nativeMaterials.At+donor*80+48),"native dynamic surface velocity");materials.Add(new HollowDeathPacking.Material(mat,native.Text(nativeMaterials.At+donor*80)));
            var prop=native.Raw(nativeProperties.At+donor*64,64);F(prop,8,body.Gravity);F(prop,12,body.TimeFactor);F(prop,24,body.LinearDamping);F(prop,28,body.AngularDamping);properties.Add(new HollowDeathPacking.Properties(prop));
            var motion=native.Raw(nativeMotions.At+donor*112,112);BitConverter.GetBytes(checked((ushort)i)).CopyTo(motion,0);F(motion,4,body.InertiaMass.W);Vector(motion,32,new(body.InertiaMass.X,body.InertiaMass.Y,body.InertiaMass.Z,1));Vector(motion,48,new(body.Center.X,body.Center.Y,body.Center.Z,BitConverter.ToSingle(motion,60)));Vector(motion,64,new(body.Orientation.X,body.Orientation.Y,body.Orientation.Z,body.Orientation.W));Vector(motion,80,body.LinearVelocity);Vector(motion,96,body.AngularVelocity);motions.Add(new HollowDeathPacking.Motion(motion));
            var info=native.Raw(nativeBodies.At+donor*96,96);BitConverter.GetBytes(i).CopyTo(info,12);BitConverter.GetBytes(checked((ushort)i)).CopyTo(info,18);Vector(info,48,new(body.Transform.Translation,1));Need(Matrix4x4.Decompose(body.Transform,out var scale,out var rotation,out _)&&Vector3.Distance(scale,Vector3.One)<.001f,"nonrigid source body transform");Vector(info,64,new(rotation.X,rotation.Y,rotation.Z,rotation.W));Need(!native.Pointers.ContainsKey(nativeBodies.At+donor*96+88),"native body local frame");bodies.Add(new HollowDeathPacking.Body(info,body.Name,shape));
        }
        var constraints=source.Constraints.Select(c=>{var converted=ConstraintData(c);return new HKX2.hknpConstraintCinfo{m_bodyA=(uint)c.A,m_bodyB=(uint)c.B,m_flags=0,m_constraintData=c.Type=="hkpRagdollConstraintData"?new HollowDeathPacking.hkpRagdollConstraintData(converted.Bytes,converted.MotorOffsets,motor):new HollowDeathPacking.hkpLimitedHingeConstraintData(converted.Bytes,converted.MotorOffsets,motor)};}).ToList();
        var data=new HKX2.hknpRagdollData{m_materials=materials,m_motionProperties=properties,m_motionCinfos=motions,m_bodyCinfos=bodies,m_constraintCinfos=constraints,m_referencedObjects=convertedShapes.Cast<HKX2.hkReferencedObject>().ToList(),m_name=native.Text(system+112),m_skeleton=rag,m_boneToBodyMap=source.BoneToBody.ToList()};
        var scene=new HKX2.hknpPhysicsSceneData{m_systemDatas=[data],m_worldCinfo=null};var container=new HKX2.hkaAnimationContainer{m_skeletons=[full,rag],m_animations=[],m_bindings=[],m_attachments=[],m_skins=[]};
        var mapForward=new HollowDeathPacking.hkaSkeletonMapper(full,rag,fSimple,fChain,forward.Unmapped);
        var mapped=bSimple.Select(m=>(int)m.B).Concat(bChain.SelectMany(c=>new[]{(int)c.StartB,(int)c.EndB})).ToHashSet();var unmapped=Enumerable.Range(0,full.Info.Names.Length).Where(i=>!mapped.Contains(i)).Select(i=>checked((short)i)).ToArray();
        var mapBackward=new HollowDeathPacking.hkaSkeletonMapper(rag,full,bSimple,bChain,unmapped);
        var root=new HKX2.hkRootLevelContainer{m_namedVariants=[new(){m_name="Merged Animation Container",m_className="hkaAnimationContainer",m_variant=container},new(){m_name="Physics Scene Data",m_className="hknpPhysicsSceneData",m_variant=scene},new(){m_name="Physics Ragdoll",m_className="hknpRagdollData",m_variant=data},new(){m_name="SkeletonMapper",m_className="hkaSkeletonMapper",m_variant=mapForward},new(){m_name="SkeletonMapper",m_className="hkaSkeletonMapper",m_variant=mapBackward}]};
        using var stream=new MemoryStream();new HKX2.PackFileSerializer().Serialize(root,new BinaryWriterEx(false,stream));var output=stream.ToArray();output[18]=1;_ = new Pack(output);
        return new(output,new(sourceModel,ConvertHex(sourcePhysics),ConvertHex(nativePhysics),bodies.Count,constraints.Count,full.Info.Names.Length,bSimple.Count-nativeMappings,nativeMappings,source.Bodies.Count(b=>b.Shape.Type=="hkpCapsuleShape"),source.Bodies.Count(b=>b.Shape.Type=="hkpBoxShape"),["Source hollow bodies, mass/inertia, convex dimensions, joint frames/limits and skin mappings are translated into native hknp physics. Havok 2015 hinge fields are explicitly adapted to Havok 2014. Native solver, implicit angular damping, deactivation, speed limits and collision-layer policy remain Bloodborne's.","Ragdoll conversion is limited to the four ordinary hollow families; other imported character families require their own verified physics adapters. Source cape remains a static settled body-following mesh."]));
    }
    static string ConvertHex(byte[] bytes)=>System.Convert.ToHexStringLower(SHA256.HashData(bytes));
}

namespace HollowDeathPacking {
    using H=HollowDeathRig;
    internal sealed class Material(byte[] raw,string? name):HKX2.hknpMaterial {public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b){s.WriteStringPointer(b,name);b.WriteBytes(raw.AsSpan(8).ToArray());}}
    internal sealed class Properties(byte[] raw):HKX2.hknpMotionProperties {public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b)=>b.WriteBytes(raw);}
    internal sealed class Motion(byte[] raw):HKX2.hknpMotionCinfo {public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b)=>b.WriteBytes(raw);}
    internal sealed class Body(byte[] raw,string name,HKX2.hknpShape shape):HKX2.hknpBodyCinfo {public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b){s.WriteClassPointer(b,shape);b.WriteBytes(raw.AsSpan(8,24).ToArray());s.WriteStringPointer(b,name);b.WriteBytes(raw.AsSpan(40).ToArray());}}
    internal sealed class hkpPositionConstraintMotor(byte[] raw):HKX2.hkpPositionConstraintMotor {public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b)=>b.WriteBytes(raw);}
    static class Constraint {
        internal static void Write(HKX2.PackFileSerializer s,BinaryWriterEx b,byte[] raw,int[] offsets,HKX2.hkpConstraintMotor motor) {int last=0;foreach(int at in offsets.Order()){b.WriteBytes(raw.AsSpan(last,at-last).ToArray());s.WriteClassPointer(b,motor);last=at+8;}b.WriteBytes(raw.AsSpan(last).ToArray());}
    }
    internal sealed class hkpRagdollConstraintData(byte[] raw,int[] offsets,HKX2.hkpConstraintMotor motor):HKX2.hkpRagdollConstraintData {public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b)=>Constraint.Write(s,b,raw,offsets,motor);}
    internal sealed class hkpLimitedHingeConstraintData(byte[] raw,int[] offsets,HKX2.hkpConstraintMotor motor):HKX2.hkpLimitedHingeConstraintData {public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b)=>Constraint.Write(s,b,raw,offsets,motor);}
    internal sealed class hkaSkeleton(HollowDeathSource.Skeleton info):HKX2.hkaSkeleton {
        internal readonly HollowDeathSource.Skeleton Info=info;
        public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b) {b.WriteUInt64(0);b.WriteUInt64(0);s.WriteStringPointer(b,Info.Name);s.WriteUInt16Array(b,Info.Parents.Select(p=>unchecked((ushort)p)).ToList());s.WriteClassArray(b,Info.Names.Select((name,i)=>new HKX2.hkaBone{m_name=name,m_lockTranslation=Info.Locked[i]}).ToList());s.WriteClassArray(b,Info.Local.Select(m=>new Transform(H.QsBytes(m))).ToList());for(int i=0;i<4;i++)s.WriteUInt32Array(b,[]);}
    }
    internal sealed class Transform(byte[] bytes):HKX2.IHavokObject {public uint Signature=>0;public void Read(HKX2.PackFileDeserializer s,BinaryReaderEx b)=>throw new NotSupportedException();public void Write(HKX2.PackFileSerializer s,BinaryWriterEx b)=>b.WriteBytes(bytes);}
    internal sealed record Simple(short A,short B,byte[] Transform):HKX2.IHavokObject {public uint Signature=>0;public void Read(HKX2.PackFileDeserializer s,BinaryReaderEx b)=>throw new NotSupportedException();public void Write(HKX2.PackFileSerializer s,BinaryWriterEx b){b.WriteInt16(A);b.WriteInt16(B);b.WriteBytes(new byte[12]);b.WriteBytes(Transform);}}
    internal sealed record Chain(short StartA,short EndA,short StartB,short EndB,byte[] Transforms):HKX2.IHavokObject {public uint Signature=>0;public void Read(HKX2.PackFileDeserializer s,BinaryReaderEx b)=>throw new NotSupportedException();public void Write(HKX2.PackFileSerializer s,BinaryWriterEx b){b.WriteInt16(StartA);b.WriteInt16(EndA);b.WriteInt16(StartB);b.WriteInt16(EndB);b.WriteBytes(new byte[8]);b.WriteBytes(Transforms);}}
    internal sealed class hkaSkeletonMapper(hkaSkeleton a,hkaSkeleton z,List<Simple> simple,List<Chain> chains,short[] unmapped):HKX2.hkaSkeletonMapper {
        public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b){b.WriteUInt64(0);b.WriteUInt64(0);s.WriteClassPointer(b,a);s.WriteClassPointer(b,z);for(int i=0;i<3;i++)s.WriteUInt32Array(b,[]);s.WriteClassArray(b,simple);s.WriteClassArray(b,chains);s.WriteUInt16Array(b,unmapped.Select(i=>unchecked((ushort)i)).ToList());b.WriteBytes(H.QsBytes(Matrix4x4.Identity));b.WriteBoolean(true);b.WriteBytes(new byte[15]);}
    }
    internal sealed class hknpCapsuleShape(byte[] raw):HKX2.hknpCapsuleShape {public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b)=>b.WriteBytes(raw);}
    internal sealed class hknpConvexPolytopeShape(byte[] raw):HKX2.hknpConvexPolytopeShape {public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b)=>b.WriteBytes(raw);}
    internal static class Shape {
        internal static byte[] Template(H.Pack pack,int at) {
            int end=96;foreach(var (offset,stride) in new[]{(40,16),(44,16),(48,4),(52,1)}){var raw=pack.Raw(at+offset,4);int count=BitConverter.ToUInt16(raw),relative=BitConverter.ToUInt16(raw,2);H.Need(count==(offset==48?6:offset==52?24:8)&&relative>=4,"native capsule relative arrays");end=Math.Max(end,offset+relative+count*stride);}
            H.Need(!pack.Pointers.Keys.Any(p=>p>=at&&p<at+end),"native capsule external properties");return pack.Raw(at,(end+15)&~15);
        }
        static Vector3 V(byte[] raw,int at)=>new(BitConverter.ToSingle(raw,at),BitConverter.ToSingle(raw,at+4),BitConverter.ToSingle(raw,at+8));
        static void Set(byte[] raw,int at,Vector3 v){BitConverter.GetBytes(v.X).CopyTo(raw,at);BitConverter.GetBytes(v.Y).CopyTo(raw,at+4);BitConverter.GetBytes(v.Z).CopyTo(raw,at+8);}
        internal static HKX2.hknpShape Convert(byte[] template,HollowDeathSource.Shape source) {
            var bytes=template.ToArray();int vertex=40+BitConverter.ToUInt16(bytes,42),planes=44+BitConverter.ToUInt16(bytes,46);Matrix4x4 change;
            if(source.Type=="hkpCapsuleShape") {
                var a=V(bytes,64);var z=V(bytes,80);var from=z-a;var to=source.B-source.A;float x=from.Length(),y=to.Length(),radius=BitConverter.ToSingle(bytes,20);H.Need(x>.000001f&&y>.000001f&&radius>0,"degenerate capsule axis");var u=from/x;var v=to/y;float dot=Math.Clamp(Vector3.Dot(u,v),-1,1);var rotation=dot<-.99999f?Quaternion.CreateFromAxisAngle(Vector3.Normalize(Vector3.Cross(u,Math.Abs(u.X)<.9f?Vector3.UnitX:Vector3.UnitY)),MathF.PI):Quaternion.Normalize(new Quaternion(Vector3.Cross(u,v),1+dot));
                float radial=source.Radius/radius,axial=y/x,d=axial-radial;var stretch=new Matrix4x4(radial+d*u.X*u.X,d*u.X*u.Y,d*u.X*u.Z,0,d*u.Y*u.X,radial+d*u.Y*u.Y,d*u.Y*u.Z,0,d*u.Z*u.X,d*u.Z*u.Y,radial+d*u.Z*u.Z,0,0,0,0,1);
                change=Matrix4x4.CreateTranslation(-(a+z)*.5f)*stretch*Matrix4x4.CreateFromQuaternion(rotation)*Matrix4x4.CreateTranslation((source.A+source.B)*.5f+source.Translation);Set(bytes,64,source.A+source.Translation);Set(bytes,80,source.B+source.Translation);
            } else {
                H.Need(source.Type=="hkpBoxShape"&&source.Half.X>0&&source.Half.Y>0&&source.Half.Z>0,"unsupported convex shape");var vertices=Enumerable.Range(0,8).Select(i=>V(bytes,vertex+i*16)).ToArray();var lo=vertices.Aggregate(Vector3.Min);var hi=vertices.Aggregate(Vector3.Max);change=Matrix4x4.CreateTranslation(-(lo+hi)*.5f)*Matrix4x4.CreateScale(source.Half*2/(hi-lo))*Matrix4x4.CreateTranslation(source.Translation);
            }
            H.Need(Matrix4x4.Invert(change,out var inverse),"singular convex conversion");var normal=Matrix4x4.Transpose(inverse);
            for(int i=0;i<8;i++)Set(bytes,vertex+i*16,Vector3.Transform(V(bytes,vertex+i*16),change));
            for(int i=0;i<8;i++){int at=planes+i*16;var plane=new Vector4(V(bytes,at),BitConverter.ToSingle(bytes,at+12));var converted=Vector4.Transform(plane,normal);float length=new Vector3(converted.X,converted.Y,converted.Z).Length();if(length>0)converted/=length;Set(bytes,at,new(converted.X,converted.Y,converted.Z));BitConverter.GetBytes(converted.W).CopyTo(bytes,at+12);}
            BitConverter.GetBytes(source.Radius).CopyTo(bytes,20);return source.Type=="hkpCapsuleShape"?new hknpCapsuleShape(bytes):new hknpConvexPolytopeShape(bytes);
        }
    }
}
