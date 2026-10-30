// SPDX-License-Identifier: GPL-3.0-or-later
// Independent packfile/spline reader shared in design with NativeAnimationTests.
// It checks emitted bytes without the production NativeAnimation.Pack reader.
using System.Numerics;
using System.Text;
sealed class NpcAnimationInspector {
    readonly byte[] bytes;readonly (int At,int Local,int Global,int Virtual,int End)[] sections=new (int,int,int,int,int)[3];
    readonly Dictionary<int,int> local=[];readonly Dictionary<int,(int Section,int At)> global=[];
    internal readonly Dictionary<string,int> Objects=new(StringComparer.Ordinal);
    internal int Data=>sections[2].At;
    public NpcAnimationInspector(byte[] source) {
        bytes=source;Check(U(0)==0x57e0e057&&U(4)==0x10c0c010&&I(12)==11&&bytes[16]==8&&bytes[17]==1&&I(20)==3,"Unexpected native pack header");
        Check(Encoding.ASCII.GetString(bytes,40,14).StartsWith("hk_2014.1.0-r1",StringComparison.Ordinal),"Unexpected native pack SDK identity");
        int headers=64+S(62);
        for(int n=0;n<3;++n){int at=headers+n*64;var section=(At:I(at+20),Local:I(at+24),Global:I(at+28),Virtual:I(at+32),End:I(at+36));Check(section.At>=0&&section.Local>=0&&section.Global>=section.Local&&section.Virtual>=section.Global&&section.End>=section.Virtual&&section.At<=bytes.Length-section.End,"Invalid native section range");sections[n]=section;}
        var data=sections[2];
        for(int at=Data+data.Local;at+8<=Data+data.Global;at+=8)if(U(at)!=uint.MaxValue){int from=I(at),to=I(at+4);Check(from>=0&&from<=data.Local-8&&to>=0&&to<data.Local,"Invalid local fixup");local.Add(from,to);}
        for(int at=Data+data.Global;at+12<=Data+data.Virtual;at+=12)if(U(at)!=uint.MaxValue){int from=I(at),section=I(at+4),to=I(at+8);Check(from>=0&&from<=data.Local-8&&section is >=0 and <3&&to>=0&&to<sections[section].Local,"Invalid global fixup");global.Add(from,(section,to));}
        for(int at=Data+data.Virtual;at+12<=Data+data.End;at+=12)if(U(at)!=uint.MaxValue){int obj=I(at),section=I(at+4),name=I(at+8);Check(section==0&&obj>=0&&obj<data.Local&&name>=0&&name<sections[0].Local,"Invalid virtual fixup");Objects.Add(Text(sections[0].At+name),obj);}
    }
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    void Range(int at,int size){Check(at>=0&&size>=0&&at<=bytes.Length-size,"Truncated packfile read");}
    uint U(int at){Range(at,4);return BitConverter.ToUInt32(bytes,at);}
    int I(int at)=>unchecked((int)U(at));
    ushort S(int at){Range(at,2);return BitConverter.ToUInt16(bytes,at);}
    float F(int at){float value=BitConverter.UInt32BitsToSingle(U(at));Check(float.IsFinite(value)&&Math.Abs(value)<100000,"Non-finite serialized animation component");return value;}
    string Text(int at){Range(at,1);int end=System.Array.IndexOf(bytes,(byte)0,at);Check(end>=at&&end-at<256,"Invalid packfile string");return Encoding.UTF8.GetString(bytes,at,end-at);}
    int Pointer(int offset){Check(local.ContainsKey(offset),"Missing local pointer");Check(BitConverter.ToUInt64(bytes,Data+offset)==0,"Relocated pointer leaked into disk file");return local[offset];}
    int Reference(int offset){Check(global.TryGetValue(offset,out var p)&&p.Section==2,"Missing data-object reference");return p.At;}
    internal (int At,int Count) Array(int offset,int stride,int limit) {
        int count=I(Data+offset+8);uint capacity=U(Data+offset+12)&0x7fffffff;Check(count>=0&&count<=limit&&capacity>=count,"Invalid serialized array count/capacity");
        int at=count==0?0:Pointer(offset);Check((long)at+(long)count*stride<=sections[2].Local,"Serialized array crosses fixups");return(at,count);
    }
    Matrix4x4 Qs(int offset,bool output=false) {
        int at=Data+offset;var p=new Vector3(F(at),F(at+4),F(at+8));var q=new Quaternion(F(at+16),F(at+20),F(at+24),F(at+28));var scale=new Vector3(F(at+32),F(at+36),F(at+40));
        Check(Math.Abs(q.LengthSquared()-1)<.001f&&scale.X>0&&scale.Y>0&&scale.Z>0,"Invalid serialized QS transform");
        if(output)Check(F(at+12)==0&&F(at+44)==0,"Serialized QS SIMD padding is not canonical");
        return Matrix4x4.CreateScale(scale)*Matrix4x4.CreateFromQuaternion(q)*Matrix4x4.CreateTranslation(p);
    }
    internal sealed record Rig(string Name,string[] Names,int[] Parents,Matrix4x4[] Local);
    internal static Rig Skeleton(byte[] bytes) {
        var p=new NpcAnimationInspector(bytes);int obj=p.Objects["hkaSkeleton"];var names=p.Array(obj+40,16,256);var parents=p.Array(obj+24,2,256);var reference=p.Array(obj+56,48,256);
        Check(names.Count==parents.Count&&names.Count==reference.Count,"Native reference arrays disagree");
        return new(p.Text(p.Data+p.Pointer(obj+16)),Enumerable.Range(0,names.Count).Select(i=>p.Text(p.Data+p.Pointer(names.At+i*16))).ToArray(),Enumerable.Range(0,parents.Count).Select(i=>(int)unchecked((short)p.S(p.Data+parents.At+i*2))).ToArray(),Enumerable.Range(0,reference.Count).Select(i=>p.Qs(reference.At+i*48)).ToArray());
    }
    internal sealed record Clip(int Object,int Frames,int Bones,float Duration,Matrix4x4[][] Poses);
    internal (int Type,float Duration,int Tracks,int FloatTracks) Header(){int obj=Objects["hkaSplineCompressedAnimation"];return(I(Data+obj+12),F(Data+obj+16),I(Data+obj+20),I(Data+obj+24));}
    internal (int Frames,int MaximumFrames,float FrameDuration,float BlockDuration,float Inverse) Timing(){int obj=Objects["hkaSplineCompressedAnimation"];return(I(Data+obj+56),I(Data+obj+64),F(Data+obj+80),F(Data+obj+72),F(Data+obj+76));}
    internal string[] TrackNames(int animation) {
        var annotations=Array(animation+40,24,256);var names=new string[annotations.Count];
        for(int i=0;i<names.Length;++i){int at=annotations.At+i*24;names[i]=Text(Data+Pointer(at));Check(Array(at+8,16,256).Count==0,"Unexpected named annotation events");}
        return names;
    }
    internal sealed record Motion(Vector4 Up,Vector4 Forward,float Duration,Vector4[] Samples);
    internal Motion? ReferenceFrame(int animation) {
        if(!global.ContainsKey(animation+32))return null;int at=Reference(animation+32);Check(Objects["hkaDefaultAnimatedReferenceFrame"]==at,"Extracted motion points at the wrong native object");
        Vector4 V(int offset)=>new(F(Data+offset),F(Data+offset+4),F(Data+offset+8),F(Data+offset+12));
        var samples=Array(at+56,16,65536);
        Check(I(Data+at)==0&&I(Data+at+4)==0&&I(Data+at+8)==0&&I(Data+at+12)==0&&I(Data+at+52)==0&&I(Data+at+72)==0&&I(Data+at+76)==0,"Native reference-frame base/padding bytes differ from retail layout");
        return new(V(at+16),V(at+32),F(Data+at+48),Enumerable.Range(0,samples.Count).Select(i=>V(samples.At+i*16)).ToArray());
    }
    internal Clip Animation(int frames,int bones) {
        int obj=Objects["hkaSplineCompressedAnimation"];
        Check(I(Data+obj+12)==3&&I(Data+obj+20)==bones&&I(Data+obj+24)==0&&I(Data+obj+28)==0,"Wrong native animation type or track counts");
        Check(TrackNames(obj).Length==bones,"Native spline is missing its per-bone annotation names");
        Check(I(Data+obj+56)==frames&&I(Data+obj+60)==1&&I(Data+obj+64)==256&&I(Data+obj+68)==bones*4,"Wrong native spline block dimensions");
        float duration=F(Data+obj+16),delta=F(Data+obj+80);
        Check(Math.Abs(duration-delta*(frames-1))<.00001f&&Math.Abs(F(Data+obj+72)-255*delta)<.00001f&&Math.Abs(F(Data+obj+76)*F(Data+obj+72)-1)<.00001f,"Invalid native spline timing");
        var offsets=Array(obj+88,4,256);var floatOffsets=Array(obj+104,4,256);
        Check(offsets.Count==1&&I(Data+offsets.At)==0&&floatOffsets.Count==1&&Array(obj+120,4,256).Count==0&&Array(obj+136,4,256).Count==0&&I(Data+obj+168)==0,"Invalid native spline offset tables");
        var data=Array(obj+152,1,64000000);var raw=bytes.AsSpan(Data+data.At,data.Count).ToArray();
        using var reader=new BinaryReader(new MemoryStream(raw,false));
        void Align(){while(reader.BaseStream.Position%4!=0)Check(reader.ReadByte()==0,"Unexpected spline alignment bytes");}
        float Scalar(){float v=reader.ReadSingle();Check(float.IsFinite(v)&&Math.Abs(v)<100000,"Non-finite spline component");return v;}
        void Curve(bool align=true) {
            Check(reader.ReadUInt16()+1==frames&&reader.ReadByte()==1,"Native spline is not a linear sampled curve");
            Check(reader.ReadByte()==0,"Native spline curve missing initial clamp");
            for(int frame=0;frame<frames;++frame)Check(reader.ReadByte()==frame,"Native spline curve knot does not match its source frame");
            Check(reader.ReadByte()==frames-1,"Native spline curve missing final clamp");if(align)Align();
        }
        Vector3[] Vectors() {
            Curve();var min=new float[3];var max=new float[3];
            for(int axis=0;axis<3;++axis){min[axis]=Scalar();max[axis]=Scalar();Check(min[axis]<=max[axis],"Inverted native spline scalar bounds");}
            var values=new Vector3[frames];
            for(int frame=0;frame<frames;++frame)for(int axis=0;axis<3;++axis)values[frame][axis]=min[axis]+(max[axis]-min[axis])*reader.ReadUInt16()/65535f;
            return values;
        }
        var dynamicPosition=new bool[bones];var dynamicRotation=new bool[bones];var dynamicScale=new bool[bones];
        for(int bone=0;bone<bones;++bone) {
            Check(reader.ReadByte()==0x45,"Native spline does not use retail THREECOMP40 quantization");
            int position=reader.ReadByte(),rotation=reader.ReadByte();Check(position is 7 or 0x70&&rotation is 15 or 0xf0,"Native spline uses unexpected position/rotation masks");dynamicPosition[bone]=position==0x70;dynamicRotation[bone]=rotation==0xf0;
            int scale=reader.ReadByte();Check(scale is 7 or 0x70,"Invalid native spline scale mask");dynamicScale[bone]=scale==0x70;
        }
        Align();var poses=Enumerable.Range(0,frames).Select(_=>new Matrix4x4[bones]).ToArray();
        for(int bone=0;bone<bones;++bone) {
            var positions=dynamicPosition[bone]?Vectors():Enumerable.Repeat(new Vector3(Scalar(),Scalar(),Scalar()),frames).ToArray();Align();
            if(dynamicRotation[bone])Curve(false);var rotations=new Quaternion[frames];
            Quaternion Rotation() {
                ulong bits=0;for(int i=0;i<5;++i)bits|=(ulong)reader.ReadByte()<<(8*i);
                int largest=(int)((bits>>36)&3),component=0;var values=new float[4];
                for(int i=0;i<4;++i)if(i!=largest)values[i]=((int)((bits>>(12*component++))&4095)-2047)*.000345436f;
                values[largest]=MathF.Sqrt(Math.Max(0,1-values.Sum(v=>v*v)))*(((bits>>38)&1)==0?1:-1);
                return new(values[0],values[1],values[2],values[3]);
            }
            var firstRotation=Rotation();rotations[0]=firstRotation;
            for(int frame=1;frame<frames;++frame)rotations[frame]=dynamicRotation[bone]?Rotation():firstRotation;
            for(int frame=0;frame<frames;++frame){Check(Math.Abs(rotations[frame].LengthSquared()-1)<.00001f,"Native spline quaternion is not unit length");if(frame>0)Check(Quaternion.Dot(rotations[frame-1],rotations[frame])>=0,"Native spline quaternion signs cross hemispheres");}
            Align();Vector3[] scales;
            if(dynamicScale[bone])scales=Vectors();else{var scale=new Vector3(Scalar(),Scalar(),Scalar());scales=Enumerable.Repeat(scale,frames).ToArray();}
            Align();
            for(int frame=0;frame<frames;++frame){var s=scales[frame];Check(s.X>0&&s.Y>0&&s.Z>0,"Invalid native spline scale");poses[frame][bone]=Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(rotations[frame])*Matrix4x4.CreateTranslation(positions[frame]);}
        }
        Check(reader.BaseStream.Position==I(Data+floatOffsets.At),"Native transform block endpoint differs from float-block offset");
        while(reader.BaseStream.Position%16!=0)Check(reader.ReadByte()==0,"Unexpected native spline block padding");
        Check(reader.BaseStream.Position==raw.Length,"Native spline block contains unexpected trailing bytes");
        bool animated=poses.Skip(1).Any(p=>!p.SequenceEqual(poses[0]));var motion=ReferenceFrame(obj);
        if(!animated)Check(dynamicPosition.All(v=>!v)&&dynamicRotation.All(v=>!v),"Native static pose contains unnecessary position/rotation splines");
        Check((motion!=null)==animated,"Native reference-frame presence does not match clip motion");
        if(motion!=null)Check(motion.Up==new Vector4(0,1,0,0)&&motion.Forward==new Vector4(0,0,1,0)&&motion.Duration==duration&&motion.Samples.Length==frames&&motion.Samples.All(v=>v==Vector4.Zero),"Native reference-frame fields or zero actor-motion samples changed");
        return new(obj,frames,bones,duration,poses);
    }
    internal Matrix4x4[] Pose(Clip clip,int frame)=>clip.Poses[frame];
    internal Matrix4x4[] FractionalPose(Clip clip,float frame) {
        int first=(int)MathF.Floor(frame),last=Math.Min(first+1,clip.Frames-1);float ratio=frame-first;var result=new Matrix4x4[clip.Bones];
        for(int bone=0;bone<result.Length;++bone) {
            Check(Matrix4x4.Decompose(clip.Poses[first][bone],out var sa,out var qa,out var pa)&&Matrix4x4.Decompose(clip.Poses[last][bone],out _,out _,out _),"Cannot decompose decoded fixture spline");
            Matrix4x4.Decompose(clip.Poses[last][bone],out var sb,out var qb,out var pb);if(Quaternion.Dot(qa,qb)<0)qb=-qb;
            var q=Quaternion.Normalize(qa*(1-ratio)+qb*ratio);result[bone]=Matrix4x4.CreateScale(Vector3.Lerp(sa,sb,ratio))*Matrix4x4.CreateFromQuaternion(q)*Matrix4x4.CreateTranslation(Vector3.Lerp(pa,pb,ratio));
        }
        return result;
    }
    internal bool Binding(string skeletonName,int animation,int bones) {
        int b=Objects["hkaAnimationBinding"];var map=Array(b+32,2,256);
        Check(Text(Data+Pointer(b+16))==skeletonName&&Reference(b+24)==animation&&map.Count==bones&&bytes[Data+b+80]==0,"Invalid native animation binding");
        for(int i=0;i<bones;++i)Check(S(Data+map.At+i*2)==i,"Native animation bindings are not identity bone order");
        Check(Array(b+48,2,256).Count==0&&Array(b+64,2,256).Count==0,"Unexpected native float or partition bindings");
        int container=Objects["hkaAnimationContainer"];Check(Array(container+16,8,256).Count==0,"Clip embedded skeletons");
        var animations=Array(container+32,8,16);var bindings=Array(container+48,8,16);
        Check(animations.Count==1&&bindings.Count==1&&Reference(animations.At)==animation&&Reference(bindings.At)==b,"Container points at wrong animation/binding objects");return true;
    }
}
