// SPDX-License-Identifier: GPL-3.0-or-later
// Spline decompression adapted from Smithbox's SplineCompressedAnimation.cs,
// adapted to C# by Meowmaritus and Katalash from the Havok Format Library:
// https://github.com/PredatorCZ/HavokLib/blob/master/source/hkaSplineDecompressor.cpp
// Original code Copyright (C) 2016-2019 Lukas Cone.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version. This program is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details. You should have received a copy of the GNU
// General Public License along with this program. If not, see
// <https://www.gnu.org/licenses/>.
using SoulsFormats;
using System.Numerics;
using System.Text;

// Reads source animations without changing their archives. Restrict this path
// to non-additive transform-only DS1R TAG0 splines; animation event conversion
// and Bloodborne skeleton retargeting belong to the export stage.
internal sealed class SourceAnimation {
    internal sealed record Bone(string Name,int Parent,Vector3 Translation,Quaternion Rotation,Vector3 Scale);
    internal IReadOnlyList<Bone> Bones {get;}
    internal float Duration {get;}
    internal float FrameDuration {get;}
    internal int FrameCount {get;}
    readonly Track[][] blocks;
    readonly int[] trackBones;
    readonly int framesPerBlock;

    SourceAnimation(Bone[] bones,float duration,float frameDuration,int frames,int blockFrames,int[] bindings,Track[][] tracks) {
        Bones=System.Array.AsReadOnly(bones);Duration=duration;FrameDuration=frameDuration;
        FrameCount=frames;framesPerBlock=blockFrames;trackBones=bindings;blocks=tracks;
    }

    internal static SourceAnimation Load(Func<string,byte[]?> read,string model,int clipID) {
        if(model.Length!=5||model[0]!='c'||!model.AsSpan(1).ToArray().All(char.IsAsciiDigit)||clipID is <0 or >9999)
            throw new ArgumentException("Invalid source character or animation identifier");
        var bytes=read($"chr/{model}.anibnd.dcx")??read($"chr/{model}.anibnd")??throw new FileNotFoundException("Source character animation archive unavailable: "+model);
        if(bytes.Length is <1 or >128*1024*1024)throw new InvalidDataException("Source animation archive exceeds budget");
        var archive=BND3.Read(bytes);
        byte[] File(string name) {
            var files=archive.Files.Where(f=>f.Name.Replace('\\','/').Split('/').Last().Equals(name,StringComparison.OrdinalIgnoreCase)).ToArray();
            return files.Length==1?files[0].Bytes.ToArray():throw new InvalidDataException("Missing or ambiguous source animation asset: "+name);
        }
        return Decode(File("skeleton.hkx"),File($"a00_{clipID:D4}.hkx"));
    }

