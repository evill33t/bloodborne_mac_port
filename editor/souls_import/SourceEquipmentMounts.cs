// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.RegularExpressions;

// FLVER equipment bind mounts can differ from the authored idle attachment
// channels. Retain those source local frames on the ordinary native body rig.
internal static class SourceEquipmentMounts {
    internal sealed record Mount(string Bone,string Parent,Matrix4x4 Local,float TranslationFromReference,float RotationFromReference,float MaximumIdleVariation);
    internal sealed record Report(string SourceModel,string AnimationPath,string AnimationSHA256,string Clip,IReadOnlyList<Mount> Mounts,string Limitation);
    internal sealed record Result(IReadOnlyDictionary<string,Matrix4x4> Frames,Report Report);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static string Key(string p)=>new(p.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
    static bool Equipment(string name)=>Regex.IsMatch(Key(name),@"\A(?:rweapon\d*|lweapon\d*|lshield|sword|bpw00)\z");
    internal static Result? Load(Func<string,byte[]?> read,string profile,string model,byte[] sourceModel,IReadOnlySet<int> masks) {
        if(profile!="DS1R"||model is not("c2500" or "c2540" or "c2550" or "c2560"))return null;
        string path="chr/"+model+".anibnd.dcx";var bytes=read(path)??throw new FileNotFoundException("Source equipment animation is unavailable",path);
        if(bytes.Length is <1 or >128*1024*1024)throw new InvalidDataException("Source equipment animation exceeds the import budget");
        var archive=BND3.Read(bytes);var skeleton=archive.Files.Single(f=>Leaf(f.Name).Equals("skeleton.hkx",StringComparison.OrdinalIgnoreCase)).Bytes.ToArray();
        var clip=SourceAnimation.Decode(skeleton,archive.Files.Single(f=>Leaf(f.Name)=="a00_0000.hkx").Bytes.ToArray());
        var source=FLVER2.Read(sourceModel);var names=clip.Bones.Select((b,i)=>(b.Name,i)).ToDictionary(p=>p.Name,p=>p.i,StringComparer.Ordinal);
        var used=new HashSet<int>();foreach(var mesh in source.Meshes) {
            var mask=Regex.Match(source.Materials[mesh.MaterialIndex].Name,@"#(\d+)#");if(mask.Success&&!masks.Contains(int.Parse(mask.Groups[1].Value)))continue;
            if(mesh.Dynamic!=1)throw new InvalidDataException("Source equipment requires weighted character meshes");
            foreach(var vertex in mesh.Vertices)for(int slot=0;slot<4;slot++)if(vertex.BoneWeights[slot]>0)used.Add(SkinRetarget.Bone(mesh,vertex.BoneIndices[slot]));
        }
        var mounts=new List<Mount>();var frames=new Dictionary<string,Matrix4x4>(StringComparer.Ordinal);
        foreach(int node in used.Where(i=>Equipment(source.Nodes[i].Name)).Order()) {
            var bind=source.Nodes[node];int parent=bind.ParentIndex;
            if(parent<0||Key(source.Nodes[parent].Name) is not("lhand" or "rhand" or "lforearm" or "rforearm")||!names.TryGetValue(bind.Name,out int bone)||clip.Bones[bone].Parent<0||clip.Bones[clip.Bones[bone].Parent].Name!=source.Nodes[parent].Name)
                throw new InvalidDataException("Source equipment attachment hierarchy is unsupported: "+bind.Name);
            var value=clip.EquipmentLocal(bone,0);var reference=clip.Bones[bone];var referenceLocal=Matrix4x4.CreateScale(reference.Scale)*Matrix4x4.CreateFromQuaternion(reference.Rotation)*Matrix4x4.CreateTranslation(reference.Translation);
            if(!Matrix4x4.Decompose(value,out var scale,out var rotation,out _)||Vector3.Distance(scale,Vector3.One)>.001f)throw new InvalidDataException("Source idle equipment scale requires its own adapter");
            float maximum=0;for(int frame=0;frame<clip.FrameCount;frame++)maximum=Math.Max(maximum,Error(value,clip.EquipmentLocal(bone,frame==clip.FrameCount-1?clip.Duration:frame*clip.FrameDuration)));
            if(maximum>.002f)throw new InvalidDataException("Source equipment idle mount is animated: "+bind.Name);
            frames.Add(bind.Name,value);mounts.Add(new(bind.Name,source.Nodes[parent].Name,value,Vector3.Distance(value.Translation,referenceLocal.Translation),2*MathF.Acos(Math.Clamp(Math.Abs(Quaternion.Dot(rotation,reference.Rotation)),0,1)),maximum));
        }
        if(frames.Count==0)throw new InvalidDataException("Selected source loadout has no supported equipment mount");
        return new(frames,new(model,path,Convert.ToHexStringLower(SHA256.HashData(bytes)),"a00_0000.hkx",mounts,"Authored source idle attachment mounts on the native anatomical hand/forearm. Other source animation-local equipment motion and visibility channels are not retargeted; native combat actions remain approximations."));
    }
    static float Error(Matrix4x4 a,Matrix4x4 b) {
        float[] x=[a.M11,a.M12,a.M13,a.M21,a.M22,a.M23,a.M31,a.M32,a.M33,a.M41,a.M42,a.M43],y=[b.M11,b.M12,b.M13,b.M21,b.M22,b.M23,b.M31,b.M32,b.M33,b.M41,b.M42,b.M43];
        return x.Zip(y,(p,q)=>Math.Abs(p-q)).Max();
    }
}

internal sealed partial class SourceAnimation {
    // Read only a selected equipment track. Unselected source weapon helpers
    // may intentionally scale to zero; sampling every bone would reject those
    // unrelated visibility channels even when this selected mount is valid.
    internal Matrix4x4 EquipmentLocal(int bone,float time) {
        if(bone<0||bone>=Bones.Count||!float.IsFinite(time)||time<0||time>Duration+.0001f)throw new ArgumentOutOfRangeException(nameof(time));
        float frame=time>=Duration?FrameCount-1:Math.Min(time/FrameDuration,FrameCount-1);int block=Math.Min((int)(frame/(framesPerBlock-1)),blocks.Length-1);float local=frame-block*(framesPerBlock-1);var b=Bones[bone];int track=Array.IndexOf(trackBones,bone);
        if(track<0)return Matrix(b.Translation,b.Rotation,b.Scale);
        var value=blocks[block][track];var scale=value.Scale.Value(local,b.Scale);ValidScale(scale);
        return Matrix(value.Position.Value(local,b.Translation),value.Rotation?.Value(local)??b.Rotation,scale);
    }
}
