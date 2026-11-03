// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;
static class LargeCharacterRigTests {
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Check(bool b,string why){if(!b)throw new Exception(why);}
    static Matrix4x4 Sample(Spline.TransformTrack t,float frame) {
        Vector3 p=t.StaticPosition,s=t.StaticScale;
        if(t.HasSplinePosition)p=new(t.SplinePosition.GetValueX(frame)??p.X,t.SplinePosition.GetValueY(frame)??p.Y,t.SplinePosition.GetValueZ(frame)??p.Z);
        if(t.HasSplineScale)s=new(t.SplineScale.GetValueX(frame)??s.X,t.SplineScale.GetValueY(frame)??s.Y,t.SplineScale.GetValueZ(frame)??s.Z);
        return Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(t.HasSplineRotation?t.SplineRotation.GetValue(frame):t.StaticRotation))*Matrix4x4.CreateTranslation(p);
    }
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{Vector3.Zero,Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Max(p=>Vector3.Distance(Vector3.Transform(p,a),Vector3.Transform(p,b)));
    static void Main(string[] args) {
        var hashes=new Dictionary<string,string>();
        byte[] Read(string p){var b=File.ReadAllBytes(p);hashes[p]=Convert.ToHexStringLower(SHA256.HashData(b));return b;}
        byte[]? Source(string p)=>File.Exists(Path.Combine(args[0],p))?Read(Path.Combine(args[0],p)):null;byte[] Native(string p)=>Read(Path.Combine(args[1],p));
        var playerBank=BND4.Read(Native("chr/c0000.anibnd.dcx"));var player=NativeAnimation.Skeleton(playerBank.Files.Single(f=>f.ID==4000000).Bytes.ToArray());
        Check(player.Names.Length==108,"Retail player playback rig was not read");
        var authored=MapCharacters.Authored(Source,"c3530");var reference=SourceAnimation.Load(Source,"c3530",0);
        var nativeAnimation=BND4.Read(Native("chr/c2730.anibnd.dcx"));var nativeCharacter=BND4.Read(Native("chr/c2730.chrbnd.dcx"));
        var nativeModel=nativeCharacter.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray();var skeleton=nativeAnimation.Files.Single(f=>f.ID==1000000).Bytes.ToArray();
        var plan=NonHumanoidEnemies.Rig(authored.Model,nativeModel,skeleton,reference);Check(plan.Count==356&&plan.Added.Count==246,"Source Hydra/native controller rig population changed");
        var skinTemplate=FLVER2.Read(BND4.Read(Native("chr/c2530.chrbnd.dcx")).Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes);
        var render=FLVER2.Read(plan.Model);render.BufferLayouts=skinTemplate.BufferLayouts;render.GXLists=skinTemplate.GXLists;render.Materials=skinTemplate.Materials;render.Meshes=skinTemplate.Meshes;
        var overrides=plan.Added.ToDictionary(b=>b.Name[4..],b=>b.Name,StringComparer.Ordinal);
        var skin=SkinRetarget.Bake(authored.Model,render.Write(),authored.Textures,overrides:overrides,textureOwner:"c8830",sourceProfile:"DS1R",preserveBoneLengths:true);
        Check(skin.DirectWeightShare>.999999&&skin.Model.Meshes.All(m=>SkinRetarget.MaximumBoneIndex(skin.Model,m)==ushort.MaxValue),"Hydra weights lost or byte layout selected");
        NonHumanoidEnemies.OrderRenderNodes(skin.Model);var written=FLVER2.Read(skin.Model.Write());int high=0;
        for(int m=0;m<written.Meshes.Count;m++)for(int v=0;v<written.Meshes[m].Vertices.Count;v++)for(int n=0;n<4;n++) {
            var a=skin.Model.Meshes[m].Vertices[v];var b=written.Meshes[m].Vertices[v];if(a.BoneWeights[n]<=0)continue;
            Check(a.BoneIndices[n]==b.BoneIndices[n],"16-bit skin index truncated during FLVER round trip");if(b.BoneIndices[n]>255)high++;
        }
        Check(high>0&&written.Nodes.Count==356,"Large visible render palette did not survive");
        var original=nativeAnimation.Files.Single(f=>f.ID==1000003000).Bytes.ToArray();var clip=NativeRigAppend.Clip(original,plan,time=>{
            var pose=reference.Sample(time*reference.Duration);Check(Matrix4x4.Invert(plan.Original.Local[0],out var inverse),"Singular native root");pose[0]*=inverse;return pose;
        },preserveAddedRootWorld:true);
        var pack=new NativeAnimation.Pack(clip);int animation=pack.Object("hkaSplineCompressedAnimation");var payload=pack.Array(animation+152,1,64*1024*1024);int frames=pack.Int(animation+56);
        var decoded=Spline.ReadSplineCompressedAnimByteBlock(false,clip.AsSpan(pack.Data+payload.At,payload.Count).ToArray(),356,pack.Int(animation+60));float maxError=0;
        foreach(int frame in new[]{0,(frames-1)/2,frames-1}) {
            int block=frame/255,local=frame-block*255;var expected=reference.Sample(frame/(float)(frames-1)*reference.Duration);
            Check(Matrix4x4.Invert(Sample(decoded[block][0],local),out var inverse),"Singular decoded root");expected[0]*=inverse;
            for(int b=0;b<246;b++){float e=Error(expected[b],Sample(decoded[block][110+b],local));maxError=Math.Max(maxError,e);Check(e<.004f,"Appended large-rig motion differs from source");}
        }
        var originalPack=new NativeAnimation.Pack(original);int oldAnimation=originalPack.Object("hkaSplineCompressedAnimation");Check(pack.Int(animation+56)==originalPack.Int(oldAnimation+56)&&pack.Float(animation+16)==originalPack.Float(oldAnimation+16),"Native clip timing changed");
        foreach(var (p,sha) in hashes)Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(p)))==sha,"Original rig/animation/texture file changed");
        Console.WriteLine($"PASS: retail player {player.Names.Length} bones; Hydra246+native110=356, {high} weighted indices above255 round-trip, independent sampled motion max error{maxError}; {hashes.Count} original files unchanged. No gameplay claim.");
    }
}
