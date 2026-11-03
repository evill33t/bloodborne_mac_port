// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using P=HydraPhysicsPacking;

// The source golden golem's own18 capsules and17hinges drive its visible rig.
// No native humanoid hurtbox, anatomical match or clipped neck proxy is retained.
internal static class GoldenGolemPhysics {
    internal sealed record Report(string SourcePhysicsSHA256,string NativePhysicsSHA256,int Bodies,int Hinges,int RagdollJoints,
        int AnimationBones,int RagdollBones,int ForwardMappings,int BackwardMappings,int UnmappedControllerBones,int UnmappedSourceBones,
        IReadOnlyList<string> Limitations);
    internal sealed record Result(byte[] Physics,Report Report);
    internal const string NativePhysicsSHA256="2e255b50d1cff21955676a64a42c1fdb642ec0d275e233f5ef81faf85cfcbc01";
    internal static void ValidatePlan(GoldenGolemPhysicsSource source,NativeRigAppend.Plan plan) {
        P.Need(plan.Original.Names.Length==110&&plan.Added.Count==29&&plan.Count==139,"expected c2730 + complete source golden golem rig");
        P.Need(plan.Added.Select(b=>b.Name).SequenceEqual(source.Animation.Names.Select(n=>"DS1_"+n)),"source physics/visible animation bone order differs");
        var full=NativeAnimation.Skeleton(plan.Skeleton,allowMirroredScale:true);
        P.Need(full.Names.Take(110).SequenceEqual(plan.Original.Names)&&full.Names.Skip(110).SequenceEqual(plan.Added.Select(b=>b.Name)),"composite skeleton identity differs");
        var sourceWorld=NativeAnimation.Worlds(source.Animation.Local,source.Animation.Parents.Select(p=>(int)p).ToArray());
        for(int i=0;i<29;i++)P.Need(Error(full.World[110+i],sourceWorld[i]*Matrix4x4.CreateTranslation(0,0,-.3f))<.001f,"authored c2711 physics reference has exactly+.3Z versus animation reference: "+i+"/"+full.Names[110+i]);
    }
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{Vector3.Zero,Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Max(v=>Vector3.Distance(Vector3.Transform(v,a),Vector3.Transform(v,b)));
    internal static Result Convert(byte[] nativePhysics,GoldenGolemPhysicsSource source,NativeRigAppend.Plan plan) {
        ValidatePlan(source,plan);string nativeHash=System.Convert.ToHexStringLower(SHA256.HashData(nativePhysics));
        P.Need(nativeHash==NativePhysicsSHA256,"unverified native c2730 physics template");
        var native=new HollowDeathRig.Pack(nativePhysics);int system=native.One("hknpRagdollData");
        var skeletons=native.Objects.Where(o=>o.Name=="hkaSkeleton").Select(o=>(o.At,Info:native.Skeleton(o.At))).ToArray();
        var animation=skeletons.Single(s=>s.Info.Names.Length==110);P.Need(animation.Info.Names.SequenceEqual(plan.Original.Names),"physics/controller donors differ");
        P.Need(skeletons.Length==2&&skeletons.Any(s=>s.Info.Names.Length==60),"native embedded skeleton contract");
        var bodyTemplate=native.Array(system+64,96,128);var materialTemplate=native.Array(system+16,80,128);
        var propertiesTemplate=native.Array(system+32,64,128);var motionTemplate=native.Array(system+48,112,128);
        P.Need(bodyTemplate.Count==60&&materialTemplate.Count==60&&propertiesTemplate.Count==60&&motionTemplate.Count==60,"native hknp system arrays changed");
        // Both shipped profiles use the same group/subsystem filter pattern;
        // preserve this source chain's self-collision exclusions verbatim.
        P.Need(source.Bodies.All(b=>(b.CollisionFilter&31)==0&&b.CollisionFilter>>16==1),"source group-filter policy changed");
        P.Need(!native.Pointers.ContainsKey(materialTemplate.At+48)&&!native.Pointers.ContainsKey(bodyTemplate.At+88),"unsupported native dynamic surface/local frame");
        var capsule=native.Objects.First(o=>o.Name=="hknpCapsuleShape");var capsuleTemplate=HollowDeathPacking.Shape.Template(native,capsule.At);
        var fullInfo=NativeAnimation.Skeleton(plan.Skeleton,allowMirroredScale:true);var fullPack=new NativeAnimation.Pack(plan.Skeleton);
        var fullBones=fullPack.Array(fullPack.Object("hkaSkeleton")+40,16,512);
        var full=new P.hkaSkeleton(new(fullInfo.Name,fullInfo.Names,fullInfo.Parents.Select(i=>checked((short)i)).ToArray(),
            Enumerable.Range(0,139).Select(i=>plan.Skeleton[fullPack.Data+fullBones.At+i*16+8]!=0).ToArray(),fullInfo.Local));
        var rag=new P.hkaSkeleton(new(source.Ragdoll.Name,source.Ragdoll.Names,source.Ragdoll.Parents,source.Ragdoll.Locked,source.Ragdoll.Local));
        var forward=source.Mappings.Single(m=>m.A.Names.Length==29);var backward=source.Mappings.Single(m=>m.B.Names.Length==29);
        var f=forward.Simple.Select(m=>new HollowDeathPacking.Simple(checked((short)(110+m.A)),m.B,m.Transform)).ToList();
        var b=backward.Simple.Select(m=>new HollowDeathPacking.Simple(m.A,checked((short)(110+m.B)),m.Transform)).ToList();
        var fc=forward.Chains.Select(c=>new HollowDeathPacking.Chain(checked((short)(110+c.StartA)),checked((short)(110+c.EndA)),c.StartB,c.EndB,c.Transforms)).ToList();
        var bc=backward.Chains.Select(c=>new HollowDeathPacking.Chain(c.StartA,c.EndA,checked((short)(110+c.StartB)),checked((short)(110+c.EndB)),c.Transforms)).ToList();
        var backUnmapped=Enumerable.Range(0,110).Select(i=>checked((short)i)).Concat(backward.Unmapped.Select(i=>checked((short)(110+i)))).ToArray();
        P.Need(f.Count==18&&b.Count==18&&fc.Count==1&&bc.Count==1&&forward.Unmapped.Length==0&&backUnmapped.Length==120,"mapper coverage changed");
        var mapForward=new hkaSkeletonMapper(full,rag,f,fc,forward.Unmapped,forward.ExtractedMotion);
        var mapBackward=new hkaSkeletonMapper(rag,full,b,bc,backUnmapped,backward.ExtractedMotion);
        var shapes=new List<HKX2.hknpShape>();var materials=new List<HKX2.hknpMaterial>();var properties=new List<HKX2.hknpMotionProperties>();
        var motions=new List<HKX2.hknpMotionCinfo>();var bodies=new List<HKX2.hknpBodyCinfo>();
        foreach(var body in source.Bodies) {
            int i=body.Index;P.Need(body.MotionType is 2 or 3&&(body.CollisionFilter&31)==0&&body.CollisionFilter>>16==1,"unsupported source motion/filter policy");
            var shape=HollowDeathPacking.Shape.Convert(capsuleTemplate,body.Shape);shapes.Add(shape);
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
            m_referencedObjects=shapes.Cast<HKX2.hkReferencedObject>().ToList(),m_name="DS1_Golden_Golem",m_skeleton=rag,m_boneToBodyMap=source.BoneToBody.ToList()};
        var scene=new HKX2.hknpPhysicsSceneData{m_systemDatas=[data],m_worldCinfo=null};
        var container=new HKX2.hkaAnimationContainer{m_skeletons=[full,rag],m_animations=[],m_bindings=[],m_attachments=[],m_skins=[]};
        var root=new HKX2.hkRootLevelContainer{m_namedVariants=[
            new(){m_name="Merged Animation Container",m_className="hkaAnimationContainer",m_variant=container},
            new(){m_name="Physics Scene Data",m_className="hknpPhysicsSceneData",m_variant=scene},
            new(){m_name="Physics Ragdoll",m_className="hknpRagdollData",m_variant=data},
            new(){m_name="SkeletonMapper",m_className="hkaSkeletonMapper",m_variant=mapForward},
            new(){m_name="SkeletonMapper",m_className="hkaSkeletonMapper",m_variant=mapBackward}]};
        using var stream=new MemoryStream();new HKX2.PackFileSerializer().Serialize(root,new BinaryWriterEx(false,stream));var output=stream.ToArray();output[18]=1;
        var written=new HollowDeathRig.Pack(output);P.Need(written.Objects.Count(o=>o.Name=="hkaSkeletonMapper")==2&&written.Objects.Count(o=>o.Name=="hknpCapsuleShape")==18&&written.Objects.Count(o=>o.Name=="hkpHingeConstraintData")==17,"native serialized class identities changed");
        return new(output,new(source.PhysicsSHA256,nativeHash,18,17,0,139,18,f.Count,b.Count,110,10,[
            "All18source capsules and17plain hinges, source mass/inertia, rest frames and mapper chains are retained; no donor collider is retained.",
            "Native c2730 material/solver/deactivation/speed/quality policy remains the native template's; source-only runtime constraint offsets/instance metadata have no native serialized equivalent.",
            "Source c2711 has no PWV. Typed native PWV uses unowned body/ragdoll entries and preserves the110native animation SkeletonParam entries; no part/damage parameter is inferred. The source physics embedded animation reference is+.3Z versus the motion skeleton; exact mapper transforms are retained.",
            "Serialization verifies source geometry and transform ownership; source attack/death and solver contact still require in-game validation."]));
    }
    // Keep the authored chain's exact two QS values and the source extracted
    // zero QS, including unused SIMD lanes. Prefix bones remain unmapped.
    sealed class hkaSkeletonMapper(P.hkaSkeleton a,P.hkaSkeleton z,List<HollowDeathPacking.Simple> simple,List<HollowDeathPacking.Chain> chains,short[] unmapped,byte[] extracted):HKX2.hkaSkeletonMapper {
        public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b) {
            P.Need(extracted.Length==48,"golden extracted QS size");b.WriteUInt64(0);b.WriteUInt64(0);s.WriteClassPointer(b,a);s.WriteClassPointer(b,z);
            for(int i=0;i<3;i++)s.WriteUInt32Array(b,[]);s.WriteClassArray(b,simple);s.WriteClassArray(b,chains);
            s.WriteUInt16Array(b,unmapped.Select(i=>unchecked((ushort)i)).ToList());b.WriteBytes(extracted);b.WriteBoolean(true);b.WriteBytes(new byte[15]);
        }
    }
    internal static byte[] Pwv(byte[] original,GoldenGolemPhysicsSource source,NativeRigAppend.Plan plan) {
        ValidatePlan(source,plan);P.Need(System.Convert.ToHexStringLower(SHA256.HashData(original))==HydraPwv.NativePwvSHA256,"unverified c2730 PWV carrier");
        var pwv=HKXPWV.Read(original);P.Need(pwv.BoneMapEntries.Count==60&&pwv.AnimBoneEntries.Count==110&&pwv.RagdollBoneEntries.Count==60&&pwv.Write().SequenceEqual(original),"native PWV round trip");
        pwv.BoneMapEntries=Enumerable.Range(0,18).Select(_=>new HKXPWV.BoneMapEntry()).ToList();
        pwv.AnimBoneEntries.AddRange(Enumerable.Range(0,29).Select(_=>new HKXPWV.AnimBoneEntry()));
        pwv.RagdollBoneEntries=Enumerable.Range(0,18).Select(_=>new HKXPWV.RagdollBoneEntry {RagdollParamID=-1,DamageAnimID=-1,NPCPartGroupIndex=0,Unknown=0,UnknownBB=-1}).ToList();
        var output=pwv.Write();P.Need(output.Length==1576,"golden PWV native stride/population");return output;
    }
}
