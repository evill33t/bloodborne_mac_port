// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text;

// Match the spline-compressed animation class used by the native clip bank.
// Encode sampled source motion as clamped degree-one splines using the retail
// THREECOMP40 quaternion and sixteen-bit translation/scale quantization profile.
internal static class NativeSpline {
    internal static byte[] Encode(IReadOnlyList<Matrix4x4> samples,int trackCount,int frameCount,float frameDuration,string skeletonName,IReadOnlyList<string> trackNames) {
        if(trackCount is <1 or >256||frameCount is <2 or >256||samples.Count!=trackCount*frameCount||
            !float.IsFinite(frameDuration)||frameDuration is <.00001f or >1||string.IsNullOrEmpty(skeletonName)||skeletonName.Length>128||skeletonName.Any(char.IsControl)||
            trackNames.Count!=trackCount||trackNames.Any(n=>string.IsNullOrEmpty(n)||n.Length>256||n.Any(char.IsControl))||trackNames.Distinct(StringComparer.Ordinal).Count()!=trackCount)
            throw new InvalidDataException("Native spline dimensions, timing or skeleton name are invalid");
        var positions=new Vector3[trackCount][];var rotations=new Quaternion[trackCount][];var scales=new Vector3[trackCount][];
        var positionSpline=new bool[trackCount];var rotationSpline=new bool[trackCount];var scaleSpline=new bool[trackCount];
        bool animated=false;
        bool Finite(Vector3 v)=>float.IsFinite(v.X)&&float.IsFinite(v.Y)&&float.IsFinite(v.Z)&&Math.Abs(v.X)<=100000&&Math.Abs(v.Y)<=100000&&Math.Abs(v.Z)<=100000;
        for(int track=0;track<trackCount;++track) {
            positions[track]=new Vector3[frameCount];rotations[track]=new Quaternion[frameCount];scales[track]=new Vector3[frameCount];
            for(int frame=0;frame<frameCount;++frame) {
                var m=samples[frame*trackCount+track];
                if(!float.IsFinite(m.M14)||!float.IsFinite(m.M24)||!float.IsFinite(m.M34)||!float.IsFinite(m.M44)||Math.Abs(m.M14)>.000001f||Math.Abs(m.M24)>.000001f||Math.Abs(m.M34)>.000001f||Math.Abs(m.M44-1)>.000001f||
                    !Matrix4x4.Decompose(m,out var s,out var q,out var p)||!Finite(p)||!Finite(s)||s.X<=0||s.Y<=0||s.Z<=0||!float.IsFinite(q.LengthSquared())||Math.Abs(q.LengthSquared()-1)>.01f)
                    throw new InvalidDataException("Native spline contains an unrepresentable sampled transform");
                q=Quaternion.Normalize(q);if(frame>0&&Quaternion.Dot(rotations[track][frame-1],q)<0)q=-q;
                positions[track][frame]=p;rotations[track][frame]=q;scales[track][frame]=s;
                if(frame>0&&Vector3.DistanceSquared(positions[track][0],p)>1e-12f)positionSpline[track]=true;
                if(frame>0&&(rotations[track][0]-q).LengthSquared()>1e-12f)rotationSpline[track]=true;
                if(frame>0&&Vector3.DistanceSquared(scales[track][0],s)>1e-12f)scaleSpline[track]=true;
            }
            animated|=positionSpline[track]||rotationSpline[track]||scaleSpline[track];
        }
        using var stream=new MemoryStream();using var writer=new BinaryWriter(stream,Encoding.UTF8,true);
        void Align(int alignment){while(stream.Position%alignment!=0)writer.Write((byte)0);}
        void Curve() {
            writer.Write((ushort)(frameCount-1));writer.Write((byte)1);
            writer.Write((byte)0);for(int frame=0;frame<frameCount;++frame)writer.Write((byte)frame);writer.Write((byte)(frameCount-1));
        }
        void VectorCurve(Vector3[] points) {
            Curve();Align(4);var min=points.Aggregate(Vector3.Min);var max=points.Aggregate(Vector3.Max);
            for(int axis=0;axis<3;++axis){writer.Write(min[axis]);writer.Write(max[axis]);}
            foreach(var point in points)for(int axis=0;axis<3;++axis) {
                float width=max[axis]-min[axis];float ratio=width==0?0:(point[axis]-min[axis])/width;
                writer.Write((ushort)Math.Clamp((int)MathF.Round(ratio*65535),0,65535));
            }
        }
        void StaticVector(Vector3 value){writer.Write(value.X);writer.Write(value.Y);writer.Write(value.Z);}
        void Rotation(Quaternion q) {
            var values=new[]{q.X,q.Y,q.Z,q.W};int omitted=0;
            for(int i=1;i<4;++i)if(Math.Abs(values[i])>Math.Abs(values[omitted]))omitted=i;
            ulong packed=((ulong)omitted<<36)|(values[omitted]<0?1UL<<38:0);int component=0;
            for(int i=0;i<4;++i)if(i!=omitted) {
                int quantized=Math.Clamp((int)MathF.Round(values[i]/.000345436f+2047),0,4095);
                packed|=(ulong)quantized<<(12*component++);
            }
            for(int i=0;i<5;++i)writer.Write((byte)(packed>>(8*i)));
        }
        for(int track=0;track<trackCount;++track){writer.Write((byte)0x45);writer.Write((byte)(positionSpline[track]?0x70:0x07));writer.Write((byte)(rotationSpline[track]?0xf0:0x0f));writer.Write((byte)(scaleSpline[track]?0x70:0x07));}
        Align(4);
        for(int track=0;track<trackCount;++track) {
            if(positionSpline[track])VectorCurve(positions[track]);else StaticVector(positions[track][0]);
            Align(4);
            // THREECOMP40 is byte aligned, including immediately after a spline
            // header. Adding float alignment here corrupts121/181-frame clips.
            if(rotationSpline[track]){Curve();foreach(var q in rotations[track])Rotation(q);}else Rotation(rotations[track][0]);
            Align(4);
            if(scaleSpline[track])VectorCurve(scales[track]);else StaticVector(scales[track][0]);
            Align(4);
        }
        uint transformEnd=checked((uint)stream.Length);Align(16);var raw=stream.ToArray();float blockDuration=255*frameDuration,duration=(frameCount-1)*frameDuration;
        var animation=new NativeSplinePacking.hkaSplineCompressedAnimation {
            m_type=HKX2.AnimationType.HK_SPLINE_COMPRESSED_ANIMATION,m_duration=duration,m_numberOfTransformTracks=trackCount,m_numberOfFloatTracks=0,
            m_annotationTracks=trackNames.Select(n=>new HKX2.hkaAnnotationTrack{m_trackName=n,m_annotations=[]}).ToList(),
            // Retain the native animated-reference-frame structure while keeping
            // source bone motion in the pose; this adapter adds no actor travel.
            m_extractedMotion=animated?new NativeSplinePacking.hkaDefaultAnimatedReferenceFrame{m_up=new(0,1,0,0),m_forward=new(0,0,1,0),m_duration=duration,m_referenceFrameSamples=Enumerable.Repeat(Vector4.Zero,frameCount).ToList()}:null,
            m_numFrames=frameCount,m_numBlocks=1,m_maxFramesPerBlock=256,m_maskAndQuantizationSize=trackCount*4,
            m_blockDuration=blockDuration,m_blockInverseDuration=1/blockDuration,m_frameDuration=frameDuration,
            m_blockOffsets=[0],m_floatBlockOffsets=[transformEnd],m_transformOffsets=[],m_floatOffsets=[],m_data=raw.ToList(),m_endian=0
        };
        var binding=new AnimationPacking.hkaAnimationBinding {m_originalSkeletonName=skeletonName,m_animation=animation,m_transformTrackToBoneIndices=Enumerable.Range(0,trackCount).Select(i=>(short)i).ToList(),m_floatTrackToFloatSlotIndices=[],m_partitionIndices=[],m_blendHint=HKX2.BlendHint.NORMAL};
        var container=new HKX2.hkaAnimationContainer {m_skeletons=[],m_animations=[animation],m_bindings=[binding],m_attachments=[],m_skins=[]};
        var root=new HKX2.hkRootLevelContainer {m_namedVariants=[new(){m_name="Merged Animation Container",m_className="hkaAnimationContainer",m_variant=container}]};
        using var output=new MemoryStream();var nativeWriter=new BinaryWriterEx(false,output);new HKX2.PackFileSerializer().Serialize(root,nativeWriter);var bytes=output.ToArray();bytes[18]=1;return bytes;
    }
}

