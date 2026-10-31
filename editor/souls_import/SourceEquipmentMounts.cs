// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.RegularExpressions;

// FLVER equipment bind mounts can differ from the authored idle attachment
// channels. Retain those source local frames on the ordinary native body rig.
internal static class SourceEquipmentMounts {
    internal sealed record Mount(string Bone,string Parent,Matrix4x4 Local,float TranslationFromReference,float RotationFromReference,float MaximumIdleVariation);
    internal sealed record Report(string SourceModel,string AnimationPath,string AnimationSHA256,string Clip,IReadOnlyList<Mount> Mounts,string Limitation) {
        public IReadOnlyList<string> HoldingPoseBones {get;init;}=[];
    }
    internal sealed record Result(IReadOnlyDictionary<string,Matrix4x4> Frames,Report Report) {
        internal IReadOnlyDictionary<string,Matrix4x4> Grip {get;init;}=new Dictionary<string,Matrix4x4>();
    }
    static readonly string[] GripNames=["L Hand","L Finger0","L Finger01","L Finger1","L Finger11","L Finger2","L Finger21"];
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
        var grip=new Dictionary<string,Matrix4x4>(StringComparer.Ordinal);
        if(mounts.Any(m=>Key(m.Bone)=="lshield"))foreach(string name in GripNames) {
            if(!names.TryGetValue(name,out int bone)||clip.Bones[bone].Parent<0)throw new InvalidDataException("Source shield holding pose is incomplete: "+name);
            string parent=name=="L Hand"?"L Forearm":name.Length==9?"L Hand":name[..^1];
            if(clip.Bones[clip.Bones[bone].Parent].Name!=parent)throw new InvalidDataException("Source shield holding hierarchy changed: "+name);
            grip.Add(name,clip.EquipmentLocal(bone,0));
        }
        string limitation="Authored source idle attachment mounts on the native anatomical hand/forearm. Other source animation-local equipment motion and visibility channels are not retargeted; native combat actions remain approximations.";
        if(grip.Count>0)limitation+=" The source idle left wrist/finger holding pose is retained during native forearm motion, with neutral grip additive tracks. Source combat hand gestures are not reproduced.";
        return new(frames,new(model,path,Convert.ToHexStringLower(SHA256.HashData(bytes)),"a00_0000.hkx",mounts,limitation){HoldingPoseBones=grip.Keys.ToArray()}){Grip=grip};
    }

    // The shield remains on its authored forearm. c2610's torch wrist/finger
    // pose otherwise separates the glove from the source handle, even when
    // the equipment's idle-local mount itself is correct. Replace only the
    // seven shared holding tracks; retain all other native compressed bytes.
    internal static byte[] ApplyGrip(byte[] input,NativeAnimation.Rig rig,IReadOnlyDictionary<string,Matrix4x4> grip) {
        if(grip.Count==0)return input.ToArray();
        if(rig.Names.Length!=80||rig.Name!="Master"||grip.Count!=GripNames.Length||!GripNames.All(grip.ContainsKey))throw new InvalidDataException("Shield holding pose requires the ordinary native hollow rig");
        var mapped=new Dictionary<int,Matrix4x4>();
        foreach(string name in GripNames) {
            int bone=Array.IndexOf(rig.Names,name);string parent=name=="L Hand"?"L Forearm":name.Length==9?"L Hand":name[..^1];var local=grip[name];
            if(bone<0||rig.Parents[bone]<0||rig.Names[rig.Parents[bone]]!=parent||
                !Matrix4x4.Decompose(local,out var scale,out var rotation,out var position)||Vector3.Distance(scale,Vector3.One)>.001f||
                !float.IsFinite(position.LengthSquared())||position.LengthSquared()>1||!float.IsFinite(rotation.LengthSquared())||Math.Abs(rotation.LengthSquared()-1)>.001f||
                Math.Abs(local.M14)+Math.Abs(local.M24)+Math.Abs(local.M34)+Math.Abs(local.M44-1)>.00001f)
                throw new InvalidDataException("Invalid source shield holding frame: "+name);
            mapped.Add(bone,local);
        }
        var edit=new NativeRigAppend.Edit(input);var p=edit.Pack;int a=p.Object("hkaSplineCompressedAnimation"),binding=p.Object("hkaAnimationBinding"),n=rig.Names.Length;
        int frames=p.Int(a+56),blocks=p.Int(a+60),blend=input[p.Data+binding+80];
        if(p.Int(a+12)!=3||p.Int(a+20)!=n||p.Int(a+24)!=0||p.Int(a+64)!=256||p.Int(a+68)!=n*4||
            frames is <2 or >65536||blocks!=(frames-2)/255+1||blend>1||p.String(binding+16)!=rig.Name||p.Int(a+168)!=0||
            p.Array(a+120,4,256).Count!=0||p.Array(a+136,4,256).Count!=0)throw new InvalidDataException("Unsupported native shield holding spline");
        var map=p.Array(binding+32,2,256);if(map.Count!=n||Enumerable.Range(0,n).Any(i=>p.Short(map.At+i*2)!=i))throw new InvalidDataException("Native shield holding bindings changed");
        // Use the same retail THREECOMP40 encoder as the animation adapters.
        var values=mapped.Values.Select(v=>blend==1?Matrix4x4.Identity:v).ToArray();
        var encoded=NativeSpline.Encode(values.Concat(values).ToArray(),values.Length,2,p.Float(a+80),rig.Name,mapped.Keys.Select(i=>rig.Names[i]).ToArray());
        var replacement=new NativeAnimation.Pack(encoded);int ra=replacement.Object("hkaSplineCompressedAnimation");var rd=replacement.Array(ra+152,1,64*1024*1024);int ru=replacement.Int(replacement.Array(ra+104,4,256).At);
        var parts=TrackRanges(encoded.AsMemory(replacement.Data+rd.At,rd.Count),values.Length,1,ru);
        var data=p.Array(a+152,1,64*1024*1024);var starts=p.Array(a+88,4,256);var ends=p.Array(a+104,4,256);
        if(starts.Count!=blocks||ends.Count!=blocks||p.Int(starts.At)!=0)throw new InvalidDataException("Invalid native holding block table");
        using var stream=new MemoryStream();var newStarts=new List<int>();var newEnds=new List<int>();
        for(int block=0;block<blocks;block++) {
            int start=p.Int(starts.At+block*4),end=block+1<blocks?p.Int(starts.At+(block+1)*4):data.Count,used=p.Int(ends.At+block*4);
            if(start<0||start%16!=0||end<=start||end>data.Count||used<n*4||used>end-start)throw new InvalidDataException("Invalid native holding block bounds");
            var raw=input.AsMemory(p.Data+data.At+start,end-start);var ranges=TrackRanges(raw,n,Math.Min(255,frames-1-block*255),used);
            newStarts.Add(checked((int)stream.Length));int replacementTrack=0;
            for(int i=0;i<n;i++)if(mapped.ContainsKey(i))stream.Write(encoded.AsSpan(replacement.Data+rd.At+replacementTrack++*4,4));else stream.Write(raw.Span.Slice(i*4,4));
            replacementTrack=0;
            for(int i=0;i<n;i++) {
                if(mapped.ContainsKey(i)){var part=parts[replacementTrack++];stream.Write(encoded.AsSpan(replacement.Data+rd.At+part.Start,part.Length));}
                else {var part=ranges[i];stream.Write(raw.Span.Slice(part.Start,part.Length));}
            }
            newEnds.Add(checked((int)stream.Length-newStarts[^1]));while(stream.Length%16!=0)stream.WriteByte(0);
        }
        edit.Array(a+152,checked((int)stream.Length),stream.ToArray());edit.Array(a+88,blocks,newStarts.SelectMany(BitConverter.GetBytes).ToArray());edit.Array(a+104,blocks,newEnds.SelectMany(BitConverter.GetBytes).ToArray());return edit.Finish();
    }

    // Half-open, four-byte-aligned payloads after the n*4 mask table. This
    // bounds check also rejects unrecognized quantization/curves fail closed.
    internal static (int Start,int Length)[] TrackRanges(ReadOnlyMemory<byte> memory,int tracks,int last,int used) {
        var bytes=memory.ToArray();int cursor=0;
        void Skip(int count){if(count<0||cursor>bytes.Length-count)throw new InvalidDataException("Truncated holding track");cursor+=count;}
        byte Byte(){Skip(1);return bytes[cursor-1];}
        void Align(int n)=>Skip((n-cursor%n)%n);
        int Curve() {
            int count=Byte()|(Byte()<<8);count++;int degree=Byte();if(count is <2 or >260||degree is <1 or >4||count<degree+1)throw new InvalidDataException("Unsupported holding spline curve");
            var knots=new byte[count+degree+1];for(int i=0;i<knots.Length;i++)knots[i]=Byte();
            if(knots[degree]!=0||knots[count]!=last||knots.Zip(knots.Skip(1)).Any(v=>v.First>v.Second))throw new InvalidDataException("Invalid holding spline domain");return count;
        }
        void Vector(byte mask) {
            int points=(mask&0x70)!=0?Curve():0;if(points>0)Align(4);
            for(int axis=0;axis<3;axis++)if((mask&(16<<axis))!=0)Skip(8);else if((mask&(1<<axis))!=0)Skip(4);
            Skip(checked(points*BitOperations.PopCount((uint)(mask&0x70))*2));
        }
        var masks=new (byte P,byte R,byte S)[tracks];
        for(int i=0;i<tracks;i++) {
            if(Byte()!=0x45)throw new InvalidDataException("Native holding spline quantization changed");byte position=Byte(),rotation=Byte(),scale=Byte();
            if((position&0x88)!=0||(scale&0x88)!=0||((position&7)&(position>>4))!=0||((scale&7)&(scale>>4))!=0)throw new InvalidDataException("Invalid native holding masks");masks[i]=(position,rotation,scale);
        }
        Align(4);var ranges=new (int Start,int Length)[tracks];
        for(int i=0;i<tracks;i++) {
            int start=cursor;var m=masks[i];Vector(m.P);Align(4);if((m.R&0xf0)!=0)Skip(checked(Curve()*5));else if(m.R!=0)Skip(5);Align(4);Vector(m.S);Align(4);ranges[i]=(start,cursor-start);
        }
        if(cursor!=used)throw new InvalidDataException("Native holding track endpoint mismatch");Align(16);if(cursor!=bytes.Length)throw new InvalidDataException("Unexpected native holding spline trailing bytes");return ranges;
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
