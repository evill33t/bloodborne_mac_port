// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text;

internal static class NativeAnimation {
    internal sealed record Rig(string Name,string[] Names,int[] Parents,Matrix4x4[] Local,Matrix4x4[] World);
    // Read only the native 2014 packfile objects needed by animation conversion.
    internal sealed class Pack {
        readonly byte[] bytes;readonly Dictionary<int,int> local=new();
        readonly Dictionary<string,int> objects=new(StringComparer.Ordinal);
        internal readonly int Data,Size;
        internal Pack(byte[] raw) {
            bytes=raw;
            if(raw.Length<272||U(0)!=0x57E0E057||U(4)!=0x10C0C010||U(12)!=11||raw[16]!=8||raw[17]!=1||U(20)!=3)
                throw new InvalidDataException("Unsupported native animation packfile");
            int section=checked(64+BitConverter.ToUInt16(raw,62));
            int classes=I(section+20),header=section+128;Data=I(header+20);Size=I(header+24);
            int global=I(header+28),virtuals=I(header+32),end=I(header+36);
            Range(Data,end);if(Size<0||global<Size||virtuals<global||end<virtuals)throw new InvalidDataException("Invalid native animation fixup sections");
            for(int i=Size;i+8<=global;i+=8)if(U(Data+i)!=uint.MaxValue) {
                int from=I(Data+i),to=I(Data+i+4);if(from<0||from+8>Size||to<0||to>=Size||!local.TryAdd(from,to))throw new InvalidDataException("Invalid animation local fixup");
            }
            for(int i=virtuals;i+12<=end;i+=12)if(U(Data+i)!=uint.MaxValue) {
                int at=I(Data+i),which=I(Data+i+4),name=I(Data+i+8);
                if(which!=0||at<0||at>=Size)throw new InvalidDataException("Invalid animation class fixup");
                string type=Text(classes+name);if(!objects.TryAdd(type,at))throw new InvalidDataException("Repeated animation object class: "+type);
            }
        }
        void Range(int at,int count){if(at<0||count<0||at>bytes.Length-count)throw new InvalidDataException("Truncated native animation packfile");}
        uint U(int at){Range(at,4);return BitConverter.ToUInt32(bytes,at);}
        int I(int at)=>unchecked((int)U(at));
        string Text(int at){Range(at,1);int end=System.Array.IndexOf(bytes,(byte)0,at,Math.Min(256,bytes.Length-at));if(end<at)throw new InvalidDataException("Invalid native animation string");return Encoding.UTF8.GetString(bytes,at,end-at);}
        internal int Object(string name)=>objects.TryGetValue(name,out int at)?at:throw new InvalidDataException("Missing native animation object: "+name);
        internal int Pointer(int offset)=>local.TryGetValue(offset,out int at)?at:throw new InvalidDataException("Missing native animation pointer");
        internal string String(int offset)=>Text(Data+Pointer(offset));
        internal int Int(int offset){if(offset<0||offset>Size-4)throw new InvalidDataException("Invalid native animation data offset");return I(Data+offset);}
        internal short Short(int offset){if(offset<0||offset>Size-2)throw new InvalidDataException("Invalid native bone index");return BitConverter.ToInt16(bytes,Data+offset);}
        internal float Float(int offset){float v=BitConverter.Int32BitsToSingle(Int(offset));if(!float.IsFinite(v)||Math.Abs(v)>100000)throw new InvalidDataException("Invalid native animation scalar");return v;}
        internal (int At,int Count) Array(int offset,int stride,int limit) {
            int count=Int(offset+8);if(count<0||count>limit)throw new InvalidDataException("Native animation array exceeds budget");
            int at=count==0?0:Pointer(offset);if(at<0||(long)at+(long)count*stride>Size)throw new InvalidDataException("Truncated native animation array");return(at,count);
        }
        internal Matrix4x4 Transform(int at) {
            var p=new Vector3(Float(at),Float(at+4),Float(at+8));var q=new Quaternion(Float(at+16),Float(at+20),Float(at+24),Float(at+28));var s=new Vector3(Float(at+32),Float(at+36),Float(at+40));
            if(Math.Abs(q.LengthSquared()-1)>.01f||s.X<=0||s.Y<=0||s.Z<=0)throw new InvalidDataException("Invalid native reference transform");
            return Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(q))*Matrix4x4.CreateTranslation(p);
        }
    }
    internal static Matrix4x4[] Worlds(IReadOnlyList<Matrix4x4> local,IReadOnlyList<int> parents) {
        var output=new Matrix4x4[local.Count];var state=new byte[local.Count];
        Matrix4x4 At(int i){if(i<0||i>=local.Count)throw new InvalidDataException("Invalid animation parent");if(state[i]==2)return output[i];if(state[i]==1)throw new InvalidDataException("Cyclic animation hierarchy");state[i]=1;output[i]=local[i]*(parents[i]<0?Matrix4x4.Identity:At(parents[i]));state[i]=2;return output[i];}
        for(int i=0;i<local.Count;i++)At(i);return output;
    }
    internal static Rig Skeleton(byte[] bytes) {
        var p=new Pack(bytes);int obj=p.Object("hkaSkeleton");var bones=p.Array(obj+40,16,256);var parents=p.Array(obj+24,2,256);var poses=p.Array(obj+56,48,256);
        if(bones.Count<1||bones.Count!=parents.Count||bones.Count!=poses.Count)throw new InvalidDataException("Native animation skeleton arrays disagree");
        var names=new string[bones.Count];var parent=new int[bones.Count];var local=new Matrix4x4[bones.Count];
        for(int i=0;i<names.Length;i++){names[i]=p.String(bones.At+i*16);parent[i]=p.Short(parents.At+i*2);if(parent[i]<-1||parent[i]>=names.Length)throw new InvalidDataException("Invalid native animation parent");local[i]=p.Transform(poses.At+i*48);}
        if(names.Distinct(StringComparer.Ordinal).Count()!=names.Length)throw new InvalidDataException("Repeated native animation bone name");
        return new(p.String(obj+16),names,parent,local,Worlds(local,parent));
    }
    internal static byte[] Convert(SourceAnimation clip,byte[] skeleton,IReadOnlyList<BoneMatch> mappings,float proportionScale,bool preserveSourceWorld=false) {
        if(!float.IsFinite(proportionScale)||proportionScale is <.5f or >2.5f)throw new InvalidDataException("Invalid animation stature scale");
        var rig=Skeleton(skeleton);var sourceNames=clip.Bones.Select((b,i)=>(b.Name,i)).ToDictionary(p=>p.Name,p=>p.i,StringComparer.Ordinal);
        var targetNames=rig.Names.Select((n,i)=>(n,i)).ToDictionary(p=>p.n,p=>p.i,StringComparer.Ordinal);var map=Enumerable.Repeat(-1,rig.Names.Length).ToArray();
        foreach(var m in mappings.Where(m=>m.Direct))if(sourceNames.TryGetValue(m.Source,out int s)&&targetNames.TryGetValue(m.Target,out int t)) {
            if(map[t]!=-1&&map[t]!=s)throw new InvalidDataException("Animation retarget maps two source bones to one target");map[t]=s;
        }
        if(preserveSourceWorld) {
            if(map.Count(i=>i>=0)<19)throw new InvalidDataException("NPC pose has too few explicit skin mappings");
            // A robe may have no vertices weighted directly to a foot/hand.
            // Native IK and hitboxes still consume those animation anchors.
            // Fill unique common names without replacing an explicit skin map.
            static string Key(string name)=>new(name.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
            var uniqueSource=clip.Bones.Select((b,i)=>(key:Key(b.Name),i)).GroupBy(p=>p.key).Where(g=>g.Count()==1).ToDictionary(g=>g.Key,g=>g.Single().i);
            foreach(var group in rig.Names.Select((n,i)=>(key:Key(n),i)).GroupBy(p=>p.key).Where(g=>g.Count()==1))
                if(map[group.Single().i]<0&&uniqueSource.TryGetValue(group.Key,out int source)&&!map.Contains(source))map[group.Single().i]=source;
        }
        // Mesh-weight mappings omit the unweighted locomotion root. Bloodborne's
        // animation modifiers still consume it; leaving Master at reference height
        // offsets a scattered body even when every weighted bone was retargeted.
        int sourceRoot=Enumerable.Range(0,clip.Bones.Count).Single(i=>clip.Bones[i].Parent<0);
        int targetRoot=Enumerable.Range(0,rig.Names.Length).Single(i=>rig.Parents[i]<0);
        if(!string.Equals(clip.Bones[sourceRoot].Name,rig.Names[targetRoot],StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Incompatible source/native animation roots");
        if(map[targetRoot]>=0&&map[targetRoot]!=sourceRoot)throw new InvalidDataException("Conflicting animation root mapping");
        map[targetRoot]=sourceRoot;
        if(map.Count(i=>i>=0)<20)throw new InvalidDataException("Animation has too few matching native bones");
        Matrix4x4[] Scaled(IReadOnlyList<Matrix4x4> local) {
            var w=Worlds(local,clip.Bones.Select(b=>b.Parent).ToArray());for(int i=0;i<w.Length;i++){w[i].M41/=proportionScale;w[i].M42/=proportionScale;w[i].M43/=proportionScale;}return w;
        }
        var rest=Scaled(clip.Bones.Select(b=>Matrix4x4.CreateScale(b.Scale)*Matrix4x4.CreateFromQuaternion(b.Rotation)*Matrix4x4.CreateTranslation(b.Translation)).ToArray());
        var alignment=new Matrix4x4[map.Length];
        for(int t=0;t<map.Length;t++)if(map[t]>=0){if(!Matrix4x4.Invert(rest[map[t]],out var inverse))throw new InvalidDataException("Singular source animation reference");alignment[t]=rig.World[t]*inverse;}
        var transforms=new List<Matrix4x4>(checked(clip.FrameCount*rig.Names.Length));
        if(transforms.Capacity>4000000)throw new InvalidDataException("Converted animation exceeds sampling budget");
        for(int frame=0;frame<clip.FrameCount;frame++) {
            var sw=Scaled(clip.Sample(frame==clip.FrameCount-1?clip.Duration:frame*clip.FrameDuration));var tw=new Matrix4x4[map.Length];var done=new byte[map.Length];
            // Imported NPC meshes already carry the source-to-native bind-pose
            // remap. Applying the native bind delta again displaces their hands
            // by roughly 40cm on the Doll rig. The explicit NPC mode retains the
            // source world pose; ordinary native-rest/combat conversion is unchanged.
            Matrix4x4 At(int t){if(done[t]==2)return tw[t];if(done[t]==1)throw new InvalidDataException("Cyclic target animation");done[t]=1;int parent=rig.Parents[t];var pw=parent<0?Matrix4x4.Identity:At(parent);tw[t]=map[t]>=0?(preserveSourceWorld?sw[map[t]]:alignment[t]*sw[map[t]]):rig.Local[t]*pw;done[t]=2;return tw[t];}
            for(int t=0;t<map.Length;t++){var world=At(t);int parent=rig.Parents[t];var local=world;if(parent>=0){if(!Matrix4x4.Invert(At(parent),out var inverse))throw new InvalidDataException("Singular animated parent");local*=inverse;}transforms.Add(local);}
        }
        return NativeSpline.Encode(transforms,rig.Names.Length,clip.FrameCount,clip.FrameDuration,rig.Name,rig.Names);
    }
}

// The stock HKX2 serializer lacks signed-short arrays. Retain Havok's class
// name/signature while writing the identity transform-track binding.
namespace AnimationPacking {
    internal sealed class hkaAnimationBinding : HKX2.hkaAnimationBinding {
        public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b){b.WriteUInt64(0);b.WriteUInt64(0);s.WriteStringPointer(b,m_originalSkeletonName);s.WriteClassPointer(b,m_animation);s.WriteUInt16Array(b,m_transformTrackToBoneIndices.Select(i=>(ushort)i).ToList());s.WriteUInt16Array(b,[]);s.WriteUInt16Array(b,[]);b.WriteSByte(0);b.WritePattern(7,0);}
    }
}
