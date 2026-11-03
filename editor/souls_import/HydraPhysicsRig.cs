// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using P=HydraPhysicsPacking;

// The source Hydra's own 97 capsules and joints drive its appended visible rig.
// No native humanoid hurtbox, anatomical match or clipped neck proxy is retained.
internal static class HydraPhysicsRig {
    internal sealed record Report(string SourcePhysicsSHA256,string NativePhysicsSHA256,int Bodies,int Hinges,int RagdollJoints,
        int AnimationBones,int RagdollBones,int ForwardMappings,int BackwardMappings,int UnmappedControllerBones,int UnmappedSourceBones,
        IReadOnlyList<string> Limitations);
    internal sealed record Result(byte[] Physics,Report Report);
    internal const string NativePhysicsSHA256="2e255b50d1cff21955676a64a42c1fdb642ec0d275e233f5ef81faf85cfcbc01";
    internal static void ValidatePlan(HydraPhysicsSource source,NativeRigAppend.Plan plan) {
        P.Need(plan.Original.Names.Length==110&&plan.Added.Count==246&&plan.Count==356,"expected c2730 + complete source Hydra rig");
        P.Need(plan.Added.Select(b=>b.Name).SequenceEqual(source.Animation.Names.Select(n=>"DS1_"+n)),"source physics/visible animation bone order differs");
        var full=NativeAnimation.Skeleton(plan.Skeleton);
        P.Need(full.Names.Take(110).SequenceEqual(plan.Original.Names)&&full.Names.Skip(110).SequenceEqual(plan.Added.Select(b=>b.Name)),"composite skeleton identity differs");
        var sourceWorld=NativeAnimation.Worlds(source.Animation.Local,source.Animation.Parents.Select(p=>(int)p).ToArray());
        for(int i=0;i<246;i++)P.Need(Error(full.World[110+i],sourceWorld[i])<.001f,"visible/reference physics bind disagreement");
    }
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{Vector3.Zero,Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Max(v=>Vector3.Distance(Vector3.Transform(v,a),Vector3.Transform(v,b)));
    internal static Result Convert(byte[] nativePhysics,HydraPhysicsSource source,NativeRigAppend.Plan plan) {
        ValidatePlan(source,plan);string nativeHash=System.Convert.ToHexStringLower(SHA256.HashData(nativePhysics));
        P.Need(nativeHash==NativePhysicsSHA256,"unverified native c2730 physics template");
        var native=new HollowDeathRig.Pack(nativePhysics);int system=native.One("hknpRagdollData");
        var skeletons=native.Objects.Where(o=>o.Name=="hkaSkeleton").Select(o=>(o.At,Info:native.Skeleton(o.At))).ToArray();
        var animation=skeletons.Single(s=>s.Info.Names.Length==110);P.Need(animation.Info.Names.SequenceEqual(plan.Original.Names),"physics/controller donors differ");
        P.Need(skeletons.Length==2&&skeletons.Any(s=>s.Info.Names.Length==60),"native embedded skeleton contract");
        var bodyTemplate=native.Array(system+64,96,128);var materialTemplate=native.Array(system+16,80,128);
        var propertiesTemplate=native.Array(system+32,64,128);var motionTemplate=native.Array(system+48,112,128);
        P.Need(bodyTemplate.Count==60&&materialTemplate.Count==60&&propertiesTemplate.Count==60&&motionTemplate.Count==60,"native hknp system arrays changed");
        P.Need(!native.Pointers.ContainsKey(materialTemplate.At+48)&&!native.Pointers.ContainsKey(bodyTemplate.At+88),"unsupported native dynamic surface/local frame");
        var capsule=native.Objects.First(o=>o.Name=="hknpCapsuleShape");var capsuleTemplate=HollowDeathPacking.Shape.Template(native,capsule.At);
        var fullInfo=NativeAnimation.Skeleton(plan.Skeleton);var fullPack=new NativeAnimation.Pack(plan.Skeleton);
        var fullBones=fullPack.Array(fullPack.Object("hkaSkeleton")+40,16,512);
        var full=new P.hkaSkeleton(new(fullInfo.Name,fullInfo.Names,fullInfo.Parents.Select(i=>checked((short)i)).ToArray(),
            Enumerable.Range(0,356).Select(i=>plan.Skeleton[fullPack.Data+fullBones.At+i*16+8]!=0).ToArray(),fullInfo.Local));
        var rag=new P.hkaSkeleton(new(source.Ragdoll.Name,source.Ragdoll.Names,source.Ragdoll.Parents,source.Ragdoll.Locked,source.Ragdoll.Local));
        var forward=source.Mappings.Single(m=>m.A.Names.Length==246);var backward=source.Mappings.Single(m=>m.B.Names.Length==246);
        var f=forward.Simple.Select(m=>new P.Simple(checked((short)(110+m.A)),m.B,m.Transform)).ToArray();
        var b=backward.Simple.Select(m=>new P.Simple(m.A,checked((short)(110+m.B)),m.Transform)).ToArray();
        var backUnmapped=Enumerable.Range(0,110).Select(i=>checked((short)i)).Concat(backward.Unmapped.Select(i=>checked((short)(110+i)))).ToArray();
        P.Need(f.Length==97&&b.Length==97&&forward.Unmapped.Length==0&&backUnmapped.Length==259,"mapper coverage changed");
        var mapForward=new P.hkaSkeletonMapper(full,rag,f,forward.Unmapped,forward.ExtractedMotion);
        var mapBackward=new P.hkaSkeletonMapper(rag,full,b,backUnmapped,backward.ExtractedMotion);
        var shapes=new List<HKX2.hknpShape>();var materials=new List<HKX2.hknpMaterial>();var properties=new List<HKX2.hknpMotionProperties>();
        var motions=new List<HKX2.hknpMotionCinfo>();var bodies=new List<HKX2.hknpBodyCinfo>();
        foreach(var body in source.Bodies) {
            int i=body.Index;P.Need(body.MotionType is 2 or 3&&body.CollisionFilter==0,"unsupported source motion/filter policy");
            var shape=HollowDeathPacking.Shape.Convert(capsuleTemplate,new("hkpCapsuleShape",body.Capsule.A,body.Capsule.B,body.Capsule.Radius,Vector3.Zero,Vector3.Zero));shapes.Add(shape);
            var mat=native.Raw(materialTemplate.At,80);P.Half(mat,18,body.Friction);P.Half(mat,20,body.Friction);P.Half(mat,22,body.Restitution);
            materials.Add(new HollowDeathPacking.Material(mat,native.Text(materialTemplate.At)));
            var prop=native.Raw(propertiesTemplate.At,64);P.F(prop,8,body.Gravity);P.F(prop,12,body.TimeFactor);P.F(prop,24,body.LinearDamping);P.F(prop,28,body.AngularDamping);
            properties.Add(new HollowDeathPacking.Properties(prop));
            var motion=native.Raw(motionTemplate.At,112);P.U16(motion,0,checked((ushort)i));P.F(motion,4,body.InertiaMassInverse.W);
            P.V(motion,32,new(body.InertiaMassInverse.X,body.InertiaMassInverse.Y,body.InertiaMassInverse.Z,1));
            P.V(motion,48,new(body.CenterOfMass.X,body.CenterOfMass.Y,body.CenterOfMass.Z,BitConverter.ToSingle(motion,60)));
            P.V(motion,64,new(body.Orientation.X,body.Orientation.Y,body.Orientation.Z,body.Orientation.W));P.V(motion,80,body.LinearVelocity);P.V(motion,96,body.AngularVelocity);
            motions.Add(new HollowDeathPacking.Motion(motion));
            var info=native.Raw(bodyTemplate.At,96);P.I32(info,12,i);P.U16(info,18,checked((ushort)i));P.I32(info,20,unchecked((int)body.CollisionFilter));
            BitConverter.GetBytes(body.UserData).CopyTo(info,40);P.V(info,48,new(body.Rest.Translation,1));
            P.Need(Matrix4x4.Decompose(body.Rest,out var scale,out var rotation,out _)&&Vector3.Distance(scale,Vector3.One)<.001f,"nonrigid source body frame");
            P.V(info,64,new(rotation.X,rotation.Y,rotation.Z,rotation.W));bodies.Add(new HollowDeathPacking.Body(info,body.Name,shape));
        }
        var motors=source.PositionMotors.ToDictionary(m=>m.SourceAt,m=>(HKX2.hkpConstraintMotor)new HKX2.hkpPositionConstraintMotor {
            m_type=(HKX2.MotorType)m.Type,m_minForce=m.MinForce,m_maxForce=m.MaxForce,m_tau=m.Tau,m_damping=m.Damping,
            m_proportionalRecoveryVelocity=m.ProportionalRecoveryVelocity,m_constantRecoveryVelocity=m.ConstantRecoveryVelocity});
        var constraints=source.Constraints.Select(c=>new HKX2.hknpConstraintCinfo {m_bodyA=(uint)c.A,m_bodyB=(uint)c.B,m_flags=0,
            m_constraintData=c.Type=="hkpHingeConstraintData"?new P.hkpHingeConstraintData(P.ConstraintBytes(c)):
                new P.hkpRagdollConstraintData(P.ConstraintBytes(c),c.Motors!.SourceMotorAddresses.Select(a=>a<0?null:motors[a]).ToArray())}).ToList();
        var data=new HKX2.hknpRagdollData {m_materials=materials,m_motionProperties=properties,m_motionCinfos=motions,m_bodyCinfos=bodies,m_constraintCinfos=constraints,
            m_referencedObjects=shapes.Cast<HKX2.hkReferencedObject>().ToList(),m_name="DS1_Hydra",m_skeleton=rag,m_boneToBodyMap=source.BoneToBody.ToList()};
        var scene=new HKX2.hknpPhysicsSceneData{m_systemDatas=[data],m_worldCinfo=null};
        var container=new HKX2.hkaAnimationContainer{m_skeletons=[full,rag],m_animations=[],m_bindings=[],m_attachments=[],m_skins=[]};
        var root=new HKX2.hkRootLevelContainer{m_namedVariants=[
            new(){m_name="Merged Animation Container",m_className="hkaAnimationContainer",m_variant=container},
            new(){m_name="Physics Scene Data",m_className="hknpPhysicsSceneData",m_variant=scene},
            new(){m_name="Physics Ragdoll",m_className="hknpRagdollData",m_variant=data},
            new(){m_name="SkeletonMapper",m_className="hkaSkeletonMapper",m_variant=mapForward},
            new(){m_name="SkeletonMapper",m_className="hkaSkeletonMapper",m_variant=mapBackward}]};
        using var stream=new MemoryStream();new HKX2.PackFileSerializer().Serialize(root,new BinaryWriterEx(false,stream));var output=stream.ToArray();output[18]=1;
        _=new HollowDeathRig.Pack(output);
        return new(output,new(source.PhysicsSHA256,nativeHash,97,95,1,356,97,f.Length,b.Length,110,149,[
            "All source capsule geometry, rest frames, mass/inertia, velocities and joint topology are translated; all controller-prefix bones stay unmapped with keepUnmappedLocal=true.",
            "Native c2730 material/solver/deactivation/speed/quality policy and motion massFactor remain the native template's; source-only runtime constraint offsets/instance metadata have no equivalent in this serialized native system.",
            "Native collision filter records preserve the source zero policy. PWV/owned RagdollParam rows and encounter events are separate required outputs; serialization fixtures do not prove solver or gameplay behavior."]));
    }
}
