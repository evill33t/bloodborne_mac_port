// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

// Source-only Hydra physics profile. Nothing here emits hknp objects or native
// PWV bytes: DS1R's body/animation/part record strides are not Bloodborne's.
internal sealed class HydraPhysicsSource {
    internal sealed record Skeleton(int SourceAt,string Name,string[] Names,short[] Parents,bool[] Locked,Matrix4x4[] Local);
    internal sealed record Simple(short A,short B,Matrix4x4 Transform);
    internal sealed record Mapping(Skeleton A,Skeleton B,IReadOnlyList<Simple> Simple,short[] Unmapped,Matrix4x4? ExtractedMotion);
    internal sealed record Capsule(Vector3 A,Vector3 B,float Radius);
    internal sealed record Body(int Index,int SourceAt,string Name,int AnimationBone,Capsule Capsule,Matrix4x4 Rest,
        Vector4 InertiaMassInverse,Vector4 CenterOfMass,Quaternion Orientation,Vector4 LinearVelocity,Vector4 AngularVelocity,
        float Friction,float Restitution,float LinearDamping,float AngularDamping,float Gravity,float TimeFactor,byte MotionType,uint CollisionFilter,ulong UserData);
    internal sealed record Stabilization(bool Enabled,float MaxLinearImpulse,float MaxAngularImpulse,float MaxAngle);
    internal sealed record BallSocket(byte SolvingMethod,byte BodiesToNotify,byte VelocityStabilizationFactor,bool LimitLinearImpulse,float BreachImpulse,float InertiaStabilizationFactor);
    internal sealed record AngularLimit(string Type,bool Enabled,byte AxisA,byte AxisB,byte MeasurementMode,ushort AngleOffset,float Min,float Max,float Tau,float Damping);
    internal sealed record PositionMotor(int SourceAt,byte Type,float MinForce,float MaxForce,float Tau,float Damping,float ProportionalRecoveryVelocity,float ConstantRecoveryVelocity);
    internal sealed record Motors(bool Enabled,short InitializedOffset,short PreviousAnglesOffset,Matrix4x4 Target,IReadOnlyList<int> SourceMotorAddresses);
    internal sealed record Friction(byte Enabled,byte FirstAxis,byte Axes,float MaxTorque);
    internal sealed record Constraint(int Index,int SourceAt,int DataSourceAt,string? Name,uint UID,ulong UserData,byte Priority,bool WantRuntime,string Type,int A,int B,Matrix4x4 FrameA,Matrix4x4 FrameB,
        Stabilization Stabilization,BallSocket BallSocket,byte? FreeRotationAxis,Motors? Motors,Friction? Friction,IReadOnlyList<AngularLimit> Limits);
    // The first two words have no proven semantic name. Keep them explicit;
    // only Group is cross-checked against source event part-group ownership.
    internal sealed record PartRecord(short UnknownA,short UnknownB,int Group);
    internal sealed record Part(int Slot,char Branch,int SourceBodyEntity,int SourceDetachedEntity,int SourcePartID,int Group,int HP,
        int DisplayMask,int CollisionMask,int SeverSocket,int SuppressionEffect,IReadOnlyList<int> BodyIndices);
    internal const string Model="c3530";
    internal readonly string PhysicsSHA256,PwvSHA256;
    internal readonly Skeleton Animation,Ragdoll;
    internal readonly IReadOnlyList<Body> Bodies;
    internal readonly IReadOnlyList<Constraint> Constraints;
    internal readonly IReadOnlyList<Mapping> Mappings;
    internal readonly IReadOnlyList<PositionMotor> PositionMotors;
    internal readonly IReadOnlyList<Part> Parts;
    internal readonly IReadOnlyList<uint> BodyFlags;
    internal readonly IReadOnlyList<PartRecord> AnimationRecords,RagdollRecords;
    internal readonly int[] BoneToBody;
    readonly byte[] bytes;
    readonly SourceCollision.Tag tag;
    readonly Dictionary<int,Skeleton> skeletons=[];
    static void Need(bool condition,string why){if(!condition)throw new InvalidDataException("Source Hydra physics: "+why);}
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
        Need(bones.Count is 97 or 246&&parents.Count==bones.Count&&pose.Count==bones.Count&&tag.Array(item,"partitions").Count==0,"unsupported skeleton population/partitions");
        var names=new string[bones.Count];var indices=new short[bones.Count];var locked=new bool[bones.Count];var local=new Matrix4x4[bones.Count];
        for(int i=0;i<bones.Count;i++){var b=tag.Element(bones,i);names[i]=Text(b,"name");indices[i]=Short(tag.Element(parents,i).At);locked[i]=Byte(b,"lockTranslation")!=0;local[i]=tag.QsTransform(tag.Element(pose,i).At);Need(indices[i]>=-1&&indices[i]<i,"skeleton is not parent-first");}
        Need(indices.Count(i=>i==-1)==1&&names.Distinct(StringComparer.Ordinal).Count()==names.Length,"ambiguous skeleton roots/names");
        var result=new Skeleton(item.At,Text(item,"name"),names,indices,locked,local);skeletons.Add(item.At,result);return result;
    }
    Mapping ReadMapping(SourceCollision.Tag.Item item) {
        var nested=tag.Nested(item.Type,item.At,"mapping");var m=new SourceCollision.Tag.Item(nested.Type,nested.At,1);var a=ReadSkeleton(Ref(m,"skeletonA"));var b=ReadSkeleton(Ref(m,"skeletonB"));
        Need(Byte(m,"keepUnmappedLocal")==1&&tag.Int(At(m,"mappingType"))==0,"unsupported mapper policy");
        foreach(string empty in new[]{"partitionMap","simpleMappingPartitionRanges","chainMappingPartitionRanges","chainMappings"})Need(tag.Array(m,empty).Count==0,"partitioned/chained Hydra mapping");
        int extracted=At(m,"extractedMotionMapping");
        // Hydra stores an all-zero (including quaternion/scale) unused value,
        // unlike the identity QS used by the hollow family. Keep it absent;
        // normalizing it to identity here would invent source motion data.
        Matrix4x4? extractedMotion=bytes.AsSpan(extracted,48).IndexOfAnyExcept((byte)0)<0?null:tag.QsTransform(extracted);
        Need(extractedMotion is null||extractedMotion==Matrix4x4.Identity,"nonidentity extracted-motion mapping");
        var simple=tag.Array(m,"simpleMappings");var unmapped=tag.Array(m,"unmappedBones");Need(simple.Count==97&&tag.ByteSize(simple.Type)==64,"mapper simple population/layout");
        var maps=Enumerable.Range(0,simple.Count).Select(i=>{int at=tag.Element(simple,i).At;return new Simple(Short(at),Short(at+2),tag.QsTransform(at+16));}).ToArray();
        var unmap=Enumerable.Range(0,unmapped.Count).Select(i=>Short(tag.Element(unmapped,i).At)).ToArray();
        Need(maps.All(s=>s.A>=0&&s.A<a.Names.Length&&s.B>=0&&s.B<b.Names.Length)&&maps.Select(s=>s.A).Distinct().Count()==97&&maps.Select(s=>s.B).Distinct().Count()==97,"invalid/duplicate mapper ownership");
        Need(unmap.All(i=>i>=0&&i<b.Names.Length)&&unmap.Distinct().Count()==unmap.Length&&!unmap.Intersect(maps.Select(s=>s.B)).Any()&&unmap.Length+maps.Length==b.Names.Length,"mapper destination coverage");
        return new(a,b,maps,unmap,extractedMotion);
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
        Need(hinge&&tag.ByteSize(data.Type)==224||type=="hkpRagdollConstraintData"&&tag.ByteSize(data.Type)==416,"unsupported Hydra joint type/layout");
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
    // Pure resource reader, exposed separately so malformed member data can be
    // rejected without constructing or changing any map/physics output.
    internal HydraPhysicsSource(byte[] physics,byte[] pwv) {
        Need(physics.Length is >32 and <=16*1024*1024,"physics read budget");Need(pwv.Length==3164,"source PWV size/layout");
        bytes=physics.ToArray();tag=new(bytes);PhysicsSHA256=Convert.ToHexStringLower(SHA256.HashData(bytes));PwvSHA256=Convert.ToHexStringLower(SHA256.HashData(pwv));
        var rigs=tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type)=="hkaRagdollInstance").ToArray();Need(rigs.Length==1,"ragdoll population");var rig=rigs[0];
        var bodyItems=tag.PointerArray(rig,"rigidBodies");var constraintItems=tag.PointerArray(rig,"constraints");Need(bodyItems.Count==97&&constraintItems.Count==96&&bodyItems.Select(b=>b.At).Distinct().Count()==97,"body/constraint population");
        Ragdoll=ReadSkeleton(Ref(rig,"skeleton"));Need(Ragdoll.Names.Length==97,"ragdoll bone population");
        var map=tag.Array(rig,"boneToRigidBodyMap");BoneToBody=Enumerable.Range(0,map.Count).Select(i=>tag.Int(tag.Element(map,i).At)).ToArray();Need(BoneToBody.SequenceEqual(Enumerable.Range(0,97)),"bone-to-body identity changed");
        Mappings=tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type)=="hkaSkeletonMapper").Select(ReadMapping).ToArray();Need(Mappings.Count==2,"mapper population");
        var forward=Mappings.Where(m=>m.A.Names.Length==246&&m.B.SourceAt==Ragdoll.SourceAt).ToArray();Need(forward.Length==1,"missing animation-to-ragdoll mapper");Animation=forward[0].A;
        Need(Mappings.Count(m=>m.A.SourceAt==Ragdoll.SourceAt&&m.B.SourceAt==Animation.SourceAt)==1,"missing reverse mapper");
        var animationBones=forward[0].Simple.ToDictionary(m=>(int)m.B,m=>(int)m.A);
        Bodies=bodyItems.Select((item,index)=> {
            Need(tag.Name(item.Type)=="hkpRigidBody","invalid rigid body object");string name=Text(item,"name");Need(name==Ragdoll.Names[index],"body/ragdoll named ownership changed");
            var shape=Ref(item,"collidable","shape");Need(tag.Name(shape.Type)=="hkpCapsuleShape","Hydra body is not a capsule");float radius=Scalar(shape,"radius");Need(radius is >0 and <=64,"invalid capsule radius");
            var capsule=new Capsule(tag.Vector(At(shape,"vertexA")),tag.Vector(At(shape,"vertexB")),radius);Need(Vector3.DistanceSquared(capsule.A,capsule.B)>.000001f,"degenerate capsule");
            int state=At(item,"motion","motionState");var rest=Frame(state);var swept=tag.Nested(item.Type,item.At,"motion","motionState","sweptTransform");Need(tag.ByteSize(swept.Type)==80,"swept-transform layout");
            var qv=Vector4(swept.At+48);var q=new Quaternion(qv.X,qv.Y,qv.Z,qv.W);Need(Math.Abs(q.LengthSquared()-1)<.001f,"nonunit rest orientation");
            Need(Error(rest,Matrix4x4.CreateFromQuaternion(q)*Matrix4x4.CreateTranslation(rest.Translation))<.002f,"rest transform/orientation mismatch");
            var mass=Vector4(At(item,"motion","inertiaAndMassInv"));Need(mass.X>0&&mass.Y>0&&mass.Z>0&&mass.W>0,"invalid inverse mass/inertia");
            float friction=Scalar(item,"material","friction"),restitution=Scalar(item,"material","restitution");Need(friction is >=0 and <=1000&&restitution is >=0 and <=1,"surface coefficients");
            return new Body(index,item.At,name,animationBones[index],capsule,rest,mass,Vector4(swept.At+16),q,Vector4(At(item,"motion","linearVelocity")),Vector4(At(item,"motion","angularVelocity")),friction,restitution,Half(At(item,"motion","motionState","linearDamping")),Half(At(item,"motion","motionState","angularDamping")),Half(At(item,"motion","gravityFactor")),Half(At(item,"motion","motionState","timeFactor")),Byte(item,"motion","type"),tag.UInt(At(item,"collidable","broadPhaseHandle","collisionFilterInfo")),BitConverter.ToUInt64(bytes,At(item,"userData")));
        }).ToArray();
        PositionMotors=tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type)=="hkpPositionConstraintMotor").Select(ReadMotor).ToArray();Need(PositionMotors.Count==1,"motor population");
        var motors=PositionMotors.ToDictionary(m=>m.SourceAt);var indices=bodyItems.Select((b,i)=>(b.At,i)).ToDictionary(p=>p.At,p=>p.i);
        Constraints=constraintItems.Select((c,i)=>ReadConstraint(c,i,indices,motors)).ToArray();Need(Constraints.Count(c=>c.Type=="hkpHingeConstraintData")==95&&Constraints.Count(c=>c.Type=="hkpRagdollConstraintData")==1,"joint family population");
        var reached=new HashSet<int>{0};for(int n=0;n<97;n++)foreach(var c in Constraints)if(reached.Contains(c.A)||reached.Contains(c.B)){reached.Add(c.A);reached.Add(c.B);}Need(reached.Count==97,"disconnected constraint topology");
        Need(BitConverter.ToUInt32(pwv,0)==1&&BitConverter.ToUInt16(pwv,4)==0&&BitConverter.ToUInt16(pwv,6)==97&&BitConverter.ToUInt16(pwv,8)==246&&BitConverter.ToUInt16(pwv,10)==97&&pwv.AsSpan(12,20).IndexOfAnyExcept((byte)0)<0,"PWV header/version/population");
        BodyFlags=Enumerable.Range(0,97).Select(i=>BitConverter.ToUInt32(pwv,32+i*4)).ToArray();
        PartRecord Record(int at)=>new(BitConverter.ToInt16(pwv,at),BitConverter.ToInt16(pwv,at+2),BitConverter.ToInt32(pwv,at+4));
        AnimationRecords=Enumerable.Range(0,246).Select(i=>Record(420+i*8)).ToArray();RagdollRecords=Enumerable.Range(0,97).Select(i=>Record(2388+i*8)).ToArray();
        Need(BodyFlags.All(f=>f==65535)&&AnimationRecords.All(r=>r==new PartRecord(-1,0,0)),"unsupported PWV body/animation flags");
        var parts=new List<Part>();int[] groups=[1,2,3,4,5,6,8];string branches="gfedcba";
        for(int slot=0;slot<7;slot++) {
            int group=groups[slot];char branch=branches[slot];var owned=Enumerable.Range(0,97).Where(i=>RagdollRecords[i].Group==group).ToArray();
            string[] expected=branch=='a'?["Ragdoll_Neck_a_01","Ragdoll_Neck_a_03","Ragdoll_Neck_a_04","Ragdoll_Head_a01"]:
                [$"Ragdoll_Neck_{branch}_03",$"Ragdoll_Neck_{branch}_04",$"Ragdoll_Neck_{branch}_05",$"Ragdoll_Head_{branch}01"];
            Need(owned.Length==4&&owned.Select(i=>Bodies[i].Name).Order(StringComparer.Ordinal).SequenceEqual(expected.Order(StringComparer.Ordinal))&&owned.All(i=>RagdollRecords[i].UnknownA==1400+slot&&RagdollRecords[i].UnknownB==-1),"head part/named capsule ownership changed");
            parts.Add(new(slot,branch,1200010,1200011+slot,3530+slot,group,176,slot,slot+1,91+slot,5430+slot,owned));
        }
        Need(RagdollRecords.Select(r=>r.Group).All(g=>g==0||groups.Contains(g))&&RagdollRecords.Where(r=>r.Group==0).All(r=>r==new PartRecord(-1,-1,0)),"unsupported unowned PWV record");Parts=parts;
    }
    // Canonical source IDs above are validated against the actual event table
    // on load; they are not native entity IDs or an alternate spawn plan.
    internal static HydraPhysicsSource Load(Func<string,byte[]?> read) {
        byte[] Read(string path){var bytes=read(path)??throw new FileNotFoundException("Source Hydra resource unavailable",path);Need(bytes.Length is >0 and <=128*1024*1024,"archive read budget");return bytes;}
        var bank=BND3.Read(Read("chr/c3530.chrbnd.dcx"));var physics=bank.Files.Single(f=>Leaf(f.Name)=="c3530.hkx").Bytes.ToArray();var pwv=bank.Files.Single(f=>Leaf(f.Name)=="c3530.hkxpwv").Bytes.ToArray();var result=new HydraPhysicsSource(physics,pwv);
        var events=EMEVD.Read(Read("event/m12_00_00_00.emevd.dcx"));Need(events.Format==EMEVD.Game.DarkSouls1,"source event format");
        var template=events.Events.Single(e=>e.ID==11205300);var starts=events.Events.Single(e=>e.ID==0).Instructions.Where(i=>i.Bank==2000&&i.ID==0&&i.ArgData.Length>=8&&BitConverter.ToInt32(i.ArgData,4)==11205300).ToArray();Need(starts.Length==7,"source sever initializer population");
        foreach(var init in starts) {
            int slot=BitConverter.ToInt32(init.ArgData,0);Need(slot is >=0 and <7,"source sever slot");var part=result.Parts[slot];
            var instructions=template.Instructions.Select(i=>new EMEVD.Instruction(i.Bank,i.ID,i.ArgData.ToArray())).ToArray();
            foreach(var p in template.Parameters){Need(p.InstructionIndex>=0&&p.InstructionIndex<instructions.Length&&p.ByteCount>0&&p.SourceStartByte>=0&&p.SourceStartByte<=init.ArgData.Length-8-p.ByteCount,"source sever parameter bounds");var target=instructions[p.InstructionIndex].ArgData;Need(p.TargetStartByte>=0&&p.TargetStartByte<=target.Length-p.ByteCount,"source sever target bounds");init.ArgData.AsSpan(checked(8+(int)p.SourceStartByte),checked((int)p.ByteCount)).CopyTo(target.AsSpan(checked((int)p.TargetStartByte)));}
            var create=instructions.Single(i=>i.Bank==2004&&i.ID==22).ArgData;
            Need(create.Length==24&&BitConverter.ToInt32(create,0)==part.SourceBodyEntity&&BitConverter.ToUInt16(create,4)==part.SourcePartID&&BitConverter.ToUInt16(create,6)==part.Group&&BitConverter.ToInt32(create,8)==part.HP&&BitConverter.ToSingle(create,12)==1&&BitConverter.ToSingle(create,16)==1,"source sever part identity/damage factors changed");
            var displays=instructions.Where(i=>i.Bank==2004&&i.ID==26).ToArray();var collisions=instructions.Where(i=>i.Bank==2004&&i.ID==27).ToArray();var effects=instructions.Where(i=>i.Bank==2004&&i.ID==8).ToArray();
            Need(instructions.Any(i=>i.Bank==2004&&i.ID==5&&BitConverter.ToInt32(i.ArgData,0)==part.SourceDetachedEntity)&&displays.Length==2&&displays.All(i=>BitConverter.ToInt32(i.ArgData,0)==part.SourceBodyEntity&&i.ArgData[4]==part.DisplayMask)&&collisions.Length==2&&collisions.All(i=>BitConverter.ToInt32(i.ArgData,0)==part.SourceBodyEntity&&i.ArgData[4]==part.CollisionMask)&&effects.Length==2&&effects.All(i=>BitConverter.ToInt32(i.ArgData,0)==part.SourceBodyEntity&&BitConverter.ToInt32(i.ArgData,4)==part.SuppressionEffect),"source sever actor/mask/effect ownership changed");
            Need(BitConverter.ToInt32(init.ArgData,20)==part.SeverSocket,"source sever socket changed");
        }
        Need(starts.Select(i=>BitConverter.ToInt32(i.ArgData,0)).Distinct().Count()==7,"duplicate source sever slots");return result;
    }
}
