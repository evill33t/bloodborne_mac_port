// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

// Object animations have their own small rig and do not use a character graph.
internal static class DoorAnimations {
    internal record Result(byte[] Bundle,float Duration,int Frames);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Door animation: "+why);}
    internal static Result Convert(byte[] sourceBundle,byte[] nativeBundle) {
        var source=BND3.Read(sourceBundle);var native=BND4.Read(nativeBundle);
        var skeleton=source.Files.Single(f=>f.ID==1000000).Bytes.ToArray();
        var open=SourceAnimation.Decode(skeleton,source.Files.Single(f=>f.ID==0).Bytes.ToArray());
        var close=SourceAnimation.Decode(skeleton,source.Files.Single(f=>f.ID==1).Bytes.ToArray());
        var rig=NativeAnimation.Skeleton(native.Files.Single(f=>f.ID==1000000).Bytes.ToArray());
        Need(open.Bones.Select(b=>b.Name).SequenceEqual(new[]{"o1305","Dummy","o1305_00","o1305_01"})&&open.Bones.Select(b=>b.Parent).SequenceEqual(new[]{-1,0,0,0}),"source swinging door rig changed");
        Need(rig.Names.SequenceEqual(new[]{"o241013","o241013_00"})&&rig.Parents.SequenceEqual(new[]{-1,0}),"native single-leaf rig changed");
        Need(open.FrameCount==91&&close.FrameCount==91&&Math.Abs(open.Duration-3)<.0001f&&Math.Abs(close.Duration-3)<.0001f,"source door timing changed");
        var reference=open.Bones.Select(b=>Matrix4x4.CreateScale(b.Scale)*Matrix4x4.CreateFromQuaternion(b.Rotation)*Matrix4x4.CreateTranslation(b.Translation)).ToArray();
        var inverse=Matrix4x4.Identity;Need(Matrix4x4.Invert(reference[3],out inverse),"singular source leaf bind");
        Matrix4x4[] Pose(SourceAnimation clip,float time) {
            var pose=clip.Sample(time);
            Need(Vector3.Distance(pose[0].Translation,Vector3.Zero)<.0001f&&Vector3.Distance(pose[3].Translation,Vector3.Zero)<.0001f,"source door translates its hinge");
            return [rig.Local[0],rig.Local[1]*inverse*pose[3]];
        }
        // Native ObjAct2420010 plays1. Keep0 as a closed hold and3 as an
        // open hold; the event restores the end of1 after a map reload.
        foreach(var (id,clip,hold) in new[]{(0,open,0),(1,open,-1),(2,close,-1),(3,open,1)}) {
            int frames=hold<0?clip.FrameCount:2;var samples=new List<Matrix4x4>(frames*2);
            for(int f=0;f<frames;f++)samples.AddRange(Pose(clip,hold<0?Math.Min(f*clip.FrameDuration,clip.Duration):hold==0?0:clip.Duration));
            var file=native.Files.Single(f=>f.ID==1000000000+id);
            file.Bytes=NativeSpline.Encode(samples,2,frames,clip.FrameDuration,rig.Name,rig.Names);
            var parsed=new NativeAnimation.Pack(file.Bytes.ToArray());int animation=parsed.Object("hkaSplineCompressedAnimation");
            Need(parsed.Int(animation+20)==2&&parsed.Int(animation+56)==frames,"native door clip did not round-trip");
        }
        Need(native.Files.Count==6&&native.Files.Single(f=>f.ID==3000000).Bytes.Span.SequenceEqual(BND4.Read(nativeBundle).Files.Single(f=>f.ID==3000000).Bytes.Span),"native object TAE changed");
        return new(native.Write(),open.Duration,open.FrameCount);
    }
}
