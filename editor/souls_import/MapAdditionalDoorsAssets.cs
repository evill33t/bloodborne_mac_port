// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

// Griggs' source o1317 reuses the authored o1315 rig. It is a two-second,
// single-leaf mechanism, distinct from Havel's larger three-second o1305.
internal static class MapAdditionalDoorsAssets {
    internal sealed record Result(byte[] Bundle,DoorPhysics.Box Box,float Duration,int Frames,string SourceSHA256);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Additional door asset: "+why);}
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{Vector3.Zero,Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Max(v=>Vector3.Distance(Vector3.Transform(v,a),Vector3.Transform(v,b)));
    internal static Result Convert(byte[] sourceBytes,byte[] nativeBytes,FLVER2 surface,Func<IFlverMaterial,FLVER2.Material> materialFactory) {
        string sha=System.Convert.ToHexStringLower(SHA256.HashData(sourceBytes));Need(sha=="4ef9112da461113b68a27b0d9aa0cb85b43efc3d59e542b548d917d9a66c368e","unverified source o1317");
        var source=BND3.Read(sourceBytes);var native=BND4.Read(nativeBytes);Need(native.Files.Select(f=>f.ID).SequenceEqual(new[]{200,300,400}),"native animated object closure");
        var authored=FLVER2.Read(source.Files.Single(f=>f.ID==200).Bytes);var donor=FLVER2.Read(native.Files.Single(f=>f.ID==200).Bytes);
        Need(authored.Nodes.Select(n=>n.Name).SequenceEqual(new[]{"dummy","o1315_00","o1315"})&&authored.Nodes.All(n=>n.ParentIndex==-1)&&donor.Nodes.Select(n=>n.Name).SequenceEqual(new[]{"o241013","o241013_00"}),"source/native rigid visual nodes");
        var visual=MapBreakables.RetargetVisual(authored,donor,surface,"DS1R",Vector3.One,new Dictionary<int,int>{{0,0},{1,1},{2,0}},materialFactory);
        foreach(var dummy in visual.Dummies){var original=authored.Dummies.Single(d=>d.ReferenceID==dummy.ReferenceID);Need(original.AttachBoneIndex==-1&&original.ParentBoneIndex is 0 or 1,"source action marker binding");dummy.Position=original.Position;dummy.Forward=original.Forward;dummy.Upward=original.Upward;dummy.ParentBoneIndex=original.ParentBoneIndex;}
        native.Files.Single(f=>f.ID==200).Bytes=visual.Write();
        var physics=source.Files.Single(f=>f.ID==300).Bytes.ToArray();var tag=new SourceCollision.Tag(physics);var bodies=tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type)=="hkpRigidBody").ToArray();Need(bodies.Length==1,"source physical body population");var body=bodies[0];
        var text=tag.Reference(tag.Nested(body.Type,body.At,"name").At);Need(SourceAnimation.DecodeBoneName(physics.AsSpan(text.At,text.Count-1).ToArray())=="o1315_00","source leaf/body identity");
        var translated=tag.Reference(tag.Nested(body.Type,body.At,"collidable","shape").At);Need(tag.Name(translated.Type)=="hkpConvexTranslateShape","source leaf translation");var shape=tag.Reference(tag.Nested(translated.Type,translated.At,"childShape","childShape").At);Need(tag.Name(shape.Type)=="hkpBoxShape","source convex box");
        var box=new DoorPhysics.Box(tag.Vector(tag.Nested(shape.Type,shape.At,"halfExtents").At),tag.Vector(tag.Nested(translated.Type,translated.At,"translation").At),BitConverter.ToSingle(physics,tag.Nested(shape.Type,shape.At,"radius").At));
        Need(Vector3.Distance(box.Half,new(.47643387f,1.0200816f,.012505652f))<.00001f&&Vector3.Distance(box.Center,new(.48892844f,1.0325873f,-.025014035f))<.00001f&&Math.Abs(box.Radius-.012505652f)<.000001f,"source leaf box dimensions");
        native.Files.Single(f=>f.ID==300).Bytes=DoorPhysics.Fit(native.Files.Single(f=>f.ID==300).Bytes.ToArray(),box);
        var sourceAnims=BND3.Read(source.Files.Single(f=>f.ID==400).Bytes);var nativeAnims=BND4.Read(native.Files.Single(f=>f.ID==400).Bytes);var rig=NativeAnimation.Skeleton(nativeAnims.Files.Single(f=>f.ID==1000000).Bytes.ToArray());
        Need(rig.Names.SequenceEqual(new[]{"o241013","o241013_00"})&&rig.Parents.SequenceEqual(new[]{-1,0}),"native object animation rig");
        SourceAnimation Clip(int id)=>SourceAnimation.Decode(sourceAnims.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),sourceAnims.Files.Single(f=>f.ID==id).Bytes.ToArray());
        var open=Clip(0);var close=Clip(1);Need(open.Bones.Select(b=>b.Name).SequenceEqual(new[]{"dummy","o1315","o1315_00"})&&open.Bones.Select(b=>b.Parent).SequenceEqual(new[]{-1,-1,1})&&close.Bones.SequenceEqual(open.Bones),"source reused door animation rig");
        Need(open.FrameCount==61&&close.FrameCount==61&&Math.Abs(open.Duration-2)<.000001f&&Math.Abs(close.Duration-2)<.000001f,"source open/close timing");
        var reference=open.Bones[2];var local=Matrix4x4.CreateScale(reference.Scale)*Matrix4x4.CreateFromQuaternion(reference.Rotation)*Matrix4x4.CreateTranslation(reference.Translation);Need(Matrix4x4.Invert(local,out var inverse),"singular leaf reference");
        foreach(var (slot,clip,hold) in new[]{(0,open,0),(1,open,-1),(2,close,-1),(3,open,1)}) {
            int frames=hold<0?clip.FrameCount:2;var samples=new List<Matrix4x4>();
            for(int frame=0;frame<frames;frame++){var pose=clip.Sample(hold<0?Math.Min(frame*clip.FrameDuration,clip.Duration):hold==0?0:clip.Duration);Need(Error(pose[0],Matrix4x4.Identity)<.00001f&&Error(pose[1],Matrix4x4.Identity)<.00001f&&pose[2].Translation.Length()<.00001f,"source root/hinge translation");samples.Add(rig.Local[0]);samples.Add(rig.Local[1]*inverse*pose[2]);}
            var file=nativeAnims.Files.Single(f=>f.ID==1000000000+slot);file.Bytes=NativeSpline.Encode(samples,2,frames,clip.FrameDuration,rig.Name,rig.Names);
        }
        // The verified native opening/closing clips are also two seconds.
        // Keep their installed sound/controller TAE and serialized fixups.
        Need(nativeAnims.Files.Count==6,"native animation closure changed");native.Files.Single(f=>f.ID==400).Bytes=nativeAnims.Write();
        foreach(var file in native.Files)file.Name=file.Name.Replace("o241013","o696001",StringComparison.Ordinal);
        return new(native.Write(DCX.Type.DCX_DFLT_10000_44_9),box,open.Duration,open.FrameCount,sha);
    }
}
