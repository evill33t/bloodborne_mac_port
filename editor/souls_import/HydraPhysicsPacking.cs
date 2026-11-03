// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

// Dedicated serializers for the shipped 2014 packfile ABI. Source TAG0 pointer
// slots are never copied into the native scene. Plain hinges are supported by
// native c1130/c3060/c4520/c5510/c7000/c8030, not substituted with limited hinges.
internal static class HydraPhysicsPacking {
    internal static void Need(bool value,string why) {if(!value)throw new InvalidDataException("Hydra native physics: "+why);}
    internal static void F(byte[] raw,int at,float value) {Need(float.IsFinite(value),"nonfinite field");BitConverter.GetBytes(value).CopyTo(raw,at);}
    internal static void V(byte[] raw,int at,Vector4 value) {F(raw,at,value.X);F(raw,at+4,value.Y);F(raw,at+8,value.Z);F(raw,at+12,value.W);}
    internal static void U16(byte[] raw,int at,ushort value)=>BitConverter.GetBytes(value).CopyTo(raw,at);
    internal static void I32(byte[] raw,int at,int value)=>BitConverter.GetBytes(value).CopyTo(raw,at);
    internal static void Half(byte[] raw,int at,float value) {Need(float.IsFinite(value)&&value>=0&&value<=1000,"invalid half coefficient");U16(raw,at,(ushort)(BitConverter.SingleToUInt32Bits(value)>>16));}
    internal static byte[] Qs(Matrix4x4 matrix)=>HollowDeathRig.QsBytes(matrix);
    static void Frame(byte[] raw,int at,Matrix4x4 m) {
        // hkTransform SIMD W lanes are padding. They are not logical matrix W.
        V(raw,at,new(m.M11,m.M12,m.M13,0));V(raw,at+16,new(m.M21,m.M22,m.M23,0));
        V(raw,at+32,new(m.M31,m.M32,m.M33,0));V(raw,at+48,new(m.M41,m.M42,m.M43,0));
    }
    static void Ball(byte[] raw,int at,HydraPhysicsSource.BallSocket b) {
        U16(raw,at,5);raw[at+2]=b.SolvingMethod;raw[at+3]=b.BodiesToNotify;raw[at+4]=b.VelocityStabilizationFactor;
        raw[at+5]=b.LimitLinearImpulse?(byte)1:(byte)0;F(raw,at+8,b.BreachImpulse);F(raw,at+12,b.InertiaStabilizationFactor);
    }
    internal static byte[] ConstraintBytes(HydraPhysicsSource.Constraint c) {
        bool hinge=c.Type=="hkpHingeConstraintData";
        Need(hinge&&c.FreeRotationAxis is <3&&c.Motors==null&&c.Friction==null&&c.Limits.Count==0||
            c.Type=="hkpRagdollConstraintData"&&c.FreeRotationAxis==null&&c.Motors!=null&&c.Friction!=null&&c.Limits.Count==3,"unsupported joint contract");
        var raw=new byte[hinge?224:416];U16(raw,32,2);Frame(raw,48,c.FrameA);Frame(raw,112,c.FrameB);
        U16(raw,176,23);raw[178]=c.Stabilization.Enabled?(byte)1:(byte)0;F(raw,180,c.Stabilization.MaxLinearImpulse);
        F(raw,184,c.Stabilization.MaxAngularImpulse);F(raw,188,c.Stabilization.MaxAngle);
        if(hinge) {U16(raw,192,12);raw[194]=c.FreeRotationAxis!.Value;Ball(raw,208,c.BallSocket);return raw;}
        var motor=c.Motors!;U16(raw,192,19);raw[194]=motor.Enabled?(byte)1:(byte)0;
        // 2015 initialized/previous-angle runtime offsets are absent in 2014.
        Frame(raw,208,motor.Target);Array.Clear(raw,256,16);
        var friction=c.Friction!;U16(raw,288,17);raw[290]=friction.Enabled;raw[291]=friction.FirstAxis;raw[292]=friction.Axes;F(raw,296,friction.MaxTorque);
        for(int i=0;i<3;i++) {
            var limit=c.Limits[i];int at=304+i*32;
            Need(limit.Type==(i==0?"hkpTwistLimitConstraintAtom":"hkpConeLimitConstraintAtom")&&limit.Damping==1&&limit.AngleOffset<=byte.MaxValue,"unsupported source angular limit/damping");
            U16(raw,at,(ushort)(i==0?15:16));raw[at+2]=limit.Enabled?(byte)1:(byte)0;raw[at+3]=limit.AxisA;raw[at+4]=limit.AxisB;
            if(i>0){raw[at+5]=limit.MeasurementMode;raw[at+6]=(byte)limit.AngleOffset;}
            F(raw,at+8,limit.Min);F(raw,at+12,limit.Max);F(raw,at+16,limit.Tau);
        }
        Ball(raw,400,c.BallSocket);return raw;
    }
    internal sealed class hkpHingeConstraintData(byte[] raw):HKX2.hkpHingeConstraintData {
        public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b) {Need(raw.Length==224,"plain hinge size");b.WriteBytes(raw);}
    }
    internal sealed class hkpRagdollConstraintData(byte[] raw,IReadOnlyList<HKX2.hkpConstraintMotor?> motors):HKX2.hkpRagdollConstraintData {
        public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b) {
            Need(raw.Length==416&&motors.Count==3,"ragdoll motor contract");b.WriteBytes(raw.AsSpan(0,256).ToArray());
            foreach(var motor in motors)s.WriteClassPointer(b,motor);b.WriteBytes(raw.AsSpan(280).ToArray());
        }
    }
    internal sealed record SkeletonInfo(string Name,string[] Names,short[] Parents,bool[] Locked,Matrix4x4[] Local);
    internal sealed class hkaSkeleton(SkeletonInfo info):HKX2.hkaSkeleton {
        internal readonly SkeletonInfo Info=info;
        public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b) {
            b.WriteUInt64(0);b.WriteUInt64(0);s.WriteStringPointer(b,Info.Name);
            s.WriteUInt16Array(b,Info.Parents.Select(p=>unchecked((ushort)p)).ToList());
            s.WriteClassArray(b,Info.Names.Select((n,i)=>new HKX2.hkaBone{m_name=n,m_lockTranslation=Info.Locked[i]}).ToList());
            s.WriteClassArray(b,Info.Local.Select(m=>new Transform(Qs(m))).ToList());for(int i=0;i<4;i++)s.WriteUInt32Array(b,[]);
        }
    }
    internal sealed class Transform(byte[] raw):HKX2.IHavokObject {
        public uint Signature=>0;public void Read(HKX2.PackFileDeserializer s,BinaryReaderEx b)=>throw new NotSupportedException();
        public void Write(HKX2.PackFileSerializer s,BinaryWriterEx b)=>b.WriteBytes(raw);
    }
    internal sealed record Simple(short A,short B,Matrix4x4 Matrix):HKX2.IHavokObject {
        public uint Signature=>0;public void Read(HKX2.PackFileDeserializer s,BinaryReaderEx b)=>throw new NotSupportedException();
        public void Write(HKX2.PackFileSerializer s,BinaryWriterEx b) {b.WriteInt16(A);b.WriteInt16(B);b.WriteBytes(new byte[12]);b.WriteBytes(Qs(Matrix));}
    }
    internal sealed class hkaSkeletonMapper(hkaSkeleton a,hkaSkeleton z,IReadOnlyList<Simple> simple,short[] unmapped,Matrix4x4? extracted):HKX2.hkaSkeletonMapper {
        public override void Write(HKX2.PackFileSerializer s,BinaryWriterEx b) {
            b.WriteUInt64(0);b.WriteUInt64(0);s.WriteClassPointer(b,a);s.WriteClassPointer(b,z);for(int i=0;i<3;i++)s.WriteUInt32Array(b,[]);
            s.WriteClassArray(b,simple.ToList());s.WriteUInt32Array(b,[]);s.WriteUInt16Array(b,unmapped.Select(i=>unchecked((ushort)i)).ToList());
            // Both shipped Hydra mappings have the explicitly unused zero QS.
            b.WriteBytes(extracted.HasValue?Qs(extracted.Value):new byte[48]);b.WriteBoolean(true);b.WriteBytes(new byte[15]);
        }
    }
}