namespace NativeSplinePacking {
    // Retail PS4 packs place up/forward at16/32, duration at48 and the sample
    // array at56. HKX2's generated base writers add two absent eight-byte fields.
    internal sealed class hkaDefaultAnimatedReferenceFrame : HKX2.hkaDefaultAnimatedReferenceFrame {
        public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b) {
            b.WriteUInt64(0);b.WriteUInt64(0);s.WriteVector4(b,m_up);s.WriteVector4(b,m_forward);
            b.WriteSingle(m_duration);b.WriteUInt32(0);s.WriteVector4Array(b,m_referenceFrameSamples);b.WriteUInt64(0);
        }
    }
    // Bloodborne's GCC/Clang layout reuses hkReferencedObject's four bytes of
    // tail padding for m_type. HKX2's stock writer uses the MSVC field offsets.
    // Subsequent pointers and spline fields align identically, so only this
    // explicit animation header differs from the stock class writer.
    internal sealed class hkaSplineCompressedAnimation : HKX2.hkaSplineCompressedAnimation {
        public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b) {
            b.WriteUInt64(0);b.WriteUInt32(0);b.WriteInt32((int)m_type);
            b.WriteSingle(m_duration);b.WriteInt32(m_numberOfTransformTracks);b.WriteInt32(m_numberOfFloatTracks);b.WriteUInt32(0);
            s.WriteClassPointer(b,m_extractedMotion);s.WriteClassArray(b,m_annotationTracks);
            b.WriteInt32(m_numFrames);b.WriteInt32(m_numBlocks);b.WriteInt32(m_maxFramesPerBlock);b.WriteInt32(m_maskAndQuantizationSize);
            b.WriteSingle(m_blockDuration);b.WriteSingle(m_blockInverseDuration);b.WriteSingle(m_frameDuration);b.WriteUInt32(0);
            s.WriteUInt32Array(b,m_blockOffsets);s.WriteUInt32Array(b,m_floatBlockOffsets);s.WriteUInt32Array(b,m_transformOffsets);s.WriteUInt32Array(b,m_floatOffsets);s.WriteByteArray(b,m_data);
            b.WriteInt32(m_endian);b.WriteUInt32(0);
        }
    }
}
