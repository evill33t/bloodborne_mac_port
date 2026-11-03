// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using Skeleton=HollowDeathSource.Skeleton;
using Shape=HollowDeathSource.Shape;
using Simple=HollowDeathSource.Simple;
using Chain=HollowDeathSource.Chain;

// The four ordinary Catacombs families have their own complete physical rigs.
// Read the source wrappers separately: physical break policies are evidence,
// not presumed compatible with Bloodborne's hknp constraint solver.
internal sealed class CatacombsCreaturesPhysicsSource {
    internal sealed record Body(int Index,string Name,Shape Shape,Matrix4x4 Rest,Vector4 InertiaMassInverse,
        Vector4 CenterOfMass,Quaternion Orientation,Vector4 LinearVelocity,Vector4 AngularVelocity,
        float Friction,float Restitution,float LinearDamping,float AngularDamping,float Gravity,float TimeFactor,
        byte MotionType,uint CollisionFilter,ulong UserData);
    internal sealed record BreakPolicy(float SolverResultLimit,bool RemoveWhenBroken,bool RevertBackVelocity,ulong UserData);
    internal sealed record Constraint(int Index,HollowDeathSource.Constraint Child,BreakPolicy? Breakable);
    internal sealed record Mapping(Skeleton A,Skeleton B,IReadOnlyList<Simple> Simple,IReadOnlyList<Chain> Chains,short[] Unmapped,byte[] Extracted);
    internal readonly string Model,PhysicsSHA256,PwvSHA256;
    internal readonly Skeleton Animation,Ragdoll;
    internal readonly IReadOnlyList<Body> Bodies;
    internal readonly IReadOnlyList<Constraint> Constraints;
    internal readonly IReadOnlyList<Mapping> Mappings;
    internal readonly int[] BoneToBody;
    internal readonly HKXPWV Pwv;
    internal readonly byte[]? Motor;
    internal readonly uint PwvHeaderPolicy;
    readonly byte[] bytes;
    readonly SourceCollision.Tag tag;
    readonly Dictionary<int,Skeleton> skeletons=[];
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs source physics: "+why);}
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    int At(SourceCollision.Tag.Item item,params string[] fields)=>tag.Nested(item.Type,item.At,fields).At;
    SourceCollision.Tag.Item Ref(SourceCollision.Tag.Item item,params string[] fields)=>tag.Reference(At(item,fields));
    float Float(int at){float f=BitConverter.ToSingle(bytes,at);Need(float.IsFinite(f),"nonfinite scalar");return f;}
    Vector4 V(int at)=>new(Float(at),Float(at+4),Float(at+8),Float(at+12));
    float Half(int at){float f=BitConverter.Int32BitsToSingle(tag.UShort(at)<<16);Need(float.IsFinite(f)&&f>=0&&f<=1000,"motion coefficient");return f;}
    string Text(SourceCollision.Tag.Item item,string field){var text=Ref(item,field);Need(text.Count is >1 and <=256&&bytes[text.At+text.Count-1]==0,"name bounds/termination");return SourceAnimation.DecodeBoneName(bytes.AsSpan(text.At,text.Count-1).ToArray());}
    Skeleton ReadSkeleton(SourceCollision.Tag.Item item) {
        if(skeletons.TryGetValue(item.At,out var cached))return cached;
        var bones=tag.Array(item,"bones");var parents=tag.Array(item,"parentIndices");var pose=tag.Array(item,"referencePose");
        Need(bones.Count is >0 and <=256&&parents.Count==bones.Count&&pose.Count==bones.Count&&tag.Array(item,"partitions").Count==0,"skeleton arrays/partitions");
        var names=new string[bones.Count];var indices=new short[bones.Count];var locked=new bool[bones.Count];var local=new Matrix4x4[bones.Count];
        for(int i=0;i<bones.Count;i++){var b=tag.Element(bones,i);names[i]=Text(b,"name");indices[i]=unchecked((short)tag.UShort(tag.Element(parents,i).At));locked[i]=tag.Byte(At(b,"lockTranslation"))!=0;local[i]=tag.QsTransform(tag.Element(pose,i).At);Need(indices[i]>=-1&&indices[i]<i,"skeleton parent order");}
        Need(indices.Count(i=>i<0)==1&&names.Distinct(StringComparer.Ordinal).Count()==names.Length,"skeleton root/names");
        var result=new Skeleton(Text(item,"name"),names,indices,locked,local);skeletons.Add(item.At,result);return result;
    }
    Mapping ReadMapping(SourceCollision.Tag.Item item) {
        var nested=tag.Nested(item.Type,item.At,"mapping");var m=new SourceCollision.Tag.Item(nested.Type,nested.At,1);
        Need(tag.Byte(At(m,"keepUnmappedLocal"))==1&&tag.Int(At(m,"mappingType"))==0,"mapper policy");
        foreach(string empty in new[]{"partitionMap","simpleMappingPartitionRanges","chainMappingPartitionRanges"})Need(tag.Array(m,empty).Count==0,"partitioned mapper");
        var a=ReadSkeleton(Ref(m,"skeletonA"));var b=ReadSkeleton(Ref(m,"skeletonB"));var simple=tag.Array(m,"simpleMappings");var chains=tag.Array(m,"chainMappings");var unmapped=tag.Array(m,"unmappedBones");
        Need(simple.Count<=256&&chains.Count<=16&&unmapped.Count<=256&&tag.ByteSize(simple.Type)==64&&tag.ByteSize(chains.Type)==112,"mapper layout/budget");
        short S(int at)=>unchecked((short)tag.UShort(at));
        var sm=Enumerable.Range(0,simple.Count).Select(i=>{int at=tag.Element(simple,i).At;return new Simple(S(at),S(at+2),bytes.AsSpan(at+16,48).ToArray());}).ToArray();
        var ch=Enumerable.Range(0,chains.Count).Select(i=>{int at=tag.Element(chains,i).At;return new Chain(S(at),S(at+2),S(at+4),S(at+6),bytes.AsSpan(at+16,96).ToArray());}).ToArray();
        var un=Enumerable.Range(0,unmapped.Count).Select(i=>S(tag.Element(unmapped,i).At)).ToArray();
        int duplicates=Model=="c2650"&&a.Names.Length==88&&b.Names.Length==23?4:0;
        Need(sm.All(s=>s.A>=0&&s.A<a.Names.Length&&s.B>=0&&s.B<b.Names.Length)&&sm.Length-sm.Select(s=>s.B).Distinct().Count()==duplicates,"simple mapper ownership");
        Need(ch.All(c=>c.StartA>=0&&c.StartA<a.Names.Length&&c.EndA>=0&&c.EndA<a.Names.Length&&c.StartB>=0&&c.StartB<b.Names.Length&&c.EndB>=0&&c.EndB<b.Names.Length),"chain bounds");
        Need(un.All(i=>i>=0&&i<b.Names.Length)&&un.Distinct().Count()==un.Length&&!un.Intersect(sm.Select(s=>s.B)).Any(),"unmapped ownership");
        return new(a,b,sm,ch,un,bytes.AsSpan(At(m,"extractedMotionMapping"),48).ToArray());
    }
    Shape ReadShape(SourceCollision.Tag.Item item,Vector3 translation,int depth=0) {
        Need(depth<=1,"nested convex translation");string type=tag.Name(item.Type);
        if(type=="hkpConvexTranslateShape")return ReadShape(Ref(item,"childShape","childShape"),translation+tag.Vector(At(item,"translation")),depth+1);
        float radius=Float(At(item,"radius"));Need(radius is >=0 and <=4&&(type=="hkpBoxShape"||radius>0),"convex radius");
        if(type=="hkpCapsuleShape")return new(type,tag.Vector(At(item,"vertexA")),tag.Vector(At(item,"vertexB")),radius,Vector3.Zero,translation);
        if(type=="hkpBoxShape"){var half=tag.Vector(At(item,"halfExtents"));Need(half.X>0&&half.Y>0&&half.Z>0,"box dimensions");return new(type,Vector3.Zero,Vector3.Zero,radius,half,translation);}
        throw new InvalidDataException("Unsupported Catacombs source shape: "+type);
    }
    Constraint ReadConstraint(SourceCollision.Tag.Item item,int index,IReadOnlyDictionary<int,int> bodies,int? motorAt) {
        var entity=tag.Nested(item.Type,item.At,"entities");bool gotA=bodies.TryGetValue(tag.Reference(entity.At).At,out int a),gotB=bodies.TryGetValue(tag.Reference(entity.At+8).At,out int b);Need(gotA&&gotB&&a!=b,"constraint endpoints");
        var data=Ref(item,"data");BreakPolicy? policy=null;
        if(tag.Name(data.Type)=="hkpBreakableConstraintData"){
            Need(tag.ByteSize(data.Type)==80,"breakable wrapper layout");float limit=Float(At(data,"solverResultLimit"));Need(limit>=0,"negative break threshold");
            policy=new(limit,tag.Byte(At(data,"removeWhenBroken"))!=0,tag.Byte(At(data,"revertBackVelocityOnBreak"))!=0,BitConverter.ToUInt64(bytes,At(data,"userData")));data=Ref(data,"constraintData");
        }
        string type=tag.Name(data.Type);int size=tag.ByteSize(data.Type);Need(type=="hkpRagdollConstraintData"&&size==416||type=="hkpLimitedHingeConstraintData"&&size==304,"unsupported child joint "+type);
        int field=type=="hkpRagdollConstraintData"?At(data,"atoms","ragdollMotors","motors"):At(data,"atoms","angMotor","motor");int count=type=="hkpRagdollConstraintData"?3:1;var offsets=new List<int>();
        for(int i=0;i<count;i++){var target=tag.Reference(field+i*8);Need(target.Count==0||target.At==motorAt,"unowned constraint motor");if(target.Count>0)offsets.Add(field+i*8-data.At);}
        var raw=bytes.AsSpan(data.At,size).ToArray();Array.Clear(raw,0,16);for(int i=0;i<count;i++)Array.Clear(raw,field-data.At+i*8,8);
        var child=new HollowDeathSource.Constraint(a,b,type,raw,offsets.ToArray());_=HollowDeathRig.ConstraintData(child);
        return new(index,child,policy);
    }
    internal CatacombsCreaturesPhysicsSource(string model,byte[] physics,byte[] pwv) {
        var expected=model switch {"c2900"=>(17,59,22),"c2910"=>(20,59,25),"c2650"=>(18,88,23),"c2930"=>(21,55,27),_=>throw new InvalidDataException("Unsupported Catacombs physical family "+model)};
        Need(physics.Length is >32 and <=16*1024*1024&&pwv.Length<=16384,"read budgets");Model=model;bytes=physics.ToArray();tag=new(bytes);PhysicsSHA256=Convert.ToHexStringLower(SHA256.HashData(bytes));PwvSHA256=Convert.ToHexStringLower(SHA256.HashData(pwv));
        var rig=tag.Items.Single(i=>i.Count==1&&tag.Name(i.Type)=="hkaRagdollInstance");var bodyItems=tag.PointerArray(rig,"rigidBodies");var jointItems=tag.PointerArray(rig,"constraints");
        Need(bodyItems.Count==expected.Item1&&jointItems.Count==expected.Item1-1,"body/joint population");Ragdoll=ReadSkeleton(Ref(rig,"skeleton"));Need(Ragdoll.Names.Length==expected.Item3,"ragdoll population");
        var bodyMap=tag.Array(rig,"boneToRigidBodyMap");BoneToBody=Enumerable.Range(0,bodyMap.Count).Select(i=>tag.Int(tag.Element(bodyMap,i).At)).ToArray();
        Need(BoneToBody.Length==Ragdoll.Names.Length&&BoneToBody.All(i=>i>=-1&&i<bodyItems.Count)&&Enumerable.Range(0,bodyItems.Count).All(i=>BoneToBody.Count(b=>b==i)==1),"bone/body ownership");
        Mappings=tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type)=="hkaSkeletonMapper").Select(ReadMapping).ToArray();Need(Mappings.Count==2,"two mapper directions required");Animation=Mappings.Single(m=>m.B.Names.SequenceEqual(Ragdoll.Names)).A;
        Need(Animation.Names.Length==expected.Item2&&Mappings.Count(m=>m.A.Names.SequenceEqual(Ragdoll.Names)&&m.B.Names.SequenceEqual(Animation.Names))==1,"animation/reverse mapper population");
        Bodies=bodyItems.Select((item,index)=>{int state=At(item,"motion","motionState");var rest=tag.Transform(state);rest.M14=rest.M24=rest.M34=0;rest.M44=1;Need(Math.Abs(rest.GetDeterminant()-1)<.002f,"nonrigid body rest");
            var swept=tag.Nested(item.Type,item.At,"motion","motionState","sweptTransform");Need(tag.ByteSize(swept.Type)==80,"swept transform layout");var qv=V(swept.At+48);var q=new Quaternion(qv.X,qv.Y,qv.Z,qv.W);var mass=V(At(item,"motion","inertiaAndMassInv"));Need(Math.Abs(q.LengthSquared()-1)<.001f&&mass.X>0&&mass.Y>0&&mass.Z>0&&mass.W>0,"body mass/orientation");
            string name=Text(item,"name");Need(name==Ragdoll.Names[Array.IndexOf(BoneToBody,index)],"named body ownership");
            return new Body(index,name,ReadShape(Ref(item,"collidable","shape"),Vector3.Zero),rest,mass,V(swept.At+16),q,V(At(item,"motion","linearVelocity")),V(At(item,"motion","angularVelocity")),Float(At(item,"material","friction")),Float(At(item,"material","restitution")),Half(At(item,"motion","motionState","linearDamping")),Half(At(item,"motion","motionState","angularDamping")),Half(At(item,"motion","gravityFactor")),Half(At(item,"motion","motionState","timeFactor")),tag.Byte(At(item,"motion","type")),tag.UInt(At(item,"collidable","broadPhaseHandle","collisionFilterInfo")),BitConverter.ToUInt64(bytes,At(item,"userData")));}).ToArray();
        Need(Bodies.All(b=>b.MotionType is 2 or 3&&b.Friction is >=0 and <=1000&&b.Restitution is >=0 and <=1&&(b.CollisionFilter&31)==0),"motion/surface/filter policy");
        var motors=tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type)=="hkpPositionConstraintMotor").ToArray();Need(motors.Length==(model=="c2650"?1:0),"motor population");int? motorAt=motors.Length==0?null:motors[0].At;
        if(motorAt.HasValue){Need(tag.ByteSize(motors[0].Type)==48,"motor layout");Motor=bytes.AsSpan(motorAt.Value,48).ToArray();Array.Clear(Motor,0,16);}
        var bodyIndices=bodyItems.Select((b,i)=>(b.At,i)).ToDictionary(x=>x.At,x=>x.i);Constraints=jointItems.Select((c,i)=>ReadConstraint(c,i,bodyIndices,motorAt)).ToArray();
        var reached=new HashSet<int>{0};for(int i=0;i<Bodies.Count;i++)foreach(var c in Constraints)if(reached.Contains(c.Child.A)||reached.Contains(c.Child.B)){reached.Add(c.Child.A);reached.Add(c.Child.B);}Need(reached.Count==Bodies.Count,"disconnected joint topology");
        Pwv=HKXPWV.Read(pwv);Need(Pwv.Write().SequenceEqual(pwv)&&Pwv.BoneMapEntries.Count==Bodies.Count&&Pwv.AnimBoneEntries.Count==Animation.Names.Length&&Pwv.RagdollBoneEntries.Count==Ragdoll.Names.Length,"source PWV round trip/population");
        PwvHeaderPolicy=BitConverter.ToUInt32(pwv,12);Need(PwvHeaderPolicy==(model is "c2900" or "c2910"?5u:model=="c2930"?1u:0u),"source PWV header policy");
        Need(Pwv.BoneMapEntries.All(b=>b.UnknownValueA==-1&&b.UnknownValueB==0)&&Pwv.AnimBoneEntries.All(b=>b.SkeletonParamID==-1&&b.Unk2==0),"unowned source SkeletonParam/body flags");
        Need(Pwv.RagdollBoneEntries.All(r=>r.RagdollParamID==-1&&r.Unknown==0&&r.DamageAnimID is -1 or 2000&&r.NPCPartGroupIndex is 0 or 7),"unsupported source ragdoll metadata");
    }
    internal static CatacombsCreaturesPhysicsSource Load(Func<string,byte[]?> read,string model) {
        var bank=BND3.Read(read("chr/"+model+".chrbnd.dcx")??throw new FileNotFoundException("Catacombs source character unavailable: "+model));
        return new(model,bank.Files.Single(f=>Leaf(f.Name)==model+".hkx").Bytes.ToArray(),bank.Files.Single(f=>Leaf(f.Name)==model+".hkxpwv").Bytes.ToArray());
    }
}