    internal static SourceAnimation Decode(byte[] skeletonBytes,byte[] animationBytes) {
        var skeleton=new SourceCollision.Tag(skeletonBytes);var clip=new SourceCollision.Tag(animationBytes);
        static SourceCollision.Tag.Item One(SourceCollision.Tag tag,string type) {
            var values=tag.Items.Where(i=>tag.Name(i.Type)==type&&i.Count==1).ToArray();
            return values.Length==1?values[0]:throw new InvalidDataException("Expected one source animation "+type);
        }
        var sk=One(skeleton,"hkaSkeleton");var names=skeleton.Array(sk,"bones");
        var parents=skeleton.Array(sk,"parentIndices");var reference=skeleton.Array(sk,"referencePose");
        if(names.Count is <1 or >256||parents.Count!=names.Count||reference.Count!=names.Count)
            throw new InvalidDataException("Source animation skeleton arrays do not match");
        var bones=new Bone[names.Count];var uniqueNames=new HashSet<string>(StringComparer.Ordinal);
        for(int i=0;i<bones.Length;++i) {
            var bone=skeleton.Element(names,i);var text=skeleton.Reference(skeleton.Nested(bone.Type,bone.At,"name").At);
            if(text.Count is <2 or >256||skeleton.Name(text.Type)!="char"||skeleton.Byte(text.At+text.Count-1)!=0)
                throw new InvalidDataException("Invalid source animation bone name");
            var name=new UTF8Encoding(false,true).GetString(Enumerable.Range(text.At,text.Count-1).Select(skeleton.Byte).ToArray());
            int parent=unchecked((short)skeleton.UShort(skeleton.Element(parents,i).At));
            if(name.Contains('\0')||!uniqueNames.Add(name)||parent< -1||parent>=bones.Length||parent==i)
                throw new InvalidDataException("Invalid source animation skeleton hierarchy");
            int at=skeleton.Element(reference,i).At;var p=Vector(skeleton,at);var q=Unit(new(Scalar(skeleton,at+16),Scalar(skeleton,at+20),Scalar(skeleton,at+24),Scalar(skeleton,at+28)));
            var scale=Vector(skeleton,at+32);ValidScale(scale);bones[i]=new(name,parent,p,q,scale);
        }
        for(int i=0;i<bones.Length;++i){int at=i,steps=0;while(at!=-1){if(++steps>bones.Length)throw new InvalidDataException("Cyclic source animation skeleton");at=bones[at].Parent;}}
        var anim=One(clip,"hkaSplineCompressedAnimation");var binding=One(clip,"hkaAnimationBinding");
        int Int(string name)=>clip.Int(clip.Nested(anim.Type,anim.At,name).At);
        float Float(string name)=>Scalar(clip,clip.Nested(anim.Type,anim.At,name).At);
        int frames=Int("numFrames"),count=Int("numBlocks"),blockFrames=Int("maxFramesPerBlock"),tracks=Int("numberOfTransformTracks");
        float duration=Float("duration"),delta=Float("frameDuration"),blockDuration=Float("blockDuration"),inverse=Float("blockInverseDuration");
        if(frames is <2 or >65536||count is <1 or >256||blockFrames is <2 or >256||tracks is <1 or >256||tracks>bones.Length||count*tracks>65536||
            duration<=0||duration>600||delta<.00001f||delta>1||Math.Abs(duration-delta*(frames-1))>.001f||
            Math.Abs(blockDuration-delta*(blockFrames-1))>.001f||Math.Abs(blockDuration*inverse-1)>.001f||
            count!=(frames-2)/(blockFrames-1)+1)
            throw new InvalidDataException("Invalid source animation frame or block metadata");
        if(Int("numberOfFloatTracks")!=0||Int("endian")!=0||clip.Byte(clip.Nested(binding.Type,binding.At,"blendHint").At)!=0)
            throw new NotSupportedException("Only little-endian, non-additive transform-only source splines are supported");
        var map=clip.Array(binding,"transformTrackToBoneIndices");
        if(map.Count!=tracks)throw new InvalidDataException("Source animation transform bindings do not match tracks");
        var trackBones=new int[tracks];var assigned=new HashSet<int>();
        for(int i=0;i<tracks;++i) {
            int bone=unchecked((short)clip.UShort(clip.Element(map,i).At));
            if(bone<0||bone>=bones.Length||!assigned.Add(bone))throw new InvalidDataException("Invalid or repeated source animation transform binding");
            trackBones[i]=bone;
        }
        var data=clip.Array(anim,"data");var offsets=clip.Array(anim,"blockOffsets");var floatOffsets=clip.Array(anim,"floatBlockOffsets");
        if(data.Count is <1 or >64*1024*1024||offsets.Count!=count||floatOffsets.Count!=count)
            throw new InvalidDataException("Invalid source animation compressed data arrays");
        var raw=Enumerable.Range(data.At,data.Count).Select(clip.Byte).ToArray();var starts=new int[count+1];starts[count]=raw.Length;
        for(int i=0;i<count;++i)starts[i]=clip.Int(clip.Element(offsets,i).At);
        if(starts[0]!=0)throw new InvalidDataException("Source animation first block has a nonzero offset");
        var decoded=new Track[count][];
        for(int i=0;i<count;++i) {
            int start=starts[i],end=starts[i+1],floatEnd=clip.Int(clip.Element(floatOffsets,i).At);
            if(start<0||end<=start||end>raw.Length||start%16!=0||floatEnd<0||floatEnd>end-start)
                throw new InvalidDataException("Invalid source animation block range");
            int lastFrame=Math.Min(blockFrames-1,frames-1-i*(blockFrames-1));
            var reader=new Reader(raw.AsMemory(start,end-start));
            decoded[i]=ReadBlock(reader,tracks,lastFrame);
            if(reader.At!=floatEnd)throw new InvalidDataException("Source animation transform data does not end at its float block offset");
            reader.Align(16);if(reader.At!=end-start)throw new InvalidDataException("Unexpected trailing source animation block data");
        }
        return new(bones,duration,delta,frames,blockFrames,trackBones,decoded);
    }

    internal Matrix4x4[] Sample(float time) {
        if(!float.IsFinite(time)||time<0||time>Duration+.0001f)throw new ArgumentOutOfRangeException(nameof(time),"Source animation sample time is outside the clip");
        // Blocks overlap by one frame. Clamp the final endpoint explicitly:
        // modulo here would sample the beginning when exporting the last pose.
        float frame=time>=Duration?FrameCount-1:Math.Min(time/FrameDuration,FrameCount-1);
        int block=Math.Min((int)(frame/(framesPerBlock-1)),blocks.Length-1);
        float localFrame=frame-block*(framesPerBlock-1);var result=new Matrix4x4[Bones.Count];
        for(int i=0;i<Bones.Count;++i){var b=Bones[i];result[i]=Matrix(b.Translation,b.Rotation,b.Scale);}
        for(int i=0;i<trackBones.Length;++i) {
            var b=Bones[trackBones[i]];var track=blocks[block][i];
            var p=track.Position.Value(localFrame,b.Translation);var q=track.Rotation?.Value(localFrame)??b.Rotation;var s=track.Scale.Value(localFrame,b.Scale);
            ValidScale(s);result[trackBones[i]]=Matrix(p,q,s);
        }
        return result;
    }

