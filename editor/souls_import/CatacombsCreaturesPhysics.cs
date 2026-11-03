// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using S=CatacombsCreaturesPhysicsSource;

// The visible DS1 rig drives its own source-sized hurt bodies. The verified
// c2610 controller remains an unmapped animation prefix, not a second body.
internal static class CatacombsCreaturesPhysics {
    internal const string NativePhysicsSHA256="2ad9e0650605a5d71e4e844bf0e7bdb835f9992278e3185e3fd3a3b2782f7b12";
    internal const string NativePwvSHA256="57be4631005016bed2c1236ef22281d4028cd0e19d5fcfa715b0a4080d315e60";
    internal sealed record Report(string SourceModel,string SourcePhysicsSHA256,string SourcePwvSHA256,string NativePhysicsSHA256,string NativePwvSHA256,
        int Bodies,int Capsules,int Boxes,int Constraints,int SourceBreakableWrappers,int AnimationBones,int RagdollBones,
        int ForwardMappings,int BackwardMappings,int ForwardChains,int BackwardChains,int UnmappedControllerBones,
        uint SourcePwvHeaderPolicy,uint NativePwvHeaderPolicy,int NativeDamagePolicySubstitutions,float WorstReferenceError,
        IReadOnlyList<S.BreakPolicy> UnportedBreakPolicies,IReadOnlyList<string> Limitations);
    internal sealed record Result(byte[] Physics,byte[] Pwv,Report Report);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs native physics: "+why);}
    static string Hash(byte[] b)=>System.Convert.ToHexStringLower(SHA256.HashData(b));
    static void F(byte[] b,int at,float f){Need(float.IsFinite(f),"nonfinite native coefficient");BitConverter.GetBytes(f).CopyTo(b,at);}
    static void I(byte[] b,int at,int n)=>BitConverter.GetBytes(n).CopyTo(b,at);
    static void U16(byte[] b,int at,ushort n)=>BitConverter.GetBytes(n).CopyTo(b,at);
    static void V(byte[] b,int at,Vector4 v){F(b,at,v.X);F(b,at+4,v.Y);F(b,at+8,v.Z);F(b,at+12,v.W);}
    static void Half(byte[] b,int at,float f){Need(float.IsFinite(f)&&f>=0&&f<=1000,"native half coefficient");U16(b,at,(ushort)(BitConverter.SingleToUInt32Bits(f)>>16));}
    internal static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{Vector3.Zero,Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Max(p=>Vector3.Distance(Vector3.Transform(p,a),Vector3.Transform(p,b)));
    internal static Result Convert(byte[] nativePhysics,byte[] nativePwv,S source,byte[] combinedSkeleton) {
        Need(Hash(nativePhysics)==NativePhysicsSHA256&&Hash(nativePwv)==NativePwvSHA256,"unverified installed c2610 physics/PWV carrier");
        var native=new HollowDeathRig.Pack(nativePhysics);int system=native.One("hknpRagdollData");
        var skeletons=native.Objects.Where(o=>o.Name=="hkaSkeleton").Select(o=>(o.At,Info:native.Skeleton(o.At))).ToArray();
        Need(skeletons.Length==2,"native embedded skeleton population");var animation=skeletons.Single(s=>s.Info.Names.Length==80);Need(skeletons.Any(s=>s.Info.Names.Length==24),"c2610 ragdoll carrier");
        var bodies=native.Array(system+64,96,128);var materials=native.Array(system+16,80,128);var properties=native.Array(system+32,64,128);var motions=native.Array(system+48,112,128);
        Need(bodies.Count==19&&materials.Count==19&&properties.Count==19&&motions.Count==19,"c2610 physical carrier arrays");
        Need(!native.Pointers.ContainsKey(materials.At+48)&&!native.Pointers.ContainsKey(bodies.At+88),"unsupported native surface/local frame");
        var info=NativeAnimation.Skeleton(combinedSkeleton,true);const int prefix=80;
        Need(info.Names.Length==prefix+source.Animation.Names.Length&&info.Names.Length<=512&&info.Names.Take(prefix).SequenceEqual(animation.Info.Names)&&info.Names.Skip(prefix).SequenceEqual(source.Animation.Names.Select(n=>"DS1_"+n)),"composite physics/playback bone identity");
        var sourceWorld=NativeAnimation.Worlds(source.Animation.Local,source.Animation.Parents.Select(p=>(int)p).ToArray());
        float worst=Enumerable.Range(0,sourceWorld.Length).Max(i=>Error(info.World[prefix+i],sourceWorld[i]));Need(worst<.003f,"source/playback physical reference disagreement: "+worst);
        var pack=new NativeAnimation.Pack(combinedSkeleton);var boneArray=pack.Array(pack.Object("hkaSkeleton")+40,16,512);
        var fullInfo=new HollowDeathSource.Skeleton(info.Name,info.Names,info.Parents.Select(p=>checked((short)p)).ToArray(),Enumerable.Range(0,info.Names.Length).Select(i=>combinedSkeleton[pack.Data+boneArray.At+i*16+8]!=0).ToArray(),info.Local);
        var full=new HollowDeathPacking.hkaSkeleton(fullInfo);var rag=new HollowDeathPacking.hkaSkeleton(source.Ragdoll);
        var forward=source.Mappings.Single(m=>m.B.Names.SequenceEqual(source.Ragdoll.Names));var backward=source.Mappings.Single(m=>m.A.Names.SequenceEqual(source.Ragdoll.Names));
        var f=forward.Simple.Select(m=>new HollowDeathPacking.Simple(checked((short)(prefix+m.A)),m.B,m.Transform)).ToList();
        var b=backward.Simple.Select(m=>new HollowDeathPacking.Simple(m.A,checked((short)(prefix+m.B)),m.Transform)).ToList();
        var fc=forward.Chains.Select(m=>new HollowDeathPacking.Chain(checked((short)(prefix+m.StartA)),checked((short)(prefix+m.EndA)),m.StartB,m.EndB,m.Transforms)).ToList();
        var bc=backward.Chains.Select(m=>new HollowDeathPacking.Chain(m.StartA,m.EndA,checked((short)(prefix+m.StartB)),checked((short)(prefix+m.EndB)),m.Transforms)).ToList();
        var unmapped=Enumerable.Range(0,prefix).Select(i=>checked((short)i)).Concat(backward.Unmapped.Select(i=>checked((short)(prefix+i)))).ToArray();
        var toRag=new hkaSkeletonMapper(full,rag,f,fc,forward.Unmapped,forward.Extracted);var toSkin=new hkaSkeletonMapper(rag,full,b,bc,unmapped,backward.Extracted);
        int capsule=native.Objects.First(o=>o.Name=="hknpCapsuleShape").At;var shapeTemplate=HollowDeathPacking.Shape.Template(native,capsule);
        var outputShapes=new List<HKX2.hknpShape>();var outputMaterials=new List<HKX2.hknpMaterial>();var outputProperties=new List<HKX2.hknpMotionProperties>();var outputMotions=new List<HKX2.hknpMotionCinfo>();var outputBodies=new List<HKX2.hknpBodyCinfo>();
        foreach(var body in source.Bodies) {
            int index=body.Index;var shape=HollowDeathPacking.Shape.Convert(shapeTemplate,body.Shape);outputShapes.Add(shape);
            var mat=native.Raw(materials.At,80);Half(mat,18,body.Friction);Half(mat,20,body.Friction);Half(mat,22,body.Restitution);outputMaterials.Add(new HollowDeathPacking.Material(mat,native.Text(materials.At)));
            var prop=native.Raw(properties.At,64);F(prop,8,body.Gravity);F(prop,12,body.TimeFactor);F(prop,24,body.LinearDamping);F(prop,28,body.AngularDamping);outputProperties.Add(new HollowDeathPacking.Properties(prop));
            var motion=native.Raw(motions.At,112);U16(motion,0,checked((ushort)index));F(motion,4,body.InertiaMassInverse.W);V(motion,32,new(body.InertiaMassInverse.X,body.InertiaMassInverse.Y,body.InertiaMassInverse.Z,1));
            V(motion,48,new(body.CenterOfMass.X,body.CenterOfMass.Y,body.CenterOfMass.Z,BitConverter.ToSingle(motion,60)));V(motion,64,new(body.Orientation.X,body.Orientation.Y,body.Orientation.Z,body.Orientation.W));V(motion,80,body.LinearVelocity);V(motion,96,body.AngularVelocity);outputMotions.Add(new HollowDeathPacking.Motion(motion));
            var raw=native.Raw(bodies.At,96);I(raw,12,index);U16(raw,18,checked((ushort)index));I(raw,20,unchecked((int)body.CollisionFilter));BitConverter.GetBytes(body.UserData).CopyTo(raw,40);V(raw,48,new(body.Rest.Translation,1));
            Need(Matrix4x4.Decompose(body.Rest,out var scale,out var rotation,out _)&&Vector3.Distance(scale,Vector3.One)<.001f,"nonrigid body placement");V(raw,64,new(rotation.X,rotation.Y,rotation.Z,rotation.W));outputBodies.Add(new HollowDeathPacking.Body(raw,body.Name,shape));
        }
        HKX2.hkpConstraintMotor? motor=source.Motor==null?null:new HollowDeathPacking.hkpPositionConstraintMotor(source.Motor);
        var constraints=source.Constraints.Select(c=>{var adapted=HollowDeathRig.ConstraintData(c.Child);Need(adapted.MotorOffsets.Length==0||motor!=null,"constraint motor absent");
            HKX2.hkpConstraintData child=c.Child.Type=="hkpRagdollConstraintData"?new HollowDeathPacking.hkpRagdollConstraintData(adapted.Bytes,adapted.MotorOffsets,motor!):new HollowDeathPacking.hkpLimitedHingeConstraintData(adapted.Bytes,adapted.MotorOffsets,motor!);
            return new HKX2.hknpConstraintCinfo{m_bodyA=(uint)c.Child.A,m_bodyB=(uint)c.Child.B,m_flags=0,m_constraintData=child};}).ToList();
        var data=new HKX2.hknpRagdollData{m_materials=outputMaterials,m_motionProperties=outputProperties,m_motionCinfos=outputMotions,m_bodyCinfos=outputBodies,m_constraintCinfos=constraints,m_referencedObjects=outputShapes.Cast<HKX2.hkReferencedObject>().ToList(),m_name="DS1_Catacombs_"+source.Model,m_skeleton=rag,m_boneToBodyMap=source.BoneToBody.ToList()};
        var scene=new HKX2.hknpPhysicsSceneData{m_systemDatas=[data],m_worldCinfo=null};var container=new HKX2.hkaAnimationContainer{m_skeletons=[full,rag],m_animations=[],m_bindings=[],m_attachments=[],m_skins=[]};
        var root=new HKX2.hkRootLevelContainer{m_namedVariants=[new(){m_name="Merged Animation Container",m_className="hkaAnimationContainer",m_variant=container},new(){m_name="Physics Scene Data",m_className="hknpPhysicsSceneData",m_variant=scene},new(){m_name="Physics Ragdoll",m_className="hknpRagdollData",m_variant=data},new(){m_name="SkeletonMapper",m_className="hkaSkeletonMapper",m_variant=toRag},new(){m_name="SkeletonMapper",m_className="hkaSkeletonMapper",m_variant=toSkin}]};
        using var stream=new MemoryStream();new HKX2.PackFileSerializer().Serialize(root,new BinaryWriterEx(false,stream));var physics=stream.ToArray();physics[18]=1;_=new HollowDeathRig.Pack(physics);
        var pwv=HKXPWV.Read(nativePwv);Need(pwv.Write().SequenceEqual(nativePwv)&&pwv.BoneMapEntries.Count==19&&pwv.AnimBoneEntries.Count==prefix&&pwv.RagdollBoneEntries.Count==24,"native PWV c2610 carrier");
        uint nativeHeaderPolicy=BitConverter.ToUInt32(nativePwv,12);Need(nativeHeaderPolicy==0,"unverified native PWV header policy");
        Need(pwv.RagdollBoneEntries.All(r=>r.RagdollParamID==-1&&r.DamageAnimID==-1&&r.NPCPartGroupIndex==0&&r.UnknownBB==-1),"native non-part damage policy");
        pwv.BoneMapEntries=source.Pwv.BoneMapEntries.Select(r=>new HKXPWV.BoneMapEntry{UnknownValueA=r.UnknownValueA,UnknownValueB=r.UnknownValueB}).ToList();
        pwv.AnimBoneEntries.AddRange(source.Pwv.AnimBoneEntries.Select(r=>new HKXPWV.AnimBoneEntry{SkeletonParamID=r.SkeletonParamID,Unk2=r.Unk2}));
        int substitutions=source.Pwv.RagdollBoneEntries.Count(r=>r.DamageAnimID>=0);
        pwv.RagdollBoneEntries=source.Pwv.RagdollBoneEntries.Select(r=>new HKXPWV.RagdollBoneEntry{RagdollParamID=r.RagdollParamID,DamageAnimID=-1,NPCPartGroupIndex=r.NPCPartGroupIndex,Unknown=r.Unknown,UnknownBB=-1}).ToList();
        var metadata=pwv.Write();Need(BitConverter.ToUInt32(metadata,12)==nativeHeaderPolicy&&HKXPWV.Read(metadata).Write().SequenceEqual(metadata),"native PWV output round trip/policy");
        var wrapped=source.Constraints.Where(c=>c.Breakable!=null).Select(c=>c.Breakable!).ToArray();
        return new(physics,metadata,new(source.Model,source.PhysicsSHA256,source.PwvSHA256,Hash(nativePhysics),Hash(nativePwv),source.Bodies.Count,source.Bodies.Count(b=>b.Shape.Type=="hkpCapsuleShape"),source.Bodies.Count(b=>b.Shape.Type=="hkpBoxShape"),constraints.Count,wrapped.Length,info.Names.Length,source.Ragdoll.Names.Length,f.Count,b.Count,fc.Count,bc.Count,prefix,source.PwvHeaderPolicy,nativeHeaderPolicy,substitutions,worst,wrapped,[
            "Source capsule/translated-box dimensions, body rest frames, mass/inertia, friction, gravity/damping, joint child frames/limits/motors and both chained skeleton mappings are retained. No donor hurt body remains.",
            "Bloodborne c2610 solver/deactivation/speed/quality defaults remain the native policy; source-only hkp instance runtime offsets are not copied into hknp.",
            "Source breakable joint wrappers are represented by their exact verified native ragdoll children. Physical break thresholds/removal/revert policies are unported and retained in this report. Collapse and reassembly use the existing source animation/event adapter; full physical DS1 bone separation is not implemented.",
            "The source PWV header policies5/5/0/1 are reported separately; their runtime meaning is unverified. Native c2610 header policy0 is emitted. A bounded installed-native census found policies0/1/2 and no5; common format acceptance alone does not establish native semantics.",
            "Source c2900 PWV a99 damage animation2000 has no loaded c2610 native carrier. Those entries use the verified native DamageAnimID−1 policy; existing ordinary/native hit reactions remain. No dangling damage clip reference is emitted.",
            "Serialization and posed-body fixtures do not prove native contact damage, ragdoll settling or temporary-collapse timing; gameplay validation is required."
        ]));
    }
    sealed class hkaSkeletonMapper(HollowDeathPacking.hkaSkeleton a,HollowDeathPacking.hkaSkeleton z,List<HollowDeathPacking.Simple> simple,List<HollowDeathPacking.Chain> chains,short[] unmapped,byte[] extracted):HKX2.hkaSkeletonMapper {
        public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b){Need(extracted.Length==48,"mapper extracted QS size");b.WriteUInt64(0);b.WriteUInt64(0);s.WriteClassPointer(b,a);s.WriteClassPointer(b,z);for(int i=0;i<3;i++)s.WriteUInt32Array(b,[]);s.WriteClassArray(b,simple);s.WriteClassArray(b,chains);s.WriteUInt16Array(b,unmapped.Select(i=>unchecked((ushort)i)).ToList());b.WriteBytes(extracted);b.WriteBoolean(true);b.WriteBytes(new byte[15]);}
    }
}
