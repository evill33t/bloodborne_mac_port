// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class CatacombsBridgeAnimations {
    internal sealed record Result(byte[] Bundle,float Duration,int Frames,Matrix4x4 SourceLeafReference);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs mechanism motion: "+why);}
    internal static Result Convert(string model,byte[] sourceBytes,byte[] nativeBytes) {
        Need(model is "o3001" or "o3011","unsupported source mechanism");var source=BND3.Read(sourceBytes);var native=BND4.Read(nativeBytes);
        var skeleton=source.Files.Single(f=>f.ID==1000000).Bytes.ToArray();var rig=NativeAnimation.Skeleton(native.Files.Single(f=>f.ID==1000000).Bytes.ToArray());
        var forward=SourceAnimation.Decode(skeleton,source.Files.Single(f=>f.ID==1).Bytes.ToArray());var reverse=SourceAnimation.Decode(skeleton,source.Files.Single(f=>f.ID==3).Bytes.ToArray());
        Need(forward.Bones.Select(b=>b.Name).SequenceEqual(new[]{model,model+"_00"})&&forward.Bones.Select(b=>b.Parent).SequenceEqual(new[]{-1,0}),"source two-bone hierarchy");
        int frames=model=="o3001"?141:131;Need(forward.FrameCount==frames&&reverse.FrameCount==frames&&Math.Abs(forward.Duration-(frames-1)/30f)<.00001f&&reverse.Duration==forward.Duration,"source mechanism timing");
        Need(rig.Names.SequenceEqual(new[]{"o241013","o241013_00"})&&rig.Parents.SequenceEqual(new[]{-1,0}),"native keyframed leaf hierarchy");
        var b=forward.Bones[1];var reference=Matrix4x4.CreateScale(b.Scale)*Matrix4x4.CreateFromQuaternion(b.Rotation)*Matrix4x4.CreateTranslation(b.Translation);Need(Matrix4x4.Invert(reference,out var inverse),"source reference singular");
        foreach(int id in new[]{0,1,2,3}) {
            SourceAnimation clip=id is 0 or 2?forward:id==1?forward:reverse;bool hold=id is 0 or 2;int count=hold?2:frames;var samples=new List<Matrix4x4>();
            for(int f=0;f<count;f++) {
                var pose=clip.Sample(hold?(id==0?0:clip.Duration):Math.Min(f*clip.FrameDuration,clip.Duration));
                Need(pose[0]==Matrix4x4.Identity,"source mechanism root motion");samples.Add(rig.Local[0]);samples.Add(rig.Local[1]*inverse*pose[1]);
            }
            native.Files.Single(f=>f.ID==1000000000+id).Bytes=NativeSpline.Encode(samples,2,count,1/30f,rig.Name,rig.Names);
        }
        // Object controller owns the bridge and lever timing. The source
        // one-frame bridge2 hold is reconstructed from the true forward endpoint.
        return new(native.Write(),forward.Duration,frames,reference);
    }
}