    static Matrix4x4 Matrix(Vector3 p,Quaternion q,Vector3 s)=>Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(q)*Matrix4x4.CreateTranslation(p);
    static float Valid(float value)=>float.IsFinite(value)&&Math.Abs(value)<=1000000?value:throw new InvalidDataException("Non-finite or unbounded source animation component");
    static float Scalar(SourceCollision.Tag tag,int at)=>Valid(BitConverter.UInt32BitsToSingle(tag.UInt(at)));
    static Vector3 Vector(SourceCollision.Tag tag,int at)=>new(Scalar(tag,at),Scalar(tag,at+4),Scalar(tag,at+8));
    static Quaternion Unit(Quaternion q) {
        float length=q.LengthSquared();if(!float.IsFinite(length)||Math.Abs(length-1)>.005f)throw new InvalidDataException("Invalid source animation quaternion");
        return Quaternion.Normalize(q);
    }
    static void ValidScale(Vector3 s){if(!float.IsFinite(s.LengthSquared())||Math.Abs(s.X)<.000001f||Math.Abs(s.Y)<.000001f||Math.Abs(s.Z)<.000001f||Math.Abs(s.X)>10000||Math.Abs(s.Y)>10000||Math.Abs(s.Z)>10000)throw new InvalidDataException("Invalid source animation scale");}

