// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using Skeleton=HydraPhysicsSource.Skeleton;
using PositionMotor=HydraPhysicsSource.PositionMotor;
using Constraint=HydraPhysicsSource.Constraint;
using Stabilization=HydraPhysicsSource.Stabilization;
using BallSocket=HydraPhysicsSource.BallSocket;
using Motors=HydraPhysicsSource.Motors;
using Friction=HydraPhysicsSource.Friction;
using AngularLimit=HydraPhysicsSource.AngularLimit;

// Pinwheel c3320 has25 authored capsules and24 plain hinges. Keep its own
// chained animation mapper, geometry and collision exclusions intact.
internal sealed class PinwheelPhysicsSource {
    internal sealed record Mapping(Skeleton A,Skeleton B,IReadOnlyList<HollowDeathSource.Simple> Simple,
        IReadOnlyList<HollowDeathSource.Chain> Chains,short[] Unmapped,byte[] ExtractedMotion);
    internal sealed record Body(int Index,string Name,HollowDeathSource.Shape Shape,Matrix4x4 Rest,
        Vector4 InertiaMassInverse,Vector4 CenterOfMass,Quaternion Orientation,Vector4 LinearVelocity,Vector4 AngularVelocity,
        float Friction,float Restitution,float LinearDamping,float AngularDamping,float Gravity,float TimeFactor,byte MotionType,uint CollisionFilter,ulong UserData);
    internal const string Model="c3320";
    internal const string ExpectedSHA256="ecdb7618ec97695943e3aebff41eafa95ee6b6acdf3eca0ad75114229ff92950";
    internal readonly string PhysicsSHA256;
    internal readonly Skeleton Animation,Ragdoll;
    internal readonly IReadOnlyList<Body> Bodies;
    internal readonly IReadOnlyList<Constraint> Constraints;
    internal readonly IReadOnlyList<Mapping> Mappings;
    internal readonly IReadOnlyList<PositionMotor> PositionMotors;
    internal readonly int[] BoneToBody;
    readonly byte[] bytes;
    readonly SourceCollision.Tag tag;
    readonly Dictionary<int,Skeleton> skeletons=[];
    static void Need(bool condition,string why){if(!condition)throw new InvalidDataException("Source Pinwheel golem physics: "+why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    int At(SourceCollision.Tag.Item item,params string[] fields)=>tag.Nested(item.Type,item.At,fields).At;
    SourceCollision.Tag.Item Ref(SourceCollision.Tag.Item item,params string[] fields)=>tag.Reference(At(item,fields));
    float Float(int at){float f=BitConverter.ToSingle(bytes,at);Need(float.IsFinite(f),"nonfinite scalar");return f;}
    float Scalar(SourceCollision.Tag.Item item,params string[] fields)=>Float(At(item,fields));
    byte Byte(SourceCollision.Tag.Item item,params string[] fields)=>tag.Byte(At(item,fields));
    short Short(int at)=>unchecked((short)tag.UShort(at));
    Vector4 Vector4(int at)=>new(Float(at),Float(at+4),Float(at+8),Float(at+12));
    float Half(int at){float f=BitConverter.Int32BitsToSingle(tag.UShort(at)<<16);Need(float.IsFinite(f)&&f>=0&&f<=1000,"invalid motion coefficient");return f;}
    string Text(SourceCollision.Tag.Item item,string field){var chars=Ref(item,field);Need(chars.Count is >1 and <=256,"invalid name");var span=bytes.AsSpan(chars.At,chars.Count);Need(span[^1]==0,"unterminated name");return SourceAnimation.DecodeBoneName(span[..^1].ToArray());}
    string? OptionalText(SourceCollision.Tag.Item item,string field){var chars=Ref(item,field);return chars.Count==0?null:chars.Count==1?"":Text(item,field);}
    static Matrix4x4 Affine(Matrix4x4 m) {m.M14=m.M24=m.M34=0;m.M44=1;return m;}
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{a.M11-b.M11,a.M12-b.M12,a.M13-b.M13,a.M21-b.M21,a.M22-b.M22,a.M23-b.M23,a.M31-b.M31,a.M32-b.M32,a.M33-b.M33,a.M41-b.M41,a.M42-b.M42,a.M43-b.M43}.Max(Math.Abs);
    Matrix4x4 Frame(int at) {var m=Affine(tag.Transform(at));Need(Math.Abs(m.GetDeterminant()-1)<.002f,"nonrigid rest/joint frame");return m;}
    Skeleton ReadSkeleton(SourceCollision.Tag.Item item) {
        if(skeletons.TryGetValue(item.At,out var existing))return existing;
        var bones=tag.Array(item,"bones");var parents=tag.Array(item,"parentIndices");var pose=tag.Array(item,"referencePose");
        Need(bones.Count is 25 or 113&&parents.Count==bones.Count&&pose.Count==bones.Count&&tag.Array(item,"partitions").Count==0,"unsupported skeleton population/partitions");
        var names=new string[bones.Count];var indices=new short[bones.Count];var locked=new bool[bones.Count];var local=new Matrix4x4[bones.Count];
        for(int i=0;i<bones.Count;i++){var b=tag.Element(bones,i);names[i]=Text(b,"name");indices[i]=Short(tag.Element(parents,i).At);locked[i]=Byte(b,"lockTranslation")!=0;local[i]=tag.QsTransform(tag.Element(pose,i).At);Need(indices[i]>=-1&&indices[i]<i,"skeleton is not parent-first");}
        Need(indices.Count(i=>i==-1)==1&&names.Distinct(StringComparer.Ordinal).Count()==names.Length,"ambiguous skeleton roots/names");
        var result=new Skeleton(item.At,Text(item,"name"),names,indices,locked,local);skeletons.Add(item.At,result);return result;
    }
    Mapping ReadMapping(SourceCollision.Tag.Item item) {
        var nested=tag.Nested(item.Type,item.At,"mapping");var m=new SourceCollision.Tag.Item(nested.Type,nested.At,1);var a=ReadSkeleton(Ref(m,"skeletonA"));var b=ReadSkeleton(Ref(m,"skeletonB"));
        Need(Byte(m,"keepUnmappedLocal")==1&&tag.Int(At(m,"mappingType"))==0,"unsupported mapper policy");
        foreach(string empty in new[]{"partitionMap","simpleMappingPartitionRanges","chainMappingPartitionRanges"})Need(tag.Array(m,empty).Count==0,"partitioned Pinwheel mapping");
        int extracted=At(m,"extractedMotionMapping");Need(bytes.AsSpan(extracted,48).IndexOfAnyExcept((byte)0)<0,"unexpected extracted mapper transform");
        var simple=tag.Array(m,"simpleMappings");var chains=tag.Array(m,"chainMappings");var unmapped=tag.Array(m,"unmappedBones");
        Need(simple.Count==25&&tag.ByteSize(simple.Type)==64&&chains.Count==3&&tag.ByteSize(chains.Type)==112,"Pinwheel mapper population/layout");
        var maps=Enumerable.Range(0,simple.Count).Select(i=>{int at=tag.Element(simple,i).At;return new HollowDeathSource.Simple(Short(at),Short(at+2),bytes.AsSpan(at+16,48).ToArray());}).ToArray();
        var chain=Enumerable.Range(0,chains.Count).Select(i=>{int at=tag.Element(chains,i).At;return new HollowDeathSource.Chain(Short(at),Short(at+2),Short(at+4),Short(at+6),bytes.AsSpan(at+16,96).ToArray());}).ToArray();
        var unmap=Enumerable.Range(0,unmapped.Count).Select(i=>Short(tag.Element(unmapped,i).At)).ToArray();
        Need(maps.All(s=>s.A>=0&&s.A<a.Names.Length&&s.B>=0&&s.B<b.Names.Length)&&maps.Select(s=>s.A).Distinct().Count()==25&&maps.Select(s=>s.B).Distinct().Count()==25,"invalid/duplicate simple ownership");
        Need(chain.All(c=>c.StartA>=0&&c.EndA<a.Names.Length&&c.StartA<=c.EndA&&c.StartB>=0&&c.EndB<b.Names.Length&&c.StartB<=c.EndB),"invalid chain ownership");
        Need(unmap.All(i=>i>=0&&i<b.Names.Length)&&unmap.Distinct().Count()==unmap.Length&&!unmap.Intersect(maps.Select(s=>s.B)).Any(),"invalid unmapped ownership");
        return new(a,b,maps,chain,unmap,bytes.AsSpan(extracted,48).ToArray());
    }
    PositionMotor ReadMotor(SourceCollision.Tag.Item item) {
        Need(tag.Name(item.Type)=="hkpPositionConstraintMotor"&&tag.ByteSize(item.Type)==48,"unsupported constraint motor");
        float min=Scalar(item,"minForce"),max=Scalar(item,"maxForce");Need(min<=max,"invalid motor force range");
        return new(item.At,Byte(item,"type"),min,max,Scalar(item,"tau"),Scalar(item,"damping"),Scalar(item,"proportionalRecoveryVelocity"),Scalar(item,"constantRecoveryVelocity"));
    }
    Constraint ReadConstraint(SourceCollision.Tag.Item item,int index,IReadOnlyDictionary<int,int> bodyIndices,IReadOnlyDictionary<int,PositionMotor> motors) {
        var entities=tag.Nested(item.Type,item.At,"entities");var aa=tag.Reference(entities.At);var bb=tag.Reference(entities.At+8);
        Need(bodyIndices.TryGetValue(aa.At,out int a),"constraint body A reference");Need(bodyIndices.TryGetValue(bb.At,out int b)&&a!=b,"constraint body B reference");
        var data=Ref(item,"data");string type=tag.Name(data.Type);bool hinge=type=="hkpHingeConstraintData";
        Need(hinge&&tag.ByteSize(data.Type)==224||type=="hkpRagdollConstraintData"&&tag.ByteSize(data.Type)==416,"unsupported Pinwheel joint type/layout");
        SourceCollision.Tag.Item Atom(string name,int expectedType) {
            var n=tag.Nested(data.Type,data.At,"atoms",name);var atom=new SourceCollision.Tag.Item(n.Type,n.At,1);
            Need(tag.UShort(At(atom,"type"))==expectedType,"constraint atom type changed: "+name);return atom;
        }
        var transforms=Atom("transforms",2);var fa=Frame(At(transforms,"transformA"));var fb=Frame(At(transforms,"transformB"));
        var stab=Atom("setupStabilization",23);var stabilization=new Stabilization(Byte(stab,"enabled")!=0,Scalar(stab,"maxLinImpulse"),Scalar(stab,"maxAngImpulse"),Scalar(stab,"maxAngle"));
        var ball=Atom("ballSocket",5);var socket=new BallSocket(Byte(ball,"solvingMethod"),Byte(ball,"bodiesToNotify"),Byte(ball,"velocityStabilizationFactor"),Byte(ball,"enableLinearImpulseLimit")!=0,Scalar(ball,"breachImpulse"),Scalar(ball,"inertiaStabilizationFactor"));
        byte? free=null;Motors? control=null;Friction? friction=null;var limits=new List<AngularLimit>();
        if(hinge){var ang=Atom("2dAng",12);free=Byte(ang,"freeRotationAxis");Need(free<3,"invalid hinge free axis");}
        else {
            var motor=Atom("ragdollMotors",19);var refs=tag.Nested(motor.Type,motor.At,"motors");var addresses=Enumerable.Range(0,3).Select(i=>tag.Reference(refs.At+i*8)).Select(m=>m.Count==0?-1:m.At).ToArray();
            Need(addresses.All(at=>at<0||motors.ContainsKey(at)),"unresolved ragdoll motor");int target=At(motor,"target_bRca");
            var rotation=new Matrix4x4(Float(target),Float(target+4),Float(target+8),0,Float(target+16),Float(target+20),Float(target+24),0,Float(target+32),Float(target+36),Float(target+40),0,0,0,0,1);
            Need(Math.Abs(rotation.GetDeterminant()-1)<.002f,"invalid motor target rotation");control=new(Byte(motor,"isEnabled")!=0,Short(At(motor,"initializedOffset")),Short(At(motor,"previousTargetAnglesOffset")),rotation,addresses);
            var fr=Atom("angFriction",17);friction=new(Byte(fr,"isEnabled"),Byte(fr,"firstFrictionAxis"),Byte(fr,"numFrictionAxes"),Scalar(fr,"maxFrictionTorque"));
            foreach(string name in new[]{"twistLimit","coneLimit","planesLimit"}) {
                bool twist=name=="twistLimit";var limit=Atom(name,twist?15:16);
                limits.Add(new(tag.Name(limit.Type),Byte(limit,"isEnabled")!=0,Byte(limit,twist?"twistAxis":"twistAxisInA"),Byte(limit,twist?"refAxis":"refAxisInB"),twist?(byte)0:Byte(limit,"angleMeasurementMode"),twist?(ushort)0:tag.UShort(At(limit,"memOffsetToAngleOffset")),Scalar(limit,"minAngle"),Scalar(limit,"maxAngle"),Scalar(limit,"angularLimitsTauFactor"),Scalar(limit,"angularLimitsDampFactor")));
            }
        }
        return new(index,item.At,data.At,OptionalText(item,"name"),tag.UInt(At(item,"uid")),BitConverter.ToUInt64(bytes,At(item,"userData")),Byte(item,"priority"),Byte(item,"wantRuntime")!=0,type,a,b,fa,fb,stabilization,socket,free,control,friction,limits);
    }
    HollowDeathSource.Shape ReadShape(SourceCollision.Tag.Item item,Vector3 translation,int depth=0) {
        Need(depth<=1,"nested shape beyond authored Pinwheel profile");string type=tag.Name(item.Type);float radius=Scalar(item,"radius");Need(radius is >0 and <=64,"invalid convex radius");
        if(type=="hkpCapsuleShape")return new(type,tag.Vector(At(item,"vertexA")),tag.Vector(At(item,"vertexB")),radius,Vector3.Zero,translation);
        if(type=="hkpBoxShape")return new(type,Vector3.Zero,Vector3.Zero,radius,tag.Vector(At(item,"halfExtents")),translation);
        if(type=="hkpConvexTranslateShape")return ReadShape(Ref(item,"childShape","childShape"),translation+tag.Vector(At(item,"translation")),depth+1);
        throw new InvalidDataException("Unsupported source Pinwheel shape: "+type);
    }
    internal PinwheelPhysicsSource(byte[] physics) {
        bytes=physics.ToArray();PhysicsSHA256=Convert.ToHexStringLower(SHA256.HashData(bytes));Need(PhysicsSHA256==ExpectedSHA256,"unverified source c3320 physics");tag=new(bytes);
        var rig=tag.Items.Single(i=>i.Count==1&&tag.Name(i.Type)=="hkaRagdollInstance");
        var bodyItems=tag.PointerArray(rig,"rigidBodies");var constraintItems=tag.PointerArray(rig,"constraints");
        Need(bodyItems.Count==25&&constraintItems.Count==24&&bodyItems.Select(b=>b.At).Distinct().Count()==25,"body/constraint population");
        Ragdoll=ReadSkeleton(Ref(rig,"skeleton"));Need(Ragdoll.Names.Length==25,"ragdoll bone population");
        var map=tag.Array(rig,"boneToRigidBodyMap");BoneToBody=Enumerable.Range(0,map.Count).Select(i=>tag.Int(tag.Element(map,i).At)).ToArray();Need(BoneToBody.SequenceEqual(Enumerable.Range(0,25)),"bone-to-body identity changed");
        Mappings=tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type)=="hkaSkeletonMapper").Select(ReadMapping).ToArray();Need(Mappings.Count==2,"mapper population");
        var forward=Mappings.Single(m=>m.A.Names.Length==113&&m.B.SourceAt==Ragdoll.SourceAt);Animation=forward.A;
        Need(Mappings.Count(m=>m.A.SourceAt==Ragdoll.SourceAt&&m.B.SourceAt==Animation.SourceAt)==1,"missing reverse mapper");
        Bodies=bodyItems.Select((item,index)=> {
            Need(tag.Name(item.Type)=="hkpRigidBody","invalid body object");string name=Text(item,"name");Need(name==Ragdoll.Names[index],"body/ragdoll named ownership");
            int state=At(item,"motion","motionState");var rest=Frame(state);var swept=tag.Nested(item.Type,item.At,"motion","motionState","sweptTransform");Need(tag.ByteSize(swept.Type)==80,"swept transform layout");
            var qv=Vector4(swept.At+48);var q=new Quaternion(qv.X,qv.Y,qv.Z,qv.W);Need(Math.Abs(q.LengthSquared()-1)<.001f,"nonunit rest orientation");
            Need(Error(rest,Matrix4x4.CreateFromQuaternion(q)*Matrix4x4.CreateTranslation(rest.Translation))<.002f,"rest transform/orientation mismatch");
            var mass=Vector4(At(item,"motion","inertiaAndMassInv"));Need(mass.X>0&&mass.Y>0&&mass.Z>0&&mass.W>0,"invalid inverse mass/inertia");
            float friction=Scalar(item,"material","friction"),restitution=Scalar(item,"material","restitution");Need(friction is >=0 and <=1000&&restitution is >=0 and <=1,"surface coefficients");
            return new Body(index,name,ReadShape(Ref(item,"collidable","shape"),Vector3.Zero),rest,mass,Vector4(swept.At+16),q,Vector4(At(item,"motion","linearVelocity")),Vector4(At(item,"motion","angularVelocity")),friction,restitution,Half(At(item,"motion","motionState","linearDamping")),Half(At(item,"motion","motionState","angularDamping")),Half(At(item,"motion","gravityFactor")),Half(At(item,"motion","motionState","timeFactor")),Byte(item,"motion","type"),tag.UInt(At(item,"collidable","broadPhaseHandle","collisionFilterInfo")),BitConverter.ToUInt64(bytes,At(item,"userData")));
        }).ToArray();
        Need(Bodies.All(b=>b.Shape.Type=="hkpCapsuleShape"),"authored shape population");
        PositionMotors=tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type)=="hkpPositionConstraintMotor").Select(ReadMotor).ToArray();Need(PositionMotors.Count==0,"motor population");
        var motors=PositionMotors.ToDictionary(m=>m.SourceAt);var indices=bodyItems.Select((b,i)=>(b.At,i)).ToDictionary(p=>p.At,p=>p.i);
        Constraints=constraintItems.Select((c,i)=>ReadConstraint(c,i,indices,motors)).ToArray();Need(Constraints.All(c=>c.Type=="hkpHingeConstraintData"),"joint family population");
        var reached=new HashSet<int>{0};for(int n=0;n<25;n++)foreach(var c in Constraints)if(reached.Contains(c.A)||reached.Contains(c.B)){reached.Add(c.A);reached.Add(c.B);}Need(reached.Count==25,"disconnected constraint topology");
    }
    internal static PinwheelPhysicsSource Load(Func<string,byte[]?> read) {
        var bank=BND3.Read(read("chr/c3320.chrbnd.dcx")??throw new FileNotFoundException("Source c3320 archive unavailable"));
        Need(bank.Files.Count(f=>Leaf(f.Name)=="c3320.hkxpwv")==1,"source PWV missing");
        return new(bank.Files.Single(f=>Leaf(f.Name)=="c3320.hkx").Bytes.ToArray());
    }
}
