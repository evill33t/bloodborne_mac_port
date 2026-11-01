// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text;

// Decode the authored physics scene, not an animation's last-frame pose.
// DS1R's hkp objects are translated to the native hknp wrapper separately.
internal sealed class HollowDeathSource {
    internal sealed record Skeleton(string Name,string[] Names,short[] Parents,bool[] Locked,Matrix4x4[] Local);
    internal sealed record Simple(short A,short B,byte[] Transform);
    internal sealed record Chain(short StartA,short EndA,short StartB,short EndB,byte[] Transforms);
    internal sealed record Mapping(Skeleton A,Skeleton B,IReadOnlyList<Simple> Simple,IReadOnlyList<Chain> Chains,short[] Unmapped);
    internal sealed record Shape(string Type,Vector3 A,Vector3 B,float Radius,Vector3 Half,Vector3 Translation);
    internal sealed record Body(string Name,Shape Shape,Matrix4x4 Transform,Vector4 InertiaMass,Vector4 Center,Quaternion Orientation,
        Vector4 LinearVelocity,Vector4 AngularVelocity,float Friction,float Restitution,float LinearDamping,float AngularDamping,float Gravity,float TimeFactor);
    internal sealed record Constraint(int A,int B,string Type,byte[] Bytes,int[] MotorOffsets);
    internal readonly byte[] Bytes;
    internal readonly SourceCollision.Tag Tag;
    internal readonly Skeleton Ragdoll;
    internal readonly int[] BoneToBody;
    internal readonly IReadOnlyList<Body> Bodies;
    internal readonly IReadOnlyList<Constraint> Constraints;
    internal readonly IReadOnlyList<Mapping> Mappings;
    internal readonly byte[] Motor;
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Source hollow ragdoll: "+why);}
    int At(SourceCollision.Tag.Item item,params string[] names)=>Tag.Nested(item.Type,item.At,names).At;
    SourceCollision.Tag.Item Ref(SourceCollision.Tag.Item item,params string[] names)=>Tag.Reference(At(item,names));
    internal float Float(int at) {float x=BitConverter.ToSingle(Bytes,at);Need(float.IsFinite(x)&&Math.Abs(x)<1000000,"nonfinite scalar");return x;}
    Vector4 Vector(int at)=>new(Float(at),Float(at+4),Float(at+8),Float(at+12));
    float Half(int at)=>BitConverter.Int32BitsToSingle(Tag.UShort(at)<<16);
    string Text(SourceCollision.Tag.Item item,string name) {
        var chars=Ref(item,name);Need(chars.Count is >0 and <=256,"invalid name");var raw=Bytes.AsSpan(chars.At,chars.Count);Need(raw[^1]==0,"unterminated name");return SourceAnimation.DecodeBoneName(raw[..^1].ToArray());
    }
    Skeleton ReadSkeleton(SourceCollision.Tag.Item item) {
        var bones=Tag.Array(item,"bones");var parent=Tag.Array(item,"parentIndices");var pose=Tag.Array(item,"referencePose");
        Need(bones.Count is >0 and <=256&&parent.Count==bones.Count&&pose.Count==bones.Count,"skeleton array mismatch");
        var names=new string[bones.Count];var parents=new short[bones.Count];var locked=new bool[bones.Count];var local=new Matrix4x4[bones.Count];
        for(int i=0;i<bones.Count;i++){var bone=Tag.Element(bones,i);names[i]=Text(bone,"name");parents[i]=unchecked((short)Tag.UShort(Tag.Element(parent,i).At));locked[i]=Tag.Byte(At(bone,"lockTranslation"))!=0;local[i]=Tag.QsTransform(Tag.Element(pose,i).At);Need(parents[i]>=-1&&parents[i]<i,"non-parent-first skeleton");}
        Need(names.Distinct(StringComparer.Ordinal).Count()==names.Length,"ambiguous skeleton bone");return new(Text(item,"name"),names,parents,locked,local);
    }
    Shape ReadShape(SourceCollision.Tag.Item item,Vector3 translation,int depth=0) {
        Need(depth<=1,"nested shape beyond the authored adapter");string type=Tag.Name(item.Type);float radius=Float(At(item,"radius"));Need(radius is >0 and <1,"invalid convex radius");
        if(type=="hkpCapsuleShape")return new(type,Tag.Vector(At(item,"vertexA")),Tag.Vector(At(item,"vertexB")),radius,Vector3.Zero,translation);
        if(type=="hkpBoxShape")return new(type,Vector3.Zero,Vector3.Zero,radius,Tag.Vector(At(item,"halfExtents")),translation);
        if(type=="hkpConvexTranslateShape")return ReadShape(Ref(item,"childShape","childShape"),translation+Tag.Vector(At(item,"translation")),depth+1);
        throw new InvalidDataException("Unsupported source hollow ragdoll shape: "+type);
    }
    internal HollowDeathSource(byte[] bytes,string model) {
        Need(model is "c2500" or "c2540" or "c2550" or "c2560","unsupported source family");Bytes=bytes.ToArray();Tag=new(Bytes);
        var rig=Tag.Items.Single(i=>i.Count==1&&Tag.Name(i.Type)=="hkaRagdollInstance");var bodyItems=Tag.PointerArray(rig,"rigidBodies");var constraintItems=Tag.PointerArray(rig,"constraints");
        Need(bodyItems.Count==(model=="c2560"?18:19)&&constraintItems.Count==bodyItems.Count-1,"authored body/constraint population changed");
        Ragdoll=ReadSkeleton(Ref(rig,"skeleton"));var map=Tag.Array(rig,"boneToRigidBodyMap");BoneToBody=Enumerable.Range(0,map.Count).Select(i=>Tag.Int(Tag.Element(map,i).At)).ToArray();
        Need(Ragdoll.Names.Length==(model=="c2560"?23:24)&&BoneToBody.Length==Ragdoll.Names.Length&&BoneToBody.All(i=>i>=-1&&i<bodyItems.Count),"ragdoll body map changed");
        Bodies=bodyItems.Select(item=> {
            int motion=At(item,"motion"),state=At(item,"motion","motionState");var transform=Tag.Transform(state);var swept=Tag.Nested(item.Type,item.At,"motion","motionState","sweptTransform");
            Need(Tag.Name(swept.Type)=="T[N]"&&Tag.ByteSize(swept.Type)==80,"swept transform layout changed");
            var center=Vector(swept.At+16);var rotation=Vector(swept.At+48);var q=new Quaternion(rotation.X,rotation.Y,rotation.Z,rotation.W);
            Need(Math.Abs(q.LengthSquared()-1)<.001f,"nonunit body quaternion");var inertia=Vector(At(item,"motion","inertiaAndMassInv"));Need(inertia.X>0&&inertia.Y>0&&inertia.Z>0&&inertia.W>0,"invalid dynamic mass/inertia");
            float friction=Float(At(item,"material","friction")),restitution=Float(At(item,"material","restitution"));Need(friction is >=0 and <=1000&&restitution is >=0 and <=1,"invalid surface coefficients");
            return new Body(Text(item,"name"),ReadShape(Ref(item,"collidable","shape"),Vector3.Zero),transform,inertia,center,q,Vector(At(item,"motion","linearVelocity")),Vector(At(item,"motion","angularVelocity")),friction,restitution,Half(Tag.Nested(item.Type,item.At,"motion","motionState","linearDamping").At),Half(Tag.Nested(item.Type,item.At,"motion","motionState","angularDamping").At),Half(Tag.Nested(item.Type,item.At,"motion","gravityFactor").At),Half(Tag.Nested(item.Type,item.At,"motion","motionState","timeFactor").At));
        }).ToArray();
        var indices=bodyItems.Select((item,i)=>(item.At,i)).ToDictionary(p=>p.At,p=>p.i);
        var motors=Tag.Items.Where(i=>i.Count==1&&Tag.Name(i.Type)=="hkpPositionConstraintMotor").ToArray();Need(motors.Length==1&&Tag.ByteSize(motors[0].Type)==48,"constraint motor contract changed");Motor=Bytes.AsSpan(motors[0].At,48).ToArray();Array.Clear(Motor,0,16);
        Constraints=constraintItems.Select(item=> {
            var entities=Tag.Nested(item.Type,item.At,"entities");int a=indices[Tag.Reference(entities.At).At],b=indices[Tag.Reference(entities.At+8).At];Need(a!=b,"self constraint");var data=Ref(item,"data");string type=Tag.Name(data.Type);int size=Tag.ByteSize(data.Type);
            Need(type=="hkpRagdollConstraintData"&&size==416||type=="hkpLimitedHingeConstraintData"&&size==304,"unsupported native-compatible constraint atom layout");
            int transforms=At(data,"atoms","transforms");Need(transforms-data.At==32,"constraint transform atom moved");
            int motor=type=="hkpRagdollConstraintData"?At(data,"atoms","ragdollMotors","motors"):At(data,"atoms","angMotor","motor");int count=type=="hkpRagdollConstraintData"?3:1;var offsets=new int[count];
            for(int i=0;i<count;i++){int location=motor+i*8;var target=Tag.Reference(location);Need(target.Count==0||target.At==motors[0].At,"different constraint motor");offsets[i]=target.Count==0?-1:location-data.At;}
            var raw=Bytes.AsSpan(data.At,size).ToArray();Array.Clear(raw,0,16);for(int i=0;i<count;i++)Array.Clear(raw,motor-data.At+i*8,8);
            return new Constraint(a,b,type,raw,offsets.Where(i=>i>=0).ToArray());
        }).ToArray();
        Mappings=Tag.Items.Where(i=>i.Count==1&&Tag.Name(i.Type)=="hkaSkeletonMapper").Select(item=> {
            var data=Tag.Nested(item.Type,item.At,"mapping");var mapItem=new SourceCollision.Tag.Item(data.Type,data.At,1);var a=ReadSkeleton(Ref(mapItem,"skeletonA"));var b=ReadSkeleton(Ref(mapItem,"skeletonB"));
            Need(Tag.Byte(At(mapItem,"keepUnmappedLocal"))==1&&Tag.Int(At(mapItem,"mappingType"))==0,"source mapper policy changed");
            foreach(string empty in new[]{"partitionMap","simpleMappingPartitionRanges","chainMappingPartitionRanges"})Need(Tag.Array(mapItem,empty).Count==0,"partitioned source mapper");
            Need(Tag.QsTransform(At(mapItem,"extractedMotionMapping"))==Matrix4x4.Identity,"nonidentity source extracted-motion mapping");
            var simple=Tag.Array(mapItem,"simpleMappings");var chain=Tag.Array(mapItem,"chainMappings");var unmapped=Tag.Array(mapItem,"unmappedBones");
            return new Mapping(a,b,Enumerable.Range(0,simple.Count).Select(i=>{int at=Tag.Element(simple,i).At;return new Simple(unchecked((short)Tag.UShort(at)),unchecked((short)Tag.UShort(at+2)),Bytes.AsSpan(at+16,48).ToArray());}).ToArray(),Enumerable.Range(0,chain.Count).Select(i=>{int at=Tag.Element(chain,i).At;return new Chain(unchecked((short)Tag.UShort(at)),unchecked((short)Tag.UShort(at+2)),unchecked((short)Tag.UShort(at+4)),unchecked((short)Tag.UShort(at+6)),Bytes.AsSpan(at+16,96).ToArray());}).ToArray(),Enumerable.Range(0,unmapped.Count).Select(i=>unchecked((short)Tag.UShort(Tag.Element(unmapped,i).At))).ToArray());
        }).ToArray();Need(Mappings.Count==2,"source mapper count changed");
        int expected=model switch {"c2500"=>52,"c2540"=>49,"c2550"=>104,_=>116};
        Need(Mappings.Count(m=>m.A.Names.Length==expected&&m.B.Names.SequenceEqual(Ragdoll.Names))==1&&Mappings.Count(m=>m.B.Names.Length==expected&&m.A.Names.SequenceEqual(Ragdoll.Names))==1,"source family mapper skeleton changed");
        foreach(var m in Mappings) {
            Need(m.Simple.All(s=>s.A>=0&&s.A<m.A.Names.Length&&s.B>=0&&s.B<m.B.Names.Length)&&m.Chains.All(c=>c.StartA>=0&&c.StartA<m.A.Names.Length&&c.EndA>=0&&c.EndA<m.A.Names.Length&&c.StartB>=0&&c.StartB<m.B.Names.Length&&c.EndB>=0&&c.EndB<m.B.Names.Length)&&m.Unmapped.All(i=>i>=0&&i<m.B.Names.Length),"invalid source mapper bone index");
        }
    }
}