    sealed class Reader(ReadOnlyMemory<byte> raw) {
        internal int At {get;private set;}
        ReadOnlySpan<byte> Take(int count){if(count<0||At>raw.Length-count)throw new InvalidDataException("Truncated source animation block");var value=raw.Span.Slice(At,count);At+=count;return value;}
        internal byte Byte()=>Take(1)[0];
        internal short Short()=>System.Buffers.Binary.BinaryPrimitives.ReadInt16LittleEndian(Take(2));
        internal ushort UShort()=>System.Buffers.Binary.BinaryPrimitives.ReadUInt16LittleEndian(Take(2));
        internal float Float()=>Valid(BitConverter.Int32BitsToSingle(System.Buffers.Binary.BinaryPrimitives.ReadInt32LittleEndian(Take(4))));
        internal void Align(int alignment){int next=(At+alignment-1)/alignment*alignment;_ = Take(next-At);}
        internal Quaternion Quaternion(int quant) {
            int size=quant switch{1=>5,2=>6,5=>16,_=>throw new NotSupportedException("Unsupported source quaternion quantization: "+quant)};
            Align(quant switch{2=>2,5=>4,_=>1});
            if(quant==5)return Unit(new(Float(),Float(),Float(),Float()));
            var values=new float[4];int omitted;bool negative;
            if(size==5) {
                ulong packed=0;for(int i=0;i<5;++i)packed|=(ulong)Byte()<<(8*i);
                omitted=(int)((packed>>36)&3);negative=((packed>>38)&1)!=0;int component=0;
                for(int i=0;i<4;++i)if(i!=omitted)values[i]=((int)((packed>>(12*component++))&4095)-2047)*.000345436f;
            } else {
                ushort x=UShort(),y=UShort(),z=UShort();omitted=((y>>14)&2)|((x>>15)&1);negative=(z&0x8000)!=0;
                var packed=new[]{x,y,z};int component=0;
                for(int i=0;i<4;++i)if(i!=omitted)values[i]=((packed[component++]&32767)-16383)*.000043161f;
            }
            values[omitted]=MathF.Sqrt(Math.Max(0,1-values.Sum(v=>v*v)))*(negative?-1:1);
            return Unit(new(values[0],values[1],values[2],values[3]));
        }
    }
    sealed record Curve(int Degree,byte[] Knots,int Count) {
        internal static Curve Read(Reader r,int lastFrame) {
            int count=r.Short()+1,degree=r.Byte();
            if(degree is <1 or >4||count<degree+1||count>260)throw new InvalidDataException("Invalid source animation spline control count or degree");
            var knots=new byte[count+degree+1];for(int i=0;i<knots.Length;++i)knots[i]=r.Byte();
            for(int i=1;i<knots.Length;++i)if(knots[i]<knots[i-1])throw new InvalidDataException("Unordered source animation spline knots");
            if(knots[degree]!=0||knots[count]!=lastFrame)throw new InvalidDataException("Source animation spline knot domain does not match its block");
            return new(degree,knots,count);
        }
        internal (int Span,float[] Weights) Weights(float frame) {
            if(frame<Knots[Degree]||frame>Knots[Count]+.001f)throw new InvalidDataException("Source animation sample exceeds spline domain");
            int low=Degree,high=Count;
            if(frame>=Knots[Count])low=Count-1;
            else while(high-low>1){int mid=(low+high)/2;if(frame<Knots[mid])high=mid;else low=mid;}
            var n=new float[Degree+1];n[0]=1;
            for(int i=1;i<=Degree;++i)for(int j=i-1;j>=0;--j) {
                int denominator=Knots[low+i-j]-Knots[low-j];
                if(denominator<=0)throw new InvalidDataException("Degenerate source animation spline span");
                float a=(frame-Knots[low-j])/denominator,tmp=n[j]*a;n[j+1]+=n[j]-tmp;n[j]=tmp;
            }
            return(low,n);
        }
        internal float Value(float frame,float[] values){var(span,weights)=Weights(frame);float result=0;for(int i=0;i<=Degree;++i)result+=values[span-i]*weights[i];return Valid(result);}
    }
    sealed record VectorTrack(Curve? Curve,float[]?[] Channels) {
        internal Vector3 Value(float frame,Vector3 reference) {
            for(int axis=0;axis<3;++axis)if(Channels[axis] is {} values)reference[axis]=values.Length==1?values[0]:Curve!.Value(frame,values);
            return reference;
        }
    }
    sealed record RotationTrack(Curve? Curve,Quaternion[] Values) {
        internal Quaternion Value(float frame) {
            if(Curve==null)return Values[0];var(span,weights)=Curve.Weights(frame);var result=new Quaternion();
            for(int i=0;i<=Curve.Degree;++i)result+=Values[span-i]*weights[i];
            // Spline quaternion components are evaluated together, then made
            // unit length; linear quaternion splines are not quaternion SLERP.
            float length=result.LengthSquared();if(!float.IsFinite(length)||length<.000001f)throw new InvalidDataException("Degenerate source animation spline quaternion");
            return Quaternion.Normalize(result);
        }
    }
    sealed record Track(VectorTrack Position,RotationTrack? Rotation,VectorTrack Scale);
    static Track[] ReadBlock(Reader r,int count,int lastFrame) {
        var masks=new (byte Quant,byte P,byte Q,byte S)[count];
        for(int i=0;i<count;++i) {
            var m=(Quant:r.Byte(),P:r.Byte(),Q:r.Byte(),S:r.Byte());
            if((m.P&0x88)!=0||(m.S&0x88)!=0||((m.P&7)&(m.P>>4))!=0||((m.S&7)&(m.S>>4))!=0||
                (m.Quant&3)>1||(m.Quant>>6)>1||((m.Quant>>2)&15) is not (1 or 2 or 5))throw new NotSupportedException("Unsupported source animation track mask");
            masks[i]=m;
        }
        r.Align(4);var tracks=new Track[count];
        VectorTrack Vector(byte mask,int quant) {
            Curve? curve=(mask&0x70)!=0?Curve.Read(r,lastFrame):null;if(curve!=null)r.Align(4);
            var channels=new float[]?[3];var min=new float[3];var max=new float[3];
            for(int axis=0;axis<3;++axis) {
                if((mask&(16<<axis))!=0){min[axis]=r.Float();max[axis]=r.Float();if(min[axis]>max[axis])throw new InvalidDataException("Inverted source animation spline bounds");channels[axis]=new float[curve!.Count];}
                else if((mask&(1<<axis))!=0)channels[axis]=[r.Float()];
            }
            if(curve!=null)for(int n=0;n<curve.Count;++n)for(int axis=0;axis<3;++axis)if((mask&(16<<axis))!=0) {
                float ratio=quant==0?r.Byte()/255f:r.UShort()/65535f;channels[axis]![n]=Valid(min[axis]+(max[axis]-min[axis])*ratio);
            }
            return new(curve,channels);
        }
        for(int i=0;i<count;++i) {
            var m=masks[i];var p=Vector(m.P,m.Quant&3);r.Align(4);RotationTrack? q=null;
            if((m.Q&0xf0)!=0) {
                var curve=Curve.Read(r,lastFrame);var values=new Quaternion[curve.Count];for(int n=0;n<values.Length;++n)values[n]=r.Quaternion((m.Quant>>2)&15);q=new(curve,values);
            } else if(m.Q!=0)q=new(null,[r.Quaternion((m.Quant>>2)&15)]);
            r.Align(4);var s=Vector(m.S,m.Quant>>6);r.Align(4);tracks[i]=new(p,q,s);
        }
        return tracks;
    }
}
